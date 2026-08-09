# Event hooks and the event loop

mudmux integrates the hosted logic layer with its asynchronous I/O loop through
hooks. A hook is a C callback with this common signature:

```c
int hook(void *context, int msg, void *data, size_t size);
```

`context` is the pointer passed to `mudmux_run()`. The meanings of the other
arguments depend on the hook type. Register one callback for a built-in hook
with `mudmux_register_hook()` before starting the runtime.

```c
#include <mudmux/mudmux.h>

static int on_input(void *context, int slot, void *data, size_t size) {
    /* Process size bytes from communication slot. */
    return 0;
}

/* After mudmux_init(), before mudmux_run(). */
mudmux_register_hook(HOOK_MESSAGE_INBOUND, on_input);
```

The callback return value is returned by `mudmux_invoke_hook()` when that API
is used directly. The event loop currently does not use a hook return value to
control I/O; callbacks should use the public comm API for actions such as
writing output, changing input mode, or closing a slot.

## Where hooks run in an iteration

Each iteration waits for async-runtime readiness, then processes work in this
order:

```text
wait for I/O, async events, or the GC keep-alive timeout
    |
    +-- console and file-input completions
    +-- registered async events
    +-- listener, read, write, close, and error readiness for slots
    +-- resume deferred inbound parsing
    +-- prompt hooks
    +-- garbage-collection hook
    +-- advance buffered writes and transport state
```

The transport processing path turns connection lifecycle and decoded input into
slot hooks. For inbound data, the layers are:

```text
transport read -> TLS decrypt -> WebSocket decode -> Telnet parse
    -> line/character parse -> HOOK_MESSAGE_INBOUND
```

This means application hooks receive parsed application bytes, rather than raw
socket, TLS, or WebSocket framing bytes. See [inbound.md](inbound.md) for the
full parsing and ordering contract.

## Built-in hook events

| Hook | Event-loop integration | `msg`, `data`, and `size` |
| --- | --- | --- |
| `HOOK_CONNECT` | A transport destination is added to a slot. | `msg` is the slot; `data` names the configured transport entry. |
| `HOOK_DISCONNECT` | A slot is closed; dispatched once for its lifecycle. | `msg` is the slot; `data == NULL` and `size == 0`. |
| `HOOK_MESSAGE_INBOUND` | A complete input unit has passed the enabled transport parsers. | `msg` is the source slot; `data` is the non-null-terminated payload. |
| `HOOK_MESSAGE_OUTBOUND` | `comm_add_message()` sends an application message. | `msg` is the destination slot; `data` is an immutable message copy. |
| `HOOK_TRANSPORT_READY` | The selected transport framing is ready for application input. It fires once before the first inbound application message. | `msg` is the slot; `data == NULL` and `size == 0`. |
| `HOOK_PROMPT` | Inbound work has drained for a slot with prompts enabled and no pending output. | `msg` is the slot; `data == NULL` and `size == 0`. |
| `HOOK_TELNET_SUBNEG` | A Telnet subnegotiation is parsed. | `msg` is the Telnet option; `data` and `size` are its payload. |
| `HOOK_TIMER` | mudmux's internal timer event is signalled. | `msg` is supplied to `mudmux_trigger_timer()`; no payload. |
| `HOOK_GARBAGE_COLLECTION` | End of every completed loop iteration. | `msg == -1`, `data == NULL`, and `size == 0`. |

The hook-specific documents in [hooks/](hooks/) define the detailed argument
and protocol contracts for the hooks that have one.

`HOOK_GARBAGE_COLLECTION` is special: it always executes inline on the event
loop thread, after prompt dispatch and before outbound flushes. When it is
registered, an idle loop also wakes at `transport.keep_alive_interval` (20
seconds by default). Keep this hook brief; it delays all I/O progress.

## Custom async events

Use `mudmux_register_event()` to make an `async_event_t` wake `mudmux_run()`.
This is useful when another thread has completed work that must be handled by
the logic layer. `async_queue_t` is the thread-safe message channel for this
pattern: the event is a wake-up notification, while the queue carries the
messages between an event hook, transport hooks, and external producers.

1. Call `mudmux_init()` so the async API is available.
2. Initialize an `async_event_t` with `async_event_init()`.
3. Create an `async_queue_t` sized for the messages your hooks exchange.
4. Register the event, before `mudmux_run()`, with a non-null callback.
5. Enqueue a message, then signal the event with `async_event_set()`.
6. Destroy the event and queue only after `mudmux_deinit()` returns.

