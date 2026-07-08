#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "outbound.hpp"

#include <mutex>
#include <openssl/bio.h>
#include <openssl/err.h>

#include "console.hpp"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

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

void comm_buffered_write (comm_abstract_t *comm, const void *buf, size_t len) {
    std::lock_guard<std::recursive_mutex> lock(mud_logic_mutex);

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
    }
}

void comm_free_outbound_buffers(comm_abstract_t* comm) {
    if (!comm)
        return;
    while (comm->outbound) {
        outbound_buffer_t* next_buffer = comm->outbound->next;
        free_outbound_buffer(comm->outbound);
        comm->outbound = next_buffer;
    }
    assert(comm->outbound == nullptr);
}

void comm_flush (async_runtime_t* runtime, int slot) {
    comm_abstract_ptr comm(slot, mud_logic_mutex);
    if (!comm || !comm->wbio)
        return; // invalid parameters
    outbound_buffer_t* obb = comm->outbound;
    while (obb) {
        size_t data_len = obb->end - obb->start;
        if (data_len > 0) {
            int written = BIO_write(comm->wbio, obb->buffer + obb->start, static_cast<int>(data_len));
            if (written <= 0) {
                if (!BIO_should_retry(comm->wbio)) {
                    SPDLOG_ERROR ("BIO_write failed during flush: {}", ERR_error_string(ERR_get_error(), nullptr));
                    comm_free_outbound_buffers(comm.get()); // drop any buffered outbound data
                    comm->flags &= ~C_BUFFERED_WRITE;
                    if (!(comm->flags & C_CLOSING))
                        async_runtime_post_completion(runtime, ASYNC_IO_ERROR_KEY, static_cast<uintptr_t>(slot));
                }
                break;
            }
            obb->start += static_cast<size_t>(written);
            if (obb->start < obb->end)
                continue; // more data to write
        }
        // No data left in this buffer, move to the next
        outbound_buffer_t* next_buffer = obb->next;
        free_outbound_buffer(obb);
        comm->outbound = next_buffer;
        obb = next_buffer;
    }

    socket_fd_t fd {INVALID_SOCKET_FD};
    if (!comm_bio_get_socket_fd(comm->wbio, &fd)) {
        // This can happen on console user because the wbio is a FILE* (stdout) and BIO does not
        // support BIO_get_fd for FILE* BIOs. In this case, we cannot modify the async runtime
        // events, but we can still flush the data.
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

        if (comm->flags & C_CLOSING) {
            socket_fd_t fd {INVALID_SOCKET_FD};
            if (comm->wbio && comm_bio_get_socket_fd(comm->wbio, &fd)) {
                SPDLOG_DEBUG ("comm slot has C_CLOSING flag set, sending shutdown signal to peer");
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
}

bool comm_close (async_runtime_t* runtime, int slot) {
    comm_abstract_ptr comm(slot, mud_logic_mutex);
    if (!comm)
        return true; // already removed

    if (!runtime)
        runtime = async_get_current_runtime();

    if (!(comm->flags & C_CLOSING)) {
        // let logic layer handle disconnect (e.g., cleanup, logging, etc.)
        comm->flags |= C_CLOSING;
        comm_invoke_disconnect(runtime, slot);
    }

    if (comm->flags & C_BUFFERED_WRITE) {
        SPDLOG_DEBUG("comm slot {} has buffered data, will flush before disconnecting", slot);
        return false;
    }

    socket_fd_t fd {INVALID_SOCKET_FD};
    if (comm->rbio && comm_bio_get_socket_fd(comm->rbio, &fd))
        async_runtime_remove (runtime, fd);
    if (comm->wbio && comm_bio_get_socket_fd(comm->wbio, &fd))
        async_runtime_remove (runtime, fd);

    if (slot == COMM_SLOT_CONSOLE) {
        comm_signal_console_eof(runtime); // let console_process_console_input() handle the disconnect
        SPDLOG_DEBUG("comm slot {} is console, signaling EOF to console worker", slot);
        return false;
    }

    comm_abstract_remove(slot);
    return true;
}

int comm_invoke_disconnect (async_runtime_t* runtime, int slot) {
    return mudmux_invoke_hook (MUDMUX_HOOK_DISCONNECT,
        async_runtime_get_context(runtime),
        slot,
        nullptr,
        0
    );
}
