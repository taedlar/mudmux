#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ssl.hpp"

#include <openssl/err.h>
#include <mutex>
#include <spdlog/spdlog.h>

#include "abstract.hpp"
#include "mudmux/comm.h"

extern std::recursive_mutex comm_slots_mtx;

class comm_abstract_ptr;
static void update_slot_event_interest(async_runtime_t* runtime, comm_abstract_ptr& comm, socket_fd_t fd, bool want_write);

namespace {

SSL_CTX* g_ssl_ctx {nullptr};

void log_openssl_error(const char* where) {
	const unsigned long err = ERR_get_error();
	if (!err) {
		SPDLOG_ERROR("{} failed with no OpenSSL error detail", where);
		return;
	}

	char err_buf[256] {};
	ERR_error_string_n(err, err_buf, sizeof(err_buf));
	SPDLOG_ERROR("{} failed: {}", where, err_buf);
}

} // namespace

bool comm_ssl_init (const std::filesystem::path& certificate_path, const std::filesystem::path& private_key_path) {
	if (certificate_path.empty() || private_key_path.empty()) {
		SPDLOG_ERROR("comm_ssl_init() requires non-empty certificate and private key paths");
		return false;
	}

	if (!OPENSSL_init_ssl(0, nullptr)) {
		SPDLOG_ERROR("OPENSSL_init_ssl failed");
		return false;
	}

	if (g_ssl_ctx != nullptr) {
		SSL_CTX_free(g_ssl_ctx);
		g_ssl_ctx = nullptr;
	}

	SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) {
		log_openssl_error("SSL_CTX_new");
		return false;
	}

	const std::string cert = certificate_path.string();
	if (SSL_CTX_use_certificate_file(ctx, cert.c_str(), SSL_FILETYPE_PEM) != 1) {
		log_openssl_error("SSL_CTX_use_certificate_file");
		SSL_CTX_free(ctx);
		return false;
	}

	const std::string key = private_key_path.string();
	if (SSL_CTX_use_PrivateKey_file(ctx, key.c_str(), SSL_FILETYPE_PEM) != 1) {
		log_openssl_error("SSL_CTX_use_PrivateKey_file");
		SSL_CTX_free(ctx);
		return false;
	}

	if (SSL_CTX_check_private_key(ctx) != 1) {
		log_openssl_error("SSL_CTX_check_private_key");
		SSL_CTX_free(ctx);
		return false;
	}

	g_ssl_ctx = ctx;
	return true;
}

void comm_ssl_deinit (void) {
	if (!g_ssl_ctx)
		return;

	SSL_CTX_free(g_ssl_ctx);
	g_ssl_ctx = nullptr;
}

void comm_enable_tls (int slot) {
	if (slot < 0) {
		SPDLOG_WARN("comm_enable_tls() called with invalid slot {}", slot);
		return;
	}

	comm_abstract_ptr comm(slot, comm_slots_mtx);
	if (!comm) {
		SPDLOG_WARN("comm_enable_tls() called for missing slot {}", slot);
		return;
	}

	if (!g_ssl_ctx) {
		SPDLOG_ERROR("comm_enable_tls() called before comm_ssl_init()");
		return;
	}

	if (!comm->rbio || !comm->wbio) {
		SPDLOG_WARN("comm_enable_tls() requires both rbio and wbio on slot {}", slot);
		return;
	}

	if (comm->ssl) {
		SPDLOG_DEBUG("TLS already enabled on slot {}", slot);
		return;
	}

	SSL* ssl = SSL_new(g_ssl_ctx);
	if (!ssl) {
		log_openssl_error("SSL_new");
		return;
	}

	BIO* transport_wbio = comm->wbio;
	if (!transport_wbio || BIO_up_ref(transport_wbio) != 1) {
		SPDLOG_ERROR("BIO_up_ref failed for TLS transport write BIO on slot {}", slot);
		SSL_free(ssl);
		return;
	}

	SSL_set_accept_state(ssl);
	SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifdef _WIN32
	// IOCP's WSARecv consumes ciphertext before the event reaches the comm
	// layer. Feed those completion buffers to SSL through a memory read BIO
	// while retaining the socket BIO as SSL's write transport.
	BIO* tls_rbio = BIO_new(BIO_s_mem());
	if (!tls_rbio) {
		SPDLOG_ERROR("BIO_new(BIO_s_mem) failed for TLS read BIO on slot {}", slot);
		BIO_free_all(transport_wbio);
		SSL_free(ssl);
		return;
	}
	BIO_set_mem_eof_return(tls_rbio, -1);
	SSL_set_bio(ssl, tls_rbio, transport_wbio); // SSL owns the memory BIO and write transport ref
#else
	BIO* transport_rbio = comm->rbio;
	if (!transport_rbio || BIO_up_ref(transport_rbio) != 1) {
		SPDLOG_ERROR("BIO_up_ref failed for TLS transport read BIO on slot {}", slot);
		BIO_free_all(transport_wbio);
		SSL_free(ssl);
		return;
	}
	SSL_set_bio(ssl, transport_rbio, transport_wbio); // SSL owns both transport BIO refs
#endif
	comm->ssl = ssl;
	comm->flags &= ~C_TLS_ESTABLISHED;

	socket_fd_t fd {INVALID_SOCKET_FD};
	if (comm->rbio) {
		int bio_fd = -1;
		if (BIO_get_fd(comm->rbio, &bio_fd) >= 0 && bio_fd >= 0)
			fd = comm_bio_fd_to_socket_fd(bio_fd);
	}
	update_slot_event_interest(async_get_current_runtime(), comm, fd, false);
	SPDLOG_DEBUG("TLS enabled on slot {} (handshake deferred to event loop)", slot);
}

