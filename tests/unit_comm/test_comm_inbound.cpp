#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "comm/inbound.hpp"
#include "comm/input_mode.hpp"
#include "../../src/execution.hpp"
#include "mudmux/mudmux.h"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

#include <string>
#include <vector>
#include <atomic>
#include <future>
#include <mutex>
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

    static int hook_telnet_subneg(void* ctx, int option, void* data, size_t len) {
        CommInboundTest* test_instance = static_cast<CommInboundTest*>(ctx);
        if (test_instance) {
            std::string payload(static_cast<char*>(data), len);
            test_instance->inbound_messages.push_back(std::string("subneg:") + std::to_string(option) + ":" + payload);
        }
        return 0;
    }
};

namespace {

std::promise<void>* thread_pool_first_slot_entered_ptr{nullptr};
std::promise<void>* thread_pool_other_slots_done_ptr{nullptr};
std::shared_future<void>* thread_pool_first_slot_release_future_ptr{nullptr};
std::promise<void>* thread_pool_first_slot_finished_ptr{nullptr};
std::atomic<int> thread_pool_other_slots_completed{0};
int thread_pool_other_slots_expected{0};
int thread_pool_first_slot_count{0};

std::promise<void>* comm_api_blocked_slot_entered_ptr{nullptr};
std::promise<void>* comm_api_blocked_slot_finished_ptr{nullptr};
std::promise<void>* comm_api_other_slots_done_ptr{nullptr};
std::shared_future<void>* comm_api_blocked_slot_release_ptr{nullptr};
std::atomic<int> comm_api_other_slots_completed{0};
int comm_api_other_slots_expected{0};
int comm_api_blocked_slot{-1};

int thread_pool_stress_hook(void*, int slot, void* data, size_t len) {
    (void)data;
    (void)len;

    if (slot == 0) {
        const int count = ++thread_pool_first_slot_count;
        if (count == 1 && thread_pool_first_slot_entered_ptr)
            thread_pool_first_slot_entered_ptr->set_value();
        if (count == 2) {
            if (thread_pool_first_slot_release_future_ptr)
                thread_pool_first_slot_release_future_ptr->wait();
            if (thread_pool_first_slot_finished_ptr)
                thread_pool_first_slot_finished_ptr->set_value();
        }
        return 0;
    }

    const int completed = thread_pool_other_slots_completed.fetch_add(1) + 1;
    if (completed == thread_pool_other_slots_expected && thread_pool_other_slots_done_ptr)
        thread_pool_other_slots_done_ptr->set_value();
    return 0;
}

int concurrent_comm_api_hook(void*, int slot, void*, size_t) {
    if (mudmux_comm_api_v1->get_flags_slot(slot) == 0u)
        return -1;

    mudmux_comm_api_v1->set_echo(slot, false);
    mudmux_comm_api_v1->set_char_input(slot);
    mudmux_comm_api_v1->set_line_input(slot, true);
    mudmux_comm_api_v1->enable_prompt(slot, true);
    mudmux_comm_api_v1->buffered_write_slot(slot, "ok", 2);

    if (slot == comm_api_blocked_slot) {
        if (comm_api_blocked_slot_entered_ptr)
            comm_api_blocked_slot_entered_ptr->set_value();
        if (comm_api_blocked_slot_release_ptr)
            comm_api_blocked_slot_release_ptr->wait();
        if (comm_api_blocked_slot_finished_ptr)
            comm_api_blocked_slot_finished_ptr->set_value();
        return 0;
    }

    const int completed = comm_api_other_slots_completed.fetch_add(1) + 1;
    if (completed == comm_api_other_slots_expected && comm_api_other_slots_done_ptr)
        comm_api_other_slots_done_ptr->set_value();
    return 0;
}

} // namespace

TEST_F(CommInboundTest, RefillInboundBuffers) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_LINE_INPUT); // Add a memory comm slot with line input mode
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx); // Assuming slot 0 for testing
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    const char* test_data = "Test data\n";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, test_data, strlen(test_data)));
    EXPECT_EQ(comm_process_input(runtime, comm, 1), 0);

    EXPECT_EQ(inbound_messages.size(), 1u); // Expect one inbound message
    EXPECT_EQ(inbound_messages[0], "Test data");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ProcessLineInputModeDispatchesCompleteLines) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_LINE_INPUT); // Add a memory comm slot with line input mode
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

    const int slot = add_memory_comm(0); // Add a memory comm slot with character input mode
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    const char* data = "ab";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, data, strlen(data)));
    EXPECT_EQ(comm_process_input(runtime, comm, 1), 0);
    EXPECT_EQ(comm_process_input(runtime, comm, 1), 0);

    ASSERT_EQ(inbound_messages.size(), 2u);
    EXPECT_EQ(inbound_messages[0], "a");
    EXPECT_EQ(inbound_messages[1], "b");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ProcessCharInputModeTreatsAnsiSequenceAsSingleMessage) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_ENABLE_ANSI); // Add a memory comm slot with ANSI support
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    const char* data = "\x1B[Ax";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, data, strlen(data)));

    EXPECT_EQ(comm_process_input(runtime, comm, 1), 0);
    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "\x1B[A");

    EXPECT_EQ(comm_process_input(runtime, comm, 1), 0);
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

