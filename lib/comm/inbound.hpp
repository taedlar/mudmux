#ifndef COMM_INBOUND_HPP
#define COMM_INBOUND_HPP

#include "async/async_runtime.h"

#include <stddef.h>

void comm_enable_prompt (int slot, bool enable);
void comm_invoke_prompt (async_runtime_t* runtime);

int comm_invoke_connect (async_runtime_t* runtime, int slot);
int comm_invoke_inbound_message (async_runtime_t* runtime, int slot, const void* data, size_t size);

int comm_process_input (async_runtime_t* runtime, const io_event_t* event, int slot);

#endif /* COMM_INBOUND_HPP */
