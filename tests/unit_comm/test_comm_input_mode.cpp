#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "comm/input_mode.hpp"
#include "comm/abstract.hpp"
#include "comm/outbound.hpp"
#include "mudmux/comm.h"

#include <array>
#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <string>

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

TEST_F(CommInputModeTest, SetCharInputWithTelnetLinemodeSendsModeAndEchoNegotiation) {
	const int slot = add_comm_slot(C_ENABLE_TELNET | C_LINE_INPUT | C_CLIENT_ECHO);
	ASSERT_NE(slot, -1);

	comm_abstract_t* comm = comm_abstract_get(slot);
	ASSERT_NE(comm, nullptr);
	comm->caps.telnet_linemode = 1;

	EXPECT_TRUE(comm_set_char_input(slot));
	comm_flush(nullptr, slot);

	std::array<char, 64> buf{};
	const int read_len = BIO_read(comm->wbio, buf.data(), static_cast<int>(buf.size()));
	ASSERT_GT(read_len, 0);
	const std::string wire(buf.data(), static_cast<size_t>(read_len));

	const std::string mode_char_req("\xff\xfa\x22\x01\x00\xff\xf0", 7); // IAC SB LINEMODE MODE 0 IAC SE
	const std::string will_echo("\xff\xfb\x01", 3); // IAC WILL ECHO

	EXPECT_NE(wire.find(mode_char_req), std::string::npos);
	EXPECT_NE(wire.find(will_echo), std::string::npos);
	EXPECT_EQ(wire.find(will_echo), wire.rfind(will_echo)); // must not emit duplicate WILL ECHO

	EXPECT_EQ(comm->flags & C_LINE_INPUT, 0u);
	EXPECT_EQ(comm->flags & C_CLIENT_ECHO, 0u);
}

TEST_F(CommInputModeTest, SetEchoUsesNegotiatedTelnetEchoCapability) {
	const int slot = add_comm_slot(C_ENABLE_TELNET);
	ASSERT_NE(slot, -1);

	comm_abstract_t* comm = comm_abstract_get(slot);
	ASSERT_NE(comm, nullptr);

	EXPECT_TRUE(comm_set_echo(slot, false));
	comm_flush(nullptr, slot);

	std::array<char, 64> buf{};
	int read_len = BIO_read(comm->wbio, buf.data(), static_cast<int>(buf.size()));
	ASSERT_GT(read_len, 0);
	std::string wire(buf.data(), static_cast<size_t>(read_len));
	const std::string will_echo("\xff\xfb\x01", 3); // IAC WILL ECHO
	EXPECT_EQ(wire, will_echo);
	EXPECT_EQ(comm->flags & C_CLIENT_ECHO, 0u);
	EXPECT_EQ(comm->caps.telnet_echo, 0u);

	comm->caps.telnet_echo = 1;
	EXPECT_TRUE(comm_set_echo(slot, false));
	comm_flush(nullptr, slot);
	read_len = BIO_read(comm->wbio, buf.data(), static_cast<int>(buf.size()));
	EXPECT_LE(read_len, 0);
	EXPECT_EQ(comm->flags & C_CLIENT_ECHO, 0u);

	EXPECT_TRUE(comm_set_echo(slot, true));
	comm_flush(nullptr, slot);
	read_len = BIO_read(comm->wbio, buf.data(), static_cast<int>(buf.size()));
	ASSERT_GT(read_len, 0);
	wire.assign(buf.data(), static_cast<size_t>(read_len));
	const std::string wont_echo("\xff\xfc\x01", 3); // IAC WONT ECHO
	EXPECT_EQ(wire, wont_echo);
	EXPECT_NE(comm->flags & C_CLIENT_ECHO, 0u);
	EXPECT_NE(comm->caps.telnet_echo, 0u);
}

TEST_F(CommInputModeTest, SetEchoOnTlsTelnetUpdatesDesiredStateWithoutNegotiation) {
	const int slot = add_comm_slot(C_ENABLE_TELNET | C_CLIENT_ECHO);
	ASSERT_NE(slot, -1);

	comm_abstract_t* comm = comm_abstract_get(slot);
	ASSERT_NE(comm, nullptr);

	comm->ssl = reinterpret_cast<SSL*>(1);

	EXPECT_TRUE(comm_set_echo(slot, false));

	std::array<char, 64> buf{};
	const int read_len = BIO_read(comm->wbio, buf.data(), static_cast<int>(buf.size()));
	EXPECT_LE(read_len, 0);
	EXPECT_EQ(comm->flags & C_CLIENT_ECHO, 0u);
	EXPECT_EQ(comm->caps.telnet_echo, 0u);
	EXPECT_EQ(comm->flags & C_BUFFERED_WRITE, 0u);
	comm->ssl = nullptr;
}
