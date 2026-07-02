#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/comm.h"

#include <string.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include <vector>

typedef struct outbound_buffer_s outbound_buffer_t;
struct outbound_buffer_s {
    outbound_buffer_t* next;
    char buffer[4096]; // ring buffer
    size_t start{0};
    size_t end{0};
};
static outbound_buffer_t* outbound_buffer_pool{nullptr};
static const int MAX_OUTBOUND_BUFFERS_PER_SLOT = 16; // maximum number of outbound buffers per comm slot

struct comm_abstract_s {
    BIO *rbio; // could be null or equal to wbio for bidirectional sockets
    BIO *wbio; // could be null or equal to rbio for bidirectional sockets
    outbound_buffer_t* outbound{nullptr};
    uint32_t flags{0};
    bool is_listener;
};

static comm_abstract_t* all_comms{nullptr};
static size_t max_comms{0};

static int comm_abstract_ensure_capacity (size_t required_slots = RESERVED_SLOTS + 1) {
    if (all_comms && max_comms >= required_slots) {
        return 0;
    }

    if (!all_comms) {
        max_comms = 64; // initial capacity: 64 slots (including RESERVED_SLOTS)
        while (max_comms < required_slots)
            max_comms *= 2;
        all_comms = new comm_abstract_t[max_comms];
        memset(all_comms, 0, sizeof(comm_abstract_t) * max_comms);
    }
    else {
        size_t new_max = max_comms * 2;
        while (new_max < required_slots)
            new_max *= 2; // grow to 128, 256, 512, etc. until it can accommodate required_slots
        comm_abstract_t* new_comms = new comm_abstract_t[new_max];
        memset(new_comms, 0, sizeof(comm_abstract_t) * new_max);
        memcpy(new_comms, all_comms, sizeof(comm_abstract_t) * max_comms);
        delete[] all_comms;
        all_comms = new_comms;
        max_comms = new_max;
    }
    return 0;
}

static int comm_abstract_find_slot (void) {
    // find the first available slot after RESERVED_SLOTS
    size_t slot = RESERVED_SLOTS;
    while (slot < max_comms && (all_comms[slot].rbio || all_comms[slot].wbio))
        slot++;
    // if no available slot, expand the array
    if (slot >= max_comms)
        comm_abstract_ensure_capacity (slot + 1);
    return static_cast<int>(slot);
}

int comm_abstract_add_bio (BIO* rbio, BIO* wbio, int slot, uint32_t flags) {
    if (!rbio && !wbio) {
        SPDLOG_WARN ("invalid arguments: both rbio and wbio are null");
        return -1;
    }
    if (comm_abstract_ensure_capacity() < 0)
        return -1;

    if (slot < 0 || slot >= static_cast<int>(max_comms)) {
        slot = comm_abstract_find_slot();
    } 
    else
        comm_abstract_remove (slot); // clear existing comm at this slot
    all_comms[slot].rbio = rbio;
    all_comms[slot].wbio = wbio;
    all_comms[slot].flags = flags;
    return slot;
}

int comm_abstract_add_file (const char* fn_in, const char* fn_out, int slot, uint32_t flags) {
    BIO* bio_in = fn_in ? BIO_new_file(fn_in, "r") : BIO_new_fp (stdin, BIO_NOCLOSE);
    if (!bio_in) {
        SPDLOG_ERROR ("failed to open file {} for reading", fn_in ? fn_in : "stdin");
        return -1;
    }
    BIO* bio_out = fn_out ? BIO_new_file(fn_out, "w") : BIO_new_fp (stdout, BIO_NOCLOSE);
    if (!bio_out) {
        SPDLOG_ERROR ("failed to open file {} for writing", fn_out ? fn_out : "stdout");
        BIO_free_all(bio_in);
        return -1;
    }
    int ret = comm_abstract_add_bio (bio_in, bio_out, slot, flags);
    if (ret < 0) {
        SPDLOG_ERROR ("failed to add communication slot");
        BIO_free_all(bio_in);
        BIO_free_all(bio_out);
        return -1;
    }
    return ret;
}

int comm_abstract_remove (int slot) {
    comm_abstract_t* comm = comm_abstract_get (slot);
    if (!comm) {
        SPDLOG_WARN ("invalid slot {} in comm_abstract_remove()", slot);
        return -1;
    }
    if (comm->rbio)
        BIO_free_all (comm->rbio);
    if (comm->wbio && comm->wbio != comm->rbio)
        BIO_free_all (comm->wbio);
    comm->rbio = comm->wbio = nullptr;
    comm->flags = 0;
    return 0;
}

comm_abstract_t* comm_abstract_get (int slot) {
    if (!all_comms || slot < 0 || slot >= static_cast<int>(max_comms)) {
        return nullptr;
    }
    return &all_comms[slot];
}

BIO* comm_abstract_get_rbio (int slot) {
    comm_abstract_t* comm = comm_abstract_get(slot);
    return comm ? comm->rbio : nullptr;
}

static outbound_buffer_t* allocate_outbound_buffer() {
    if (outbound_buffer_pool) {
        outbound_buffer_t* buffer = outbound_buffer_pool;
        outbound_buffer_pool = outbound_buffer_pool->next;
        buffer->next = nullptr;
        buffer->start = 0;
        buffer->end = 0;
        return buffer;
    }
    return new outbound_buffer_t();
}

