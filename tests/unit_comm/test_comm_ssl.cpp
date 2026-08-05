#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "comm/abstract.hpp"
#include "comm/inbound.hpp"
#include "comm/outbound.hpp"
#include "comm/ssl.hpp"
#include "comm/websocket.hpp"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

#include <gtest/gtest.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <chrono>
#include <array>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

std::string random_suffix() {
    std::mt19937_64 rng(static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    return std::to_string(rng());
}

bool write_test_cert_and_key(const std::filesystem::path& cert_path, const std::filesystem::path& key_path) {
    EVP_PKEY* pkey = EVP_RSA_gen(4096);
    BIGNUM* e = BN_new();
    X509* x509 = X509_new();
    X509_NAME* name = X509_NAME_new();
    BIO* cert_bio = nullptr;
    BIO* key_bio = nullptr;

    if (!pkey || !e || !x509 || !name)
        goto fail;
    if (BN_set_word(e, RSA_F4) != 1)
        goto fail;

    if (X509_set_version(x509, 2) != 1)
        goto fail;
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 60 * 60L);
    if (X509_set_pubkey(x509, pkey) != 1)
        goto fail;

    if (X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("US"), -1, -1, 0) != 1)
        goto fail;
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("mudmux-test"), -1, -1, 0) != 1)
        goto fail;
    if (X509_set_subject_name(x509, name) != 1)
        goto fail;
    if (X509_set_issuer_name(x509, name) != 1)
        goto fail;
    if (X509_sign(x509, pkey, EVP_sha256()) <= 0)
        goto fail;

    cert_bio = BIO_new_file(cert_path.string().c_str(), "w");
    key_bio = BIO_new_file(key_path.string().c_str(), "w");
    if (!cert_bio || !key_bio)
        goto fail;
    if (PEM_write_bio_X509(cert_bio, x509) != 1)
        goto fail;
    if (PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1)
        goto fail;

    BIO_free_all(cert_bio);
    BIO_free_all(key_bio);
    X509_free(x509);
    X509_NAME_free(name);
    EVP_PKEY_free(pkey);
    BN_free(e);
    return true;

fail:
    if (cert_bio)
        BIO_free_all(cert_bio);
    if (key_bio)
        BIO_free_all(key_bio);
    if (x509)
        X509_free(x509);
    if (name)
        X509_NAME_free(name);
    if (pkey)
        EVP_PKEY_free(pkey);
    if (e)
        BN_free(e);
    return false;
}

class CommSslTest : public ::testing::Test {
protected:
    std::filesystem::path cert_path_;
    std::filesystem::path key_path_;

    void SetUp() override {
        const auto base = std::filesystem::temp_directory_path() / ("mudmux_tls_test_" + random_suffix());
        cert_path_ = base;
        cert_path_ += ".crt.pem";
        key_path_ = base;
        key_path_ += ".key.pem";

        ASSERT_TRUE(write_test_cert_and_key(cert_path_, key_path_));
        ASSERT_TRUE(comm_ssl_init(cert_path_, key_path_));
    }

    void TearDown() override {
        comm_abstract_remove_all();
        comm_ssl_deinit();
        std::error_code ec;
        std::filesystem::remove(cert_path_, ec);
        std::filesystem::remove(key_path_, ec);
    }
};

struct inbound_collector_t {
    std::vector<std::string> messages;

    static int hook(void* ctx, int, void* data, size_t size) {
        auto* self = static_cast<inbound_collector_t*>(ctx);
        if (!self || !data)
            return 0;
        self->messages.emplace_back(static_cast<const char*>(data), size);
        return 0;
    }
};

