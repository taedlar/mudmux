#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define NOMINMAX
#include "inbound.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <wchar.h>
#include <openssl/bio.h>

#include "abstract.hpp"
#include "execution.hpp"
#include "hooks.hpp"
#include "input_mode.hpp"
#include "outbound.hpp"
#include "ssl.hpp"
#include "telnet.hpp"
#include "websocket.hpp"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

#ifdef _WIN32
typedef SSIZE_T ssize_t;
#endif

#define INBOUND_BAND_DATA       0
#define INBOUND_BAND_SUBNEG     1

std::atomic<bool> has_deferred_input{false};

struct inbound_buffer_s {
    inbound_buffer_t* next{nullptr};
    unsigned int band: 4;
    size_t start{0}; // <-- async read position (data to be processed)
    size_t end{0}; // <-- append new data position (non-blocking)
    char buffer[4096];
    inbound_buffer_t* reset () {
        next = nullptr;
        band = INBOUND_BAND_DATA;
        start = 0;
        end = 0;
        return this;
    }
};
static inbound_buffer_t* inbound_buffer_pool{nullptr}; // recycled inbound buffers to avoid frequent heap allocations

static inbound_buffer_t* allocate_inbound_buffer() {
    if (inbound_buffer_pool) {
        inbound_buffer_t* buffer = inbound_buffer_pool;
        inbound_buffer_pool = inbound_buffer_pool->next;
        return buffer->reset();
    }
    return new inbound_buffer_t();
}

static void free_inbound_buffer(inbound_buffer_t* buffer) {
    if (!buffer)
        return;
    buffer->next = inbound_buffer_pool;
    inbound_buffer_pool = buffer;
}

extern "C" void comm_enable_prompt (int slot, bool enable) {
    if (slot < 0) {
        return;
    }

    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm) {
        return;
    }

    if (enable) {
        comm->flags |= C_ENABLE_PROMPT;
    } else {
        comm->flags &= ~C_ENABLE_PROMPT;
    }
}

void comm_invoke_prompt (async_runtime_t* runtime) {
    if (!runtime)
        return;

    std::lock_guard<std::recursive_mutex> lock(comm_slots_mtx);
    for (int max_slot = comm_max_slot(), slot = 0; slot < max_slot; ++slot) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        if (!comm)
            continue;
        if (comm->flags & C_BUFFERED_WRITE)
            continue; // skip comms with pending buffered write
        if ((comm->flags & C_ENABLE_PROMPT) && !(comm->flags & C_INVOKED_PROMPT)) {
            const mudmux_dispatch_result_t dispatch_result = mudmux_dispatch_hook_after(
                HOOK_PROMPT,
                async_runtime_get_context(runtime),
                slot,
                nullptr,
                0,
                nullptr,
                nullptr,
                slot
            );
            if (dispatch_result != MUDMUX_DISPATCH_QUEUE_FULL)
                comm->flags |= C_INVOKED_PROMPT;
        }
    }
}

bool comm_has_deferred_input (void) {
    return has_deferred_input.load(std::memory_order_acquire);
}

void comm_defer_input(int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return;
    comm->flags |= C_DEFERRED_INBOUND;
    has_deferred_input.store(true, std::memory_order_release);
}

size_t comm_copy_inbound_data_prefix(comm_abstract_ptr& comm, size_t limit, std::string& out) {
    out.clear();
    if (!comm || limit == 0) {
        return 0;
    }

    size_t copied = 0;
    for (inbound_buffer_t* ibb = comm->inbound; ibb && copied < limit; ibb = ibb->next) {
        if (ibb->band != INBOUND_BAND_DATA || ibb->end <= ibb->start) {
            continue;
        }

        const size_t available = ibb->end - ibb->start;
        const size_t chunk = std::min(available, limit - copied);
        out.append(ibb->buffer + ibb->start, chunk);
        copied += chunk;
    }

    return copied;
}

void comm_consume_inbound_data(comm_abstract_ptr& comm, size_t bytes) {
    if (!comm || bytes == 0) {
        return;
    }

    while (comm->inbound && bytes > 0) {
        inbound_buffer_t* head = comm->inbound;

        if (head->band != INBOUND_BAND_DATA) {
            comm->inbound = head->next;
            free_inbound_buffer(head);
            continue;
        }

        const size_t available = (head->end > head->start) ? (head->end - head->start) : 0;
        if (available == 0) {
            comm->inbound = head->next;
            free_inbound_buffer(head);
            continue;
        }

        const size_t consume = std::min(available, bytes);
        head->start += consume;
        bytes -= consume;

        if (head->start >= head->end) {
            comm->inbound = head->next;
            free_inbound_buffer(head);
        }
    }
}

