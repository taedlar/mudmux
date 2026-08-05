#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include "mudmux/mudmux.h"
#include "mudmux/async.h"
#include "mudmux/comm.h"
#include "mudmux/hooks.h"

static int close_console_on_quit(void*, int slot, void* data, size_t len) {
    std::string message(static_cast<char*>(data), len);
    if (message == "/quit")
        (void)comm_close(nullptr, slot);
    return 0;
}

static int write_and_close_console_on_quit(void*, int slot, void* data, size_t len) {
    std::string message(static_cast<char*>(data), len);
    if (message == "/quit") {
        const char* pending = "pending before close\n";
        comm_buffered_write(slot, pending, strlen(pending));
        (void)comm_close(nullptr, slot);
    }
    return 0;
}

static std::atomic<int> timer_hook_calls{0};
static std::atomic<int> timer_hook_msg{-1};

static int shutdown_on_timer(void*, int msg, void*, size_t) {
    ++timer_hook_calls;
    timer_hook_msg.store(msg);
    mudmux_shutdown();
    return 0;
}

static std::atomic<int> custom_event_hook_calls{0};

static int shutdown_on_custom_event(void*, int, void*, size_t) {
    ++custom_event_hook_calls;
    mudmux_shutdown();
    return 0;
}

static std::atomic<int> garbage_collection_hook_calls{0};

static int count_garbage_collection(void*, int, void*, size_t) {
    ++garbage_collection_hook_calls;
    return 0;
}

TEST(MudmuxTest, BasicInitialization) {
    // Test that the mudmux library initializes correctly
    ASSERT_TRUE(mudmux_init(nullptr));
    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}

TEST(MudmuxTest, InitializationWithEmptyConfig) {
    // Test that the mudmux library initializes correctly with a configuration
    // Configuration can be provided as YAML or JSON (JSON is a subset of YAML)
    const char* config = R"({
    })";
    ASSERT_TRUE(mudmux_init(config));
    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}

TEST(MudmuxTest, InitializationWithIncorrectConfig) {
    // Test that the mudmux library fails to initialize with an incorrect configuration
    const char* incorrect_config = R"({
        "transport": {
            "console": "not_a_boolean"
        }
    })";
    ASSERT_FALSE(mudmux_init(incorrect_config));
}

TEST(MudmuxTest, InitializationWithInvalidThreadPoolSize) {
    const char* incorrect_config = R"({
        "transport": {
            "thread_pool": {
                "size": 0
            }
        }
    })";
    ASSERT_FALSE(mudmux_init(incorrect_config));
}

TEST(MudmuxTest, InitializationWithThreadPoolSize) {
    const char* config = R"({
        "transport": {
            "thread_pool": {
                "size": 2
            }
        }
    })";
    ASSERT_TRUE(mudmux_init(config));
    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}

