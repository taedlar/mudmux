# HOOK_MESSAGE_OUTBOUND

`HOOK_MESSAGE_OUTBOUND` lets the logic layer inspect or transform an
application message before it is sent to a communication slot.

Use `comm_add_message()` for output that should pass through this hook:

~~~cxx
comm_add_message(to_slot, data, len);
~~~

If no `HOOK_MESSAGE_OUTBOUND` callback is registered, `comm_add_message()`
preserves the normal output path by calling `comm_buffered_write(to_slot, data,
len)` directly.

## Hook signature

~~~cxx
int hook_message_outbound(void* context, int msg, void* data, size_t len);
~~~

## Arguments

`context` is the user-defined context passed to `mudmux_run()`. It is `nullptr`
when `comm_add_message()` is called with no current async runtime.

`msg` is the destination slot (`to_slot`).

`data` points to an immutable copy of the message bytes and `len` is their byte
count. The data is not null-terminated and may contain arbitrary bytes. Treat
the `void*` hook argument as read-only: it uses the common hook signature and
is not a permission to modify the payload. The pointer is valid only for the
duration of the callback; copy the data before retaining it.

In relaxed mode (`transport.thread_pool.size` greater than one), outbound hooks
are queued per `to_slot`. The queued task owns its payload copy, so the caller
may reuse or discard its input buffer as soon as `comm_add_message()` returns.

## Sending the result

When this hook is registered, it owns the final send decision:

- Forward the original bytes with `comm_buffered_write(to_slot, data, len)`.
- Build and write a replacement message with `comm_buffered_write()`.
- Omit the write to suppress the message.

`comm_add_message()` does not automatically buffer the original message after
an outbound hook returns. Calling `comm_buffered_write()` inside the hook avoids
recursively invoking `HOOK_MESSAGE_OUTBOUND`.

## Example

~~~cxx
static int on_message_outbound(void*, int msg, void* data, size_t len) {
    const int to_slot = msg;
    const auto* bytes = static_cast<const char*>(data);
    std::string replacement(bytes, len);
    for (char& byte : replacement)
        byte = static_cast<char>(std::toupper(static_cast<unsigned char>(byte)));

    comm_buffered_write(to_slot, replacement.data(), replacement.size());
    return 0;
}
~~~
