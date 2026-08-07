#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "execution.hpp"

#include "mudmux/mudmux.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include "async/async_runtime.h"
#include "async/thread_pool.hpp"
#include "comm/abstract.hpp"
#include "hooks.hpp"
#include "mudmux/async.h"

#include <spdlog/spdlog.h>

namespace {

// Relaxed mode deliberately has no per-slot task queue.  Transport parsing
// leaves the next unit in its input buffer until this single task completes.
struct in_flight_hook_t {
    enum mudmux_hook_type_t hook_type{MAX_HOOK_TYPE};
    void* ctx{nullptr};
    int msg{-1};
    int current_slot{-1};
    uint64_t generation{0};
    std::vector<char> payload;
    mudmux_hook_completion_t completion{nullptr};
    void* completion_context{nullptr};
};

struct slot_execution_state_t {
    std::mutex mutex;
    bool active{false};
    // Only explicit non-inbound/API dispatches use this queue.  Transport
    // parsers never place decoded input here.
    std::deque<in_flight_hook_t> pending;
};

struct detached_completion_t {
    async_closure_t closure{};
    int message{ASYNC_CLOSURE_SCHEDULER_OK};
};

struct detached_work_t {
    async_closure_t work{};
    async_closure_t completion{};
};

constexpr std::size_t auxiliary_queue_capacity = 8;

static slot_execution_state_t& ensure_slot_state_locked(std::deque<slot_execution_state_t>& states, int slot) {
    while (slot >= static_cast<int>(states.size()))
        states.emplace_back();
    return states[static_cast<std::size_t>(slot)];
}

thread_local bool is_execution_worker_thread = false;

struct worker_thread_scope_t {
    worker_thread_scope_t() { is_execution_worker_thread = true; }
    ~worker_thread_scope_t() { is_execution_worker_thread = false; }
};

struct execution_state_t {
    int thread_pool_size{1};
    mudmux_determinism_mode_t determinism_mode{MUDMUX_DETERMINISM_STRICT};
    async_thread_pool_t worker_pool;
    std::atomic<bool> running{false};
    std::mutex slot_states_mutex;
    std::deque<slot_execution_state_t> slot_states;
    std::mutex event_mutex;
    struct event_task_t {
        mudmux_hook_func_t hook_func;
        void* ctx;
        int msg;
    };
    std::deque<event_task_t> pending_events;
    bool event_active{false};
    std::mutex detached_completion_mutex;
    std::deque<detached_completion_t> pending_detached_completions;
    bool detached_completion_active{false};
};

execution_state_t execution_state;

static void run_slot_task(int slot, in_flight_hook_t task);

static void log_closure_exception(const char* phase, const std::exception& exception) noexcept {
    try {
        SPDLOG_ERROR("detached worker {} closure threw an exception: {}", phase, exception.what());
    }
    catch (...) {
    }
}

static void log_closure_exception(const char* phase) noexcept {
    try {
        SPDLOG_ERROR("detached worker {} closure threw a non-standard exception", phase);
    }
    catch (...) {
    }
}

static bool invoke_closure_safely(async_closure_t* closure, int message, const char* phase) noexcept {
    try {
        if (async_closure_invoke(closure, message))
            return true;
        log_closure_exception(phase);
    }
    catch (const std::exception& exception) {
        log_closure_exception(phase, exception);
    }
    catch (...) {
        log_closure_exception(phase);
    }
    return false;
}

static void destroy_closure_safely(async_closure_t* closure, const char* phase) noexcept {
    try {
        if (!async_closure_destroy(closure))
            log_closure_exception(phase);
    }
    catch (const std::exception& exception) {
        log_closure_exception(phase, exception);
    }
    catch (...) {
        log_closure_exception(phase);
    }
}

static void run_detached_completion(detached_completion_t completion) {
    worker_thread_scope_t worker_scope;
    for (;;) {
        (void)invoke_closure_safely(&completion.closure, completion.message, "completion");

        detached_completion_t next{};
        {
            std::lock_guard<std::mutex> lock(execution_state.detached_completion_mutex);
            if (execution_state.pending_detached_completions.empty()) {
                execution_state.detached_completion_active = false;
                return;
            }
            next = std::move(execution_state.pending_detached_completions.front());
            execution_state.pending_detached_completions.pop_front();
        }

        if (execution_state.worker_pool.submit([next = std::move(next)]() mutable {
                run_detached_completion(std::move(next));
            })) {
            return;
        }
        completion = std::move(next);
    }
}

static void schedule_detached_completion(async_closure_t completion, int message) {
    detached_completion_t next{completion, message};
    {
        std::lock_guard<std::mutex> lock(execution_state.detached_completion_mutex);
        if (execution_state.detached_completion_active) {
            execution_state.pending_detached_completions.push_back(std::move(next));
            return;
        }
        execution_state.detached_completion_active = true;
    }

    if (!execution_state.worker_pool.submit([next = std::move(next)]() mutable {
            run_detached_completion(std::move(next));
        })) {
        run_detached_completion(std::move(next));
    }
}

static void run_detached_work(detached_work_t task) {
    worker_thread_scope_t worker_scope;
    const int completion_message = invoke_closure_safely(
        &task.work, ASYNC_CLOSURE_SCHEDULER_OK, "work")
        ? ASYNC_CLOSURE_SCHEDULER_OK
        : ASYNC_CLOSURE_SCHEDULER_FAILED;
    schedule_detached_completion(task.completion, completion_message);
}

static void finish_slot_task(int slot) {
    in_flight_hook_t next_task;
    bool run_next = false;
    {
        std::lock_guard<std::mutex> states_lock(execution_state.slot_states_mutex);
        if (slot >= 0 && slot < static_cast<int>(execution_state.slot_states.size())) {
            slot_execution_state_t& state = execution_state.slot_states[static_cast<std::size_t>(slot)];
            std::lock_guard<std::mutex> slot_lock(state.mutex);
            if (!state.pending.empty()) {
                next_task = std::move(state.pending.front());
                state.pending.pop_front();
                run_next = true;
            } else {
                state.active = false;
            }
        }
    }
    if (run_next && !execution_state.worker_pool.submit([slot, task = std::move(next_task)]() mutable {
            run_slot_task(slot, std::move(task));
        })) {
        std::lock_guard<std::mutex> states_lock(execution_state.slot_states_mutex);
        if (slot >= 0 && slot < static_cast<int>(execution_state.slot_states.size())) {
            slot_execution_state_t& state = execution_state.slot_states[static_cast<std::size_t>(slot)];
            std::lock_guard<std::mutex> slot_lock(state.mutex);
            state.pending.clear();
            state.active = false;
        }
    }
    if (async_runtime_t* runtime = async_get_current_runtime())
        async_runtime_wakeup(runtime);
}

static void run_slot_task(int slot, in_flight_hook_t task) {
    worker_thread_scope_t worker_scope;

    // A slot may have been removed and reused after this task was accepted.
    // Do not invoke a hook or completion against that new connection.
    if (comm_abstract_generation(slot) == task.generation) {
        void* data = task.payload.empty() ? nullptr : task.payload.data();
        (void)mudmux_invoke_registered_hook(
            task.hook_type, task.ctx, task.msg, data, task.payload.size(), false, task.current_slot);
        if (comm_abstract_generation(slot) == task.generation && task.completion)
            task.completion(task.completion_context, task.msg);
    }
    finish_slot_task(slot);
}

} // namespace

