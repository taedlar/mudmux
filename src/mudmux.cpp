#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/mudmux.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "async/async_event.h"
#include "async/console_worker.h"
#include "comm/accept.hpp"
#include "comm/abstract.hpp"
#include "comm/console.hpp"
#include "comm/file_input.hpp"
#include "comm/inbound.hpp"
#include "comm/input_mode.hpp"
#include "mudmux/async.h"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

extern "C" {
    mudmux_async_api_v1_t* mudmux_async_api_v1 {nullptr}; // global pointer to async API struct, initialized by mudmux_init()
    mudmux_comm_api_v1_t* mudmux_comm_api_v1 {nullptr}; // global pointer to comm API struct, initialized by mudmux_init()
}

static std::thread::id mud_logic_thread_id; // thread ID of the logic layer thread (main thread)
static std::atomic<bool> is_running{false};
static std::atomic<bool> is_shutting_down{false};

static bool enable_standard_input{false};
static bool enable_console{false};
static std::vector<std::string> accept_names; // array of names for BIO_set_accept_name()

static bool comm_api_thread_guard(const char* api_name) {
    if (mud_logic_thread_id == std::thread::id())
        return true; // logic thread not bound yet (before mudmux_run)
    if (std::this_thread::get_id() == mud_logic_thread_id)
        return true;

    SPDLOG_CRITICAL("comm API {} called from non-logic thread", api_name);
    return false;
}

template <typename FallbackT, typename Fn, typename... Args>
static FallbackT guarded_call(const char* api_name, FallbackT fallback, Fn&& fn, Args&&... args) {
    if (!comm_api_thread_guard(api_name))
        return fallback;
    return fn(std::forward<Args>(args)...);
}

template <typename Fn, typename... Args>
static void guarded_call_void(const char* api_name, Fn&& fn, Args&&... args) {
    if (!comm_api_thread_guard(api_name))
        return;
    fn(std::forward<Args>(args)...);
}

static void init_async_api (void) {
    static mudmux_async_api_v1_t async_api;
    async_api.event_init = async_event_init;
    async_api.event_destroy = async_event_destroy;
    async_api.event_set = async_event_set;
    async_api.event_reset = async_event_reset;
    // async_api.event_wait = async_event_wait;
    async_api.event_get_wait_handle = async_event_get_wait_handle;

    mudmux_async_api_v1 = &async_api; // set global pointer to initialized struct
}

/**
 * @brief Initialize the mudmux_comm_api_v1 struct with function pointers to the communication API.
 */
static void init_comm_api (void) {
    static mudmux_comm_api_v1_t comm_api;
    comm_api.max_slot = comm_max_slot;
    comm_api.add_bio = +[](BIO* rbio, BIO* wbio, int slot, uint32_t flags) -> int {
        return guarded_call<int>("add_bio", -1, comm_abstract_add_bio, rbio, wbio, slot, flags);
    };
    comm_api.add_file = +[](const char* fn_in, const char* fn_out, int slot, uint32_t flags) -> int {
        return guarded_call<int>("add_file", -1, comm_abstract_add_file, fn_in, fn_out, slot, flags);
    };
    comm_api.get = +[](int slot) -> comm_abstract_t* {
        return guarded_call<comm_abstract_t*>("get", nullptr, comm_abstract_get, slot);
    };
    comm_api.get_flags = +[](comm_abstract_t* comm) -> uint32_t {
        return guarded_call<uint32_t>("get_flags", 0, comm_get_flags, comm);
    };
    comm_api.buffered_write = +[](comm_abstract_t* comm, const void* buf, size_t len) {
        guarded_call_void("buffered_write", comm_buffered_write, comm, buf, len);
    };
    comm_api.close = +[](async_runtime_t* runtime, int slot) -> bool {
        return guarded_call<bool>("close", false, comm_close, runtime, slot);
    };
    comm_api.set_line_input = +[](int slot, bool echo) -> bool {
        return guarded_call<bool>("set_line_input", false, comm_set_line_input, slot, echo);
    };
    comm_api.set_char_input = +[](int slot) -> bool {
        return guarded_call<bool>("set_char_input", false, comm_set_char_input, slot);
    };
    comm_api.set_echo = +[](int slot, bool echo) -> bool {
        return guarded_call<bool>("set_echo", false, comm_set_echo, slot, echo);
    };
    comm_api.enable_prompt = +[](int slot, bool enable) {
        guarded_call_void("enable_prompt", comm_enable_prompt, slot, enable);
    };
    comm_api.enable_virtual_terminal = +[](int slot) -> bool {
        return guarded_call<bool>("enable_virtual_terminal", false, comm_enable_virtual_terminal, slot);
    };

    mudmux_comm_api_v1 = &comm_api; // set global pointer to initialized struct
}

static int context_to_slot (void* context) {
    return static_cast<int>(reinterpret_cast<intptr_t>(context));
}

