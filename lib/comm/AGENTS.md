# Communication Layer (lib/comm) — Agent Guide

## Module Overview

The communication layer (`lib/comm/`) provides the transport-agnostic infrastructure for mudmux to handle multiple concurrent input sources (console, files, network sockets) through a unified abstraction.

**Core responsibilities:**
1. **Slot abstraction** (`abstract.cpp`): OpenSSL BIO-based unified I/O for all transport types
2. **Hook dispatch** (`inbound.cpp`): Route all input events to hooks with metadata
3. **Console processing** (`console.cpp`): Interactive stdin/console input with mode switching
4. **File input** (`file_input.cpp`): Async file reading via dedicated thread
5. **Socket handling** (`accept.cpp`): Listen, accept, and inbound message routing

## Architecture: Slot Abstraction Layer

### Concept: Slots

All communication is slot-based. Each slot represents an input/output pair:

```
Slot 0 (Primary):
  - Console/stdin input + stdout/output
  - OR file input + file output
  - (exclusive choice, not both)

Slot 1+ (Transport Slots):
  - Each listening address gets a slot (e.g., :4000 on slot 1)
  - Accepted connections don't get slots; they route to existing slots or discard
```

### Implementation

**Slot state** (`abstract.h`):
```c
typedef struct comm_abstract_s comm_abstract_t;  // Opaque struct
```

Slots are accessed exclusively through opaque `comm_abstract_t` structs via public APIs. Internal implementation manages:
- **BIO pairs**: read and write BIO objects for each slot
- **Transport context**: transport-specific state
- **Slot mapping**: dynamic array of active slots

**BIO types supported:**
- **Memory BIO** (for pipes, internal)
- **File BIO** (for file I/O)
- **Socket BIO** (for network)
- **Console BIO** (custom, Windows/POSIX)

### Key API (`abstract.h`)

```c
// Register I/O sources at a slot
int comm_abstract_add_bio(BIO *rbio, BIO *wbio, int slot);
int comm_abstract_add_file(const char *fn_in, const char *fn_out, int slot);

// Query state
BIO* comm_abstract_get_rbio(int slot);
comm_abstract_t* comm_abstract_get(int slot);
int comm_is_listener(comm_abstract_t *comm);

// Read/write operations
int comm_read(comm_abstract_t *comm, void *buf, size_t len);
int comm_write(comm_abstract_t *comm, const void *buf, size_t len);
void comm_flush(comm_abstract_t *comm);

// Lifecycle
int comm_abstract_remove(int slot);
void comm_abstract_cleanup(void);
```

## Architecture: Hook Dispatch System

All input events flow through hooks via `comm_invoke_inbound_message()` (`inbound.cpp`):

```c
void comm_invoke_inbound_message(async_runtime_t *runtime, int slot, 
                                 const char *data, size_t len)
{
  // 1. Construct context from slot
  // 2. Invoke MUDMUX_HOOK_MESSAGE_INBOUND with slot number
  // 3. Logic layer receives: context=transport context, slot=source identifier
}
```

**Hook delivery guarantees:**
- Slot number always identifies the source
- Data is line-buffered (null-terminated)
- Called in main loop context (not from worker threads)

### Inbound Message Processing API (`inbound.h`)

```c
int comm_invoke_inbound_message(async_runtime_t* runtime, int slot, 
                                const void* data, size_t size);
int comm_process_input(async_runtime_t* runtime, const io_event_t* event, int slot);
```

- `comm_invoke_inbound_message()`: Invokes `MUDMUX_HOOK_MESSAGE_INBOUND` for data received on a slot
- `comm_process_input()`: Route a transport event (socket, etc.) to message processing

## Key Architecture

### Components

**file_input.h / file_input.cpp:**
- Provides async file input processing via a dedicated reader thread
- Uses OpenSSL BIO abstraction layer (same as sockets/console)
- Communicates with main loop through thread-safe queue and completion events

### Static State

```c
static async_queue_t* file_input_queue      // Shared queue for buffered lines
static std::thread* file_input_thread       // Reader thread handle
static bool file_input_eof                  // EOF detection flag
static std::mutex file_input_mutex          // Thread synchronization
```

Source of truth: **file_input_queue existence** indicates active file input (`comm_has_file_inputs()`)

### Completion Key Encoding

File input completion keys encode the slot number:

