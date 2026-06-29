#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "async/async_runtime.h"
#include "comm/console.h"
#include "comm/listen.h"

#include <atomic>
#include <yaml-cpp/yaml.h>

static bool enable_console = false;

extern "C" bool mudmux_init(const char* config_yaml) {
    if (!config_yaml)
        return true; // use defaults

    try {
        YAML::Node config = YAML::Load(config_yaml);
        if (config["transport"]) {
            const YAML::Node& transport = config["transport"];
            if (transport["console"]) {
                enable_console = transport["console"].as<bool>();
            }
        }
    }
    catch (const YAML::ParserException& e) {
        SPDLOG_ERROR ("failed to parse YAML configuration content: {}", e.what());
        return false;
    }
    return true;
}

extern "C" int mudmux_run(void* context) {
    static std::atomic<bool> is_running{false};
    if (is_running.exchange(true)) {
        SPDLOG_ERROR ("mudmux_run() called while already running");
        return EXIT_FAILURE;
    }

    // initialize subsystems
    auto runtime = async_runtime_init(context);
    bool success = runtime
        && (!enable_console || comm_init_console(runtime));
        // && comm_init_listening_port (runtime, 4000, nullptr);
    if (!success) {
        SPDLOG_ERROR ("failed to initialize");
        async_runtime_deinit(runtime);
        return EXIT_FAILURE;
    }

    // main event loop
    SPDLOG_INFO ("entering event loop");
    io_event_t events[64];
    bool will_shutdown = false;
    while (!will_shutdown) {
        // [BLOCKING] wait for I/O events
        int num_events = async_runtime_wait(
            runtime, events, sizeof(events) / sizeof(events[0]), nullptr);
        if (num_events < 0) {
            SPDLOG_CRITICAL ("async_runtime_wait failed"); // TODO: initiate retry or shutdown
            break;
        }
        SPDLOG_DEBUG ("async_runtime_wait returned {} events", num_events);

        // process console input (always check for console input, even if no events were returned)
        comm_process_console_input (runtime, &will_shutdown);

        // process I/O events (non-blocking)
        for (int i = 0; i < num_events; ++i) {
#ifndef NDEBUG
            auto& event = events[i];
            SPDLOG_DEBUG ("event: fd={}, event_type={}, bytes_transferred={}",
                event.fd, event.event_type, event.bytes_transferred);
#endif
        }
    }
    SPDLOG_INFO ("exited event loop");

    comm_shutdown_console (runtime);
    async_runtime_deinit (runtime);
    return EXIT_SUCCESS;
}
