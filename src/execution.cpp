#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "execution.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "async/async_runtime.h"
#include "async/thread_pool.hpp"
#include "hooks.hpp"

namespace {

constexpr std::size_t slot_queue_capacity = 8;

struct queued_hook_task_t {
    enum mudmux_hook_type_t hook_type{MAX_HOOK_TYPE};
    void* ctx{nullptr};
    int msg{-1};
    std::vector<char> payload;

    void clear() {
        hook_type = MAX_HOOK_TYPE;
        ctx = nullptr;
        msg = -1;
        payload.clear();
    }
};

struct pending_telnet_subneg_t {
    bool valid{false};
    void* ctx{nullptr};
    int slot{-1};
    int option{-1};
    std::vector<char> payload;
};

struct slot_queue_state_t {
    std::mutex mutex;
    std::array<queued_hook_task_t, slot_queue_capacity> tasks;
    std::size_t head{0};
    std::size_t tail{0};
    std::size_t count{0};
    bool active{false};
};

static slot_queue_state_t& ensure_slot_queue_locked(std::deque<slot_queue_state_t>& slot_queues, int slot) {
    while (slot >= static_cast<int>(slot_queues.size()))
        slot_queues.emplace_back();
    return slot_queues[static_cast<std::size_t>(slot)];
}

thread_local bool is_execution_worker_thread = false;

struct worker_thread_scope_t {
    worker_thread_scope_t() {
        is_execution_worker_thread = true;
    }

    ~worker_thread_scope_t() {
        is_execution_worker_thread = false;
    }
};

struct execution_state_t {
    int thread_pool_size{1};
    mudmux_determinism_mode_t determinism_mode{MUDMUX_DETERMINISM_STRICT};
    async_thread_pool_t worker_pool;
    std::atomic<bool> running{false};
    std::mutex slot_queues_mutex;
    std::deque<slot_queue_state_t> slot_queues;
    std::mutex pending_telnet_subneg_mutex;
    std::deque<pending_telnet_subneg_t> pending_telnet_subnegs;
};

execution_state_t execution_state;

static void drain_slot_queue(int slot) {
    for (;;) {
        queued_hook_task_t task;
        {
            std::lock_guard<std::mutex> state_lock(execution_state.slot_queues_mutex);
            if (slot < 0 || slot >= static_cast<int>(execution_state.slot_queues.size()))
                return;

            slot_queue_state_t& slot_state = execution_state.slot_queues[static_cast<std::size_t>(slot)];
            std::lock_guard<std::mutex> slot_lock(slot_state.mutex);
            if (slot_state.count == 0) {
                slot_state.active = false;
                break;
            }

            task = std::move(slot_state.tasks[slot_state.head]);
            slot_state.tasks[slot_state.head].clear();
            slot_state.head = (slot_state.head + 1) % slot_queue_capacity;
            slot_state.count--;
        }

        void* hook_data = task.payload.empty() ? nullptr : task.payload.data();
        (void)mudmux_invoke_registered_hook(task.hook_type, task.ctx, task.msg, hook_data, task.payload.size(), false);
    }

    async_runtime_t* runtime = async_get_current_runtime();
    if (runtime)
        async_runtime_wakeup(runtime);
}

} // namespace

void mudmux_execution_configure(int thread_pool_size) {
    execution_state.thread_pool_size = thread_pool_size;
    execution_state.determinism_mode = (thread_pool_size == 1)
        ? MUDMUX_DETERMINISM_STRICT
        : MUDMUX_DETERMINISM_RELAXED;
}

bool mudmux_execution_start() {
    if (execution_state.running.exchange(true))
        return false;

    if (!execution_state.worker_pool.start(static_cast<std::size_t>(execution_state.thread_pool_size))) {
        execution_state.running.store(false);
        return false;
    }
    return true;
}

void mudmux_execution_stop() {
    // Prevent new enqueue attempts before tearing down the pool/queues.
    execution_state.running.store(false);

    execution_state.worker_pool.stop();
    {
        std::lock_guard<std::mutex> lock(execution_state.slot_queues_mutex);
        execution_state.slot_queues.clear();
    }
    {
        std::lock_guard<std::mutex> lock(execution_state.pending_telnet_subneg_mutex);
        execution_state.pending_telnet_subnegs.clear();
    }
}

