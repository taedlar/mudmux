#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define NOMINMAX
#include "outbound.hpp"

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <openssl/bio.h>
#include <openssl/err.h>

#include "console.hpp"
#include "current_slot.hpp"
#include "execution.hpp"
#include "file_input.hpp"
#include "inbound.hpp"
#include "hooks.hpp"
#include "ssl.hpp"
#include "telnet.hpp"
#include "websocket.hpp"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"
#include "mudmux/mudmux.h"

struct outbound_buffer_s {
    outbound_buffer_t* next{nullptr};
    char buffer[4096];
    size_t start{0}; // <-- async write position (data to be flushed)
    size_t end{0}; // <-- bufferd write position (non-blocking)
    outbound_buffer_t* reset () {
        next = nullptr;
        start = 0;
        end = 0;
        return this;
    }
};
static outbound_buffer_t* outbound_buffer_pool{nullptr}; // recycled outbound buffers to avoid frequent heap allocations
static const int MAX_OUTBOUND_BUFFERS_PER_SLOT = 16; // maximum number of outbound buffers per comm slot

/**
 * @brief Allocate a new outbound buffer, either from the pool or by creating a new one.
 */
static outbound_buffer_t* allocate_outbound_buffer() {
    if (outbound_buffer_pool) {
        outbound_buffer_t* buffer = outbound_buffer_pool;
        outbound_buffer_pool = outbound_buffer_pool->next;
        return buffer->reset();
    }
    return new outbound_buffer_t();
}

/**
 * @brief Free an outbound buffer, returning it to the pool for reuse.
 * @param buffer The outbound buffer to free.
 */
static void free_outbound_buffer(outbound_buffer_t* buffer) {
    if (!buffer)
        return;
    buffer->next = outbound_buffer_pool;
    outbound_buffer_pool = buffer;
}

static bool append_websocket_upgrade_barrier(comm_abstract_ptr& comm, const void* data, size_t size) {
    if (!comm || !data || size == 0)
        return false;

    outbound_buffer_t*& barrier = comm->websocket_upgrade_barrier;
    if (!barrier)
        barrier = allocate_outbound_buffer();
    if (!barrier)
        return false;

    outbound_buffer_t* tail = barrier;
    int buffer_count = 1;
    while (tail->next) {
        tail = tail->next;
        if (++buffer_count >= MAX_OUTBOUND_BUFFERS_PER_SLOT)
            return false;
    }

    while (size > 0) {
        const size_t space = sizeof(tail->buffer) - tail->end;
        if (space == 0) {
            if (++buffer_count > MAX_OUTBOUND_BUFFERS_PER_SLOT)
                return false;
            tail->next = allocate_outbound_buffer();
            tail = tail->next;
            if (!tail)
                return false;
            continue;
        }
        const size_t copied = std::min(space, size);
        memcpy(tail->buffer + tail->end, data, copied);
        tail->end += copied;
        data = static_cast<const char*>(data) + copied;
        size -= copied;
    }
    return true;
}

static std::string normalize_telnet_newlines(std::string_view input) {
    std::string normalized;
    normalized.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '\r') {
            normalized.push_back('\r');
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                normalized.push_back('\n'); // preserve CR LF sequence
                ++i;
            }
            if (i + 1 < input.size() && input[i + 1] == '\0') {
                normalized.push_back('\n'); // convert CR NUL sequence to CR LF
                ++i;
            }
            // If CR is followed by any other character, it will be preserved as-is.
            continue;
        }
        if (ch == '\n') {
            // Convert standalone LF to CR LF for Telnet clients.
            normalized.push_back('\r');
            normalized.push_back('\n');
            continue;
        }
        normalized.push_back(ch);
    }

    return normalized;
}

