# mudmux

A generic transport-layer server for MUD development.

## Features (planned)

Transport types:
- Raw socket connections
- Websockets
- Support for TLS
- Local console (STDIO)
- Unix domain sockets / Pipes

Transport mode:
- **Line mode**: enable client-side editing
- **Char mode**: single character or virtual terminal control key
- **Structured message**: for smart clients (GUI, agents, ... etc.)

Collects user inputs and deliver to the MUD logics layer in their desired transport mode.

Line/Char/Message delivery as:
- Message queue (producer consumer)
- Flood control
- Out-of-band messages

## How to build

### Configure
```bash
cmake --preset linux-gcc
```

### Build
```bash
cmake --build --preset dev-linux-gcc
```

### Unit-tests
```bash
ctest --preset units-linux-gcc
```
