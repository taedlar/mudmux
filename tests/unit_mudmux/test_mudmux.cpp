#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtest/gtest.h>
#include <thread>

#include "mudmux/mudmux.h"

TEST(MudmuxTest, BasicInitialization) {
    // Test that the mudmux library initializes correctly
    ASSERT_TRUE(mudmux_init(nullptr));
    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}

TEST(MudmuxTest, EventLoopRun) {
    // Test that the mudmux event loop can run and shutdown correctly
    ASSERT_TRUE(mudmux_init(nullptr));
    
    std::thread server_thread([]() {
        int result = mudmux_run(nullptr);
        ASSERT_EQ(result, EXIT_SUCCESS);
    });

    // Allow some time for the server to start
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Shutdown the server
    mudmux_shutdown();

    // Wait for the server thread to finish
    server_thread.join();

    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}

TEST(MudmuxTest, EventLoopRunWithConsoleEnabled) {
    mudmux_set_log_level(0);
    SPDLOG_INFO ("CTEST_FULL_OUTPUT");

    // Test that the mudmux event loop can run and shutdown correctly with console enabled
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"console\": true}}"));
    
    std::thread server_thread([]() {
        int result = mudmux_run(nullptr);
        ASSERT_EQ(result, EXIT_SUCCESS);
    });

    // Allow some time for the server to start
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Shutdown the server
    mudmux_shutdown();

    // Wait for the server thread to finish
    server_thread.join();

    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}
