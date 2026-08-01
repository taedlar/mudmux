# Thread Pool Execution Design

## Purpose

This document describes the current thread-pool execution design in mudmux.
It is a runtime policy that preserves deterministic behavior by default and enables concurrent hook execution when explicitly configured.

## Configuration

Thread-pool mode is configured through mudmux_init YAML.

```yaml
transport:
  thread_pool:
    size: 1
```

Rules:

- Missing value defaults to size 1.
- Size less than 1 is rejected during initialization.
- Size 1 selects strict deterministic mode.
- Size greater than 1 selects relaxed concurrent mode.

## Runtime Policy

mudmux exposes two determinism modes:

- strict: selected when thread_pool.size equals 1
- relaxed: selected when thread_pool.size is greater than 1

In strict mode, behavior remains equivalent to single-thread event-loop semantics.
In relaxed mode, hooks may run concurrently, with strict FIFO maintained per slot.

## Execution Model

The main event loop remains authoritative for:

- inbound refill
- outbound flush
- async runtime registration updates

Hook execution is routed through the execution subsystem:

- each slot has a bounded queue
- each slot queue preserves FIFO order
- a slot is drained by at most one active worker at a time
- independent slots can be processed in parallel in relaxed mode

Current queue behavior:

- per-slot queue capacity is fixed at 8 tasks
- queue-full returns MUDMUX_DISPATCH_QUEUE_FULL
- normal enqueue returns MUDMUX_DISPATCH_OK
- invalid runtime state or scheduling failure returns MUDMUX_DISPATCH_ERROR

## Telnet Subneg Backpressure

Telnet subnegotiation dispatch uses the same per-slot queue.
When the slot queue is full, one pending subneg payload is retained per slot and retried later.
This keeps subneg delivery ordered while avoiding unbounded queue growth.

## Hook Dispatch Policy

Asynchronous dispatch in relaxed mode is enabled for:

- HOOK_CONNECT
- HOOK_DISCONNECT
- HOOK_MESSAGE_INBOUND
- HOOK_PROMPT
- HOOK_TELNET_SUBNEG

Synchronous behavior is retained in strict mode.

## Public Comm API Threading Contract

Public comm APIs are slot-based and exposed via mudmux_comm_api_v1.
Calls are allowed from:

- the logic thread
- worker threads owned by the execution pool

Calls from other threads are rejected by the thread guard.

This contract enables concurrent hook callbacks in relaxed mode without requiring logic-layer code to take a global hook lock.
The comm layer is responsible for internal synchronization of slot and buffer state.

## Ordering and Safety Guarantees

Guaranteed:

- strict mode preserves deterministic ordering semantics
- relaxed mode permits one hook in flight per slot; later inbound bytes remain
  in transport buffers until it returns
- no global hook serialization in relaxed mode
- queue-full is explicit and observable by callers

Not guaranteed in relaxed mode:

- global cross-slot ordering

## Internal Locking Notes

The comm and execution layers use scoped synchronization to protect slot state and queue state.
Key invariants:

- avoid lock-order inversions
- keep critical sections short
- do not hold a slot lock while waiting for another slot lock

For internal comm helpers that operate on slot state, use a stack-scoped comm_abstract_ptr reference contract.

## Operational Defaults

Recommended default configuration:

- thread_pool.size: 1

Use size greater than 1 only when the logic layer is prepared for concurrent hook callbacks and non-deterministic cross-slot interleavings.

## Test Coverage Summary

Current regression coverage includes:

- CommInboundTest.ThreadPoolKeepsPerSlotOrderWhileOtherSlotsAdvance
- CommInboundTest.RelaxedModeCommApiCallsFromConcurrentHooksDoNotDeadlock
- CommInboundTest.RelaxedModeQueuePressureOnOneSlotDoesNotBlockOtherSlots
- CommInboundTest.RelaxedModeConcurrentEnqueueAndCommApiMutationsRemainStable
- MudmuxStdinThreadPoolTest.InboundQueueFullDefersAndResumesInFifoOrder
- MudmuxStdinThreadPoolTest.PromptHookRunsOnWorkerThreadInRelaxedMode
- MudmuxStdinThreadPoolTest.ConnectHookFiresForConsoleInRelaxedMode

## Known Follow-up Hardening

Unit-level race and deadlock regressions are in place.
Optional CI hardening can add sanitizer-focused jobs such as TSAN for broader coverage under long-running stress conditions.
