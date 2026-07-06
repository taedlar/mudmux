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
#elif defined(__linux__)
#include <chrono>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>
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
#elif defined(__linux__)
/* Linux: Use eventfd for pollable signaling with userspace reset semantics. */
struct EventImpl {
    int event_fd;
    std::mutex state_mutex;
    bool signaled;
    bool manual_reset;
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

#if defined(__linux__)
static bool read_eventfd_once(int event_fd, uint64_t* value) {
    ssize_t result;

    do {
        result = read(event_fd, value, sizeof(*value));
    } while (result < 0 && errno == EINTR);

    return result == static_cast<ssize_t>(sizeof(*value));
}

static bool write_eventfd_once(int event_fd, uint64_t value) {
    ssize_t result;

    do {
        result = write(event_fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);

    return result == static_cast<ssize_t>(sizeof(value));
}

static void drain_eventfd(int event_fd) {
    uint64_t value;

    while (read_eventfd_once(event_fd, &value)) {
    }
}

static bool consume_event_signal(EventImpl* impl) {
    uint64_t value;

    if (!impl->signaled) {
        return false;
    }

    if (!read_eventfd_once(impl->event_fd, &value)) {
        return false;
    }

    impl->signaled = false;
    return true;
}
#endif

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
#elif defined(__linux__)
        int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (event_fd < 0) {
            return false;
        }

        new (event) EventImpl{
            event_fd,
            std::mutex{},
            initial_state,
            manual_reset
        };

        if (initial_state && !write_eventfd_once(event_fd, 1)) {
            get_event(event)->~EventImpl();
            close(event_fd);
            return false;
        }

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
#elif defined(__linux__)
        EventImpl* impl = get_event(event);
        if (impl->event_fd >= 0) {
            close(impl->event_fd);
            impl->event_fd = -1;
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
#elif defined(__linux__)
    EventImpl* impl = get_event(event);
    std::lock_guard<std::mutex> lock(impl->state_mutex);

    if (impl->signaled) {
        return;
    }

    if (write_eventfd_once(impl->event_fd, 1)) {
        impl->signaled = true;
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
#elif defined(__linux__)
    EventImpl* impl = get_event(event);
    std::lock_guard<std::mutex> lock(impl->state_mutex);

    if (!impl->signaled) {
        return;
    }

    drain_eventfd(impl->event_fd);
    impl->signaled = false;
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
#elif defined(__linux__)
    auto deadline = std::chrono::steady_clock::time_point::max();
    if (timeout_ms >= 0) {
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    }

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(impl->state_mutex);

            if (impl->signaled) {
                if (impl->manual_reset) {
                    return true;
                }

                return consume_event_signal(impl);
            }

            if (timeout_ms == 0) {
                return false;
            }
        }

        int wait_timeout = -1;
        if (timeout_ms >= 0) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }

            wait_timeout = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            if (wait_timeout == 0) {
                wait_timeout = 1;
            }
        }

        struct pollfd pfd;
        pfd.fd = impl->event_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int poll_result;
        do {
            poll_result = poll(&pfd, 1, wait_timeout);
        } while (poll_result < 0 && errno == EINTR);

        if (poll_result <= 0) {
            return poll_result > 0;
        }
    }
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

#if defined(__linux__)
int async_event_get_fd(async_event_t* event) {
    if (!event) return -1;
    return get_event(event)->event_fd;
}
#endif

#ifdef _WIN32
void* async_event_get_native_handle(async_event_t* event) {
    if (!event) return NULL;
    return get_event(event)->event;
}
#endif
