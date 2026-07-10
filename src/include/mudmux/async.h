#ifndef MUDMUX_ASYNC_H
#define MUDMUX_ASYNC_H

#include "mudmux_export.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
typedef HANDLE async_wait_handle_t;
#define ASYNC_INVALID_WAIT_HANDLE NULL
#else
typedef int async_wait_handle_t;
#define ASYNC_INVALID_WAIT_HANDLE (-1)
#endif

typedef struct async_event_s async_event_t;
typedef struct async_queue_s async_queue_t;
typedef struct async_runtime_s async_runtime_t;

typedef struct mudmux_async_api_s {
    bool (*event_init)(async_event_t* event, bool manual_reset, bool initial_state);
    void (*event_destroy)(async_event_t* event);
    void (*event_set)(async_event_t* event);
    void (*event_reset)(async_event_t* event);
    // bool (*event_wait)(async_event_t* event, int timeout_ms);
    async_wait_handle_t (*event_get_wait_handle)(async_event_t* event);
} mudmux_async_api_v1_t;

#if !defined(MUDMUX_STATIC_DEFINE) && !defined(mudmux_EXPORTS)
#define async_event_init                mudmux_async_api_v1->event_init
#define async_event_destroy             mudmux_async_api_v1->event_destroy
#define async_event_set                 mudmux_async_api_v1->event_set
#define async_event_reset               mudmux_async_api_v1->event_reset
// #define async_event_wait                mudmux_async_api_v1->event_wait
#define async_event_get_wait_handle     mudmux_async_api_v1->event_get_wait_handle
#endif

#ifdef __cplusplus
extern "C" {
#endif

MUDMUX_EXPORT extern mudmux_async_api_v1_t* mudmux_async_api_v1;

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_ASYNC_H */
