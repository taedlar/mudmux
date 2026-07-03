#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/hooks.h"
#include "comm/outbound.h"

static mudmux_hook_func_t all_hooks[MUDMUX_HOOK_MAX] = {nullptr}; // array of hook functions

extern "C" bool mudmux_register_hook (enum mudmux_hook_type_t hook_type, mudmux_hook_func_t hook_func) {
    if (hook_type <= 0 || hook_type >= MUDMUX_HOOK_MAX || !hook_func) {
        SPDLOG_ERROR ("mudmux_register_hook() called with invalid hook_type or null hook_func");
        return false;
    }
    all_hooks[hook_type] = hook_func;
    return true;
}

extern "C" int mudmux_invoke_hook (enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size) {
    if (hook_type <= 0 || hook_type >= MUDMUX_HOOK_MAX) {
        SPDLOG_ERROR ("mudmux_invoke_hook() called with invalid hook_type");
        return -1;
    }
    mudmux_hook_func_t hook_func = all_hooks[hook_type];
    if (!hook_func)
        return 0; // no-op if no hook is registered for this type
    int ret = hook_func(ctx, msg, data, size);
    comm_flush_all_outbound (async_get_current_runtime()); // flush any buffered output after hook invocation
    return ret;
}
