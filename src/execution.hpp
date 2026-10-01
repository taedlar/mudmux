#ifndef MUDMUX_EXECUTION_HPP
#define MUDMUX_EXECUTION_HPP

#include <cstddef>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "hooks.hpp"
#include "mudmux/hooks.h"
#include "mudmux/workers.h"

enum mudmux_determinism_mode_t {
    MUDMUX_DETERMINISM_STRICT = 0,
    MUDMUX_DETERMINISM_RELAXED = 1,
};

/** Admit at most one execution per event registration, without a pending FIFO. */
constexpr unsigned int MUDMUX_EXECUTION_TIMELY_EVENT = 1u << 0;

constexpr bool mudmux_dispatch_accepted(mudmux_dispatch_result_t result) noexcept {
    return result == MUDMUX_DISPATCH_OK || result == MUDMUX_DISPATCH_QUEUED;
}

/**
 * Value context for one registered hook or direct event hook.
 *
 * Payload bytes are copied at construction; ctx and completion_context are
 * borrowed and must outlive execution. Payload is a byte buffer, not storage
 * for arbitrary C++ objects. Registered hooks are resolved when invoked.
 *
 * Copyable so it can be captured by the worker pool's std::function tasks.
 * Copies own independent payloads but share borrowed contexts. The scheduler
 * must invoke only the accepted task and call its completion at most once.
 * Destruction only releases storage; it never invokes user callbacks.
 *
 * Queue ownership, slot generation checks, cancellation, and await handling
 * belong to the scheduler. In particular, recheck the slot generation between
 * invoke() and complete(), since the hook may close or replace its slot.
 * Configure completion and current-slot settings before dispatch. Instances
 * require exclusive access while configuring or invoking their mutable payload.
 */
class mudmux_execution {
    template<typename T>
    using payload_element_t = std::remove_pointer_t<decltype(std::data(std::declval<const T&>()))>;

    // Accept contiguous byte containers/views, but never copy an arbitrary
    // object's representation (for example, std::string's internal pointers).
    template<typename T>
    using enable_payload_t = std::enable_if_t<
        sizeof(payload_element_t<T>) == 1 &&
        std::is_trivially_copyable_v<payload_element_t<T>> &&
        std::is_convertible_v<decltype(std::data(std::declval<const T&>())), const void*> &&
        std::is_convertible_v<decltype(std::size(std::declval<const T&>())), std::size_t>, int>;

public:
    explicit mudmux_execution(
        mudmux_hook_type_t hook_type, void* ctx = nullptr, int msg = -1,
        std::size_t size = 0, const void* data = nullptr)
        : target_(hook_type), ctx_(ctx), msg_(msg) {
        if (hook_type <= 0 || hook_type >= MAX_PUBLIC_HOOKS)
            throw std::invalid_argument("invalid registered hook type");
        copy_payload(data, size);
    }

    /**
     * Copy a contiguous byte payload, such as std::string, std::string_view,
     * std::vector<char>, or std::array<std::byte, N>. Strings copy size() bytes,
     * including embedded NULs but excluding the terminating NUL. Views and
     * temporaries are safe because the bytes are copied before returning.
     * Built-in arrays copy all elements, including a string literal's NUL.
     */
    template<typename T, enable_payload_t<T> = 0>
    mudmux_execution(
        mudmux_hook_type_t hook_type, void* ctx, int msg, const T& data)
        : mudmux_execution(hook_type, ctx, msg, std::size(data), std::data(data)) {}

    template<typename T, enable_payload_t<T> = 0>
    mudmux_execution(
        mudmux_hook_func_t func, void* ctx, int msg, const T& data)
        : mudmux_execution(func, ctx, msg, std::size(data), std::data(data)) {}

    explicit mudmux_execution(
        mudmux_hook_func_t func, void* ctx = nullptr, int msg = -1,
        std::size_t size = 0, const void* data = nullptr)
        : target_(func), ctx_(ctx), msg_(msg) {
        if (!func)
            throw std::invalid_argument("null event hook");
        copy_payload(data, size);
    }

    /** Set or clear the completion callback and its borrowed context. */
    void set_completion(mudmux_hook_completion_t completion, void* context = nullptr) noexcept {
        completion_ = completion;
        completion_context_ = context;
    }

    /** Set the callback's current-slot scope; -1 means no specific slot. */
    void set_current_slot(int slot) noexcept {
        current_slot_ = slot;
    }

    /**
     * Borrow a stable registration identity, required for timely events.
     * Distinct registrations may share the same callback and context.
     * The identity must remain valid until execution and completion finish.
     */
    void set_event_registration(const void* registration) noexcept {
        event_registration_ = registration;
    }

    bool is_registered_hook() const noexcept {
        return std::holds_alternative<mudmux_hook_type_t>(target_);
    }

    /** MAX_HOOK_TYPE identifies a direct event callback. */
    mudmux_hook_type_t hook_type() const noexcept {
        const auto* type = std::get_if<mudmux_hook_type_t>(&target_);
        return type ? *type : MAX_HOOK_TYPE;
    }

