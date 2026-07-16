#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ssl.hpp"

#include <openssl/err.h>
#include <spdlog/spdlog.h>

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
