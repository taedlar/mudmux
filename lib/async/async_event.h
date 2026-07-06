/**
 * @file async_event.h
 * @brief Platform-agnostic synchronization primitives (events)
 * 
 * C++11-based implementation providing C-compatible API.
 * Internal use only - encapsulated within async library implementations.
 * Main thread code should use the higher-level async APIs instead.
 */
#ifndef ASYNC_SYNC_H
#define ASYNC_SYNC_H

#include <stdint.h>

#include "mudmux/async.h"

/**
 * Platform-agnostic pollable event object for thread synchronization, multiplexing, and signaling.
 * Opaque type; use async_event_* functions to operate on it.
 *
 * Use this to synchronize main threads and worker threads. The waitable handle can be integrated
 * with I/O multiplexing APIs (poll, select, epoll, kqueue) for event-driven programming.
 */
typedef struct async_event_s {
    /* Opaque storage sized for the platform-specific implementation */
    uint64_t _opaque[20];
} async_event_t;

/**
 * Initialize an event
 * @param manual_reset If true, event stays signaled until reset; if false, auto-resets after one wait
 * @param initial_state If true, event starts signaled
 * @returns true on success, false on failure
 */
bool async_event_init(async_event_t* event, bool manual_reset, bool initial_state);

/**
 * Destroy an event
 */
void async_event_destroy(async_event_t* event);

/**
 * Signal an event (wake waiting threads)
 */
void async_event_set(async_event_t* event);

/**
 * Reset an event to unsignaled state
 */
void async_event_reset(async_event_t* event);

/**
 * Wait for an event to be signaled
 * @param timeout_ms Timeout in milliseconds (-1 = infinite)
 * @returns true if event signaled, false if timeout
 */
bool async_event_wait(async_event_t* event, int timeout_ms);

/**
 * Get native pollable/waitable OS handle for event loop integration.
 *
 * - Windows: HANDLE for WaitForMultipleObjects-style waits.
 * - POSIX: readable fd for poll/select/epoll/kqueue integration.
 *
 * @param event Event to query
 * @returns OS wait handle, or ASYNC_INVALID_WAIT_HANDLE if invalid
 */
async_wait_handle_t async_event_get_wait_handle(async_event_t* event);

#endif /* !ASYNC_SYNC_H */
