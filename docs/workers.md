# Workers and Multi-threaded Applications

mudmux has one configured **worker pool** for asynchronous event-hook callbacks. The
event loop remains responsible for I/O; workers run application event-hook code when
relaxed concurrency is enabled. This division lets independent connections
make progress concurrently without letting two callbacks mutate the same slot
at once.

![Worker and execution model](images/workers-execution-model.svg)

## Configure concurrency

Set `transport.thread_pool.size` and, optionally, the per-slot continuation
queue size with `transport.thread_pool.backlog_capacity` in the YAML passed to
`mudmux_init()`:

```yaml
transport:
  thread_pool:
    size: 1
    backlog_capacity: 8
```

`size` defaults to `1` and `backlog_capacity` defaults to `8`; both must be at
least `1`.

| Size | Mode | Application behavior |
| --- | --- | --- |
| `1` | Strict | Event hooks execute on the event-loop thread with serialized behavior. |
| Greater than `1` | Relaxed | Eligible event hooks may run on pool workers; event hooks for different slots can overlap. |

Start with the default unless the application is prepared for concurrent
event-hook callbacks and nondeterministic ordering between different slots.

## What mudmux runs where

The event-loop thread always owns transport work: accepting connections,
reading and buffering bytes, parsing input, flushing output, and updating async
runtime registrations. It produces event-hook work but does not wait for workers to
finish before servicing other slots.

In strict mode, application event hooks execute inline on that event-loop
thread. In relaxed mode, these event hooks are eligible for worker execution:

- `HOOK_CONNECT`
- `HOOK_DISCONNECT`
- `HOOK_MESSAGE_INBOUND`
- `HOOK_MESSAGE_OUTBOUND`
- `HOOK_TRANSPORT_READY`
- `HOOK_PROMPT`
- `HOOK_TELNET_SUBNEG`

`HOOK_TIMER` and registered non-slot async events use a separate serialized
event lane. They do not consume a communication slot's execution state.
`HOOK_GARBAGE_COLLECTION` always remains inline on the event-loop thread.

## Player logic ordering

Think of a slot as one player connection and its logic-layer conversation.
Relaxed mode preserves the following rule for that conversation:

> mudmux never calls logic for two events from the same slot at the same time,
> and later player input is delivered only after the earlier input handler has
> finished.

For example, a player sends `get rope`, then immediately sends `inventory`.
The `inventory` handler does not run early or alongside the `get rope` handler.
The later input waits until the logic layer has finished adding the rope to the
player's possessions, so the player can see the rope in their inventory. This
lets the logic layer treat one player's commands as an ordered sequence without
adding a per-player lock just to protect that sequence.

The rule is per slot, not global. While one player's `look` handler is waiting,
another player's `say hello` handler can run on another worker. If both handlers
modify shared game state, such as a room's occupants or a global economy, the
logic layer must still synchronize that shared state.

### What happens while a player's handler is busy

Normal player input is held by mudmux until the current handler finishes; it is
not turned into an unbounded list of application callbacks. The same applies to
Telnet protocol messages. This keeps the logic layer from receiving a later
command before it has finished deciding the result of the earlier one.

Some logic-layer notifications can be requested explicitly for a slot while it
is busy, such as an outbound-message, transport-ready, or prompt event. mudmux
remembers a limited number of these follow-up notifications and delivers them
in request order after the current handler. Set that limit with
`transport.thread_pool.backlog_capacity`; it defaults to `8`. When the limit is
full, mudmux declines an additional follow-up notification rather than allowing
one busy connection to consume unlimited memory.

Connection setup and teardown retain the same conversation ordering. Input for
a new connection waits until its `HOOK_CONNECT` handler has finished choosing
its transport options. If the connection closes while another handler is
running, `HOOK_DISCONNECT` runs after that handler, once, so the logic layer can
perform its per-player cleanup in order. See [HOOK_CONNECT](hooks/HOOK_CONNECT.md).

## Worker thread-safety

The public comm APIs are thread-safe by contract. An event hook running on a
worker may call them directly to write output, change input settings, or close
a connection.

Logic-layer thread safety belongs to the application. In relaxed mode,
synchronize any game state shared between callbacks—such as worlds, player
maps, rooms, or caches—with the application's own mutexes, atomics, or queues.

## Pool lifecycle and public APIs

`mudmux_init()` validates the configuration and starts the worker pool.
`mudmux_run()` produces work for the existing pool; it does not create or stop
workers. `mudmux_deinit()` stops and joins workers before tearing down the rest
of mudmux.

