#ifndef COMM_OUTBOUND_H
#define COMM_OUTBOUND_H

#include "abstract.hpp"

#include "async/async_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void comm_buffered_write (comm_abstract_t *comm, const void *buf, size_t len);

void comm_free_outbound_buffers(comm_abstract_t* comm);

void comm_flush (async_runtime_t* runtime, int slot);

void comm_flush_all (async_runtime_t* runtime);

/**
 * @brief Close a communication slot, removing it from the async runtime and invoking
 * the disconnect hook if necessary.
 * @param runtime The async runtime instance.
 * @param slot The communication slot to close.
 * @return true if the slot was successfully closed or already removed, false if it
 * has buffered data and needs to be flushed first.
 */
bool comm_close(async_runtime_t* runtime, int slot);

int comm_invoke_disconnect (async_runtime_t* runtime, int slot);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* COMM_OUTBOUND_H */