```c
FILE_INPUT_COMPLETION_KEY(slot) = (0xFFF0 << 16) | (slot & 0xFFFF)
```

- Marker: `0xFFF0` in upper 16 bits
- Slot: stored in lower 16 bits, supports up to 65,536 slots
- Decoding: `FILE_INPUT_SLOT_FROM_KEY(key)` extracts slot



## Transport Types

### 1. Console Input (`console.cpp`)

**Usage:**
- Interactive real-time input from stdin or real console
- Enabled by `mudmux_enable_standard_input()` or `mudmux_enable_console()`

**Architecture:**
- Worker thread reads stdin/console
- Thread-safe queue buffers lines
- Main loop drains queue and invokes hooks

**Key constraints:**
- Must call `comm_process_console_input()` every iteration (Windows mode switching)
- Returns early if no console initialized (safe to call unconditionally)

**API** (`console.h`):
```c
bool comm_init_console(async_runtime_t *runtime);
void comm_shutdown_console(async_runtime_t *runtime);
int comm_process_console_input(async_runtime_t *runtime, bool allow_reconnect);
```

### 2. File Input (`file_input.cpp`)

**Usage:**
- Non-blocking file reading for testing/scripting
- Enabled by `--input` flag at startup
- One active file input per slot at a time

**Architecture:**
- Dedicated reader thread (one per active slot)
- Thread-safe queue buffers lines
- Completion events wake main loop (not primary trigger)
- Slot encoded in completion key

**Key constraints:**
- **Cannot rely on completion events**: Windows file I/O is synchronous; data enqueued before event posted. Main loop must drain queue every iteration.
- **File BIO limitation**: `BIO_get_fd()` fails on Windows file BIOs. Solution: dedicated thread with sync reads.
- **Must process before I/O loop**: Queue must be drained before transport I/O processing.

**Completion key encoding:**
```c
FILE_INPUT_COMPLETION_KEY(slot) = (0xFFF0 << 16) | (slot & 0xFFFF)
// Marker: 0xFFF0 in upper 16 bits
// Slot: lower 16 bits (supports up to 65,536 slots)

// Decoding
int slot = FILE_INPUT_SLOT_FROM_KEY(key);
bool is_file_event = IS_FILE_INPUT_COMPLETION_KEY(key);
```

**API** (`file_input.h`):
```c
bool comm_init_async_file_input(async_runtime_t *runtime, int slot);
int comm_process_file_input(async_runtime_t *runtime, int slot, const io_event_t* event);
bool comm_has_file_inputs(void);
void comm_shutdown_async_file_input(void);
```

### 3. Network Sockets (`accept.cpp`)

**Usage:**
- Listen for incoming connections on configured addresses
- Accept connections, route inbound data to hooks
- Multiple listening slots supported

**Architecture:**
- Listener created per listening address
- Async event-driven (IOCP/epoll/poll)
- Connections flow through inbound message hook

**API** (`accept.h`):
```c
int comm_accept(async_runtime_t* runtime, const char* accept_name);
int comm_process_listener_event(async_runtime_t* runtime, int listener_slot, socket_fd_t event_fd);
```

## Data Flow Diagram

```
Input Sources:
  [Console] → worker thread → queue ──┐
  [File]    → reader thread  → queue ──┼→ Main Loop
  [Socket]  → async events   ────────────┤
                                         ├→ comm_process_*_input()
                                         └→ comm_invoke_inbound_message()
                                              ↓
                                           [Hook dispatch]
                                              ↓
                                           [Logic layer]
```

## Main Loop Integration

From `mudmux.cpp`:

```c
// Every iteration:

// 1. Process console input (unconditional, safe if not initialized)
comm_process_console_input(runtime, enable_console);

// 2. Process file input (if active, drain queue before events)
if (comm_has_file_inputs())
    comm_process_file_input(runtime, COMM_SLOT_CONSOLE, nullptr);

// 3. Process transport I/O events
for (int i = 0; i < num_events; ++i) {
    // Skip console/file completion keys (already processed)
    if (IS_FILE_INPUT_COMPLETION_KEY(event.fd) || 
        event.fd == CONSOLE_COMPLETION_KEY)
        continue;
        
    // Process listener events, socket I/O, etc.
    ...
}
```

## Critical Design Constraints

### 1. Cannot Rely on Completion Events for File Input Queue Draining

**Problem**: On Windows, file I/O completes synchronously. Reader thread enqueues data and posts completion in rapid succession. Race condition: completion event may not arrive before queue fills.

