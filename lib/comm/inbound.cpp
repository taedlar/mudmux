#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define NOMINMAX
#include "inbound.hpp"

#include <algorithm>
#include <cstring>
#include <wchar.h>
#include <openssl/bio.h>

#include "abstract.hpp"
#include "telnet.hpp"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

#ifdef _WIN32
typedef SSIZE_T ssize_t;
#endif

struct inbound_buffer_s {
    inbound_buffer_t* next{nullptr};
    char buffer[4096];
    size_t start{0}; // <-- async read position (data to be processed)
    size_t end{0}; // <-- append new data position (non-blocking)
    inbound_buffer_t* reset () {
        next = nullptr;
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

void comm_enable_prompt (int slot, bool enable) {
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
            mudmux_invoke_hook (
                HOOK_PROMPT,
                async_runtime_get_context(runtime),
                slot,
                nullptr,
                0
            );
            comm->flags |= C_INVOKED_PROMPT;
        }
    }
}

int comm_invoke_connect (async_runtime_t* runtime, int slot, int entry_slot) {
    if (!runtime)
        return -1;
    const char* entry_name = (entry_slot == COMM_SLOT_CONSOLE) ? "-" : nullptr;
    {
        comm_abstract_ptr comm(entry_slot, comm_slots_mtx);
        if (comm && (comm->flags & C_SOCKET_LISTENING))
            entry_name = BIO_get_accept_name(comm->rbio);
    }
    if (!entry_name)
        entry_name = "unknown";
    assert(entry_name != nullptr);
    return mudmux_invoke_hook (
        HOOK_CONNECT,
        async_runtime_get_context(runtime),
        slot,
        static_cast<void*>(const_cast<char*>(entry_name)),
        strlen(entry_name)
	);
}

