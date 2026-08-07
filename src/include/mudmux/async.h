#ifndef MUDMUX_ASYNC_H
#define MUDMUX_ASYNC_H

#include "mudmux_export.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/**
 * Platform-independent storage for an async event. Initialize it with
 * async_event_init() before use; callers may allocate it on the stack.
 */
typedef struct async_event_s {
    uint64_t _opaque[20];
} async_event_t;
typedef struct async_queue_s async_queue_t;
typedef struct async_runtime_s async_runtime_t;

typedef enum async_queue_flags_e {
    ASYNC_QUEUE_DROP_OLDEST = 0x01,
    ASYNC_QUEUE_BLOCK_WRITER = 0x02,
    ASYNC_QUEUE_SIGNAL_ON_DATA = 0x04
} async_queue_flags_t;

typedef struct async_queue_stats_s {
    size_t capacity;
    size_t current_size;
    size_t max_msg_size;
    uint64_t enqueue_count;
    uint64_t dequeue_count;
    uint64_t dropped_count;
} async_queue_stats_t;

typedef struct mudmux_async_api_s {
    bool (*event_init)(async_event_t* event, bool manual_reset, bool initial_state);
    void (*event_destroy)(async_event_t* event);
    void (*event_set)(async_event_t* event);
    void (*event_reset)(async_event_t* event);
    bool (*event_wait)(async_event_t* event, int timeout_ms);
    async_wait_handle_t (*event_get_wait_handle)(async_event_t* event);
    async_queue_t* (*queue_create)(size_t capacity, size_t max_msg_size, async_queue_flags_t flags);
    void (*queue_destroy)(async_queue_t* queue);
    bool (*queue_enqueue)(async_queue_t* queue, const void* data, size_t size);
    bool (*queue_dequeue)(async_queue_t* queue, void* buffer, size_t buffer_size, size_t* out_size);
    bool (*queue_is_empty)(const async_queue_t* queue);
    bool (*queue_is_full)(const async_queue_t* queue);
    void (*queue_clear)(async_queue_t* queue);
    void (*queue_get_stats)(const async_queue_t* queue, async_queue_stats_t* stats);
} mudmux_async_api_v1_t;

#if !defined(MUDMUX_STATIC_DEFINE) && !defined(mudmux_EXPORTS)
#define async_event_init                mudmux_async_api_v1->event_init
#define async_event_destroy             mudmux_async_api_v1->event_destroy
#define async_event_set                 mudmux_async_api_v1->event_set
#define async_event_reset               mudmux_async_api_v1->event_reset
#define async_event_wait                mudmux_async_api_v1->event_wait
#define async_event_get_wait_handle     mudmux_async_api_v1->event_get_wait_handle
#define async_queue_create              mudmux_async_api_v1->queue_create
#define async_queue_destroy             mudmux_async_api_v1->queue_destroy
#define async_queue_enqueue             mudmux_async_api_v1->queue_enqueue
#define async_queue_dequeue             mudmux_async_api_v1->queue_dequeue
#define async_queue_is_empty            mudmux_async_api_v1->queue_is_empty
#define async_queue_is_full             mudmux_async_api_v1->queue_is_full
#define async_queue_clear               mudmux_async_api_v1->queue_clear
#define async_queue_get_stats           mudmux_async_api_v1->queue_get_stats
#endif

#ifdef __cplusplus
extern "C" {
#endif

MUDMUX_EXPORT extern mudmux_async_api_v1_t* mudmux_async_api_v1;

#ifdef __cplusplus
}
#endif

#endif /* MUDMUX_ASYNC_H */