`<mudmux/workers.h>` also provides lifecycle controls for applications that
intentionally stop and restart workers while mudmux remains initialized:

- `mudmux_workers_start()` starts a stopped configured pool and returns `false`
  if it is already running or cannot start.
- `mudmux_workers_stop()` waits for queued work, stops and joins workers, and
  clears scheduler state. Do not call it from a worker callback.
- `mudmux_workers_pool_size()` reports the number of live workers; it is zero
  after stop.
- `mudmux_workers_submit(work, completion)` runs detached application work on
  a worker, then runs its completion in a serialized non-slot lane with
  the non-registerable continuation-hook context `HOOK_COMPLETION` active.
  mudmux
  takes ownership of both `async_closure_t` values only when submission
  succeeds; neither closure has slot-ordering guarantees. Exceptions from
  either closure or its destructor are logged and contained. Work receives
  `ASYNC_CLOSURE_SCHEDULER_OK`; completion receives that message on success or
  `ASYNC_CLOSURE_SCHEDULER_FAILED` when work or its cleanup fails.
- `mudmux_workers_await(work, resume)` is a non-blocking, slot-held operation.
  It is valid only from the current `HOOK_TRANSPORT_READY`,
  `HOOK_MESSAGE_INBOUND`, or `HOOK_TELNET_SUBNEG` event hook, or from a
  `HOOK_RESUME` continuation hook. On success it records the request, lets
  that event or continuation hook return, then runs `work` on a worker.
  Same-slot inbound parsing remains deferred until `resume` runs through the
  slot gate with the non-registerable continuation-hook context `HOOK_RESUME`
  active. A disconnected slot cancels and destroys the resume closure; a
  reused slot is additionally protected by generation validation.

`HOOK_RESUME` and `HOOK_COMPLETION` are non-registerable continuation hooks:
execution contexts in which mudmux invokes closures, not application event
hooks. They cannot be passed to `mudmux_register_hook()`.

## Why this is not Python async/await

`mudmux_workers_await()` is intentionally a C ABI continuation contract, not a
language coroutine contract. This keeps the transport layer independent from
any particular scripting runtime while still giving MUD servers the ordering
behavior they usually need: one in-flight operation per player slot, deferred
same-slot input, and automatic cancellation when the slot disconnects or is
reused.

Python `async`/`await` suspends a coroutine frame. Local variables, the program
counter, exception state, and the logical call stack survive while the awaited
operation is pending, and cancellation is delivered back into that coroutine at
the `await` expression. That is a powerful application-language feature, but it
requires cooperation from the Python runtime and from awaitable libraries such
as async database drivers.

mudmux cannot provide that language-level suspension through a plain C event
hook alone. Once an event hook returns, its stack frame is gone. Instead,
`mudmux_workers_await()` lets the caller capture any interpreter state it owns
inside `async_closure_t` contexts. A scripting binding can store a coroutine,
fiber, VM continuation, explicit state-machine object, or pending command
record in the resume closure and continue it when mudmux invokes that
continuation hook with `HOOK_RESUME` active.

For example, if a command starts a database lookup and the same player sends
more input before the lookup finishes, mudmux keeps later same-slot input in
the transport buffers until the resume closure runs. If the player disconnects
or the slot is reused first, mudmux destroys the resume closure instead of
invoking it. The binding remains responsible for deciding how that maps into
its scripting runtime: resume a coroutine, raise a cancellation error, mark a
state machine canceled, or release a pending command object.

`mudmux_workers_submit()` has a different purpose. It runs detached work and a
serialized completion continuation hook with `HOOK_COMPLETION` active, but it
does not hold a slot, defer input, validate a slot generation, or imply any disconnect
cancellation. Use it for background jobs whose completion is not part of a
specific slot's inbound ordering contract.

`<mudmux/execution.h>` exposes `mudmux_is_running()`. It reports whether a
`mudmux_run()` call is active, not whether the worker pool exists. Use these
runtime-bound APIs after `mudmux_init()` and before `mudmux_deinit()`.

## Failure behavior and verification

Workers catch standard and non-standard C++ exceptions from submitted tasks,
log them, and continue processing later tasks. This prevents an uncaught
event-hook exception from terminating its worker, but it cannot recover from a process
fatal fault such as a segmentation fault or explicit process termination.

The regression suite covers same-slot ordering, progress on independent slots
while another slot is busy, queue-pressure deferral and resume, concurrent comm
API use from event hooks, worker-thread prompt and connect event hooks, and scheduler
stress. Sanitizer-backed stress testing remains useful for application-specific
shared-state designs.
