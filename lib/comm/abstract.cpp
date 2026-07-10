#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "abstract.hpp"

#include <cstdlib>
#include <mutex>
#include <vector>
#include <type_traits>
#include <string.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include "inbound.hpp"
#include "outbound.hpp"
#include "mudmux/comm.h"

std::recursive_mutex comm_slots_mtx;

comm_abstract_t* comm_abstract_ptr::all_comms_ = nullptr;
size_t comm_abstract_ptr::max_comms_ = 0;

int comm_abstract_ptr::ensure_capacity(size_t required_slots) {
    if (all_comms_ && max_comms_ >= required_slots) {
        return 0;
    }

    if (!all_comms_) {
        max_comms_ = 64; // initial capacity: 64 slots (including RESERVED_SLOTS)
        while (max_comms_ < required_slots)
            max_comms_ *= 2;
        all_comms_ = static_cast<comm_abstract_t*>(std::calloc(max_comms_, sizeof(comm_abstract_t)));
        if (!all_comms_) {
            SPDLOG_ERROR("failed to allocate memory for comm slots");
            return -1;
        }
    }
    else {
        size_t new_max = max_comms_ * 2;
        while (new_max < required_slots)
            new_max *= 2; // grow to 128, 256, 512, etc. until it can accommodate required_slots
        comm_abstract_t* new_comms = static_cast<comm_abstract_t*>(std::realloc(all_comms_, sizeof(comm_abstract_t) * new_max));
        if (!new_comms) {
            SPDLOG_ERROR("failed to reallocate memory for comm slots");
            return -1;
        }
        memset(new_comms + max_comms_, 0, sizeof(comm_abstract_t) * (new_max - max_comms_));
        all_comms_ = new_comms;
        max_comms_ = new_max;
    }
    return 0;
}

int comm_abstract_ptr::find_slot(void) {
    // find the first available slot after RESERVED_SLOTS
    size_t slot = RESERVED_SLOTS;
    while (slot < max_comms_ && (all_comms_[slot].rbio || all_comms_[slot].wbio))
        slot++;
    // if no available slot, expand the array
    if (slot >= max_comms_)
        ensure_capacity (slot + 1);
    return static_cast<int>(slot);
}

int comm_abstract_ptr::max_slot_count(void) {
    return static_cast<int>(max_comms_);
}

void comm_abstract_ptr::reset_storage(void) {
    std::free(all_comms_); // allocated by calloc/realloc
    all_comms_ = nullptr;
    max_comms_ = 0;
}

int comm_max_slot (void) {
    std::lock_guard<std::recursive_mutex> lock(comm_slots_mtx);
    return comm_abstract_ptr::max_slot_count();
}

int comm_abstract_add_bio (BIO* rbio, BIO* wbio, int slot, uint32_t flags) {
    if (!rbio && !wbio) {
        SPDLOG_WARN ("invalid arguments: both rbio and wbio are null");
        return -1;
    }

    std::lock_guard<std::recursive_mutex> lock(comm_slots_mtx);
    if (comm_abstract_ptr::ensure_capacity() < 0)
        return -1;

    if (slot < 0 || slot >= comm_abstract_ptr::max_slot_count()) {
        slot = comm_abstract_ptr::find_slot();
    } 
    else if (comm_abstract_get(slot)) {
        SPDLOG_WARN ("slot {} is already in use; removing existing comm", slot);
        comm_abstract_remove (slot); // clear existing comm at this slot
    }
    comm_abstract_t* slot_comm = comm_abstract_ptr::slot_ptr_unlocked(slot);
    if (!slot_comm)
        return -1;
    slot_comm->rbio = rbio;
    slot_comm->wbio = wbio;
    slot_comm->flags = flags;
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
    comm_abstract_ptr comm(slot, comm_slots_mtx);
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
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    if (!comm)
        return 0; // already removed or invalid slot
    comm_free_inbound_buffers(comm);
    comm_free_outbound_buffers(comm);

    comm_abstract_t* raw = comm.raw();
    if (!raw)
        return 0;
    if (raw->rbio)
        BIO_free_all (raw->rbio);
    if (raw->wbio && raw->wbio != raw->rbio)
        BIO_free_all (raw->wbio);
    raw->flags = 0;
    raw->rbio = raw->wbio = nullptr; // makes comm_abstract_ptr::get() return nullptr for this slot
    SPDLOG_DEBUG ("removed comm slot {}", slot);
    return 0;
}

void comm_abstract_remove_all (void) {
    std::lock_guard<std::recursive_mutex> lock(comm_slots_mtx);
    if (!comm_abstract_ptr::all_comms_)
        return;
    int i = 0;
    while (i < comm_abstract_ptr::max_slot_count()) {
        comm_abstract_remove (i);
        i++;
    }
    comm_abstract_ptr::reset_storage();
}

comm_abstract_t* comm_abstract_get (int slot) {
    return comm_abstract_ptr::get_slot(slot);
}

bool comm_abstract_has_rbio (int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    return comm.has_rbio();
}

bool comm_abstract_has_wbio (int slot) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    return comm.has_wbio();
}

bool comm_abstract_get_rbio_fd (int slot, socket_fd_t* out_fd) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    return comm.get_rbio_fd(out_fd);
}

bool comm_abstract_get_wbio_fd (int slot, socket_fd_t* out_fd) {
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    return comm.get_wbio_fd(out_fd);
}

uint32_t comm_get_flags (comm_abstract_t *comm) {
    return comm ? comm->flags : 0;
}
