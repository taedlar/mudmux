#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/mudmux.h"
#include "async/async_runtime.h"
#include "comm/abstract.h"
#include "comm/console.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

static std::atomic<bool> is_running{false};
static std::atomic<bool> is_shutting_down{false};

static bool enable_console{false};
static std::vector<std::string> accept_names; // array of names for BIO_set_accept_name()

extern "C" bool mudmux_init (const char* config_yaml) {
    if (is_running.load()) {
        SPDLOG_ERROR ("mudmux_init() called while already running");
        return false;
    }
    try {
        YAML::Node config = YAML::Load (config_yaml ? config_yaml : "{\"transport\":{\"console\":true}}");
        const YAML::Node& transport = config["transport"];
        // initialize transport layer
        enable_console = transport["console"].as<bool>(false);
        if (transport["accept"]) {
            for (const auto& name : transport["accept"]) {
                accept_names.push_back(name.as<std::string>());
            }
        }
        is_shutting_down.store(false);
    }
    catch (const YAML::Exception& e) {
        SPDLOG_ERROR ("configuration error: {}", e.what());
        return false;
    }
    return true;
}

extern "C" void mudmux_deinit (void) {
    if (is_running.load()) {
        SPDLOG_ERROR ("mudmux_deinit() called while running");
        return;
    }
    enable_console = false;
    accept_names.clear();
}

extern "C" int mudmux_run (void* context) {
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
        is_running.store(false);
        return EXIT_FAILURE;
    }

    // main event loop
    SPDLOG_INFO ("===== entering event loop =====");
    io_event_t events[64];
    while (!is_shutting_down.load()) {
        // [BLOCKING] wait for I/O events
        int num_events = async_runtime_wait(
            runtime, events, sizeof(events) / sizeof(events[0]), nullptr);
        if (num_events < 0) {
            SPDLOG_CRITICAL ("async_runtime_wait failed"); // TODO: initiate retry or shutdown
            break;
        }
        SPDLOG_DEBUG ("async_runtime_wait returned {} events", num_events);

        // process console input (always check for console input, even if no events were returned)
        if (enable_console)
            comm_process_console_input (runtime);

        // process I/O events (non-blocking)
        for (int i = 0; i < num_events; ++i) {
#ifndef NDEBUG
            auto& event = events[i];
            SPDLOG_DEBUG ("event: fd={}, event_type={}, bytes_transferred={}",
                event.fd, event.event_type, event.bytes_transferred);
#endif
        }
    }
    SPDLOG_INFO ("===== exited event loop =====");

    // cleanup communications and teardown subsystems
    comm_abstract_cleanup();
    comm_shutdown_console (runtime);
    async_runtime_deinit (runtime);
    is_running.store(false);
    return EXIT_SUCCESS;
}

extern "C" void mudmux_shutdown (void) {
    is_shutting_down.store(true);
}
