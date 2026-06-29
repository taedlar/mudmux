#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "console.h"
#include "async/console_worker.h"

static async_queue_t* console_queue = nullptr;
static console_worker_context_t* console_ctx = nullptr;

extern "C" bool comm_init_console (async_runtime_t *runtime) {

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
        return false;
    }

    return true;
}

extern "C" void comm_shutdown_console (async_runtime_t *runtime) {
    bool stopped = console_worker_shutdown(console_ctx, 5000);
    (void)runtime; // unused parameter
    if (!stopped) {
        if (console_ctx) {
            console_worker_destroy (console_ctx);
            console_ctx = nullptr;
        }
        if (console_queue) {
            async_queue_destroy (console_queue);
            console_queue = nullptr;
        }
    }
}

extern "C" void comm_process_console_input (async_runtime_t *runtime, bool *eof_received) {
    if (console_ctx) {
        if (console_worker_take_eof (console_ctx)) {
            if (console_ctx->console_type == CONSOLE_TYPE_REAL) {
                // re-arm console worker for next console input (e.g., after Ctrl+D EOF)
                SPDLOG_INFO ("console EOF detected, re-initializing console worker");
                console_worker_destroy(console_ctx);
                console_ctx = console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);
            }
            else {
                SPDLOG_INFO ("console EOF detected");
                if (eof_received) {
                    *eof_received = true;
                }
            }
        }
    }
}
