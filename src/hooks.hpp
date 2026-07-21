#ifndef MUDMUX_HOOKS_INTERNAL_HPP
#define MUDMUX_HOOKS_INTERNAL_HPP

#include "mudmux/hooks.h"

enum mudmux_dispatch_result_t {
	MUDMUX_DISPATCH_ERROR = -1,
	MUDMUX_DISPATCH_OK = 0,
	MUDMUX_DISPATCH_QUEUE_FULL = 1,
};

int mudmux_invoke_registered_hook(enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size, bool flush_after);
mudmux_dispatch_result_t mudmux_dispatch_hook(enum mudmux_hook_type_t hook_type, void* ctx, int msg, const void* data, size_t size);

#endif