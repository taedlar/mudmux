#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "comm/current_slot.hpp"
#include "comm/inbound.hpp"
#include "comm/input_mode.hpp"
#include "comm/outbound.hpp"
#include "comm/telnet.hpp"
#include "comm/websocket.hpp"
#include "../../src/execution.hpp"
#include "mudmux/mudmux.h"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"
#include "mudmux/workers.h"

#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
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
    enum class ConnectTransport {
        Telnet,
        WebSocket,
        WebSocketTelnet
    };

    std::vector<std::string> inbound_messages; // Store inbound messages for verification
    ConnectTransport connect_transport{ConnectTransport::Telnet};
    int observed_current_slot{-2};

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

    static int hook_message_inbound_and_rearm_char(void* ctx, int slot, void* data, size_t len) {
        const int result = hook_message_inbound(ctx, slot, data, len);
        return comm_set_char_input(slot) ? result : -1;
    }

    static int hook_record_current_slot(void* ctx, int, void*, size_t) {
        CommInboundTest* test_instance = static_cast<CommInboundTest*>(ctx);
        if (test_instance)
            test_instance->observed_current_slot = comm_current_slot();
        return 0;
    }

    static int hook_connect_select_transport(void* ctx, int, void*, size_t) {
        CommInboundTest* test_instance = static_cast<CommInboundTest*>(ctx);
        if (!test_instance)
            return -1;
        switch (test_instance->connect_transport) {
        case ConnectTransport::Telnet:
            mudmux_comm_api_v1->enable_telnet();
            return 0;
        case ConnectTransport::WebSocket:
            return mudmux_comm_api_v1->enable_websocket(nullptr) ? 0 : -1;
        case ConnectTransport::WebSocketTelnet:
            return mudmux_comm_api_v1->enable_websocket("telnet.ietf.org") ? 0 : -1;
        }
        return -1;
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

int outbound_hook_slot{-1};
std::string outbound_hook_message;

int rewrite_outbound_message(void*, int slot, void* data, size_t len) {
    outbound_hook_slot = slot;
    outbound_hook_message.assign(static_cast<char*>(data), len);
    comm_buffered_write(outbound_hook_slot, data, len);
    return 0;
}

int outbound_hook_calls{0};

int outbound_attempts_recursive_add(void*, int slot, void* data, size_t len) {
    ++outbound_hook_calls;
    comm_add_message(slot, data, len);
    return 0;
}

int outbound_cross_slot{-1};

int outbound_attempts_cross_slot_write(void*, int, void* data, size_t len) {
    ++outbound_hook_calls;
    comm_buffered_write(outbound_cross_slot, data, len);
    return 0;
}

int inbound_attempt_buffered_write(void*, int slot, void*, size_t) {
    comm_buffered_write(slot, "blocked", 7);
    return 0;
}

int prompt_buffered_write(void*, int slot, void*, size_t) {
    comm_buffered_write(slot, "prompt", 6);
    return 0;
}

int prompt_hook_calls{0};
std::promise<void>* prompt_hook_called_ptr{nullptr};

int count_prompt_hook(void*, int, void*, size_t) {
    ++prompt_hook_calls;
    if (prompt_hook_called_ptr)
        prompt_hook_called_ptr->set_value();
    return 0;
}

int prompt_hook_attempts_add(void*, int slot, void*, size_t) {
    ++prompt_hook_calls;
    comm_add_message(slot, "prompt", 6);
    return 0;
}

int connect_websocket_with_output(void*, int slot, void*, size_t) {
    if (!comm_enable_websocket_for_slot(slot, nullptr))
        return -1;
    comm_buffered_write(slot, "connect", 7);
    return 0;
}

int transport_ready_with_output(void*, int slot, void*, size_t) {
    comm_buffered_write(slot, "ready", 5);
    return 0;
}

std::promise<void>* thread_pool_first_slot_entered_ptr{nullptr};
std::promise<void>* thread_pool_other_slots_done_ptr{nullptr};
std::shared_future<void>* thread_pool_first_slot_release_future_ptr{nullptr};
std::promise<void>* thread_pool_first_slot_finished_ptr{nullptr};
std::atomic<int> thread_pool_other_slots_completed{0};
int thread_pool_other_slots_expected{0};
int thread_pool_first_slot_count{0};
int thread_pool_blocked_slot{0};

std::promise<void>* comm_api_blocked_slot_entered_ptr{nullptr};
std::promise<void>* comm_api_blocked_slot_finished_ptr{nullptr};
std::promise<void>* comm_api_other_slots_done_ptr{nullptr};
std::shared_future<void>* comm_api_blocked_slot_release_ptr{nullptr};
std::atomic<int> comm_api_other_slots_completed{0};
int comm_api_other_slots_expected{0};
int comm_api_blocked_slot{-1};

std::promise<void>* queue_pressure_hot_slot_entered_ptr{nullptr};
std::shared_future<void>* queue_pressure_hot_slot_release_ptr{nullptr};
std::promise<void>* queue_pressure_other_slots_done_ptr{nullptr};
std::atomic<int> queue_pressure_hot_slot_count{0};
std::atomic<int> queue_pressure_other_slots_completed{0};
int queue_pressure_other_slots_expected{0};
int queue_pressure_hot_slot{-1};

std::atomic<int> enqueue_race_processed_count{0};

std::promise<void>* inbound_hook_entered_ptr{nullptr};
std::shared_future<void>* inbound_hook_release_ptr{nullptr};
std::promise<void>* inbound_hook_finished_ptr{nullptr};
std::atomic<int> inbound_hook_call_count{0};

std::promise<void>* await_work_started_ptr{nullptr};
std::shared_future<void>* await_work_release_ptr{nullptr};
std::promise<void>* await_resume_finished_ptr{nullptr};
std::promise<void>* await_chained_resume_finished_ptr{nullptr};
std::promise<void>* await_second_inbound_ptr{nullptr};
std::atomic<int> await_inbound_call_count{0};
std::atomic<int> await_work_message{ASYNC_CLOSURE_SCHEDULER_FAILED};
std::atomic<int> await_resume_message{ASYNC_CLOSURE_SCHEDULER_FAILED};
std::atomic<int> await_resume_hook_type{MAX_HOOK_TYPE};
std::atomic<int> await_chained_work_calls{0};
std::atomic<int> await_chained_resume_hook_type{MAX_HOOK_TYPE};
std::atomic<int> await_chained_request_result{0};
std::promise<void>* submit_completion_finished_ptr{nullptr};
std::atomic<int> submit_completion_hook_type{MAX_HOOK_TYPE};

void await_work(void*, int message) {
    await_work_message.store(message);
    if (await_work_started_ptr)
        await_work_started_ptr->set_value();
    if (await_work_release_ptr)
        await_work_release_ptr->wait();
}

void await_chained_work(void*, int) {
    ++await_chained_work_calls;
}

void await_chained_resume(void*, int) {
    await_chained_resume_hook_type.store(comm_current_hook_type());
    if (await_chained_resume_finished_ptr)
        await_chained_resume_finished_ptr->set_value();
}

void submit_completion_noop_work(void*, int) {
}

void submit_completion_records_hook_type(void*, int) {
    submit_completion_hook_type.store(comm_current_hook_type());
    if (submit_completion_finished_ptr)
        submit_completion_finished_ptr->set_value();
}

void await_resume(void*, int message) {
    await_resume_message.store(message);
    await_resume_hook_type.store(comm_current_hook_type());
    if (await_chained_resume_finished_ptr) {
        async_closure_t work{await_chained_work, nullptr, nullptr};
        async_closure_t resume{await_chained_resume, nullptr, nullptr};
        await_chained_request_result.store(mudmux_workers_await(&work, &resume) ? 1 : -1);
    }
    if (await_resume_finished_ptr)
        await_resume_finished_ptr->set_value();
}

int awaiting_inbound_hook(void* context, int, void* data, size_t len) {
    auto* test = static_cast<CommInboundTest*>(context);
    if (test)
        test->inbound_messages.emplace_back(static_cast<char*>(data), len);

    if (await_inbound_call_count.fetch_add(1) == 0) {
        async_closure_t work{await_work, nullptr, nullptr};
        async_closure_t resume{await_resume, nullptr, nullptr};
        return mudmux_workers_await(&work, &resume) ? 0 : -1;
    }

    if (await_second_inbound_ptr)
        await_second_inbound_ptr->set_value();
    return 0;
}

int thread_pool_stress_hook(void*, int slot, void* data, size_t len) {
    (void)data;
    (void)len;

    if (slot == thread_pool_blocked_slot) {
        const int count = ++thread_pool_first_slot_count;
        if (count == 1) {
            if (thread_pool_first_slot_entered_ptr)
                thread_pool_first_slot_entered_ptr->set_value();
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
    if (mudmux_comm_api_v1->get_flags(slot) == 0u)
        return -1;

    mudmux_comm_api_v1->set_echo(slot, false);
    mudmux_comm_api_v1->set_char_input(slot);
    mudmux_comm_api_v1->set_line_input(slot, true);
    mudmux_comm_api_v1->enable_prompt(slot, true);
    mudmux_comm_api_v1->buffered_write(slot, "ok", 2);

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

int queue_pressure_hook(void*, int slot, void*, size_t) {
    mudmux_comm_api_v1->buffered_write(slot, "x", 1);

    if (slot == queue_pressure_hot_slot) {
        const int count = queue_pressure_hot_slot_count.fetch_add(1) + 1;
        if (count == 1) {
            if (queue_pressure_hot_slot_entered_ptr)
                queue_pressure_hot_slot_entered_ptr->set_value();
            if (queue_pressure_hot_slot_release_ptr)
                queue_pressure_hot_slot_release_ptr->wait();
        }
        return 0;
    }

    mudmux_comm_api_v1->set_echo(slot, false);
    const int completed = queue_pressure_other_slots_completed.fetch_add(1) + 1;
    if (completed == queue_pressure_other_slots_expected && queue_pressure_other_slots_done_ptr)
        queue_pressure_other_slots_done_ptr->set_value();
    return 0;
}

int enqueue_race_comm_api_hook(void*, int slot, void*, size_t) {
    if (mudmux_comm_api_v1->get_flags(slot) == 0u)
        return -1;

    mudmux_comm_api_v1->set_char_input(slot);
    mudmux_comm_api_v1->set_line_input(slot, true);
    mudmux_comm_api_v1->buffered_write(slot, "r", 1);
    enqueue_race_processed_count.fetch_add(1);
    return 0;
}

int blocking_inbound_hook(void*, int, void*, size_t) {
    const int call_count = inbound_hook_call_count.fetch_add(1) + 1;
    if (call_count == 1) {
        if (inbound_hook_entered_ptr)
            inbound_hook_entered_ptr->set_value();
        if (inbound_hook_release_ptr)
            inbound_hook_release_ptr->wait();
        if (inbound_hook_finished_ptr)
            inbound_hook_finished_ptr->set_value();
    }
    return 0;
}

} // namespace

TEST_F(CommInboundTest, WriteMessageBuffersDirectlyOrRoutesThroughOutboundHook) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(0);
    ASSERT_NE(slot, -1);

    char direct[] = "direct";
    comm_add_message(slot, direct, sizeof(direct) - 1);
    comm_flush(runtime, slot);
    std::array<char, 16> output{};
    ASSERT_EQ(BIO_read(comm_abstract_get(slot)->wbio, output.data(), static_cast<int>(output.size())), 6);
    EXPECT_EQ(std::string(output.data(), 6), "direct");

    outbound_hook_slot = -1;
    outbound_hook_message.clear();
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_OUTBOUND, rewrite_outbound_message));
    char hooked[] = "hello";
    comm_add_message(slot, hooked, sizeof(hooked) - 1);
    EXPECT_EQ(outbound_hook_slot, slot);
    EXPECT_EQ(outbound_hook_message, "hello");
    EXPECT_EQ(std::string(hooked, sizeof(hooked) - 1), "hello");

    comm_flush(runtime, slot);
    ASSERT_EQ(BIO_read(comm_abstract_get(slot)->wbio, output.data(), static_cast<int>(output.size())), 5);
    EXPECT_EQ(std::string(output.data(), 5), "hello");
    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, FormattedMessageFormatsAndRoutesOutboundPayload) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(0);
    ASSERT_NE(slot, -1);

    comm_add_formatted_message(slot, "%s %d", "hello", 42);
    comm_flush(runtime, slot);

    std::array<char, 16> output{};
    ASSERT_EQ(BIO_read(comm_abstract_get(slot)->wbio, output.data(), static_cast<int>(output.size())), 8);
    EXPECT_EQ(std::string(output.data(), 8), "hello 42");

    outbound_hook_slot = -1;
    outbound_hook_message.clear();
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_OUTBOUND, rewrite_outbound_message));

    comm_add_formatted_message(slot, "[%s]", "ok");
    EXPECT_EQ(outbound_hook_slot, slot);
    EXPECT_EQ(outbound_hook_message, "[ok]");

    comm_flush(runtime, slot);
    ASSERT_EQ(BIO_read(comm_abstract_get(slot)->wbio, output.data(), static_cast<int>(output.size())), 4);
    EXPECT_EQ(std::string(output.data(), 4), "[ok]");
    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, OutboundHookCannotRecursivelyAddMessage) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);

    outbound_hook_calls = 0;
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_OUTBOUND, outbound_attempts_recursive_add));
    comm_add_message(slot, "message", 7);

    EXPECT_EQ(outbound_hook_calls, 1);
    comm_flush(runtime, slot);
    EXPECT_EQ(BIO_ctrl_pending(comm_abstract_get(slot)->wbio), 0u);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, OutboundHookCanOnlyWriteItsCurrentSlot) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int source_slot = add_memory_comm(C_LINE_INPUT);
    const int other_slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(source_slot, -1);
    ASSERT_NE(other_slot, -1);

    outbound_hook_calls = 0;
    outbound_cross_slot = other_slot;
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_OUTBOUND, outbound_attempts_cross_slot_write));
    comm_add_message(source_slot, "message", 7);

    EXPECT_EQ(outbound_hook_calls, 1);
    comm_flush(runtime, other_slot);
    EXPECT_EQ(BIO_ctrl_pending(comm_abstract_get(other_slot)->wbio), 0u);
    outbound_cross_slot = -1;

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, PromptHookCannotAddMessage) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);

    prompt_hook_calls = 0;
    ASSERT_TRUE(mudmux_register_hook(HOOK_PROMPT, prompt_hook_attempts_add));
    comm_enable_prompt(slot, true);
    comm_invoke_prompt(runtime);

    EXPECT_EQ(prompt_hook_calls, 1);
    comm_flush(runtime, slot);
    EXPECT_EQ(BIO_ctrl_pending(comm_abstract_get(slot)->wbio), 0u);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, BufferedWriteIsRestrictedToOutboundAndPromptHooksWhenOutboundHookRegistered) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    outbound_hook_slot = -1;
    outbound_hook_message.clear();
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_OUTBOUND, rewrite_outbound_message));
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, inbound_attempt_buffered_write));

    ASSERT_TRUE(comm_refill_inbound_buffers(comm, "hello\n", 6));
    EXPECT_EQ(comm_process_input(runtime, comm, 1), COMM_PROCESS_OK);
    comm_flush(runtime, slot);

    EXPECT_EQ(outbound_hook_slot, -1);
    EXPECT_EQ(BIO_ctrl_pending(comm_abstract_get(slot)->wbio), 0);

    ASSERT_TRUE(mudmux_register_hook(HOOK_PROMPT, prompt_buffered_write));
    comm_enable_prompt(slot, true);
    comm_invoke_prompt(runtime);
    comm_flush(runtime, slot);

    std::array<char, 16> output{};
    ASSERT_EQ(BIO_read(comm_abstract_get(slot)->wbio, output.data(), static_cast<int>(output.size())), 6);
    EXPECT_EQ(std::string(output.data(), 6), "prompt");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, PromptFiresOnlyOnceWhenSlotHasNoPendingInputOrOutput) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    prompt_hook_calls = 0;
    ASSERT_TRUE(mudmux_register_hook(HOOK_PROMPT, count_prompt_hook));
    comm_enable_prompt(slot, true);

    // A partial line remains buffered but is not marked C_DEFERRED_INBOUND.
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, "partial", 7));
    comm_invoke_prompt(runtime);
    EXPECT_EQ(prompt_hook_calls, 0);
    EXPECT_EQ(comm_get_flags(slot) & C_INVOKED_PROMPT, 0u);

    comm_free_inbound_buffers(comm);
    comm_invoke_prompt(runtime);
    EXPECT_EQ(prompt_hook_calls, 1);
    EXPECT_NE(comm->flags & C_INVOKED_PROMPT, 0u);

    // C_INVOKED_PROMPT makes the idle notification one-shot.
    comm_invoke_prompt(runtime);
    EXPECT_EQ(prompt_hook_calls, 1);

    comm->flags &= ~C_INVOKED_PROMPT;
    comm->flags |= C_DEFERRED_INBOUND;
    comm_invoke_prompt(runtime);
    EXPECT_EQ(prompt_hook_calls, 1);
    EXPECT_EQ(comm->flags & C_INVOKED_PROMPT, 0u);

    comm->flags &= ~C_DEFERRED_INBOUND;
    comm_buffered_write(slot, "output", 6);
    comm_invoke_prompt(runtime);
    EXPECT_EQ(prompt_hook_calls, 1);
    EXPECT_EQ(comm->flags & C_INVOKED_PROMPT, 0u);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, BufferedOutputRearmsPromptOnlyForCharInput) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    // Keep the tested memory slot away from COMM_SLOT_CONSOLE so switching
    // input mode cannot alter the test process terminal.
    ASSERT_NE(add_memory_comm(C_LINE_INPUT), -1);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    ASSERT_NE(slot, COMM_SLOT_CONSOLE);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    prompt_hook_calls = 0;
    ASSERT_TRUE(mudmux_register_hook(HOOK_PROMPT, count_prompt_hook));
    comm_enable_prompt(slot, true);
    comm_invoke_prompt(runtime);
    ASSERT_EQ(prompt_hook_calls, 1);
    ASSERT_NE(comm->flags & C_INVOKED_PROMPT, 0u);

    // Normal command input keeps the current prompt gate across broadcasts.
    comm_buffered_write(slot, "line output", 11);
    EXPECT_NE(comm->flags & C_INVOKED_PROMPT, 0u);
    comm_flush(runtime, slot);
    // Memory BIOs do not expose a socket fd, so model the final writable
    // completion that clears this transport bookkeeping flag.
    comm->flags &= ~C_BUFFERED_WRITE;

    ASSERT_TRUE(comm_set_char_input(slot));
    comm_buffered_write(slot, "choice update", 13);
    EXPECT_EQ(comm->flags & C_INVOKED_PROMPT, 0u);
    comm_flush(runtime, slot);
    comm->flags &= ~C_BUFFERED_WRITE;
    comm_invoke_prompt(runtime);
    EXPECT_EQ(prompt_hook_calls, 2);
    EXPECT_NE(comm->flags & C_INVOKED_PROMPT, 0u);

    // Output emitted by the prompt callback itself must not schedule another
    // prompt after it drains.
    {
        comm_hook_type_scope_t prompt_scope(HOOK_PROMPT);
        comm_current_slot_scope_t current_slot_scope(slot);
        comm_buffered_write(slot, "> ", 2);
    }
    EXPECT_NE(comm->flags & C_INVOKED_PROMPT, 0u);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, PromptWaitsForAnInFlightSlotHook) {
    mudmux_deinit();
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 2}}}"));

    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
    }

    std::promise<void> inbound_entered_promise;
    std::future<void> inbound_entered_future = inbound_entered_promise.get_future();
    std::promise<void> inbound_release_promise;
    std::shared_future<void> inbound_release_future = inbound_release_promise.get_future().share();
    std::promise<void> inbound_finished_promise;
    std::future<void> inbound_finished_future = inbound_finished_promise.get_future();
    std::promise<void> prompt_called_promise;
    std::future<void> prompt_called_future = prompt_called_promise.get_future();

    thread_pool_first_slot_entered_ptr = &inbound_entered_promise;
    thread_pool_first_slot_release_future_ptr = &inbound_release_future;
    thread_pool_first_slot_finished_ptr = &inbound_finished_promise;
    thread_pool_first_slot_count = 0;
    thread_pool_blocked_slot = slot;
    prompt_hook_calls = 0;
    prompt_hook_called_ptr = &prompt_called_promise;

    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, thread_pool_stress_hook));
    ASSERT_TRUE(mudmux_register_hook(HOOK_PROMPT, count_prompt_hook));
    comm_enable_prompt(slot, true);

    const char input[] = "busy";
    ASSERT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, slot, input, sizeof(input) - 1),
              MUDMUX_DISPATCH_OK);
    ASSERT_EQ(inbound_entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    comm_invoke_prompt(runtime);
    EXPECT_EQ(prompt_hook_calls, 0);
    EXPECT_EQ(comm_get_flags(slot) & C_INVOKED_PROMPT, 0u);

    inbound_release_promise.set_value();
    ASSERT_EQ(inbound_finished_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (mudmux_execution_slot_busy(slot) && std::chrono::steady_clock::now() < idle_deadline)
        std::this_thread::yield();
    ASSERT_FALSE(mudmux_execution_slot_busy(slot));

    comm_invoke_prompt(runtime);
    EXPECT_EQ(prompt_called_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(prompt_hook_calls, 1);
    EXPECT_NE(comm_get_flags(slot) & C_INVOKED_PROMPT, 0u);

    prompt_hook_called_ptr = nullptr;
    thread_pool_first_slot_entered_ptr = nullptr;
    thread_pool_first_slot_release_future_ptr = nullptr;
    thread_pool_first_slot_finished_ptr = nullptr;
    thread_pool_blocked_slot = 0;
    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, BufferedWriteNormalizesNewlinesWhenTelnetEnabled) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_ENABLE_TELNET);
    ASSERT_NE(slot, -1);

    const char payload[] = "line1\nline2\r\nline3\rline4";
    comm_buffered_write(slot, payload, sizeof(payload) - 1);
    comm_flush(runtime, slot);

    std::array<char, 64> output{};
    const int output_len = BIO_read(comm_abstract_get(slot)->wbio, output.data(), static_cast<int>(output.size()));
    ASSERT_GT(output_len, 0);
    EXPECT_EQ(
        std::string(output.data(), static_cast<size_t>(output_len)),
        "line1\r\nline2\r\nline3\rline4");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, BufferedWritePreservesNewlinesWhenTelnetDisabled) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(0);
    ASSERT_NE(slot, -1);

    const char payload[] = "line1\nline2\r\nline3\rline4";
    comm_buffered_write(slot, payload, sizeof(payload) - 1);
    comm_flush(runtime, slot);

    std::array<char, 64> output{};
    const int output_len = BIO_read(comm_abstract_get(slot)->wbio, output.data(), static_cast<int>(output.size()));
    ASSERT_GT(output_len, 0);
    EXPECT_EQ(
        std::string(output.data(), static_cast<size_t>(output_len)),
        "line1\nline2\r\nline3\rline4");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, TelnetSubnegotiationOutboundPayloadRemainsByteExact) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_ENABLE_TELNET);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    const char payload[] = "A\nB\rC\r\nD";
    comm_telnet_send_subnegotiation(comm, TELOPT_TTYPE, payload, sizeof(payload) - 1);
    comm_flush(runtime, slot);

    std::array<char, 64> output{};
    const int output_len = BIO_read(comm->wbio, output.data(), static_cast<int>(output.size()));
    ASSERT_GT(output_len, 0);

    std::string expected;
    expected.push_back(static_cast<char>(255));
    expected.push_back(static_cast<char>(250));
    expected.push_back(static_cast<char>(TELOPT_TTYPE));
    expected.append(payload, sizeof(payload) - 1);
    expected.push_back(static_cast<char>(255));
    expected.push_back(static_cast<char>(240));

    EXPECT_EQ(std::string(output.data(), static_cast<size_t>(output_len)), expected);

    async_runtime_deinit(runtime);
}

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

