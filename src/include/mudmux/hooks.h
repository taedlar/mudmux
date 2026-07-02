#ifndef MUDMUX_HOOKS_H
#define MUDMUX_HOOKS_H

#include "mudmux_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mudmux_hook_type_t {
    MUDMUX_HOOK_CONNECT = 1,
    MUDMUX_HOOK_DISCONNECT = 2,
    MUDMUX_HOOK_MESSAGE_INBOUND = 3,
    MUDMUX_HOOK_MESSAGE_OUTBOUND = 4,
    MUDMUX_HOOK_ERROR = 5,
    MUDMUX_HOOK_MAX = 255
};

/**
 * @brief Register a hook function for a specific event.
 * @param hook_type Type of the hook (e.g., MUDMUX_HOOK_CONNECT).
 * @param hook_func Function pointer to the hook function.
 * @return true on success, false on failure.
 */
typedef int (*mudmux_hook_func_t)(void* ctx, int msg, void* data, size_t size);
MUDMUX_EXPORT bool mudmux_register_hook (enum mudmux_hook_type_t hook_type, mudmux_hook_func_t hook_func);

MUDMUX_EXPORT int mudmux_invoke_hook (enum mudmux_hook_type_t hook_type, void* ctx, int msg, void* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_HOOKS_H */
