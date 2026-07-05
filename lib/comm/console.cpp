#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "console.h"

#include <cstring>
#include <mutex>
#include <thread>
#include <openssl/bio.h>

#include "abstract.h"
#include "input_mode.h"
#include "async/console_worker.h"
#include "mudmux/hooks.h"
#include "mudmux/comm.h"
#include "mudmux/mudmux.h"

static std::mutex console_mutex;
static async_queue_t* console_queue{nullptr};
static console_worker_context_t* console_ctx{nullptr};

extern "C" bool comm_init_console (async_runtime_t *runtime) {

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
    if (!comm_abstract_get(COMM_SLOT_CONSOLE)) {
        if (comm_abstract_add_file (nullptr, nullptr, COMM_SLOT_CONSOLE, 0) < 0) { // unlike adding BIOs, null file names will use stdin/stdout
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
    mudmux_invoke_hook (MUDMUX_HOOK_CONNECT,
        async_runtime_get_context(runtime), COMM_SLOT_CONSOLE, nullptr, 0); // invoke connect hook for console user
    return true;
}

extern "C" void comm_signal_console_eof (async_runtime_t *runtime) {
    std::lock_guard<std::mutex> lock(console_mutex);
    (void)runtime; // unused parameter
    if (console_ctx) {
        console_worker_set_eof(console_ctx);
    }
}

extern "C" void comm_shutdown_console (async_runtime_t *runtime) {
    (void)runtime; // unused parameter
    std::lock_guard<std::mutex> lock(console_mutex);
    
    if (console_ctx) {
#ifdef _WIN32
        comm_set_console_char_input (); /* interrupt line mode console read */
#endif
        bool stopped = console_worker_shutdown (console_ctx, 5000);
        if (!stopped) {
            SPDLOG_WARN ("console worker did not stop within timeout");
        }
        console_worker_destroy (console_ctx);
        console_ctx = nullptr;
        comm_set_console_line_input (true); /* re-enable echo to avoid leaving console in a bad state */
    }

    if (console_queue) {
        async_queue_destroy (console_queue);
        console_queue = nullptr;
    }
}

extern "C" int comm_process_console_input (async_runtime_t *runtime, bool allow_reconnect) {
    bool disconnected = false;
    // check EOF on console worker and handle disconnect/re-connect if needed
    {
        std::lock_guard<std::mutex> lock(console_mutex);
        if (console_ctx && console_worker_take_eof (console_ctx)) {
            auto console_type = async_runtime_get_console_type (runtime);
            if (console_type == CONSOLE_TYPE_REAL) {
#ifdef _WIN32
                comm_set_console_char_input (); /* interrupt line mode console read */
#endif
            }
            bool stopped = console_worker_shutdown (console_ctx, 5000);
            if (!stopped) {
                SPDLOG_WARN ("console worker did not stop within timeout during reconnect");
            }
            console_worker_destroy (console_ctx);
            console_ctx = nullptr;
            comm_set_console_line_input (true); /* re-enable echo to avoid leaving console in a bad state */
            if (console_type == CONSOLE_TYPE_REAL && allow_reconnect) {
                // re-arm console worker for next console input (e.g., after Ctrl+D EOF)
                SPDLOG_INFO ("----- console user disconnected (press ENTER to reconnect)");
                console_ctx = console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);
            }
            disconnected = true;
        }

        if (!comm_abstract_get(COMM_SLOT_CONSOLE) && allow_reconnect && console_ctx) {
            if (!async_queue_is_empty(console_queue)) {
                SPDLOG_INFO ("----- reconnecting console communication");
                if (comm_abstract_add_file (nullptr, nullptr, COMM_SLOT_CONSOLE, 0) < 0) {
                    SPDLOG_ERROR ("failed to re-connect console communication");
                    return -1;
                }
                async_queue_clear (console_queue); // clear any pending lines in the queue
                mudmux_invoke_hook (MUDMUX_HOOK_CONNECT,
                    async_runtime_get_context(runtime), COMM_SLOT_CONSOLE, nullptr, 0); // invoke connect hook for console user
                return 0;
            }
        }
    }

    // drain completed lines from the console queue and invoke the shared inbound hook path
    char console_line_buffer[4096];
    while (async_queue_dequeue (console_queue, console_line_buffer, sizeof(console_line_buffer), nullptr)) {
        comm_invoke_inbound_message(runtime, COMM_SLOT_CONSOLE, console_line_buffer, strlen(console_line_buffer));
    }

    if (disconnected) {
        auto comm = comm_abstract_get(COMM_SLOT_CONSOLE);
        if (comm) {
            if (!(comm_get_flags(comm) & C_SOCKET_CLOSING))
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