TEST_F(CommInboundTest, CurrentSlotIsAvailableOnlyToSlotScopedHooks) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_record_current_slot));
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, "message\n", 8));
    EXPECT_EQ(comm_process_input(runtime, comm, 1), COMM_PROCESS_OK);
    EXPECT_EQ(observed_current_slot, slot);
    EXPECT_EQ(comm_current_slot(), -1);

    observed_current_slot = -2;
    ASSERT_TRUE(mudmux_register_hook(HOOK_CONNECT, CommInboundTest::hook_record_current_slot));
    EXPECT_EQ(comm_invoke_connect(runtime, slot, slot), MUDMUX_DISPATCH_OK);
    EXPECT_EQ(observed_current_slot, slot);

    observed_current_slot = -2;
    ASSERT_TRUE(mudmux_register_hook(HOOK_PROMPT, CommInboundTest::hook_record_current_slot));
    comm_enable_prompt(slot, true);
    comm_invoke_prompt(runtime);
    EXPECT_EQ(observed_current_slot, slot);

    observed_current_slot = -2;
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_OUTBOUND, CommInboundTest::hook_record_current_slot));
    const char output[] = "message";
    comm_add_message(slot, output, sizeof(output) - 1);
    EXPECT_EQ(observed_current_slot, slot);
    EXPECT_EQ(comm_current_slot(), -1);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, TransportReadyFiresOnceAfterConnectBeforeInbound) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    observed_current_slot = -2;
    ASSERT_TRUE(mudmux_register_hook(HOOK_TRANSPORT_READY, CommInboundTest::hook_record_current_slot));
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound));
    EXPECT_EQ(comm_invoke_connect(runtime, slot, slot), MUDMUX_DISPATCH_OK);
    EXPECT_EQ(observed_current_slot, slot);
    EXPECT_NE(comm_get_flags(slot) & C_TRANSPORT_READY, 0u);

    observed_current_slot = -2;
    inbound_messages.clear();
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, "message\n", 8));
    EXPECT_EQ(comm_process_input(runtime, comm, 1), COMM_PROCESS_OK);
    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "message");
    EXPECT_EQ(observed_current_slot, -2);

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
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);

    ASSERT_EQ(inbound_messages.size(), 2u);
    EXPECT_EQ(inbound_messages[0], "hello");
    EXPECT_EQ(inbound_messages[1], "world");

    ASSERT_TRUE(comm_refill_inbound_buffers(comm, "\n", 1));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), 0);

    ASSERT_EQ(inbound_messages.size(), 3u);
    EXPECT_EQ(inbound_messages[2], "partial");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ProcessLineInputModeDispatchesCarriageReturnTerminatedLines) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    const char* data = "alpha\rbeta\r";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, data, strlen(data)));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);

    ASSERT_EQ(inbound_messages.size(), 2u);
    EXPECT_EQ(inbound_messages[0], "alpha");
    EXPECT_EQ(inbound_messages[1], "beta");

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
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound_and_rearm_char);

    const char* data = "ab";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, data, strlen(data)));
    EXPECT_EQ(comm_process_input(runtime, comm, 1), COMM_PROCESS_DEFERRED);
    EXPECT_EQ(comm_process_input(runtime, comm, 1), COMM_PROCESS_DEFERRED);

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
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound_and_rearm_char);

    const char* data = "\x1B[Ax";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, data, strlen(data)));

    EXPECT_EQ(comm_process_input(runtime, comm, 1), COMM_PROCESS_DEFERRED);
    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "\x1B[A");

    EXPECT_EQ(comm_process_input(runtime, comm, 1), COMM_PROCESS_DEFERRED);
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
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    EXPECT_TRUE(inbound_messages.empty());

    const char* complete = "31~";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, complete, strlen(complete)));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_DEFERRED);

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