void mudmux_workers_configure(int thread_pool_size) {
    execution_state.thread_pool_size = thread_pool_size;
    execution_state.determinism_mode = (thread_pool_size == 1)
        ? MUDMUX_DETERMINISM_STRICT
        : MUDMUX_DETERMINISM_RELAXED;
}

extern "C" MUDMUX_EXPORT bool mudmux_workers_start() {
    if (execution_state.running.exchange(true))
        return false;
    if (!execution_state.worker_pool.start(static_cast<std::size_t>(execution_state.thread_pool_size))) {
        execution_state.running.store(false);
        return false;
    }
    return true;
}

extern "C" MUDMUX_EXPORT void mudmux_workers_stop() {
    execution_state.running.store(false);
    execution_state.worker_pool.stop();
    {
        std::lock_guard<std::mutex> lock(execution_state.slot_states_mutex);
        execution_state.slot_states.clear();
    }
    {
        std::lock_guard<std::mutex> event_lock(execution_state.event_mutex);
        execution_state.pending_events.clear();
        execution_state.event_active = false;
    }

    std::deque<detached_completion_t> pending_completions;
    {
        std::lock_guard<std::mutex> completion_lock(execution_state.detached_completion_mutex);
        pending_completions.swap(execution_state.pending_detached_completions);
        execution_state.detached_completion_active = false;
    }
    while (!pending_completions.empty()) {
        detached_completion_t completion = std::move(pending_completions.front());
        pending_completions.pop_front();
        destroy_closure_safely(&completion.closure, "completion destruction");
    }
}

