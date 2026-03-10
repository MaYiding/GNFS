#include "gnfs/util/thread_pool.hpp"

namespace gnfs::util {

ThreadPool::ThreadPool(size_t num_threads) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    condition_.wait(lock, [this] {
                        return stop_ || !tasks_.empty();
                    });

                    if (stop_ && tasks_.empty()) {
                        return;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }

                ++active_tasks_;
                task();
                --active_tasks_;
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::wait() {
    while (true) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (tasks_.empty() && active_tasks_ == 0) {
            break;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace gnfs::util