void comm_resume_deferred_input (async_runtime_t* runtime) {
    if (!runtime)
        return;

    bool any_deferred = false;
    for (int max_slot = comm_max_slot(), slot = 0; slot < max_slot; ++slot) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        if (!comm || !(comm->flags & C_DEFERRED_INBOUND))
            continue;

        // A completion callback wakes the event loop before the worker clears
        // its one in-flight slot state. Keep the transport/parser paused until
        // that hook has returned.
        if (mudmux_execution_slot_busy(slot)) {
            any_deferred = true;
            continue;
        }

        comm->flags &= ~C_DEFERRED_INBOUND;
        const comm_process_result_t process_result = comm_process_input(runtime, comm);
        if (process_result == COMM_PROCESS_CLOSED || process_result == COMM_PROCESS_ERROR) {
            (void) comm_close(runtime, slot);
            continue;
        }
        if (comm && (comm->flags & C_DEFERRED_INBOUND))
            any_deferred = true;
    }

    has_deferred_input.store(any_deferred, std::memory_order_release);
}

static void _resume_input_after_transport_ready_hook(void*, int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return;

    // If close was requested while the lifecycle hook ran, its queued
    // HOOK_DISCONNECT now owns this shared await flag until it completes.
    if (!(comm->flags & C_CLOSING))
        comm->flags &= ~C_AWAITING_HOOK;
    comm->flags |= C_DEFERRED_INBOUND;
    has_deferred_input.store(true, std::memory_order_release);
}

void comm_invoke_transport_ready(async_runtime_t* runtime, int slot) {
    if (!runtime || slot < 0)
        return;

    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm || (comm->flags & (C_CLOSING | C_TRANSPORT_READY)))
        return;
    if (comm->ssl && !(comm->flags & C_TLS_ESTABLISHED))
        return;
    // A WebSocket becomes application-ready only after the HTTP 101 response
    // (and, for Telnet-over-WebSocket, the initial Telnet bytes) have drained.
    if ((comm->flags & C_ENABLE_WEBSOCKET) && !C_WEBSOCKET_IS_READY(comm->flags))
        return;

    comm->flags |= C_TRANSPORT_READY;
    const bool await_ready_hook = mudmux_execution_should_dispatch_async(HOOK_TRANSPORT_READY);
    if (await_ready_hook)
        comm->flags |= C_AWAITING_HOOK;

    const mudmux_dispatch_result_t result = mudmux_dispatch_hook_after(
        HOOK_TRANSPORT_READY,
        async_runtime_get_context(runtime),
        slot,
        nullptr,
        0,
        await_ready_hook ? _resume_input_after_transport_ready_hook : nullptr,
        nullptr,
        slot);
    if (result != MUDMUX_DISPATCH_OK) {
        comm->flags &= ~(C_TRANSPORT_READY | C_AWAITING_HOOK);
    }
}

static void _resume_input_after_connect_hook(void* context, int slot) {
    async_runtime_t* runtime = static_cast<async_runtime_t*>(context);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return;

    // The connect task is complete before transport-ready is queued.  This
    // lets the latter occupy the same slot's normal serialized lifecycle lane.
    if (!(comm->flags & C_CLOSING))
        comm->flags &= ~C_AWAITING_HOOK;
    comm_invoke_transport_ready(runtime, slot);
    comm->flags |= C_DEFERRED_INBOUND;
    has_deferred_input.store(true, std::memory_order_release);
}

int comm_invoke_connect (async_runtime_t* runtime, int slot, int entry_slot) {
    if (!runtime)
        return -1;
    std::string entry_name;
    if (entry_slot == COMM_SLOT_CONSOLE) {
        entry_name = "-";
    } else {
        comm_abstract_ptr comm(entry_slot, comm_slots_mtx);
        if (comm && (comm->flags & C_SOCKET_LISTENING)) {
            entry_name = BIO_get_accept_name(comm->rbio);
            entry_name += ":";
            entry_name += BIO_get_accept_port(comm->rbio);
        }
    }
    if (entry_name.empty())
        entry_name = "unknown";
    assert(!entry_name.empty());
    const bool await_connect_hook = mudmux_execution_should_dispatch_async(HOOK_CONNECT);
    if (await_connect_hook) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        if (comm)
            comm->flags |= C_AWAITING_HOOK;
    }

    const mudmux_dispatch_result_t result = mudmux_dispatch_hook_after(
        HOOK_CONNECT,
        async_runtime_get_context(runtime),
        slot,
        entry_name.data(),
        entry_name.size(),
        await_connect_hook ? _resume_input_after_connect_hook : nullptr,
        await_connect_hook ? runtime : nullptr,
        slot);
    if (await_connect_hook && result != MUDMUX_DISPATCH_OK) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        if (comm)
            comm->flags &= ~C_AWAITING_HOOK;
    }
    if (!await_connect_hook)
        comm_invoke_transport_ready(runtime, slot);
    return static_cast<int>(result);
}

