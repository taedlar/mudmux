#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/mudmux.h"

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "async/async_event.h"
#include "async/async_queue.h"
#include "async/console_worker.h"
#include "comm/accept.hpp"
#include "comm/abstract.hpp"
#include "comm/console.hpp"
#include "comm/current_slot.hpp"
#include "comm/file_input.hpp"
#include "comm/inbound.hpp"
#include "comm/input_mode.hpp"
#include "comm/outbound.hpp"
#include "comm/ssl.hpp"
#include "comm/telnet.hpp"
#include "comm/websocket.hpp"
#include "execution.hpp"
#include "mudmux/async.h"
#include "mudmux/comm.h"
#include "mudmux/execution.h"
#include "mudmux/hooks.h"

extern "C" {
    mudmux_async_api_v1_t* mudmux_async_api_v1 {nullptr}; // global pointer to async API struct, initialized by mudmux_init()
    mudmux_comm_api_v1_t* mudmux_comm_api_v1 {nullptr}; // global pointer to comm API struct, initialized by mudmux_init()
    mudmux_execution_api_v1_t* mudmux_execution_api_v1 {nullptr}; // global pointer to execution API struct, initialized by mudmux_init()
}

static std::thread::id mud_logic_thread_id; // thread ID of the logic layer thread (main thread)
static std::atomic<bool> is_running{false};
static std::atomic<bool> is_shutting_down{false};

static bool enable_standard_input{false};
static bool enable_console{false};
static std::vector<std::string> accept_names; // array of names for BIO_set_accept_name()

static std::filesystem::path server_certificate_path; // path to server certificate file (PEM format)
static std::filesystem::path server_private_key_path; // path to server private key file (PEM format)

struct event_registration_t {
    async_event_t* event;
    mudmux_hook_func_t hook_func;
};

static std::mutex event_registrations_mutex;
static std::vector<std::unique_ptr<event_registration_t>> event_registrations;
static async_event_t timer_event;
static bool timer_event_initialized{false};
static bool timer_event_registered{false};
static std::atomic<int> timer_event_msg{-1};
static int keep_alive_interval_seconds{20};

static bool comm_api_thread_guard(const char* api_name) {
    if (mud_logic_thread_id == std::thread::id())
        return true; // logic thread not bound yet (before mudmux_run)
    if (std::this_thread::get_id() == mud_logic_thread_id)
        return true;
    if (mudmux_workers_is_worker_thread())
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
    async_api.event_wait = async_event_wait;
    async_api.event_get_wait_handle = async_event_get_wait_handle;
    async_api.queue_create = async_queue_create;
    async_api.queue_destroy = async_queue_destroy;
    async_api.queue_enqueue = async_queue_enqueue;
    async_api.queue_dequeue = async_queue_dequeue;
    async_api.queue_is_empty = async_queue_is_empty;
    async_api.queue_is_full = async_queue_is_full;
    async_api.queue_clear = async_queue_clear;
    async_api.queue_get_stats = async_queue_get_stats;

    mudmux_async_api_v1 = &async_api; // set global pointer to initialized struct
}

static bool mudmux_is_running(void) {
    return is_running.load(std::memory_order_acquire);
}

static void init_execution_api(void) {
    static mudmux_execution_api_v1_t execution_api;
    execution_api.is_running = mudmux_is_running;
    mudmux_execution_api_v1 = &execution_api;
}

/**
 * @brief Initialize the mudmux_comm_api_v1 struct with function pointers to the communication API.
 */
