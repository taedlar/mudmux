# mudmux

`mudmux` handles the painful **transport-layer** tasks (low-level network communication and asynchronous I/O) between MUD server and clients, generates **input events** for high-level logics layer, and enables graceful **non-blcoking** event loop to drive the virtual world.

## Features

Connection types:
- Network Sockets
- Websockets
- Local users (console, pipes, UDS)

Transport modes:
- **Line mode**: conventional MUD UX (e.g. `telnet`)
- **Char mode**: single character or virtual terminal control key
- **Structured messages**: for smart clients (e.g. graphical clients, AI agents, ... etc.)

Transport layer integrations:
- TELNET Support
- TLS Support

In-Process integration with the MUD server:
- Loaded as a shared library and provides in-process C APIs for transport layer
- Interact with the main server by registered hook functions
- Provides RAII guards and multithread-safety synchronizations as C++ wrappers

## How to build

### Configure
```bash
# See `cmake --list-presets` for supported platforms
cmake --preset linux-gcc
```

### Build
```bash
# Use `dev-*` preset for fast development build; or `rel-*` preset for optimized release build
cmake --build --preset dev-linux-gcc
```

### Running Tests
```bash
# Use corresponding `unit-*` preset for configured platform
ctest --preset units-linux-gcc
```
