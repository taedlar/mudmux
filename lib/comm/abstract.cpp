#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "abstract.h"

#include <cstdlib>
#include <vector>
#include <type_traits>
#include <string.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include "mudmux/comm.h"

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
        all_comms = static_cast<comm_abstract_t*>(std::calloc(max_comms, sizeof(comm_abstract_t)));
        if (!all_comms) {
            SPDLOG_ERROR("failed to allocate memory for comm slots");
            return -1;
        }
    }
    else {
        size_t new_max = max_comms * 2;
        while (new_max < required_slots)
            new_max *= 2; // grow to 128, 256, 512, etc. until it can accommodate required_slots
        comm_abstract_t* new_comms = static_cast<comm_abstract_t*>(std::realloc(all_comms, sizeof(comm_abstract_t) * new_max));
        if (!new_comms) {
            SPDLOG_ERROR("failed to reallocate memory for comm slots");
            return -1;
        }
        memset(new_comms + max_comms, 0, sizeof(comm_abstract_t) * (new_max - max_comms));
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

int comm_max_slot (void) {
    return static_cast<int>(max_comms);
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
    else if (comm_abstract_get(slot)) {
        SPDLOG_WARN ("slot {} is already in use; removing existing comm", slot);
        comm_abstract_remove (slot); // clear existing comm at this slot
    }
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

int comm_abstract_disconnect (int slot) {
    comm_abstract_t* comm = comm_abstract_get (slot);
    if (!comm) {
        SPDLOG_WARN ("invalid slot {} in comm_abstract_disconnect()", slot);
        return -1;
    }
    if (comm->rbio)
        BIO_reset (comm->rbio);
    if (comm->wbio && comm->wbio != comm->rbio)
        BIO_reset (comm->wbio);
    return 0;
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
    SPDLOG_DEBUG ("removed comm slot {}", slot);
    return 0;
}

comm_abstract_t* comm_abstract_get (int slot) {
    if (!all_comms || slot < 0 || slot >= static_cast<int>(max_comms))
        return nullptr;
    comm_abstract_t* comm = &all_comms[slot];
    return (comm->rbio || comm->wbio) ? comm : nullptr;
}

BIO* comm_abstract_get_rbio (int slot) {
    comm_abstract_t* comm = comm_abstract_get(slot);
    return comm ? comm->rbio : nullptr;
}

BIO* comm_abstract_get_wbio (int slot) {
    comm_abstract_t* comm = comm_abstract_get(slot);
    return comm ? comm->wbio : nullptr;
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
