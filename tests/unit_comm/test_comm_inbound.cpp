#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "comm/inbound.hpp"
#include "comm/input_mode.hpp"
#include "mudmux/mudmux.h"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <openssl/bio.h>

using namespace testing;

class CommInboundTest : public Test {
protected:
    void SetUp() override {
        // Setup code before each test
        mudmux_set_log_level(0);
        SPDLOG_DEBUG("CTEST_FULL_OUTPUT");
        ASSERT_TRUE(mudmux_init(nullptr)); // Initialize mudmux for testing
        ASSERT_EQ(comm_abstract_add_file (nullptr, nullptr, 0, C_LINE_INPUT), 0); // Add a console slot for testing
    }
    void TearDown() override {
        // Cleanup code after each test
        comm_abstract_remove_all(); // Remove all comm slots
        mudmux_deinit(); // Deinitialize mudmux after testing
    }

public:
    std::vector<std::string> inbound_messages; // Store inbound messages for verification

    int add_memory_comm(uint32_t flags) {
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

    static int hook_message_inbound(void* ctx, int, void* data, size_t len) {
        CommInboundTest* test_instance = static_cast<CommInboundTest*>(ctx);
        if (test_instance) {
            test_instance->inbound_messages.push_back(std::string(static_cast<char*>(data), len));
        }
        return 0; // Indicate success
    }
};

TEST_F(CommInboundTest, RefillInboundBuffers) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    comm_abstract_ptr comm(0, comm_slots_mtx); // Assuming slot 0 for testing
    const char* test_data = "Test data\n";
    size_t test_size = strlen(test_data);

    bool result = comm_refill_inbound_buffers(comm, test_data, test_size);
    EXPECT_TRUE(result);

    comm_set_line_input(0, true); // Set line input mode for the comm slot
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);
    int ret = comm_process_input(runtime, comm, 1);
    EXPECT_EQ(ret, 0); // Expect success

    async_runtime_deinit(runtime);

    EXPECT_EQ(inbound_messages.size(), 1); // Expect one inbound message
    ASSERT_FALSE(inbound_messages.empty());
    EXPECT_EQ(inbound_messages[0], "Test data");
}

TEST_F(CommInboundTest, ProcessLineInputModeDispatchesCompleteLines) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    const char* data = "  hello  \r\nworld\npartial";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, data, strlen(data)));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);

    ASSERT_EQ(inbound_messages.size(), 2u);
    EXPECT_EQ(inbound_messages[0], "hello");
    EXPECT_EQ(inbound_messages[1], "world");

    ASSERT_TRUE(comm_refill_inbound_buffers(comm, "\n", 1));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);

    ASSERT_EQ(inbound_messages.size(), 3u);
    EXPECT_EQ(inbound_messages[2], "partial");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ProcessCharInputModeDispatchesOneCharacterAtATime) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(0);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    const char* data = "ab";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, data, strlen(data)));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);
    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);

    ASSERT_EQ(inbound_messages.size(), 2u);
    EXPECT_EQ(inbound_messages[0], "a");
    EXPECT_EQ(inbound_messages[1], "b");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ProcessCharInputModeTreatsAnsiSequenceAsSingleMessage) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_ENABLE_ANSI);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    const char* data = "\x1B[Ax";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, data, strlen(data)));

    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);
    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "\x1B[A");

    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);
    ASSERT_EQ(inbound_messages.size(), 2u);
    EXPECT_EQ(inbound_messages[1], "x");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ProcessCharInputModeWaitsForCompleteAnsiSequence) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_ENABLE_ANSI);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    const char* partial = "\x1B[";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, partial, strlen(partial)));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);
    EXPECT_TRUE(inbound_messages.empty());

    const char* complete = "31~";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, complete, strlen(complete)));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);

    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "\x1B[31~");

    async_runtime_deinit(runtime);
}