TEST_F(CommInboundTest, ConnectHookCanSelectTelnetOnly) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);
    inbound_messages.clear();
    connect_transport = ConnectTransport::Telnet;
    mudmux_register_hook(HOOK_CONNECT, CommInboundTest::hook_connect_select_transport);
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    ASSERT_EQ(comm_invoke_connect(runtime, slot, slot), 0);
    EXPECT_NE(comm_get_flags(slot) & C_ENABLE_TELNET, 0u);
    EXPECT_EQ(comm_get_flags(slot) & C_ENABLE_WEBSOCKET, 0u);

    const char payload[] = "hi\n";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, payload, sizeof(payload) - 1));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "hi");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ConnectHookCanSelectWebSocketOnly) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);
    inbound_messages.clear();
    connect_transport = ConnectTransport::WebSocket;
    mudmux_register_hook(HOOK_CONNECT, CommInboundTest::hook_connect_select_transport);
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    ASSERT_EQ(comm_invoke_connect(runtime, slot, slot), 0);
    const std::string request =
        "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    std::string packet = request;
    packet.append("\x82\x83\x01\x02\x03\x04", 6);
    packet.push_back(static_cast<char>('h' ^ 0x01));
    packet.push_back(static_cast<char>('i' ^ 0x02));
    packet.push_back(static_cast<char>('\n' ^ 0x03));
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, packet.data(), packet.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    EXPECT_EQ(C_WEBSOCKET_STATE(comm_get_flags(slot)), WS_READY);
    EXPECT_EQ(comm_get_flags(slot) & C_ENABLE_TELNET, 0u);
    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "hi");

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, WebSocketReadyOutputAppendsAfterConnectBarrier) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    ASSERT_TRUE(mudmux_register_hook(HOOK_CONNECT, connect_websocket_with_output));
    ASSERT_TRUE(mudmux_register_hook(HOOK_TRANSPORT_READY, transport_ready_with_output));
    ASSERT_EQ(comm_invoke_connect(runtime, slot, slot), MUDMUX_DISPATCH_OK);

    const std::string request =
        "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, request.data(), request.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    comm_flush(runtime, slot);

    std::array<char, 512> response_buf{};
    const int response_len = BIO_read(comm->wbio, response_buf.data(), static_cast<int>(response_buf.size()));
    ASSERT_GT(response_len, 0);
    const std::string response(response_buf.data(), static_cast<size_t>(response_len));
    std::string connect_frame("\x82\x07", 2);
    connect_frame += "connect";
    std::string ready_frame("\x82\x05", 2);
    ready_frame += "ready";
    const size_t connect_pos = response.find(connect_frame);
    const size_t ready_pos = response.find(ready_frame);
    EXPECT_NE(connect_pos, std::string::npos);
    EXPECT_NE(ready_pos, std::string::npos);
    EXPECT_LT(connect_pos, ready_pos);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, TransportEnableApisRejectCallsOutsideConnectHook) {
    const int slot = add_memory_comm(0);
    ASSERT_NE(slot, -1);

    EXPECT_FALSE(mudmux_comm_api_v1->enable_websocket(nullptr));
    mudmux_comm_api_v1->enable_telnet();
    mudmux_comm_api_v1->enable_tls();

    const uint32_t flags = comm_get_flags(slot);
    EXPECT_EQ(flags & (C_ENABLE_TELNET | C_ENABLE_WEBSOCKET | C_TLS_ESTABLISHED), 0u);
}

