#include "thread_pool.hpp"

#include <utility>

async_thread_pool_t::~async_thread_pool_t() {
    stop();
}

bool async_thread_pool_t::start(std::size_t worker_count) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (worker_count == 0 || !workers_.empty())
        return false;

    stopping_ = false;
    try {
        workers_.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index)
            workers_.emplace_back(&async_thread_pool_t::worker_loop, this);
    }
    catch (...) {
        stopping_ = true;
        condition_.notify_all();
        lock.unlock();
        stop();
        return false;
    }
    return true;
}

bool async_thread_pool_t::submit(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || workers_.empty())
        return false;

    tasks_.push(std::move(task));
    condition_.notify_one();
    return true;
}

void async_thread_pool_t::stop() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (workers_.empty()) {
            stopping_ = true;
            return;
        }

        stopping_ = true;
        workers.swap(workers_);
    }

    condition_.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable())
            worker.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    while (!tasks_.empty())
        tasks_.pop();
}

std::size_t async_thread_pool_t::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.size();
}

void async_thread_pool_t::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                if (stopping_)
                    return;
                continue;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        task();
    }
}