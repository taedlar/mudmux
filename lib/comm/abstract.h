#ifndef COMM_ABSTRACT_H
#define COMM_ABSTRACT_H

#include <openssl/bio.h>

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

#ifdef __cplusplus
extern "C" {
#endif

int comm_max_slot (void);
int comm_abstract_add_bio (BIO *rbio, BIO *wbio, int slot, uint32_t flags);
BIO* comm_abstract_get_rbio (int slot);
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
