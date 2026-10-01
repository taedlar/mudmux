#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include "mudmux/mudmux.h"
#include "mudmux/workers.h"
#include "mudmux/async.h"
#include "mudmux/comm.h"
#include "mudmux/execution.h"
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
    if (msg == 42) {
        ++timer_hook_calls;
        timer_hook_msg.store(msg);
        mudmux_shutdown();
    }
    return 0;
}

static std::mutex timer_lifecycle_mutex;
static std::vector<int> timer_lifecycle_messages;

static int record_timer_lifecycle(void*, int msg, void*, size_t) {
    std::lock_guard<std::mutex> lock(timer_lifecycle_mutex);
    timer_lifecycle_messages.push_back(msg);
    return 0;
}

static std::atomic<int> custom_event_hook_calls{0};
static std::atomic<int> auto_reset_event_hook_calls{0};
static async_event_t* auto_reset_event{nullptr};

static int shutdown_on_custom_event(void*, int, void*, size_t) {
    ++custom_event_hook_calls;
    mudmux_shutdown();
    return 0;
}

static int resignal_auto_reset_event(void*, int, void*, size_t) {
    if (++auto_reset_event_hook_calls == 1) {
        async_event_set(auto_reset_event);
    } else {
        mudmux_shutdown();
    }
    return 0;
}

static std::atomic<int> garbage_collection_hook_calls{0};

static std::atomic<int> shutdown_disconnect_hook_calls{0};

static int count_shutdown_disconnect(void*, int, void*, size_t) {
    ++shutdown_disconnect_hook_calls;
    return 0;
}

struct detached_closure_context_t {
    std::atomic<int> work_calls{0};
    std::atomic<int> completion_calls{0};
    std::atomic<int> destruction_calls{0};
    std::atomic<int> completion_message{ASYNC_CLOSURE_SCHEDULER_FAILED - 1};
    std::promise<void> completed;
};

struct serialized_completion_context_t {
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    std::atomic<int> completed{0};
    std::promise<void> all_completed;
};

static void run_detached_work(void* context, int) {
    ++static_cast<detached_closure_context_t*>(context)->work_calls;
}

static void throw_from_detached_work(void*, int) {
    throw std::runtime_error("expected detached work failure");
}

static void run_detached_completion(void* context, int message) {
    auto* task = static_cast<detached_closure_context_t*>(context);
    ++task->completion_calls;
    task->completion_message.store(message);
    task->completed.set_value();
}

static void destroy_detached_closure(void* context) {
    ++static_cast<detached_closure_context_t*>(context)->destruction_calls;
}

static void run_noop_detached_work(void*, int) {
}

static void run_serialized_completion(void* context, int) {
    auto* completion = static_cast<serialized_completion_context_t*>(context);
    const int active = completion->active.fetch_add(1) + 1;
    int observed_max = completion->max_active.load();
    while (active > observed_max &&
           !completion->max_active.compare_exchange_weak(observed_max, active)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    completion->active.fetch_sub(1);
    if (completion->completed.fetch_add(1) + 1 == 2)
        completion->all_completed.set_value();
}

static void throw_from_detached_destroy(void*) {
    throw std::runtime_error("expected detached destroy failure");
}

static int count_garbage_collection(void*, int, void*, size_t) {
    ++garbage_collection_hook_calls;
    return 0;
}

struct logger_callback_record_t {
    void* context;
    int level;
    std::string file;
    int line;
    std::string function;
    std::string message;
};

static std::mutex logger_callback_mutex;
static std::vector<logger_callback_record_t> logger_callback_records;

static void record_logger_callback(void* context, int level, const char* file, int line, const char* function, const char* message) {
    std::lock_guard<std::mutex> lock(logger_callback_mutex);
    logger_callback_records.push_back(logger_callback_record_t{
        context,
        level,
        file ? file : "",
        line,
        function ? function : "",
        message ? message : ""});
}

TEST(MudmuxTest, LoggerCallbackReceivesDefaultLoggerMessages) {
    {
        std::lock_guard<std::mutex> lock(logger_callback_mutex);
        logger_callback_records.clear();
    }

    int logger_context = 0;
    mudmux_register_logger_callback(record_logger_callback, &logger_context);
    ASSERT_FALSE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 0}}}"));

    {
        std::lock_guard<std::mutex> lock(logger_callback_mutex);
        ASSERT_EQ(logger_callback_records.size(), 1u);
        EXPECT_EQ(logger_callback_records[0].context, &logger_context);
        EXPECT_EQ(logger_callback_records[0].level, spdlog::level::err);
        EXPECT_NE(logger_callback_records[0].line, 0);
        EXPECT_NE(logger_callback_records[0].file.find("mudmux.cpp"), std::string::npos);
        EXPECT_NE(logger_callback_records[0].function.find("mudmux_init"), std::string::npos);
        EXPECT_EQ(logger_callback_records[0].message, "transport.thread_pool.size must be at least 1");
    }
}

