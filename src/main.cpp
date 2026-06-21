// main.cpp
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "async_queue.h"
#include "async_runtime.h"
#include "console_worker.h"

#include <iostream>
#include <spdlog/spdlog.h>

int main() {

    // initialize async runtime
    auto runtime = async_runtime_init();

    // initialize console worker
    auto console_queue = async_queue_create (100, 4096, ASYNC_QUEUE_DROP_OLDEST);
    if (!console_queue) {
        debug_error ("Failed to create console queue");
        return EXIT_FAILURE;
    }
    auto console_ctx =console_worker_init (runtime, console_queue, CONSOLE_COMPLETION_KEY);

    // main event loop
    debug_info ("mudmux starting event loop");
    io_event_t events[64];
    bool will_shutdown = false;
    while (true) {
        // [BLOCKING] wait for I/O events
        int num_events = async_runtime_wait(runtime, events, 64, nullptr);
        if (num_events < 0) {
            debug_error ("async_runtime_wait failed");
            break;
        }
        debug_info ("async_runtime_wait returned {} events", num_events);

        // process events
        for (int i = 0; i < num_events; ++i) {
            auto& event = events[i];
            debug_info ("event: fd={}, event_type={}, bytes_transferred={}", event.fd, event.event_type, event.bytes_transferred);
            if (console_worker_take_eof (console_ctx))
                will_shutdown = true;
        }

        // event loop exit condition (e.g. EOF on console input)
        if (will_shutdown)
            break;
    }

    // shutdown console worker (gracefully)
    debug_info ("shutting down console worker");
    bool stopped = console_worker_shutdown(console_ctx, 5000);
    if (!stopped) {
        console_worker_destroy(console_ctx);
        async_queue_destroy(console_queue);
    }

    debug_info ("shutting down mudmux");
    async_runtime_deinit (runtime);
    return EXIT_SUCCESS;
}
