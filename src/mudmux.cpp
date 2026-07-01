#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/mudmux.h"
#include "async/async_runtime.h"
#include "comm/abstract.h"
#include "comm/accept.h"
#include "comm/console.h"
#include "comm/inbound.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

static std::atomic<bool> is_running{false};
static std::atomic<bool> is_shutting_down{false};

static bool enable_console{false};
static std::vector<std::string> accept_names; // array of names for BIO_set_accept_name()

static int context_to_slot (void* context) {
    return static_cast<int>(reinterpret_cast<intptr_t>(context));
}

static void disconnect_comm (async_runtime_t* runtime, void* context, int slot, socket_fd_t fd) {
    async_runtime_remove(runtime, fd);
    mudmux_invoke_hook(MUDMUX_HOOK_DISCONNECT, context, slot, nullptr, 0);
    comm_abstract_remove(slot);
}

extern "C" void mudmux_set_log_level (int level) {
    spdlog::set_level(static_cast<spdlog::level::level_enum>(level));
}

extern "C" bool mudmux_init (const char* config_yaml) {
    if (is_running.load()) {
        SPDLOG_ERROR ("mudmux_init() called while already running");
        return false;
    }
    try {
        YAML::Node config = YAML::Load (config_yaml ? config_yaml : "{\"transport\":{\"console\":false}}");
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
    bool success = (runtime != nullptr);

    if (success && enable_console)
        success = comm_init_console (runtime);

    if (success) {
        for (const auto& accept_name : accept_names) {
            if (comm_accept(runtime, accept_name.c_str()) < 0) {
                SPDLOG_ERROR ("failed to initialize accept transport {}", accept_name);
                success = false;
                break;
            }
        }
    }
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
            auto& event = events[i];
#ifndef NDEBUG
            SPDLOG_DEBUG ("event: fd={}, event_type={}, bytes_transferred={}",
                event.fd, event.event_type, event.bytes_transferred);
#endif

            if (!event.context) {
                continue;
            }

            int slot = context_to_slot(event.context);
            auto* comm = comm_abstract_get (slot);
            if (!comm) {
                // This can happen if the comm was removed (e.g., due to disconnect) while events were still pending
                continue;
            }
            SPDLOG_DEBUG ("processing event for slot {} (event.fd={})", slot, event.fd);

            if (comm_is_listener(comm)) {
                comm_process_listener_event (runtime, slot, event.fd);
                continue;
            }

            if (event.event_type & EVENT_READ) {
                if (comm_process_input(runtime, &event, slot) != 0) {
                    disconnect_comm(runtime, async_runtime_get_context(runtime), slot, event.fd);
                }
                continue;
            }

            if (event.event_type & (EVENT_CLOSE | EVENT_ERROR)) {
                disconnect_comm(runtime, async_runtime_get_context(runtime), slot, event.fd);
            }
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
    if (is_running.load()) {
        SPDLOG_INFO ("mudmux_shutdown() called");
        is_shutting_down.store(true);
        async_runtime_wakeup(async_get_current_runtime());
    }
}