int mudmux_execution_thread_pool_size() {
    return execution_state.thread_pool_size;
}

mudmux_determinism_mode_t mudmux_execution_mode() {
    return execution_state.determinism_mode;
}

const char* mudmux_execution_mode_name() {
    return execution_state.determinism_mode == MUDMUX_DETERMINISM_STRICT ? "strict" : "relaxed";
}

bool mudmux_execution_is_worker_thread() {
    return is_execution_worker_thread;
}

mudmux_dispatch_result_t mudmux_execution_enqueue_hook(enum mudmux_hook_type_t hook_type, void* ctx, int slot, const void* data, size_t size) {
    if (!execution_state.running.load() || slot < 0)
        return MUDMUX_DISPATCH_ERROR;

    queued_hook_task_t task;
    task.hook_type = hook_type;
    task.ctx = ctx;
    task.msg = slot;
    if (data && size > 0) {
        task.payload.resize(size);
        memcpy(task.payload.data(), data, size);
    }

    bool schedule_worker = false;
    {
        std::lock_guard<std::mutex> state_lock(execution_state.slot_queues_mutex);
        slot_queue_state_t& slot_state = ensure_slot_queue_locked(execution_state.slot_queues, slot);
        std::lock_guard<std::mutex> slot_lock(slot_state.mutex);
        if (slot_state.count >= slot_queue_capacity) {
            SPDLOG_ERROR("slot {} execution queue is full (capacity={})", slot, slot_queue_capacity);
            return MUDMUX_DISPATCH_QUEUE_FULL;
        }

        slot_state.tasks[slot_state.tail] = std::move(task);
        slot_state.tail = (slot_state.tail + 1) % slot_queue_capacity;
        slot_state.count++;
        if (!slot_state.active) {
            slot_state.active = true;
            schedule_worker = true;
        }
    }

    if (!schedule_worker)
        return MUDMUX_DISPATCH_OK;

    if (execution_state.worker_pool.submit([slot]() {
        worker_thread_scope_t worker_scope;
        drain_slot_queue(slot);
    })) {
        return MUDMUX_DISPATCH_OK;
    }

    std::lock_guard<std::mutex> state_lock(execution_state.slot_queues_mutex);
    if (slot >= 0 && slot < static_cast<int>(execution_state.slot_queues.size())) {
        slot_queue_state_t& slot_state = execution_state.slot_queues[static_cast<std::size_t>(slot)];
        std::lock_guard<std::mutex> slot_lock(slot_state.mutex);
        // Drop queued work and clear active flag to avoid wedging the slot if submit fails (e.g., during shutdown).
        slot_state.head = 0;
        slot_state.tail = 0;
        slot_state.count = 0;
        slot_state.active = false;
    }
    return MUDMUX_DISPATCH_ERROR;
}