#ifdef _WIN32
static bool feed_tls_from_src_fragments(comm_abstract_ptr& comm, async_runtime_t* runtime, size_t fragment_size) {
    if (!comm || !comm->rbio)
        return false;

    std::array<char, 4096> cipher{};
    size_t bytes_read = 0;

    while (BIO_read_ex(comm->rbio, cipher.data(), cipher.size(), &bytes_read) && bytes_read > 0) {
        size_t offset = 0;
        while (offset < bytes_read) {
            size_t chunk = (std::min)(fragment_size, bytes_read - offset);
            if (!comm_refill_inbound_buffers(comm, cipher.data() + offset, chunk))
                return false;
            offset += chunk;
        }
    }

    int hs = comm_tls_handshake_step(runtime, comm.slot());
    return hs >= 0;
}
#endif

} // namespace

TEST_F(CommSslTest, EnableTlsStartsAndCompletesNonBlockingHandshake) {
    BIO* server_io = nullptr;
    BIO* client_io = nullptr;
    ASSERT_EQ(BIO_new_bio_pair(&server_io, 0, &client_io, 0), 1);
    ASSERT_NE(server_io, nullptr);
    ASSERT_NE(client_io, nullptr);

    const int slot = comm_abstract_add_bio(server_io, server_io, -1, C_SOCKET_READABLE);
    ASSERT_GE(slot, 0);

    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);

    SSL* client_ssl = SSL_new(client_ctx);
    ASSERT_NE(client_ssl, nullptr);
    SSL_set_connect_state(client_ssl);
    SSL_set_bio(client_ssl, client_io, client_io); // transfer ownership to client SSL

    comm_enable_tls(slot);

    async_runtime_t* runtime = async_runtime_init(nullptr);
    ASSERT_NE(runtime, nullptr);

    bool server_done = false;
    bool client_done = false;
    for (int i = 0; i < 256 && !(server_done && client_done); ++i) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);

        ASSERT_TRUE(comm_refill_inbound_buffers(comm));

        if (!server_done) {
            const int s = comm_tls_handshake_step(runtime, slot);
            ASSERT_GE(s, 0);
            server_done = (s == 1);
        }

        if (!client_done) {
            const int c = SSL_do_handshake(client_ssl);
            if (c == 1) {
                client_done = true;
            } else {
                const int err = SSL_get_error(client_ssl, c);
                ASSERT_TRUE(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    << "unexpected client SSL error: " << err;
            }
        }
    }

    EXPECT_TRUE(server_done);
    EXPECT_TRUE(client_done);

    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);
    ASSERT_NE(comm->ssl, nullptr);
    EXPECT_EQ(SSL_is_init_finished(comm->ssl), 1);

    async_runtime_deinit(runtime);
    SSL_free(client_ssl);
    SSL_CTX_free(client_ctx);
}

TEST_F(CommSslTest, WebSocketUpgradeBarrierFlushesOverTls) {
    BIO* server_io = nullptr;
    BIO* client_io = nullptr;
    ASSERT_EQ(BIO_new_bio_pair(&server_io, 0, &client_io, 0), 1);

    const int slot = comm_abstract_add_bio(server_io, server_io, -1, C_SOCKET_READABLE);
    ASSERT_GE(slot, 0);

    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);
    SSL* client_ssl = SSL_new(client_ctx);
    ASSERT_NE(client_ssl, nullptr);
    SSL_set_connect_state(client_ssl);
    SSL_set_bio(client_ssl, client_io, client_io);

    comm_enable_tls(slot);
    async_runtime_t* runtime = async_runtime_init(nullptr);
    ASSERT_NE(runtime, nullptr);

    bool server_done = false;
    bool client_done = false;
    for (int i = 0; i < 256 && !(server_done && client_done); ++i) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
        ASSERT_TRUE(comm_refill_inbound_buffers(comm));
        if (!server_done) {
            const int result = comm_tls_handshake_step(runtime, slot);
            ASSERT_GE(result, 0);
            server_done = result == 1;
        }
        if (!client_done) {
            const int result = SSL_do_handshake(client_ssl);
            if (result == 1) {
                client_done = true;
            } else {
                const int error = SSL_get_error(client_ssl, result);
                ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
            }
        }
    }
    ASSERT_TRUE(server_done);
    ASSERT_TRUE(client_done);

    {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
        ASSERT_TRUE(comm_enable_websocket(slot, nullptr));
        comm_buffered_write_comm(comm, "banner", 6);
    }

    const char request[] =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    size_t sent = 0;
    ASSERT_EQ(SSL_write_ex(client_ssl, request, sizeof(request) - 1, &sent), 1);
    ASSERT_EQ(sent, sizeof(request) - 1);

    {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
        ASSERT_TRUE(comm_refill_inbound_buffers(comm));
        EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    }

    std::string received;
    std::array<char, 1024> buffer{};
    for (int i = 0; i < 256; ++i) {
        comm_flush(runtime, slot);
        size_t bytes_read = 0;
        const int result = SSL_read_ex(client_ssl, buffer.data(), buffer.size(), &bytes_read);
        if (result == 1 && bytes_read > 0)
            received.append(buffer.data(), bytes_read);
        else if (result != 1) {
            const int error = SSL_get_error(client_ssl, result);
            ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
        }
        if (received.find("101 Switching Protocols") != std::string::npos
            && received.find(std::string("\x82\x06", 2) + "banner") != std::string::npos)
            break;
    }

    const auto upgrade = received.find("HTTP/1.1 101 Switching Protocols");
    const auto banner = received.find(std::string("\x82\x06", 2) + "banner");
    EXPECT_EQ(upgrade, 0u);
    EXPECT_NE(banner, std::string::npos);
    EXPECT_GT(banner, upgrade);

    async_runtime_deinit(runtime);
    SSL_free(client_ssl);
    SSL_CTX_free(client_ctx);
}