int comm_invoke_inbound_message (async_runtime_t* runtime, int slot, const void* data, size_t size) {
    if (!runtime || !data || size == 0) {
        return -1;
    }

    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (comm) {
        comm->flags &= ~C_INVOKED_PROMPT; // reset C_INVOKED_PROMPT flag on inbound message
        mudmux_invoke_hook (
            HOOK_MESSAGE_INBOUND,
            async_runtime_get_context(runtime),
            slot,
            const_cast<void*>(data),
            size);
    }
    return 0;
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

/**
 * @brief Refill the inbound buffer chain for the specified comm slot by reading from the underlying BIO.
 * This function will *NOT* grow the inbound buffer chain beyond the initial buffer. If all buffers are
 * full, it will stop reading. If a comm slot needs a larger inbound buffer, setup the buffer chain explicitly
 * before calling this function.
 * @param comm Reference to the comm abstract pointer.
 * @param status Optional pointer to a refill_status_t variable to receive the refill status.
 * @return Pointer to the head of the inbound buffer chain, or nullptr if no buffer is available.
 */
static inbound_buffer_t* _refill_inbound_buffers (comm_abstract_ptr& comm, refill_status_t* status = nullptr) {
    if (status)
        *status = refill_status_t::no_data;
    if (!comm)
        return nullptr;

    inbound_buffer_t* ibb = comm->inbound;
    if (!ibb) {
        // do lazy allocation for the first inbound buffer if it doesn't exist yet
        ibb = comm->inbound = allocate_inbound_buffer();
    }
    inbound_buffer_t* head = ibb;
    while (ibb) {
        if (ibb->end < sizeof(ibb->buffer))
            break; // found a buffer with space to read more data
        ibb = ibb->next; // move to the last buffer in the chain
    }
    assert ((ibb == nullptr) || (ibb->next == nullptr)); // ensure we are at the end of the chain

    if (ibb && ibb->end < sizeof(ibb->buffer)) {
        size_t bytes_read = 0;
        if (BIO_read_ex(comm->rbio, ibb->buffer + ibb->end, sizeof(ibb->buffer) - ibb->end, &bytes_read)) {
            if (comm->flags & C_ENABLE_TELNET) {
                // process Telnet negotiation and strip IAC sequences from the inbound buffer
                uint32_t state = comm->flags & M_TELNET_STATE;
                comm_telnet_negotiation_t telnet_neg;
                memset(&telnet_neg, 0, sizeof(telnet_neg));
                size_t processed_len = comm_telnet_process_inbound(
                    ibb->buffer + ibb->end, ibb->buffer + ibb->end, bytes_read,
                    &state, &telnet_neg); // process the newly read data in place
                bytes_read = processed_len;
                comm->flags = (comm->flags & ~M_TELNET_STATE) | (state & M_TELNET_STATE);
            }
            ibb->end += bytes_read;
            if (bytes_read > 0 && status)
                *status = refill_status_t::data;
        } else if (!BIO_should_retry(comm->rbio)) {
            if (status)
                *status = refill_status_t::closed;
        }
        SPDLOG_DEBUG ("refilled inbound buffers : total_bytes_read={}", bytes_read);
    } else {
        SPDLOG_DEBUG ("inbound buffers are full");
        if (status)
            *status = refill_status_t::no_data;
    }
    return head;
}

bool comm_refill_inbound_buffers (int slot, const char* src, size_t size) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return false;
    if (src && size > 0) {
        // copy data from src into the inbound buffer chain
        size_t remaining = size;
        inbound_buffer_t* ibb = comm->inbound;
        if (!ibb) {
            ibb = comm->inbound = allocate_inbound_buffer();
        }
        while (ibb && ibb->next) {
            ibb = ibb->next; // move to the last buffer in the chain
        }
        while (ibb && remaining > 0) {
            if (ibb->start == ibb->end) {
                ibb->start = 0;
                ibb->end = 0;
            } else if (ibb->end == sizeof(ibb->buffer) && ibb->start > 0) {
                memmove(ibb->buffer, ibb->buffer + ibb->start, ibb->end - ibb->start);
                ibb->end -= ibb->start;
                ibb->start = 0;
            }
            if (ibb->end == sizeof(ibb->buffer) && ibb->next) {
                ibb = ibb->next;
                continue;
            }
            if (ibb->end == sizeof(ibb->buffer)) {
                // current buffer is full and there is no next buffer, cannot read more data
                SPDLOG_WARN ("inbound buffer chain is full, discarded {} bytes of data", remaining);
                break;
            }
            const size_t copy_len = std::min(remaining, sizeof(ibb->buffer) - ibb->end);
            size_t copied = 0;
            if (comm->flags & C_ENABLE_TELNET) {
                // process Telnet negotiation and strip IAC sequences from the source data
                uint32_t state = comm->flags & M_TELNET_STATE;
                comm_telnet_negotiation_t telnet_neg;
                memset(&telnet_neg, 0, sizeof(telnet_neg));
                size_t processed_len = comm_telnet_process_inbound(
                    ibb->buffer + ibb->end, const_cast<char*>(src), copy_len,
                    &state, &telnet_neg);
                copied = processed_len;
                comm->flags = (comm->flags & ~M_TELNET_STATE) | (state & M_TELNET_STATE);
            } else {
                memcpy(ibb->buffer + ibb->end, src, copy_len);
                copied = copy_len;
            }
            ibb->end += copied;
            src += copy_len;
            remaining -= copy_len;
            ibb = ibb->next; // move to the next buffer in the chain if available
        }
        return (remaining == 0); // return true if all data copied, false if some data was discarded
    }

    // refill from underlying BIO if src is nullptr
    refill_status_t status = refill_status_t::no_data;
    _refill_inbound_buffers(comm, &status);
    return (status != refill_status_t::closed); // return true if data read or no data, false if connection closed
}

