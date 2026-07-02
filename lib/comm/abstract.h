#ifndef COMM_ABSTRACT_H
#define COMM_ABSTRACT_H

#include "mudmux/mudmux.h"
#include <openssl/bio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef __cplusplus
extern "C" {
#endif

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

/* non-blocking I/O */
void comm_buffered_write (comm_abstract_t *comm, const void *buf, size_t len);

/* synchronous I/O, internal use only */
int comm_read (comm_abstract_t *comm, void *buf, size_t len);
int comm_write (comm_abstract_t *comm, const void *buf, size_t len);
void comm_flush (comm_abstract_t *comm);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* COMM_ABSTRACT_H */
