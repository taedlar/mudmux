# HOOK_GARBAGE_COLLECTION

`HOOK_GARBAGE_COLLECTION` provides a periodic safe point for maintenance of
the logic layer's simulated world, such as incremental garbage collection.

Function signature:

~~~cxx
int hook_garbage_collection(void* context, int msg, void* data, size_t size);
~~~

## Invocation

mudmux invokes this hook once at the end of each completed event-loop
iteration: after transport events, deferred inbound processing, and prompt
dispatch, and before the loop advances buffered outbound writes. When this
hook is registered, mudmux also limits an otherwise-idle event-loop wait to a
keep-alive interval, so the hook continues to run without I/O.

The keep-alive interval defaults to 20 seconds and is configured in
`mudmux_init()` YAML as `transport.keep_alive_interval` (in seconds). It must
be at least one second. I/O and other events may cause earlier invocations, so
applications must not use one invocation as a measure of elapsed time.

The hook is invoked directly, not dispatched through the execution pool. Its
return value is ignored.

## Arguments

The hook is global rather than slot-specific, so mudmux supplies the runtime
context and fixed non-slot values:

- `context` is the user-defined context supplied to `mudmux_run()`.
- `msg` is `-1`.
- `data` is `nullptr`.
- `size` is `0`.

## Threading contract

The callback always runs on the event-loop thread: the thread that called
`mudmux_run()`. This remains true when `transport.thread_pool.size` is greater
than one. It never runs on an execution-pool worker thread.

As a callback on the logic/event-loop thread, it may use the public slot-based
comm API. In relaxed mode, worker-thread hooks for other slots may be running
at the same time, so logic-layer state shared with those hooks must still be
synchronized.

## Performance expectations

The callback executes inline in the event loop. Keep each invocation short and
incremental: a long-running collection pauses I/O progress, hook scheduling,
and outbound flushing for all connections. For substantial work, retain GC
state in the logic layer and process it in bounded portions on successive
invocations.
