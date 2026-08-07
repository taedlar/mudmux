# Inbound Processing

This document describes how mudmux turns transport bytes into
`HOOK_MESSAGE_INBOUND` calls.  The transport and parser state belong to a
communication slot, so inbound ordering is defined **per slot**.  Different
slots may advance concurrently in relaxed mode.

## Processing pipeline

For a socket slot, the normal path is:

```text
socket readiness
  -> transport read
  -> TLS decrypt (when enabled)
  -> WebSocket HTTP upgrade or frame decode (when enabled)
  -> Telnet negotiation/parser (when enabled)
  -> line or character parser
  -> HOOK_MESSAGE_INBOUND
```

TLS always precedes WebSocket and Telnet parsing.  A WebSocket upgrade is HTTP
inside the TLS plaintext stream, not a separate transport.  After a successful
upgrade, WebSocket frames are decoded before an optional Telnet subprotocol is
parsed.

## Transport combinations

| Socket configuration | Bytes delivered to the logic layer |
|---|---|
| Plain | Line or character input, according to the slot input mode. |
| Plain + Telnet | Telnet commands/options are consumed; application payload is delivered using the slot input mode. |
| TLS | Decrypted line or character input. |
| TLS + Telnet | Decrypted Telnet stream, then application payload. |
| WebSocket | HTTP Upgrade first, then decoded WebSocket message payloads. |
| TLS + WebSocket | TLS handshake first, HTTP Upgrade second, then decoded WebSocket messages. This is `wss://`. |
| WebSocket + negotiated `telnet.ietf.org` or `telnet.mudstandards.org` | Decoded WebSocket payload is parsed as Telnet before it reaches the logic layer. |

Transport protocols are selected only from `HOOK_CONNECT`, and each enabling
API always targets that hook's current slot: `comm_enable_tls()`,
`comm_enable_websocket(preferred_protocols)`, and `comm_enable_telnet()`.
`comm_enable_websocket()` cannot be combined with an already enabled direct
Telnet stream.  Telnet-over-WebSocket is enabled only after the HTTP upgrade
selects one of the supported Telnet subprotocols.

Application output queued by `HOOK_CONNECT` is held behind a pending WebSocket
upgrade.  It is sent as WebSocket data only after the `101 Switching Protocols`
response has drained.  If an upgrade is rejected, mudmux discards the pending
application output and the rejected HTTP bytes, sends only the HTTP error, and
closes the slot.

## Strict and relaxed hook ordering

With a thread-pool size of one, hooks run in strict mode on the event-loop
thread.  Each message is parsed and its hook completes before the same slot
continues.

With a thread-pool size greater than one, mudmux uses relaxed mode.  Hooks for
different slots can run concurrently, but the following invariant still holds:

> A slot must not parse or retain another decoded inbound message while an
> earlier same-slot hook callback is running.

The execution subsystem permits exactly one in-flight hook per slot.  There is
no per-slot inbound-hook or decoded-message queue.  The execution state marks
the one in-flight hook, while `C_DEFERRED_INBOUND` records that buffered
transport bytes need another parsing pass. Completion wakes the event loop,
which resumes parsing only after the hook is idle. Consequences:

- Line and character input dispatch at most one asynchronous message before
  pausing.
- The WebSocket decoder consumes at most one complete application message per
  pass; later frames remain raw inbound bytes until the hook returns.
- On POSIX readiness backends, socket reads are also deferred while a connect
  or inbound hook is configuring that slot.  Since epoll/poll are level
  triggered, unread bytes are reported again after the hook completion wakes
  the event loop.
- The same rule prevents a relaxed `HOOK_CONNECT` from racing TLS setup: a
  ClientHello cannot be consumed as plaintext before the hook calls
  `comm_enable_tls()`.

Concise `HOOK_CONNECT` contract: it is the slot-initialization boundary; input
parsing for that slot remains deferred until connect returns, and if connect
requests close, disconnect progression runs after connect completes.

An explicit hook/API request for a slot (for example a prompt or lifecycle
action requested by a hook running for another slot) is different from inbound
parsing: it may use that target slot's bounded FIFO continuation queue.  Those
entries are copied and generation-checked, execute after the active target-slot
hook, and are discarded if the slot is removed/reused.  `HOOK_MESSAGE_INBOUND`
and Telnet parser dispatches never use this queue.
See [docs/workers.md](workers.md) for the per-hook queueability table in the
ordering section.

Do not retain `comm_abstract_ptr` in a hook or across an asynchronous boundary.
Logic-layer state touched by hooks for different slots must still be synchronized
by the host application.

If a hook closes its slot, no further decoded input is retained.  A pending
`HOOK_DISCONNECT` is a terminal flag (not a queued callback) and is dispatched
after the current hook returns; the slot is then flushed/removed normally.

## Input modes and prompts

`comm_set_line_input(slot, echo)` selects line input.  A complete stripped line
causes one inbound hook call.  `comm_set_char_input(slot)` selects raw character
input; ANSI control sequences and UTF-8 characters are kept intact, and the
slot returns to line input before the hook runs.  A hook may explicitly re-arm
character mode.

After inbound work drains, mudmux may invoke `HOOK_PROMPT` for slots with
`C_ENABLE_PROMPT`.  Prompt-producing logic should treat outbound writes as
normal buffered transport output; TLS and WebSocket framing remain below the
hook API.
