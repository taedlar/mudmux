#ifndef COMM_OUTBOUND_HPP
#define COMM_OUTBOUND_HPP

#include "abstract.hpp"

#include "async/async_runtime.h"

extern "C" void comm_write_message (int from_slot, int to_slot, const void *buf, size_t len);

extern "C" void comm_buffered_write (int slot, const void *buf, size_t len);
void comm_buffered_write_comm (comm_abstract_ptr& comm, const void *buf, size_t len);
/** Queue already-framed transport bytes (used for WebSocket control frames). */
void comm_buffered_write_raw_comm (comm_abstract_ptr& comm, const void *buf, size_t len);

void comm_free_outbound_buffers(comm_abstract_ptr& comm);

/**
 * @brief Flush any buffered outbound data for the specified communication slot.
 */
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

#endif // COMM_OUTBOUND_HPP
