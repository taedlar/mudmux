#ifndef MUDMUX_EXECUTION_H
#define MUDMUX_EXECUTION_H

#include "mudmux_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Version 1 of the runtime-bound execution API. */
typedef struct mudmux_execution_api_s {
    /**
     * Return whether a call to mudmux_run() is currently active.
     *
     * The result becomes true when mudmux_run() begins and becomes false
     * before it returns, including when runtime initialization fails. This is
     * independent of the worker-pool lifecycle.
     */
    bool (*is_running)(void);
} mudmux_execution_api_v1_t;

#if !defined(MUDMUX_STATIC_DEFINE) && !defined(mudmux_EXPORTS)
#define mudmux_is_running mudmux_execution_api_v1->is_running
#endif

MUDMUX_EXPORT extern mudmux_execution_api_v1_t* mudmux_execution_api_v1;

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_EXECUTION_H */
