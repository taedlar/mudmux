#ifndef COMM_INBOUND_H
#define COMM_INBOUND_H

#include "async/async_runtime.h"
#include "comm/abstract.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int comm_invoke_inbound_message (async_runtime_t* runtime, int slot, const void* data, size_t size);

int comm_process_input (async_runtime_t* runtime, const io_event_t* event, int slot, comm_abstract_t* comm);

#ifdef __cplusplus
}
#endif

#endif // COMM_INBOUND_H