static void _resume_input_after_inbound_hook(void*, int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return;

    // The hook has now had an opportunity to choose the input mode. Resume
    // buffered bytes using that post-hook mode.
    comm->flags |= C_DEFERRED_INBOUND;
    has_deferred_input.store(true, std::memory_order_release);
}

int comm_invoke_inbound_message (async_runtime_t* runtime, comm_abstract_ptr& comm, const void* data, size_t size) {
    if (!runtime || !comm || !data) {
        return -1;
    }

    comm->flags &= ~C_INVOKED_PROMPT; // reset C_INVOKED_PROMPT flag on inbound message
    SPDLOG_TRACE ("invoking inbound message hook for slot {} with {} bytes of data", comm.slot(), size);
    const bool await_inbound_hook = mudmux_execution_should_dispatch_async(HOOK_MESSAGE_INBOUND);
    if (await_inbound_hook) {
        comm->flags |= C_DEFERRED_INBOUND;
        has_deferred_input.store(true, std::memory_order_release);
    }

    const mudmux_dispatch_result_t result = mudmux_dispatch_hook_after(
        HOOK_MESSAGE_INBOUND,
        async_runtime_get_context(runtime),
        comm.slot(),
        data,
        size,
        await_inbound_hook ? _resume_input_after_inbound_hook : nullptr,
        nullptr,
        comm.slot());
    return static_cast<int>(result);
}

static int _invoke_char_input_message(async_runtime_t* runtime, comm_abstract_ptr& comm, const void* data, size_t size) {
    if (!runtime || !comm || !data)
        return -1;

    comm->flags &= ~C_INVOKED_PROMPT;
    SPDLOG_TRACE("invoking single-character inbound message hook for slot {} with {} bytes of data", comm.slot(), size);
    const int result = comm_invoke_inbound_message(runtime, comm, data, size);
    if (!mudmux_execution_should_dispatch_async(HOOK_MESSAGE_INBOUND))
        _resume_input_after_inbound_hook(nullptr, comm.slot());
    return result;
}

void comm_free_inbound_buffers(comm_abstract_ptr& comm) {
    if (comm) {
        while (comm->inbound) {
            inbound_buffer_t* next = comm->inbound->next;
            free_inbound_buffer(comm->inbound);
            comm->inbound = next;
        }
        assert(comm->inbound == nullptr);
    }
}

enum class refill_status_t {
    no_data,
    data,
    closed
};

