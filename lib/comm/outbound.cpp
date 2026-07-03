#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "outbound.h"

#include <openssl/bio.h>
#include <openssl/err.h>

#include "mudmux/comm.h"

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
    if (!comm || !comm->wbio || !buf || len == 0)
        return; // invalid parameters
    if (len > sizeof(outbound_buffer_t::buffer) * MAX_OUTBOUND_BUFFERS_PER_SLOT) {
        SPDLOG_ERROR("Attempted to write more data than allowed for buffered write");
        return;
    }
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
        comm_set_flags(comm, C_BUFFERED_WRITE); // data written for flush
    }
}

void comm_flush (comm_abstract_t *comm, async_runtime_t* runtime) {
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
                    // TODO: set error flag and invoke disconnect hook
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

    if (comm->outbound) {
        comm_set_flags(comm, C_SOCKET_WRITABLE); // we shall wait for the socket to be writable to continue flushing
        socket_fd_t fd;
        if (BIO_get_fd(comm->wbio, reinterpret_cast<int*>(&fd)) <= 0 || fd == INVALID_SOCKET_FD) {
            SPDLOG_ERROR("BIO_get_fd failed during flush");
            return;
        }
        async_runtime_add (runtime, fd, EVENT_WRITE, nullptr);
        return;
    }

    // all buffered data flushed, remove writable event if it was set
    if (comm_get_flags(comm) & C_SOCKET_WRITABLE) {
        comm_clear_flags(comm, C_SOCKET_WRITABLE);
        socket_fd_t fd;
        if (BIO_get_fd(comm->wbio, reinterpret_cast<int*>(&fd)) <= 0 || fd == INVALID_SOCKET_FD) {
            SPDLOG_ERROR("BIO_get_fd failed during flush");
            return;
        }
        async_runtime_remove (runtime, fd);
    }

    // clear flushable flag, it will be set until next buffered write
    comm_clear_flags(comm, C_BUFFERED_WRITE);
}

void comm_flush_all_outbound (async_runtime_t* runtime) {
    int max_slot = comm_max_slot();
    for (int slot = 0; slot < max_slot; ++slot) {
        auto* comm = comm_abstract_get(slot);
        if (comm && comm->flags & C_BUFFERED_WRITE) {
            comm_flush (comm, runtime);
        }
    }
}
