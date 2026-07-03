#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "console.h"

#include <cstring>
#include <mutex>
#include <thread>
#include <openssl/bio.h>

#include "abstract.h"
#include "async/console_worker.h"
#include "mudmux/hooks.h"
#include "mudmux/comm.h"
#include "mudmux/mudmux.h"

static std::mutex console_mutex;
static async_queue_t* console_queue{nullptr};
static console_worker_context_t* console_ctx{nullptr};

extern "C" bool comm_init_console (async_runtime_t *runtime) {

    std::lock_guard<std::mutex> lock(console_mutex);
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

    // Register stdin/stdout communication at slot #0
    if (comm_abstract_add_file (nullptr, nullptr, COMM_SLOT_CONSOLE, 0) < 0) {
        SPDLOG_ERROR ("failed to connect console communication");
        console_worker_destroy (console_ctx);
        console_ctx = nullptr;
        async_queue_destroy (console_queue);
        console_queue = nullptr;
        return false;
    }
    // invoke connect hook for console user
    if (console_ctx->console_type == CONSOLE_TYPE_REAL) {
        SPDLOG_INFO ("----- connecting console user");
    }
    mudmux_invoke_hook (MUDMUX_HOOK_CONNECT,
        async_runtime_get_context(runtime), COMM_SLOT_CONSOLE, nullptr, 0); // invoke connect hook for console user
    return true;
}

extern "C" void comm_shutdown_console (async_runtime_t *runtime) {
    (void)runtime; // unused parameter
    std::lock_guard<std::mutex> lock(console_mutex);
    
    if (console_ctx) {
        bool stopped = console_worker_shutdown (console_ctx, 5000);
        if (!stopped) {
            SPDLOG_WARN ("console worker did not stop within timeout");
            console_worker_destroy (console_ctx);
        }
        console_ctx = nullptr;
    }
    if (console_queue) {
        async_queue_destroy (console_queue);
        console_queue = nullptr;
    }
}

extern "C" int comm_process_console_input (async_runtime_t *runtime, bool allow_reconnect) {
    std::lock_guard<std::mutex> lock(console_mutex);
    
    // Handle worker thread input (stdin/console)
    if (console_ctx) {
        if (console_worker_take_eof (console_ctx)) {
            auto console_type = console_ctx->console_type;
            console_worker_destroy (console_ctx);
            console_ctx = nullptr;
            comm_abstract_remove (COMM_SLOT_CONSOLE); // remove console from comm_abstract
            if (console_type == CONSOLE_TYPE_REAL && allow_reconnect) {
                // re-arm console worker for next console input (e.g., after Ctrl+D EOF)
                SPDLOG_INFO ("----- console user disconnected (press ENTER to reconnect)");
                console_ctx = console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);
                return 0;
            }
            else {
                // stdin is either a pipe or a file, so EOF means the end of input; shut down the server
                SPDLOG_INFO ("EOF detected, shutting down server");
                mudmux_shutdown();
                return 0;
            }
        }

        // check if console communication needs re-connection (e.g., after Ctrl+D EOF on a real console)
        if (!comm_abstract_get_rbio(COMM_SLOT_CONSOLE) && allow_reconnect) {
            SPDLOG_INFO ("----- recconnecting console communication");
            if (comm_abstract_add_file (nullptr, nullptr, COMM_SLOT_CONSOLE, 0) < 0) {
                SPDLOG_ERROR ("failed to re-connect console communication");
                return -1;
            }
            async_queue_clear (console_queue); // clear any pending lines in the queue
            mudmux_invoke_hook (MUDMUX_HOOK_CONNECT,
                async_runtime_get_context(runtime), COMM_SLOT_CONSOLE, nullptr, 0); // invoke connect hook for console user
            return 1;
        }

        // drain completed lines from the console queue and invoke the shared inbound hook path
        char console_line_buffer[4096];
        while (async_queue_dequeue (console_queue, console_line_buffer, sizeof(console_line_buffer), nullptr)) {
            comm_invoke_inbound_message(runtime, COMM_SLOT_CONSOLE, console_line_buffer, strlen(console_line_buffer));
        }
        return 1;
    }
    return -1; // no console input available
}