void comm_buffered_write_raw_comm(comm_abstract_ptr& comm, const void *buf, size_t len) {
    if (!comm || !comm->wbio || !buf || len == 0)
        return; // invalid parameters
    if (len > sizeof(outbound_buffer_t::buffer) * MAX_OUTBOUND_BUFFERS_PER_SLOT) {
        SPDLOG_ERROR("Attempted to write more data than allowed for buffered write");
        return;
    }
    // This queue stores plaintext payload only; TLS framing and encryption must happen
    // below this layer so hooks remain non-blocking and transport buffering stays explicit.
    if (!comm->outbound) {
        // No existing outbound buffer, allocate one (always buffer-and-flush)
        comm->outbound = allocate_outbound_buffer();
    }
    if (comm->outbound) {
        outbound_buffer_t* obb = comm->outbound; // find the last buffer in the chain
        int buffer_count = 1;
        while (obb->next) {
            obb = obb->next;
            buffer_count++;
            if (buffer_count >= MAX_OUTBOUND_BUFFERS_PER_SLOT) {
                SPDLOG_ERROR("Exceeded maximum outbound buffers per slot");
                return;
            }
        }
        assert(obb->next == nullptr);
        size_t space = sizeof(obb->buffer) - obb->end; // space left in the tail buffer
        while (len > space) {
            // fill the current buffer and allocate a new one
            memcpy(obb->buffer + obb->end, buf, space);
            obb->end += space;
            buf = static_cast<const char*>(buf) + space;
            len -= space;
            if (++buffer_count > MAX_OUTBOUND_BUFFERS_PER_SLOT) {
                SPDLOG_ERROR("Exceeded maximum outbound buffers per slot");
                len = 0; // drop remaining data
                break;
            }
            obb->next = allocate_outbound_buffer();
            obb = obb->next;
            if (!obb) {
                SPDLOG_ERROR("Failed to allocate new outbound buffer");
                break;
            }
            space = sizeof(obb->buffer) - obb->end;
        }
        if (len > 0 && obb) {
            memcpy(obb->buffer + obb->end, buf, len);
            obb->end += len;
        }
        comm->flags |= C_BUFFERED_WRITE; // data written for flush

        // Ensure the event loop is notified to flush buffered data even when
        // no readable events are pending.
        if (!(comm->flags & C_SOCKET_WRITABLE)) {
            socket_fd_t fd {INVALID_SOCKET_FD};
            if (comm_bio_get_socket_fd(comm->wbio, &fd) && fd != INVALID_SOCKET_FD) {
                async_runtime_t* runtime = async_get_current_runtime();
                if (runtime) {
                    if (async_runtime_modify(runtime, fd, EVENT_READ | EVENT_WRITE, nullptr) == 0)
                        comm->flags |= C_SOCKET_WRITABLE;
                }
            }
        }
    }
}

static void comm_buffered_write_comm_impl(
    comm_abstract_ptr& comm,
    const void *buf,
    size_t len,
    bool normalize_telnet_text_newlines) {
    if (!comm || !buf || len == 0)
        return;

    std::string normalized;
    if (normalize_telnet_text_newlines && (comm->flags & C_ENABLE_TELNET)) {
        // Telnet clients expect CRLF line endings on outbound text.
        normalized = normalize_telnet_newlines(std::string_view(static_cast<const char*>(buf), len));
        buf = normalized.data();
        len = normalized.size();
    }

    // RFC 6455 forbids data frames after Close. A relaxed hook that was
    // already queued can otherwise append output after the handshake starts.
    if (C_WEBSOCKET_IS_READY(comm->flags) &&
        C_WEBSOCKET_STATE(comm->flags) == WS_CLOSE_SENT) {
        SPDLOG_DEBUG("discarding {} application bytes after WebSocket Close on slot {}", len, comm.slot());
        return;
    }
    if ((comm->flags & C_ENABLE_WEBSOCKET) && !C_WEBSOCKET_IS_READY(comm->flags)) {
        // Application output must not precede the HTTP 101 response. Preserve
        // it in the upgrade barrier as already-framed WebSocket data.
        std::string frame;
        if (!comm_websocket_encode_frame(std::string_view(static_cast<const char*>(buf), len), 0x2, frame)
            || !append_websocket_upgrade_barrier(comm, frame.data(), frame.size()))
            SPDLOG_ERROR("failed to queue WebSocket upgrade-barrier output");
        return;
    }
    if (C_WEBSOCKET_IS_READY(comm->flags)) {
        std::string frame;
        if (!comm_websocket_encode_frame(std::string_view(static_cast<const char*>(buf), len), 0x2, frame)) {
            SPDLOG_ERROR("failed to encode WebSocket outbound frame");
            return;
        }
        comm_buffered_write_raw_comm(comm, frame.data(), frame.size());
        return;
    }
    comm_buffered_write_raw_comm(comm, buf, len);
}