/**
 * @brief Find the first newline (LF or CR LF) or null character in the inbound buffer
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
    SPDLOG_DEBUG ("searching for newline in inbound buffer: start={}, end={}", ibb->start, ibb->end);
    for (size_t i = ibb->start; i < ibb->end; ++i) {
        if (ibb->buffer[i] == '\n' || ibb->buffer[i] == '\0') {
            // strip CR LF or LF by replacing with null terminators
            ssize_t ret = static_cast<ssize_t>(i + 1);
            if (i > ibb->start && ibb->buffer[i - 1] == '\r') {
                ibb->buffer[i--] = '\0';
            }
            ibb->buffer[i] = '\0';
            // strip leading and trailing whitespace
            while (ibb->start < i && isspace(static_cast<unsigned char>(ibb->buffer[ibb->start]))) {
                ibb->buffer[ibb->start++] = '\0';
            }
            while (i > ibb->start && isspace(static_cast<unsigned char>(ibb->buffer[i - 1]))) {
                ibb->buffer[--i] = '\0';
            }
            SPDLOG_DEBUG ("stripped line: [{}], length={}", ibb->buffer + ibb->start, i - ibb->start);
            if (line_len) {
                *line_len = i - ibb->start; // length of stripped line
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
    SPDLOG_DEBUG ("searching for character input sequence in inbound buffer: start={}, end={}", ibb->start, ibb->end);
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

int comm_process_input (async_runtime_t* runtime, int slot, int max_message) {
    if (!runtime || slot < 0) {
        SPDLOG_ERROR ("comm_process_input() called with invalid parameters: runtime={}, slot={}", (void*)runtime, slot);
        return -1;
    }
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return 1;
    inbound_buffer_t* ibb = comm->inbound;
    int num_messages_processed = 0;
    if (comm->flags & C_LINE_INPUT) {
        //
        // [LINE INPUT MODE] process input data line by line, invoking the inbound message hook for each complete line
        //
        ssize_t next_line_start;
        size_t line_len;
        while (ibb && (max_message < 0 || num_messages_processed < max_message)) {
            if ((next_line_start = _find_newline_and_strip(ibb, &line_len)) >= 0) {
                // invoke inbound message hook for each complete line
                comm_invoke_inbound_message(runtime, slot, ibb->buffer + ibb->start, line_len);
                ibb->start = static_cast<size_t>(next_line_start);
                ++num_messages_processed;
            }
            if (ibb->end == sizeof(ibb->buffer) && ibb->next) {
                // (current buffer is full and there is a next buffer)
                // attempt to move data from next buffer to current buffer if there is space
                size_t space = sizeof(ibb->buffer) - (ibb->end - ibb->start);
                if (space > 0 && ibb->next) {
                    size_t next_len = ibb->next->end - ibb->next->start;
                    size_t copy_len = std::min(space, next_len);
                    if (ibb->start > 0) {
                        // shift existing data to the beginning of the buffer
                        memmove(ibb->buffer, ibb->buffer + ibb->start, ibb->end - ibb->start);
                        ibb->end -= ibb->start;
                        ibb->start = 0;
                    }
                    // fill the remaining space in the current buffer with data from the next buffer
                    memcpy(ibb->buffer + ibb->end, ibb->next->buffer + ibb->next->start, copy_len);
                    ibb->end += copy_len;
                    ibb->next->start += copy_len;
                    if (ibb->next->start == ibb->next->end) {
                        // (next buffer is now empty)
                        // move next buffer to the tail of chain, keeping size of chain unchanged
                        inbound_buffer_t* empty_buffer = ibb->next;
                        ibb->next = empty_buffer->next;
                        while (ibb->next)
                            ibb = ibb->next;
                        ibb->next = empty_buffer->reset();
                    }
                    continue; // try to find a complete line again after moving data from next buffer
                }
            }
            else {
                // no complete line found, break to read more data
                break;
            }
        }
    }
    else {
        //
        // [CHAR MODE] process input data as single character (or ANSI control sequence, UTF-8 character)
        //
        ssize_t next_char_start;
        size_t char_len;
        while (ibb && (max_message < 0 || num_messages_processed < max_message)) {
            if ((next_char_start = _find_char_input_sequence(ibb, comm->flags, &char_len)) >= 0) {
                // invoke inbound message hook for each complete ANSI character sequence
                comm_invoke_inbound_message(runtime, slot, ibb->buffer + ibb->start, char_len);
                ibb->start = static_cast<size_t>(next_char_start);
                ++num_messages_processed;
            }
            else {
                // no complete ANSI character sequence found, break to read more data
                break;
            }
        }
    }

    return comm ? 0 : 1; // return 0 if comm is valid, 1 if comm was closed
}