TEST(MudmuxTest, BasicInitialization) {
    // Test that the mudmux library initializes correctly
    ASSERT_TRUE(mudmux_init(nullptr));
    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}

TEST(MudmuxTest, InitializationStartsWorkers) {
    ASSERT_TRUE(mudmux_init(nullptr));
    EXPECT_FALSE(mudmux_workers_start());
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

TEST(MudmuxTest, InitializationWithInvalidThreadPoolBacklogCapacity) {
    const char* incorrect_config = R"({
        "transport": {
            "thread_pool": {
                "backlog_capacity": 0
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
    EXPECT_EQ(mudmux_workers_pool_size(), 2);
    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
    EXPECT_EQ(mudmux_workers_pool_size(), 0);
}

TEST(MudmuxTest, InitializationWithThreadPoolBacklogCapacity) {
    const char* config = R"({
        "transport": {
            "thread_pool": {
                "size": 2,
                "backlog_capacity": 3
            }
        }
    })";
    ASSERT_TRUE(mudmux_init(config));
    EXPECT_EQ(mudmux_workers_pool_size(), 2u);
    ASSERT_NO_FATAL_FAILURE(mudmux_deinit());
}

TEST(MudmuxTest, PseudoHookTypesAreNotRegisterable) {
    ASSERT_TRUE(mudmux_init(nullptr));
    EXPECT_TRUE(mudmux_register_hook(HOOK_GARBAGE_COLLECTION, count_garbage_collection));
    EXPECT_FALSE(mudmux_register_hook(MAX_PUBLIC_HOOKS, count_garbage_collection));
    EXPECT_FALSE(mudmux_register_hook(HOOK_RESUME, count_garbage_collection));
    EXPECT_FALSE(mudmux_register_hook(HOOK_COMPLETION, count_garbage_collection));
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

TEST(MudmuxTest, WorkersSubmitRunsWorkThenCompletion) {
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 2}}}"));
    detached_closure_context_t context;
    std::future<void> completed = context.completed.get_future();
    async_closure_t work{run_detached_work, destroy_detached_closure, &context};
    async_closure_t completion{run_detached_completion, destroy_detached_closure, &context};

    ASSERT_TRUE(mudmux_workers_submit(&work, &completion));
    EXPECT_FALSE(async_closure_is_valid(&work));
    EXPECT_FALSE(async_closure_is_valid(&completion));
    EXPECT_EQ(completed.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(context.work_calls.load(), 1);
    EXPECT_EQ(context.completion_calls.load(), 1);
    EXPECT_EQ(context.completion_message.load(), ASYNC_CLOSURE_SCHEDULER_OK);

    mudmux_deinit();
    EXPECT_EQ(context.destruction_calls.load(), 2);
}

TEST(MudmuxTest, WorkersSubmitFailureRetainsClosureOwnership) {
    detached_closure_context_t context;
    async_closure_t work{run_detached_work, destroy_detached_closure, &context};
    async_closure_t completion{run_detached_completion, destroy_detached_closure, &context};

    EXPECT_FALSE(mudmux_workers_submit(&work, &completion));
    EXPECT_TRUE(async_closure_is_valid(&work));
    EXPECT_TRUE(async_closure_is_valid(&completion));

    async_closure_destroy(&work);
    async_closure_destroy(&completion);
    EXPECT_EQ(context.destruction_calls.load(), 2);
}

