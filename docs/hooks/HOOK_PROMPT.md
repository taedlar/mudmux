# HOOK_PROMPT

`HOOK_PROMPT` is the slot-idle notification used to render a command prompt or
other "ready for input" output. It is evaluated after the event loop flushes
outbound transport data, so output produced by the preceding command is sent
before the prompt callback is eligible.

```c
static int on_prompt(void *context, int slot, void *data, size_t size) {
    (void)context;
    (void)data;
    (void)size;
    comm_buffered_write(slot, "> ", 2);
    return 0;
}

mudmux_register_hook(HOOK_PROMPT, on_prompt);
```

## Arguments

`msg` is the communication slot. `data` is `NULL` and `size` is zero. The
callback context is the value passed to `mudmux_run()`.

## Eligibility and one-shot behavior

Enable prompt delivery for a slot with `comm_enable_prompt(slot, true)`. The
callback is dispatched only when all of the following are true:

- `C_ENABLE_PROMPT` is set and `C_INVOKED_PROMPT` is clear.
- No inbound work is pending: this includes deferred input, raw buffered
  bytes, and WebSocket decoder or fragmented-message state.
- No outbound work is pending: this includes the normal outbound queue and a
  WebSocket upgrade barrier.
- The slot has no in-flight or awaiting hook.

After a successful dispatch, mudmux sets `C_INVOKED_PROMPT`; this prevents
duplicate prompts while the slot remains idle. The next
`HOOK_MESSAGE_INBOUND` clears that gate, allowing a later idle transition to
produce another prompt. Disabling and re-enabling prompts does not itself
clear the gate.

If the callback cannot be dispatched, for example because the worker queue
cannot accept it, the gate remains clear and mudmux can retry when the slot is
next evaluated as idle.

## Output and threading

Prompt callbacks may use `comm_buffered_write()` to send the prompt. This is
also permitted when `HOOK_MESSAGE_OUTBOUND` is registered, avoiding recursive
outbound-hook dispatch. Do not expect the prompt bytes to be delivered before
the callback returns; they follow the normal non-blocking flush path.

In strict mode, the callback runs on the event-loop thread. In relaxed mode,
it may run on a worker, but it remains serialized with other hooks for the
same slot. `mudmux_workers_await()` is not supported from `HOOK_PROMPT`.