static bool _refill_inbound_buffers_from_src(comm_abstract_ptr& comm, const char* src, size_t size) {
    if (!comm)
        return false;

    async_runtime_t* runtime = async_get_current_runtime();
    size_t remaining = size;
    bool telnet_parsing_paused = false;
    inbound_buffer_t* ibb = comm->inbound;
    if (!ibb) {
        // lazy allocation for the first inbound buffer if it doesn't exist yet
        // (this is not freed until the comm slot is closed or explicitly freed)
        ibb = comm->inbound = allocate_inbound_buffer();
    } else {
        // find first empty buffer or the last buffer in the chain to append data
        while (ibb && ibb->next) {
            if (ibb->end == ibb->start)
                break;
            ibb = ibb->next; // move to the last buffer in the chain
        }
    }
    assert ((ibb == nullptr) || (ibb->next == nullptr) || (ibb->end == ibb->start));

    // fill inbound buffer with data from src, growing the buffer chain only for
    // non-data bands (e.g. Telnet subnegotiation) data
    while (ibb && remaining > 0) {
        if (ibb->start >= ibb->end)
            ibb->start = ibb->end = 0; // reset empty buffer
        if (ibb->end == sizeof(ibb->buffer) && ibb->start > 0) {
            memmove(ibb->buffer, ibb->buffer + ibb->start, ibb->end - ibb->start);
            ibb->end -= ibb->start;
            ibb->start = 0; // make room from a partially consumed buffer by shifting data to the beginning of the buffer
        }
        if (ibb->band != INBOUND_BAND_DATA) {
            ibb = ibb->next;
            continue;
        }
        assert (ibb->band == INBOUND_BAND_DATA);
        size_t bytes_to_copy = std::min(remaining, sizeof(ibb->buffer) - ibb->end);
        if (!bytes_to_copy)
            break; // no more space to copy data into the current buffer
        size_t bytes_copied = 0;
        const bool websocket_handshake_pending =
            (comm->flags & C_ENABLE_WEBSOCKET) && !C_WEBSOCKET_IS_READY(comm->flags);
        // WebSocket frames must remain intact until the WebSocket decoder has
        // removed framing and client masking.  For Telnet-over-WebSocket, the
        // decoded payload is passed through the Telnet parser later in
        // _dispatch_websocket_telnet_payload().
        const bool websocket_ready = C_WEBSOCKET_IS_READY(comm->flags);
        if ((comm->flags & C_ENABLE_TELNET) && !websocket_handshake_pending && !websocket_ready && !telnet_parsing_paused) {
            // copy telnet data from src, preserving telnet state in comm flags
            uint32_t state = comm->flags & M_TELNET_STATE; // restore saved telnet state from comm flags
            comm_telnet_negotiation_t telnet_neg;
            memset(&telnet_neg, 0, sizeof(telnet_neg));
            size_t bytes_consumed = 0;
            bytes_copied = comm_telnet_process_inbound(
                ibb->buffer + ibb->end, const_cast<char*>(src), bytes_to_copy, &bytes_consumed,
                &state, &telnet_neg);
            bytes_to_copy = bytes_consumed; // actual number of raw data consumed from src
            comm->flags = (comm->flags & ~M_TELNET_STATE) | (state & M_TELNET_STATE); // save telnet state back to comm flags
            if (bytes_consumed > bytes_copied) {
                comm_process_telnet_options(comm, &telnet_neg); // update client capabilities based on WILL/WONT claims
            }

            if (telnet_neg.sb_len > 0) {
                const mudmux_dispatch_result_t dispatch_result = comm_dispatch_telnet_subnegotiation(runtime, comm, telnet_neg);
                if (dispatch_result == MUDMUX_DISPATCH_QUEUE_FULL) {
                    telnet_parsing_paused = true;
                    comm->flags |= C_DEFERRED_INBOUND;
                    has_deferred_input.store(true, std::memory_order_release);
                }
            }
        } else {
            // default: copy raw data bytes
            memcpy(ibb->buffer + ibb->end, src, bytes_to_copy);
            bytes_copied = bytes_to_copy;
        }
        ibb->end += bytes_copied; // actually copied bytes into the inbound buffer
        src += bytes_to_copy; // advance source pointer by the number of bytes consumed
        remaining -= bytes_to_copy; // decrease remaining bytes to copy from source
    }

    if ((comm->flags & C_ENABLE_WEBSOCKET) && !C_WEBSOCKET_IS_READY(comm->flags)) {
        comm_try_upgrade_websocket(runtime, comm);
    }

    if (remaining > 0)
        SPDLOG_WARN ("inbound buffers full, {} bytes discarded", remaining);
    return (remaining == 0);
}

