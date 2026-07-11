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

`slot` is the communication slot of the newlly added transport destination. Console is always added at slot #0, and socket connections are added at an empty slot available when the connection established.

`entry_name` is null-terminated string of the transport entry name, for example `*:4000`, as specified in the configuration's `transport.accept` list.

`len` is number of characters in `entry_name` (not includes null terminator).

## Return Value
(TBD)