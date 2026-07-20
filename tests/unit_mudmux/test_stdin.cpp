#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "mudmux/mudmux.h"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

using namespace testing;

static std::atomic_bool stdin_hook_called{false};
static std::promise<std::thread::id>* prompt_thread_promise_ptr{nullptr};
static std::promise<void>* backpressure_first_entered_ptr{nullptr};
static std::shared_future<void>* backpressure_release_ptr{nullptr};
static std::promise<void>* backpressure_done_ptr{nullptr};
static std::atomic<int> backpressure_hook_count{0};
static std::mutex backpressure_messages_mtx;
static std::vector<std::string>* backpressure_messages_ptr{nullptr};

static int enable_prompt_on_inbound(void*, int slot, void*, size_t) {
    comm_enable_prompt(slot, true);
    return 0;
}

static int capture_prompt_thread_and_shutdown(void*, int, void*, size_t) {
    if (prompt_thread_promise_ptr)
        prompt_thread_promise_ptr->set_value(std::this_thread::get_id());
    mudmux_shutdown();
    return 0;
}

static int block_first_inbound_then_shutdown_on_last(void*, int, void* data, size_t len) {
    const int count = backpressure_hook_count.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(backpressure_messages_mtx);
        if (backpressure_messages_ptr)
            backpressure_messages_ptr->emplace_back(static_cast<char*>(data), len);
    }

    if (count == 1) {
        if (backpressure_first_entered_ptr)
            backpressure_first_entered_ptr->set_value();
        if (backpressure_release_ptr)
            backpressure_release_ptr->wait();
    }

    if (count == 12) {
        if (backpressure_done_ptr)
            backpressure_done_ptr->set_value();
        mudmux_shutdown();
    }
    return 0;
}

class MudmuxStdinTest : public Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(mudmux_init(nullptr));
    }
    void TearDown() override {
        ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
    }
};

TEST_F(MudmuxStdinTest, PipeInput) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to prevent test from crashing on broken pipe
#endif
    mudmux_set_log_level (0);
    SPDLOG_INFO ("CTEST_FULL_OUTPUT");

    // Enable standard input
    mudmux_enable_standard_input(true);
#ifdef _WIN32
    int pipefd[2] = {-1, -1};
    int saved_stdin = _dup(_fileno(stdin));
    HANDLE saved_stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    ASSERT_NE(saved_stdin, -1);
    ASSERT_EQ(_pipe(pipefd, 4096, _O_BINARY), 0);
    ASSERT_NE(_dup2(pipefd[0], _fileno(stdin)), -1);
    intptr_t pipe_read_handle = _get_osfhandle(pipefd[0]);
    ASSERT_NE(pipe_read_handle, static_cast<intptr_t>(-1));
    ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(pipe_read_handle)));
#else
    int pipefd[2] = {-1, -1};
    int saved_stdin = dup(STDIN_FILENO);
    ASSERT_NE(saved_stdin, -1);
    ASSERT_EQ(pipe(pipefd), 0);
    ASSERT_NE(dup2(pipefd[0], STDIN_FILENO), -1);
    close(pipefd[0]);
#endif

    // define a hook function to validate that input is received
    stdin_hook_called = false;
    bool result = mudmux_register_hook (HOOK_MESSAGE_INBOUND, [](void* /*context*/, int slot, void* data, size_t len) -> int {
        EXPECT_EQ(slot, COMM_SLOT_CONSOLE);
        std::string received_message(static_cast<char*>(data), len);
        SPDLOG_INFO ("Received message: {}", received_message);
        EXPECT_EQ(received_message, "test input");
        stdin_hook_called = true;
        return 0;
    });
    EXPECT_TRUE(result);

    // Run the event loop in a separate thread
    std::thread server_thread([]() {
        int result = mudmux_run(nullptr);
        ASSERT_EQ(result, EXIT_SUCCESS);
    });

    const char* test_input = "test input\n";
#ifdef _WIN32
    const auto test_input_len = static_cast<unsigned int>(strlen(test_input));
    ASSERT_EQ(_write(pipefd[1], test_input, test_input_len), static_cast<int>(test_input_len));
    _close(pipefd[1]);
#else
    write(pipefd[1], test_input, strlen(test_input));
    close(pipefd[1]); // Close the write end of the pipe
#endif

    // Allow some time for the input to be processed
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Shutdown the server
    mudmux_shutdown();

    // Wait for the server thread to finish
    server_thread.join();

#ifdef _WIN32
    ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, saved_stdin_handle));
    ASSERT_NE(_dup2(saved_stdin, _fileno(stdin)), -1);
    _close(saved_stdin);
    _close(pipefd[0]);
#else
    ASSERT_NE(dup2(saved_stdin, STDIN_FILENO), -1);
    close(saved_stdin);
#endif

    EXPECT_TRUE(stdin_hook_called);
}

TEST(MudmuxStdinThreadPoolTest, PromptHookRunsOnWorkerThreadInRelaxedMode) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 2}}}"));
    mudmux_enable_standard_input(true);

#ifdef _WIN32
    int pipefd[2] = {-1, -1};
    int saved_stdin = _dup(_fileno(stdin));
    HANDLE saved_stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    ASSERT_NE(saved_stdin, -1);
    ASSERT_EQ(_pipe(pipefd, 4096, _O_BINARY), 0);
    ASSERT_NE(_dup2(pipefd[0], _fileno(stdin)), -1);
    intptr_t pipe_read_handle = _get_osfhandle(pipefd[0]);
    ASSERT_NE(pipe_read_handle, static_cast<intptr_t>(-1));
    ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(pipe_read_handle)));