void comm_buffered_write_comm (comm_abstract_ptr& comm, const void *buf, size_t len) {
    comm_buffered_write_comm_impl(comm, buf, len, true);
}

void comm_buffered_write_comm_no_telnet_normalize (comm_abstract_ptr& comm, const void *buf, size_t len) {
    comm_buffered_write_comm_impl(comm, buf, len, false);
}

void comm_buffered_write (int slot, const void *buf, size_t len) {
    const mudmux_hook_type_t current_hook = comm_current_hook_type();
    if (current_hook == HOOK_MESSAGE_OUTBOUND || current_hook == HOOK_PROMPT) {
        const int current_slot = comm_current_slot();
        if (current_slot < 0 || slot != current_slot) {
            SPDLOG_ERROR(
                "comm_buffered_write() from {} must target its current slot {}",
                current_hook == HOOK_MESSAGE_OUTBOUND ? "HOOK_MESSAGE_OUTBOUND" : "HOOK_PROMPT",
                current_slot);
            return;
        }
    }

    if (mudmux_get_registered_hook(HOOK_MESSAGE_OUTBOUND)) {
        if (current_hook != HOOK_MESSAGE_OUTBOUND && current_hook != HOOK_PROMPT) {
            SPDLOG_ERROR(
                "comm_buffered_write() is restricted while HOOK_MESSAGE_OUTBOUND is registered; "
                "use comm_add_message() or comm_add_formatted_message() instead");
            return;
        }
    }

    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return;

    // Unsolicited application output displaces a character-mode choice
    // prompt. Re-arm it so the post-flush idle pass redraws that prompt.
    // Line-input sessions deliberately retain their existing prompt state,
    // and prompt output itself must not re-arm another prompt.
    if ((comm->flags & C_ENABLE_PROMPT) && !(comm->flags & C_LINE_INPUT) &&
        comm_current_hook_type() != HOOK_PROMPT)
        comm->flags &= ~C_INVOKED_PROMPT;
    comm_buffered_write_comm(comm, buf, len);
}

void comm_add_message (int to_slot, const void *buf, size_t len) {
    if (!buf || len == 0)
        return;

    // Prompt and outbound hooks own their final output. Routing through this
    // API would recursively dispatch (or requeue) an outbound hook; use the
    // current-slot comm_buffered_write() path instead.
    const mudmux_hook_type_t current_hook = comm_current_hook_type();
    if (current_hook == HOOK_MESSAGE_OUTBOUND || current_hook == HOOK_PROMPT) {
        SPDLOG_ERROR(
            "comm_add_message() cannot be called from {}; use comm_buffered_write() "
            "for the current slot instead",
            current_hook == HOOK_MESSAGE_OUTBOUND ? "HOOK_MESSAGE_OUTBOUND" : "HOOK_PROMPT");
        return;
    }

    if (!mudmux_get_registered_hook(HOOK_MESSAGE_OUTBOUND)) {
        comm_buffered_write(to_slot, buf, len);
        return;
    }

    // The hook receives an isolated payload. This keeps the caller's input
    // immutable even when the generic hook signature exposes a void pointer.
    std::vector<char> message(static_cast<const char*>(buf), static_cast<const char*>(buf) + len);
    async_runtime_t* runtime = async_get_current_runtime();
    (void)mudmux_dispatch_hook_after(HOOK_MESSAGE_OUTBOUND,
        runtime ? async_runtime_get_context(runtime) : nullptr,
        to_slot,
        message.data(),
        len);
}