static void update_slot_event_interest(async_runtime_t* runtime, comm_abstract_ptr& comm, socket_fd_t fd, bool want_write) {
	if (!runtime || fd == INVALID_SOCKET_FD)
		return;

	uint32_t events = EVENT_READ;
	if (want_write || (comm->flags & C_SOCKET_WRITABLE))
		events |= EVENT_WRITE;
	if (async_runtime_modify(runtime, fd, events, nullptr) < 0) {
		SPDLOG_WARN("async_runtime_modify failed while updating TLS event interest on slot {}", comm.slot());
	}
}

int comm_tls_handshake_step (async_runtime_t* runtime, int slot) {
	if (slot < 0)
		return -1;
	if (!runtime)
		runtime = async_get_current_runtime();
	if (!runtime)
		return -1;

	comm_abstract_ptr comm(slot, comm_slots_mtx);
	if (!comm)
		return -1;
	if (!comm->ssl)
		return 1;
	if (comm->flags & C_TLS_ESTABLISHED)
		return 1;

	if (SSL_is_init_finished(comm->ssl)) {
		comm->flags |= C_TLS_ESTABLISHED;
		return 1;
	}

	const int ret = SSL_do_handshake(comm->ssl);
	if (ret == 1) {
		comm->flags |= C_TLS_ESTABLISHED;
		socket_fd_t fd {INVALID_SOCKET_FD};
		if (comm->rbio) {
			int bio_fd = -1;
			if (BIO_get_fd(comm->rbio, &bio_fd) >= 0 && bio_fd >= 0)
				fd = comm_bio_fd_to_socket_fd(bio_fd);
		}
		update_slot_event_interest(runtime, comm, fd, false);
		SPDLOG_INFO("TLS handshake completed for slot {}", slot);
		return 1;
	}

	const int ssl_err = SSL_get_error(comm->ssl, ret);
	if (ssl_err == SSL_ERROR_WANT_READ) {
		socket_fd_t fd {INVALID_SOCKET_FD};
		if (comm->rbio) {
			int bio_fd = -1;
			if (BIO_get_fd(comm->rbio, &bio_fd) >= 0 && bio_fd >= 0)
				fd = comm_bio_fd_to_socket_fd(bio_fd);
		}
		update_slot_event_interest(runtime, comm, fd, false);
		return 0;
	}
	if (ssl_err == SSL_ERROR_WANT_WRITE) {
		socket_fd_t fd {INVALID_SOCKET_FD};
		if (comm->rbio) {
			int bio_fd = -1;
			if (BIO_get_fd(comm->rbio, &bio_fd) >= 0 && bio_fd >= 0)
				fd = comm_bio_fd_to_socket_fd(bio_fd);
		}
		update_slot_event_interest(runtime, comm, fd, true);
		return 0;
	}

	if (ssl_err == SSL_ERROR_ZERO_RETURN) {
		SPDLOG_INFO("TLS peer closed during handshake on slot {}", slot);
	} else {
		log_openssl_error("SSL_do_handshake");
		SPDLOG_ERROR("TLS handshake failed on slot {} (ssl_err={})", slot, ssl_err);
	}
	return -1;
}
