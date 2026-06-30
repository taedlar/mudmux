#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/mudmux.h"
#include "abstract.h"
#include <string.h>

int comm_write (comm_abstract_t *comm, const void *buf, size_t len) {
    if (!comm || !comm->bio || !buf) {
        return -1; // invalid parameters
    }
    if (len == 0)
        len = strlen(static_cast<const char*>(buf)); // auto-detect length for null-terminated strings
    int written = BIO_write(comm->bio, buf, static_cast<int>(len));
    if (written <= 0) {
        return -1; // write error
    }
    return written;
}