TEST_F(CommInboundTest, WebSocketUpgradeDispatchesBinaryUtf8StreamAndFramesOutboundBinary) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    ASSERT_TRUE(comm_enable_websocket_for_slot(slot, nullptr));

    std::string request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    std::string packet = request;
    packet.push_back(static_cast<char>(0x82));
    packet.push_back(static_cast<char>(0x83));
    packet.push_back(static_cast<char>(0x01));
    packet.push_back(static_cast<char>(0x02));
    packet.push_back(static_cast<char>(0x03));
    packet.push_back(static_cast<char>(0x04));
    packet.push_back(static_cast<char>('h' ^ 0x01));
    packet.push_back(static_cast<char>('i' ^ 0x02));
    packet.push_back(static_cast<char>('\n' ^ 0x03));

    ASSERT_TRUE(comm_refill_inbound_buffers(comm, packet.data(), packet.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);

    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "hi");

    comm_buffered_write(slot, "ok", 2);

    comm_flush(runtime, slot);
    std::array<char, 512> response_buf{};
    const int response_len = BIO_read(comm->wbio, response_buf.data(), static_cast<int>(response_buf.size()));
    ASSERT_GT(response_len, 0);
    std::string response(response_buf.data(), static_cast<size_t>(response_len));
    EXPECT_EQ(response.rfind("HTTP/1.1 101 Switching Protocols", 0), 0u);
    EXPECT_NE(response.find("101 Switching Protocols"), std::string::npos);
    EXPECT_NE(response.find("Sec-WebSocket-Accept:"), std::string::npos);
    EXPECT_NE(response.find(std::string("\x82\x02ok", 4)), std::string::npos);
    EXPECT_EQ(C_WEBSOCKET_STATE(comm_get_flags(slot)), WS_READY);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ServerInitiatedWebSocketCloseWaitsForPeerReplyBeforeRemovingSlot) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(WS_READY);
    ASSERT_NE(slot, -1);

    // Application output queued by a disconnect hook must drain before the
    // Close control frame, which must be the final outgoing WebSocket frame.
    comm_buffered_write(slot, "bye", 3);
    EXPECT_FALSE(comm_close(runtime, slot));

    comm_flush(runtime, slot);
    std::array<char, 16> outbound{};
    const int outbound_len = BIO_read(comm_abstract_get(slot)->wbio, outbound.data(), static_cast<int>(outbound.size()));
    ASSERT_EQ(outbound_len, 5);
    EXPECT_EQ(std::string(outbound.data(), static_cast<size_t>(outbound_len)), std::string("\x82\x03" "bye", 5));

    // Memory BIOs have no writable-event bookkeeping; the frame above has
    // drained, so model the equivalent socket state before processing reply.
    comm_abstract_get(slot)->flags &= ~C_BUFFERED_WRITE;
    EXPECT_FALSE(comm_close(runtime, slot));

    comm_flush(runtime, slot);
    const int close_len = BIO_read(comm_abstract_get(slot)->wbio, outbound.data(), static_cast<int>(outbound.size()));
    ASSERT_EQ(close_len, 4);
    EXPECT_EQ(std::string(outbound.data(), static_cast<size_t>(close_len)), std::string("\x88\x02\x03\xe8", 4));
    EXPECT_EQ(C_WEBSOCKET_STATE(comm_get_flags(slot)), WS_CLOSE_SENT);
    comm_abstract_get(slot)->flags &= ~C_BUFFERED_WRITE;

    // Late application output from a queued relaxed hook must not follow Close.
    comm_buffered_write(slot, "late", 4);
    EXPECT_EQ(BIO_ctrl_pending(comm_abstract_get(slot)->wbio), 0);

    const char peer_close[] = {
        static_cast<char>(0x88), static_cast<char>(0x82),
        0x01, 0x02, 0x03, 0x04,
        static_cast<char>(0x03 ^ 0x01), static_cast<char>(0xe8 ^ 0x02)
    };
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, peer_close, sizeof(peer_close)));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_CLOSED);
    EXPECT_NE(comm_abstract_get(slot), nullptr);
    EXPECT_TRUE(comm_close(runtime, slot)); // matches the event-loop close path
    EXPECT_EQ(comm_abstract_get(slot), nullptr);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, WebSocketUpgradeRejectsInvalidHandshake) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(0);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);

    ASSERT_TRUE(comm_enable_websocket_for_slot(slot, nullptr));

    const char* invalid_request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    ASSERT_TRUE(comm_refill_inbound_buffers(comm, invalid_request, strlen(invalid_request)));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    EXPECT_TRUE(inbound_messages.empty());

    comm_flush(runtime, slot);
    std::array<char, 512> response_buf{};
    const int response_len = BIO_read(comm->wbio, response_buf.data(), static_cast<int>(response_buf.size()));
    ASSERT_GT(response_len, 0);
    std::string response(response_buf.data(), static_cast<size_t>(response_len));
    EXPECT_NE(response.find("400 Bad Request"), std::string::npos);
    EXPECT_TRUE((comm_get_flags(slot) & C_CLOSING) != 0);
    EXPECT_EQ(comm_get_flags(slot) & C_ENABLE_WEBSOCKET, 0u);

    // The rejected request must not remain pending and trigger another 400
    // while the first response is being closed.
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, TelnetEnableRejectedWhenWebSocketUpgradePending) {
    // TELNET cannot be enabled manually while WebSocket mode is active but upgrade not yet done.
    const int slot = add_memory_comm(0);
    ASSERT_NE(slot, -1);

    ASSERT_TRUE(comm_enable_websocket_for_slot(slot, nullptr));
    comm_enable_telnet_for_slot(slot);

    const uint32_t flags = comm_get_flags(slot);
    EXPECT_TRUE((flags & C_ENABLE_WEBSOCKET) != 0);
    EXPECT_TRUE((flags & C_ENABLE_TELNET) == 0);
}

