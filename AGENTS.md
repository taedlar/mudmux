# mudmux — Agent Instructions

## Project Overview

**mudmux** is a C++17 transport-layer host for MUD servers. It handles low-level networking and async I/O, generates input events for the hosting server, and interact with in-process C APIs and hook functions.

See [README.md](README.md) for the full feature list.

## Architecture

```
src/              — mudmux shared library + example_server binary
  mudmux.cpp      — C API impl (mudmux_init / mudmux_run / mudmux_shutdown)
  hooks.cpp       — hook registration & dispatch
  include/mudmux/ — public C API headers (mudmux.h, hooks.h)
lib/async/        — platform-agnostic async event loop (epoll/IOCP/poll)
lib/comm/         — unified read/write abstraction (socket, console, pipe)
cmake/            — helper CMake modules (fetch-settings, utils, setup)
```

**Key dependency rule**: any `src/` file that exports a symbol via `MUDMUX_EXPORT` **must** `#include "mudmux/mudmux.h"` (which pulls in the generated `mudmux_export.h`). Missing this will cause undefined-reference link errors on Linux.

## Build & Test

```bash
# Configure (Linux/GCC — see cmake --list-presets for other platforms)
cmake --preset linux-gcc

# Build (dev = Debug; rel = RelWithDebInfo)
cmake --build --preset dev-linux-gcc

# Run tests
ctest --preset units-linux-gcc
```

Build output lands in `out/build/<preset>/`. A `compile_commands.json` is always generated there (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`).

**Preferred verify step**: build the default target — it builds both `mudmux` (the shared library) and `example_server`.

### Quick Testing with `example_server`

To test the `mudmux` shared library, use the executable `example_server` to setup a simple MUD server.

- Running `example_server` without argumnets will start the server without listening ports and idle until Ctrl-C pressed or SIGINT received. The default logging verbosity is set to "warning". Use `--help` option to get program help documents.

## Dependencies

Fetched automatically by CMake via FetchContent (declared in `cmake/fetch-settings.cmake`):

| Library | Used by |
|---------|---------|
| spdlog | logging throughout |
| yaml-cpp | config parsing in `mudmux_init` |
| argparse | `example_server` CLI |
| GoogleTest | unit tests (when `BUILD_TESTING=ON`) |

System deps: **OpenSSL** (required), **Boost.JSON** (optional, via `find_boost`).

## Conventions

- **C++17**, `-Wall -Wextra -Wpedantic` (GCC/Clang); `/W3 /permissive- /utf-8` (MSVC).
- Public API is plain **C** (`extern "C"`) with `MUDMUX_EXPORT` visibility. Internal C++ code uses default-hidden visibility (`CMAKE_CXX_VISIBILITY_PRESET hidden`).
- Platform-specific async runtime files are named with a suffix: `_epoll`, `_iocp`, `_poll`. Keep this naming for new runtime backends.
- Logging: `SPDLOG_*` macros. In Debug builds `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE` is set at compile time.
- Configuration is YAML, passed as a string to `mudmux_init()`; see `src/mud.conf` for an example.

## Hook System

Hooks (`mudmux_hook_type_t`) let the loaded logic layer react to transport events:

| Hook | When fired |
|------|-----------|
| `MUDMUX_HOOK_CONNECT` | new connection accepted |
| `MUDMUX_HOOK_DISCONNECT` | connection closed |
| `MUDMUX_HOOK_MESSAGE_INBOUND` | data received from client |
| `MUDMUX_HOOK_MESSAGE_OUTBOUND` | data sent to client |
| `MUDMUX_HOOK_ERROR` | transport error |

Register with `mudmux_register_hook()`; invoke with `mudmux_invoke_hook()`.

### Proactive Slot Closing (`comm_close`)

Logic-layer hooks can proactively close a communication slot via `comm_close(runtime, slot)` from `mudmux/comm.h`.

- Typical usage: call from `MUDMUX_HOOK_MESSAGE_INBOUND` when processing a command like `quit`/`exit` (see `src/main.cpp`).
- `runtime` may be `nullptr`; `comm_close()` resolves the current runtime internally.
- First close request sets `C_CLOSING` and invokes `MUDMUX_HOOK_DISCONNECT` once, so logic code can run disconnect cleanup.
- If outbound data is still buffered (`C_BUFFERED_WRITE`), close is deferred until flush completes; in this case `comm_close()` returns `false`.
- For console slot (`COMM_SLOT_CONSOLE`), close is coordinated through console EOF signaling and may also return `false` until final teardown finishes.
- When the slot is already gone or fully removed, `comm_close()` returns `true`.