TEST_F(CommSslTest, RejectedWebSocketUpgradeOverTlsQueuesBadRequestResponse) {
    BIO* server_io = nullptr;
    BIO* client_io = nullptr;
    ASSERT_EQ(BIO_new_bio_pair(&server_io, 0, &client_io, 0), 1);

    const int slot = comm_abstract_add_bio(server_io, server_io, -1, C_SOCKET_READABLE);
    ASSERT_GE(slot, 0);

    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);
    SSL* client_ssl = SSL_new(client_ctx);
    ASSERT_NE(client_ssl, nullptr);
    SSL_set_connect_state(client_ssl);
    SSL_set_bio(client_ssl, client_io, client_io);

    comm_enable_tls(slot);
    async_runtime_t* runtime = async_runtime_init(nullptr);
    ASSERT_NE(runtime, nullptr);

    bool server_done = false;
    bool client_done = false;
    for (int i = 0; i < 256 && !(server_done && client_done); ++i) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
        ASSERT_TRUE(comm_refill_inbound_buffers(comm));
        if (!server_done) {
            const int result = comm_tls_handshake_step(runtime, slot);
            ASSERT_GE(result, 0);
            server_done = result == 1;
        }
        if (!client_done) {
            const int result = SSL_do_handshake(client_ssl);
            if (result == 1) {
                client_done = true;
            } else {
                const int error = SSL_get_error(client_ssl, result);
                ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
            }
        }
    }
    ASSERT_TRUE(server_done);
    ASSERT_TRUE(client_done);

    ASSERT_TRUE(comm_enable_websocket(slot, nullptr));
    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t sent = 0;
    ASSERT_EQ(SSL_write_ex(client_ssl, request, sizeof(request) - 1, &sent), 1);
    ASSERT_EQ(sent, sizeof(request) - 1);

    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);
    ASSERT_TRUE(comm_refill_inbound_buffers(comm));
    EXPECT_NE(comm->flags & C_CLOSING, 0u);

    std::string received;
    std::array<char, 512> buffer{};
    for (int i = 0; i < 256; ++i) {
        comm_flush(runtime, slot);
        size_t bytes_read = 0;
        const int result = SSL_read_ex(client_ssl, buffer.data(), buffer.size(), &bytes_read);
        if (result == 1 && bytes_read > 0)
            received.append(buffer.data(), bytes_read);
        else if (result != 1) {
            const int error = SSL_get_error(client_ssl, result);
            ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
        }
        if (received.find("HTTP/1.1 400 Bad Request") != std::string::npos)
            break;
    }
    EXPECT_EQ(received.find("HTTP/1.1 400 Bad Request"), 0u);

    async_runtime_deinit(runtime);
    SSL_free(client_ssl);
    SSL_CTX_free(client_ctx);
}

