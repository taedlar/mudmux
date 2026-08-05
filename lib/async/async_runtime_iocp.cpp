/**
 * @file async_runtime_iocp.c
 * @brief Windows IOCP-based async runtime implementation
 * 
 * This implementation uses Windows I/O Completion Ports for unified handling
 * of both I/O events and worker thread completions.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include "async_runtime.h"
#include "async_event.h"

#include <atomic>
#pragma comment(lib, "ws2_32.lib")

#include "console_worker.h"

/* Maximum text buffer size */
#ifndef MAX_TEXT
#define MAX_TEXT 2048
#endif

/* Initial capacity for context pool */
#define INITIAL_POOL_SIZE 256

/* Operation types for IOCP contexts */
#define OP_READ    1
#define OP_WRITE   2
#define OP_ACCEPT  3

/* Completion keys for special events */
#define ACCEPT_COMPLETION_KEY  IOCP_COMPLETION_KEY(1)
#define WAKEUP_COMPLETION_KEY  IOCP_COMPLETION_KEY(2)
#define EVENT_COMPLETION_KEY   IOCP_COMPLETION_KEY(3)

/**
 * IOCP context for each I/O operation
 * The OVERLAPPED structure must be the first member
 */
typedef struct iocp_context_s {
    OVERLAPPED overlapped;
    void* user_context;
    int operation;
    WSABUF wsa_buf;
    char buffer[MAX_TEXT];
    socket_fd_t fd;
} iocp_context_t;

/**
 * Listening socket entry for accept worker
 */
typedef struct listening_socket_s {
    socket_fd_t fd;
    void* context;
} listening_socket_t;

typedef struct event_wait_registration_s {
    HANDLE wait_handle;
    HANDLE event_handle;
    async_runtime_t* runtime;
    void* context;
} event_wait_registration_t;

/**
 * Async runtime implementation for Windows
 */
struct async_runtime_s {
    void* context;  /* User-defined context pointer */
    HANDLE iocp_handle;
    int num_fds;
    
    /* Context pool for allocation efficiency */
    iocp_context_t** context_pool;
    int pool_size;
    int pool_capacity;
    
    /* Accept worker for listening sockets */
    HANDLE accept_thread;
    DWORD accept_thread_id;
    CRITICAL_SECTION listen_lock;
    listening_socket_t* listen_sockets;
    int listen_count;
    int listen_capacity;
    volatile int accept_thread_running;

    event_wait_registration_t** event_waits;
    int event_wait_count;
    int event_wait_capacity;
    
    /* Console support */
    console_type_t console_type;
};

static std::atomic<async_runtime_t*> current_runtime(nullptr);

static VOID CALLBACK event_wait_callback(PVOID parameter, BOOLEAN) {
    event_wait_registration_t* registration = static_cast<event_wait_registration_t*>(parameter);
    (void) PostQueuedCompletionStatus(registration->runtime->iocp_handle, 0,
        EVENT_COMPLETION_KEY, reinterpret_cast<LPOVERLAPPED>(registration));
}

/* Context pool management */

static iocp_context_t* alloc_iocp_context (async_runtime_t* runtime, socket_fd_t fd,
                                           void* user_context, int operation) {
    iocp_context_t* ctx;
    
    if (runtime->pool_size > 0) {
        ctx = runtime->context_pool[--runtime->pool_size];
    } else {
        ctx = (iocp_context_t*) calloc(1, sizeof(iocp_context_t));
        if (!ctx) return NULL;
    }
    
    ZeroMemory (&ctx->overlapped, sizeof(OVERLAPPED));
    ctx->user_context = user_context;
    ctx->operation = operation;
    ctx->fd = fd;
    ctx->wsa_buf.buf = ctx->buffer;
    ctx->wsa_buf.len = sizeof(ctx->buffer);
    
    return ctx;
}

static void free_iocp_context (async_runtime_t* runtime, iocp_context_t* ctx) {
    if (!ctx) return;
    
    if (runtime->pool_size < runtime->pool_capacity) {
        runtime->context_pool[runtime->pool_size++] = ctx;
    } else {
        free (ctx);
    }
}

