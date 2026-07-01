#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/mudmux.h"
#include "accept.h"
#include "abstract.h"
#include <cstdint>
#include <openssl/bio.h>

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

	int bio_fd = -1;
	if (BIO_get_fd(listener_bio, &bio_fd) <= 0 || bio_fd == INVALID_SOCKET_FD) {
		SPDLOG_ERROR ("BIO_get_fd failed for {}", accept_name);
		BIO_free(listener_bio);
		return -1;
	}
	socket_fd_t listen_fd = static_cast<socket_fd_t>(bio_fd);
	if (listen_fd == INVALID_SOCKET_FD) {
		SPDLOG_ERROR ("BIO_get_fd returned invalid socket fd for {}", accept_name);
		BIO_free(listener_bio);
		return -1;
	}

	int slot = comm_abstract_add_bio (listener_bio, nullptr, -1);
	if (slot < 0) {
		SPDLOG_ERROR ("comm_abstract_add_bio failed for {}", accept_name);
		BIO_free(listener_bio);
		return -1;
	}

	void* runtime_context = reinterpret_cast<void*>(static_cast<intptr_t>(slot));
	if (async_runtime_add(runtime, listen_fd, EVENT_READ, runtime_context) < 0) {
		SPDLOG_ERROR ("async_runtime_add(fd={}) failed for {}", listen_fd, accept_name);
		comm_abstract_remove (slot);
		return -1;
	}

	if (comm_is_listener(comm_abstract_get(slot)))
		SPDLOG_INFO ("listening transport {} registered (slot={}, fd={})", accept_name, slot, listen_fd);

	return 0;
}

static int _accept_new_comm (int slot, socket_fd_t event_fd) {
	comm_abstract_t* listener_comm = comm_abstract_get(slot);
	BIO* listener_bio = comm_abstract_get_rbio(slot);
	if (!listener_bio || !comm_is_listener(listener_comm)) {
		SPDLOG_WARN ("slot {} is not a valid listener", slot);
		return -1;
	}

#ifdef _WIN32
    // Windows IOCP delivers the accepted fd directly in the event (proactive).
    // Unix epoll/poll only signal readability; accept() must be called to extract fd (reactive).
    if (event_fd != INVALID_SOCKET_FD) {
        BIO* accepted_bio = BIO_new_socket (static_cast<int>(event_fd), BIO_CLOSE);
        if (!accepted_bio) {
            SPDLOG_ERROR("failed to create BIO for accepted window socket {}", event_fd);
            return -1;
        }
		int accepted_slot = comm_abstract_add_bio (accepted_bio, accepted_bio, -1);
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
    int accepted_slot = comm_abstract_add_bio (accepted_bio, accepted_bio, -1);
    if (accepted_slot < 0) {
        BIO_free (accepted_bio);
        return -1;
    }

    return accepted_slot;
}

int _async_poll_read (async_runtime_t* runtime, int slot) {
	BIO* rbio = comm_abstract_get_rbio (slot);
	if (!runtime || !rbio) {
		SPDLOG_WARN ("invalid arguments to _async_poll_read");
		return -1;
	}
	socket_fd_t fd = INVALID_SOCKET_FD;
	if (BIO_get_fd(rbio, &fd) <= 0 || fd == INVALID_SOCKET_FD) {
		SPDLOG_WARN ("invalid fd for slot {}", slot);
		return -1;
	}
	return async_runtime_add (runtime, fd, EVENT_READ, slot_to_context(slot));
}

int comm_process_listener_event (async_runtime_t* runtime, int listener_slot, socket_fd_t event_fd) {
	BIO* listener_bio = comm_abstract_get_rbio (listener_slot);
	if (!runtime || !listener_bio) {
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
	BIO* accepted_rbio = comm_abstract_get_rbio (accepted_slot);
	if (_async_poll_read (runtime, accepted_slot) < 0) {
		SPDLOG_ERROR ("failed to register accepted comm slot {} with runtime", accepted_slot);
		comm_abstract_remove (accepted_slot);
		return -1;
	}
	// we don't need to register write events; we will write when needed and handle EAGAIN/EWOULDBLOCK

#ifdef _WIN32
	// [IOCP] post an initial IOCP read for the accepted socket to trigger the first read event
	socket_fd_t accepted_fd = INVALID_SOCKET_FD;
	if (BIO_get_fd(accepted_rbio, &accepted_fd) <= 0 ||
		async_runtime_post_read (runtime, accepted_fd, nullptr, 0) < 0) {
		SPDLOG_ERROR ("failed to post initial read for fd {}", accepted_fd);
		async_runtime_remove (runtime, accepted_fd);
		comm_abstract_remove (accepted_slot);
		return -1;
	}
#endif

	// invoke connect hook for the new connection
	mudmux_invoke_hook (
		MUDMUX_HOOK_CONNECT,
		async_runtime_get_context (runtime),
		accepted_slot,
		nullptr,
		0);

	return accepted_slot;
}

