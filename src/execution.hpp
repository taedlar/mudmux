#ifndef MUDMUX_EXECUTION_HPP
#define MUDMUX_EXECUTION_HPP

#include <cstddef>

#include "hooks.hpp"
#include "mudmux/hooks.h"

enum mudmux_determinism_mode_t {
    MUDMUX_DETERMINISM_STRICT = 0,
    MUDMUX_DETERMINISM_RELAXED = 1,
};

void mudmux_execution_configure(int thread_pool_size);
bool mudmux_execution_start();
void mudmux_execution_stop();
int mudmux_execution_thread_pool_size();
mudmux_determinism_mode_t mudmux_execution_mode();
const char* mudmux_execution_mode_name();
bool mudmux_execution_is_worker_thread();

/** Dispatch a non-slot event hook.  Relaxed mode serializes all event hooks. */
bool mudmux_execution_dispatch_event(mudmux_hook_func_t hook_func, void* ctx);

/** True while the slot has its one permitted relaxed-mode hook in flight. */
bool mudmux_execution_slot_busy(int slot);

/**
 * @brief Enqueue a hook for execution in the thread pool.
 * @param hook_type The type of hook to enqueue.
 * @param ctx The context pointer to pass to the hook.
 * @param slot The communication slot associated with the hook.
 * @param data Optional pointer to additional data to pass to the hook.
 * @param size The size of the additional data in bytes.
 * @param completion Optional completion callback to invoke after the hook has been executed.
 * @param completion_context Optional context pointer to pass to the completion callback.
 * @param allow_pending If true, allows the hook to be queued if the slot is busy; otherwise, returns MUDMUX_DISPATCH_QUEUE_FULL if the slot is busy.
 * @retval MUDMUX_DISPATCH_OK if the hook was successfully enqueued
 * @retval MUDMUX_DISPATCH_QUEUE_FULL if the slot is busy and allow_pending is false
 * @retval MUDMUX_DISPATCH_ERROR on any other error
 */
mudmux_dispatch_result_t mudmux_execution_enqueue_hook(
    enum mudmux_hook_type_t hook_type, void* ctx, int slot, const void* data, size_t size,
    mudmux_hook_completion_t completion = nullptr, void* completion_context = nullptr,
    bool allow_pending = false);

mudmux_dispatch_result_t mudmux_execution_enqueue_telnet_subneg(void* ctx, int slot, int option, const void* data, size_t size);

/**
 * @brief Determine if a hook type should be dispatched asynchronously in the thread pool.
 * @param hook_type The type of hook to check.
 * @return true if the hook type should be dispatched asynchronously, false otherwise.
 */
bool mudmux_execution_should_dispatch_async(enum mudmux_hook_type_t hook_type);

#endif
