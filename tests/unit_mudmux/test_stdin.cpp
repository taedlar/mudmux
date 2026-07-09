#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <signal.h>
#include <string>
#include <thread>
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
    bool result = mudmux_register_hook (MUDMUX_HOOK_MESSAGE_INBOUND, [](void* /*context*/, int slot, void* data, size_t len) -> int {
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
