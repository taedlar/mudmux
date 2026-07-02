# Communication Layer (lib/comm)

The communication layer provides mudmux with a **unified interface for all I/O sources**: console/stdin, files, and network sockets. This enables the logic layer to process input uniformly through a slot-based routing system and hook-based event dispatch, regardless of the actual transport.

## Module Organization

### Core Components

| File | Purpose |
|------|---------|
| `abstract.h/cpp` | **Slot abstraction layer** — BIO-based unified I/O for all sources, slot lifecycle |
| `inbound.h/cpp` | **Hook dispatch** — Route all input to `MUDMUX_HOOK_MESSAGE_INBOUND` with metadata |
| `console.h/cpp` | **Console/stdin input** — Worker thread, real-time mode switching |
| `file_input.h/cpp` | **File input** — Async reader thread, queue-based buffering |
| `accept.h/cpp` | **Network sockets** — Listen, accept connections, dispatch inbound data |

### Threading Model

- **Main loop thread**: Processes events and invokes hooks
- **Console worker thread** (optional): Reads stdin/console in background
- **File input thread** (optional): Reads file in background, posts completions
- **Async runtime threads**: Handle network I/O (IOCP/epoll/poll)

Worker threads communicate with main loop via **thread-safe queues** and **completion events**.

## Slot-Based Architecture

Every input/output source is assigned a **slot** — a logical address within mudmux:

```
Slot 0: Primary transport
  ├─ Console/stdin input (if interactive)
  └─ OR File input (if testing/scripting)
  
Slot 1+: Network transports
  ├─ Listening on each configured address
  ├─ New connections flow through inbound hook
  └─ Multiple slots for multiple listen addresses
```

**Benefits:**
- Logic layer identifies message source by slot number
- No code changes needed to support new transports
- Multiplexing built-in: serve console + network simultaneously

## Unified I/O Abstraction

All I/O is abstracted as **OpenSSL BIO** pairs:

```c
// At each slot: read and write BIO
typedef struct {
    BIO* rbio;              // read stream (input source)
    BIO* wbio;              // write stream (output destination)
    void* context;          // transport-specific data
} comm_slot_t;

// Uniform API works for all transports:
BIO_read(rbio, buffer, len);    // Same API for file, socket, console
BIO_write(wbio, data, len);
```

**Supported BIO types:**
- **Memory BIO** (pipes, internal buffering)
- **File BIO** (file I/O)
- **Socket BIO** (network sockets)
- **Console BIO** (custom Windows/POSIX terminal)

## Hook-Based Input Routing

## Hook-Based Input Routing

All input events — regardless of source — are delivered via `MUDMUX_HOOK_MESSAGE_INBOUND`:

```c
// The logic layer only sees hooks, never the transport details
MUDMUX_HOOK_CONNECT          // New input session starts (slot=0 for console, 1+ for network)
MUDMUX_HOOK_MESSAGE_INBOUND  // Each line received, slot identifies source
MUDMUX_HOOK_MESSAGE_OUTBOUND // Data sent to output
MUDMUX_HOOK_ERROR            // Transport errors
```

**Key advantage:** Logic layer logic is transport-agnostic. The same code handles console, file, and network input without modification.

## Transport Types

### 1. Console/stdin Input (`console.cpp`)

**When used:** Interactive mode, real-time user commands

**Features:**
- Mode switching support (Windows ReadConsoleW, POSIX terminal modes)
- Reconnection after EOF (interactive console only, not pipes)
- Real-time line input

**Configuration:**
```bash
# Enable with flag
example_server --console

# Or auto-detect from stdin
example_server  # (will use console if TTY)
```

### 2. File Input (`file_input.cpp`)

**When used:** Automated testing, scripted input

**Features:**
- Async reading via dedicated thread
- Queue-based buffering (100 items × 4KB default)
- Slot encoding in completion keys (supports multi-file future)
- Graceful EOF detection and shutdown

**Configuration:**
```bash
# Specify input file
example_server --input input.txt

# Pipe from command
cat input.txt | example_server
```