**Solution**: Main loop calls `comm_process_file_input()` **unconditionally every iteration**, regardless of events. Queue draining is decoupled from event arrival.

### 2. File BIO Limitations on Windows

**Problem**: `BIO_get_fd()` fails on Windows file BIOs — cannot register with IOCP for async notification.

**Solution**: Dedicated reader thread with synchronous `BIO_read()` calls, mimics async behavior via queue and completion posting.

### 3. Console Must Process Every Iteration

**Constraint**: `comm_process_console_input()` must be called before the transport I/O event processing loop, even when zero events are returned from `async_runtime_wait()`.

**Rationale**: Windows `ReadConsoleW()` mode switching requires unconditional draining. Similar to file input but for Windows console specifically.


## Testing Strategy

### Unit Tests

**Console input:**
- Verify mode switching (no hangs on Windows)
- Verify EOF handling and reconnection

**File input:**
- Verify queue doesn't lose data on fast EOF
- Verify slot encoding in completion keys
- Verify multi-slot isolation

**Slot abstraction:**
- Verify BIO lifecycle (add/remove/cleanup)
- Verify context association

### Integration Tests

**Console + file input:**
```bash
# Test file input routing
echo -e "cmd1\ncmd2\ncmd3" > input.txt
example_server --input input.txt --output output.txt

# Verify output contains all commands
```

**Network + console:**
```bash
# Run server listening
example_server -i

# In another terminal, telnet and send commands
telnet localhost 4000
```

**Multi-slot file inputs** (if supported):
- Verify each slot processes independently
- Verify slot numbers routed correctly to hooks

### Performance Testing

- Queue saturation: rapid EOF scenarios
- Thread contention: multiple readers
- Event loop latency: measure iteration time

## Debugging Guide

### Common Issues

**"File data missing"**
- Check: Is `comm_process_file_input()` called unconditionally?
- Check: `comm_has_file_inputs()` returns true?
- Check: Log "file input reader thread started for slot X"?
- Check: Queue overflow? (ASYNC_QUEUE_DROP_OLDEST policy)

**"Console hangs"**
- Check: Mode switching code executing? (Windows ReadConsoleW)
- Check: `comm_process_console_input()` called every iteration?
- Check: Worker thread alive? (Not deadlocked on queue mutex)

**"Wrong slot in hook"**
- Check: Completion key encoding correct? (FILE_INPUT_COMPLETION_KEY macro)
- Check: Slot extracted correctly? (FILE_INPUT_SLOT_FROM_KEY)
- Check: Context_to_slot() mapping valid for socket events?

### Debug Log Points

```
[abstract.cpp]    "adding BIO at slot X"
[console.cpp]     "console worker thread started"
[console.cpp]     "console user disconnected"
[file_input.cpp]  "file input reader thread started for slot X"
[file_input.cpp]  "file EOF detected for slot X"
[accept.cpp]      "listening transport *:XXXX registered (slot=X)"
[inbound.cpp]     "invoking hook MESSAGE_INBOUND for slot X"
```

## Performance Characteristics

| Transport | Latency | Throughput | Notes |
|-----------|---------|-----------|-------|
| Console | 10-50ms | ~1000 lines/sec | OS dependent, mode switching overhead |
| File | <1ms | >100k lines/sec | Limited by disk I/O, queue draining |
| Socket | <1ms | >100k lines/sec | Async event-driven, minimal overhead |

**Queue configuration:**
- Size: 100 items × 4096 bytes = ~400KB per queue
- Policy: ASYNC_QUEUE_DROP_OLDEST (preserves recent data)
- Overflow: Logged as warning, old entries discarded

## Thread Safety

**Protected by file_input_mutex:**
- file_input_queue
- file_input_thread
- file_input_eof

**Protected by console_mutex:**
- console_queue
- console_ctx
- console_type

**Async-safe completion posting:**
- `async_runtime_post_completion()` thread-safe
- Used to wake main loop from worker threads

## Future Enhancements

1. **Per-slot queues**: Support true simultaneous multi-file inputs
2. **Backpressure**: Pause reading when queue full, instead of dropping
3. **Configurable queue sizes**: Parameterize 100/4096 defaults
4. **Metrics**: Queue depth, dropped lines, thread latencies
5. **Hot-swapping**: Replace file input on the fly without shutdown
