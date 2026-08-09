# HOOK_DISCONNECT

`HOOK_DISCONNECT` is the terminal lifecycle callback for a communication slot.
It runs once when a slot begins closing, before mudmux removes the slot and
releases its transport resources.

```c
int hook_disconnect(void *context, int slot, void *data, size_t len);
```

`context` is the pointer supplied to `mudmux_run()`. `slot` is the closing
communication slot. `data` is `NULL` and `len` is zero.

The event loop does not interpret the return value. Use the hook for
logic-layer cleanup, such as removing a player associated with the slot.

```c
static int on_disconnect(void *context, int slot, void *data, size_t len) {
    (void)context;
    (void)data;
    (void)len;

    remove_session_for_slot(slot);
    return 0;
}
```

## Requesting a close

Use `comm_close(runtime, slot)` from a hook or other runtime code to begin a
close. Passing `NULL` for `runtime` resolves the current runtime, which is the
normal form inside a hook:

```c
if (command_is_quit(message))
    (void)comm_close(NULL, slot);
```

The first request marks the slot closing and dispatches `HOOK_DISCONNECT`.
Subsequent requests advance the pending close; they do not dispatch the hook
again. A close initiated by transport EOF/error follows the same lifecycle.
Calling `comm_close()` for an already removed or invalid slot is successful and
does not invoke a hook.

`comm_close()` returns `true` only when the slot has been removed (or was
already gone). It returns `false` while work remains, including an asynchronous
disconnect callback, buffered output, the WebSocket close handshake, or
console EOF teardown. The event loop continues the close automatically; callers
normally do not need to retry it.

## Close ordering

The disconnect callback happens before transport teardown. It may queue a final
message with `comm_add_message(slot, ...)`; mudmux normally flushes that
message before closing an ordinary socket. Pending output is dropped instead
when TLS has not completed its handshake, because it cannot be flushed safely.
Once closing begins, prompt delivery is disabled and no further decoded input
is retained.

If no outbound hook is registered, `comm_add_message()` falls back to
`comm_buffered_write()`. If `HOOK_MESSAGE_OUTBOUND` is registered, this keeps
disconnect output on the outbound-hook path.

For WebSocket slots that completed their upgrade, mudmux sends a normal Close
frame after buffered application output drains, then waits for the peer Close
or a transport-level termination. For an established TLS slot, it makes a
best-effort TLS shutdown before removal. The console is special: closing it
signals console EOF and its final removal is coordinated by the console/input
worker; pending console output is discarded rather than deferred.

In relaxed mode, the callback can run on a worker. The slot remains allocated
and cannot be reused until that callback completes. If another same-slot hook
is active when a close is requested, the terminal disconnect transition is
preserved and runs after the active hook; it is not lost due to the normal
per-slot queue limit.

Do not retain `data` (there is no payload) or slot-owned internal state beyond
the callback. Synchronize any application state shared with hooks for other
slots in relaxed mode.

See [Inbound Processing](../inbound.md) for close behavior while an inbound
hook is active, and [Event hooks](../events.md) for the common hook API.
