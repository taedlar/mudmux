#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "console.hpp"

#include <cstring>
#include <mutex>
#include <thread>
#include <openssl/bio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "abstract.hpp"
#include "inbound.hpp"
#include "input_mode.hpp"
#include "async/console_worker.h"
#include "mudmux/hooks.h"
#include "mudmux/comm.h"
#include "mudmux/mudmux.h"

static std::mutex console_mutex;
static async_queue_t* console_queue{nullptr};
static console_worker_context_t* console_ctx{nullptr};

bool comm_init_console (async_runtime_t *runtime) {

    std::lock_guard<std::mutex> lock(console_mutex);
    auto console_type = async_runtime_get_console_type (runtime);

    // create console queue if it doesn't exist
    if (!console_queue) {
        console_queue = async_queue_create (100, 4096, ASYNC_QUEUE_DROP_OLDEST);
        if (!console_queue) {
            SPDLOG_ERROR ("failed to create console queue");
            return false;
        }
    }

    // start console worker thread to read from stdin and enqueue lines into console_queue
    console_ctx = console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);
    if (!console_ctx) {
        SPDLOG_ERROR ("failed to initialize console worker");
        async_queue_destroy (console_queue);
        console_queue = nullptr;
        return false;
    }

    // if COMM_SLOT_CONSOLE already exists, it could be setup to pipe/file input, we'll leave it
    // alone and only initialize the console worker to read from stdin (whatever it is) and
    // enqueue lines into console_queue.
    comm_abstract_ptr console_comm(COMM_SLOT_CONSOLE, comm_slots_mtx);
    if (!console_comm) {
        if (comm_abstract_add_file (nullptr, nullptr, COMM_SLOT_CONSOLE, C_LINE_INPUT) < 0) { // unlike adding BIOs, null file names will use stdin/stdout
            SPDLOG_ERROR ("failed to connect console communication");
            console_worker_destroy (console_ctx);
            console_ctx = nullptr;
            async_queue_destroy (console_queue);
            console_queue = nullptr;
            return false;
        }
    }

    // invoke connect hook for console user
    if (console_type == CONSOLE_TYPE_REAL) {
        SPDLOG_INFO ("----- connecting console user");
    }
    comm_invoke_connect (runtime, COMM_SLOT_CONSOLE, COMM_SLOT_CONSOLE);
    return true;
}

void comm_signal_console_eof (async_runtime_t *runtime) {
    std::lock_guard<std::mutex> lock(console_mutex);
    (void)runtime; // unused parameter
    if (console_ctx) {
        console_worker_set_eof(console_ctx);
    }
}

void comm_shutdown_console (async_runtime_t *runtime) {
    (void)runtime; // unused parameter
    std::lock_guard<std::mutex> lock(console_mutex);
    
    if (console_ctx) {
#ifdef _WIN32
        comm_set_char_input (COMM_SLOT_CONSOLE); /* interrupt line mode console read */
#endif
        bool stopped = console_worker_shutdown (console_ctx, 5000);
        if (!stopped) {
            SPDLOG_WARN ("console worker did not stop within timeout");
        }
        console_worker_destroy (console_ctx);
        console_ctx = nullptr;
        comm_set_line_input (COMM_SLOT_CONSOLE, true); /* re-enable echo to avoid leaving console in a bad state */
    }

    if (console_queue) {
        async_queue_destroy (console_queue);
        console_queue = nullptr;
    }
}

#ifdef _WIN32
bool comm_enable_virtual_terminal (int slot) {
    HANDLE handle = GetStdHandle (STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == NULL)
        return false;

    DWORD mode;
    if (!GetConsoleMode(handle, &mode))
        return false;

    SetConsoleOutputCP (CP_UTF8);
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;
    if (!SetConsoleMode(handle, mode)) {
        SPDLOG_WARN ("SetConsoleMode() failed for console stdout: {}", GetLastError());
        return false;
    }
    return true;
}
#else
bool comm_enable_virtual_terminal (int slot) {
    (void)slot;
    return true; // no-op on Linux/Unix, ANSI escape sequences are always supported
}
#endif

int comm_process_console_input (async_runtime_t *runtime, bool allow_reconnect) {
    bool disconnected = false;
    // check EOF on console worker and handle disconnect/re-connect if needed
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        if (console_ctx && console_worker_take_eof (console_ctx)) {
            auto console_type = async_runtime_get_console_type (runtime);
            if (console_type == CONSOLE_TYPE_REAL) {
#ifdef _WIN32
                comm_set_char_input (COMM_SLOT_CONSOLE); /* interrupt line mode console read */
#endif
            }
            bool stopped = console_worker_shutdown (console_ctx, 5000);
            if (!stopped) {
                SPDLOG_WARN ("console worker did not stop within timeout during reconnect");
            }
            console_worker_destroy (console_ctx);
            console_ctx = nullptr;
            comm_set_line_input (COMM_SLOT_CONSOLE, true); /* re-enable echo to avoid leaving console in a bad state */
            if (console_type == CONSOLE_TYPE_REAL && allow_reconnect) {
                // re-arm console worker for next console input (e.g., after Ctrl+D EOF)
                SPDLOG_INFO ("----- console user disconnected (press ENTER to reconnect)");
                console_ctx = console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);
            }
            disconnected = true;
        }

        comm_abstract_ptr console_comm(COMM_SLOT_CONSOLE, comm_slots_mtx);
        if (!console_comm && allow_reconnect && console_ctx) {
            if (!async_queue_is_empty(console_queue)) {
                SPDLOG_INFO ("----- reconnecting console communication");
                if (comm_abstract_add_file (nullptr, nullptr, COMM_SLOT_CONSOLE, C_LINE_INPUT) < 0) {
                    SPDLOG_ERROR ("failed to re-connect console communication");
                    return -1;
                }
                async_queue_clear (console_queue); // clear any pending lines in the queue
                comm_invoke_connect (runtime, COMM_SLOT_CONSOLE, COMM_SLOT_CONSOLE); // invoke connect hook for console user
                return 0;
            }
        }
    }

    // drain completed lines from the console queue and invoke the shared inbound hook path
    char console_line_buffer[4096];
    size_t line_len = 0;
    while (async_queue_dequeue (console_queue, console_line_buffer, sizeof(console_line_buffer), &line_len)) {
        if (line_len > 0 && console_line_buffer[line_len - 1] == '\0') {
            --line_len;
        }
        if (!comm_refill_inbound_buffers (COMM_SLOT_CONSOLE, console_line_buffer, line_len)) {
            SPDLOG_WARN ("failed to refill inbound buffers for console input");
            console_worker_set_eof (console_ctx); // signal EOF to console worker to stop reading
            break;
        }
    }
    comm_process_input (runtime, COMM_SLOT_CONSOLE);

    if (disconnected) {
        comm_abstract_ptr comm (COMM_SLOT_CONSOLE, comm_slots_mtx);
        if (comm) {
            if (!(comm->flags & C_CLOSING))
                comm_invoke_disconnect (runtime, COMM_SLOT_CONSOLE); // invoke disconnect hook for console user
            comm_abstract_remove (COMM_SLOT_CONSOLE); // remove console from comm_abstract
        }
        async_queue_clear (console_queue); // clear any pending lines in the queue
        if (!allow_reconnect) {
            // stdin is either a pipe or a file, so EOF means the end of input; shut down the server
            SPDLOG_INFO ("EOF detected, shutting down server");
            mudmux_shutdown();
        }
    }

    return 0;
}
