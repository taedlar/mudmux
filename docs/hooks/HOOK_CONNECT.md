# HOOK_CONNECT
The hook function for `HOOK_CONNECT` is called when a transport destination is added to a communication slot.

Function signature:
~~~cxx
int hook_connect (void* context, int slot, void* entry_name, size_t len);
~~~

Transport destination includes:
- console (stdin / stdout)
- file inputs
- network sockets (TCP)

## Arguments
`context` is a user-defined context specified when calling `mudmux_run`.

`slot` is the communication slot of the newly added transport destination. Console is always added at slot #0, and socket connections are added at an empty slot available when the connection established.

`entry_name` is null-terminated string of the transport entry name, for example `*:4000`, as specified in the configuration's `transport.accept` list.

`len` is number of characters in `entry_name` (not includes null terminator).

## Ordering Contract (Relaxed Mode)
- `HOOK_CONNECT` is the slot-initialization boundary for transport mode setup.
- Same-slot input parsing remains deferred until `HOOK_CONNECT` returns.
- If the hook requests `comm_close(slot)`, disconnect progression is serialized after connect completes.
- Rare queued-connect edge cases are generation-checked before execution, so stale work is dropped after slot reuse.

## Return Value
(TBD)