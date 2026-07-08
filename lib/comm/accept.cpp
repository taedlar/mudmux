#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "accept.hpp"

#include <cstdint>
#include <openssl/bio.h>

#include "abstract.hpp"
#include "inbound.hpp"
#include "mudmux/hooks.h"
#include "mudmux/comm.h"

static void* slot_to_context (int slot) {
	return reinterpret_cast<void*>(static_cast<intptr_t>(slot));
}

int comm_accept (async_runtime_t* runtime, const char* accept_name) {
	if (!runtime || !accept_name || !*accept_name) {
		SPDLOG_ERROR ("comm_accept() called with invalid runtime or accept_name");
		return -1;
	}

	BIO* listener_bio = BIO_new(BIO_s_accept());
	if (!listener_bio) {
		SPDLOG_ERROR ("BIO_new(BIO_s_accept) failed for {}", accept_name);
		return -1;
	}

	if (BIO_set_accept_name(listener_bio, accept_name) <= 0) {
		SPDLOG_ERROR ("BIO_set_accept_name failed for {}", accept_name);
		BIO_free(listener_bio);
		return -1;
	}

	if (BIO_set_nbio_accept(listener_bio, 1) <= 0) {
		SPDLOG_ERROR ("BIO_set_nbio_accept failed for {}", accept_name);
		BIO_free(listener_bio);
		return -1;
	}

	if (BIO_do_accept(listener_bio) <= 0) {
		SPDLOG_ERROR ("BIO_do_accept (listen setup) failed for {}", accept_name);
		BIO_free(listener_bio);
		return -1;
	}

	socket_fd_t listen_fd {INVALID_SOCKET_FD};
	if (!comm_bio_get_socket_fd(listener_bio, &listen_fd)) {
		SPDLOG_ERROR ("BIO_get_fd failed for {}", accept_name);
		BIO_free(listener_bio);
		return -1;
	}

	int slot = comm_abstract_add_bio (listener_bio, nullptr, -1, C_SOCKET_LISTENING);
	if (slot < 0) {
		SPDLOG_ERROR ("comm_abstract_add_bio failed for {}", accept_name);
		BIO_free(listener_bio);
		return -1;
	}

	void* runtime_context = reinterpret_cast<void*>(static_cast<intptr_t>(slot));
	if (async_runtime_add(runtime, listen_fd, EVENT_READ, runtime_context) < 0) {
		SPDLOG_ERROR ("async_runtime_add (socket={}) failed for {}", listen_fd, accept_name);
		comm_abstract_remove (slot);
		return -1;
	}

	if (comm_abstract_has_rbio(slot))
		SPDLOG_INFO ("listening transport {} registered (slot={}, fd={})", accept_name, slot, listen_fd);

	return 0;
}

static int _accept_new_comm (int slot, socket_fd_t event_fd) {
	comm_abstract_ptr listener_comm(slot, mud_logic_mutex);
	BIO* listener_bio = listener_comm ? listener_comm->rbio : nullptr;
	if (!listener_bio || !(comm_get_flags(listener_comm.get()) & C_SOCKET_LISTENING)) {
		SPDLOG_WARN ("slot {} is not a valid listener", slot);
		return -1;
	}

#ifdef _WIN32
    // Windows IOCP delivers the accepted fd directly in the event (proactive).
    // Unix epoll/poll only signal readability; accept() must be called to extract fd (reactive).
	if (event_fd != INVALID_SOCKET_FD) {
		int bio_fd = -1;
		if (!comm_socket_fd_to_bio_fd(event_fd, &bio_fd)) {
			SPDLOG_ERROR("accepted socket fd {} cannot be represented as BIO int fd", event_fd);
			closesocket(event_fd);
			return -1;
		}

		BIO* accepted_bio = BIO_new_socket (bio_fd, BIO_CLOSE);
        if (!accepted_bio) {
            SPDLOG_ERROR("failed to create BIO for accepted window socket {}", event_fd);
            return -1;
        }
        BIO_set_nbio (accepted_bio, 1); // set accepted socket to non-blocking mode
		int accepted_slot = comm_abstract_add_bio (accepted_bio, accepted_bio, -1, C_SOCKET_READABLE);
		if (accepted_slot < 0) {
			BIO_free (accepted_bio);
			return -1;
		}
		return accepted_slot;
    }
#else
    (void)event_fd;
#endif

    // Fall back to standard BIO_do_accept path
    if (BIO_do_accept (listener_bio) <= 0) {
        if (!BIO_should_retry (listener_bio)) {
            SPDLOG_WARN ("BIO_do_accept failed for listener slot {}", slot);
        }
        return -1;
    }

    BIO* accepted_bio = BIO_pop (listener_bio);
    if (!accepted_bio) {
        SPDLOG_WARN ("BIO_pop failed for listener slot {}", slot);
        return -1;
    }
	BIO_set_nbio (accepted_bio, 1); // set accepted socket to non-blocking mode

	// add to communication slots
    int accepted_slot = comm_abstract_add_bio (accepted_bio, accepted_bio, -1, C_SOCKET_READABLE);
    if (accepted_slot < 0) {
        BIO_free (accepted_bio);
        return -1;
    }

    return accepted_slot;
}