void comm_add_vformatted_message (int to_slot, const char *fmt, va_list args) {
    if (!fmt)
        return;

    va_list size_args;
    va_copy(size_args, args);
    const int formatted_size = std::vsnprintf(nullptr, 0, fmt, size_args);
    va_end(size_args);
    if (formatted_size < 0) {
        SPDLOG_ERROR("vsnprintf failed while formatting outbound message");
        return;
    }
    if (formatted_size == 0)
        return;

    std::vector<char> message(static_cast<size_t>(formatted_size) + 1u);
    va_list render_args;
    va_copy(render_args, args);
    const int written = std::vsnprintf(message.data(), message.size(), fmt, render_args);
    va_end(render_args);
    if (written != formatted_size) {
        SPDLOG_ERROR("vsnprintf wrote unexpected size while formatting outbound message");
        return;
    }

    comm_add_message(to_slot, message.data(), static_cast<size_t>(written));
}

void comm_add_formatted_message (int to_slot, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    comm_add_vformatted_message(to_slot, fmt, args);
    va_end(args);
}

void comm_free_outbound_buffers(comm_abstract_ptr& comm) {
    if (comm) {
        while (comm->outbound) {
            outbound_buffer_t* next_buffer = comm->outbound->next;
            free_outbound_buffer(comm->outbound);
            comm->outbound = next_buffer;
        }
        while (comm->websocket_upgrade_barrier) {
            outbound_buffer_t* next_buffer = comm->websocket_upgrade_barrier->next;
            free_outbound_buffer(comm->websocket_upgrade_barrier);
            comm->websocket_upgrade_barrier = next_buffer;
        }
        assert(comm->outbound == nullptr);
    }
}

