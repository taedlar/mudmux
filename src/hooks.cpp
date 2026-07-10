#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <mutex>

#include "mudmux/mudmux.h"
#include "mudmux/hooks.h"
#include "comm/outbound.hpp"

static mudmux_hook_func_t all_hooks[MUDMUX_HOOK_MAX] = {nullptr}; // array of hook functions

MUDMUX_EXPORT bool mudmux_register_hook (enum mudmux_hook_type_t hook_type, mudmux_hook_func_t hook_func) {
    if (hook_type <= 0 || hook_type >= MUDMUX_HOOK_MAX || !hook_func) {
        SPDLOG_ERROR ("mudmux_register_hook() called with invalid hook_type or null hook_func");
        return false;
    }
    all_hooks[hook_type] = hook_func;
    return true;
}

MUDMUX_EXPORT int mudmux_invoke_hook (enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size) {
    if (hook_type <= 0 || hook_type >= MUDMUX_HOOK_MAX) {
        SPDLOG_ERROR ("mudmux_invoke_hook() called with invalid hook_type");
        return -1;
    }
    mudmux_hook_func_t hook_func = all_hooks[hook_type];
    if (!hook_func)
        return 0; // no-op if no hook is registered for this type

    // ===== ENTERING LOGIC LAYER =====
    int ret = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(comm_slots_mtx);
        ret = hook_func(ctx, msg, data, size);
    }
    // ===== EXITING LOGIC LAYER =====

    comm_flush_all (async_get_current_runtime()); // flush any buffered output after hook invocation
    return ret;
}