/* Accept worker thread - monitors listening sockets and posts accepted connections to IOCP */
static DWORD WINAPI accept_worker_thread (LPVOID param) {
    async_runtime_t* runtime = (async_runtime_t*)param;
    
    while (runtime->accept_thread_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        socket_fd_t max_fd = INVALID_SOCKET;
        
        /* Build fd_set from listening sockets */
        EnterCriticalSection(&runtime->listen_lock);
        for (int i = 0; i < runtime->listen_count; i++) {
            FD_SET(runtime->listen_sockets[i].fd, &read_fds);
            if (max_fd == INVALID_SOCKET || runtime->listen_sockets[i].fd > max_fd) {
                max_fd = runtime->listen_sockets[i].fd;
            }
        }
        int listen_count = runtime->listen_count;
        LeaveCriticalSection(&runtime->listen_lock);
        
        if (listen_count == 0) {
            /* No listening sockets, sleep briefly */
            Sleep (100);
            continue;
        }
        
        /* Wait for activity with 1 second timeout */
        struct timeval timeout = {1, 0};
        int result = select ((int)max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (result > 0) {
            /* Check each listening socket */
            EnterCriticalSection(&runtime->listen_lock);
            for (int i = 0; i < runtime->listen_count; i++) {
                socket_fd_t listen_fd = runtime->listen_sockets[i].fd;
                void* context = runtime->listen_sockets[i].context;
                
                if (FD_ISSET(listen_fd, &read_fds)) {
                    LeaveCriticalSection(&runtime->listen_lock);
                    SPDLOG_TRACE ("readability reported on listen socket {}", listen_fd);

                    /* Drain all pending accepts so the listener stops reporting
                     * readability once the backlog is empty. */
                    for (;;) {
                        struct sockaddr_storage addr; // generic storage for IPv4 or IPv6
                        int addr_len = sizeof(addr);
                        socket_fd_t accepted_fd = accept (listen_fd, (struct sockaddr*)&addr, &addr_len);

                        if (accepted_fd == INVALID_SOCKET) {
                            int err = WSAGetLastError();
                            if (err == WSAEWOULDBLOCK) {
                                break;
                            }
                            SPDLOG_WARN ("accept on listen socket {} failed: {}", listen_fd, err);
                            break;
                        }

                        /* Carry the full 64-bit SOCKET in an iocp_context_t to avoid
                         * truncation through DWORD (dwNumberOfBytesTransferred is 32-bit). */
                        iocp_context_t* ctx = alloc_iocp_context (runtime, accepted_fd, context, OP_ACCEPT);
                        if (ctx) {
                            PostQueuedCompletionStatus (runtime->iocp_handle, 0, ACCEPT_COMPLETION_KEY, &ctx->overlapped);
                        } else {
                            closesocket (accepted_fd);
                        }
                    }
                    EnterCriticalSection(&runtime->listen_lock);
                }
            }
            LeaveCriticalSection(&runtime->listen_lock);
        }
    }
    
    return 0;
}

extern "C" async_runtime_t* async_get_current_runtime(void) {
    return current_runtime.load(std::memory_order_acquire);
}

/* Lifecycle management */

extern "C" async_runtime_t* async_runtime_init(void* context) {
    async_runtime_t* runtime = (async_runtime_t*) calloc(1, sizeof(async_runtime_t));
    if (!runtime) return NULL;
    
    runtime->context = context;

    runtime->event_wait_capacity = 8;
    runtime->event_waits = (event_wait_registration_t**)calloc(runtime->event_wait_capacity, sizeof(*runtime->event_waits));
    if (!runtime->event_waits) {
        free(runtime);
        return NULL;
    }

    /* Create IOCP with 1 concurrent thread (single-threaded model) */
    runtime->iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!runtime->iocp_handle) {
        free(runtime->event_waits);
        free(runtime);
        return NULL;
    }
    
    /* Initialize context pool */
    runtime->pool_capacity = INITIAL_POOL_SIZE;
    runtime->context_pool = (iocp_context_t**) calloc(runtime->pool_capacity, sizeof(iocp_context_t*));
    if (!runtime->context_pool) {
        CloseHandle(runtime->iocp_handle);
        free(runtime->event_waits);
        free(runtime);
        return NULL;
    }
    
    /* Initialize listening socket array and accept worker */
    runtime->listen_capacity = 8;
    runtime->listen_sockets = (listening_socket_t*) calloc(runtime->listen_capacity, sizeof(listening_socket_t));
    if (!runtime->listen_sockets) {
        free(runtime->context_pool);
        CloseHandle(runtime->iocp_handle);
        free(runtime->event_waits);
        free(runtime);
        return NULL;
    }
    
    InitializeCriticalSection(&runtime->listen_lock);
    runtime->accept_thread_running = 1;
    runtime->accept_thread = CreateThread(NULL, 0, accept_worker_thread, runtime, 0, &runtime->accept_thread_id);
    if (!runtime->accept_thread) {
        DeleteCriticalSection(&runtime->listen_lock);
        free(runtime->listen_sockets);
        free(runtime->context_pool);
        CloseHandle(runtime->iocp_handle);
        free(runtime->event_waits);
        free(runtime);
        return NULL;
    }
    
    /* Initialize console support */
    runtime->console_type = console_detect_type();

    current_runtime.store(runtime, std::memory_order_release);  /* Set current runtime */
    return runtime;
}

