#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "abstract.h"
#include "console.h"
#include "inbound.h"
#include "async/console_worker.h"
#include "mudmux/mudmux.h"
#include <openssl/bio.h>
#include <cstring>
#include <mutex>

static std::mutex console_mutex;
static async_queue_t* console_queue{nullptr};
static console_worker_context_t* console_ctx{nullptr};
static char console_line_buffer[4096];

extern "C" bool comm_init_console (async_runtime_t *runtime) {

    std::lock_guard<std::mutex> lock(console_mutex);
    if (!console_queue) {
        console_queue = async_queue_create (100, 4096, ASYNC_QUEUE_DROP_OLDEST);
        if (!console_queue) {
            SPDLOG_ERROR ("failed to create console queue");
            return false;
        }
    }

    console_ctx = console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);
    if (!console_ctx) {
        SPDLOG_ERROR ("failed to initialize console worker");
        async_queue_destroy (console_queue);
        console_queue = nullptr;
        return false;
    }

    // invoke connect hook for console user
    if (comm_abstract_add (STDIN_FILENO) < 0) {
        SPDLOG_ERROR ("failed to register STDIN in comm_abstract_add()");
        console_worker_destroy (console_ctx);
        console_ctx = nullptr;
        async_queue_destroy (console_queue);
        console_queue = nullptr;
        return false;
    }
    mudmux_invoke_hook (MUDMUX_HOOK_CONNECT,
        async_runtime_get_context(runtime), 0, nullptr, 0); // invoke connect hook for console user
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

extern "C" int comm_process_console_input (async_runtime_t *runtime) {
    std::lock_guard<std::mutex> lock(console_mutex);
    if (console_ctx) {
        if (console_worker_take_eof (console_ctx)) {
            auto console_type = console_ctx->console_type;
            console_worker_destroy (console_ctx);
            console_ctx = nullptr;
            comm_abstract_remove (STDIN_FILENO); // remove console from comm_abstract
            if (console_type == CONSOLE_TYPE_REAL) {
                // re-arm console worker for next console input (e.g., after Ctrl+D EOF)
                SPDLOG_INFO ("console EOF detected, re-initializing console worker");
                console_ctx = console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);

                // TODO: invoke hook_connect when next console input is received
                return 0;
            }
            else {
                // stdin is either a pipe or a file, so EOF means the end of input; shut down the server
                SPDLOG_INFO ("EOF detected (pipe/file), shutting down server");
                mudmux_shutdown();
                return 0;
            }
        }
        if (!comm_abstract_get(0)) {
            comm_abstract_add (STDIN_FILENO); // re-connect console user if it was removed due to EOF
            mudmux_invoke_hook (MUDMUX_HOOK_CONNECT,
                async_runtime_get_context(runtime), 0, nullptr, 0); // invoke connect hook for console user
        }

        // drain completed lines from the console queue and invoke the shared inbound hook path
        while (async_queue_dequeue (console_queue, console_line_buffer, sizeof(console_line_buffer), nullptr)) {
            comm_invoke_inbound_message(runtime, 0, console_line_buffer, strlen(console_line_buffer));
        }
        return 1;
    }
    return -1; // no console worker
}