TEST_F(CommSslTest, CloseWithBufferedTlsDataSurvivesPeerCloseRace) {
    BIO* server_io = nullptr;
    BIO* client_io = nullptr;
    ASSERT_EQ(BIO_new_bio_pair(&server_io, 0, &client_io, 0), 1);
    ASSERT_NE(server_io, nullptr);
    ASSERT_NE(client_io, nullptr);

    const int slot = comm_abstract_add_bio(server_io, server_io, -1, C_SOCKET_READABLE);
    ASSERT_GE(slot, 0);

    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);

    SSL* client_ssl = SSL_new(client_ctx);
    ASSERT_NE(client_ssl, nullptr);
    SSL_set_connect_state(client_ssl);
    SSL_set_bio(client_ssl, client_io, client_io);

    comm_enable_tls(slot);
    async_runtime_t* runtime = async_runtime_init(nullptr);
    ASSERT_NE(runtime, nullptr);

    bool server_done = false;
    bool client_done = false;
    for (int i = 0; i < 256 && !(server_done && client_done); ++i) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
        ASSERT_TRUE(comm_refill_inbound_buffers(comm));
        if (!server_done) {
            const int result = comm_tls_handshake_step(runtime, slot);
            ASSERT_GE(result, 0);
            server_done = result == 1;
        }
        if (!client_done) {
            const int result = SSL_do_handshake(client_ssl);
            if (result == 1) {
                client_done = true;
            } else {
                const int error = SSL_get_error(client_ssl, result);
                ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
            }
        }
    }
    ASSERT_TRUE(server_done);
    ASSERT_TRUE(client_done);

    comm_buffered_write(slot, "bye", 3);
    EXPECT_FALSE(comm_close(runtime, slot));

    // Simulate abrupt peer close while server still has buffered TLS data to flush.
    SSL_free(client_ssl);
    client_ssl = nullptr;

    comm_flush(runtime, slot);

    comm_abstract_t* comm = comm_abstract_get(slot);
    ASSERT_NE(comm, nullptr);
    EXPECT_NE(comm->flags & C_CLOSING, 0u);
    EXPECT_EQ(comm->flags & C_BUFFERED_WRITE, 0u);
    EXPECT_EQ(comm->flags & C_TLS_ESTABLISHED, 0u);

    // A later writable event must not restart a TLS handshake after the
    // closing write failure cleared the established flag.
    comm_flush(runtime, slot);

    EXPECT_TRUE(comm_close(runtime, slot));
    EXPECT_EQ(comm_abstract_get(slot), nullptr);

    async_runtime_deinit(runtime);
    SSL_CTX_free(client_ctx);
}

