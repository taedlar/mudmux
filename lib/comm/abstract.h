#ifndef COMM_ABSTRACT_H
#define COMM_ABSTRACT_H

#include "mudmux_export.h"
#include "async/socket_intf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct comm_abstract_s comm_abstract_t;

MUDMUX_EXPORT int comm_abstract_add (socket_fd_t fd);
MUDMUX_EXPORT int comm_abstract_add_bio (void* bio);
MUDMUX_EXPORT int comm_abstract_accept (int slot);
MUDMUX_EXPORT int comm_abstract_remove (int slot);
MUDMUX_EXPORT comm_abstract_t* comm_abstract_get (int slot);
MUDMUX_EXPORT socket_fd_t comm_abstract_get_fd (comm_abstract_t *comm);
MUDMUX_EXPORT int comm_abstract_is_listener (comm_abstract_t *comm);
MUDMUX_EXPORT void comm_abstract_cleanup (void);

MUDMUX_EXPORT int comm_read (comm_abstract_t *comm, void *buf, size_t len);
MUDMUX_EXPORT int comm_write (comm_abstract_t *comm, const void *buf, size_t len);
MUDMUX_EXPORT void comm_flush (comm_abstract_t *comm);

#ifdef __cplusplus
}
#endif

#endif // COMM_ABSTRACT_H
