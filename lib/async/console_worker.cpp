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
#include "async_event.h"

#include <chrono>
#include <condition_variable>
#include <string.h>
#include <new>
#include <mutex>
#include <thread>

struct console_worker_context_s {
    async_queue_t* line_queue;     /**< Queue for completed lines */
    async_runtime_t* runtime;      /**< Runtime for posting completions */
    uintptr_t completion_key;      /**< Completion key for runtime */
    std::mutex state_mutex;        /**< Guards worker state flags */
    std::mutex lifecycle_mutex;    /**< Guards stop/done lifecycle state */
    std::condition_variable done_cv; /**< Signaled when worker thread exits */
    bool eof_detected;             /**< Set true when stdin EOF is observed */
    bool stop_requested;           /**< Signals worker thread stop */
    bool worker_finished;          /**< Signals worker thread exit */
    std::thread worker;            /**< Native C++17 worker thread */
    bool worker_started;           /**< True once worker thread has started */
#ifndef _WIN32
    async_event_t stop_event;      /**< Pollable POSIX stop signal */
#else
    HANDLE stop_event;             /**< Native stop event for WaitForMultipleObjects */
#endif
};

#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static bool console_worker_is_stop_requested(console_worker_context_t* ctx) {
    std::lock_guard<std::mutex> lock(ctx->lifecycle_mutex);
    return ctx->stop_requested;
}

static void console_worker_mark_done(console_worker_context_t* ctx) {
    {
        std::lock_guard<std::mutex> lock(ctx->lifecycle_mutex);
        ctx->worker_finished = true;
    }
    ctx->done_cv.notify_all();
}

static bool console_worker_wait_done(console_worker_context_t* ctx, int timeout_ms) {
    std::unique_lock<std::mutex> lock(ctx->lifecycle_mutex);

    if (timeout_ms < 0) {
        ctx->done_cv.wait(lock, [ctx] { return ctx->worker_finished; });
        return true;
    }

    return ctx->done_cv.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [ctx] { return ctx->worker_finished; }
    );
}

static void console_worker_signal_stop(console_worker_context_t* ctx) {
    if (!ctx) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ctx->lifecycle_mutex);
        ctx->stop_requested = true;
    }

#ifdef _WIN32
    if (ctx->stop_event) {
        SetEvent(ctx->stop_event);
    }
#else
    async_event_set(&ctx->stop_event);
#endif
}

void console_worker_set_eof(console_worker_context_t* ctx) {
    if (!ctx)
        return;

    std::lock_guard<std::mutex> lock(ctx->state_mutex);
    ctx->eof_detected = true;

    /* Wake the main loop so EOF can be handled on the main thread. */
    async_runtime_post_completion(ctx->runtime, ctx->completion_key, 0);
}