TEST(MudmuxTest, WorkersSubmitContainsClosureExceptions) {
    ASSERT_TRUE(mudmux_init(nullptr));
    detached_closure_context_t context;
    std::future<void> completed = context.completed.get_future();
    async_closure_t work{throw_from_detached_work, destroy_detached_closure, &context};
    async_closure_t completion{run_detached_completion, destroy_detached_closure, &context};

    ASSERT_TRUE(mudmux_workers_submit(&work, &completion));
    EXPECT_EQ(completed.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(context.work_calls.load(), 0);
    EXPECT_EQ(context.completion_calls.load(), 1);
    EXPECT_EQ(context.completion_message.load(), ASYNC_CLOSURE_SCHEDULER_FAILED);

    mudmux_deinit();
    EXPECT_EQ(context.destruction_calls.load(), 2);
}

TEST(MudmuxTest, WorkersSubmitSerializesCompletions) {
    ASSERT_TRUE(mudmux_init("{\"transport\": {\"thread_pool\": {\"size\": 2}}}"));
    serialized_completion_context_t context;
    std::future<void> completed = context.all_completed.get_future();
    async_closure_t work_one{run_noop_detached_work, nullptr, nullptr};
    async_closure_t completion_one{run_serialized_completion, nullptr, &context};
    async_closure_t work_two{run_noop_detached_work, nullptr, nullptr};
    async_closure_t completion_two{run_serialized_completion, nullptr, &context};

    ASSERT_TRUE(mudmux_workers_submit(&work_one, &completion_one));
    ASSERT_TRUE(mudmux_workers_submit(&work_two, &completion_two));
    EXPECT_EQ(completed.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(context.max_active.load(), 1);

    mudmux_deinit();
}

TEST(MudmuxTest, WorkersSubmitContainsClosureDestroyExceptions) {
    ASSERT_TRUE(mudmux_init(nullptr));
    detached_closure_context_t context;
    std::future<void> completed = context.completed.get_future();
    async_closure_t work{run_detached_work, throw_from_detached_destroy, &context};
    async_closure_t completion{run_detached_completion, throw_from_detached_destroy, &context};

    ASSERT_TRUE(mudmux_workers_submit(&work, &completion));
    EXPECT_EQ(completed.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(context.work_calls.load(), 1);
    EXPECT_EQ(context.completion_calls.load(), 1);

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

TEST(MudmuxTest, ExecutionStateTracksActiveRun) {
    ASSERT_TRUE(mudmux_init(nullptr));
    ASSERT_NE(mudmux_execution_api_v1, nullptr);
    ASSERT_NE(mudmux_execution_api_v1->is_running, nullptr);
    EXPECT_FALSE(mudmux_is_running());

    std::thread server_thread([] {
        EXPECT_EQ(mudmux_run(nullptr), EXIT_SUCCESS);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!mudmux_is_running() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(mudmux_is_running());

    mudmux_shutdown();
    server_thread.join();
    EXPECT_FALSE(mudmux_is_running());
    mudmux_deinit();
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

TEST(MudmuxTest, TimerHookReceivesEventLoopLifecycleNotifications) {
    {
        std::lock_guard<std::mutex> lock(timer_lifecycle_mutex);
        timer_lifecycle_messages.clear();
    }
    ASSERT_TRUE(mudmux_init(nullptr));
    ASSERT_TRUE(mudmux_register_hook(HOOK_TIMER, record_timer_lifecycle));

    std::thread server_thread([]() {
        EXPECT_EQ(mudmux_run(nullptr), EXIT_SUCCESS);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mudmux_shutdown();
    server_thread.join();

    {
        std::lock_guard<std::mutex> lock(timer_lifecycle_mutex);
        ASSERT_EQ(timer_lifecycle_messages.size(), 2u);
        EXPECT_EQ(timer_lifecycle_messages[0], 0);
        EXPECT_EQ(timer_lifecycle_messages[1], -1);
    }
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

TEST(MudmuxTest, AutoResetAsyncEventResignalsDuringHook) {
    auto_reset_event_hook_calls.store(0);
    ASSERT_TRUE(mudmux_init(nullptr));
    async_event_t event{};
    auto_reset_event = &event;
    ASSERT_TRUE(async_event_init(&event, false, false));
    ASSERT_TRUE(mudmux_register_event(&event, resignal_auto_reset_event));

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
        FAIL() << "auto-reset event did not re-signal mudmux_run";
    }
    EXPECT_EQ(run_result.get(), EXIT_SUCCESS);
    server_thread.join();
    EXPECT_EQ(auto_reset_event_hook_calls.load(), 2);
    async_event_destroy(&event);
    auto_reset_event = nullptr;
    mudmux_deinit();
}

namespace {

enum class coalescing_event_mode_t { Manual, Auto, Timer };

struct coalescing_event_context_t {
    async_queue_t* queue{nullptr};
    std::mutex mutex;
    std::condition_variable changed;
    int iterations{0};
    int calls{0};
    int drained{0};
    std::vector<int> messages;
    bool release{false};
};

int coalescing_event_callback(void* ctx, int msg, void*, size_t) {
    auto& state = *static_cast<coalescing_event_context_t*>(ctx);
    int item = 0;
    size_t size = 0;
    int drained = 0;
    while (async_queue_dequeue(state.queue, &item, sizeof(item), &size))
        ++drained;
    std::unique_lock<std::mutex> lock(state.mutex);
    state.drained += drained;
    state.messages.push_back(msg);
    ++state.calls;
    state.changed.notify_all();
    if (state.calls == 1)
        state.changed.wait(lock, [&] { return state.release; });
    return 0;
}

int coalescing_timer_callback(void* ctx, int msg, void* data, size_t size) {
    return msg == 0 || msg == -1 ? 0 : coalescing_event_callback(ctx, msg, data, size);
}

int coalescing_iteration_callback(void* ctx, int, void*, size_t) {
    auto& state = *static_cast<coalescing_event_context_t*>(ctx);
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.iterations;
    state.changed.notify_all();
    return 0;
}

void verify_relaxed_event_coalescing(coalescing_event_mode_t mode) {
    ASSERT_TRUE(mudmux_init("transport: {thread_pool: {size: 2}, keep_alive_interval: 1}"));
    coalescing_event_context_t state;
    state.queue = async_queue_create(32, sizeof(int), static_cast<async_queue_flags_t>(0));
    ASSERT_NE(state.queue, nullptr);
    ASSERT_TRUE(mudmux_register_hook(HOOK_GARBAGE_COLLECTION, coalescing_iteration_callback));
    async_event_t event{};
    const bool timer = mode == coalescing_event_mode_t::Timer;
    if (timer) {
        ASSERT_TRUE(mudmux_register_hook(HOOK_TIMER, coalescing_timer_callback));
    } else {
        ASSERT_TRUE(async_event_init(&event, mode == coalescing_event_mode_t::Manual, false));
        ASSERT_TRUE(mudmux_register_event(&event, coalescing_event_callback));
    }
    int run_result = EXIT_FAILURE;
    std::thread server([&] { run_result = mudmux_run(&state); });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        EXPECT_TRUE(state.changed.wait_for(lock, std::chrono::seconds(3),
                                          [&] { return state.iterations > 0; }));
    }
    int item = 0;
    EXPECT_TRUE(async_queue_enqueue(state.queue, &item, sizeof(item)));
    if (timer)
        EXPECT_TRUE(mudmux_trigger_timer(11));
    else
        async_event_set(&event);
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        EXPECT_TRUE(state.changed.wait_for(lock, std::chrono::seconds(3),
                                          [&] { return state.calls == 1; }));
    }
    for (int index = 0; index < 20; ++index) {
        EXPECT_TRUE(async_queue_enqueue(state.queue, &index, sizeof(index)));
        if (timer)
            EXPECT_TRUE(mudmux_trigger_timer(90 + index));
        else
            async_event_set(&event);
    }
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        // Let the loop consume readiness while the first callback is blocked.
        const int target = state.iterations + 2;
        EXPECT_TRUE(state.changed.wait_for(lock, std::chrono::seconds(3),
                                          [&] { return state.iterations >= target; }));
        EXPECT_EQ(state.calls, 1);
        state.release = true;
        state.changed.notify_all();
        EXPECT_TRUE(state.changed.wait_for(lock, std::chrono::seconds(3),
                                          [&] { return state.calls >= 2; }));
    }
    mudmux_shutdown();
    server.join();
    // Deinit joins workers before releasing the borrowed event/queue storage.
    const auto destroy_event = mudmux_async_api_v1->event_destroy;
    const auto destroy_queue = mudmux_async_api_v1->queue_destroy;
    mudmux_deinit();
    if (!timer)
        destroy_event(&event);
    destroy_queue(state.queue);
    EXPECT_EQ(run_result, EXIT_SUCCESS);
    EXPECT_EQ(state.calls, 2);
    EXPECT_EQ(state.drained, 21);
    ASSERT_EQ(state.messages.size(), 2u);
    EXPECT_EQ(state.messages[0], timer ? 11 : -1);
    EXPECT_EQ(state.messages[1], timer ? 109 : -1);
}

} // namespace

TEST(MudmuxTest, RelaxedManualResetEventCoalescesSignalsDuringHook) {
    verify_relaxed_event_coalescing(coalescing_event_mode_t::Manual);
}

TEST(MudmuxTest, RelaxedAutoResetEventCoalescesSignalsDuringHook) {
    verify_relaxed_event_coalescing(coalescing_event_mode_t::Auto);
}

TEST(MudmuxTest, RelaxedTimerRetainsLatestMessageDuringHook) {
    verify_relaxed_event_coalescing(coalescing_event_mode_t::Timer);
}

TEST(MudmuxTest, ShutdownJoinsCustomEventBeforeRunReturns) {
    ASSERT_TRUE(mudmux_init("transport: {thread_pool: {size: 2}}"));
    struct context_t {
        std::promise<void> started;
        std::promise<void> entered;
        std::shared_future<void> release;
        std::atomic<bool> runtime_running{false};
    } context;
    std::promise<void> release;
    context.release = release.get_future().share();
    auto started = context.started.get_future();
    auto entered = context.entered.get_future();
    ASSERT_TRUE(mudmux_register_hook(HOOK_TIMER, [](void* ctx, int msg, void*, size_t) {
        if (msg == 0)
            static_cast<context_t*>(ctx)->started.set_value();
        return 0;
    }));
    async_event_t event{};
    ASSERT_TRUE(async_event_init(&event, true, false));
    ASSERT_TRUE(mudmux_register_event(&event, [](void* ctx, int, void*, size_t) {
        auto& state = *static_cast<context_t*>(ctx);
        mudmux_shutdown();
        state.entered.set_value();
        state.release.wait();
        state.runtime_running.store(mudmux_is_running());
        mudmux_shutdown(); // The runtime must still support wakeups here.
        return 0;
    }));
    std::promise<int> result;
    auto done = result.get_future();
    std::thread server([&] { result.set_value(mudmux_run(&context)); });
    EXPECT_EQ(started.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    async_event_set(&event);
    EXPECT_EQ(entered.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(done.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    release.set_value();
    mudmux_shutdown(); // Also cleans up if event dispatch failed to start.
    EXPECT_EQ(done.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    server.join();
    EXPECT_EQ(done.get(), EXIT_SUCCESS);
    EXPECT_TRUE(context.runtime_running.load());
    EXPECT_EQ(mudmux_workers_pool_size(), 0u);
    const auto destroy_event = mudmux_async_api_v1->event_destroy;
    mudmux_deinit();
    destroy_event(&event);
}

namespace {

void verify_run_joins_detached_execution(bool initialization_failure) {
    ASSERT_TRUE(mudmux_init(initialization_failure
        ? "transport: {thread_pool: {size: 2}, accept: ['udp://127.0.0.1:0']}"
        : "transport: {thread_pool: {size: 2}}"));
    struct context_t {
        std::promise<void> started;
        std::promise<void> work_entered;
        std::promise<void> completion_entered;
        std::shared_future<void> release_work;
        std::shared_future<void> release_completion;
        std::atomic<bool> work_saw_running{false};
        std::atomic<bool> completion_saw_running{false};
        std::atomic<int> destroyed_while_running{0};
    } context;
    std::promise<void> release_work;
    std::promise<void> release_completion;
    context.release_work = release_work.get_future().share();
    context.release_completion = release_completion.get_future().share();
    auto started = context.started.get_future();
    auto work_entered = context.work_entered.get_future();
    auto completion_entered = context.completion_entered.get_future();
    ASSERT_TRUE(mudmux_register_hook(HOOK_TIMER, [](void* ctx, int msg, void*, size_t) {
        if (msg == 0)
            static_cast<context_t*>(ctx)->started.set_value();
        return 0;
    }));
    const async_closure_destroy_t destroy = [](void* ctx) {
        if (mudmux_is_running())
            static_cast<context_t*>(ctx)->destroyed_while_running.fetch_add(1);
    };
    async_closure_t work{[](void* ctx, int) {
        auto& state = *static_cast<context_t*>(ctx);
        state.work_entered.set_value();
        state.release_work.wait();
        state.work_saw_running.store(mudmux_is_running());
    }, destroy, &context};
    async_closure_t completion{[](void* ctx, int) {
        auto& state = *static_cast<context_t*>(ctx);
        state.completion_entered.set_value();
        state.release_completion.wait();
        state.completion_saw_running.store(mudmux_is_running());
        mudmux_shutdown();
    }, destroy, &context};
    ASSERT_TRUE(mudmux_workers_submit(&work, &completion));
    EXPECT_EQ(work_entered.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    std::promise<int> result;
    auto done = result.get_future();
    std::thread server([&] { result.set_value(mudmux_run(&context)); });
    if (!initialization_failure) {
        EXPECT_EQ(started.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        mudmux_shutdown();
    }
    EXPECT_EQ(done.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    release_work.set_value();
    EXPECT_EQ(completion_entered.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(done.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    release_completion.set_value();
    mudmux_shutdown();
    EXPECT_EQ(done.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    server.join();
    EXPECT_EQ(done.get(), initialization_failure ? EXIT_FAILURE : EXIT_SUCCESS);
    EXPECT_TRUE(context.work_saw_running.load());
    EXPECT_TRUE(context.completion_saw_running.load());
    EXPECT_EQ(context.destroyed_while_running.load(), 2);
    EXPECT_EQ(mudmux_workers_pool_size(), 0u);
    mudmux_deinit();
}

} // namespace

TEST(MudmuxTest, ShutdownJoinsDetachedWorkAndCompletionBeforeRunReturns) {
    verify_run_joins_detached_execution(false);
}

TEST(MudmuxTest, RuntimeInitializationFailureJoinsDetachedWorkAndCompletion) {
    verify_run_joins_detached_execution(true);
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

TEST(MudmuxTest, ShutdownDispatchesDisconnectForConnectedConsole) {
    shutdown_disconnect_hook_calls.store(0);
    ASSERT_TRUE(mudmux_init(
        "{\"transport\": {\"console\": true, \"thread_pool\": {\"size\": 2}}}"));
    ASSERT_TRUE(mudmux_register_hook(HOOK_DISCONNECT, count_shutdown_disconnect));

    std::thread server_thread([]() {
        EXPECT_EQ(mudmux_run(nullptr), EXIT_SUCCESS);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mudmux_shutdown();
    server_thread.join();

    EXPECT_EQ(shutdown_disconnect_hook_calls.load(), 1);
    mudmux_deinit();
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