TEST_F(CommInboundTest, WebSocketClientSubprotocolIsIgnoredWithoutServerPreference) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(0);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);

    ASSERT_TRUE(comm_enable_websocket_for_slot(slot, nullptr));

    const std::string request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: telnet.ietf.org\r\n"
        "\r\n";

    ASSERT_TRUE(comm_refill_inbound_buffers(comm, request.data(), request.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);

    const uint32_t flags = comm_get_flags(slot);
    EXPECT_EQ(C_WEBSOCKET_STATE(flags), WS_READY);
    EXPECT_TRUE((flags & C_ENABLE_TELNET) == 0);

    comm_flush(runtime, slot);
    std::array<char, 512> response_buf{};
    const int response_len = BIO_read(comm->wbio, response_buf.data(), static_cast<int>(response_buf.size()));
    ASSERT_GT(response_len, 0);
    const std::string response(response_buf.data(), static_cast<size_t>(response_len));
    EXPECT_NE(response.find("101 Switching Protocols"), std::string::npos);
    EXPECT_EQ(response.find("Sec-WebSocket-Protocol:"), std::string::npos);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, WebSocketTelnetSubprotocolHonorsInputModesAcrossLinemodeNegotiation) {
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);

    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    comm_abstract_ptr comm(slot, comm_slots_mtx);
    ASSERT_TRUE(comm);
    inbound_messages.clear();
    mudmux_register_hook(HOOK_MESSAGE_INBOUND, CommInboundTest::hook_message_inbound);
    connect_transport = ConnectTransport::WebSocketTelnet;
    mudmux_register_hook(HOOK_CONNECT, CommInboundTest::hook_connect_select_transport);
    ASSERT_EQ(comm_invoke_connect(runtime, slot, slot), 0);

    std::string packet =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: telnet.ietf.org, telnet.mudstandards.org\r\n"
        "\r\n";
    const std::array<unsigned char, 4> mask{{1, 2, 3, 4}};
    const auto masked_binary_frame = [&mask](std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x82));
        frame.push_back(static_cast<char>(0x80 | payload.size()));
        for (unsigned char byte : mask)
            frame.push_back(static_cast<char>(byte));
        for (size_t i = 0; i < payload.size(); ++i)
            frame.push_back(static_cast<char>(payload[i] ^ mask[i % mask.size()]));
        return frame;
    };

    ASSERT_TRUE(comm_refill_inbound_buffers(comm, packet.data(), packet.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    EXPECT_TRUE(inbound_messages.empty());

    const std::string linemode = masked_binary_frame(std::string("\xff\xfb\x22", 3));
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, linemode.data(), linemode.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    EXPECT_TRUE(inbound_messages.empty());

    const std::string key_g = masked_binary_frame("g");
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, key_g.data(), key_g.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    EXPECT_TRUE(inbound_messages.empty());

    const std::string key_o = masked_binary_frame("o");
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, key_o.data(), key_o.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    EXPECT_TRUE(inbound_messages.empty());

    const std::string enter = masked_binary_frame("\r\n");
    ASSERT_TRUE(comm_refill_inbound_buffers(comm, enter.data(), enter.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_OK);
    ASSERT_EQ(inbound_messages.size(), 1u);
    EXPECT_EQ(inbound_messages[0], "go");

    ASSERT_TRUE(comm_set_char_input(slot));

    // This arrives after the WebSocket/Telnet upgrade. Its WebSocket mask
    // begins with IAC (0xff), which must not be interpreted as raw Telnet
    // input before WebSocket unmasking. Character mode dispatches it without
    // requiring Telnet LINEMODE negotiation.
    const std::array<unsigned char, 1> second_payload{{'h'}};
    const std::array<unsigned char, 4> second_mask{{255, 0, 0, 0}};
    std::string second_frame;
    second_frame.push_back(static_cast<char>(0x82));
    second_frame.push_back(static_cast<char>(0x80 | second_payload.size()));
    for (unsigned char byte : second_mask)
        second_frame.push_back(static_cast<char>(byte));
    for (size_t i = 0; i < second_payload.size(); ++i)
        second_frame.push_back(static_cast<char>(second_payload[i] ^ second_mask[i % second_mask.size()]));

    ASSERT_TRUE(comm_refill_inbound_buffers(comm, second_frame.data(), second_frame.size()));
    EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_DEFERRED);
    ASSERT_EQ(inbound_messages.size(), 2u);
    EXPECT_EQ(inbound_messages[1], "h");

    comm_flush(runtime, slot);
    std::array<char, 512> response_buf{};
    const int response_len = BIO_read(comm->wbio, response_buf.data(), static_cast<int>(response_buf.size()));
    ASSERT_GT(response_len, 0);
    const std::string response(response_buf.data(), static_cast<size_t>(response_len));
    EXPECT_NE(response.find("Sec-WebSocket-Protocol: telnet.ietf.org"), std::string::npos);
    EXPECT_NE(response.find(std::string("\x82\x03\xff\xfb\x03", 5)), std::string::npos);

    async_runtime_deinit(runtime);
}

TEST_F(CommInboundTest, ThreadPoolKeepsPerSlotOrderWhileOtherSlotsAdvance) {
    mudmux_deinit();
    ASSERT_TRUE(mudmux_init(
        "{\"transport\": {\"thread_pool\": {\"size\": 2, \"backlog_capacity\": 1}}}"));

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

    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, thread_pool_stress_hook));

    const char slot0_first[] = "slot0-first";
    const char slot0_second[] = "slot0-second";
    ASSERT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, 0, slot0_first, strlen(slot0_first)), MUDMUX_DISPATCH_OK);
    ASSERT_EQ(first_slot_entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, 0, slot0_second, strlen(slot0_second)), MUDMUX_DISPATCH_QUEUE_FULL);
    EXPECT_EQ(mudmux_dispatch_hook_after(HOOK_PROMPT, this, 0, nullptr, 0), MUDMUX_DISPATCH_OK);
    EXPECT_EQ(mudmux_dispatch_hook_after(HOOK_PROMPT, this, 0, nullptr, 0), MUDMUX_DISPATCH_QUEUE_FULL);

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
    mudmux_deinit();
}

