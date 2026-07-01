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

	int slot = comm_abstract_add_bio(accept_bio, -1);
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

int comm_process_listener_event (
	async_runtime_t* runtime,
	int listener_slot,
	socket_fd_t event_fd) {
	if (!runtime || listener_slot < 0) {
		SPDLOG_WARN ("comm_process_listener_event() called with invalid args");
		return -1;
	}

	comm_abstract_t* listener_comm = comm_abstract_get(listener_slot);
	if (!listener_comm || !comm_abstract_is_listener(listener_comm)) {
		SPDLOG_WARN ("slot {} is not a valid listener", listener_slot);
		return -1;
	}

	int accepted_slot = -1;
#ifdef _WIN32
	accepted_slot = comm_abstract_accept(listener_slot, event_fd);
#else
	(void)event_fd;
	accepted_slot = comm_abstract_accept(listener_slot, INVALID_SOCKET_FD);
#endif

	if (accepted_slot < 0) {
		return -1;
	}

	auto* accepted_comm = comm_abstract_get(accepted_slot);
	BIO* accepted_bio = comm_abstract_get_bio(accepted_comm);
	socket_fd_t accepted_fd = INVALID_SOCKET_FD;
	if (!accepted_bio || BIO_get_fd(accepted_bio, &accepted_fd) <= 0) {
		accepted_fd = INVALID_SOCKET_FD;
	}
	if (!accepted_comm || accepted_fd == INVALID_SOCKET_FD) {
		SPDLOG_WARN ("accepted comm slot {} has invalid fd", accepted_slot);
		comm_abstract_remove(accepted_slot);
		return -1;
	}

	if (async_runtime_add(runtime, accepted_fd, EVENT_READ, slot_to_context(accepted_slot)) < 0) {
		SPDLOG_ERROR ("failed to register accepted fd {} with runtime", accepted_fd);
		comm_abstract_remove(accepted_slot);
		return -1;
	}

#ifdef _WIN32
	if (async_runtime_post_read(runtime, accepted_fd, nullptr, 0) < 0) {
		SPDLOG_ERROR ("failed to post initial read for fd {}", accepted_fd);
		async_runtime_remove(runtime, accepted_fd);
		comm_abstract_remove(accepted_slot);
		return -1;
	}
#endif

	mudmux_invoke_hook (
		MUDMUX_HOOK_CONNECT,
		async_runtime_get_context(runtime),
		accepted_slot,
		nullptr,
		0);

	return accepted_slot;
}

