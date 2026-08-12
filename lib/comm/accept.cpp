#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "accept.hpp"

#include <cstdint>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <openssl/bio.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "abstract.hpp"
#include "inbound.hpp"
#include "mudmux/hooks.h"
#include "mudmux/comm.h"

static void* slot_to_context (int slot) {
	return reinterpret_cast<void*>(static_cast<intptr_t>(slot));
}

static bool set_socket_nonblocking (socket_fd_t fd) {
	int bio_fd = -1;
	if (!comm_socket_fd_to_bio_fd(fd, &bio_fd)) {
		SPDLOG_ERROR ("socket fd {} cannot be represented as an OpenSSL socket fd", fd);
		return false;
	}

	if (BIO_socket_nbio(bio_fd, 1) != 1) {
#ifdef _WIN32
		SPDLOG_ERROR ("failed to set socket {} non-blocking (Winsock error {})", fd, WSAGetLastError());
#else
		SPDLOG_ERROR ("failed to set socket {} non-blocking", fd);
#endif
		return false;
	}
	return true;
}

std::string comm_listener_name(comm_abstract_ptr& listener) {
	if (!listener || !listener.has_rbio())
		return {};

#ifndef _WIN32
	socket_fd_t fd {INVALID_SOCKET_FD};
	sockaddr_un unix_address{};
	socklen_t unix_address_length = sizeof(unix_address);
	if (comm_bio_get_socket_fd(listener->rbio, &fd) &&
		getsockname(fd, reinterpret_cast<sockaddr*>(&unix_address), &unix_address_length) == 0 &&
		unix_address.sun_family == AF_UNIX) {
		return std::string{"unix://"} + unix_address.sun_path;
	}
#endif

	const char* host = BIO_get_accept_name(listener->rbio);
	const char* port = BIO_get_accept_port(listener->rbio);
	if (!host || !port)
		return {};
	return std::string{"tcp://"} + host + ":" + port;
}

#ifndef _WIN32
static bool listener_is_unix_socket (BIO* listener_bio) {
	socket_fd_t fd {INVALID_SOCKET_FD};
	if (!comm_bio_get_socket_fd(listener_bio, &fd))
		return false;

	sockaddr_storage address{};
	socklen_t address_length = sizeof(address);
	return getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_length) == 0 &&
		address.ss_family == AF_UNIX;
}

static int accept_unix_listener (BIO* listener_bio) {
	socket_fd_t listener_fd {INVALID_SOCKET_FD};
	if (!comm_bio_get_socket_fd(listener_bio, &listener_fd))
		return -1;

	const socket_fd_t accepted_fd = accept(listener_fd, nullptr, nullptr);
	if (accepted_fd == INVALID_SOCKET_FD) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			SPDLOG_WARN("accept failed for Unix-domain listener: {}", strerror(errno));
		return -1;
	}
	if (!set_socket_nonblocking(accepted_fd)) {
		close(accepted_fd);
		return -1;
	}

	BIO* accepted_bio = BIO_new_socket(accepted_fd, BIO_CLOSE);
	if (!accepted_bio) {
		close(accepted_fd);
		return -1;
	}
	const int accepted_slot = comm_abstract_add_bio(accepted_bio, accepted_bio, -1, C_SOCKET_READABLE);
	if (accepted_slot < 0)
		BIO_free(accepted_bio);
	return accepted_slot;
}

static int create_unix_listener (async_runtime_t* runtime, const char* accept_name, const char* path) {
	if (std::strlen(path) >= sizeof(sockaddr_un::sun_path)) {
		SPDLOG_ERROR("Unix-domain socket path is too long for {}", accept_name);
		return -1;
	}

	const socket_fd_t listener_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener_fd == INVALID_SOCKET_FD) {
		SPDLOG_ERROR("failed to create Unix-domain listener {}: {}", accept_name, strerror(errno));
		return -1;
	}
	if (!set_socket_nonblocking(listener_fd)) {
		close(listener_fd);
		return -1;
	}

	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
	const socklen_t address_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(address.sun_path) + 1);
	if (bind(listener_fd, reinterpret_cast<const sockaddr*>(&address), address_length) != 0 || listen(listener_fd, SOMAXCONN) != 0) {
		SPDLOG_ERROR("failed to bind Unix-domain listener {}: {}", accept_name, strerror(errno));
		close(listener_fd);
		return -1;
	}

	BIO* listener_bio = BIO_new_socket(listener_fd, BIO_CLOSE);
	if (!listener_bio) {
		close(listener_fd);
		return -1;
	}
	const int slot = comm_abstract_add_bio(listener_bio, nullptr, -1, C_SOCKET_LISTENING);
	if (slot < 0) {
		BIO_free(listener_bio);
		return -1;
	}
	if (async_runtime_add(runtime, listener_fd, EVENT_READ, slot_to_context(slot)) < 0) {
		SPDLOG_ERROR("failed to register Unix-domain listener {}", accept_name);
		comm_abstract_remove(slot);
		return -1;
	}
	SPDLOG_INFO("listening transport {} registered (slot={}, fd={})", accept_name, slot, listener_fd);
	return 0;
}
#endif

