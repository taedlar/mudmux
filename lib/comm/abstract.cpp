#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/mudmux.h"
#include "abstract.h"
#include <string.h>
#include <openssl/bio.h>

struct comm_abstract_s {
    BIO *bio;
    bool is_listener;
};

static comm_abstract_t* all_comms{nullptr};
static size_t max_comms{0};

static int comm_abstract_ensure_capacity (void) {
    if (all_comms)
        return 0;

    max_comms = 64;
    all_comms = new comm_abstract_t[max_comms];
    memset(all_comms, 0, sizeof(comm_abstract_t) * max_comms);
    return 0;
}

static int comm_abstract_find_slot (void) {
    size_t slot = 1;
    while (slot < max_comms && all_comms[slot].bio) {
        slot++;
    }
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

int comm_abstract_add_bio (void* bio_ptr) {
    BIO* bio = static_cast<BIO*>(bio_ptr);
    if (!bio) {
        SPDLOG_ERROR ("comm_abstract_add_bio() called with null BIO");
        return -1;
    }

    if (comm_abstract_ensure_capacity() < 0) {
        return -1;
    }

    int slot = comm_abstract_find_slot();
    all_comms[slot].bio = bio;
    all_comms[slot].is_listener = BIO_method_type(bio) == BIO_TYPE_ACCEPT;
    return slot;
}

int comm_abstract_add (socket_fd_t fd) {
    if (comm_abstract_ensure_capacity() < 0) {
        return -1;
    }

    // STDIN is a special case, always use slot 0 for it
    if (fd == STDIN_FILENO) {
        if (all_comms[0].bio) {
            SPDLOG_WARN ("STDIN already registered in comm_abstract_add()");
            return 0; // already registered
        }
        all_comms[0].bio = BIO_new_fp (stdin, BIO_NOCLOSE); // leave stdin open for the lifetime of the program
        if (!all_comms[0].bio) {
            SPDLOG_ERROR ("failed to create BIO for STDIN");
            return -1;
        }
        SPDLOG_INFO ("STDIN registered in comm_abstract_add() at slot 0");
        return 0;
    }

    BIO* bio = BIO_new_fd (fd, BIO_CLOSE); // close fd when BIO is freed
    if (!bio) {
        SPDLOG_ERROR ("failed to create BIO for fd {}", fd);
        return -1;
    }

    int slot = comm_abstract_add_bio(bio);
    if (slot < 0) {
        BIO_free(bio);
        return -1;
    }

    SPDLOG_INFO ("fd {} registered in comm_abstract_add() at slot {}", fd, slot);
    return slot;
}

int comm_abstract_remove (int slot) {
    if (!all_comms || slot >= static_cast<int>(max_comms) || !all_comms[slot].bio) {
        SPDLOG_WARN ("invalid slot {} in comm_abstract_remove()", slot);
        return -1;
    }
    BIO_free (all_comms[slot].bio);
    all_comms[slot].bio = nullptr;
    all_comms[slot].is_listener = false;
    return 0;
}

comm_abstract_t* comm_abstract_get (int slot) {
    if (!all_comms || slot >= static_cast<int>(max_comms) || !all_comms[slot].bio) {
        return nullptr;
    }
    return &all_comms[slot];
}

socket_fd_t comm_abstract_get_fd (comm_abstract_t *comm) {
    if (!comm || !comm->bio) {
        return INVALID_SOCKET_FD;
    }

    socket_fd_t fd = INVALID_SOCKET_FD;
    if (BIO_get_fd(comm->bio, &fd) <= 0) {
        return INVALID_SOCKET_FD;
    }
    return fd;
}

int comm_abstract_is_listener (comm_abstract_t *comm) {
    return comm && comm->bio && comm->is_listener;
}

int comm_abstract_accept (int slot) {
    comm_abstract_t* comm = comm_abstract_get(slot);
    if (!comm || !comm->bio || !comm->is_listener) {
        SPDLOG_WARN ("invalid listener slot {} in comm_abstract_accept()", slot);
        return -1;
    }

    if (BIO_do_accept(comm->bio) <= 0) {
        if (!BIO_should_retry(comm->bio)) {
            SPDLOG_WARN ("BIO_do_accept failed for listener slot {}", slot);
        }
        return -1;
    }

    BIO* accepted_bio = BIO_pop(comm->bio);
    if (!accepted_bio) {
        SPDLOG_WARN ("BIO_pop failed for listener slot {}", slot);
        return -1;
    }

    int accepted_slot = comm_abstract_add_bio(accepted_bio);
    if (accepted_slot < 0) {
        BIO_free(accepted_bio);
        return -1;
    }

    return accepted_slot;
}

void comm_abstract_cleanup (void) {
    if (!all_comms) {
        return;
    }
    for (size_t i = 0; i < max_comms; ++i) {
        if (all_comms[i].bio) {
            BIO_free (all_comms[i].bio);
            all_comms[i].bio = nullptr;
        }
    }
    delete[] all_comms;
    all_comms = nullptr;
    max_comms = 0;
}

int comm_read (comm_abstract_t *comm, void *buf, size_t len) {
    if (!comm || !comm->bio || !buf) {
        return -1; // invalid parameters
    }
    int read_bytes = BIO_read (comm->bio, buf, static_cast<int>(len));
    if (read_bytes <= 0) {
        return -1; // read error or EOF
    }
    return read_bytes;
}

int comm_write (comm_abstract_t *comm, const void *buf, size_t len) {
    if (!comm || !comm->bio || !buf) {
        return -1; // invalid parameters
    }
    if (len == 0)
        len = strlen (static_cast<const char*>(buf)); // auto-detect length for null-terminated strings
    int written = BIO_get_fd(comm->bio, nullptr) == 0 ?
        write (STDOUT_FILENO, buf, len) :  // write to stdout if BIO is stdin
        BIO_write (comm->bio, buf, static_cast<int>(len));
    if (written <= 0) {
        return -1; // write error
    }
    return written;
}

void comm_flush (comm_abstract_t *comm) {
    if (!comm || !comm->bio) {
        return; // invalid parameters
    }
    BIO_flush (comm->bio);
}
