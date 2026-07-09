#ifndef COMM_INBOUND_HPP
#define COMM_INBOUND_HPP

#include "abstract.hpp"
#include "async/async_runtime.h"

#include <stddef.h>

void comm_free_inbound_buffers(comm_abstract_t* comm);

void comm_enable_prompt (int slot, bool enable);
void comm_invoke_prompt (async_runtime_t* runtime);

int comm_invoke_connect (async_runtime_t* runtime, int slot);
int comm_invoke_inbound_message (async_runtime_t* runtime, int slot, const void* data, size_t size);

/**
 * @brief Process input data from the specified comm slot's inbound buffer.
 *
 * @param runtime The async runtime context.
 * @param event The I/O event containing the input data.
 * @param slot The comm slot to process input for.
 * @param max_message Optional maximum number of messages to process; -1 for no limit.
 * @return 0 on success, 1 if the connection was closed, -1 on error.
 */
int comm_process_input (async_runtime_t* runtime, const io_event_t* event, int slot, int max_message = -1);

#endif /* COMM_INBOUND_HPP */
