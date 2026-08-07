# Worker and Execution Model

mudmux uses one configured thread pool for all asynchronous hook work. The
pool supplies worker threads; the execution layer decides which hook work runs
there and coordinates its ordering.

![Worker and execution model](images/workers-execution-model.svg)

## Lifecycle

Configure the pool through `transport.thread_pool.size` in the YAML passed to
`mudmux_init()`. The default size is one and values below one are rejected.

`mudmux_init()` creates and starts the pool after configuration has been
validated. `mudmux_run()` is a work producer: it accepts transport and async
events, turns them into hook work, and submits eligible work to the already
running pool. It does not start or stop workers. `mudmux_deinit()` always stops
and joins the pool before it tears down the rest of mudmux.

`mudmux/execution.h` exposes the runtime-bound
`mudmux_execution_api_v1->is_running()` API (also available as the
`mudmux_is_running()` convenience macro) for code that needs to
determine whether a `mudmux_run()` call is currently active. It reports
event-loop execution state, not whether the worker pool is running. As with
the comm API package, use it after `mudmux_init()` and before `mudmux_deinit()`.

The public `mudmux/workers.h` header exposes `mudmux_workers_start()` and
`mudmux_workers_stop()` for explicitly stopping and restarting the configured
pool while mudmux remains initialized. Normally applications should rely on
the init/deinit lifecycle. `mudmux_workers_stop()` must not be called by a
worker callback. `mudmux_workers_pool_size()` returns the number of currently
live pool workers (zero after the pool is stopped).

## Responsibilities

The APIs reflect the split between pool management and execution coordination:

- `mudmux_workers_*` manages the pool configuration, lifecycle, worker count,
  and worker-thread identity.
- `mudmux_execution_*` selects asynchronous hooks, schedules slot and event
  work, and enforces execution ordering.

This distinction is intentional: workers are resources shared by all queued
work, while execution is the policy that coordinates that work.

## Modes and Ordering

`thread_pool.size: 1` selects strict mode. Hook execution stays on the event
loop thread, preserving the original serialized behavior.

A size greater than one selects relaxed mode. The execution layer may schedule
eligible hooks on pool workers. Hooks from different slots can run
concurrently, but a slot has at most one hook in flight. While an inbound hook
is running, further bytes for that slot remain in the transport buffers rather
than being decoded into another inbound message.

Explicit non-inbound dispatches may use a bounded continuation queue for the
same slot. Non-slot async events also use the pool in relaxed mode, but are
**serialized** with one another. This preserves their order without consuming a
slot's execution state.

Per-slot continuation queue behavior in relaxed mode:

| Hook type | Queued when same-slot hook is in flight? | Notes |
| --- | --- | --- |
| `HOOK_CONNECT` | Conditionally | Scheduler allows queueing, but normal accept flow starts with connect as the first slot hook. |
| `HOOK_DISCONNECT` | Yes | Queued in the slot bounded FIFO when dispatch is accepted. |
| `HOOK_MESSAGE_OUTBOUND` | Yes | Queued by `to_slot` target. |
| `HOOK_PROMPT` | Yes | Queued in the slot bounded FIFO. |
| `HOOK_MESSAGE_INBOUND` | No | Parser-originated inbound does not queue; it returns queue-full and parsing is deferred. |
| `HOOK_TELNET_SUBNEG` | No | Uses dedicated dispatch path; if slot is busy it returns queue-full and parsing is deferred. |

Concise `HOOK_CONNECT` ordering contract:

- Connect is the slot-initialization boundary for parser and transport mode setup.
- While connect is in flight, same-slot parser work is deferred (not queued).
- If connect requests close, disconnect progression is serialized after connect.
- Rare queued-connect cases are generation-checked before execution.

`HOOK_TIMER` and other non-slot async events do not use a slot continuation
queue. They run through the separate serialized event lane.

The event loop remains responsible for transport reads, writes, parser
progress, and runtime registration. Worker callbacks use the normal comm API
threading contract; logic-layer state that is shared across callbacks still
requires application-level synchronization in relaxed mode.

## Worker Fault Containment

Each worker catches exceptions thrown by a submitted task. mudmux logs the
exception and returns that worker to the task loop, preserving the configured
pool capacity. This is equivalent to immediately respawning the failed worker,
without allowing an exception to escape the thread and terminate the server.
The task that threw is abandoned; later queued work continues to run. This
protection covers standard and non-standard C++ exceptions. It cannot recover
from process-fatal faults such as a segmentation fault or explicit process
termination.
