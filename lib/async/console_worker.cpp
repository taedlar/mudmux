/**
 * @file console_worker.cpp
 * @brief Console input worker implementation
 *
 * Platform-agnostic console input handling via worker thread.
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */
#include "console_worker.h"
#include "sync.h"

#include <string.h>
#include <new>
#include <thread>

struct console_worker_context_s {
    async_queue_t* line_queue;     /**< Queue for completed lines */
    async_runtime_t* runtime;      /**< Runtime for posting completions */
    uintptr_t completion_key;      /**< Completion key for runtime */
    platform_mutex_t state_mutex;  /**< Guards worker state flags */
    bool eof_detected;             /**< Set true when stdin EOF is observed */
    platform_event_t stop_event;   /**< Signals worker thread stop */
    platform_event_t done_event;   /**< Signals worker thread exit */
    std::thread worker;            /**< Native C++17 worker thread */
    bool worker_started;           /**< True once worker thread has started */
#ifndef _WIN32
    int stop_pipe_fds[2];          /**< Self-pipe for POSIX stop signaling: [0]=read, [1]=write */
#endif
};

#ifndef _WIN32
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <errno.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void console_worker_signal_stop(console_worker_context_t* ctx) {
    if (!ctx) {
        return;
    }

    platform_event_set(&ctx->stop_event);

#ifndef _WIN32
    if (ctx->stop_pipe_fds[1] >= 0) {
        char byte = 1;
        ssize_t written;
        do {
            written = write(ctx->stop_pipe_fds[1], &byte, 1);
        } while (written < 0 && errno == EINTR);
    }
#endif
}

void console_worker_set_eof(console_worker_context_t* ctx) {
    if (!ctx)
        return;

    platform_mutex_lock(&ctx->state_mutex);
    ctx->eof_detected = true;
    platform_mutex_unlock(&ctx->state_mutex);

    /* Wake the main loop so EOF can be handled on the main thread. */
    async_runtime_post_completion(ctx->runtime, ctx->completion_key, 0);
}

/**
 * Convert console type to string
 */
extern "C" const char* console_type_str(console_type_t type) {
    switch (type) {
        case CONSOLE_TYPE_NONE: return "NONE";
        case CONSOLE_TYPE_REAL: return "REAL";
        case CONSOLE_TYPE_PIPE: return "PIPE";
        case CONSOLE_TYPE_FILE: return "FILE";
        default: return "UNKNOWN";
    }
}

/**
 * Detect console type (platform-specific)
 */
extern "C" console_type_t console_detect_type(void) {
#ifdef _WIN32
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE || hStdin == NULL) {
        return CONSOLE_TYPE_NONE;
    }

    DWORD mode;
    if (GetConsoleMode(hStdin, &mode)) {
        /* Real Windows console */
        return CONSOLE_TYPE_REAL;
    }

    /* Not a console - check if pipe or file */
    DWORD file_type = GetFileType(hStdin);
    if (file_type == FILE_TYPE_PIPE) {
        return CONSOLE_TYPE_PIPE;
    } else if (file_type == FILE_TYPE_DISK) {
        return CONSOLE_TYPE_FILE;
    }

    return CONSOLE_TYPE_NONE;
#else
    /* POSIX: use isatty */
    if (isatty(STDIN_FILENO)) {
        return CONSOLE_TYPE_REAL;
    }

    /* Check if pipe or file via stat */
    struct stat st;
    if (fstat(STDIN_FILENO, &st) == 0) {
        if (S_ISFIFO(st.st_mode)) {
            return CONSOLE_TYPE_PIPE;
        } else if (S_ISREG(st.st_mode)) {
            return CONSOLE_TYPE_FILE;
        }
    }

    return CONSOLE_TYPE_NONE;
#endif
}

#ifdef _WIN32
/**
 * Windows console worker thread procedure
 */
