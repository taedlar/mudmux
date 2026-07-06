#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#endif

#include "async/async_event.h"
#include "async/async_queue.h"

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

} // namespace