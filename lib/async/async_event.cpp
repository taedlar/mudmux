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
    #include <chrono>
    #include <errno.h>
    #include <fcntl.h>
    #include <mutex>
    #include <poll.h>
    #include <unistd.h>
    #if defined(__linux__)
    #include <sys/eventfd.h>
    #endif
#endif

#include "mudmux/async.h"

#ifdef _WIN32
/* Windows: Use native event for WaitForMultipleObjects compatibility */
struct EventImpl {
    HANDLE event;
    bool manual_reset;
};
#else
/* POSIX: Use a pollable kernel object with userspace reset semantics. */
struct EventImpl {
#if defined(__linux__)
    int event_fd;
#else
    int read_fd;
    int write_fd;
#endif
    std::mutex state_mutex;
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

static inline const EventImpl* get_event(const async_event_t* event) {
    return reinterpret_cast<const EventImpl*>(event);
}

#ifndef _WIN32
static int get_poll_fd(const EventImpl* impl) {
#if defined(__linux__)
    return impl->event_fd;
#else
    return impl->read_fd;
#endif
}

#if !defined(__linux__)
static bool set_fd_flags(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }

    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) {
        return false;
    }

    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}
#endif

static bool read_signal_once(const EventImpl* impl, uint64_t* value) {
    ssize_t result;

#if defined(__linux__)
    uint64_t local_value = 0;
    do {
        result = read(impl->event_fd, &local_value, sizeof(local_value));
    } while (result < 0 && errno == EINTR);

    if (result == static_cast<ssize_t>(sizeof(local_value)) && value) {
        *value = local_value;
    }

    return result == static_cast<ssize_t>(sizeof(local_value));
#else
    unsigned char byte = 0;
    do {
        result = read(impl->read_fd, &byte, sizeof(byte));
    } while (result < 0 && errno == EINTR);

    if (result == static_cast<ssize_t>(sizeof(byte)) && value) {
        *value = byte;
    }

    return result == static_cast<ssize_t>(sizeof(byte));
#endif
}

static bool write_signal_once(const EventImpl* impl) {
    ssize_t result;

#if defined(__linux__)
    uint64_t value = 1;
    do {
        result = write(impl->event_fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);

    return result == static_cast<ssize_t>(sizeof(value));
#else
    unsigned char byte = 1;
    do {
        result = write(impl->write_fd, &byte, sizeof(byte));
    } while (result < 0 && errno == EINTR);

    return result == static_cast<ssize_t>(sizeof(byte));
#endif
}

static void drain_signal(const EventImpl* impl) {
    uint64_t value;

    while (read_signal_once(impl, &value)) {
    }
}

static bool consume_event_signal(EventImpl* impl) {
    uint64_t value;

    if (!impl->signaled) {
        return false;
    }

    if (!read_signal_once(impl, &value)) {
        return false;
    }

    impl->signaled = false;
    return true;
}
#endif

/* Event API */

bool async_event_init (async_event_t* event, bool manual_reset, bool initial_state) {
    if (!event)
        return false;
    
    try {
#ifdef _WIN32
        HANDLE h = CreateEvent(
            NULL,
            manual_reset ? TRUE : FALSE,
            initial_state ? TRUE : FALSE,
            NULL
        );
        if (!h) return false;
        
        new (event) EventImpl{ h, manual_reset };
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

        if (initial_state && !write_signal_once(get_event(event))) {
            get_event(event)->~EventImpl();
            close(event_fd);
            return false;
        }

        return true;
#else
        int pipe_fds[2] = {-1, -1};
        if (pipe(pipe_fds) != 0) {
            return false;
        }

        if (!set_fd_flags(pipe_fds[0]) || !set_fd_flags(pipe_fds[1])) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return false;
        }

        new (event) EventImpl{
            pipe_fds[0],
            pipe_fds[1],
            std::mutex{},
            initial_state,
            manual_reset
        };

        if (initial_state && !write_signal_once(get_event(event))) {
            get_event(event)->~EventImpl();
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return false;
        }

        return true;
#endif
    } catch (...) {
        return false;
    }
}

void async_event_destroy (async_event_t* event) {
    if (event) {
        EventImpl* impl = get_event(event);
#ifdef _WIN32
        if (impl->event) {
            CloseHandle(impl->event);
        }
#elif defined(__linux__)
        if (impl->event_fd >= 0) {
            close(impl->event_fd);
            impl->event_fd = -1;
        }
#else
        if (impl->read_fd >= 0) {
            close(impl->read_fd);
            impl->read_fd = -1;
        }
        if (impl->write_fd >= 0) {
            close(impl->write_fd);
            impl->write_fd = -1;
        }
#endif
        get_event(event)->~EventImpl();
    }
}

void async_event_set (async_event_t* event) {
    if (!event) return;
    
#ifdef _WIN32
    EventImpl* impl = get_event(event);
    if (impl->event) {
        SetEvent(impl->event);
    }
#else
    EventImpl* impl = get_event(event);
    std::lock_guard<std::mutex> lock(impl->state_mutex);

    if (impl->signaled) {
        return;
    }

    if (write_signal_once(impl)) {
        impl->signaled = true;
    }
#endif
}

void async_event_reset (async_event_t* event) {
    if (!event) return;
    
#ifdef _WIN32
    EventImpl* impl = get_event(event);
    if (impl->event) {
        ResetEvent(impl->event);
    }
#else
    EventImpl* impl = get_event(event);
    std::lock_guard<std::mutex> lock(impl->state_mutex);

    if (!impl->signaled) {
        return;
    }

    drain_signal(impl);
    impl->signaled = false;
#endif
}

bool async_event_wait (async_event_t* event, int timeout_ms) {
    if (!event) return false;
    
    EventImpl* impl = get_event(event);
    
#ifdef _WIN32
    if (!impl->event) return false;
    
    DWORD timeout = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD result = WaitForSingleObject(impl->event, timeout);
    
    return result == WAIT_OBJECT_0;
#else
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
        pfd.fd = get_poll_fd(impl);
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
#endif
}

bool async_event_is_manual_reset(const async_event_t* event) {
    return event && get_event(event)->manual_reset;
}

async_wait_handle_t async_event_get_wait_handle(async_event_t* event) {
    if (!event) {
        return ASYNC_INVALID_WAIT_HANDLE;
    }

#ifdef _WIN32
    return get_event(event)->event;
#else
    return get_poll_fd(get_event(event));
#endif
}