TEST_F(CommInboundTest, WorkersAwaitDefersInboundUntilResume) {
    mudmux_deinit();
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 2}}}"));

    std::promise<void> work_started_promise;
    std::future<void> work_started_future = work_started_promise.get_future();
    std::promise<void> work_release_promise;
    std::shared_future<void> work_release_future = work_release_promise.get_future().share();
    std::promise<void> resume_finished_promise;
    std::future<void> resume_finished_future = resume_finished_promise.get_future();
    std::promise<void> chained_resume_finished_promise;
    std::future<void> chained_resume_finished_future = chained_resume_finished_promise.get_future();
    std::promise<void> second_inbound_promise;
    std::future<void> second_inbound_future = second_inbound_promise.get_future();

    await_work_started_ptr = &work_started_promise;
    await_work_release_ptr = &work_release_future;
    await_resume_finished_ptr = &resume_finished_promise;
    await_chained_resume_finished_ptr = &chained_resume_finished_promise;
    await_second_inbound_ptr = &second_inbound_promise;
    await_inbound_call_count.store(0);
    await_work_message.store(ASYNC_CLOSURE_SCHEDULER_FAILED);
    await_resume_message.store(ASYNC_CLOSURE_SCHEDULER_FAILED);
    await_resume_hook_type.store(MAX_HOOK_TYPE);
    await_chained_work_calls.store(0);
    await_chained_resume_hook_type.store(MAX_HOOK_TYPE);
    await_chained_request_result.store(0);
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, awaiting_inbound_hook));

    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
        ASSERT_TRUE(comm_refill_inbound_buffers(comm, "first\nsecond\n", 13));
        EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_DEFERRED);
    }

    ASSERT_EQ(work_started_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(await_work_message.load(), ASYNC_CLOSURE_SCHEDULER_OK);
    {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
        EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_DEFERRED);
    }
    EXPECT_EQ(await_inbound_call_count.load(), 1);

    work_release_promise.set_value();
    ASSERT_EQ(resume_finished_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(await_resume_message.load(), ASYNC_CLOSURE_SCHEDULER_OK);
    EXPECT_EQ(await_resume_hook_type.load(), HOOK_RESUME);
    EXPECT_EQ(await_chained_request_result.load(), 1);
    ASSERT_EQ(chained_resume_finished_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(await_chained_work_calls.load(), 1);
    EXPECT_EQ(await_chained_resume_hook_type.load(), HOOK_RESUME);

    comm_resume_deferred_input(runtime);
    ASSERT_EQ(second_inbound_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(inbound_messages.size(), 2u);
    EXPECT_EQ(inbound_messages[0], "first");
    EXPECT_EQ(inbound_messages[1], "second");

    await_work_started_ptr = nullptr;
    await_work_release_ptr = nullptr;
    await_resume_finished_ptr = nullptr;
    await_chained_resume_finished_ptr = nullptr;
    await_second_inbound_ptr = nullptr;
    async_runtime_deinit(runtime);
    mudmux_deinit();
}

TEST_F(CommInboundTest, WorkersSubmitCompletionHasCompletionHookContext) {
    mudmux_deinit();
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 2}}}"));

    std::promise<void> completion_finished_promise;
    std::future<void> completion_finished_future = completion_finished_promise.get_future();
    submit_completion_finished_ptr = &completion_finished_promise;
    submit_completion_hook_type.store(MAX_HOOK_TYPE);

    async_closure_t work{submit_completion_noop_work, nullptr, nullptr};
    async_closure_t completion{submit_completion_records_hook_type, nullptr, nullptr};
    ASSERT_TRUE(mudmux_workers_submit(&work, &completion));
    ASSERT_EQ(completion_finished_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(submit_completion_hook_type.load(), HOOK_COMPLETION);

    submit_completion_finished_ptr = nullptr;
    mudmux_deinit();
}

