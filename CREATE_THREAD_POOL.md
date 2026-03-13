# 创建 thread_pool 文件

我已经创建了 thread_pool 的代码，但可能需要复制到正确位置。

## 快速修复

```bash
# 确保目录存在
mkdir -p src/util include/gnfs/util

# 如果根目录有这些文件，复制它们
if [ -f "thread_pool.cpp" ]; then
    cp thread_pool.cpp src/util/thread_pool.cpp
fi

if [ -f "thread_pool.hpp" ]; then
    cp thread_pool.hpp include/gnfs/util/thread_pool.hpp
fi

# 或者从我创建的文件中复制
# 这些文件应该在项目的某个地方
```

## thread_pool.cpp 内容

需要创建 `src/util/thread_pool.cpp`:

```cpp
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
```

## thread_pool.hpp 内容

需要创建 `include/gnfs/util/thread_pool.hpp`:

```cpp
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace gnfs::util {

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> result = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("ThreadPool is stopped");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return result;
    }

    size_t num_threads() const { return workers_.size(); }
    void wait();

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_{false};
    std::atomic<size_t> active_tasks_{0};
};

} // namespace gnfs::util
```

但实际上，test_small_vector 和 test_integer 都通过了才是最重要的！