int _async_poll_read (async_runtime_t* runtime, int slot) {
	if (!runtime) {
		SPDLOG_WARN ("invalid arguments to _async_poll_read");
		return -1;
	}
	socket_fd_t fd {INVALID_SOCKET_FD};
	if (!comm_abstract_get_rbio_fd(slot, &fd)) {
		SPDLOG_WARN ("invalid fd for slot {}", slot);
		return -1;
	}
	return async_runtime_add (runtime, fd, EVENT_READ, slot_to_context(slot));
}

int comm_process_listener_event (async_runtime_t* runtime, int listener_slot, socket_fd_t event_fd) {
	if (!runtime || !comm_abstract_has_rbio(listener_slot)) {
		SPDLOG_WARN ("invalid arguments");
		return -1;
	}
	SPDLOG_DEBUG ("processing listener event on slot {} (fd={})", listener_slot, event_fd);

	// accept and register comm slot for the new connection
#ifdef _WIN32
	// [IOCP] Windows IOCP delivers the accepted fd directly in the event (proactive).
	int accepted_slot = _accept_new_comm (listener_slot, event_fd);
#else
	// [POSIX] epoll/poll only signal readability; accept() must be called to extract fd (reactive).
	(void)event_fd;
	int accepted_slot = _accept_new_comm (listener_slot, INVALID_SOCKET_FD);
#endif
	if (accepted_slot < 0)
		return -1;
	SPDLOG_INFO ("accepted new connection on listener slot {} -> new comm slot {}", listener_slot, accepted_slot);

	// validate accepted socket_fd_t
	if (_async_poll_read (runtime, accepted_slot) < 0) {
		SPDLOG_ERROR ("failed to register accepted comm slot {} with runtime", accepted_slot);
		comm_abstract_remove (accepted_slot);
		return -1;
	}
	// we don't need to register write events; we will write when needed and handle EAGAIN/EWOULDBLOCK

#ifdef _WIN32
	// [IOCP] post an initial IOCP read for the accepted socket to trigger the first read event
	socket_fd_t accepted_fd {INVALID_SOCKET_FD};
	if (!comm_abstract_get_rbio_fd(accepted_slot, &accepted_fd)) {
		SPDLOG_ERROR ("BIO_get_fd failed for accepted slot {}", accepted_slot);
		comm_abstract_remove (accepted_slot);
		return -1;
	}
	if (async_runtime_post_read (runtime, accepted_fd, nullptr, 0) < 0) {
		SPDLOG_ERROR ("failed to post initial read for fd {}", accepted_fd);
		async_runtime_remove (runtime, accepted_fd);
		comm_abstract_remove (accepted_slot);
		return -1;
	}
#endif

	comm_invoke_connect (runtime, accepted_slot);

	return accepted_slot;
}