int mudmux_workers_configured_pool_size() { return execution_state.thread_pool_size; }

extern "C" MUDMUX_EXPORT size_t mudmux_workers_pool_size() {
    return execution_state.worker_pool.size();
}

extern "C" MUDMUX_EXPORT bool mudmux_workers_submit(async_closure_t* work, async_closure_t* completion) {
    if (!async_closure_is_valid(work) || !async_closure_is_valid(completion) || !execution_state.running.load())
        return false;

    detached_work_t task{*work, *completion};
    if (!execution_state.worker_pool.submit([task = std::move(task)]() mutable {
            run_detached_work(std::move(task));
        })) {
        return false;
    }

    work->invoke = 0;
    work->destroy = 0;
    work->context = 0;
    completion->invoke = 0;
    completion->destroy = 0;
    completion->context = 0;
    return true;
}

mudmux_determinism_mode_t mudmux_execution_mode() { return execution_state.determinism_mode; }
const char* mudmux_execution_mode_name() { return execution_state.determinism_mode == MUDMUX_DETERMINISM_STRICT ? "strict" : "relaxed"; }
bool mudmux_workers_is_worker_thread() { return is_execution_worker_thread; }

static void run_event_task(execution_state_t::event_task_t task) {
    worker_thread_scope_t worker_scope;
    (void)mudmux_invoke_hook_function(task.hook_func, task.ctx, task.msg, nullptr, 0, true);

    execution_state_t::event_task_t next{};
    bool run_next = false;
    {
        std::lock_guard<std::mutex> lock(execution_state.event_mutex);
        if (!execution_state.pending_events.empty()) {
            next = execution_state.pending_events.front();
            execution_state.pending_events.pop_front();
            run_next = true;
        } else {
            execution_state.event_active = false;
        }
    }
    if (run_next && !execution_state.worker_pool.submit([next] { run_event_task(next); })) {
        std::lock_guard<std::mutex> lock(execution_state.event_mutex);
        execution_state.pending_events.clear();
        execution_state.event_active = false;
    }
}

bool mudmux_execution_dispatch_event(mudmux_hook_func_t hook_func, void* ctx, int msg) {
    if (!hook_func || !execution_state.running.load())
        return false;
    if (execution_state.determinism_mode == MUDMUX_DETERMINISM_STRICT) {
        (void)mudmux_invoke_hook_function(hook_func, ctx, msg, nullptr, 0, true);
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(execution_state.event_mutex);
        if (execution_state.event_active) {
            execution_state.pending_events.push_back({hook_func, ctx, msg});
            return true;
        }
        execution_state.event_active = true;
    }
    if (execution_state.worker_pool.submit([hook_func, ctx, msg] { run_event_task({hook_func, ctx, msg}); }))
        return true;
    std::lock_guard<std::mutex> lock(execution_state.event_mutex);
    execution_state.event_active = false;
    return false;
}

bool mudmux_execution_slot_busy(int slot) {
    if (execution_state.determinism_mode != MUDMUX_DETERMINISM_RELAXED || slot < 0)
        return false;
    std::lock_guard<std::mutex> states_lock(execution_state.slot_states_mutex);
    if (slot >= static_cast<int>(execution_state.slot_states.size()))
        return false;
    slot_execution_state_t& state = execution_state.slot_states[static_cast<std::size_t>(slot)];
    std::lock_guard<std::mutex> slot_lock(state.mutex);
    return state.active;
}

