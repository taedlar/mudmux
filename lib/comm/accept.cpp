#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mudmux/mudmux.h"
#include "accept.h"
#include "abstract.h"
#include <cstdint>
#include <openssl/bio.h>

int comm_accept (async_runtime_t* runtime, const char* accept_name) {
	if (!runtime || !accept_name || !*accept_name) {
		SPDLOG_ERROR ("comm_accept() called with invalid runtime or accept_name");
		return -1;
	}

	BIO* accept_bio = BIO_new(BIO_s_accept());
	if (!accept_bio) {
		SPDLOG_ERROR ("BIO_new(BIO_s_accept) failed for {}", accept_name);
		return -1;
	}

	if (BIO_set_accept_name(accept_bio, accept_name) <= 0) {
		SPDLOG_ERROR ("BIO_set_accept_name failed for {}", accept_name);
		BIO_free(accept_bio);
		return -1;
	}

	if (BIO_set_nbio_accept(accept_bio, 1) <= 0) {
		SPDLOG_ERROR ("BIO_set_nbio_accept failed for {}", accept_name);
		BIO_free(accept_bio);
		return -1;
	}

	if (BIO_do_accept(accept_bio) <= 0) {
		SPDLOG_ERROR ("BIO_do_accept (listen setup) failed for {}", accept_name);
		BIO_free(accept_bio);
		return -1;
	}

	socket_fd_t listen_fd = INVALID_SOCKET_FD;
	if (BIO_get_fd(accept_bio, &listen_fd) <= 0 || listen_fd == INVALID_SOCKET_FD) {
		SPDLOG_ERROR ("BIO_get_fd failed for {}", accept_name);
		BIO_free(accept_bio);
		return -1;
	}

	int slot = comm_abstract_add_bio(static_cast<void*>(accept_bio));
	if (slot < 0) {
		SPDLOG_ERROR ("comm_abstract_add_bio failed for {}", accept_name);
		BIO_free(accept_bio);
		return -1;
	}

	void* runtime_context = reinterpret_cast<void*>(static_cast<intptr_t>(slot));
	if (async_runtime_add(runtime, listen_fd, EVENT_READ, runtime_context) < 0) {
		SPDLOG_ERROR ("async_runtime_add failed for {}", accept_name);
		comm_abstract_remove(slot);
		return -1;
	}

	SPDLOG_INFO ("listening transport {} registered (slot={}, fd={})", accept_name, slot, listen_fd);
	return 0;
}

