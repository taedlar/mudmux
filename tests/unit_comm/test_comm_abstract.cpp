#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtest/gtest.h>
#include <limits>
#include <openssl/bio.h>

#include "comm/accept.hpp"
#include "comm/abstract.hpp"
#include "mudmux/comm.h"

using namespace testing;

TEST(CommTest, AbstractAddBio) {
    EXPECT_EQ(comm_abstract_add_bio(nullptr, nullptr, -1, 0), -1); // both rbio and wbio are null: reject
    int slot_r = comm_abstract_add_bio(BIO_new_fp (stdin, BIO_NOCLOSE), nullptr, -1, 0);
    EXPECT_NE(slot_r, -1); // read-only: accept
    int slot_w = comm_abstract_add_bio(nullptr, BIO_new_fp (stdout, BIO_NOCLOSE), -1, 0);
    EXPECT_NE(slot_w, -1); // write-only: accept
    BIO* bio = BIO_new_fp (stdin, BIO_NOCLOSE);
    int slot_rw = comm_abstract_add_bio(bio, bio, -1, 0);
    EXPECT_NE(slot_rw, -1); // bi-directional: accept

    if (slot_r >= 0) {
        EXPECT_TRUE(comm_close(nullptr, slot_r));
    }
    if (slot_w >= 0) {
        EXPECT_TRUE(comm_close(nullptr, slot_w));
    }
    if (slot_rw >= 0) {
        EXPECT_TRUE(comm_close(nullptr, slot_rw));
    }
    EXPECT_GE(comm_max_slot(), 3);
}

TEST(CommTest, AbstractGet) {
    EXPECT_EQ(comm_abstract_get(-1), nullptr);
    EXPECT_EQ(comm_abstract_get(comm_max_slot()), nullptr);
}

TEST(CommTest, AbstractRAIIGuard) {
    std::recursive_mutex mtx;
#ifdef _WIN32
    int slot = comm_abstract_add_bio(BIO_new_fd (_fileno(stdin), BIO_NOCLOSE), nullptr, -1, 0);
#else
    int slot = comm_abstract_add_bio(BIO_new_fd (STDIN_FILENO, BIO_NOCLOSE), nullptr, -1, 0);
#endif
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, mtx); // for unit-testing, not actual layer guards
    EXPECT_NE(comm.get(), nullptr);
    EXPECT_EQ(comm.slot(), slot);
    EXPECT_TRUE(comm.has_rbio());
    EXPECT_FALSE(comm.has_wbio());
    socket_fd_t fd = INVALID_SOCKET_FD;
    EXPECT_TRUE(comm.get_rbio_fd(&fd));
    EXPECT_NE(fd, INVALID_SOCKET_FD);

    EXPECT_TRUE(comm_close(nullptr, slot));
}

TEST(CommTest, AbstractCloseInvalidSlot) {
    EXPECT_TRUE(comm_close(nullptr, -1));
    EXPECT_TRUE(comm_close(nullptr, comm_max_slot()));
}

TEST(CommTest, SocketFdToBioFdRejectsInvalidSocket) {
    int bio_fd = 123;
    EXPECT_FALSE(comm_socket_fd_to_bio_fd(INVALID_SOCKET_FD, &bio_fd));
    EXPECT_EQ(bio_fd, 123);
}

TEST(CommTest, SocketFdToBioFdRoundTripsIntMax) {
    const int source_bio_fd = (std::numeric_limits<int>::max)();
    const socket_fd_t socket_fd = comm_bio_fd_to_socket_fd(source_bio_fd);

    int round_trip_bio_fd = -1;
    ASSERT_TRUE(comm_socket_fd_to_bio_fd(socket_fd, &round_trip_bio_fd));
    EXPECT_EQ(round_trip_bio_fd, source_bio_fd);
}

TEST(CommTest, AcceptRequiresTcpScheme) {
    async_runtime_t* runtime = async_runtime_init(nullptr);
    ASSERT_NE(runtime, nullptr);

    EXPECT_EQ(comm_accept(runtime, "*:4000"), -1);
    EXPECT_EQ(comm_accept(runtime, "udp://*:4000"), -1);

    async_runtime_deinit(runtime);
}

#ifdef _WIN32
TEST(CommTest, SocketFdToBioFdRejectsValuesAboveIntMax) {
    const socket_fd_t oversized_socket_fd =
        static_cast<socket_fd_t>(static_cast<unsigned long long>((std::numeric_limits<int>::max)()) + 1ull);

    int bio_fd = -1;
    EXPECT_FALSE(comm_socket_fd_to_bio_fd(oversized_socket_fd, &bio_fd));
    EXPECT_EQ(bio_fd, -1);
}
#endif
