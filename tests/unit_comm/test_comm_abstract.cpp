#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtest/gtest.h>
#include <limits>
#include <openssl/bio.h>

#include "comm/abstract.h"
#include "mudmux/mudmux.h"
#include "mudmux/comm.h"

using namespace testing;

TEST(CommTest, AbstractAddBio) {
    mudmux_init(nullptr);
    EXPECT_EQ(comm_abstract_add_bio(nullptr, nullptr, -1, 0), -1); // both rbio and wbio are null: reject
    EXPECT_NE(comm_abstract_add_bio(BIO_new_fp (stdin, BIO_NOCLOSE), nullptr, -1, 0), -1); // read-only: accept
    EXPECT_NE(comm_abstract_add_bio(nullptr, BIO_new_fp (stdout, BIO_NOCLOSE), -1, 0), -1); // write-only: accept
    BIO* bio = BIO_new_fp (stdin, BIO_NOCLOSE);
    EXPECT_NE(comm_abstract_add_bio(bio, bio, -1, 0), -1); // bi-directional: accept
    ASSERT_NO_FATAL_FAILURE(comm_abstract_cleanup());
}

TEST(CommTest, AbstractGet) {
    mudmux_init(nullptr);
    EXPECT_EQ(comm_abstract_get(-1), nullptr);
    EXPECT_EQ(comm_abstract_get(100), nullptr);
}

TEST(CommTest, AbstractRemove) {
    mudmux_init(nullptr);
    EXPECT_EQ(comm_abstract_remove(-1), -1);
    EXPECT_EQ(comm_abstract_remove(100), -1);
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

#ifdef _WIN32
TEST(CommTest, SocketFdToBioFdRejectsValuesAboveIntMax) {
    const socket_fd_t oversized_socket_fd =
        static_cast<socket_fd_t>(static_cast<unsigned long long>((std::numeric_limits<int>::max)()) + 1ull);

    int bio_fd = -1;
    EXPECT_FALSE(comm_socket_fd_to_bio_fd(oversized_socket_fd, &bio_fd));
    EXPECT_EQ(bio_fd, -1);
}
#endif