mudmux_dispatch_result_t mudmux_execution_enqueue_hook(
    enum mudmux_hook_type_t hook_type, void* ctx, int msg, const void* data, size_t size,
    mudmux_hook_completion_t completion, void* completion_context, bool allow_pending, int queue_slot, int current_slot_) {
    const int slot = queue_slot >= 0 ? queue_slot : msg;
    if (!execution_state.running.load() || slot < 0)
        return MUDMUX_DISPATCH_ERROR;

    in_flight_hook_t task;
    task.hook_type = hook_type;
    task.ctx = ctx;
    task.msg = msg;
    task.current_slot = current_slot_;
    task.generation = comm_abstract_generation(slot);
    task.completion = completion;
    task.completion_context = completion_context;
    if (data && size > 0) {
        task.payload.resize(size);
        memcpy(task.payload.data(), data, size);
    }

    {
        std::lock_guard<std::mutex> states_lock(execution_state.slot_states_mutex);
        slot_execution_state_t& state = ensure_slot_state_locked(execution_state.slot_states, slot);
        std::lock_guard<std::mutex> slot_lock(state.mutex);
        if (state.active) {
            if (!allow_pending || state.pending.size() >= auxiliary_queue_capacity)
                return MUDMUX_DISPATCH_QUEUE_FULL;
            state.pending.push_back(std::move(task));
            return MUDMUX_DISPATCH_OK;
        }
        state.active = true;
    }

    if (execution_state.worker_pool.submit([slot, task = std::move(task)]() mutable { run_slot_task(slot, std::move(task)); }))
        return MUDMUX_DISPATCH_OK;

    finish_slot_task(slot);
    return MUDMUX_DISPATCH_ERROR;
}

mudmux_dispatch_result_t mudmux_execution_enqueue_telnet_subneg(void* ctx, int slot, int option, const void* data, size_t size) {
    // Telnet subnegotiation uses the same single in-flight guard.  Its hook
    // message is the Telnet option rather than the communication slot.
    if (!execution_state.running.load() || slot < 0)
        return MUDMUX_DISPATCH_ERROR;
    if (mudmux_execution_slot_busy(slot))
        return MUDMUX_DISPATCH_QUEUE_FULL;

    // The generic dispatcher uses slot as msg, so keep the special Telnet
    // entry point until its hook signature is unified.
    in_flight_hook_t task;
    task.hook_type = HOOK_TELNET_SUBNEG;
    task.ctx = ctx;
    task.msg = option;
    task.current_slot = slot;
    task.generation = comm_abstract_generation(slot);
    if (data && size) {
        task.payload.resize(size);
        memcpy(task.payload.data(), data, size);
    }
    {
        std::lock_guard<std::mutex> states_lock(execution_state.slot_states_mutex);
        slot_execution_state_t& state = ensure_slot_state_locked(execution_state.slot_states, slot);
        std::lock_guard<std::mutex> slot_lock(state.mutex);
        if (state.active)
            return MUDMUX_DISPATCH_QUEUE_FULL;
        state.active = true;
    }
    if (execution_state.worker_pool.submit([slot, task = std::move(task)]() mutable { run_slot_task(slot, std::move(task)); }))
        return MUDMUX_DISPATCH_OK;
    finish_slot_task(slot);
    return MUDMUX_DISPATCH_ERROR;
}

bool mudmux_execution_should_dispatch_async(enum mudmux_hook_type_t hook_type) {
    if (execution_state.determinism_mode != MUDMUX_DETERMINISM_RELAXED)
        return false;
    switch (hook_type) {
    case HOOK_CONNECT:
    case HOOK_DISCONNECT:
    case HOOK_MESSAGE_INBOUND:
    case HOOK_MESSAGE_OUTBOUND:
    case HOOK_PROMPT:
    case HOOK_TELNET_SUBNEG:
        return true;
    case HOOK_TIMER:
    case HOOK_GARBAGE_COLLECTION:
    case MAX_HOOK_TYPE:
    default:
        return false;
    }
}