static void init_comm_api (void) {
    static mudmux_comm_api_v1_t comm_api;
    comm_api.max_slot = comm_max_slot;
    comm_api.current_slot = comm_current_slot;
    comm_api.add_bio = +[](BIO* rbio, BIO* wbio, int slot, uint32_t flags) -> int {
        return guarded_call<int>("add_bio", -1, comm_abstract_add_bio, rbio, wbio, slot, flags);
    };
    comm_api.add_file = +[](const char* fn_in, const char* fn_out, int slot, uint32_t flags) -> int {
        return guarded_call<int>("add_file", -1, comm_abstract_add_file, fn_in, fn_out, slot, flags);
    };
    comm_api.get_flags = +[](int slot) -> uint32_t {
        return guarded_call<uint32_t>("get_flags", 0, comm_get_flags, slot);
    };
    comm_api.buffered_write = +[](int slot, const void* buf, size_t len) {
        guarded_call_void("buffered_write", comm_buffered_write, slot, buf, len);
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
    comm_api.ssl_init = +[](const char* certificate_path, const char* private_key_path) -> bool {
        return guarded_call<bool>("ssl_init", false, comm_ssl_init, certificate_path, private_key_path);
    };
    comm_api.ssl_deinit = +[]() {
        guarded_call_void("ssl_deinit", comm_ssl_deinit);
    };
    comm_api.enable_prompt = +[](int slot, bool enable) {
        guarded_call_void("enable_prompt", comm_enable_prompt, slot, enable);
    };
    comm_api.enable_telnet = +[]() {
        guarded_call_void("enable_telnet", comm_enable_telnet);
    };
    comm_api.enable_websocket = +[](const char* preferred_protocols) -> bool {
        return guarded_call<bool>("enable_websocket", false, comm_enable_websocket, preferred_protocols);
    };
    comm_api.enable_virtual_terminal = +[](int slot) -> bool {
        return guarded_call<bool>("enable_virtual_terminal", false, comm_enable_virtual_terminal, slot);
    };
    comm_api.enable_tls = +[]() {
        guarded_call_void("enable_tls", comm_enable_tls);
    };
    comm_api.add_message = +[](int to_slot, const void* buf, size_t len) {
        guarded_call_void("add_message", comm_add_message, to_slot, buf, len);
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
    mudmux_workers_configure(1);
    keep_alive_interval_seconds = 20;
    timer_event_msg.store(-1, std::memory_order_relaxed);
    init_async_api();
    init_comm_api();
    init_execution_api();
    if (!timer_event_initialized) {
        if (!async_event_init(&timer_event, true, false)) {
            SPDLOG_ERROR("failed to initialize timer event");
            return false;
        }
        timer_event_initialized = true;
    }
    if (!timer_event_registered) {
        std::lock_guard<std::mutex> lock(event_registrations_mutex);
        event_registrations.emplace_back(std::make_unique<event_registration_t>(event_registration_t{&timer_event, nullptr}));
        timer_event_registered = true;
    }
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
        if (transport["thread_pool"].IsDefined() && transport["thread_pool"]["size"].IsDefined()) {
            const int thread_pool_size = transport["thread_pool"]["size"].as<int>();
            if (thread_pool_size < 1) {
                SPDLOG_ERROR ("transport.thread_pool.size must be at least 1");
                return false;
            }
            mudmux_workers_configure(thread_pool_size);
        }
        if (transport["keep_alive_interval"].IsDefined()) {
            const int interval = transport["keep_alive_interval"].as<int>();
            if (interval < 1) {
                SPDLOG_ERROR("transport.keep_alive_interval must be at least 1 second");
                return false;
            }
            keep_alive_interval_seconds = interval;
        }
        if (transport["ssl"].IsDefined()) {
            server_certificate_path = transport["ssl"]["certificate"].as<std::string>();
            server_private_key_path = transport["ssl"]["private_key"].as<std::string>();
            if (!comm_ssl_init(server_certificate_path, server_private_key_path)) {
                SPDLOG_ERROR ("failed to initialize SSL with certificate {} and private key {}", server_certificate_path.string(), server_private_key_path.string());
                return false;
            }
        }
        if (!mudmux_workers_start()) {
            SPDLOG_ERROR("failed to start thread pool with {} workers", mudmux_workers_configured_pool_size());
            return false;
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
    mudmux_workers_stop();
    mudmux_workers_configure(1);
    accept_names.clear();
    {
        std::lock_guard<std::mutex> lock(event_registrations_mutex);
        event_registrations.clear();
    }
    timer_event_registered = false;
    if (timer_event_initialized) {
        async_event_destroy(&timer_event);
        timer_event_initialized = false;
    }
    memset(mudmux_comm_api_v1, 0, sizeof(mudmux_comm_api_v1_t));
    memset(mudmux_async_api_v1, 0, sizeof(mudmux_async_api_v1_t));
    memset(mudmux_execution_api_v1, 0, sizeof(mudmux_execution_api_v1_t));
    comm_ssl_deinit();
}

MUDMUX_EXPORT bool mudmux_register_event(async_event_t* event, mudmux_hook_func_t hook_func) {
    if (!event || !hook_func || is_running.load()) {
        SPDLOG_ERROR("mudmux_register_event() requires an initialized event and must be called before mudmux_run()");
        return false;
    }
    std::lock_guard<std::mutex> lock(event_registrations_mutex);
    for (const auto& registration : event_registrations) {
        if (registration->event == event) {
            SPDLOG_ERROR("mudmux_register_event() called more than once for the same event");
            return false;
        }
    }
    event_registrations.emplace_back(std::make_unique<event_registration_t>(event_registration_t{event, hook_func}));
    return true;
}

MUDMUX_EXPORT async_event_t* mudmux_get_timer_event(void) {
    return timer_event_initialized ? &timer_event : nullptr;
}

MUDMUX_EXPORT bool mudmux_trigger_timer(int msg) {
    if (!timer_event_initialized)
        return false;
    if (msg == 0 || msg == -1) {
        SPDLOG_ERROR("mudmux_trigger_timer() message {} is reserved for timer lifecycle notifications", msg);
        return false;
    }
    timer_event_msg.store(msg, std::memory_order_release);
    async_event_set(&timer_event);
    return true;
}

static event_registration_t* find_event_registration(void* context) {
    std::lock_guard<std::mutex> lock(event_registrations_mutex);
    for (const auto& registration : event_registrations) {
        if (registration.get() == context)
            return registration.get();
    }
    return nullptr;
}

static bool register_runtime_events(async_runtime_t* runtime) {
    std::lock_guard<std::mutex> lock(event_registrations_mutex);
    for (const auto& registration : event_registrations) {
        if (async_runtime_add_event(runtime, registration->event, registration.get()) < 0) {
            SPDLOG_ERROR("failed to register an async event with the runtime");
            return false;
        }
    }
    return true;
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

    if (success)
        success = register_runtime_events(runtime);

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
        comm_shutdown_async_file_input();
        comm_shutdown_console(runtime);
        async_runtime_deinit(runtime);
        is_running.store(false);
        return EXIT_FAILURE;
    }

    SPDLOG_INFO (
        "thread pool execution configured: size={} mode={}",
        mudmux_workers_pool_size(),
        mudmux_execution_mode_name());
    if (mudmux_execution_mode() == MUDMUX_DETERMINISM_RELAXED) {
        SPDLOG_WARN ("relaxed mode enabled: inbound data will be processed concurrently");
    }

    // Timer lifecycle notifications are synchronous: logic sees the running
    // state before I/O dispatch begins, regardless of execution-pool mode.
    mudmux_invoke_registered_hook(
        HOOK_TIMER, async_runtime_get_context(runtime), 0, nullptr, 0, false);

    // main event loop
    SPDLOG_INFO ("===== entering event loop =====");
    io_event_t events[64];
    while (!is_shutting_down.load()) {
        // [BLOCKING] wait for I/O events, but periodically wake for registered GC.
        timeval keep_alive_timeout{};
        timeval* timeout = nullptr;
        if (mudmux_get_registered_hook(HOOK_GARBAGE_COLLECTION)) {
            keep_alive_timeout.tv_sec = keep_alive_interval_seconds;
            timeout = &keep_alive_timeout;
        }
        int num_events = async_runtime_wait(
            runtime, events, sizeof(events) / sizeof(events[0]), timeout);
        if (num_events < 0) {
            SPDLOG_CRITICAL ("async_runtime_wait failed"); // TODO: initiate retry or shutdown
            break;
        }
        SPDLOG_TRACE ("async_runtime_wait returned {} events", num_events);

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

            if (event_registration_t* registration = find_event_registration(event.context)) {
                // Manual-reset events are acknowledged before dispatch so a
                // signal delivered while the hook runs remains pending.
                if (async_event_is_manual_reset(registration->event))
                    async_event_reset(registration->event);
                mudmux_hook_func_t hook_func = registration->hook_func
                    ? registration->hook_func
                    : mudmux_get_registered_hook(HOOK_TIMER);
                const int msg = registration->event == &timer_event
                    ? timer_event_msg.exchange(-1, std::memory_order_acq_rel)
                    : -1;
                if (hook_func && !mudmux_execution_dispatch_event(hook_func, async_runtime_get_context(runtime), msg))
                    SPDLOG_WARN("failed to dispatch async event hook");
                continue;
            }

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
                SPDLOG_TRACE ("processing event for slot {} (event.fd={}, type=0x{:x})", slot, event.fd, event.event_type);
                if (comm->flags & C_SOCKET_LISTENING) {
                    comm_process_listener_event (runtime, comm, event.fd);
                    continue;
                }

                if (event.event_type & EVENT_READ) {
#ifndef _WIN32
                    // In relaxed mode HOOK_CONNECT configures the accepted
                    // slot asynchronously.  Do not consume socket bytes
                    // before it can enable TLS: otherwise a ClientHello is
                    // buffered as plaintext and can never reach OpenSSL.
                    // epoll/poll are level-triggered, so the read event is
                    // delivered again after the hook completion wakes us.
                    if ((comm->flags & C_AWAITING_HOOK) || mudmux_execution_slot_busy(slot))
                        continue;
#endif
#ifdef _WIN32
                    bool refilled = comm_refill_inbound_buffers (comm, static_cast<char*>(event.buffer), event.bytes_transferred)
                        && 0 == async_runtime_post_read (runtime, event.fd, nullptr, 0); // re-arm IOCP for next read
#else
                    bool refilled = comm_refill_inbound_buffers (comm); // read more data from rbio
#endif
                    const comm_process_result_t process_result = comm_process_input(runtime, comm);
                    // Input processing may synchronously close the slot (for
                    // example, a rejected WebSocket upgrade).  The guard then
                    // no longer resolves to a live communication object.
                    if (!comm)
                        continue;
                    if (!refilled || process_result == COMM_PROCESS_ERROR || process_result == COMM_PROCESS_CLOSED) {
                        const bool closed = comm_close(runtime, slot);
                        // EOF can arrive with EVENT_READ|EVENT_CLOSE.  In that
                        // case the peer cannot send the Close reply we were
                        // waiting for, so finish the transport teardown here.
                        if (!closed && !refilled && (comm->flags & C_CLOSING) &&
                            C_WEBSOCKET_IS_READY(comm->flags)) {
                            C_WEBSOCKET_SET_STATE(comm->flags, WS_CLOSE_RECEIVED);
                            (void) comm_close(runtime, slot);
                        }
                        continue;
                    }
                }

                if (event.event_type & EVENT_WRITE) {
                    comm_flush(runtime, slot);
                }

                if (event.event_type & (EVENT_CLOSE | EVENT_ERROR)) {
                    // A transport-level close/error means there can be no
                    // WebSocket Close reply.  Complete any pending close
                    // handshake now instead of retaining the slot and
                    // repeatedly receiving the terminal event.
                    const bool closed = comm_close(runtime, slot);
                    if (!closed && (comm->flags & C_CLOSING) &&
                        C_WEBSOCKET_IS_READY(comm->flags)) {
                        C_WEBSOCKET_SET_STATE(comm->flags, WS_CLOSE_RECEIVED);
                        (void) comm_close(runtime, slot);
                    }
                    continue;
                }

                if (comm->flags & C_CLOSING) {
                    SPDLOG_DEBUG ("comm slot {} has C_CLOSING flag set, proceeding with graceful close", slot);
                    (void) comm_close(runtime, slot); // proceed pending graceful close if C_CLOSING flag is set
                    continue;
                }
            }
        }
        if (comm_has_deferred_input())
            comm_resume_deferred_input(runtime);
        comm_invoke_prompt(runtime); // invoke prompt hook for all comms with C_ENABLE_PROMPT flag set

        // invoke garbage collection hook before continue to next iteration of event loop
        // typically used to implement mark-and-sweep garbage collection for scripting languages like Lua, Python, etc.
        mudmux_invoke_registered_hook(
            HOOK_GARBAGE_COLLECTION, async_runtime_get_context(runtime), -1, nullptr, 0, false);

        comm_flush_all(runtime); // advance buffered writes and TLS state in non-blocking mode
    }
    SPDLOG_INFO ("===== exited event loop =====");

    // Notify the logic layer while the runtime and workers are still
    // available, but after the event loop has stopped dispatching I/O.
    mudmux_invoke_registered_hook(
        HOOK_TIMER, async_runtime_get_context(runtime), -1, nullptr, 0, false);

    // cleanup communications and teardown subsystems
    comm_shutdown_async_file_input();
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