static void console_worker_proc_win32(console_worker_context_t* cctx) {
    console_type_t console_type = async_runtime_get_console_type(cctx->runtime);

    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (!hStdin || hStdin == INVALID_HANDLE_VALUE) {
        SPDLOG_ERROR("invalid STD_INPUT_HANDLE for console worker");
        platform_event_set(&cctx->done_event);
        return;
    }

    HANDLE hStopEvent = (HANDLE)platform_event_get_native_handle(&cctx->stop_event);
    if (!hStopEvent) {
        SPDLOG_ERROR("failed to get native event handle");
        platform_event_set(&cctx->done_event);
        return;
    }

    /* Set UTF-8 code page */
    SetConsoleCP(CP_UTF8);

    char line_buffer[CONSOLE_MAX_LINE];
    WCHAR wide_buffer[CONSOLE_MAX_LINE];
    DWORD chars_read = 0;

    SPDLOG_INFO ("console worker started (type: {})", console_type_str (console_type));

    while (!platform_event_wait(&cctx->stop_event, 0)) {
        /* Wait for stdin to be signaled OR stop event */
        HANDLE events[2] = { hStdin, hStopEvent };
        DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        
        if (wait_result == WAIT_OBJECT_0) {
            /* stdin is signaled - data available, read it synchronously */
            BOOL result;
            DWORD err {ERROR_SUCCESS};
            if (console_type == CONSOLE_TYPE_REAL) {
                /* Real console: ReadConsoleW (Unicode).
                 * dwCtrlWakeupMask is only honored by ReadConsoleW, not ReadConsoleA.
                 * Bit 27 (ESC = 0x1B) lets ESCAPE unblock ReadConsole in cooked mode.
                 */
                CONSOLE_READCONSOLE_CONTROL rdcon;
                rdcon.nLength = sizeof(rdcon);
                rdcon.nInitialChars = 0;
                rdcon.dwCtrlWakeupMask = (1UL << 27); /* ESC = ASCII 27 */
                rdcon.dwControlKeyState = 0;
                DWORD wchars_read = 0;
                result = ReadConsoleW (hStdin, wide_buffer, CONSOLE_MAX_LINE - 1, &wchars_read, &rdcon);
                if (!result)
                    err = GetLastError();
                if (result && wchars_read > 0) {
                    /* ESC wakeup: last wide char is ESC - discard the partial line */
                    if (wide_buffer[wchars_read - 1] == L'\x1B')
                        continue;
                    /* Convert UTF-16 → UTF-8 into line_buffer */
                    int nb = WideCharToMultiByte (CP_UTF8, 0, wide_buffer, (int)wchars_read,
                                                 line_buffer, CONSOLE_MAX_LINE - 1, NULL, NULL);
                    if (nb <= 0) {
                        SPDLOG_ERROR ("WideCharToMultiByte() failed: {}", GetLastError());
                        continue;
                    }
                    chars_read = (DWORD)nb;
                }
                else if (result) {
                    /* ReadConsoleW returned TRUE with 0 chars: spurious wakeup from an
                     * injected input event (e.g. VK_ESCAPE key-up during mode switch).
                     * This is not EOF; loop back and wait for real input. */
                    SPDLOG_DEBUG ("console woken with no chars (spurious wakeup), continuing\n");
                    continue;
                }
                else {
                    chars_read = 0;
                }
            } else {
                /* Pipe or file: Use ReadFile (synchronous) */
                result = ReadFile (hStdin, line_buffer, CONSOLE_MAX_LINE - 1, &chars_read, NULL);
                if (!result)
                    err = GetLastError();
            }

            if (!result) {
                if (err == ERROR_BROKEN_PIPE) {
                    SPDLOG_INFO ("EOF detected (pipe)");
                    console_worker_set_eof(cctx);
                    break;
                }
                SPDLOG_ERROR ("console read failed: {}", err);
                break;
            }

            if (chars_read > 0) {
                /* Null-terminate */
                line_buffer[chars_read] = '\0';

#ifdef _WIN32
                if (line_buffer[0] == '\x1A') {
                    if (strspn (line_buffer, "\x1A\r\n") == chars_read) {
                        /* Ctrl+Z + ENTER (EOF) on Windows console */
                        SPDLOG_INFO ("EOF detected (Ctrl+Z)");
                        console_worker_set_eof(cctx);
                        break;
                    }
                }
#endif
                /* Enqueue line */
                if (!async_queue_enqueue(cctx->line_queue, line_buffer, chars_read + 1)) {
                    SPDLOG_WARN  ("line queue full, dropping line");
                }

                /* Post completion to wake main thread (blocked in async_runtime_wait) */
                SPDLOG_DEBUG ("read {} chars from console", chars_read);
                async_runtime_post_completion (cctx->runtime, cctx->completion_key, chars_read);
            } else {
                /* EOF */
                SPDLOG_INFO ("EOF detected (no more data)");
                console_worker_set_eof(cctx);
                break;
            }
        } else if (wait_result == WAIT_OBJECT_0 + 1) {
            /* Stop event signaled - exit cleanly */
            break;
        } else {
            /* Error or unexpected result */
            SPDLOG_ERROR ("WaitForMultipleObjects() failed: {} (error: {})", wait_result, GetLastError());
            break;
        }
    }

    SPDLOG_INFO ("console worker stopped");
    platform_event_set(&cctx->done_event);
}
#else
/**
 * POSIX console worker thread procedure
 */