    int message() const noexcept { return msg_; }
    int current_slot() const noexcept { return current_slot_; }
    const void* event_registration() const noexcept { return event_registration_; }

    /**
     * Invoke through the existing hook helpers, preserving strict-mode locking
     * and current-slot scope. The scheduler selects output flushing: current
     * slot tasks use false and event tasks use true. Exceptions propagate to
     * the caller; completion is always a separate, explicit operation.
     */
    int invoke(bool flush_after) {
        void* data = payload_.empty() ? nullptr : payload_.data();
        if (const auto* hook_type = std::get_if<mudmux_hook_type_t>(&target_))
            return mudmux_invoke_registered_hook(
                *hook_type, ctx_, msg_, data, payload_.size(), flush_after, current_slot_);
        return mudmux_invoke_hook(
            std::get<mudmux_hook_func_t>(target_), ctx_, msg_, data, payload_.size(),
            flush_after, current_slot_);
    }

    /** Pass the original message, not the hook return value, to completion. */
    void complete() const {
        if (completion_)
            completion_(completion_context_, msg_);
    }

private:
    void copy_payload(const void* data, std::size_t size) {
        if (size && !data)
            throw std::invalid_argument("nonempty hook payload requires data");
        payload_.resize(size);
        if (size)
            std::memcpy(payload_.data(), data, size);
    }

    std::variant<mudmux_hook_type_t, mudmux_hook_func_t> target_;
    void* ctx_;
    int msg_;
    // Independent of msg: Telnet subnegotiation passes its option as msg.
    int current_slot_{-1};
    const void* event_registration_{nullptr};
    std::vector<char> payload_;
    mudmux_hook_completion_t completion_{nullptr};
    void* completion_context_{nullptr};
};

void mudmux_workers_configure(int thread_pool_size, std::size_t backlog_capacity = 8);
int mudmux_workers_configured_pool_size();
std::size_t mudmux_workers_configured_backlog_capacity();
bool mudmux_workers_is_worker_thread();

/** Start a slot await after the hook that requested it has returned. */
bool mudmux_execution_finalize_await(int slot);

/** Cancel a pending slot await when its communication slot begins closing. */
void mudmux_execution_cancel_await(int slot);

mudmux_determinism_mode_t mudmux_execution_mode();
const char* mudmux_execution_mode_name();

/**
 * Dispatch a registered hook or direct event callback with owned payload and
 * optional completion. Strict mode invokes inline. Relaxed mode uses the
 * per-slot scheduler for transport hooks. Ordinary direct callbacks use the
 * serialized event scheduler; timely events are scheduled independently per
 * registration. Registered timer/GC hooks remain inline.
 *
 * @param flags Default 0 permits inline execution while stopped, preserving
 * normal routing; queueing still requires running workers. TIMELY_EVENT is
 * for direct callbacks with an event registration: require running workers and
 * reject only if that registration already has an accepted execution (including
 * worker-queued work and completion). Other registrations remain schedulable.
 * Rejected timely events are dropped without invocation or completion.
 * @retval MUDMUX_DISPATCH_OK if inline invocation returned a nonnegative result.
 * @retval MUDMUX_DISPATCH_QUEUED if accepted for worker execution.
 * @retval MUDMUX_DISPATCH_QUEUE_FULL if a slot or the same registration is busy.
 * @retval MUDMUX_DISPATCH_ERROR on lifecycle rejection, inline hook failure,
 * or scheduling failure. Completion still runs after a negative inline hook
 * result; rejected executions never invoke completion.
 */
mudmux_dispatch_result_t mudmux_execution_dispatch(
    mudmux_execution execution, unsigned int flags = 0);

/** True while this registration has accepted work, including completion. */
bool mudmux_execution_event_busy(const void* registration);

/** True while the slot has its one permitted relaxed-mode hook in flight. */
bool mudmux_execution_slot_busy(int slot);

/**
 * @brief Enqueue a registered-hook execution in the thread pool.
 * @param execution Owned hook context, including payload and completion.
 * @param allow_pending Allow explicit non-inbound hooks to queue behind a busy
 * slot. Inbound and Telnet parser hooks always return MUDMUX_DISPATCH_QUEUE_FULL
 * when busy, keeping later input in raw transport buffers.
 * @param queue_slot Slot used to preserve per-slot ordering. Defaults to the
 * execution message. Telnet subnegotiation must pass its slot explicitly,
 * because its message is the Telnet option.
 * @retval MUDMUX_DISPATCH_QUEUED if the hook was successfully enqueued
 * @retval MUDMUX_DISPATCH_QUEUE_FULL if the slot is busy and allow_pending is false
 * @retval MUDMUX_DISPATCH_ERROR on any other error
 */
mudmux_dispatch_result_t mudmux_execution_enqueue(
    mudmux_execution execution, bool allow_pending = false, int queue_slot = -1);

/**
 * @brief Determine if a hook type should be dispatched asynchronously in the thread pool.
 * @param hook_type The type of hook to check.
 * @return true if the hook type should be dispatched asynchronously, false otherwise.
 */
bool mudmux_execution_should_dispatch_async(enum mudmux_hook_type_t hook_type);

#endif
