#ifndef COMM_ABSTRACT_H
#define COMM_ABSTRACT_H

#include "mudmux_export.h"
#include <openssl/bio.h>

typedef struct comm_abstract_s {
    BIO *bio;
} comm_abstract_t;

MUDMUX_EXPORT int comm_write (comm_abstract_t *comm, const void *buf, size_t len);

#endif // COMM_ABSTRACT_H