#else
    int pipefd[2] = {-1, -1};
    int saved_stdin = dup(STDIN_FILENO);
    ASSERT_NE(saved_stdin, -1);
    ASSERT_EQ(pipe(pipefd), 0);
    ASSERT_NE(dup2(pipefd[0], STDIN_FILENO), -1);
    close(pipefd[0]);
#endif

    std::promise<std::thread::id> loop_thread_promise;
    std::future<std::thread::id> loop_thread_future = loop_thread_promise.get_future();
    std::promise<std::thread::id> prompt_thread_promise;
    std::future<std::thread::id> prompt_thread_future = prompt_thread_promise.get_future();
    prompt_thread_promise_ptr = &prompt_thread_promise;

    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, enable_prompt_on_inbound));
    ASSERT_TRUE(mudmux_register_hook(HOOK_PROMPT, capture_prompt_thread_and_shutdown));

    std::thread server_thread([&loop_thread_promise]() {
        loop_thread_promise.set_value(std::this_thread::get_id());
        EXPECT_EQ(mudmux_run(nullptr), EXIT_SUCCESS);
    });

    const char* test_input = "prompt\n";
#ifdef _WIN32
    const auto test_input_len = static_cast<unsigned int>(strlen(test_input));
    ASSERT_EQ(_write(pipefd[1], test_input, test_input_len), static_cast<int>(test_input_len));
    _close(pipefd[1]);
#else
    ASSERT_EQ(write(pipefd[1], test_input, strlen(test_input)), static_cast<ssize_t>(strlen(test_input)));
    close(pipefd[1]);
#endif

    ASSERT_EQ(prompt_thread_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const std::thread::id prompt_thread_id = prompt_thread_future.get();
    const std::thread::id loop_thread_id = loop_thread_future.get();

    server_thread.join();

#ifdef _WIN32
    ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, saved_stdin_handle));
    ASSERT_NE(_dup2(saved_stdin, _fileno(stdin)), -1);
    _close(saved_stdin);
    _close(pipefd[0]);
#else
    ASSERT_NE(dup2(saved_stdin, STDIN_FILENO), -1);
    close(saved_stdin);
#endif

    EXPECT_NE(prompt_thread_id, loop_thread_id);
    prompt_thread_promise_ptr = nullptr;
    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}

TEST(MudmuxStdinThreadPoolTest, InboundQueueFullDefersAndResumesInFifoOrder) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 2}}}"));
    mudmux_enable_standard_input(true);

#ifdef _WIN32
    int pipefd[2] = {-1, -1};
    int saved_stdin = _dup(_fileno(stdin));
    HANDLE saved_stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    ASSERT_NE(saved_stdin, -1);
    ASSERT_EQ(_pipe(pipefd, 4096, _O_BINARY), 0);
    ASSERT_NE(_dup2(pipefd[0], _fileno(stdin)), -1);
    intptr_t pipe_read_handle = _get_osfhandle(pipefd[0]);
    ASSERT_NE(pipe_read_handle, static_cast<intptr_t>(-1));
    ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(pipe_read_handle)));
#else
    int pipefd[2] = {-1, -1};
    int saved_stdin = dup(STDIN_FILENO);
    ASSERT_NE(saved_stdin, -1);
    ASSERT_EQ(pipe(pipefd), 0);
    ASSERT_NE(dup2(pipefd[0], STDIN_FILENO), -1);
    close(pipefd[0]);
#endif

    std::promise<void> first_entered_promise;
    std::future<void> first_entered_future = first_entered_promise.get_future();
    std::promise<void> release_promise;
    std::shared_future<void> release_future = release_promise.get_future().share();
    std::promise<void> done_promise;
    std::future<void> done_future = done_promise.get_future();
    std::vector<std::string> messages;

    backpressure_first_entered_ptr = &first_entered_promise;
    backpressure_release_ptr = &release_future;
    backpressure_done_ptr = &done_promise;
    backpressure_messages_ptr = &messages;
    backpressure_hook_count.store(0);

    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, block_first_inbound_then_shutdown_on_last));

    std::thread server_thread([]() {
        EXPECT_EQ(mudmux_run(nullptr), EXIT_SUCCESS);
    });

    std::string payload;
    for (int index = 0; index < 12; ++index) {
        payload += "line" + std::to_string(index) + "\n";
    }

#ifdef _WIN32
    ASSERT_EQ(_write(pipefd[1], payload.data(), static_cast<unsigned int>(payload.size())), static_cast<int>(payload.size()));
    _close(pipefd[1]);
#else
    ASSERT_EQ(write(pipefd[1], payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));
    close(pipefd[1]);
#endif

    ASSERT_EQ(first_entered_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    release_promise.set_value();
    ASSERT_EQ(done_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    server_thread.join();

#ifdef _WIN32
    ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, saved_stdin_handle));
    ASSERT_NE(_dup2(saved_stdin, _fileno(stdin)), -1);
    _close(saved_stdin);
    _close(pipefd[0]);
#else
    ASSERT_NE(dup2(saved_stdin, STDIN_FILENO), -1);
    close(saved_stdin);
#endif

    ASSERT_EQ(messages.size(), 12u);
    for (int index = 0; index < 12; ++index)
        EXPECT_EQ(messages[static_cast<std::size_t>(index)], "line" + std::to_string(index));

    backpressure_first_entered_ptr = nullptr;
    backpressure_release_ptr = nullptr;
    backpressure_done_ptr = nullptr;
    backpressure_messages_ptr = nullptr;
    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}
