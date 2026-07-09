#ifndef COMM_INBOUND_HPP
#define COMM_INBOUND_HPP

#include "abstract.hpp"
#include "async/async_runtime.h"

#include <stddef.h>

void comm_enable_prompt (int slot, bool enable);
void comm_invoke_prompt (async_runtime_t* runtime);

int comm_invoke_connect (async_runtime_t* runtime, int slot);
int comm_invoke_inbound_message (async_runtime_t* runtime, int slot, const void* data, size_t size);

/**
 * @brief Refill the inbound buffer chain for the specified comm slot by reading from the underlying BIO.
 * If src and size are provided, data will be copied from src into the inbound buffer chain instead.
 * @param slot The comm slot to refill.
 * @param src Optional source buffer to copy data from.
 * @param size Size of the source buffer.
 * @return true on success, false on error.
 */
bool comm_refill_inbound_buffers (int slot, const char* src = nullptr, size_t size = 0);

void comm_free_inbound_buffers(comm_abstract_t* comm);

/**
 * @brief Process input data from the specified comm slot's inbound buffer.
 *
 * @param runtime The async runtime context.
 * @param slot The comm slot to process input for.
 * @param max_message Optional maximum number of messages to process; -1 for no limit.
 * @return 0 on success, 1 if the connection was closed, -1 on error.
 */
int comm_process_input (async_runtime_t* runtime, int slot, int max_message = -1);

#endif /* COMM_INBOUND_HPP */