bool comm_refill_inbound_buffers (comm_abstract_ptr& comm, const char* src, size_t size) {
    if (!comm)
        return false;

    if (comm->ssl) {
        BIO* tls_rbio = SSL_get_rbio(comm->ssl);
        if (!tls_rbio) {
            SPDLOG_ERROR("TLS enabled slot {} has no SSL rbio", comm.slot());
            return false;
        }

        const bool tls_rbio_is_mem = (BIO_method_type(tls_rbio) == BIO_TYPE_MEM);

        bool fed_tls_ciphertext = false;

        if (src && size > 0 && tls_rbio_is_mem) {
            const int written = BIO_write(tls_rbio, src, static_cast<int>(size));
            if (written <= 0 && !BIO_should_retry(tls_rbio)) {
                SPDLOG_ERROR("failed to feed TLS ciphertext into SSL rbio for slot {}", comm.slot());
                return false;
            }
            fed_tls_ciphertext = (written > 0);
        } else if (!src && tls_rbio_is_mem) {
            std::array<char, 4096> cipher_data{};
            for (;;) {
                size_t bytes_read = 0;
                if (!BIO_read_ex(comm->rbio, cipher_data.data(), sizeof(cipher_data), &bytes_read)) {
                    if (!BIO_should_retry(comm->rbio))
                        return false;
                    break;
                }
                if (bytes_read == 0)
                    break;

                const int written = BIO_write(tls_rbio, cipher_data.data(), static_cast<int>(bytes_read));
                if (written <= 0 && !BIO_should_retry(tls_rbio)) {
                    SPDLOG_ERROR("failed to queue transport bytes into TLS rbio for slot {}", comm.slot());
                    return false;
                }
                if (written > 0)
                    fed_tls_ciphertext = true;
            }
        }

        // Avoid retrying SSL_do_handshake() in a no-progress loop when there is no
        // new ciphertext available on the read path for memory-BIO transport mode;
        // writable events drive WANT_WRITE.
        if (tls_rbio_is_mem && !fed_tls_ciphertext && !src && !(comm->flags & C_TLS_ESTABLISHED))
            return true;

        async_runtime_t* runtime = async_get_current_runtime();
        if (!(comm->flags & C_TLS_ESTABLISHED)) {
            for (;;) {
                int hs = comm_tls_handshake_step(runtime, comm.slot());
                if (hs < 0)
                    return false;
                if (hs == 1)
                    break;

                if (comm->wbio)
                    BIO_flush(comm->wbio);

                // Keep stepping while there is still buffered ciphertext in SSL rbio;
                // this avoids stalling when one SSL_do_handshake() call does not
                // consume all bytes fed from the transport.
                if (BIO_ctrl_pending(tls_rbio) == 0)
                    return true;
            }
        }

        std::array<char, 4096> plain_data{};
        for (;;) {
            size_t out_len = 0;
            int ret = SSL_read_ex(comm->ssl, plain_data.data(), plain_data.size(), &out_len);
            if (ret == 1) {
                if (out_len == 0)
                    return true;
                if (!_refill_inbound_buffers_from_src(comm, plain_data.data(), out_len))
                    return false;

                // Keep draining only decrypted bytes already buffered in SSL.
                // If none are pending, return to the event loop instead of issuing
                // another transport read from this call path.
                if (!SSL_has_pending(comm->ssl))
                    return true;
                continue;
            }

            const int ssl_err = SSL_get_error(comm->ssl, ret);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
                return true;
            if (ssl_err == SSL_ERROR_ZERO_RETURN)
                return false;

            SPDLOG_ERROR("SSL_read_ex failed for slot {} (ssl_err={})", comm.slot(), ssl_err);
            return false;
        }
    }

    if (src)
        return _refill_inbound_buffers_from_src(comm, src, size);

    // refill from underlying BIO if src is nullptr
    std::array<char, 4096> raw_data{};
    size_t bytes_read = 0;
    if (BIO_read_ex(comm->rbio, raw_data.data(), sizeof(raw_data), &bytes_read)) {
        if (bytes_read > 0)
            return comm_refill_inbound_buffers(comm, raw_data.data(), bytes_read);
        return true;
    }
    return BIO_should_retry(comm->rbio); // return true if data read or no data, false if connection closed
}

bool comm_append_decoded_input(comm_abstract_ptr& comm, const char* src, size_t size) {
    if (!comm || (!src && size != 0))
        return false;
    return _refill_inbound_buffers_from_src(comm, src, size);
}

/**
 * @brief Find the first newline sequence (LF, NUL, CR LF, CR NUL) in the inbound buffer
 * and strip it by removing newline, leading and trailing whitespaces.
 * Stripped characters are replaced with null characters.
 *
 * If newline or null character is found, adjusts the start index of the inbound buffer to
 * point to the first character of the line after stripping (always null-terminated).
 *
 * If no newline is found, returns -1 and does not modify the inbound buffer.
 *
 * @param ibb Pointer to the inbound buffer.
 * @return Index of the beginning of next line, or -1 if no newline found.
 */
