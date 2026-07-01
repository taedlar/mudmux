#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "abstract.h"
#include <string.h>
#include <openssl/bio.h>

struct comm_abstract_s {
    BIO *rbio; // could be null or equal to wbio for bidirectional sockets
    BIO *wbio; // could be null or equal to rbio for bidirectional sockets
    bool is_listener;
};

static comm_abstract_t* all_comms{nullptr};
static size_t max_comms{0};
static const int RESERVED_SLOTS = 1; // reserve slot #0 for console communication

static bool _bio_is_listener (BIO* bio) {
    if (!bio)
        return false;

    // Some platforms/OpenSSL builds may report accept BIOs with composite type bits.
    if (BIO_method_type(bio) == BIO_TYPE_ACCEPT)
        return true;

    const char* method_name = BIO_method_name(bio);
    return method_name && strstr(method_name, "accept");
}

static int comm_abstract_ensure_capacity (void) {
    if (all_comms)
        return 0;

    max_comms = 64;
    all_comms = new comm_abstract_t[max_comms];
    memset(all_comms, 0, sizeof(comm_abstract_t) * max_comms);
    return 0;
}

static int comm_abstract_find_slot (void) {
    // find the first available slot after RESERVED_SLOTS
    size_t slot = RESERVED_SLOTS;
    while (slot < max_comms && (all_comms[slot].rbio || all_comms[slot].wbio))
        slot++;
    // if no available slot, expand the array
    if (slot >= max_comms) {
        size_t new_max = max_comms * 2;
        comm_abstract_t* new_comms = new comm_abstract_t[new_max];
        memset(new_comms, 0, sizeof(comm_abstract_t) * new_max);
        memcpy(new_comms, all_comms, sizeof(comm_abstract_t) * max_comms);
        delete[] all_comms;
        all_comms = new_comms;
        max_comms = new_max;
    }
    return static_cast<int>(slot);
}

int comm_abstract_add_bio (BIO* rbio, BIO* wbio, int slot) {
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
    all_comms[slot].is_listener = [](BIO* bio) {
        if (!bio)
            return false;
        if (BIO_method_type(bio) == BIO_TYPE_ACCEPT)
            return true;
        const char* method_name = BIO_method_name(bio);
        return method_name && strstr(method_name, "accept");
    } (rbio);
    return slot;
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
    comm->is_listener = false;
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

int comm_is_listener (comm_abstract_t *comm) {
    return comm && comm->rbio && comm->is_listener;
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

void comm_flush (comm_abstract_t *comm) {
    if (!comm || !comm->wbio)
        return; // invalid parameters
    BIO_flush (comm->wbio);
}
