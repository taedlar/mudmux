#ifndef MUDMUX_HOOKS_H
#define MUDMUX_HOOKS_H

#include "mudmux_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mudmux_hook_type_t {
    HOOK_CONNECT = 1,
    HOOK_DISCONNECT = 2,
    HOOK_MESSAGE_INBOUND = 3,
    HOOK_MESSAGE_OUTBOUND = 4,
    HOOK_PROMPT = 5,
    HOOK_TELNET_SUBNEG = 6,
    HOOK_TIMER = 7,
    HOOK_GARBAGE_COLLECTION = 8,
    MAX_HOOK_TYPE
};

enum mudmux_dispatch_result_t {
	MUDMUX_DISPATCH_ERROR = -1,
	MUDMUX_DISPATCH_OK = 0,
	MUDMUX_DISPATCH_QUEUE_FULL = 1,
};

typedef int (*mudmux_hook_func_t)(void* ctx, int msg, void* data, size_t size);

/**
 * @brief Register a hook function for a specific event.
 * @param hook_type Type of the hook (e.g., HOOK_CONNECT).
 * @param hook_func Function pointer to the hook function.
 * @return true on success, false on failure.
 */
MUDMUX_EXPORT bool mudmux_register_hook (enum mudmux_hook_type_t hook_type, mudmux_hook_func_t hook_func);

MUDMUX_EXPORT int mudmux_invoke_hook (enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_HOOKS_H */