mudmux_dispatch_result_t mudmux_execution_enqueue_telnet_subneg(void* ctx, int slot, int option, const void* data, size_t size) {
    if (!execution_state.running.load() || slot < 0)
        return MUDMUX_DISPATCH_ERROR;

    queued_hook_task_t task;
    task.hook_type = HOOK_TELNET_SUBNEG;
    task.ctx = ctx;
    task.msg = option;
    if (data && size > 0) {
        task.payload.resize(size);
        memcpy(task.payload.data(), data, size);
    }

    bool schedule_worker = false;
    {
        std::lock_guard<std::mutex> state_lock(execution_state.slot_queues_mutex);
        slot_queue_state_t& slot_state = ensure_slot_queue_locked(execution_state.slot_queues, slot);
        std::lock_guard<std::mutex> slot_lock(slot_state.mutex);
        if (slot_state.count >= slot_queue_capacity) {
            std::lock_guard<std::mutex> pending_lock(execution_state.pending_telnet_subneg_mutex);
            if (static_cast<std::size_t>(slot) >= execution_state.pending_telnet_subnegs.size())
                execution_state.pending_telnet_subnegs.resize(static_cast<std::size_t>(slot) + 1);

            pending_telnet_subneg_t& pending = execution_state.pending_telnet_subnegs[static_cast<std::size_t>(slot)];
            if (!pending.valid) {
                pending.valid = true;
                pending.ctx = ctx;
                pending.slot = slot;
                pending.option = option;
                pending.payload = std::move(task.payload);
            }
            else {
                SPDLOG_WARN("telnet subneg pending queue already occupied for slot {}", slot);
            }
            return MUDMUX_DISPATCH_QUEUE_FULL;
        }

        slot_state.tasks[slot_state.tail] = std::move(task);
        slot_state.tail = (slot_state.tail + 1) % slot_queue_capacity;
        slot_state.count++;
        if (!slot_state.active) {
            slot_state.active = true;
            schedule_worker = true;
        }
    }

    if (!schedule_worker)
        return MUDMUX_DISPATCH_OK;

    if (execution_state.worker_pool.submit([slot]() {
        worker_thread_scope_t worker_scope;
        drain_slot_queue(slot);
    })) {
        return MUDMUX_DISPATCH_OK;
    }

    std::lock_guard<std::mutex> state_lock(execution_state.slot_queues_mutex);
    if (slot >= 0 && slot < static_cast<int>(execution_state.slot_queues.size())) {
        slot_queue_state_t& slot_state = execution_state.slot_queues[static_cast<std::size_t>(slot)];
        std::lock_guard<std::mutex> slot_lock(slot_state.mutex);
        slot_state.head = 0;
        slot_state.tail = 0;
        slot_state.count = 0;
        slot_state.active = false;
    }
    return MUDMUX_DISPATCH_ERROR;
}

bool mudmux_execution_has_pending_telnet_subneg(int slot) {
    std::lock_guard<std::mutex> pending_lock(execution_state.pending_telnet_subneg_mutex);
    return slot >= 0 && static_cast<std::size_t>(slot) < execution_state.pending_telnet_subnegs.size()
        && execution_state.pending_telnet_subnegs[static_cast<std::size_t>(slot)].valid;
}

bool mudmux_execution_retry_pending_telnet_subneg(int slot) {
    pending_telnet_subneg_t pending;
    {
        std::lock_guard<std::mutex> pending_lock(execution_state.pending_telnet_subneg_mutex);
        if (slot < 0 || static_cast<std::size_t>(slot) >= execution_state.pending_telnet_subnegs.size())
            return true;

        pending = execution_state.pending_telnet_subnegs[static_cast<std::size_t>(slot)];
    }

    if (!pending.valid)
        return true;

    const mudmux_dispatch_result_t dispatch_result = mudmux_execution_enqueue_telnet_subneg(
        pending.ctx,
        pending.slot,
        pending.option,
        pending.payload.empty() ? nullptr : pending.payload.data(),
        pending.payload.size());

    if (dispatch_result == MUDMUX_DISPATCH_OK) {
        std::lock_guard<std::mutex> pending_lock(execution_state.pending_telnet_subneg_mutex);
        pending_telnet_subneg_t& stored = execution_state.pending_telnet_subnegs[static_cast<std::size_t>(slot)];
        if (stored.valid && stored.option == pending.option && stored.slot == pending.slot)
            stored.valid = false;
        stored.payload.clear();
        return true;
    }

    return dispatch_result != MUDMUX_DISPATCH_QUEUE_FULL;
}

bool mudmux_execution_should_dispatch_async(enum mudmux_hook_type_t hook_type) {
    if (execution_state.determinism_mode != MUDMUX_DETERMINISM_RELAXED)
        return false;

    switch (hook_type) {
    case HOOK_CONNECT:
    case HOOK_DISCONNECT:
    case HOOK_MESSAGE_INBOUND:
    case HOOK_PROMPT:
        return true;
    case HOOK_MESSAGE_OUTBOUND:
    case MAX_HOOK_TYPE:
    default:
        return false;
    }
}