# HOOK_MESSAGE_INBOUND
The hook function for `HOOK_MESSAGE_INBOUND` is called when inbound input data is parsed from a communication slot.

Function signature:
~~~cxx
int hook_message_inbound (void* context, int slot, void* data, size_t len);
~~~

This hook is transport-agnostic. Input can come from:
- console (stdin)
- file input
- network sockets

## When it is called
`HOOK_MESSAGE_INBOUND` is dispatched by the communication layer after input parsing in the current input mode of the slot:

- line input mode (`C_LINE_INPUT`):
  - called once per complete line
  - line delimiters (`\n`, `\r\n`) are stripped
  - leading/trailing whitespace is stripped
  - partial lines are buffered until a newline arrives
- char input mode (default when `C_LINE_INPUT` is not set):
  - called for one character input sequence per processing pass
  - with ANSI enabled (`C_ENABLE_ANSI`), an ANSI sequence (for example `ESC [ A`, `ESC [ 31 ~`) is delivered as one message
  - incomplete ANSI/UTF-8 sequences are buffered until complete

## Arguments
`context` is a user-defined context specified when calling `mudmux_run`.

`slot` is the communication slot that produced this inbound data.

`data` points to the inbound payload bytes.

`len` is number of payload bytes pointed by `data`.

Notes:
- `data` is not guaranteed to be null-terminated.
- payload may contain arbitrary bytes, depending on input mode and transport.

## Return Value
The return value is propagated back from `mudmux_invoke_hook`, but inbound processing currently does not branch on it.
Use hook side effects (for example `comm_close`, `comm_buffered_write`) to control behavior.

## Typical usage
Typical logic-layer usage includes:
- command parsing and dispatch
- slot-local echo or response generation
- calling `comm_close(runtime, slot)` for commands like `quit` or `exit`

Example:
~~~cxx
static int on_message_inbound (void*, int slot, void* data, size_t len) {
    std::string msg(static_cast<const char*>(data), len);
    auto comm = comm_abstract_get(slot);
    if (comm)
        *comm << "Received: [" << msg << "]\n\r";
    if (msg == "quit" || msg == "exit")
        comm_close(nullptr, slot);
    return 0;
}
~~~

## Threading and safety
Hook callbacks run on the event-loop thread while mudmux holds the logic-layer comm mutex during hook dispatch.
This makes comm API calls from inside the hook deterministic with respect to slot state.

Outside hook dispatch, your own shared logic-layer data still needs explicit synchronization.
