#ifndef COMM_SSL_HPP
#define COMM_SSL_HPP

#include <filesystem>
#include <openssl/ssl.h>

bool comm_ssl_init (const std::filesystem::path& certificate_path, const std::filesystem::path& private_key_path);

void comm_ssl_deinit (void);

void comm_enable_tls (int slot);

#endif // COMM_SSL_HPP
