#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "comm/input_mode.hpp"
#include "comm/abstract.hpp"
#include "mudmux/comm.h"

#include <gtest/gtest.h>
#include <openssl/bio.h>

using namespace testing;

class CommInputModeTest : public Test {
protected:
	void TearDown() override {
		comm_abstract_remove_all();
	}

	static int add_comm_slot(uint32_t flags = 0) {
		BIO* rbio = BIO_new(BIO_s_mem());
		BIO* wbio = BIO_new(BIO_s_mem());
		if (!rbio || !wbio) {
			if (rbio)
				BIO_free_all(rbio);
			if (wbio)
				BIO_free_all(wbio);
			return -1;
		}
		return comm_abstract_add_bio(rbio, wbio, -1, flags);
	}
};

TEST_F(CommInputModeTest, SetLineInputEnablesLineAndEchoFlags) {
	const int slot = add_comm_slot();
	ASSERT_NE(slot, -1);

	EXPECT_TRUE(comm_set_line_input(slot, true));

	comm_abstract_t* comm = comm_abstract_get(slot);
	ASSERT_NE(comm, nullptr);
	EXPECT_NE(comm->flags & C_LINE_INPUT, 0u);
	EXPECT_NE(comm->flags & C_CLIENT_ECHO, 0u);
}

TEST_F(CommInputModeTest, SetLineInputCanDisableEchoWhileKeepingLineMode) {
	const int slot = add_comm_slot();
	ASSERT_NE(slot, -1);

	EXPECT_TRUE(comm_set_line_input(slot, true));
	EXPECT_TRUE(comm_set_line_input(slot, false));

	comm_abstract_t* comm = comm_abstract_get(slot);
	ASSERT_NE(comm, nullptr);
	EXPECT_NE(comm->flags & C_LINE_INPUT, 0u);
	EXPECT_EQ(comm->flags & C_CLIENT_ECHO, 0u);
}

TEST_F(CommInputModeTest, SetCharInputClearsLineAndEchoFlags) {
	const int slot = add_comm_slot(C_LINE_INPUT | C_CLIENT_ECHO);
	ASSERT_NE(slot, -1);

	EXPECT_TRUE(comm_set_char_input(slot));

	comm_abstract_t* comm = comm_abstract_get(slot);
	ASSERT_NE(comm, nullptr);
	EXPECT_EQ(comm->flags & C_LINE_INPUT, 0u);
	EXPECT_EQ(comm->flags & C_CLIENT_ECHO, 0u);
}

TEST_F(CommInputModeTest, SetInputModeRejectsInvalidSlot) {
	EXPECT_FALSE(comm_set_line_input(-1, true));
	EXPECT_FALSE(comm_set_char_input(-1));
}