#ifdef _WIN32
TEST_F(CommSslTest, TlsInboundSupportsFragmentedSrcBufferPath) {
    BIO* server_io = nullptr;
    BIO* client_io = nullptr;
    ASSERT_EQ(BIO_new_bio_pair(&server_io, 0, &client_io, 0), 1);
    ASSERT_NE(server_io, nullptr);
    ASSERT_NE(client_io, nullptr);

    const int slot = comm_abstract_add_bio(server_io, server_io, -1, C_SOCKET_READABLE | C_LINE_INPUT);
    ASSERT_GE(slot, 0);

    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);

    SSL* client_ssl = SSL_new(client_ctx);
    ASSERT_NE(client_ssl, nullptr);
    SSL_set_connect_state(client_ssl);
    SSL_set_bio(client_ssl, client_io, client_io);

    inbound_collector_t collector;
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, inbound_collector_t::hook));

    comm_enable_tls(slot);

    async_runtime_t* runtime = async_runtime_init(&collector);
    ASSERT_NE(runtime, nullptr);

    bool server_done = false;
    bool client_done = false;
    for (int i = 0; i < 256 && !(server_done && client_done); ++i) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);

        ASSERT_TRUE(feed_tls_from_src_fragments(comm, runtime, 7));

        if (!server_done)
            server_done = (comm->ssl && SSL_is_init_finished(comm->ssl) == 1);

        if (!client_done) {
            const int c = SSL_do_handshake(client_ssl);
            if (c == 1) {
                client_done = true;
            } else {
                const int err = SSL_get_error(client_ssl, c);
                ASSERT_TRUE(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    << "unexpected client SSL error: " << err;
            }
        }
    }

    ASSERT_TRUE(server_done);
    ASSERT_TRUE(client_done);

    const char* line = "iocp tls fragmented path\n";
    size_t sent = 0;
    ASSERT_EQ(SSL_write_ex(client_ssl, line, strlen(line), &sent), 1);
    ASSERT_EQ(sent, strlen(line));

    for (int i = 0; i < 256 && collector.messages.empty(); ++i) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);

        ASSERT_TRUE(feed_tls_from_src_fragments(comm, runtime, 3));
        ASSERT_EQ(comm_process_input(runtime, comm, -1), 0);
    }

    ASSERT_FALSE(collector.messages.empty());
    EXPECT_EQ(collector.messages.front(), "iocp tls fragmented path");

    async_runtime_deinit(runtime);
    SSL_free(client_ssl);
    SSL_CTX_free(client_ctx);
}

TEST_F(CommSslTest, TlsInboundSupportsSingleByteSrcFragments) {
    BIO* server_io = nullptr;
    BIO* client_io = nullptr;
    ASSERT_EQ(BIO_new_bio_pair(&server_io, 0, &client_io, 0), 1);
    ASSERT_NE(server_io, nullptr);
    ASSERT_NE(client_io, nullptr);

    const int slot = comm_abstract_add_bio(server_io, server_io, -1, C_SOCKET_READABLE | C_LINE_INPUT);
    ASSERT_GE(slot, 0);

    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);

    SSL* client_ssl = SSL_new(client_ctx);
    ASSERT_NE(client_ssl, nullptr);
    SSL_set_connect_state(client_ssl);
    SSL_set_bio(client_ssl, client_io, client_io);

    inbound_collector_t collector;
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, inbound_collector_t::hook));

    comm_enable_tls(slot);

    async_runtime_t* runtime = async_runtime_init(&collector);
    ASSERT_NE(runtime, nullptr);

    bool server_done = false;
    bool client_done = false;
    for (int i = 0; i < 1024 && !(server_done && client_done); ++i) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);

        ASSERT_TRUE(feed_tls_from_src_fragments(comm, runtime, 1));

        if (!server_done)
            server_done = (comm->ssl && SSL_is_init_finished(comm->ssl) == 1);

        if (!client_done) {
            const int c = SSL_do_handshake(client_ssl);
            if (c == 1) {
                client_done = true;
            } else {
                const int err = SSL_get_error(client_ssl, c);
                ASSERT_TRUE(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    << "unexpected client SSL error: " << err;
            }
        }
    }

    ASSERT_TRUE(server_done);
    ASSERT_TRUE(client_done);

    const char* line = "single byte tls fragment path\n";
    size_t sent = 0;
    ASSERT_EQ(SSL_write_ex(client_ssl, line, strlen(line), &sent), 1);
    ASSERT_EQ(sent, strlen(line));

    for (int i = 0; i < 2048 && collector.messages.empty(); ++i) {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);

        ASSERT_TRUE(feed_tls_from_src_fragments(comm, runtime, 1));
        ASSERT_EQ(comm_process_input(runtime, comm, -1), 0);
    }

    ASSERT_FALSE(collector.messages.empty());
    EXPECT_EQ(collector.messages.front(), "single byte tls fragment path");

    async_runtime_deinit(runtime);
    SSL_free(client_ssl);
    SSL_CTX_free(client_ctx);
}
#endif