static void console_worker_proc_posix(console_worker_context_t* cctx) {
    char line_buffer[CONSOLE_MAX_LINE];
    int stop_fd = cctx->stop_pipe_fds[0];

    console_type_t console_type = async_runtime_get_console_type(cctx->runtime);
    SPDLOG_INFO ("console worker started (type: {})", console_type_str (console_type));

    while (!platform_event_wait(&cctx->stop_event, 0)) {
        /* Block in select() on stdin and the stop-pipe read end.
         * NULL timeout means infinite wait - no polling needed. */
        fd_set readfds;
        FD_ZERO (&readfds);
        FD_SET (STDIN_FILENO, &readfds);

        int nfds = STDIN_FILENO + 1;
        struct timeval poll_timeout;
        struct timeval* timeout_ptr = NULL; /* NULL = infinite, used when pipe is available */
        if (stop_fd >= 0) {
            FD_SET (stop_fd, &readfds);
            if (stop_fd >= nfds)
                nfds = stop_fd + 1;
        } else {
            /* Pipe unavailable (creation failed at init): fall back to 10ms polling
             * so the should_stop flag check at the top of the loop is eventually reached. */
            poll_timeout.tv_sec = 0;
            poll_timeout.tv_usec = 10000;
            timeout_ptr = &poll_timeout;
        }

        int ret = select (nfds, &readfds, NULL, NULL, timeout_ptr);
        if (ret < 0) {
            if (errno == EINTR) {
                continue; /* Interrupted by signal, retry */
            }
            SPDLOG_ERROR ("select() failed: {}", strerror(errno));
            break;
        }

        /* Stop pipe signaled - exit cleanly */
        if (stop_fd >= 0 && FD_ISSET(stop_fd, &readfds)) {
            break;
        }

        if (!FD_ISSET (STDIN_FILENO, &readfds)) {
            continue;
        }

        /* stdin is readable */
        ssize_t bytes_read = read (STDIN_FILENO, line_buffer, CONSOLE_MAX_LINE - 1);
        if (bytes_read < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            SPDLOG_ERROR ("read() failed: {}", strerror(errno));
            break;
        } else if (bytes_read == 0) {
            /* EOF */
            SPDLOG_INFO ("console EOF detected");
            console_worker_set_eof (cctx);
            break;
        }

        /* Null-terminate */
        line_buffer[bytes_read] = '\0';

        /* Enqueue line */
        if (!async_queue_enqueue(cctx->line_queue, line_buffer, bytes_read + 1)) {
            SPDLOG_WARN ("console line queue full, dropping line");
        }

        /* Post completion to wake main thread */
        async_runtime_post_completion (cctx->runtime, cctx->completion_key, bytes_read);
    }

    SPDLOG_INFO ("console worker stopped");
    platform_event_set(&cctx->done_event);
}
#endif

/**
 * Initialize console worker
 */
