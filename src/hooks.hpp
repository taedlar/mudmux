#ifndef MUDMUX_HOOKS_INTERNAL_HPP
#define MUDMUX_HOOKS_INTERNAL_HPP

#include "mudmux/hooks.h"

extern bool spdlog_initialized;

/**
 * Invoke a hook function (no need to be registered) with the given context, message, and data.
 * Optionally flush all pending output after invocation.
 */
int mudmux_invoke_hook(mudmux_hook_func_t hook_func, void* ctx, int msg, void* data, size_t size,
                       bool flush_after, int current_slot_ = -1);

int mudmux_invoke_registered_hook(enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size,
                                  bool flush_after, int current_slot_ = -1);
mudmux_hook_func_t mudmux_get_registered_hook(enum mudmux_hook_type_t hook_type);


using mudmux_hook_completion_t = void (*)(void* context, int msg);

void mudmux_reset_registered_hooks();

#endif