static void free_outbound_buffer(outbound_buffer_t* buffer) {
    if (!buffer)
        return;
    buffer->next = outbound_buffer_pool;
    outbound_buffer_pool = buffer;
}

uint32_t comm_get_flags (comm_abstract_t *comm) {
    return comm ? comm->flags : 0;
}

void comm_set_flags (comm_abstract_t *comm, uint32_t flags) {
    if (comm)
        comm->flags |= flags;
}

void comm_clear_flags (comm_abstract_t *comm, uint32_t flags) {
    if (comm)
        comm->flags &= ~flags;
}

void comm_abstract_cleanup (void) {
    comm_abstract_t* comm = all_comms;
    int i = 0;
    while (comm && i < static_cast<int>(max_comms)) {
        if (comm->rbio)
            BIO_free_all (comm->rbio);
        if (comm->wbio && comm->wbio != comm->rbio)
            BIO_free_all (comm->wbio);
        comm->rbio = comm->wbio = nullptr;
        comm++;
        i++;
    }
    delete[] all_comms;
    all_comms = nullptr;
    max_comms = 0;
}

void comm_buffered_write (comm_abstract_t *comm, const void *buf, size_t len) {
    if (!comm || !comm->wbio || !buf || len == 0)
        return; // invalid parameters
    if (comm->outbound) {
        // There is already data in the outbound buffer, append to it
        outbound_buffer_t* buffer = comm->outbound;
        int buffer_count = 1;
        while (buffer->next) {
            buffer = buffer->next;
            buffer_count++;
            if (buffer_count >= MAX_OUTBOUND_BUFFERS_PER_SLOT) {
                SPDLOG_ERROR("Exceeded maximum outbound buffers per slot");
                return;
            }
        }
        size_t space = sizeof(buffer->buffer) - buffer->end;
        if (len > space) {
            // Not enough space in the buffer, attempt to flush first
            comm_flush(comm);
            space = sizeof(buffer->buffer) - buffer->end;
            if (len > space) {
                // Still not enough space, allocate a new buffer
                outbound_buffer_t* new_buffer = allocate_outbound_buffer();
                buffer->next = new_buffer;
                buffer = new_buffer;
            }
        }
        memcpy(buffer->buffer + buffer->end, buf, len);
        buffer->end += len;
        comm_flush(comm); // attempt to flush the buffer
        return;
    }
    int written = BIO_write (comm->wbio, buf, static_cast<int>(len));
    if (written <= 0) {
        SPDLOG_ERROR ("BIO_write failed: {}", ERR_error_string(ERR_get_error(), nullptr));
        // TODO: call a hook function to handle the error, e.g., close the connection
        return;
    }
    if (BIO_should_retry(comm->wbio)) {
        size_t remaining = len - static_cast<size_t>(written);
        if (remaining > 0) {
            comm->outbound = allocate_outbound_buffer();
            comm_buffered_write (comm, static_cast<const char*>(buf) + written, remaining); // buffer the remaining data
        }
    }
    else {
        comm_flush(comm); // flush the BIO if all data was written
    }
}

int comm_read (comm_abstract_t *comm, void *buf, size_t len) {
    if (!comm || !comm->rbio || !buf)
        return -1; // invalid parameters
    return BIO_read (comm->rbio, buf, static_cast<int>(len));
}

int comm_write (comm_abstract_t *comm, const void *buf, size_t len) {
    if (!comm || !comm->wbio || !buf)
        return -1; // invalid parameters
    if (len == 0)
        len = strlen (static_cast<const char*>(buf)); // auto-detect length for null-terminated strings
    return BIO_write (comm->wbio, buf, static_cast<int>(len));
}

void comm_flush (comm_abstract_t *comm) {
    if (!comm || !comm->wbio)
        return; // invalid parameters
    if (comm->outbound) {
        outbound_buffer_t* buffer = comm->outbound;
        while (buffer) {
            size_t data_len = buffer->end - buffer->start;
            if (data_len > 0) {
                int written = BIO_write(comm->wbio, buffer->buffer + buffer->start, static_cast<int>(data_len));
                if (written <= 0) {
                    if (BIO_should_retry(comm->wbio)) {
                        // BIO is not ready for writing, exit the flush loop
                        return;
                    }
                    SPDLOG_ERROR ("BIO_write failed during flush: {}", ERR_error_string(ERR_get_error(), nullptr));
                    return; // stop flushing on error
                }
                buffer->start += static_cast<size_t>(written);
                if (buffer->start >= buffer->end) {
                    // This buffer has been fully written, move to the next
                    outbound_buffer_t* next_buffer = buffer->next;
                    free_outbound_buffer(buffer);
                    comm->outbound = next_buffer;
                    buffer = next_buffer;
                }
            } else {
                // No data left in this buffer, move to the next
                outbound_buffer_t* next_buffer = buffer->next;
                free_outbound_buffer(buffer);
                comm->outbound = next_buffer;
                buffer = next_buffer;
            }
        }
    }
    if (!comm->outbound) {
        // If all outbound buffers have been flushed, we can attempt to flush the BIO
        BIO_flush (comm->wbio);
        // TODO: remove socket from async_runtime write set
    }
}