extern "C" console_worker_context_t* console_worker_init(async_runtime_t* runtime, async_queue_t* queue, uintptr_t completion_key) {
    if (!runtime || !queue) {
        SPDLOG_ERROR ("console_worker_init: invalid arguments");
        return NULL;
    }

    console_worker_context_t* ctx = new (std::nothrow) console_worker_context_t();
    if (!ctx) {
        SPDLOG_ERROR ("console_worker_init: out of memory");
        return NULL;
    }

    ctx->line_queue = queue;
    ctx->runtime = runtime;
    ctx->completion_key = completion_key;
    ctx->eof_detected = false;
    ctx->worker_started = false;

    if (!platform_mutex_init(&ctx->state_mutex)) {
        delete ctx;
        return NULL;
    }

    if (!platform_event_init(&ctx->stop_event, true, false)) {
        platform_mutex_destroy(&ctx->state_mutex);
        delete ctx;
        return NULL;
    }

    if (!platform_event_init(&ctx->done_event, true, false)) {
        platform_event_destroy(&ctx->stop_event);
        platform_mutex_destroy(&ctx->state_mutex);
        delete ctx;
        return NULL;
    }

#ifndef _WIN32
    /* Create self-pipe for stop signaling so the worker can block in select() indefinitely
     * and be woken by either stdin becoming readable or a stop signal. */
    ctx->stop_pipe_fds[0] = ctx->stop_pipe_fds[1] = -1;
    if (pipe(ctx->stop_pipe_fds) != 0) {
        SPDLOG_WARN ("console_worker_init: failed to create stop pipe: {}", strerror(errno));
        /* Non-fatal: worker falls back to polling with timeout */
    }
#endif

    console_type_t console_type = async_runtime_get_console_type(ctx->runtime);
    if (console_type == CONSOLE_TYPE_NONE) {
        SPDLOG_WARN ("no console detected, worker will not start");
        /* Don't treat as fatal - allow mudlib to run without console */
        return ctx;
    }

#ifdef _WIN32
    try {
        ctx->worker = std::thread(console_worker_proc_win32, ctx);
        ctx->worker_started = true;
    } catch (const std::system_error&) {
        ctx->worker_started = false;
    }
#else
    try {
        ctx->worker = std::thread(console_worker_proc_posix, ctx);
        ctx->worker_started = true;
    } catch (const std::system_error&) {
        ctx->worker_started = false;
    }
#endif

    if (!ctx->worker_started) {
        SPDLOG_ERROR ("failed to create console worker thread");
#ifndef _WIN32
        if (ctx->stop_pipe_fds[0] >= 0) close(ctx->stop_pipe_fds[0]);
        if (ctx->stop_pipe_fds[1] >= 0) close(ctx->stop_pipe_fds[1]);
#endif
        platform_event_destroy(&ctx->done_event);
        platform_event_destroy(&ctx->stop_event);
        platform_mutex_destroy(&ctx->state_mutex);
        delete ctx;
        return NULL;
    }

    return ctx;
}

/**
 * Shutdown console worker
 */
extern "C" bool console_worker_shutdown(console_worker_context_t* ctx, int timeout_ms) {
    if (!ctx || !ctx->worker_started) {
        return true; /* No worker to shutdown */
    }

    console_worker_signal_stop(ctx);

    int wait_timeout = timeout_ms;
    if (timeout_ms < 0) {
        wait_timeout = -1;
    }

    bool stopped = platform_event_wait(&ctx->done_event, wait_timeout);
    if (stopped && ctx->worker.joinable()) {
        ctx->worker.join();
        ctx->worker_started = false;
    }

    return stopped;
}

/**
 * Destroy console worker
 */
extern "C" void console_worker_destroy(console_worker_context_t* ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->worker_started) {
        console_worker_signal_stop(ctx);
        (void)platform_event_wait(&ctx->done_event, -1);
        if (ctx->worker.joinable()) {
            ctx->worker.join();
        }
        ctx->worker_started = false;
    }

#ifndef _WIN32
    if (ctx->stop_pipe_fds[0] >= 0) {
        close(ctx->stop_pipe_fds[0]);
        ctx->stop_pipe_fds[0] = -1;
    }
    if (ctx->stop_pipe_fds[1] >= 0) {
        close(ctx->stop_pipe_fds[1]);
        ctx->stop_pipe_fds[1] = -1;
    }
#endif

    platform_event_destroy(&ctx->done_event);
    platform_event_destroy(&ctx->stop_event);
    platform_mutex_destroy(&ctx->state_mutex);

    delete ctx;
}

extern "C" bool console_worker_take_eof(console_worker_context_t* ctx) {
    bool saw_eof;

    if (!ctx)
        return false;

    platform_mutex_lock(&ctx->state_mutex);
    saw_eof = ctx->eof_detected;
    ctx->eof_detected = false;
    platform_mutex_unlock(&ctx->state_mutex);

    return saw_eof;
}