TEST(MudmuxTest, AsyncApiExposesQueueOperations) {
    ASSERT_TRUE(mudmux_init(nullptr));
    async_queue_t* queue = async_queue_create(2, 16, ASYNC_QUEUE_DROP_OLDEST);
    ASSERT_NE(queue, nullptr);
    EXPECT_TRUE(async_queue_is_empty(queue));

    const char message[] = "event";
    ASSERT_TRUE(async_queue_enqueue(queue, message, sizeof(message)));
    EXPECT_TRUE(async_queue_is_full(queue) == false);
    char received[16]{};
    size_t received_size = 0;
    ASSERT_TRUE(async_queue_dequeue(queue, received, sizeof(received), &received_size));
    EXPECT_EQ(received_size, sizeof(message));
    EXPECT_STREQ(received, message);

    async_queue_stats_t stats{};
    async_queue_get_stats(queue, &stats);
    EXPECT_EQ(stats.enqueue_count, 1u);
    EXPECT_EQ(stats.dequeue_count, 1u);
    async_queue_destroy(queue);
    mudmux_deinit();
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

TEST(MudmuxTest, TimerEventDispatchesTimerHook) {
    timer_hook_calls.store(0);
    timer_hook_msg.store(-1);
    ASSERT_TRUE(mudmux_init(nullptr));
    ASSERT_NE(mudmux_get_timer_event(), nullptr);
    ASSERT_TRUE(mudmux_register_hook(HOOK_TIMER, shutdown_on_timer));

    std::promise<int> run_result_promise;
    auto run_result = run_result_promise.get_future();
    std::thread server_thread([&run_result_promise]() {
        run_result_promise.set_value(mudmux_run(nullptr));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(mudmux_trigger_timer(42));
    ASSERT_EQ(run_result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(run_result.get(), EXIT_SUCCESS);
    server_thread.join();
    EXPECT_EQ(timer_hook_calls.load(), 1);
    EXPECT_EQ(timer_hook_msg.load(), 42);
    mudmux_deinit();
}

TEST(MudmuxTest, GarbageCollectionHookWakesIdleEventLoop) {
    garbage_collection_hook_calls.store(0);
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"keep_alive_interval\": 1}}"));
    ASSERT_TRUE(mudmux_register_hook(HOOK_GARBAGE_COLLECTION, count_garbage_collection));

    std::promise<int> run_result_promise;
    auto run_result = run_result_promise.get_future();
    std::thread server_thread([&run_result_promise]() {
        run_result_promise.set_value(mudmux_run(nullptr));
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (garbage_collection_hook_calls.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool garbage_collection_called = garbage_collection_hook_calls.load() > 0;

    mudmux_shutdown();
    ASSERT_EQ(run_result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(run_result.get(), EXIT_SUCCESS);
    server_thread.join();
    EXPECT_TRUE(garbage_collection_called);
    mudmux_deinit();
}

TEST(MudmuxTest, CustomAsyncEventDispatchesRegisteredHook) {
    custom_event_hook_calls.store(0);
    ASSERT_TRUE(mudmux_init(nullptr));
    async_event_t event{};
    ASSERT_TRUE(async_event_init(&event, true, false));
    ASSERT_TRUE(mudmux_register_event(&event, shutdown_on_custom_event));

    std::promise<int> run_result_promise;
    auto run_result = run_result_promise.get_future();
    std::thread server_thread([&run_result_promise]() {
        run_result_promise.set_value(mudmux_run(nullptr));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    async_event_set(&event);
    const auto status = run_result.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        mudmux_shutdown();
        server_thread.join();
        FAIL() << "custom async event did not wake mudmux_run";
    }
    EXPECT_EQ(run_result.get(), EXIT_SUCCESS);
    server_thread.join();
    EXPECT_EQ(custom_event_hook_calls.load(), 1);
    async_event_destroy(&event);
    mudmux_deinit();
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

TEST(MudmuxTest, FileInputQuitShutsDownServer) {
    const auto temp_dir = std::filesystem::temp_directory_path();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto input_path = temp_dir / ("mudmux-quit-input-" + stamp + ".txt");
    const auto output_path = temp_dir / ("mudmux-quit-output-" + stamp + ".txt");

    {
        std::ofstream input_file(input_path);
        ASSERT_TRUE(input_file.is_open());
        input_file << "/quit\n";
    }

    ASSERT_TRUE(mudmux_init(nullptr));
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, close_console_on_quit));
    ASSERT_GE(comm_abstract_add_file(input_path.string().c_str(), output_path.string().c_str(), COMM_SLOT_CONSOLE, C_LINE_INPUT), 0);

    std::promise<int> run_result_promise;
    std::future<int> run_result_future = run_result_promise.get_future();
    std::thread server_thread([&run_result_promise]() {
        run_result_promise.set_value(mudmux_run(nullptr));
    });

    const auto status = run_result_future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        mudmux_shutdown();
        server_thread.join();
        FAIL() << "mudmux_run did not return after /quit from file input";
    }

    EXPECT_EQ(run_result_future.get(), EXIT_SUCCESS);
    server_thread.join();

    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
    std::error_code ec;
    std::filesystem::remove(input_path, ec);
    std::filesystem::remove(output_path, ec);
}

TEST(MudmuxTest, FileInputQuitWithBufferedOutputStillShutsDownServer) {
    const auto temp_dir = std::filesystem::temp_directory_path();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto input_path = temp_dir / ("mudmux-quit-buffered-input-" + stamp + ".txt");
    const auto output_path = temp_dir / ("mudmux-quit-buffered-output-" + stamp + ".txt");

    {
        std::ofstream input_file(input_path);
        ASSERT_TRUE(input_file.is_open());
        input_file << "/quit\n";
    }

    ASSERT_TRUE(mudmux_init(nullptr));
    ASSERT_TRUE(mudmux_register_hook(HOOK_MESSAGE_INBOUND, write_and_close_console_on_quit));
    ASSERT_GE(comm_abstract_add_file(input_path.string().c_str(), output_path.string().c_str(), COMM_SLOT_CONSOLE, C_LINE_INPUT), 0);

    std::promise<int> run_result_promise;
    std::future<int> run_result_future = run_result_promise.get_future();
    std::thread server_thread([&run_result_promise]() {
        run_result_promise.set_value(mudmux_run(nullptr));
    });

    const auto status = run_result_future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        mudmux_shutdown();
        server_thread.join();
        FAIL() << "mudmux_run did not return after /quit with buffered output from file input";
    }

    EXPECT_EQ(run_result_future.get(), EXIT_SUCCESS);
    server_thread.join();

    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
    std::error_code ec;
    std::filesystem::remove(input_path, ec);
    std::filesystem::remove(output_path, ec);
}

TEST(MudmuxTest, FileInputEofShutsDownServerWithoutExplicitClose) {
    const auto temp_dir = std::filesystem::temp_directory_path();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto input_path = temp_dir / ("mudmux-eof-input-" + stamp + ".txt");
    const auto output_path = temp_dir / ("mudmux-eof-output-" + stamp + ".txt");

    {
        std::ofstream input_file(input_path);
        ASSERT_TRUE(input_file.is_open());
        input_file << "hello\n";
    }

    ASSERT_TRUE(mudmux_init(nullptr));
    ASSERT_GE(comm_abstract_add_file(input_path.string().c_str(), output_path.string().c_str(), COMM_SLOT_CONSOLE, C_LINE_INPUT), 0);

    std::promise<int> run_result_promise;
    std::future<int> run_result_future = run_result_promise.get_future();
    std::thread server_thread([&run_result_promise]() {
        run_result_promise.set_value(mudmux_run(nullptr));
    });

    const auto status = run_result_future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        mudmux_shutdown();
        server_thread.join();
        FAIL() << "mudmux_run did not return on file input EOF";
    }

    EXPECT_EQ(run_result_future.get(), EXIT_SUCCESS);
    server_thread.join();

    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
    std::error_code ec;
    std::filesystem::remove(input_path, ec);
    std::filesystem::remove(output_path, ec);
}
