#ifndef COMM_ABSTRACT_H
#define COMM_ABSTRACT_H

#include "mudmux_export.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#ifndef MUDMUX_NO_OPENSSL
#include <openssl/bio.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct comm_abstract_s comm_abstract_t;
enum comm_slot_e {
    COMM_SLOT_CONSOLE = 0,
    COMM_SLOT_FIRST = 1,
    COMM_SLOT_LAST = 1023
};

#ifndef MUDMUX_NO_OPENSSL
MUDMUX_EXPORT int comm_abstract_add_bio (BIO *rbio, BIO *wbio, int slot);
MUDMUX_EXPORT BIO* comm_abstract_get_rbio (int slot);
#endif
MUDMUX_EXPORT comm_abstract_t* comm_abstract_get (int slot);
MUDMUX_EXPORT int comm_abstract_remove (int slot);
MUDMUX_EXPORT void comm_abstract_cleanup (void);

MUDMUX_EXPORT int comm_is_listener (comm_abstract_t *comm);
MUDMUX_EXPORT int comm_read (comm_abstract_t *comm, void *buf, size_t len);
MUDMUX_EXPORT int comm_write (comm_abstract_t *comm, const void *buf, size_t len);
MUDMUX_EXPORT void comm_flush (comm_abstract_t *comm);

#ifdef __cplusplus
}
#endif

#endif // COMM_ABSTRACT_H
