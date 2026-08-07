# HOOK_TRANSPORT_READY

`HOOK_TRANSPORT_READY` fires once per communication slot when mudmux has
finished the transport work required before application input can be delivered.
It is the protocol-neutral place to begin login or session setup.

```c
static int on_transport_ready(void *context, int slot, void *data, size_t size) {
    (void)context;
    (void)data;
    (void)size;
    begin_login(slot);
    return 0;
}

mudmux_register_hook(HOOK_TRANSPORT_READY, on_transport_ready);
```

The callback receives the slot in `msg`; `data` is `NULL` and `size` is zero.
It is invoked after `HOOK_CONNECT` and before the first
`HOOK_MESSAGE_INBOUND` for that slot.

- Plain TCP and direct Telnet are ready after `HOOK_CONNECT` has selected the
  transport and initial Telnet negotiation has been queued.
- TLS is ready after its TLS handshake succeeds.
- WebSocket is ready after a valid upgrade and its HTTP `101` response drain.
  Telnet-over-WebSocket additionally starts its initial Telnet negotiation
  before readiness.

This does not wait for a peer to finish Telnet option negotiation; that
protocol has no finite completion point.

`mudmux_workers_await()` is permitted for the current slot in this callback.
While the hook or its await operation is pending, mudmux holds parsed
application input for the slot and resumes it after completion.