void comm_flush (async_runtime_t* runtime, int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm || !comm->wbio)
        return; // invalid parameters

    // EVENT_WRITE should also drive TLS handshaking until the session is ready.
    // Once closing has begun, however, a failed final write may clear
    // C_TLS_ESTABLISHED.  Do not mistake that terminal TLS state for an
    // incomplete handshake and call SSL_do_handshake() after close_notify.
    if (comm->ssl && !(comm->flags & C_TLS_ESTABLISHED) && !(comm->flags & C_CLOSING)) {
        int hs = comm_tls_handshake_step(runtime, slot);
        if (hs < 0) {
            if (!(comm->flags & C_CLOSING))
                async_runtime_post_completion(runtime, ASYNC_IO_ERROR_KEY, static_cast<uintptr_t>(slot));
            return;
        }
        if (hs == 0) {
            BIO_flush(comm->wbio);
            return;
        }
    }

    outbound_buffer_t* obb = comm->outbound;
    while (obb) {
        size_t data_len = obb->end - obb->start;
        if (data_len > 0) {
            if (comm->ssl) {
                size_t written = 0;
                int ok = SSL_write_ex(comm->ssl, obb->buffer + obb->start, data_len, &written);
                if (ok != 1) {
                    const int ssl_err = SSL_get_error(comm->ssl, ok);
                    if (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_WANT_WRITE) {
                        if (comm->flags & C_CLOSING) {
                            SPDLOG_DEBUG("SSL_write_ex failed during closing flush for slot {} (ssl_err={})", slot, ssl_err);
                            // Closing teardown can race with peer close; stop treating this slot as TLS-established
                            // so we do not retry TLS close-notify on subsequent close progression.
                            comm->flags &= ~C_TLS_ESTABLISHED;
                        } else {
                            SPDLOG_ERROR("SSL_write_ex failed during flush for slot {} (ssl_err={})", slot, ssl_err);
                        }
                        comm_free_outbound_buffers(comm); // drop any buffered outbound data
                        comm->flags &= ~C_BUFFERED_WRITE;
                        if (!(comm->flags & C_CLOSING))
                            async_runtime_post_completion(runtime, ASYNC_IO_ERROR_KEY, static_cast<uintptr_t>(slot));
                    }
                    break;
                }
                obb->start += written;
            }
            else {
                int written = BIO_write(comm->wbio, obb->buffer + obb->start, static_cast<int>(data_len));
                if (written <= 0) {
                    if (!BIO_should_retry(comm->wbio)) {
                        SPDLOG_ERROR ("BIO_write failed during flush: {}", ERR_error_string(ERR_get_error(), nullptr));
                        comm_free_outbound_buffers(comm); // drop any buffered outbound data
                        comm->flags &= ~C_BUFFERED_WRITE;
                        if (!(comm->flags & C_CLOSING))
                            async_runtime_post_completion(runtime, ASYNC_IO_ERROR_KEY, static_cast<uintptr_t>(slot));
                    }
                    break;
                }
                obb->start += static_cast<size_t>(written);
            }
            if (obb->start < obb->end)
                continue; // more data to write
        }
        // No data left in this buffer, move to the next
        outbound_buffer_t* next_buffer = obb->next;
        free_outbound_buffer(obb);
        comm->outbound = next_buffer;
        obb = next_buffer;
    }
    BIO_flush(comm->wbio); // ensure all data is sent to the transport layer

    // The HTTP upgrade response has drained. Queue the Telnet negotiation only
    // now, so it is sent as WebSocket data after the response bytes.
    if (!comm->outbound && C_WEBSOCKET_STATE(comm->flags) == WS_TELNET_PENDING) {
        C_WEBSOCKET_SET_STATE(comm->flags, WS_READY);
        if (!(comm->flags & C_CLOSING)) {
            comm_start_telnet_negotiation(slot);
            comm_flush(runtime, slot);
            return;
        }
    }

    // Application data queued by the connect hook must remain behind the
    // HTTP 101 response.  comm_flush_all() also runs between accept and the
    // first client read, so releasing this barrier merely because the normal
    // outbound queue is empty would put a WebSocket frame on the wire before
    // the upgrade has completed.
    if (!comm->outbound && C_WEBSOCKET_IS_READY(comm->flags) && comm->websocket_upgrade_barrier) {
        SPDLOG_DEBUG("releasing WebSocket upgrade barrier on slot {} after HTTP 101", slot);
        comm->outbound = comm->websocket_upgrade_barrier;
        comm->websocket_upgrade_barrier = nullptr;
        comm->flags |= C_BUFFERED_WRITE;
        // The barrier now occupies the normal queue, so output written by a
        // strict transport-ready hook appends after it.  This also makes a
        // nested flush from hook dispatch preserve the same ordering.
        comm_invoke_transport_ready(runtime, slot);
        if (!comm)
            return;
        comm_flush(runtime, slot);
        return;
    }

    // No application output was held behind the HTTP 101 response.  This is
    // the remaining WebSocket-ready path, including Telnet-over-WebSocket
    // after its initial negotiation bytes drain.
    if (!comm->outbound && C_WEBSOCKET_IS_READY(comm->flags))
        comm_invoke_transport_ready(runtime, slot);

    socket_fd_t fd {INVALID_SOCKET_FD};
    if (!comm_bio_get_socket_fd(comm->wbio, &fd)) {
        SPDLOG_WARN ("Failed to retrieve socket fd from BIO during flush for slot {}", slot);
        return;
    }

    if (comm->outbound) {
        if (!(comm->flags & C_SOCKET_WRITABLE)) {
            comm->flags |= C_SOCKET_WRITABLE;
            if (fd != INVALID_SOCKET_FD)
                async_runtime_modify (runtime, fd, EVENT_READ | EVENT_WRITE, nullptr);
        }
        // we'll wait for writable event to flush remaining data
        return;
    }
    else {
        // all buffered data flushed, remove writable event if it was set
        if (comm->flags & C_SOCKET_WRITABLE) {
            if (fd != INVALID_SOCKET_FD)
                async_runtime_modify (runtime, fd, EVENT_READ, nullptr); // remove writable event, keep readable event
            comm->flags &= ~C_SOCKET_WRITABLE;
        }

        // clear buffered-write flag, it will be set again when new data is written
        comm->flags &= ~C_BUFFERED_WRITE;

        const bool waiting_for_websocket_close =
            C_WEBSOCKET_IS_READY(comm->flags) &&
            C_WEBSOCKET_STATE(comm->flags) != WS_CLOSE_RECEIVED;
        if ((comm->flags & C_CLOSING) && !waiting_for_websocket_close) {
            SPDLOG_TRACE ("comm slot has C_CLOSING flag set, sending shutdown signal to peer");
            if (comm->ssl && (comm->flags & C_TLS_ESTABLISHED)) {
                (void) SSL_shutdown(comm->ssl);
            } else {
                BIO_shutdown_wr(comm->wbio); // shutdown write side of the socket and expect the peer to close the connection
            }
        }

    }
}

