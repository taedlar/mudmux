# mudmux

`mudmux` handles the painful **transport-layer** tasks (low-level network communication and asynchronous I/O) between MUD server and clients, generates **input events** for high-level logics layer, and enables graceful **non-blcoking** event loop to drive the virtual world.

![High-level mudmux architecture: clients connect through mudmux's async transport layer, which exchanges hooks and output with the MUD server.](docs/images/mudmux-overview.svg)

## Features

Connection types:
- Network Sockets
- Websockets
- Local users (console, pipes, UDS)

Transport modes:
- **Line mode**: conventional MUD UX (e.g. `telnet`)
- **Char mode**: single character or virtual terminal control key
- **Structured messages**: for smart clients (e.g. graphical clients, AI agents, ... etc.)

Input mode can be switched at runtime per-slot via the communication API (`comm_set_line_input`, `comm_set_char_input`, `comm_set_echo` in `mudmux/comm.h`). ANSI/VT100 output processing can be enabled per-slot with `comm_enable_virtual_terminal`.

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

# Examples

## Simple Chatroom

The [chatroom](examples/chatroom/) example server provides a simple demonstration for using `mudmux` to create a chatroom server.

Select the network transport with `--transport` (the default is `tls-telnet`,
which preserves the original example behaviour):

| Value | Protocol |
| --- | --- |
| `plaintext` | Plain TCP |
| `telnet` | TELNET over TCP |
| `tls-telnet` | TLS + TELNET |
| `ws` / `wss` | WebSocket over TCP / TLS |
| `ws-telnet` / `wss-telnet` | WebSocket with the TELNET subprotocol, over TCP / TLS |

TLS variants require the certificate and private-key settings in `mud.conf`.

To run a TLS variant, prepare a server certificate and private key (using
`openssl`, for example) and configure them in `mud.conf`.

For local development, [`mkcert`](https://github.com/FiloSottile/mkcert) can
create a certificate trusted by the local machine. From the directory that
contains `mud.conf`, run:

```bash
mkcert -install
mkcert -cert-file cert.pem -key-file key.pem localhost 127.0.0.1 ::1
```

On Windows, import the `rootCA.pem` from the directory printed by
`mkcert -CAROOT` into the OS **Trusted Root Certification Authorities** store
so browsers trust the locally generated `wss://` certificate.

You also need a telnet client with SSL support (on Ubuntu: `sudo apt-get install telnet-ssl`):
```bash
# start the example chatroom server
$ chatroom -f mud.conf --transport tls-telnet

# then, in another terminal, connect to the chatroom server
$ telnet -zssl localhost 4000
Trying 127.0.0.1...
SSL: Server has a self-signed certificate
SSL: unknown Issuer: /CN=localhost
Connected to localhost.
Escape character is '^]'.
Please enter your username: Annihilator

Welcome, Annihilator!
You are now logged in. Type your messages to chat withothers.
You can also use slash commands like /help, /quit, etc. to interact with the chatroom.
[Annihilator] /quit

Connection closed by foreign host.
```

### WebSocket client

The browser client at [`examples/websocket/index.html`](examples/websocket/index.html)
uses xterm.js and requests the TELNET WebSocket subprotocol. Start chatroom with
one of the WebSocket TELNET transports:

```bash
# Plain WebSocket
chatroom -f mud.conf --transport ws-telnet

# Or WebSocket over TLS (uses the certificate and key configured in mud.conf)
chatroom -f mud.conf --transport wss-telnet
```

Open `examples/websocket/index.html` directly in a browser, choose the matching
transport, and connect to
`ws://localhost:4000/` or `wss://localhost:4000/`. The `wss-telnet` option
requires the browser to trust the configured certificate; the `mkcert` commands
above set this up for local development.
