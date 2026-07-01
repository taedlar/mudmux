#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtest/gtest.h>
#ifndef MUDMUX_NO_OPENSSL
#include <openssl/bio.h>
#endif
#include "comm/abstract.h"

using namespace testing;

#ifndef MUDMUX_NO_OPENSSL
TEST(CommTest, AbstractAddBio) {
    EXPECT_EQ(comm_abstract_add_bio(nullptr, nullptr, -1), -1); // both rbio and wbio are null: reject
    EXPECT_NE(comm_abstract_add_bio(BIO_new_fp (stdin, BIO_NOCLOSE), nullptr, -1), -1); // read-only: accept
    EXPECT_NE(comm_abstract_add_bio(nullptr, BIO_new_fp (stdout, BIO_NOCLOSE), -1), -1); // write-only: accept
    BIO* bio = BIO_new_fp (stdin, BIO_NOCLOSE);
    EXPECT_NE(comm_abstract_add_bio(bio, bio, -1), -1); // bi-directional: accept
    ASSERT_NO_FATAL_FAILURE(comm_abstract_cleanup());
}
#endif

TEST(CommTest, AbstractGet) {
    EXPECT_EQ(comm_abstract_get(-1), nullptr);
    EXPECT_EQ(comm_abstract_get(100), nullptr);
}

TEST(CommTest, AbstractRemove) {
    EXPECT_EQ(comm_abstract_remove(-1), -1);
    EXPECT_EQ(comm_abstract_remove(100), -1);
}