extern "C" void* async_runtime_get_context(async_runtime_t* runtime) {
    return runtime ? runtime->context : nullptr;
}

extern "C" void async_runtime_deinit(async_runtime_t* runtime) {
    if (!runtime) return;
    
    /* Stop accept worker thread */
    if (runtime->accept_thread) {
        runtime->accept_thread_running = 0;
        WaitForSingleObject(runtime->accept_thread, 5000);
        CloseHandle(runtime->accept_thread);
    }
    
    DeleteCriticalSection(&runtime->listen_lock);
    free(runtime->listen_sockets);

    for (int i = 0; i < runtime->event_wait_count; ++i) {
        UnregisterWaitEx(runtime->event_waits[i]->wait_handle, INVALID_HANDLE_VALUE);
        free(runtime->event_waits[i]);
    }
    free(runtime->event_waits);
    
    /* Free context pool */
    for (int i = 0; i < runtime->pool_size; i++) {
        free(runtime->context_pool[i]);
    }
    free(runtime->context_pool);
    
    if (runtime->iocp_handle && runtime->iocp_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(runtime->iocp_handle);
    }
    
    current_runtime.compare_exchange_strong(runtime, nullptr); // read-modify-write to clear current_runtime if it matches this runtime
    free(runtime);
}

/* I/O source management */

extern "C" int async_runtime_add (async_runtime_t* runtime, socket_fd_t fd, uint32_t events, void* context) {
    (void)events;  /* IOCP doesn't need explicit event flags */
    if (!runtime || fd == INVALID_SOCKET)
        return -1;
    
    /* Check if this is a listening socket */
    BOOL is_listening = FALSE;
    int optlen = sizeof(is_listening);
    if (getsockopt (fd, SOL_SOCKET, SO_ACCEPTCONN, (char*)&is_listening, &optlen) == 0 && is_listening) {
        SPDLOG_DEBUG ("adding listening socket {} to accept worker", fd);
        /* Listening socket - add to accept worker's monitoring list */
        EnterCriticalSection (&runtime->listen_lock);
        
        /* Grow array if needed */
        if (runtime->listen_count >= runtime->listen_capacity) {
            int new_capacity = runtime->listen_capacity * 2;
            listening_socket_t* new_array = (listening_socket_t*) realloc (runtime->listen_sockets,
                                                   new_capacity * sizeof(listening_socket_t));
            if (!new_array) {
                LeaveCriticalSection (&runtime->listen_lock);
                return -1;
            }
            runtime->listen_sockets = new_array;
            runtime->listen_capacity = new_capacity;
        }
        
        /* Add to array */
        runtime->listen_sockets[runtime->listen_count].fd = fd;
        runtime->listen_sockets[runtime->listen_count].context = context;
        runtime->listen_count++;
        
        LeaveCriticalSection(&runtime->listen_lock);
        
        runtime->num_fds++;
        return 0;
    }
    
    /* Associate socket with IOCP */
    HANDLE result = CreateIoCompletionPort((HANDLE)fd, runtime->iocp_handle,
                                          (ULONG_PTR)context, 0);
    if (!result) return -1;
    
    /* Note: For connected sockets, initial async read is posted by caller after
     * user object setup completes (see setup_accepted_connection in comm.c).
     * This avoids race conditions where read completes before mudlib_connect()
     * transfers the interactive pointer to the user object. */
    
    runtime->num_fds++;
    return 0;
}

extern "C" int async_runtime_modify(async_runtime_t* runtime, socket_fd_t fd, uint32_t events, void* context) {
    /* IOCP doesn't need explicit modify - just post new operations */
    (void)runtime; (void)fd; (void)events; (void)context;
    return 0;
}

