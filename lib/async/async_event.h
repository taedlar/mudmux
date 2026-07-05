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

/**
 * Platform-agnostic event
 * Opaque storage - actual C++ objects constructed via placement new
 */
typedef struct async_event_s {
    /* Opaque storage sized for std::mutex + std::condition_variable + flags */
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

#ifdef _WIN32
/**
 * Get native Windows HANDLE for event (for WaitForMultipleObjects usage)
 * @param event Event to query
 * @returns Windows HANDLE, or NULL if invalid
 */
void* async_event_get_native_handle(async_event_t* event);
#endif

#endif /* !ASYNC_SYNC_H */