void comm_flush_all (async_runtime_t* runtime) {
    int max_slot = comm_max_slot();
    while (max_slot >= 0) {
        comm_flush (runtime, max_slot);
        max_slot--;
    }

    // Worker-hook completions wake the runtime without an I/O event context.
    // Revisit closing slots here so a queued HOOK_DISCONNECT can advance to
    // transport teardown even when the peer sends no further event.
    for (int slot = comm_max_slot() - 1; slot >= 0; --slot) {
        bool should_progress = false;
        {
            comm_abstract_ptr comm(slot, comm_slots_mtx);
            should_progress = comm && (comm->flags & C_CLOSING) &&
                !(comm->flags & C_AWAITING_HOOK);
        }
        if (should_progress)
            (void)comm_close(runtime, slot);
    }
}

static void _disconnect_hook_complete(void*, int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (comm)
        comm->flags &= ~C_AWAITING_HOOK;
}

bool comm_close (async_runtime_t* runtime, int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return true; // already removed

    if (!runtime)
        runtime = async_get_current_runtime();

    if (!(comm->flags & C_CLOSING) || (comm->flags & C_DISCONNECT_PENDING)) {
        // let logic layer handle disconnect (e.g., cleanup, logging, etc.)
        comm->flags |= C_CLOSING;
        comm->flags &= ~C_DISCONNECT_PENDING;
        mudmux_execution_cancel_await(slot);
        comm->flags |= C_AWAITING_HOOK;
        const mudmux_dispatch_result_t dispatch_result = mudmux_dispatch_hook_after(
            HOOK_DISCONNECT,
            async_runtime_get_context(runtime),
            slot,
            nullptr,
            0,
            _disconnect_hook_complete,
            nullptr);
        if (dispatch_result == MUDMUX_DISPATCH_QUEUE_FULL) {
            // A hook for this slot is still running.  Preserve only this
            // terminal lifecycle transition; no application payload is kept.
            // Its completion wakes the event loop, which retries comm_close().
            comm->flags |= C_DISCONNECT_PENDING;
            comm->flags &= ~C_AWAITING_HOOK;
            return false;
        }
        if (dispatch_result != MUDMUX_DISPATCH_OK) {
            comm->flags &= ~C_AWAITING_HOOK;
        } else if (mudmux_execution_should_dispatch_async(HOOK_DISCONNECT)) {
            // Do not remove/reuse the slot while the disconnect callback is
            // executing on a worker.  Its completion wakes the event loop.
            return false;
        }

        comm->flags &= ~C_ENABLE_PROMPT; // disable prompt to avoid corrupted L7 shutdown sequence
    }

    if (comm->flags & C_AWAITING_HOOK)
        return false;

    if (slot == COMM_SLOT_CONSOLE) {
        // Console slots are lifecycle-driven by console/file-input workers rather than
        // transport writable events. Do not block close on buffered-write deferral.
        comm_free_outbound_buffers(comm);
        comm->flags &= ~C_BUFFERED_WRITE;

        if (comm_has_file_inputs()) {
            // Async file-input mode: close slot immediately and terminate server.
            SPDLOG_DEBUG("comm slot {} is async file input console, closing and shutting down", slot);
            comm_abstract_remove(slot);
            mudmux_shutdown();
            return true;
        }

        // Standard/interactive console mode: signal worker EOF and let
        // comm_process_console_input perform final disconnect + shutdown decision.
        comm_signal_console_eof(runtime);
        SPDLOG_TRACE("signaling EOF to console worker");
        return false;
    }

    if (comm->flags & C_BUFFERED_WRITE) {
        // If TLS is not established yet, buffered plaintext cannot be flushed safely.
        // Drop pending data and close immediately to avoid close/flush deadlock.
        if (comm->ssl && !(comm->flags & C_TLS_ESTABLISHED)) {
            SPDLOG_DEBUG("comm slot {} dropping buffered data during pre-handshake close", slot);
            comm_free_outbound_buffers(comm);
            comm->flags &= ~C_BUFFERED_WRITE;
        } else {
            SPDLOG_DEBUG("comm slot {} has buffered data, will flush before disconnecting", slot);
            return false;
        }
    }

    // The close control frame must be the final WebSocket frame.  In
    // particular, HOOK_DISCONNECT may have queued a final message above, so
    // wait until it has drained before sending Close.
    if (C_WEBSOCKET_IS_READY(comm->flags) &&
        C_WEBSOCKET_STATE(comm->flags) != WS_CLOSE_SENT &&
        C_WEBSOCKET_STATE(comm->flags) != WS_CLOSE_RECEIVED) {
        const char normal_close[] = {0x03, static_cast<char>(0xe8)}; // 1000
        SPDLOG_DEBUG("outbound data drained; initiating WebSocket close on slot {}", slot);
        (void) comm_websocket_queue_close(comm, std::string_view(normal_close, sizeof(normal_close)));
        return false;
    }

    if (C_WEBSOCKET_IS_READY(comm->flags) &&
        C_WEBSOCKET_STATE(comm->flags) != WS_CLOSE_RECEIVED) {
        return false;
    }

    if (comm->ssl && (comm->flags & C_TLS_ESTABLISHED)) {
        // Best-effort TLS close_notify before tearing down transport.
        const int shutdown_rc = SSL_shutdown(comm->ssl);
        if (shutdown_rc < 0) {
            const int ssl_err = SSL_get_error(comm->ssl, shutdown_rc);
            if (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_WANT_WRITE)
                SPDLOG_DEBUG("SSL_shutdown failed during close on slot {} (ssl_err={})", slot, ssl_err);
        }
    }

    socket_fd_t fd {INVALID_SOCKET_FD};
    if (comm->rbio && comm_bio_get_socket_fd(comm->rbio, &fd))
        async_runtime_remove (runtime, fd);
    if (comm->wbio && comm_bio_get_socket_fd(comm->wbio, &fd))
        async_runtime_remove (runtime, fd);

    comm_abstract_remove(slot);
    return true;
}

int comm_invoke_disconnect (async_runtime_t* runtime, int slot) {
    return mudmux_dispatch_hook_after (HOOK_DISCONNECT,
        async_runtime_get_context(runtime),
        slot,
        nullptr,
        0
    );
}