extern "C" const char* console_type_str(console_type_t type) {
    switch (type) {
        case CONSOLE_TYPE_NONE: return "console:none"; // deliberately no console
        case CONSOLE_TYPE_TTY: return "console:tty"; // Windows console or POSIX TTY, isatty(STDIN_FILENO) returns true
        case CONSOLE_TYPE_PIPE: return "console:pipe"; // No random access, no EOF detection, no seek (FILE_TYPE_PIPE on Windows)
        case CONSOLE_TYPE_FILE: return "console:file"; // Has EOF detection, random access, and seek (FILE_TYPE_DISK on Windows)
        default: return "console:unknown-type";
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
        return CONSOLE_TYPE_TTY;
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
        SPDLOG_DEBUG ("stdin is a TTY, assuming real console");
        return CONSOLE_TYPE_TTY;
    }

    /* Check if pipe or file via stat */
    struct stat st;
    if (fstat(STDIN_FILENO, &st) == 0) {
        if (S_ISFIFO(st.st_mode)) {
            SPDLOG_DEBUG ("stdin is a FIFO, assuming pipe");
            return CONSOLE_TYPE_PIPE;
        } else /*if (S_ISREG(st.st_mode))*/ {
            SPDLOG_DEBUG ("fstat succeeded on stdin, assuming file");
            return CONSOLE_TYPE_FILE;
        }
    }

    SPDLOG_DEBUG ("stdin is not a TTY, pipe, or file ({}), assuming none", errno);
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
        console_worker_mark_done(cctx);
        return;
    }

    HANDLE hStopEvent = cctx->stop_event;
    if (!hStopEvent) {
        SPDLOG_ERROR("failed to get native event handle");
        console_worker_mark_done(cctx);
        return;
    }

    /* Set UTF-8 code page */
    SetConsoleCP(CP_UTF8);

    char line_buffer[CONSOLE_MAX_LINE];
    WCHAR wide_buffer[CONSOLE_MAX_LINE];
    DWORD chars_read = 0;

    SPDLOG_INFO ("console worker started (type: {})", console_type_str (console_type));

    while (!console_worker_is_stop_requested(cctx)) {
        /* Wait for stdin to be signaled OR stop event */
        HANDLE events[2] = { hStdin, hStopEvent };
        DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        
        if (wait_result == WAIT_OBJECT_0) {
            /* stdin is signaled - data available, read it synchronously */
            BOOL result;
            DWORD err {ERROR_SUCCESS};
            if (console_type == CONSOLE_TYPE_TTY) {
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
            } else if (console_type == CONSOLE_TYPE_PIPE) {
                /*
                 * Anonymous-pipe handles can appear signaled before readable
                 * bytes are available. Guard ReadFile with PeekNamedPipe so
                 * shutdown does not get stuck in a blocking pipe read.
                 */
                DWORD available = 0;
                if (!PeekNamedPipe(hStdin, NULL, 0, NULL, &available, NULL)) {
                    err = GetLastError();
                    if (err == ERROR_BROKEN_PIPE) {
                        SPDLOG_INFO ("EOF detected (pipe)");
                        console_worker_set_eof(cctx);
                        break;
                    }
                    SPDLOG_ERROR ("PeekNamedPipe() failed: {}", err);
                    break;
                }

                if (available == 0) {
                    continue;
                }

                DWORD to_read = (available < static_cast<DWORD>(CONSOLE_MAX_LINE - 1))
                    ? available
                    : static_cast<DWORD>(CONSOLE_MAX_LINE - 1);
                result = ReadFile(hStdin, line_buffer, to_read, &chars_read, NULL);
                if (!result)
                    err = GetLastError();
            } else {
                /* File input: regular blocking ReadFile is acceptable. */
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
    console_worker_mark_done(cctx);
}
#else
/**
 * POSIX console worker thread procedure
 */
static void console_worker_proc_posix(console_worker_context_t* cctx) {
    char line_buffer[CONSOLE_MAX_LINE];
    async_wait_handle_t stop_fd = async_event_get_wait_handle(&cctx->stop_event);

    console_type_t console_type = async_runtime_get_console_type(cctx->runtime);
    SPDLOG_INFO ("console worker started (type: {})", console_type_str (console_type));

    if (stop_fd < 0) {
        SPDLOG_ERROR ("console worker missing stop event fd");
        console_worker_mark_done(cctx);
        return;
    }

    while (!console_worker_is_stop_requested(cctx)) {
        struct pollfd pollfds[2];
        pollfds[0].fd = STDIN_FILENO;
        pollfds[0].events = POLLIN;
        pollfds[0].revents = 0;
        pollfds[1].fd = stop_fd;
        pollfds[1].events = POLLIN;
        pollfds[1].revents = 0;

        int ret = poll (pollfds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) {
                continue; /* Interrupted by signal, retry */
            }
            SPDLOG_ERROR ("poll() failed: {}", strerror(errno));
            break;
        }

        if (pollfds[1].revents & POLLIN) {
            break;
        }

        if (!(pollfds[0].revents & POLLIN)) {
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
    console_worker_mark_done(cctx);
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
    ctx->stop_requested = false;
    ctx->worker_finished = false;
    ctx->worker_started = false;

#ifdef _WIN32
    ctx->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ctx->stop_event) {
        delete ctx;
        return NULL;
    }
#else
    if (!async_event_init(&ctx->stop_event, true, false)) {
        delete ctx;
        return NULL;
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
        async_event_destroy(&ctx->stop_event);
    #else
        if (ctx->stop_event) CloseHandle(ctx->stop_event);
#endif
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

    bool stopped = console_worker_wait_done(ctx, wait_timeout);
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
        (void)console_worker_wait_done(ctx, -1);
        if (ctx->worker.joinable()) {
            ctx->worker.join();
        }
        ctx->worker_started = false;
    }

#ifndef _WIN32
    async_event_destroy(&ctx->stop_event);
#else
    if (ctx->stop_event) {
        CloseHandle(ctx->stop_event);
        ctx->stop_event = NULL;
    }
#endif

    delete ctx;
}

extern "C" bool console_worker_take_eof(console_worker_context_t* ctx) {
    bool saw_eof;

    if (!ctx)
        return false;

    std::lock_guard<std::mutex> lock(ctx->state_mutex);
    saw_eof = ctx->eof_detected;
    ctx->eof_detected = false;

    return saw_eof;
}