TEST_F(CommInboundTest, RelaxedModeDefersSameSlotParsingUntilInboundHookReturns) {
    mudmux_deinit();
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 2}}}"));

    std::promise<void> entered_promise;
    std::future<void> entered_future = entered_promise.get_future();
    std::promise<void> release_promise;
    std::shared_future<void> release_future = release_promise.get_future().share();
    std::promise<void> finished_promise;
    std::future<void> finished_future = finished_promise.get_future();
    inbound_hook_entered_ptr = &entered_promise;
    inbound_hook_release_ptr = &release_future;
    inbound_hook_finished_ptr = &finished_promise;
    inbound_hook_call_count.store(0);

    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, blocking_inbound_hook));
    async_runtime_t* runtime = async_runtime_init(this);
    ASSERT_NE(runtime, nullptr);
    const int slot = add_memory_comm(C_LINE_INPUT);
    ASSERT_NE(slot, -1);
    {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
        ASSERT_TRUE(comm_refill_inbound_buffers(comm, "first\nsecond\n", 13));
        EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_DEFERRED);
    }
    ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    {
        comm_abstract_ptr comm(slot, comm_slots_mtx);
        ASSERT_TRUE(comm);
        EXPECT_EQ(comm_process_input(runtime, comm, -1), COMM_PROCESS_DEFERRED);
    }
    EXPECT_EQ(inbound_hook_call_count.load(), 1);

    release_promise.set_value();
    ASSERT_EQ(finished_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    for (int i = 0; i < 100 && inbound_hook_call_count.load() != 2; ++i) {
        comm_resume_deferred_input(runtime);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(inbound_hook_call_count.load(), 2);

    inbound_hook_entered_ptr = nullptr;
    inbound_hook_release_ptr = nullptr;
    inbound_hook_finished_ptr = nullptr;
    async_runtime_deinit(runtime);
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

    mudmux_deinit();
}

TEST_F(CommInboundTest, RelaxedModeQueuePressureOnOneSlotDoesNotBlockOtherSlots) {
    mudmux_deinit();
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 4}}}"));

    std::vector<int> slots;
    slots.reserve(6);
    for (int index = 0; index < 6; ++index) {
        const int slot = add_memory_comm(C_LINE_INPUT);
        ASSERT_NE(slot, -1);
        slots.push_back(slot);
    }

    queue_pressure_hot_slot = slots.front();
    queue_pressure_hot_slot_count.store(0);
    queue_pressure_other_slots_completed.store(0);
    queue_pressure_other_slots_expected = static_cast<int>(slots.size()) - 1;

    std::promise<void> hot_slot_entered_promise;
    std::future<void> hot_slot_entered_future = hot_slot_entered_promise.get_future();
    std::promise<void> hot_slot_release_promise;
    std::shared_future<void> hot_slot_release_future = hot_slot_release_promise.get_future().share();
    std::promise<void> other_slots_done_promise;
    std::future<void> other_slots_done_future = other_slots_done_promise.get_future();

    queue_pressure_hot_slot_entered_ptr = &hot_slot_entered_promise;
    queue_pressure_hot_slot_release_ptr = &hot_slot_release_future;
    queue_pressure_other_slots_done_ptr = &other_slots_done_promise;

    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, queue_pressure_hook));

    const char payload[] = "phase6-queue-pressure";
    ASSERT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, queue_pressure_hot_slot, payload, strlen(payload)), MUDMUX_DISPATCH_OK);
    ASSERT_EQ(hot_slot_entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    std::atomic<int> queue_full_count{0};
    std::atomic<int> enqueue_error_count{0};
    std::vector<std::thread> producers;
    producers.reserve(4);
    for (int producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&, producer]() {
            for (int index = 0; index < 32; ++index) {
                const std::string msg = "hot-" + std::to_string(producer) + "-" + std::to_string(index);
                const mudmux_dispatch_result_t rc = mudmux_execution_enqueue_hook(
                    HOOK_MESSAGE_INBOUND, this, queue_pressure_hot_slot, msg.data(), msg.size());
                if (rc == MUDMUX_DISPATCH_QUEUE_FULL)
                    queue_full_count.fetch_add(1);
                else if (rc != MUDMUX_DISPATCH_OK)
                    enqueue_error_count.fetch_add(1);
            }
        });
    }

    for (std::size_t index = 1; index < slots.size(); ++index) {
        ASSERT_EQ(mudmux_execution_enqueue_hook(HOOK_MESSAGE_INBOUND, this, slots[index], payload, strlen(payload)), MUDMUX_DISPATCH_OK);
    }

    ASSERT_EQ(other_slots_done_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_GT(queue_full_count.load(), 0);
    EXPECT_EQ(enqueue_error_count.load(), 0);

    hot_slot_release_promise.set_value();
    for (auto& producer : producers)
        producer.join();

    queue_pressure_hot_slot_entered_ptr = nullptr;
    queue_pressure_hot_slot_release_ptr = nullptr;
    queue_pressure_other_slots_done_ptr = nullptr;
    queue_pressure_other_slots_expected = 0;
    queue_pressure_hot_slot = -1;

    mudmux_deinit();
}