TEST_F(CommInboundTest, RefillInboundBuffersFromBioPreservesTelnetAndDataOrder) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_ENABLE_TELNET | C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    const unsigned char payload[] = {
        'h', 'e', 'l', 'l', 'o',
        255, 250, 31, 'a', 'b', 255, 240,
        'w', 'o', 'r', 'l', 'd', '\n'
    };
    ASSERT_EQ(BIO_write(comm->rbio, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));

    ASSERT_TRUE(comm_refill_inbound_buffers(comm));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);

    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "helloworld");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, TelnetSubnegHookDispatchesBeforeSubsequentLineInput) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_ENABLE_TELNET | C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);
    mudmux_register_hook(HOOK_TELNET_SUBNEG, CommInboundTest::hook_telnet_subneg);

    const unsigned char payload[] = {
        'f', 'o', 'o',
        255, 250, 24, 'x', 'y', 255, 240,
        'b', 'a', 'r', '\n'
    };
    ASSERT_EQ(BIO_write(comm->rbio, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));

    ASSERT_TRUE(comm_refill_inbound_buffers(comm));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);

    ASSERT_EQ(inbound_messages.size(), 2u);
    EXPECT_EQ(inbound_messages[0], "subneg:24:xy");
    EXPECT_EQ(inbound_messages[1], "foobar");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ThreadPoolKeepsPerSlotOrderWhileOtherSlotsAdvance) {
    mudmux_deinit();
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 2}}}"));

    std::promise<void> first_slot_entered_promise;
    std::future<void> first_slot_entered_future = first_slot_entered_promise.get_future();
    std::promise<void> other_slots_done_promise;
    std::future<void> other_slots_done_future = other_slots_done_promise.get_future();
    std::promise<void> first_slot_released_promise;
    std::shared_future<void> first_slot_release_future = first_slot_released_promise.get_future().share();
    std::promise<void> first_slot_finished_promise;
    std::future<void> first_slot_finished_future = first_slot_finished_promise.get_future();

    thread_pool_first_slot_entered_ptr = &first_slot_entered_promise;
    thread_pool_other_slots_done_ptr = &other_slots_done_promise;
    thread_pool_first_slot_release_future_ptr = &first_slot_release_future;
    thread_pool_first_slot_finished_ptr = &first_slot_finished_promise;
    thread_pool_other_slots_completed.store(0);
    thread_pool_other_slots_expected = 5;
    thread_pool_first_slot_count = 0;

    ASSERT_TRUE(mudmux_execution_start());
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, thread_pool_stress_hook));

    const char slot0_first[] = "slot0-first";
    const char slot0_second[] = "slot0-second";
    ASSERT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, 0, slot0_first, strlen(slot0_first)), MUDMUX_DISPATCH_OK);
    ASSERT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, 0, slot0_second, strlen(slot0_second)), MUDMUX_DISPATCH_OK);

    ASSERT_EQ(first_slot_entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    for (int slot = 1; slot <= 5; ++slot) {
        const std::string payload = "slot" + std::to_string(slot);
        ASSERT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, slot, payload.data(), payload.size()), MUDMUX_DISPATCH_OK);
    }

    ASSERT_EQ(other_slots_done_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    first_slot_released_promise.set_value();
    ASSERT_EQ(first_slot_finished_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    thread_pool_first_slot_entered_ptr = nullptr;
    thread_pool_other_slots_done_ptr = nullptr;
    thread_pool_first_slot_release_future_ptr = nullptr;
    thread_pool_first_slot_finished_ptr = nullptr;
    mudmux_execution_stop();
    mudmux_deinit();
}

TEST_F(CommInboundTest, RelaxedModeCommApiCallsFromConcurrentHooksDoNotDeadlock) {
    mudmux_deinit();
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 4}}}"));

    std::vector<int> slots;
    slots.reserve(6);
    for (int index = 0; index < 6; ++index) {
        const int slot = add_memory_comm(C_LINE_INPUT);
        ASSERT_NE(slot, -1);
        slots.push_back(slot);
    }
    comm_api_blocked_slot = slots.front();

    std::promise<void> blocked_slot_entered_promise;
    std::future<void> blocked_slot_entered_future = blocked_slot_entered_promise.get_future();
    std::promise<void> blocked_slot_release_promise;
    std::shared_future<void> blocked_slot_release_future = blocked_slot_release_promise.get_future().share();
    std::promise<void> blocked_slot_finished_promise;
    std::future<void> blocked_slot_finished_future = blocked_slot_finished_promise.get_future();
    std::promise<void> other_slots_done_promise;
    std::future<void> other_slots_done_future = other_slots_done_promise.get_future();

    comm_api_blocked_slot_entered_ptr = &blocked_slot_entered_promise;
    comm_api_blocked_slot_release_ptr = &blocked_slot_release_future;
    comm_api_blocked_slot_finished_ptr = &blocked_slot_finished_promise;
    comm_api_other_slots_done_ptr = &other_slots_done_promise;
    comm_api_other_slots_completed.store(0);
    comm_api_other_slots_expected = static_cast<int>(slots.size()) - 1;

    ASSERT_TRUE(mudmux_execution_start());
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, concurrent_comm_api_hook));

    const char payload[] = "phase6";
    ASSERT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, comm_api_blocked_slot, payload, strlen(payload)), MUDMUX_DISPATCH_OK);
    ASSERT_EQ(blocked_slot_entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    for (std::size_t index = 1; index < slots.size(); ++index) {
        ASSERT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, slots[index], payload, strlen(payload)), MUDMUX_DISPATCH_OK);
    }

    ASSERT_EQ(other_slots_done_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    blocked_slot_release_promise.set_value();
    ASSERT_EQ(blocked_slot_finished_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    comm_api_blocked_slot_entered_ptr = nullptr;
    comm_api_blocked_slot_release_ptr = nullptr;
    comm_api_blocked_slot_finished_ptr = nullptr;
    comm_api_other_slots_done_ptr = nullptr;
    comm_api_other_slots_expected = 0;
    comm_api_blocked_slot = -1;

    mudmux_execution_stop();
    mudmux_deinit();
}
