#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <signal.h>
#include <string>
#include <thread>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "mudmux/mudmux.h"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

using namespace testing;

static std::atomic_bool stdin_hook_called{false};

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
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to prevent test from crashing on broken pipe

    // Enable standard input
    mudmux_enable_standard_input(true);
#ifndef _WIN32
    int pipefd[2] = {-1, -1};
    int saved_stdin = dup(STDIN_FILENO);
    ASSERT_NE(saved_stdin, -1);
    ASSERT_EQ(pipe(pipefd), 0);
    ASSERT_NE(dup2(pipefd[0], STDIN_FILENO), -1);
    close(pipefd[0]);
#endif

    // define a hook function to validate that input is received
    stdin_hook_called = false;
    bool result = mudmux_register_hook (MUDMUX_HOOK_MESSAGE_INBOUND, [](void* /*context*/, int slot, void* data, size_t len) -> int {
        EXPECT_EQ(slot, COMM_SLOT_CONSOLE);
        std::string received_message(static_cast<char*>(data), len);
        SPDLOG_INFO ("Received message: {}", received_message);
        EXPECT_EQ(received_message, "test input\n");
        stdin_hook_called = true;
        return 0;
    });
    EXPECT_TRUE(result);

    // Run the event loop in a separate thread
    std::thread server_thread([]() {
        int result = mudmux_run(nullptr);
        ASSERT_EQ(result, EXIT_SUCCESS);
    });

    // Simulate input to stdin (this is a placeholder; actual implementation may vary)
#ifndef _WIN32
    const char* test_input = "test input\n";
    write(pipefd[1], test_input, strlen(test_input));
    close(pipefd[1]); // Close the write end of the pipe
#endif

    // Allow some time for the input to be processed
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Shutdown the server
    mudmux_shutdown();

    // Wait for the server thread to finish
    server_thread.join();

#ifndef _WIN32
    ASSERT_NE(dup2(saved_stdin, STDIN_FILENO), -1);
    close(saved_stdin);
#endif

    EXPECT_TRUE(stdin_hook_called);
}