**Key design:**
- Cannot rely on completion events (Windows file I/O synchronous)
- Must call `comm_process_file_input()` unconditionally every loop iteration
- Separate from console: exclusive choice, not both

### 3. Network Sockets (`accept.cpp`)

**When used:** Remote client connections

**Features:**
- Listen on configured addresses (e.g., port 4000)
- Async event-driven (IOCP/epoll/poll)
- Accept connections, dispatch inbound messages
- Multiple listen slots supported

**Configuration:** See `mud.conf` for listen address configuration

## Main Event Loop Integration

From `mudmux.cpp`, every iteration performs:

```c
// 1. Process console input (unconditional, safe if not initialized)
comm_process_console_input(runtime, allow_reconnect);

// 2. Process file input (if active)
if (comm_has_file_inputs())
    comm_process_file_input(runtime, slot, nullptr);

// 3. Process network I/O events
for (each event from async_runtime_wait) {
    if (listener event)
        comm_process_listener_event(runtime, slot, fd);
    else if (connection event)
        process_inbound_message(runtime, slot, data);
}
```

**Order matters:**
- Console/file must process before network to prevent starvation
- Console/file called unconditionally for queue draining (not event-triggered)
- Network events handled in main loop for scalability

## Configuration

### Command-Line Flags

```bash
# Console/stdin (interactive)
example_server --console          # Force console mode
example_server                    # Auto-detect (TTY = console)

# File input (automated testing)
example_server --input FILE       # Read from FILE
cat FILE | example_server         # Read from pipe

# Output routing
example_server --output FILE      # Write to FILE

# Network (configured in mud.conf)
# See network listen configuration in mud.conf
```

### Network Configuration

Network listen addresses are configured in `mud.conf`:

```yaml
listen:
  - address: "*"
    port: 4000
  - address: "127.0.0.1"
    port: 8000
```

Each listen address occupies a unique slot for inbound message routing.

## Usage Examples

### Interactive Console Mode

```bash
# Run server with interactive console input
example_server -f mud.conf

# Type commands interactively
telnet localhost 4000  # Can also connect remotely
```

### Automated Testing with File Input

```bash
# Create test input file
cat > input.txt << EOF
hello
world
test
EOF

# Run with file input, capture output
example_server -f mud.conf -i input.txt -o output.txt

# Verify output
cat output.txt
```

### Combined Console + Network

```bash
# Run server with console AND network listening
example_server -f mud.conf --console

# In another terminal, connect remotely
telnet localhost 4000

# In server terminal, type console commands
# All commands processed through same hook system
```

## Performance & Limits

| Metric | Value | Notes |
|--------|-------|-------|
| Queue size (console) | 100 lines | 4096 bytes per line |
| Queue size (file) | 100 lines | 4096 bytes per line |
| Console latency | 10-50ms | Per line, OS dependent |
| File latency | <1ms | Main loop iteration latency |
| Network latency | <1ms | Async event-driven |
| Overflow policy | DROP_OLDEST | Recent data preserved |

**Tuning:**
- Queue sizes hardcoded (see `console.cpp`, `file_input.cpp`)
- Modify `async_queue_create(100, 4096, ...)` to adjust
- Monitor logs for "queue full" warnings

## Slot Abstraction Benefits

1. **Transport-agnostic logic layer** — No code changes to support new transports
2. **Multiplexing** — Handle console + network simultaneously
3. **Testing** — File input for automated testing without code changes
4. **Scalability** — Easy to add new listen addresses
5. **Routing** — Slot number identifies all message sources

## Debugging

**Common questions:**

- **"Where did my input go?"** — Check slot routing. Run with `-VV` flag to see debug logs.
- **"Why is console slow?"** — Windows mode switching, expected 10-50ms per line.
- **"File input stops after first line?"** — Check queue isn't full or EOF detected early.
- **"Can I use console and file at the same time?"** — No, exclusive choice. Use file for testing.

**Debug flags:**
```bash
example_server -VV          # Very verbose logging
example_server -VV -i test.txt  # Verbose + file input
```

For detailed implementation and testing guidance, see [AGENTS.md](AGENTS.md).