TEST_F(CommInboundTest, RelaxedModeConcurrentEnqueueAndCommApiMutationsRemainStable) {
    mudmux_deinit();
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 4}}}"));

    std::vector<int> slots;
    slots.reserve(8);
    for (int index = 0; index < 8; ++index) {
        const int slot = add_memory_comm(C_LINE_INPUT);
        ASSERT_NE(slot, -1);
        slots.push_back(slot);
    }

    enqueue_race_processed_count.store(0);
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, enqueue_race_comm_api_hook));

    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = 40;
    const int expected_tasks = producer_count * tasks_per_producer;
    std::atomic<int> accepted_tasks{0};
    std::atomic<bool> enqueue_failed{false};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);

    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer]() {
            for (int index = 0; index < tasks_per_producer; ++index) {
                const int slot = slots[static_cast<std::size_t>((producer + index) % static_cast<int>(slots.size()))];
                const std::string msg = "race-" + std::to_string(producer) + "-" + std::to_string(index);
                bool submitted = false;
                for (int retry = 0; retry < 5000; ++retry) {
                    const mudmux_dispatch_result_t rc = mudmux_execution_enqueue_hook(
                        HOOK_MESSAGE_INBOUND, this, slot, msg.data(), msg.size());
                    if (rc == MUDMUX_DISPATCH_OK) {
                        accepted_tasks.fetch_add(1);
                        submitted = true;
                        break;
                    }
                    if (rc == MUDMUX_DISPATCH_ERROR) {
                        enqueue_failed.store(true);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (!submitted)
                    enqueue_failed.store(true);
            }
        });
    }

    for (auto& producer : producers)
        producer.join();

    ASSERT_FALSE(enqueue_failed.load());
    ASSERT_EQ(accepted_tasks.load(), expected_tasks);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && enqueue_race_processed_count.load() < expected_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(enqueue_race_processed_count.load(), expected_tasks);

    mudmux_deinit();
}