extern "C" int async_runtime_remove(async_runtime_t* runtime, socket_fd_t fd) {
    if (!runtime) return -1;
    
    /* Check if this is a listening socket */
    EnterCriticalSection(&runtime->listen_lock);
    for (int i = 0; i < runtime->listen_count; i++) {
        if (runtime->listen_sockets[i].fd == fd) {
            /* Remove from array (swap with last element) */
            runtime->listen_count--;
            if (i < runtime->listen_count) {
                runtime->listen_sockets[i] = runtime->listen_sockets[runtime->listen_count];
            }
            
            LeaveCriticalSection(&runtime->listen_lock);
            runtime->num_fds--;
            return 0;
        }
    }
    LeaveCriticalSection(&runtime->listen_lock);
    
    /* Not a listening socket - just decrement count */
    runtime->num_fds--;
    return 0;
}

extern "C" int async_runtime_add_event(async_runtime_t* runtime, async_event_t* event, void* context) {
    if (!runtime || !event)
        return -1;
    HANDLE event_handle = async_event_get_wait_handle(event);
    if (!event_handle)
        return -1;
    event_wait_registration_t* registration = (event_wait_registration_t*)calloc(1, sizeof(*registration));
    if (!registration)
        return -1;
    registration->event_handle = event_handle;
    registration->runtime = runtime;
    registration->context = context;
    if (!RegisterWaitForSingleObject(&registration->wait_handle, event_handle, event_wait_callback,
            registration, INFINITE, WT_EXECUTEDEFAULT)) {
        free(registration);
        return -1;
    }
    if (runtime->event_wait_count == runtime->event_wait_capacity) {
        const int new_capacity = runtime->event_wait_capacity * 2;
        event_wait_registration_t** waits = (event_wait_registration_t**)realloc(
            runtime->event_waits, new_capacity * sizeof(*waits));
        if (!waits) {
            UnregisterWaitEx(registration->wait_handle, INVALID_HANDLE_VALUE);
            free(registration);
            return -1;
        }
        runtime->event_waits = waits;
        runtime->event_wait_capacity = new_capacity;
    }
    runtime->event_waits[runtime->event_wait_count++] = registration;
    return 0;
}

extern "C" int async_runtime_wakeup(async_runtime_t* runtime) {
    if (!runtime || !runtime->iocp_handle) return -1;
    
    /* Post special wakeup completion to interrupt GetQueuedCompletionStatusEx */
    BOOL result = PostQueuedCompletionStatus(
        runtime->iocp_handle,
        0,  /* No data */
        WAKEUP_COMPLETION_KEY,
        NULL  /* NULL overlapped */
    );
    
    return result ? 0 : -1;
}

/* Event loop */

