/**
 * @file async_event.cpp
 * @brief C++11 implementation of platform-agnostic synchronization primitives
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include "async_event.h"
#include <new>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <mutex>
#include <condition_variable>
#include <chrono>
#endif

#ifdef _WIN32
/* Windows: Use native event for WaitForMultipleObjects compatibility */
struct EventImpl {
    HANDLE event;
};
#else
/* POSIX: Use C++11 condition_variable */
struct EventImpl {
    std::mutex mtx;
    std::condition_variable cv;
    bool signaled;
    bool manual_reset;
};
#endif

/* Static assertions to ensure opaque storage is large enough */
static_assert(sizeof(EventImpl) <= sizeof(async_event_t),
              "async_event_t storage too small for EventImpl");

/* Helper to get EventImpl pointer from opaque storage */
static inline EventImpl* get_event(async_event_t* event) {
    return reinterpret_cast<EventImpl*>(event);
}

/* Event API */

bool async_event_init(async_event_t* event, bool manual_reset, bool initial_state) {
    if (!event) return false;
    
    try {
#ifdef _WIN32
        HANDLE h = CreateEvent(
            NULL,
            manual_reset ? TRUE : FALSE,
            initial_state ? TRUE : FALSE,
            NULL
        );
        if (!h) return false;
        
        new (event) EventImpl{ h };
        return true;
#else
        new (event) EventImpl{
            std::mutex{},
            std::condition_variable{},
            initial_state,
            manual_reset
        };
        return true;
#endif
    } catch (...) {
        return false;
    }
}

void async_event_destroy(async_event_t* event) {
    if (event) {
#ifdef _WIN32
        EventImpl* impl = get_event(event);
        if (impl->event) {
            CloseHandle(impl->event);
        }
#endif
        get_event(event)->~EventImpl();
    }
}

void async_event_set(async_event_t* event) {
    if (!event) return;
    
#ifdef _WIN32
    EventImpl* impl = get_event(event);
    if (impl->event) {
        SetEvent(impl->event);
    }
#else
    EventImpl* impl = get_event(event);
    std::lock_guard<std::mutex> lock(impl->mtx);
    impl->signaled = true;
    
    if (impl->manual_reset) {
        impl->cv.notify_all();  /* Wake all waiters for manual-reset */
    } else {
        impl->cv.notify_one();  /* Wake one waiter for auto-reset */
    }
#endif
}

void async_event_reset(async_event_t* event) {
    if (!event) return;
    
#ifdef _WIN32
    EventImpl* impl = get_event(event);
    if (impl->event) {
        ResetEvent(impl->event);
    }
#else
    EventImpl* impl = get_event(event);
    std::lock_guard<std::mutex> lock(impl->mtx);
    impl->signaled = false;
#endif
}

bool async_event_wait(async_event_t* event, int timeout_ms) {
    if (!event) return false;
    
    EventImpl* impl = get_event(event);
    
#ifdef _WIN32
    if (!impl->event) return false;
    
    DWORD timeout = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD result = WaitForSingleObject(impl->event, timeout);
    
    return result == WAIT_OBJECT_0;
#else
    std::unique_lock<std::mutex> lock(impl->mtx);
    
    bool result;
    
    if (impl->signaled) {
        /* Already signaled */
        result = true;
        if (!impl->manual_reset) {
            impl->signaled = false;  /* Auto-reset */
        }
    } else if (timeout_ms == 0) {
        /* Non-blocking check */
        result = false;
    } else if (timeout_ms < 0) {
        /* Infinite wait */
        impl->cv.wait(lock, [impl] { return impl->signaled; });
        result = true;
        if (!impl->manual_reset) {
            impl->signaled = false;  /* Auto-reset */
        }
    } else {
        /* Timed wait with proper spurious wakeup handling */
        result = impl->cv.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            [impl] { return impl->signaled; }
        );
        
        if (result && !impl->manual_reset) {
            impl->signaled = false;  /* Auto-reset */
        }
    }
    
    return result;
#endif
}

#ifdef _WIN32
void* async_event_get_native_handle(async_event_t* event) {
    if (!event) return NULL;
    return get_event(event)->event;
}
#endif
