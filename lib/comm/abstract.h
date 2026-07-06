#ifndef COMM_ABSTRACT_H
#define COMM_ABSTRACT_H

#include <cstdint>
#include <limits>
#include <type_traits>
#include <openssl/bio.h>

#include "async/async_runtime.h"

typedef struct outbound_buffer_s outbound_buffer_t;

typedef struct comm_abstract_s {
    BIO *rbio; // could be null or equal to wbio for bidirectional sockets
    BIO *wbio; // could be null or equal to rbio for bidirectional sockets
    outbound_buffer_t* outbound;
    uint32_t flags;
} comm_abstract_t;
static_assert(std::is_trivially_default_constructible_v<comm_abstract_t>,
    "comm_abstract_t must be trivially default constructible"); // for std::calloc to work correctly
static_assert(std::is_trivially_copyable_v<comm_abstract_t>,
    "comm_abstract_t must be trivially copyable"); // for std::realloc to work correctly

/* socket type BIO helpers */
#ifdef _WIN32
static inline socket_fd_t comm_bio_fd_to_socket_fd (int bio_fd) {
    return static_cast<socket_fd_t>(static_cast<unsigned int>(bio_fd));
}

static inline bool comm_socket_fd_to_bio_fd (socket_fd_t fd, int* out_bio_fd) {
    if (!out_bio_fd || fd == INVALID_SOCKET_FD)
        return false;

    if (fd > static_cast<socket_fd_t>((std::numeric_limits<int>::max)()))
        return false;

    *out_bio_fd = static_cast<int>(fd);
    return true;
}
#else
static inline socket_fd_t comm_bio_fd_to_socket_fd (int bio_fd) {
    return static_cast<socket_fd_t>(bio_fd);
}

static inline bool comm_socket_fd_to_bio_fd (socket_fd_t fd, int* out_bio_fd) {
    if (!out_bio_fd || fd == INVALID_SOCKET_FD)
        return false;

    *out_bio_fd = static_cast<int>(fd);
    return true;
}
#endif

static inline bool comm_bio_get_socket_fd (BIO* bio, socket_fd_t* out_fd) {
    if (!bio || !out_fd)
        return false;

    int bio_fd = -1;
    if (BIO_get_fd(bio, &bio_fd) <= 0 || bio_fd < 0)
        return false;

    *out_fd = comm_bio_fd_to_socket_fd(bio_fd);
    return *out_fd != INVALID_SOCKET_FD;
}



#ifdef __cplusplus
extern "C" {
#endif

int comm_max_slot (void);
int comm_abstract_add_bio (BIO *rbio, BIO *wbio, int slot, uint32_t flags);
BIO* comm_abstract_get_rbio (int slot);
BIO* comm_abstract_get_wbio (int slot);
int comm_abstract_add_file(const char *fn_in, const char* fn_out, int slot, uint32_t flags);
comm_abstract_t* comm_abstract_get (int slot);
int comm_abstract_remove (int slot);
void comm_abstract_cleanup (void);

/* flag management */
uint32_t comm_get_flags (comm_abstract_t *comm);
void comm_set_flags (comm_abstract_t *comm, uint32_t flags);
void comm_clear_flags (comm_abstract_t *comm, uint32_t flags);

/* synchronous I/O, internal use only */
int comm_read (comm_abstract_t *comm, void *buf, size_t len);
int comm_write (comm_abstract_t *comm, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* COMM_ABSTRACT_H */
