#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstring>
#include <mutex>
#include <vector>

#include "execution.hpp"
#include "mudmux/mudmux.h"
#include "mudmux/hooks.h"
#include "comm/outbound.hpp"
#include "comm/current_slot.hpp"
#include "hooks.hpp"

static mudmux_hook_func_t all_hooks[MAX_HOOK_TYPE] = {nullptr}; // array of hook functions

mudmux_hook_func_t mudmux_get_registered_hook(enum mudmux_hook_type_t hook_type) {
    return hook_type > 0 && hook_type < MAX_HOOK_TYPE ? all_hooks[hook_type] : nullptr;
}

int mudmux_invoke_registered_hook(enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size,
                                  bool flush_after, int current_slot_) {
    if (hook_type <= 0 || hook_type >= MAX_HOOK_TYPE) {
        SPDLOG_ERROR ("mudmux_invoke_hook() called with invalid hook_type");
        return -1;
    }

    mudmux_hook_func_t hook_func = all_hooks[hook_type];
    if (!hook_func)
        return 0;

    comm_hook_type_scope_t hook_type_scope(hook_type);
    return mudmux_invoke_hook_function(hook_func, ctx, msg, data, size, flush_after, current_slot_);
}

int mudmux_invoke_hook_function(mudmux_hook_func_t hook_func, void* ctx, int msg, void* data, size_t size,
                                bool flush_after, int current_slot_) {
    int ret = 0;
    comm_current_slot_scope_t current_slot_scope(current_slot_);
    if (mudmux_execution_mode() == MUDMUX_DETERMINISM_STRICT) {
        std::lock_guard<std::recursive_mutex> lock(comm_slots_mtx);
        ret = hook_func(ctx, msg, data, size);
    }
    else {
        ret = hook_func(ctx, msg, data, size);
    }

    if (flush_after)
        comm_flush_all(async_get_current_runtime());
    return ret;
}

MUDMUX_EXPORT bool mudmux_register_hook (enum mudmux_hook_type_t hook_type, mudmux_hook_func_t hook_func) {
    if (hook_type <= 0 || hook_type >= MAX_HOOK_TYPE || !hook_func) {
        SPDLOG_ERROR ("mudmux_register_hook() called with invalid hook_type or null hook_func");
        return false;
    }
    all_hooks[hook_type] = hook_func;
    return true;
}

MUDMUX_EXPORT int mudmux_invoke_hook (enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size) {
    return mudmux_invoke_registered_hook(hook_type, ctx, msg, data, size, true);
}

mudmux_dispatch_result_t mudmux_dispatch_hook(enum mudmux_hook_type_t hook_type, void* ctx, int msg, const void* data, size_t size) {
    return mudmux_dispatch_hook_after(hook_type, ctx, msg, data, size, nullptr, nullptr);
}

mudmux_dispatch_result_t mudmux_dispatch_hook_after(
    enum mudmux_hook_type_t hook_type,
    void* ctx,
    int msg,
    const void* data,
    size_t size,
    mudmux_hook_completion_t completion,
    void* completion_context,
    int current_slot_) {
    if (!mudmux_execution_should_dispatch_async(hook_type)) {
        const mudmux_dispatch_result_t result = static_cast<mudmux_dispatch_result_t>(
            mudmux_invoke_registered_hook(hook_type, ctx, msg, const_cast<void*>(data), size, true, current_slot_) < 0
                ? MUDMUX_DISPATCH_ERROR
                : MUDMUX_DISPATCH_OK);
        if (completion)
            completion(completion_context, msg);
        (void)mudmux_execution_finalize_await(current_slot_);
        return result;
    }

    if (hook_type == HOOK_TELNET_SUBNEG) {
        return MUDMUX_DISPATCH_ERROR;
    }

    // Only parser-originated input is forbidden from queueing.  A hook/API
    // request targeting another slot is an explicit lifecycle/action request;
    // retain it in that target slot's bounded FIFO after the active hook.
    const bool allow_pending = hook_type != HOOK_MESSAGE_INBOUND;
    const int queue_slot = hook_type == HOOK_MESSAGE_OUTBOUND
        ? static_cast<int>(static_cast<uint32_t>(msg) & 0xffffu)
        : -1;
    return mudmux_execution_enqueue_hook(
        hook_type, ctx, msg, data, size, completion, completion_context, allow_pending, queue_slot, current_slot_);
}
