#ifndef MUDMUX_ASYNC_THREAD_POOL_HPP
#define MUDMUX_ASYNC_THREAD_POOL_HPP

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class async_thread_pool_t {
public:
    async_thread_pool_t() = default;
    ~async_thread_pool_t();

    async_thread_pool_t(const async_thread_pool_t&) = delete;
    async_thread_pool_t& operator=(const async_thread_pool_t&) = delete;

    bool start(std::size_t worker_count);
    bool submit(std::function<void()> task);
    void stop();
    std::size_t size() const;

private:
    void worker_loop();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};

#endif