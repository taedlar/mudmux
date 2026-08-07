#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#endif

#include "async/async_event.h"
#include "async/async_queue.h"
#include "async/async_runtime.h"
#include "async/thread_pool.hpp"

namespace {

#ifndef _WIN32
bool is_fd_readable(int fd) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    return poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0;
}

TEST(AsyncEventTest, ManualResetStaysReadableUntilReset) {
    async_event_t event;
    async_wait_handle_t handle;

    ASSERT_TRUE(async_event_init(&event, true, false));
    handle = async_event_get_wait_handle(&event);
    EXPECT_NE(handle, ASYNC_INVALID_WAIT_HANDLE);
    EXPECT_FALSE(is_fd_readable(handle));

    async_event_set(&event);
    EXPECT_TRUE(is_fd_readable(handle));
    EXPECT_TRUE(async_event_wait(&event, 0));
    EXPECT_TRUE(is_fd_readable(handle));

    async_event_reset(&event);
    EXPECT_FALSE(is_fd_readable(handle));

    async_event_destroy(&event);
}

TEST(AsyncEventTest, AutoResetConsumesReadableState) {
    async_event_t event;
    async_wait_handle_t handle;

    ASSERT_TRUE(async_event_init(&event, false, false));
    handle = async_event_get_wait_handle(&event);
    async_event_set(&event);

    EXPECT_TRUE(is_fd_readable(handle));
    EXPECT_TRUE(async_event_wait(&event, 0));
    EXPECT_FALSE(is_fd_readable(handle));
    EXPECT_FALSE(async_event_wait(&event, 0));

    async_event_destroy(&event);
}
#endif

TEST(AsyncRuntimeTest, ManualResetEventRemainsSignaledUntilReset) {
    async_runtime_t* runtime = async_runtime_init(nullptr);
    ASSERT_NE(runtime, nullptr);

    async_event_t event{};
    ASSERT_TRUE(async_event_init(&event, true, false));
    ASSERT_EQ(async_runtime_add_event(runtime, &event, &event), 0);

    async_event_set(&event);

    io_event_t events[1]{};
    timeval timeout{0, 500000};
    ASSERT_EQ(async_runtime_wait(runtime, events, 1, &timeout), 1);
    EXPECT_EQ(events[0].context, &event);
    EXPECT_EQ(events[0].event_type, EVENT_READ);

    ASSERT_EQ(async_runtime_wait(runtime, events, 1, &timeout), 1);
    EXPECT_EQ(events[0].context, &event);
    EXPECT_EQ(events[0].event_type, EVENT_READ);

    async_event_reset(&event);
    timeval no_wait{0, 0};
    EXPECT_EQ(async_runtime_wait(runtime, events, 1, &no_wait), 0);

    async_runtime_deinit(runtime);
    async_event_destroy(&event);
}

TEST(AsyncRuntimeTest, AutoResetEventIsConsumedByOneDelivery) {
    async_runtime_t* runtime = async_runtime_init(nullptr);
    ASSERT_NE(runtime, nullptr);

    async_event_t event{};
    ASSERT_TRUE(async_event_init(&event, false, false));
    ASSERT_EQ(async_runtime_add_event(runtime, &event, &event), 0);

    async_event_set(&event);

    io_event_t events[1]{};
    timeval timeout{0, 500000};
    ASSERT_EQ(async_runtime_wait(runtime, events, 1, &timeout), 1);
    EXPECT_EQ(events[0].context, &event);
    EXPECT_EQ(events[0].event_type, EVENT_READ);

    timeval no_wait{0, 0};
    EXPECT_EQ(async_runtime_wait(runtime, events, 1, &no_wait), 0);

    async_runtime_deinit(runtime);
    async_event_destroy(&event);
}

TEST(AsyncQueueTest, BlockingWriterResumesAfterDequeue) {
    async_queue_t* queue = async_queue_create(1, 32, ASYNC_QUEUE_BLOCK_WRITER);
    ASSERT_NE(queue, nullptr);

    const char first[] = "first";
    const char second[] = "second";
    ASSERT_TRUE(async_queue_enqueue(queue, first, sizeof(first)));

    std::promise<bool> enqueue_result;
    std::future<bool> enqueue_future = enqueue_result.get_future();
    std::thread writer([&queue, &enqueue_result, &second] {
        enqueue_result.set_value(async_queue_enqueue(queue, second, sizeof(second)));
    });

    EXPECT_EQ(enqueue_future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);

    char buffer[32];
    size_t size = 0;
    ASSERT_TRUE(async_queue_dequeue(queue, buffer, sizeof(buffer), &size));
    EXPECT_EQ(size, sizeof(first));

    EXPECT_EQ(enqueue_future.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
    EXPECT_TRUE(enqueue_future.get());

    ASSERT_TRUE(async_queue_dequeue(queue, buffer, sizeof(buffer), &size));
    EXPECT_STREQ(buffer, second);

    writer.join();
    async_queue_destroy(queue);
}

TEST(AsyncThreadPoolTest, SubmittedTasksExecute) {
    async_thread_pool_t pool;
    std::atomic<int> completed{0};
    std::promise<void> finished;
    std::future<void> finished_future = finished.get_future();

    ASSERT_TRUE(pool.start(2));
    ASSERT_EQ(pool.size(), 2u);

    ASSERT_TRUE(pool.submit([&completed] {
        ++completed;
    }));
    ASSERT_TRUE(pool.submit([&completed, &finished] {
        if (++completed == 2)
            finished.set_value();
    }));

    EXPECT_EQ(finished_future.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
    EXPECT_EQ(completed.load(), 2);

    pool.stop();
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_FALSE(pool.submit([] {}));
}

TEST(AsyncThreadPoolTest, WorkerSurvivesThrowingTask) {
    async_thread_pool_t pool;
    std::promise<void> finished;
    std::future<void> finished_future = finished.get_future();

    ASSERT_TRUE(pool.start(1));
    ASSERT_TRUE(pool.submit([] {
        throw std::runtime_error("expected test exception");
    }));
    ASSERT_TRUE(pool.submit([&finished] {
        finished.set_value();
    }));

    EXPECT_EQ(finished_future.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
    EXPECT_EQ(pool.size(), 1u);
    pool.stop();
}

} // namespace