MUDMUX_EXPORT void mudmux_set_log_level (int level) {
    spdlog::set_level(static_cast<spdlog::level::level_enum>(level));
}

MUDMUX_EXPORT void mudmux_enable_standard_input (bool enable) {
    enable_standard_input = enable;
}

MUDMUX_EXPORT void mudmux_enable_console (bool enable) {
    enable_console = enable;
}

MUDMUX_EXPORT bool mudmux_init (const char* config_yaml) {
    if (is_running.load()) {
        SPDLOG_ERROR ("mudmux_init() called while already running");
        return false;
    }
    init_async_api();
    init_comm_api();
    try {
        YAML::Node config = YAML::Load (config_yaml ? config_yaml : "{\"transport\":{\"console\":false}}");
        const YAML::Node& transport = config["transport"];
        // initialize transport layer
        if (transport["console"].IsDefined()) {
            mudmux_enable_console (transport["console"].as<bool>());
        }
        if (transport["accept"].IsDefined()) {
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

MUDMUX_EXPORT void mudmux_deinit (void) {
    if (is_running.load()) {
        SPDLOG_ERROR ("mudmux_deinit() called while running");
        return;
    }
    enable_console = false;
    mud_logic_thread_id = std::thread::id();
    accept_names.clear();
    memset(mudmux_comm_api_v1, 0, sizeof(mudmux_comm_api_v1_t));
    memset(mudmux_async_api_v1, 0, sizeof(mudmux_async_api_v1_t));
}

MUDMUX_EXPORT int mudmux_run (void* context) {
    if (is_running.exchange(true)) {
        SPDLOG_ERROR ("mudmux_run() called while already running");
        return EXIT_FAILURE;
    }
    mud_logic_thread_id = std::this_thread::get_id(); // store the thread ID of the logic layer thread (main thread)

    // initialize subsystems
    auto runtime = async_runtime_init(context);
    bool success = (runtime != nullptr);

    if (success) {
        if (enable_console || enable_standard_input) { // console input is enabled, initialize console worker
            success = comm_init_console (runtime);
        }
        else { // file input is enabled (before entering event loop), simulate a single console session with file input
            if (comm_abstract_has_rbio(COMM_SLOT_CONSOLE)) {
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
            comm_process_file_input (runtime, COMM_SLOT_CONSOLE, nullptr);
        }

        // Process I/O events from transports (non-blocking)
        for (int i = 0; i < num_events; ++i) {
            auto& event = events[i];

            if (!event.context) {
                // completion events: we always drain completion queue before processing
                // I/O events (e.g., console input, file input, etc.)
                continue;
            }

            int slot = context_to_slot (event.context);
            {
                comm_abstract_ptr comm (slot, comm_slots_mtx);
                if (!comm) {
                    // This can happen if the comm was removed (e.g., due to disconnect) while events were still pending
                    continue;
                }
                SPDLOG_DEBUG ("processing event for slot {} (event.fd={})", slot, event.fd);
                if (comm->flags & C_SOCKET_LISTENING) {
                    comm_process_listener_event (runtime, slot, event.fd);
                    continue;
                }
                if (event.event_type & EVENT_READ) {
#ifdef _WIN32
                    bool refilled = comm_refill_inbound_buffers (slot, static_cast<char*>(event.buffer), event.bytes_transferred)
                        && 0 == async_runtime_post_read (runtime, event.fd, nullptr, 0); // re-arm IOCP for next read
#else
                    bool refilled = comm_refill_inbound_buffers (slot); // read more data from rbio
#endif
                    if (!refilled || comm_process_input(runtime, slot) != 0) {
                        (void) comm_close(runtime, slot);
                        continue;
                    }
                    continue;
                }

                if (event.event_type & (EVENT_CLOSE | EVENT_ERROR)) {
                    (void) comm_close(runtime, slot); // close the connection on error or close event
                    continue;
                }

                if (comm->flags & C_CLOSING) {
                    SPDLOG_DEBUG ("comm slot {} has C_CLOSING flag set, proceeding with graceful close", slot);
                    (void) comm_close(runtime, slot); // proceed pending graceful close if C_CLOSING flag is set
                    continue;
                }
            }
        }
        comm_invoke_prompt(runtime); // invoke prompt hook for all comms with C_ENABLE_PROMPT flag set
    }
    SPDLOG_INFO ("===== exited event loop =====");

    // cleanup communications and teardown subsystems
    comm_shutdown_console (runtime);
    async_runtime_deinit (runtime);
    is_running.store(false);
    mud_logic_thread_id = std::thread::id();
    comm_abstract_remove_all(); // do this after async_runtime_deinit() to avoid accept worker error on invalid socket hanndles
    return EXIT_SUCCESS;
}

MUDMUX_EXPORT void mudmux_shutdown (void) {
    if (is_running.load()) {
        SPDLOG_INFO ("mudmux_shutdown() called");
        is_shutting_down.store(true);
        async_runtime_wakeup(async_get_current_runtime());
    }
}
