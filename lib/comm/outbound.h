#ifndef COMM_OUTBOUND_H
#define COMM_OUTBOUND_H

#include "abstract.h"

#include "async/async_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void comm_buffered_write (comm_abstract_t *comm, const void *buf, size_t len);

void comm_flush (comm_abstract_t *comm, async_runtime_t* runtime);

void comm_flush_all_outbound (async_runtime_t* runtime);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* COMM_OUTBOUND_H */
