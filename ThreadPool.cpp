#include "ThreadPool.h"

#include <stdexcept>
#include <utility>

ThreadPool::ThreadPool(std::size_t workerCount) {
    if (workerCount == 0) {
        throw std::invalid_argument("A thread pool needs at least one worker.");
    }

    workers_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back(&ThreadPool::workerLoop, this);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::enqueue(std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("Cannot enqueue an empty task.");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("Cannot enqueue a task while the pool is stopping.");
        }
        tasks_.push(std::move(task));
    }
    condition_.notify_one();
}

void ThreadPool::waitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idleCondition_.wait(lock, [this] {
        return tasks_.empty() && activeTasks_ == 0;
    });
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });

            if (stopping_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
            ++activeTasks_;
        }

        task();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --activeTasks_;
            if (tasks_.empty() && activeTasks_ == 0) {
                idleCondition_.notify_all();
            }
        }
    }
}