extern "C" int async_runtime_wait (async_runtime_t* runtime, io_event_t* events,
                                   int max_events, struct timeval* timeout) {
    if (!runtime || !events || max_events <= 0) return -1;
    
    DWORD timeout_ms = INFINITE;
    if (timeout) {
        timeout_ms = (timeout->tv_sec * 1000) + (timeout->tv_usec / 1000);
    }
    
    int event_count = 0;
    
    /* All events (connected sockets, accepted connections, worker completions)
     * flow through IOCP - single blocking point, no polling */
    OVERLAPPED_ENTRY entries[64];
    ULONG num_entries = 0;

    current_runtime.store (runtime, std::memory_order_release);
    if (GetQueuedCompletionStatusEx (runtime->iocp_handle, entries, 64,
                                     &num_entries, timeout_ms, FALSE)) {
        /* Process IOCP completions */
        for (ULONG i = 0; i < num_entries && event_count < max_events; i++) {
            iocp_context_t* io_ctx = (iocp_context_t*) entries[i].lpOverlapped;
            
            if (io_ctx) {
                events[event_count].fd = io_ctx->fd;
                events[event_count].handle = NULL;
                events[event_count].completion_key = entries[i].lpCompletionKey;
                events[event_count].bytes_transferred = entries[i].dwNumberOfBytesTransferred;
                events[event_count].buffer = io_ctx->buffer;

                if (io_ctx->operation == OP_ACCEPT) {
                    /* Accept worker posted accepted socket via iocp_context_t.
                     * user_context holds the listener slot's context pointer;
                     * fd holds the full 64-bit accepted SOCKET. */
                    events[event_count].context = io_ctx->user_context;
                    events[event_count].event_type = EVENT_READ;
                    events[event_count].bytes_transferred = 0;
                    events[event_count].buffer = NULL;
                } else if (io_ctx->operation == OP_READ) {
                    events[event_count].context = (void*) entries[i].lpCompletionKey;
                    events[event_count].event_type = (entries[i].dwNumberOfBytesTransferred > 0)
                                                     ? EVENT_READ : EVENT_CLOSE;
                } else if (io_ctx->operation == OP_WRITE) {
                    events[event_count].context = (void*) entries[i].lpCompletionKey;
                    events[event_count].event_type = EVENT_WRITE;
                }
                free_iocp_context(runtime, io_ctx);
                event_count++;
            } else {
                /* NULL overlapped - wakeup or console worker completion */
                if (entries[i].lpCompletionKey == WAKEUP_COMPLETION_KEY) {
                    continue;
                }
                if (entries[i].lpCompletionKey == CONSOLE_COMPLETION_KEY) {
                    continue;
                }
                if (entries[i].lpCompletionKey == EVENT_COMPLETION_KEY) {
                    event_wait_registration_t* registration = reinterpret_cast<event_wait_registration_t*>(entries[i].lpOverlapped);
                    events[event_count].fd = INVALID_SOCKET_FD;
                    events[event_count].handle = registration->event_handle;
                    events[event_count].completion_key = entries[i].lpCompletionKey;
                    events[event_count].context = registration->context;
                    events[event_count].event_type = EVENT_READ;
                    events[event_count].bytes_transferred = 0;
                    events[event_count].buffer = NULL;
                    event_count++;
                    continue;
                }
                if (entries[i].lpCompletionKey == ASYNC_IO_ERROR_KEY) {
                    events[event_count].fd = INVALID_SOCKET_FD;
                    events[event_count].handle = NULL;
                    events[event_count].completion_key = entries[i].lpCompletionKey;
                    events[event_count].context = reinterpret_cast<void*>(
                        static_cast<intptr_t>(entries[i].dwNumberOfBytesTransferred));
                    events[event_count].event_type = EVENT_ERROR;
                    events[event_count].bytes_transferred = entries[i].dwNumberOfBytesTransferred;
                    events[event_count].buffer = NULL;
                    event_count++;
                }
            }
        }
    }
    
    return event_count;
}

extern "C" int async_runtime_post_completion(async_runtime_t* runtime, uintptr_t completion_key, uintptr_t data) {
    if (!runtime || !runtime->iocp_handle) return -1;
    
    BOOL result = PostQueuedCompletionStatus(
        runtime->iocp_handle,
        (DWORD)data,
        completion_key,
        NULL  /* NULL overlapped indicates worker completion */
    );
    
    return result ? 0 : -1;
}

extern "C" int async_runtime_post_read(async_runtime_t* runtime, socket_fd_t fd, void* buffer, size_t len) {
    if (!runtime || fd == INVALID_SOCKET) return -1;
    
    iocp_context_t* io_ctx = alloc_iocp_context(runtime, fd, NULL, OP_READ);
    if (!io_ctx) return -1;
    
    if (buffer && len > 0) {
        io_ctx->wsa_buf.buf = (char*)buffer;
        io_ctx->wsa_buf.len = (ULONG)len;
    }
    
    DWORD flags = 0;
    DWORD bytes_received;
    int result = WSARecv(fd, &io_ctx->wsa_buf, 1, &bytes_received, &flags,
                         &io_ctx->overlapped, NULL);
    int err = WSAGetLastError();
    if (result == SOCKET_ERROR && err != WSA_IO_PENDING) {
        SPDLOG_DEBUG ("WSARecv failed on fd {}: {}", fd, err);
        free_iocp_context(runtime, io_ctx);
        return -1;
    }
    
    return 0;
}

extern "C" int async_runtime_post_write(async_runtime_t* runtime, socket_fd_t fd, void* buffer, size_t len) {
    if (!runtime || fd == INVALID_SOCKET || !buffer) return -1;
    
    iocp_context_t* io_ctx = alloc_iocp_context(runtime, fd, NULL, OP_WRITE);
    if (!io_ctx) return -1;
    
    io_ctx->wsa_buf.buf = (char*)buffer;
    io_ctx->wsa_buf.len = (ULONG)len;
    
    DWORD bytes_sent;
    int result = WSASend(fd, &io_ctx->wsa_buf, 1, &bytes_sent, 0,
                         &io_ctx->overlapped, NULL);
    
    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        free_iocp_context(runtime, io_ctx);
        return -1;
    }
    
    return 0;
}

/* Console support */

extern "C" console_type_t async_runtime_get_console_type(async_runtime_t* runtime) {
    return runtime ? runtime->console_type : CONSOLE_TYPE_NONE;
}
