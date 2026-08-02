#ifndef COMM_INBOUND_HPP
#define COMM_INBOUND_HPP

#include "abstract.hpp"
#include "async/async_runtime.h"

#include <atomic>
#include <stddef.h>

enum comm_process_result_t {
	COMM_PROCESS_ERROR = -1,
	COMM_PROCESS_OK = 0,
	COMM_PROCESS_CLOSED = 1,
	COMM_PROCESS_DEFERRED = 2,
};

extern "C" void comm_enable_prompt (int slot, bool enable);

void comm_invoke_prompt (async_runtime_t* runtime);
bool comm_has_deferred_input (void);
void comm_resume_deferred_input (async_runtime_t* runtime);

int comm_invoke_connect (async_runtime_t* runtime, int slot, int entry_slot);
int comm_invoke_inbound_message (async_runtime_t* runtime, comm_abstract_ptr& comm, const void* data, size_t size);

/**
 * @brief Refill the inbound buffer chain for the specified comm slot by reading from the
 * underlying BIO. Process transport layer details such as TLS decryption and Telnet negotiation
 * if applicable. If the inbound buffer is full or the underlying BIO is closed, the function will
 * return false.
 *
 * If src (and size) are provided, data will be copied from src into the inbound buffer chain instead
 * (to integrate with Windows IOCP or other custom data sources). In this case, the function will
 * return true if all data was copied, or false if some data was discarded due to buffers capacity.
 *
 * @param comm The comm object to refill.
 * @param src Optional source buffer to copy raw data from.
 * @param size Size of the source raw data buffer.
 * @return true on success, false on error.
 */
bool comm_refill_inbound_buffers (comm_abstract_ptr& comm, const char* src = nullptr, size_t size = 0);

/**
 * Append application bytes that have already passed all transport decoders.
 * Unlike comm_refill_inbound_buffers(), this does not feed TLS, WebSocket, or
 * Telnet processing again.
 */
bool comm_append_decoded_input(comm_abstract_ptr& comm, const char* src, size_t size);

void comm_free_inbound_buffers (comm_abstract_ptr& comm);

size_t comm_copy_inbound_data_prefix(comm_abstract_ptr& comm, size_t limit, std::string& out);
void comm_consume_inbound_data(comm_abstract_ptr& comm, size_t bytes);

/**
 * @brief Process input data from the specified comm slot's inbound buffer.
 *
 * @param runtime The async runtime context.
 * @param comm The comm object to process input for.
 * @param max_message Optional maximum number of messages to process; -1 for no limit.
 * @return 0 on success, 1 if the connection was closed, -1 on error.
 */
comm_process_result_t comm_process_input (async_runtime_t* runtime, comm_abstract_ptr& comm, int max_message = -1);

/**
 * Process application bytes that have already been decoded from a transport
 * framing layer (for example, a WebSocket message).  This applies the normal
 * line/character input-mode parser without attempting another transport decode.
 */
comm_process_result_t comm_process_decoded_input(async_runtime_t* runtime, comm_abstract_ptr& comm,
                                                 int max_message = -1);

extern std::atomic<bool> has_deferred_input;

#endif /* COMM_INBOUND_HPP */
