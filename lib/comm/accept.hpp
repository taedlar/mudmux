#ifndef COMM_ACCEPT_HPP
#define COMM_ACCEPT_HPP

#include "async/async_runtime.h"

int comm_accept (async_runtime_t* runtime, const char* accept_name);

int comm_process_listener_event (async_runtime_t* runtime, int listener_slot, socket_fd_t event_fd);

#endif /* COMM_ACCEPT_HPP */
