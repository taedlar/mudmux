#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtest/gtest.h>
#include <openssl/bio.h>

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
