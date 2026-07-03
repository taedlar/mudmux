#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/mudmux.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "async/console_worker.h"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

extern "C" {
    mudmux_comm_api_t* mudmux_comm_api {nullptr}; // global pointer to comm API struct, initialized by mudmux_init()
}

static std::atomic<bool> is_running{false};
static std::atomic<bool> is_shutting_down{false};

static bool enable_standard_input{false};
static bool enable_console{false};
static std::vector<std::string> accept_names; // array of names for BIO_set_accept_name()

/**
 * @brief Initialize the mudmux_comm_api struct with function pointers to the communication API.
 */
static void init_comm_api (void) {
    static mudmux_comm_api_t comm_api;
    comm_api.max_slot = comm_max_slot;
    comm_api.add_bio = comm_abstract_add_bio;
    comm_api.add_file = comm_abstract_add_file;
    comm_api.get = comm_abstract_get;
    comm_api.remove = comm_abstract_remove;
    comm_api.cleanup = comm_abstract_cleanup;
    comm_api.get_flags = comm_get_flags;
    comm_api.set_flags = comm_set_flags;
    comm_api.clear_flags = comm_clear_flags;
    comm_api.buffered_write = comm_buffered_write;
    comm_api.close = comm_close;

    mudmux_comm_api = &comm_api; // set global pointer to initialized struct
}

static int context_to_slot (void* context) {
    return static_cast<int>(reinterpret_cast<intptr_t>(context));
}

extern "C" void mudmux_set_log_level (int level) {
    spdlog::set_level(static_cast<spdlog::level::level_enum>(level));
}

extern "C" void mudmux_enable_standard_input (bool enable) {
    enable_standard_input = enable;
}

extern "C" void mudmux_enable_console (bool enable) {
    enable_console = enable;
}

extern "C" bool mudmux_init (const char* config_yaml) {
    if (is_running.load()) {
        SPDLOG_ERROR ("mudmux_init() called while already running");
        return false;
    }
    init_comm_api();
    try {
        YAML::Node config = YAML::Load (config_yaml ? config_yaml : "{\"transport\":{\"console\":false}}");
        const YAML::Node& transport = config["transport"];
        // initialize transport layer
        mudmux_enable_console (transport["console"].as<bool>(false));
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
    memset(mudmux_comm_api, 0, sizeof(mudmux_comm_api_t));
}

extern "C" int mudmux_run (void* context) {
    if (is_running.exchange(true)) {
        SPDLOG_ERROR ("mudmux_run() called while already running");
        return EXIT_FAILURE;
    }

    // initialize subsystems
    auto runtime = async_runtime_init(context);
    bool success = (runtime != nullptr);

    if (success) {
        if (enable_console || enable_standard_input) { // console input is enabled, initialize console worker
            success = comm_init_console (runtime);
        }
        else { // file input is enabled (before entering event loop), simulate a single console session with file input
            BIO* rbio = comm_abstract_get_rbio (COMM_SLOT_CONSOLE);
            if (rbio) {
                SPDLOG_DEBUG("detected existing file input, initializing async file input processing");
                // Initialize file input (starts reader thread)
                if (!comm_init_async_file_input(runtime, COMM_SLOT_CONSOLE)) {
                    SPDLOG_ERROR("failed to initialize async file input");
                    success = false;
                }
            }
        }
    }

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

        // Process console input before event loop (needed for mode switching and ReadConsoleW limitations)
        if (enable_console || enable_standard_input)
            comm_process_console_input (runtime, enable_console);

        // Process file input before event loop (async file reader thread posts completions,
        // but needs to be drained even if no events were returned by async_runtime_wait)
        if (comm_has_file_inputs()) {
            comm_process_file_input(runtime, COMM_SLOT_CONSOLE, nullptr);
        }

        // Process I/O events from transports (non-blocking)
        for (int i = 0; i < num_events; ++i) {
            auto& event = events[i];
            
            // Skip file input and console completion events (already processed above)
            if (IS_FILE_INPUT_COMPLETION_KEY(event.fd) || event.fd == CONSOLE_COMPLETION_KEY)
                continue;
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

            if (comm_get_flags(comm) & C_SOCKET_LISTENING) {
                comm_process_listener_event (runtime, slot, event.fd);
                continue;
            }

            if (event.event_type & EVENT_READ) {
                if (comm_process_input(runtime, &event, slot) != 0) {
                    (void) comm_close(runtime, slot);
                    continue;
                }
                // TODO: dispatch inbound message to logic layer with fair command turns
                continue;
            }

            if (event.event_type & (EVENT_CLOSE | EVENT_ERROR)) {
                (void) comm_close(runtime, slot); // close the connection on error or close event
                continue;
            }

            if (comm_get_flags(comm) & C_SOCKET_CLOSING) {
                SPDLOG_DEBUG ("comm slot {} has C_SOCKET_CLOSING flag set, proceeding with graceful close", slot);
                (void) comm_close(runtime, slot); // proceed pending graceful close if C_SOCKET_CLOSING flag is set
                continue;
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
