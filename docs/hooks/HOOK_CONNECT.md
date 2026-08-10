# HOOK_CONNECT

`HOOK_CONNECT` runs when a transport destination is assigned to a communication
slot. It is the one hook that can select the slot's transport protocol before
any input for that slot is parsed.

```c
int hook_connect(void *context, int slot, void *entry_name, size_t len);
```

Transport destinations include the console (stdin/stdout), file inputs, and
accepted TCP sockets.

## Arguments

`context` is the user-defined context supplied to `mudmux_run()`.

`slot` identifies the new destination. The console is always
`COMM_SLOT_CONSOLE` (0); accepted sockets use an available nonzero slot.

`entry_name` points to the configured transport-entry name, such as `*:4000`.
It is null-terminated for convenience, but `len` is its length excluding that
terminator.

## Transport setup

Call the following APIs only while this hook is running. They always configure
the *current* connect hook's slot; do not pass or retain a slot for them. Calls
outside `HOOK_CONNECT` are rejected (and log a warning).

| API | Effect |
| --- | --- |
| `comm_enable_tls()` | Enables server-side TLS on the socket. Call `comm_ssl_init()` successfully before the runtime starts. |
| `comm_enable_telnet()` | Enables direct Telnet parsing and starts Telnet option negotiation. |
| `comm_enable_websocket(preferred_protocols)` | Enables the HTTP WebSocket-upgrade handshake. Returns `false` if the slot is not a suitable transport or direct Telnet is already enabled. `preferred_protocols` is a comma-separated server-preference list; `NULL` or `""` offers no subprotocol. |

TLS is applied before all other enabled protocols, so call it before choosing
Telnet or WebSocket for clarity. Direct Telnet and WebSocket are alternative
top-level protocols: `comm_enable_websocket()` refuses a slot on which direct
Telnet was already enabled. To use Telnet inside WebSocket, offer one of the
supported Telnet subprotocols instead; mudmux enables its Telnet parser only
when the HTTP upgrade selects `telnet.ietf.org` or `telnet.mudstandards.org`.

```c
#include <mudmux/comm.h>

static int on_connect(void *context, int slot, void *entry_name, size_t len) {
    (void)context;
    (void)slot;
    (void)entry_name;
    (void)len;

    comm_enable_tls();
    comm_enable_telnet();             /* TLS-protected direct Telnet */
    return 0;
}
```

```c
static int on_connect(void *context, int slot, void *entry_name, size_t len) {
    (void)context;
    (void)slot;
    (void)entry_name;
    (void)len;

    comm_enable_tls();                /* optional: makes this wss:// */
    if (!comm_enable_websocket("telnet.ietf.org, telnet.mudstandards.org"))
        return -1;
    return 0;
}
```

The second example does not call `comm_enable_telnet()`: Telnet is activated
only if the client offers and the server selects one of those subprotocols.
Application output buffered during `HOOK_CONNECT` waits for the WebSocket
upgrade to succeed, then is sent as WebSocket data.

## Ordering contract

- Input parsing for the same slot remains deferred until `HOOK_CONNECT` returns.
- In relaxed mode, this prevents a TLS ClientHello from being consumed as
  plaintext before `comm_enable_tls()` has run.
- If the hook calls `comm_close(nullptr, slot)`, disconnect progression begins
  only after this hook completes.
- Queued connect work is generation-checked, so stale work is discarded if a
  slot has been reused.

See [Inbound Processing](../inbound.md) for the complete protocol pipeline and
transport combinations.

## Return value

The event loop does not interpret this hook's return value.
