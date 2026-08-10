# mudmux — Agent Instructions

## Project Overview

**mudmux** is a C++17 transport-layer host for MUD servers. It handles low-level networking and async I/O, generates input events for the hosting server, and interact with in-process C APIs and hook functions.

See [README.md](README.md) for the full feature list.

## Architecture

```
src/              — mudmux shared library
  mudmux.cpp      — C API impl (mudmux_init / mudmux_run / mudmux_shutdown)
  hooks.cpp       — hook registration & dispatch
  include/mudmux/ — public C API headers (mudmux.h, hooks.h)
lib/async/        — platform-agnostic async event loop (epoll/IOCP/poll)
lib/comm/         — unified read/write abstraction (socket, console, pipe)
cmake/            — helper CMake modules (fetch-settings, utils, setup)
examples/         — example MUD servers
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

**Preferred verify step**: build the default target — it builds both `mudmux` (the shared library) and `chatroom`.

### Quick Testing with `chatroom`

To test the `mudmux` shared library, use the executable `chatroom` to setup a simple MUD server.

- Running `chatroom` without argumnets will start the server without listening ports and idle until Ctrl-C pressed or SIGINT received. The default logging verbosity is set to "warning". Use `--help` option to get program help documents.

## Dependencies

Fetched automatically by CMake via FetchContent (declared in `cmake/fetch-settings.cmake`):

| Library | Used by |
|---------|---------|
| spdlog | logging throughout |
| yaml-cpp | config parsing in `mudmux_init` |
| argparse | examples CLI |
| GoogleTest | unit tests (when `BUILD_TESTING=ON`) |

System deps: **OpenSSL** (required), **Boost.JSON** (optional, via `find_boost`).

## Conventions

- **C++17**, `-Wall -Wextra -Wpedantic` (GCC/Clang); `/W3 /permissive- /utf-8` (MSVC).
- Public API is plain **C** (`extern "C"`) with `MUDMUX_EXPORT` visibility. Internal C++ code uses default-hidden visibility (`CMAKE_CXX_VISIBILITY_PRESET hidden`).
- Platform-specific async runtime files are named with a suffix: `_epoll`, `_iocp`, `_poll`. Keep this naming for new runtime backends.
- Logging: `SPDLOG_*` macros. In Debug builds `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE` is set at compile time.
- Configuration is YAML, passed as a string to `mudmux_init()`; see `src/mud.conf` for an example.

### Internal Comm Helper Contract

For `lib/comm` internal helper functions that operate on slot state, prefer a stack-scoped `comm_abstract_ptr&` parameter over raw `comm_abstract_t*`.

- Acquire `comm_abstract_ptr comm(slot, comm_slots_mtx);` at the boundary, then pass `comm` down helper calls.
- Do not store `comm_abstract_ptr` beyond the current stack frame or pass it across async/thread boundaries.
- Use raw `comm_abstract_t*` only for tightly local leaf code where lock ownership is unambiguous and cannot escape.

## Hook System

Hooks (`mudmux_hook_type_t`) let the loaded logic layer react to transport events:

| Hook | When fired |
|------|-----------|
| `HOOK_CONNECT` | new connection accepted |
| `HOOK_DISCONNECT` | connection closed |
| `HOOK_MESSAGE_INBOUND` | data received from client |
| `HOOK_MESSAGE_OUTBOUND` | data sent to client |
| `HOOK_PROMPT` | finished draining inbound data |

Register with `mudmux_register_hook()`; mudmux invokes registered hooks as the
corresponding transport events occur.

### Hook API Protection and Threading Model

In strict mode, the event-loop thread enters the logic layer while holding the MUD logic mutex. This preserves the original serialized behavior for comm API usage inside hooks.

In relaxed mode, hook callbacks may run concurrently on worker threads. The comm API is expected to remain safe for these callbacks through its own internal synchronization, without relying on a global hook mutex.

- In strict-mode hook callbacks: treat comm API calls as part of a serialized critical section on the main event-loop thread.
- Outside the documented API path, or when touching logic-layer shared state from multiple threads, use proper synchronization (mutexes/atomics/queues) to avoid races.

Practical rule: if logic code touches shared data from worker threads or from code paths not running under the documented comm API, synchronization is required.

### Inbound Transport Ordering

Inbound state is ordered per slot. In relaxed mode, hooks for different slots may run concurrently, but each slot permits only one hook in flight. A slot must not parse or retain another decoded inbound message while that hook is running; later bytes stay in the raw transport buffers.

- The execution state gates same-slot processing while an inbound hook is in flight; `C_DEFERRED_INBOUND` records that buffered bytes need a resume pass. There is no per-slot inbound-hook/payload queue.
- Parser-originated inbound hooks never queue. Explicit non-inbound hook/API requests for another slot may use that slot's bounded, generation-checked continuation queue and execute after its active hook.
- Preserve this gate in every inbound path: line input, character input, WebSocket-decoded messages, Telnet payloads, and any new transport parser.
- On POSIX readiness backends, do not consume socket data while `HOOK_CONNECT` or an inbound hook is still configuring that slot. This prevents TLS ClientHello bytes from being buffered as plaintext before `comm_enable_tls()` runs.
- The inbound layering is: transport read → TLS decrypt (if enabled) → WebSocket upgrade/frame decode (if enabled) → Telnet parser (if enabled) → line/character parser → `HOOK_MESSAGE_INBOUND`.
- WebSocket must be enabled before direct Telnet. Telnet-over-WebSocket is enabled only after negotiating the supported Telnet subprotocol during the HTTP upgrade.

See [docs/inbound.md](docs/inbound.md) for the full state and transport-combination design.

### Proactive Slot Closing (`comm_close`)

Logic-layer hooks can proactively close a communication slot via `comm_close(runtime, slot)` from `mudmux/comm.h`.

- Typical usage: call from `HOOK_MESSAGE_INBOUND` when processing a command like `quit`/`exit` (see `src/main.cpp`).
- `runtime` may be `nullptr`; `comm_close()` resolves the current runtime internally.
- First close request sets `C_CLOSING` and invokes `HOOK_DISCONNECT` once, so logic code can run disconnect cleanup.
- If outbound data is still buffered (`C_BUFFERED_WRITE`), close is deferred until flush completes; in this case `comm_close()` returns `false`.
- For console slot (`COMM_SLOT_CONSOLE`), close is coordinated through console EOF signaling and may also return `false` until final teardown finishes.
- When the slot is already gone or fully removed, `comm_close()` returns `true`.

### Input Mode Control

Logic-layer hooks can switch the input mode and echo behaviour of a slot using three functions from `mudmux/comm.h`:

| Function | Description |
|----------|-------------|
| `comm_set_line_input(slot, echo)` | Switch to cooked/line-input mode (TELNET line mode, `termios` `ICANON`, Windows cooked). `echo` controls local character echo. |
| `comm_set_char_input(slot)` | Switch to single-character (raw) input mode. Echo is always disabled; call `comm_set_echo()` to re-enable it. On Windows, `ENABLE_VIRTUAL_TERMINAL_INPUT` is set so ANSI escape sequences for special keys are delivered. |
| `comm_set_echo(slot, echo)` | Enable or disable client echo without changing the current input mode. |

All three functions support `COMM_SLOT_CONSOLE` as well as network slots and return `true` on success.

### Virtual Terminal Output (`comm_enable_virtual_terminal`)

Logic-layer hooks can enable ANSI/VT100 output processing on a slot via `comm_enable_virtual_terminal(slot)` from `mudmux/comm.h`.

- No-op on Linux/POSIX where VT processing is always active.
- On Windows, sets `ENABLE_VIRTUAL_TERMINAL_PROCESSING` (plus `ENABLE_PROCESSED_OUTPUT` and `ENABLE_WRAP_AT_EOL_OUTPUT`) on the console stdout so that ANSI escape sequences in outbound data are rendered correctly.
- Supports `COMM_SLOT_CONSOLE` as well as network slots; returns `true` on success.
