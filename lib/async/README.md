# Asynchronous Runtime

This library implements asynchronous runtime operations and provides a platform-agnostic control interface.

## Backends

- Linux: epoll-based runtime
- Windows: IOCP-based runtime
- Apple/portable fallback: poll-based runtime

## Threading

- Console input uses a native C++17 `std::thread` in `console_worker.cpp`.
- Runtime completion signaling uses `async_runtime_post_completion()` to wake the main event loop.
- Synchronization primitives are provided by `async_event.cpp`/`async_event.h` as a C-compatible wrapper over C++ primitives.