static ssize_t _find_newline_and_strip (inbound_buffer_t* ibb, size_t* line_len = nullptr) {
    if (!ibb || ibb->start >= ibb->end)
        return -1;
    SPDLOG_TRACE ("searching for newline in inbound buffer: start={}, end={}", ibb->start, ibb->end);
    for (size_t i = ibb->start; i < ibb->end; ++i) {
        if (ibb->buffer[i] == '\r' || ibb->buffer[i] == '\n' || ibb->buffer[i] == '\0') {
            // Accept CRLF, CRNUL, LF, NUL, and bare CR line endings.
            size_t line_end = i;
            ssize_t ret = static_cast<ssize_t>(i + 1);
            if (ibb->buffer[i] == '\r' && i + 1 < ibb->end && (ibb->buffer[i + 1] == '\n' || ibb->buffer[i + 1] == '\0')) {
                ibb->buffer[i + 1] = '\0';
                ret = static_cast<ssize_t>(i + 2);
            } else if ((ibb->buffer[i] == '\n' || ibb->buffer[i] == '\0') && i > ibb->start && ibb->buffer[i - 1] == '\r') {
                line_end = i - 1;
            }
            ibb->buffer[line_end] = '\0';
            // strip leading and trailing whitespace
            while (ibb->start < line_end && isspace(static_cast<unsigned char>(ibb->buffer[ibb->start]))) {
                ibb->buffer[ibb->start++] = '\0';
            }
            while (line_end > ibb->start && isspace(static_cast<unsigned char>(ibb->buffer[line_end - 1]))) {
                ibb->buffer[--line_end] = '\0';
            }
            SPDLOG_TRACE ("stripped line: [{}], length={}, next_line_start={}", ibb->buffer + ibb->start, line_end - ibb->start, ret);
            if (line_len) {
                *line_len = line_end - ibb->start; // length of stripped line
            }
            return ret;
        }
    }
    return -1;
}

/**
 * @brief Find the next character input sequence (terminal input sequence, UTF-8 char) in
 * the inbound buffer.
 * If found, returns the index of the first character after the sequence.
 * If not found, returns -1. The ANSI sequence is defined as starting with ESC '['
 * and ending with a letter (A-Z or a-z) or a tilde (~). The sequence may contain
 * digits and semicolons in between. Otherwise, it is treated as a UTF-8 character.
 * 
 * See https://en.wikipedia.org/wiki/ANSI_escape_code for more details on ANSI escape sequences.
 *
 * @param ibb Pointer to the inbound buffer.
 * @param char_len Optional pointer to size_t to receive the length of the ANSI sequence.
 * @return Index of the first character after the ANSI sequence, or -1 if not found
 */
static ssize_t _find_char_input_sequence (inbound_buffer_t* ibb, uint32_t comm_flags, size_t* char_len = nullptr) {
    if (!ibb || ibb->start >= ibb->end)
        return -1;
    SPDLOG_TRACE ("searching for character input sequence in inbound buffer: start={}, end={}", ibb->start, ibb->end);
    if (comm_flags & C_ENABLE_ANSI) {
        size_t i = ibb->start;
        if (ibb->buffer[i] == '\x1B') { // ESC
            // ANSI escape sequence starts with ESC
            if (++i >= ibb->end)
                return -1; // incomplete ANSI sequence, wait for more data
            if (ibb->buffer[i] == '[') { // ESC '['
                // skip digits and semicolons (optional parameters of the ANSI sequence)
                while (++i < ibb->end && (isdigit(static_cast<unsigned char>(ibb->buffer[i])) || ibb->buffer[i] == ';'));
                if (i >= ibb->end)
                    return -1; // incomplete ANSI sequence, wait for terminal character
                if ((ibb->buffer[i] >= 'A' && ibb->buffer[i] <= 'Z') || (ibb->buffer[i] >= 'a' && ibb->buffer[i] <= 'z') || ibb->buffer[i] == '~') {
                    ++i;
                    // valid ANSI control sequence found
                    if (char_len) {
                        *char_len = i - ibb->start; // length of the ANSI sequence
                    }
                    return static_cast<ssize_t>(i); // return index of first character after the ANSI sequence
                }
                // not a valid ANSI sequence, treat ESC as a single character
                if (char_len) {
                    *char_len = 1;
                }
                return static_cast<ssize_t>(ibb->start + 1);
            }
        }
    }
    // Not an ANSI control sequence, treat as a multibyte character (UTF-8) or
    // single byte if not valid UTF-8 (including ESC followed by non-ANSI sequence).
    mbstate_t state{};
    size_t mbc_len = mbrlen(ibb->buffer + ibb->start, ibb->end - ibb->start, &state);
    if (mbc_len == static_cast<size_t>(-2)) {
        // Incomplete multibyte character, wait for more data
        return -1;
    }    
    mbc_len = (mbc_len == static_cast<size_t>(-1)) ? 1 : mbc_len; // treat invalid multibyte as single byte
    if (char_len) {
        *char_len = static_cast<size_t>(mbc_len);
    }
    return static_cast<ssize_t>(ibb->start + mbc_len);
}

static inbound_buffer_t* _skip_non_data_head_buffers(comm_abstract_ptr& comm) {
    inbound_buffer_t* ibb = comm ? comm->inbound : nullptr;
    while (ibb && ibb->band != INBOUND_BAND_DATA) {
        comm->inbound = ibb->next;
        free_inbound_buffer(ibb);
        ibb = comm->inbound;
    }
    return ibb;
}

