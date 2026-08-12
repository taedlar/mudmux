#ifndef COMM_ACCEPT_HPP
#define COMM_ACCEPT_HPP

#include "abstract.hpp"
#include "async/async_runtime.h"

/**
 * @brief Add a listening transport for accepting incoming connections.
 * @param runtime The async_runtime_t instance.
 * @param accept_name A TCP listener URI in tcp://host:port form (for example, tcp://localhost:4000).
 * @return 0 on success, -1 on error
 */
int comm_accept (async_runtime_t* runtime, const char* accept_name);

/**
 * @brief Process a listener event for a comm slot.
 * @param runtime The async_runtime_t instance.
 * @param listener The comm_abstract_ptr representing the listener slot.
 * @param event_fd The socket file descriptor associated with the event (IOCP only, ignored in POSIX).
 * @return The slot number of the accepted connection, or -1 on error.
 */
int comm_process_listener_event (async_runtime_t* runtime, comm_abstract_ptr& listener, socket_fd_t event_fd);

#endif /* COMM_ACCEPT_HPP */