```c
#include <mudmux/comm.h>
#include <string.h>

static async_event_t work_ready;
static async_queue_t *work_queue;

enum { WORK_QUEUE_CAPACITY = 64, WORK_MAX_MESSAGE = 1024 };

/* The queue owns a copy of this fixed-size message, not data from a hook. */
struct work_message {
    int slot;
    size_t size;
    char data[WORK_MAX_MESSAGE];
};

static int drain_completed_work(void *context, int msg, void *data, size_t size) {
    (void)context;
    (void)msg;   /* Custom events use -1. */
    (void)data;  /* Custom events have no payload. */
    (void)size;

    struct work_message work;
    size_t received;
    while (async_queue_dequeue(work_queue, &work, sizeof(work), &received)) {
        if (received != sizeof(work) || work.size > WORK_MAX_MESSAGE)
            continue; /* Reject malformed application messages. */

        /* Route this result through HOOK_MESSAGE_OUTBOUND, if registered. */
        comm_add_message(work.slot, work.data, work.size);
    }
    return 0;
}

static int on_message_inbound(void *context, int slot, void *data, size_t size) {
    (void)context;
    if (!data || size > WORK_MAX_MESSAGE)
        return 0;

    struct work_message work;
    work.slot = slot;
    work.size = size;
    memcpy(work.data, data, size);
    if (async_queue_enqueue(work_queue, &work, sizeof(work)))
        async_event_set(&work_ready); /* Ask the event loop to drain work. */
    return 0;
}

bool start_server(void *context) {
    if (!mudmux_init(NULL))
        return false;
    work_queue = async_queue_create(
        WORK_QUEUE_CAPACITY, sizeof(struct work_message), 0);
    if (!work_queue || !async_event_init(&work_ready, true, false))
        return false;
    if (!mudmux_register_hook(HOOK_MESSAGE_INBOUND, on_message_inbound) ||
        !mudmux_register_event(&work_ready, drain_completed_work))
        return false;
    return mudmux_run(context) == 0;
}

/* An external producer uses the same queue, then calls async_event_set(). */

/* After mudmux_run() has returned: */
mudmux_deinit();
async_event_destroy(&work_ready);
async_queue_destroy(work_queue);
```

When the event becomes ready, mudmux acknowledges it before dispatching its
callback. A manual-reset event is reset; an auto-reset event is consumed by
that delivery. In both cases, a signal that arrives while the callback runs
remains pending for a later loop iteration. Signals are notifications, not a
counted work queue: repeated sets can coalesce, so callbacks should drain an
application-owned queue rather than expect one callback per produced item.

For this single-consumer callback model, either reset mode is suitable. Use a
manual-reset event when the signalled condition must also remain observable to
other waiters until it is explicitly reset. Use an auto-reset event for a
one-consumer wake-up; mudmux consumes one signal per callback dispatch.
The queue stores copies of messages up to its configured maximum size; do not
enqueue a pointer to an inbound-hook payload, because that payload is only
valid for the duration of the callback.

The example uses no queue flags, so a full queue fails immediately instead of
blocking the event loop or a hook. Choose `ASYNC_QUEUE_DROP_OLDEST` only when
loss is acceptable. `ASYNC_QUEUE_BLOCK_WRITER` is generally inappropriate for
an event-loop callback because it can stall I/O progress.

Each event may be registered once, and registrations cannot be added after
`mudmux_run()` has begun. The event storage must remain initialized through
`mudmux_deinit()` because the runtime continues to hold its registration until
then.

## Timer event

mudmux creates and registers one timer event during `mudmux_init()`. A timer
hook also receives two synchronous lifecycle notifications on the thread that
called `mudmux_run()`:

- `msg == 0` once setup has succeeded, immediately before event-loop I/O
  dispatch begins.
- `msg == -1` once the loop has stopped, immediately before `mudmux_run()`
  tears down its runtime and returns.

Both lifecycle callbacks have `data == NULL` and `size == 0`. They run inline
even in relaxed mode, so the start notification completes before I/O dispatch
and the stop notification is guaranteed before the call returns.

For application timer work, call `mudmux_trigger_timer(msg)` to wake the loop
and deliver that message:

```c
static int on_timer(void *context, int msg, void *data, size_t size) {
    (void)context;
    (void)data;
    (void)size;
    /* Handle timer reason msg. */
    return 0;
}

mudmux_register_hook(HOOK_TIMER, on_timer);
mudmux_trigger_timer(42); /* Application messages must not use 0 or -1. */
```

Timer signals also coalesce. If several calls happen before the event-loop
dispatch, the callback observes the most recently stored `msg`; use an
application queue when every timer request must be retained. Values `0` and
`-1` are reserved for the start and stop notifications, respectively;
`mudmux_trigger_timer()` rejects them.

## Threading and ordering

With `transport.thread_pool.size: 1` (the default), hooks execute on the event
loop thread. mudmux holds its logic-layer comm mutex while invoking them, so
comm API calls made by the callback are serialized with slot state.

With a pool size greater than one, mudmux uses relaxed mode. Slot-bound hooks
may run on worker threads and hooks for different slots may run concurrently.
Only one hook is in flight for a slot; parser-originated inbound input remains
in the transport buffers until that hook finishes. Do not rely on ordering
between different slots, and synchronize logic-layer state shared by callbacks.

### Current parser slot

`mudmux_comm_api_v1->current_slot()` (normally used as
`comm_current_slot()`) returns the callback's current communication slot. It
is available for `HOOK_CONNECT`, `HOOK_MESSAGE_INBOUND`, `HOOK_PROMPT`, and
`HOOK_TELNET_SUBNEG`, including when those callbacks run on relaxed-mode
workers. It returns `-1` in every other hook, when called outside a hook, and
for explicit `mudmux_invoke_hook()` calls. The value is thread-local and is
restored when the callback returns; do not retain it as a substitute for the
slot argument supplied to a hook.

Registered custom events and `HOOK_TIMER` are global rather than slot-bound.
In relaxed mode their callbacks are dispatched through the execution pool but
are serialized with one another. `HOOK_GARBAGE_COLLECTION` remains on the
event-loop thread in both modes and can overlap worker-thread callbacks in
relaxed mode.

Callbacks should complete promptly. Blocking a strict-mode callback blocks the
event loop; blocking a relaxed-mode callback occupies an execution worker and
can delay later work for its slot or global event hooks.