int comm_accept (async_runtime_t* runtime, const char* accept_name) {
	if (!runtime || !accept_name || !*accept_name) {
		SPDLOG_ERROR ("comm_accept() called with invalid runtime or accept_name");
		return -1;
	}

	constexpr std::string_view tcp_scheme{"tcp://"};
	constexpr std::string_view unix_scheme{"unix://"};
	const std::string_view configured_name{accept_name};
	if (configured_name.compare(0, unix_scheme.size(), unix_scheme) == 0) {
		const char* path = accept_name + unix_scheme.size();
		if (!*path) {
			SPDLOG_ERROR("invalid Unix-domain accept transport {}; expected unix:///path/to/socket", accept_name);
			return -1;
		}
#ifdef _WIN32
		SPDLOG_ERROR("Unix-domain accept transport {} is not supported on Windows", accept_name);
		return -1;
#else
		return create_unix_listener(runtime, accept_name, path);
#endif
	}
	if (configured_name.compare(0, tcp_scheme.size(), tcp_scheme) != 0 ||
		configured_name.size() == tcp_scheme.size()) {
		SPDLOG_ERROR ("invalid accept transport {}; expected tcp://host:port or unix:///path/to/socket", accept_name);
		return -1;
	}
	const char* endpoint = accept_name + tcp_scheme.size();

	BIO* listener_bio = BIO_new(BIO_s_accept());
	if (!listener_bio) {
		SPDLOG_ERROR ("BIO_new(BIO_s_accept) failed for {}", accept_name);
		return -1;
	}

	if (BIO_set_accept_name(listener_bio, endpoint) <= 0) {
		SPDLOG_ERROR ("BIO_set_accept_name failed for {}", accept_name);
		BIO_free(listener_bio);
		return -1;
	}

	// Must be set before BIO_do_accept() creates and binds the listener socket.
	if (BIO_set_bind_mode(listener_bio, BIO_BIND_REUSEADDR) <= 0) {
		SPDLOG_ERROR ("BIO_set_bind_mode (SO_REUSEADDR) failed for {}", accept_name);
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

static int _accept_new_comm (comm_abstract_ptr& listener_comm, socket_fd_t event_fd) {
	BIO* listener_bio = listener_comm ? listener_comm->rbio : nullptr;
	if (!listener_comm.has_rbio() || !(listener_comm->flags & C_SOCKET_LISTENING)) {
		return -1; // invalid listener port
	}

#ifndef _WIN32
	if (listener_is_unix_socket(listener_bio))
		return accept_unix_listener(listener_bio);
#endif

#ifdef _WIN32
    // Windows IOCP delivers the accepted fd directly in the event (proactive).
    // Unix epoll/poll only signal readability; accept() must be called to extract fd (reactive).
	if (event_fd != INVALID_SOCKET_FD) {
		if (!set_socket_nonblocking(event_fd)) {
			closesocket(event_fd);
			return -1;
		}

		int bio_fd = -1;
		if (!comm_socket_fd_to_bio_fd(event_fd, &bio_fd)) {
			SPDLOG_ERROR("accepted socket fd {} cannot be represented as BIO int fd", event_fd);
			closesocket(event_fd);
			return -1;
		}

		BIO* accepted_bio = BIO_new_socket (bio_fd, BIO_CLOSE);
        if (!accepted_bio) {
            SPDLOG_ERROR("failed to create BIO for accepted window socket {}", event_fd);
			closesocket(event_fd);
            return -1;
        }
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
            SPDLOG_WARN ("BIO_do_accept failed for listener slot {}", listener_comm.slot());
        }
        return -1;
    }

    BIO* accepted_bio = BIO_pop (listener_bio);
    if (!accepted_bio) {
        SPDLOG_WARN ("BIO_pop failed for listener slot {}", listener_comm.slot());
        return -1;
    }
	socket_fd_t accepted_fd {INVALID_SOCKET_FD};
	if (!comm_bio_get_socket_fd(accepted_bio, &accepted_fd) || !set_socket_nonblocking(accepted_fd)) {
		SPDLOG_ERROR ("failed to set accepted socket non-blocking on listener slot {}", listener_comm.slot());
		BIO_free (accepted_bio);
		return -1;
	}
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

int comm_process_listener_event (async_runtime_t* runtime, comm_abstract_ptr& listener, socket_fd_t event_fd) {
	if (!runtime || !listener.has_rbio()) {
		SPDLOG_WARN ("invalid arguments");
		return -1;
	}
	SPDLOG_DEBUG ("processing listener event on slot {} (fd={})", listener.slot(), event_fd);

	// accept and register comm slot for the new connection
#ifdef _WIN32
	// [IOCP] Windows IOCP delivers the accepted fd directly in the event (proactive).
	int accepted_slot = _accept_new_comm (listener, event_fd);
#else
	// [POSIX] epoll/poll only signal readability; accept() must be called to extract fd (reactive).
	(void)event_fd;
	int accepted_slot = _accept_new_comm (listener, INVALID_SOCKET_FD);
#endif
	if (accepted_slot < 0)
		return -1;
	SPDLOG_INFO ("accepted new connection on listener slot {} -> new comm slot {}", listener.slot(), accepted_slot);

	// validate accepted socket_fd_t
	if (_async_poll_read (runtime, accepted_slot) < 0) {
		SPDLOG_ERROR ("failed to register accepted comm slot {} with runtime", accepted_slot);
		comm_abstract_remove (accepted_slot);
		return -1;
	}
	// we don't need to register write events; we will write when needed and handle EAGAIN/EWOULDBLOCK

	comm_invoke_connect (runtime, accepted_slot, listener.slot());

	return accepted_slot;
}