static inbound_buffer_t* _recycle_current_inbound_buffer(comm_abstract_ptr& comm, inbound_buffer_t* ibb) {
    if (!comm || !ibb)
        return nullptr;

    // recycle current buffer by moving it to the tail of the chain, keeping size of chain unchanged
    comm->inbound = ibb->next;
    inbound_buffer_t** tail = &comm->inbound;
    while (*tail)
        tail = &(*tail)->next;
    assert(*tail == nullptr);
    *tail = ibb->reset();
    return _skip_non_data_head_buffers(comm);
}

static comm_process_result_t _comm_process_input (async_runtime_t* runtime, comm_abstract_ptr& comm,
                                                   int max_message, bool decode_transport) {
    if (!comm)
        return COMM_PROCESS_ERROR;
    if (comm->flags & C_AWAITING_HOOK)
        return COMM_PROCESS_DEFERRED;
    if (mudmux_execution_slot_busy(comm.slot()))
        return COMM_PROCESS_DEFERRED;
    comm->flags &= ~C_DEFERRED_INBOUND;
    inbound_buffer_t* ibb = _skip_non_data_head_buffers(comm);
    int num_messages_processed = 0;
    if ((comm->flags & C_ENABLE_WEBSOCKET) && !C_WEBSOCKET_IS_READY(comm->flags)) {
        // Input may have arrived while the asynchronous connect hook was still
        // configuring the slot. In that case it is already buffered when the
        // hook enables WebSocket support, so retry the upgrade here rather than
        // waiting for another socket read.
        comm_try_upgrade_websocket(runtime, comm);
        // A rejected handshake closes and removes the slot.  Do not continue
        // through the input state machine with the now-invalid guard.
        if (!comm)
            return COMM_PROCESS_CLOSED;
        if (!C_WEBSOCKET_IS_READY(comm->flags))
            return COMM_PROCESS_OK;
    }

    if (decode_transport && C_WEBSOCKET_IS_READY(comm->flags)) {
        return comm_process_websocket_input(runtime, comm, max_message, num_messages_processed);
    }
    else if (comm->flags & C_LINE_INPUT) {
        //
        // [LINE INPUT MODE] process input data line by line, invoking the inbound message hook for each complete line
        //
        ssize_t next_line_start; // index of next line start, or -1 if no complete line found
        size_t line_len;
        while (ibb && (max_message < 0 || num_messages_processed < max_message)) {
            if (ibb->end <= ibb->start)
                break; // no more data in the current buffer, wait for more data
            if ((next_line_start = _find_newline_and_strip(ibb, &line_len)) >= 0) {
                if (comm->flags & C_ENABLE_TELNET) {
                    if (!(comm->flags & C_CLOSING))
                        comm_buffered_write_comm(comm, "\r\n", 2); // echo newline for Telnet clients
                }
                // invoke inbound message hook for each complete line
                const mudmux_dispatch_result_t dispatch_result = static_cast<mudmux_dispatch_result_t>(
                    comm_invoke_inbound_message(runtime, comm, ibb->buffer + ibb->start, line_len));
                if (dispatch_result == MUDMUX_DISPATCH_QUEUE_FULL) {
                    comm->flags |= C_DEFERRED_INBOUND;
                    has_deferred_input.store(true, std::memory_order_release);
                    break;
                }
                ibb->start = static_cast<size_t>(next_line_start);
                ++num_messages_processed;
                if (!comm)
                    break; // comm slot may have been closed by the inbound message hook
                if (mudmux_execution_slot_busy(comm.slot()))
                    break; // do not parse another message before its hook returns
            }
            SPDLOG_DEBUG ("next_line_start={}, ibb->start={}, ibb->end={}", next_line_start, ibb->start, ibb->end);
            size_t space = sizeof(ibb->buffer) - (ibb->end - ibb->start);
            if (next_line_start < 0 && space == 0)
                break; // partial line filled the current buffer; no forward progress is possible in this pass
            if (next_line_start < 0 || static_cast<size_t>(next_line_start) < ibb->end) {
                // (the line is not complete, or there is still data in the current buffer)
                // attempt to refill current buffer from the next buffer in the chain if available
                if (ibb->next && (ibb->next->end - ibb->next->start) > 0) { // data available in next buffer
                    inbound_buffer_t* next_buffer = ibb->next;
                    switch (next_buffer->band) {
                    case INBOUND_BAND_DATA: {
                        size_t next_len = next_buffer->end - next_buffer->start;
                        size_t copy_len = std::min(space, next_len);
                        if (ibb->start > 0) {
                            // shift existing data to the beginning of the buffer
                            memmove(ibb->buffer, ibb->buffer + ibb->start, ibb->end - ibb->start);
                            ibb->end -= ibb->start;
                            ibb->start = 0;
                        }
                        // fill the remaining space in the current buffer with data from the next buffer
                        memcpy(ibb->buffer + ibb->end, next_buffer->buffer + next_buffer->start, copy_len);
                        ibb->end += copy_len;
                        next_buffer->start += copy_len;
                        if (next_buffer->start == next_buffer->end) {
                            // (next buffer is now empty)
                            // move next buffer to the tail of chain, keeping size of chain unchanged
                            inbound_buffer_t* empty_buffer = next_buffer;
                            ibb->next = empty_buffer->next;
                            inbound_buffer_t** tail = &comm->inbound;
                            while (*tail)
                                tail = &(*tail)->next;
                            *tail = empty_buffer->reset();
                        }
                        continue; // try to find a complete line again after moving data from next buffer
                    }
                    case INBOUND_BAND_SUBNEG:
                    default:
                        // skip subnegotiation buffer and continue processing the next data buffer
                        ibb->next = next_buffer->next;
                        free_inbound_buffer(next_buffer);
                        continue;
                    }
                }
                if (next_line_start < 0)
                    break; // no complete line and no way to make progress this round
            }
            else {
                ibb = _recycle_current_inbound_buffer(comm, ibb); // move to the next data buffer in the chain
                if (!ibb || ibb->end <= ibb->start)
                    break; // no more data in the current buffer, wait for more data
                continue;
            }
        }
    }
    else {
        //
        // [CHAR MODE] process input data as single character (or ANSI control sequence, UTF-8 character)
        //
        ssize_t next_char_start; // index of next character start, or -1 if no complete character sequence found
        size_t char_len;
        while (ibb && (max_message < 0 || num_messages_processed < max_message)) {
            if ((next_char_start = _find_char_input_sequence(ibb, comm->flags, &char_len)) >= 0) {
                // Character input is one-shot: return to line input before dispatching
                // so a hook must explicitly re-arm character input for the next message.
                if (!comm_set_line_input(comm.slot(), true))
                    return COMM_PROCESS_ERROR;
                const mudmux_dispatch_result_t dispatch_result = static_cast<mudmux_dispatch_result_t>(
                    ((comm->flags & C_ENABLE_TELNET) && mudmux_execution_should_dispatch_async(HOOK_MESSAGE_INBOUND))
                        ? comm_invoke_inbound_message(runtime, comm, ibb->buffer + ibb->start, char_len)
                        : _invoke_char_input_message(runtime, comm, ibb->buffer + ibb->start, char_len));
                if (dispatch_result == MUDMUX_DISPATCH_QUEUE_FULL) {
                    comm->flags |= C_DEFERRED_INBOUND;
                    has_deferred_input.store(true, std::memory_order_release);
                    break;
                }
                ibb->start = static_cast<size_t>(next_char_start);
                ++num_messages_processed;
                if (!comm)
                    break; // comm slot may have been closed by the inbound message hook
                break; // Do not dispatch additional buffered characters before the hook re-arms char input.
            }
            SPDLOG_TRACE ("next_char_start={}, ibb->start={}, ibb->end={}", next_char_start, ibb->start, ibb->end);
            if (next_char_start < 0)
                break; // incomplete character sequence, wait for more data
            if (static_cast<size_t>(next_char_start) < ibb->end)
                continue; // there is still data in the current buffer, continue until max_message

            ibb = _recycle_current_inbound_buffer(comm, ibb); // move to the next data buffer in the chain
            if (!ibb || ibb->end <= ibb->start)
                break; // no more data in the current buffer, wait for more data
            continue;
        }
    }

    if (!comm)
        return COMM_PROCESS_CLOSED;
    if (comm->flags & C_DEFERRED_INBOUND)
        return COMM_PROCESS_DEFERRED;
    return COMM_PROCESS_OK;
}

comm_process_result_t comm_process_input (async_runtime_t* runtime, comm_abstract_ptr& comm, int max_message) {
    return _comm_process_input(runtime, comm, max_message, true);
}

comm_process_result_t comm_process_decoded_input (async_runtime_t* runtime, comm_abstract_ptr& comm, int max_message) {
    return _comm_process_input(runtime, comm, max_message, false);
}
