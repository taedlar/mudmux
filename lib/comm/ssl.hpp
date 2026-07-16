#ifndef COMM_SSL_HPP
#define COMM_SSL_HPP

#include <filesystem>
#include <openssl/ssl.h>

#include "async/async_runtime.h"

bool comm_ssl_init (const std::filesystem::path& certificate_path, const std::filesystem::path& private_key_path);

void comm_ssl_deinit (void);

void comm_enable_tls (int slot);

// returns 1 when handshake is complete, 0 when pending, -1 on error
int comm_tls_handshake_step (async_runtime_t* runtime, int slot);

#endif // COMM_SSL_HPP
