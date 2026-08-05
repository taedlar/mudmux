#ifndef MUDMUX_HOOKS_INTERNAL_HPP
#define MUDMUX_HOOKS_INTERNAL_HPP

#include "mudmux/hooks.h"

using mudmux_hook_completion_t = void (*)(void* context, int msg);

int mudmux_invoke_registered_hook(enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size, bool flush_after);

mudmux_dispatch_result_t mudmux_dispatch_hook(enum mudmux_hook_type_t hook_type, void* ctx, int msg, const void* data, size_t size);

/**
 * @brief Dispatch a hook for execution in the thread pool, with optional completion callback.
 * @param hook_type The type of hook to dispatch.
 * @param ctx The context pointer to pass to the hook.
 * @param msg The message or event code to pass to the hook.
 * @param data Optional pointer to additional data to pass to the hook.
 * @param size The size of the additional data in bytes.
 * @param completion Optional completion callback to invoke after the hook has been executed.
 * @param completion_context Optional context pointer to pass to the completion callback.
 * @retval MUDMUX_DISPATCH_OK if the hook was successfully dispatched.
 * @retval MUDMUX_DISPATCH_QUEUE_FULL if the slot is busy and the hook cannot be queued.
 * @retval MUDMUX_DISPATCH_ERROR on any other error.
 */
mudmux_dispatch_result_t mudmux_dispatch_hook_after(
    enum mudmux_hook_type_t hook_type,
    void* ctx,
    int msg,
    const void* data,
    size_t size,
    mudmux_hook_completion_t completion,
    void* completion_context);

#endif
