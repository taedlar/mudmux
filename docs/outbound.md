# Outbound buffering and TLS

## Purpose

This document sketches the outbound slot model for the current plaintext path and the
future TLS path where a TLS session may already be initialized before the comm slot is
registered.

The design goal is simple:

- hook functions must stay non-blocking
- plaintext buffering must remain explicit in the comm layer
- TLS handshakes and record framing must be handled by the transport layer, not by hooks

## Current behavior

Today, `comm_buffered_write(slot, buf, len)` appends plaintext bytes to a per-slot outbound queue.
`comm_flush()` later drains that queue to the slot's write BIO.

That means the current buffering policy is:

- application code owns plaintext buffering
- the transport layer owns socket progress
- flush requests are explicit and event-loop driven

This works for raw socket BIOs, but it becomes ambiguous if a slot is handed a BIO chain
that already contains buffering or filter layers.

## Recommended TLS shape

For TLS, the comm slot should treat the TLS session as part of the transport state.
The slot should still expose a plaintext outbound queue to hooks, but the flush path must
drive TLS handshake and TLS writes before anything reaches the socket.

### Slot-state layout sketch

```text
comm_abstract_t
  rbio            read-side BIO chain or endpoint BIO
  wbio            write-side BIO chain or endpoint BIO
  inbound         plaintext inbound queue
  outbound        plaintext outbound queue
  flags           C_BUFFERED_WRITE, C_CLOSING, C_SOCKET_WRITABLE, ...
  transport_state transport-specific metadata

transport_state
  kind            RAW | TLS | FILE | CONSOLE
  tls             SSL* or equivalent TLS session object
  tls_state       HANDSHAKE | ACTIVE | SHUTDOWN | FAILED
  want_read       transport currently needs read readiness
  want_write      transport currently needs write readiness
  peer_fd         underlying socket fd for runtime registration
```

### Behavior by state

- `RAW`: drain plaintext queue directly to the socket write BIO.
- `TLS` with handshake pending: advance handshake first.
- `TLS` active: write plaintext queue through TLS, not directly to the socket.
- `TLS` shutdown pending: finish queued output, then initiate TLS shutdown.
- `FILE` / `CONSOLE`: keep their own transport-specific handling and avoid assuming a
  socket-only flush model.

## Why not rely on `BIO_f_buffer`

`BIO_f_buffer` is not a good primary outbound buffer for the TLS case here.

- It introduces a second buffering layer beneath the comm queue.
- It hides whether data has merely moved into OpenSSL memory or has actually been sent.
- It complicates close semantics because the slot would need to account for both queued
  plaintext and filter-internal bytes.
- It does not remove the need for explicit runtime readiness handling.

In practice, TLS already has its own internal buffering and record framing, so the
comm-layer queue should remain the single visible buffering layer for plaintext outputs.

## Flush rule

The flush path should follow this order:

1. If TLS is present and not active, drive handshake.
2. If TLS is active, write queued plaintext through the TLS layer.
3. If the transport wants more read or write readiness, keep the corresponding runtime
   interest set.
4. Clear `C_BUFFERED_WRITE` only after both the comm queue and transport internal pending
   output are drained.

## WebSocket upgrades and subprotocols

WebSocket is a transport upgrade: HTTP bytes are sent first, then every subsequent
application or subprotocol byte is carried in a WebSocket data frame.  The outbound
queue must preserve that boundary even when a connect hook writes immediately.

Required ordering:

1. Keep output produced while the HTTP upgrade is pending in an upgrade barrier.
2. Send and fully drain the HTTP `101 Switching Protocols` response.
3. Enable WebSocket framing, then release the barrier as WebSocket data frames.
4. Start any negotiated subprotocol output only after the `101` response.  For example,
   Telnet negotiation is payload inside a WebSocket binary frame, never raw socket data.

`comm_flush_all()` may run between accept and the first request read, so an empty normal
outbound queue is not sufficient reason to release the upgrade barrier.  It must also be
known that `WS_READY` is set.

For inbound processing, decode and unmask a complete WebSocket frame before passing its
payload to a subprotocol parser.  Applying a Telnet parser to raw WebSocket bytes can
consume masking or frame-header bytes that happen to contain IAC (`0xff`) and corrupt the
next frame.

### WebSocket close handshake

The WebSocket Close control frame is a protocol message, not a TCP half-close.  On a
server-initiated close:

1. Mark the slot closing and run disconnect cleanup once.
2. Drain application frames that were already queued by that cleanup.
3. Queue one `Close` frame (normally code `1000`) as the final WebSocket frame.
4. Reject later application writes, including output from delayed relaxed-mode hooks.
5. Keep the transport readable until the peer replies with Close, then tear down TCP/TLS.

Do not call `BIO_shutdown_wr()` or `SSL_shutdown()` while waiting for the peer Close
reply.  A transport EOF/error is the exception: there can be no reply, so teardown may
finish then.  For a peer-initiated Close, queue the required Close reply, flush it, and
only then remove the transport.

## Practical contract for pre-initialized TLS slots

If a TLS object is already initialized before the slot is added, the slot registration API
should record it as a TLS transport explicitly instead of trying to infer it from the BIO
shape.

That keeps the transport contract stable:

- hooks enqueue plaintext only
- the comm layer decides how to encrypt and flush
- runtime event interest follows transport state, not queue shape alone
