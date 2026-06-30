#ifndef COMM_ABSTRACT_H
#define COMM_ABSTRACT_H

#include "mudmux_export.h"
#include "async/socket_intf.h"

typedef struct comm_abstract_s comm_abstract_t;

MUDMUX_EXPORT int comm_abstract_add (socket_fd_t fd);
MUDMUX_EXPORT int comm_abstract_remove (int slot);
MUDMUX_EXPORT comm_abstract_t* comm_abstract_get (int slot);
MUDMUX_EXPORT void comm_abstract_cleanup (void);

MUDMUX_EXPORT int comm_read (comm_abstract_t *comm, void *buf, size_t len);
MUDMUX_EXPORT int comm_write (comm_abstract_t *comm, const void *buf, size_t len);
MUDMUX_EXPORT void comm_flush (comm_abstract_t *comm);

#endif // COMM_ABSTRACT_H
