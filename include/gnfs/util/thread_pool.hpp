#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace gnfs {
namespace util {

/// 高性能线程池
class ThreadPool {
public:
    /// 构造函数
    /// @param num_threads 线程数量，0 表示使用硬件并发数
    explicit ThreadPool(uint32_t num_threads = 0)
        : stop_(false), pending_(0) {
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) {
                num_threads = 4;  // 默认值
            }
        }

        workers_.reserve(num_threads);
        for (uint32_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    /// 析构函数 - 等待所有任务完成
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    // 禁止拷贝和移动
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// 提交任务
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> result = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) {
                throw std::runtime_error("submit on stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
            ++pending_;
        }
        cv_.notify_one();

        return result;
    }

    /// 并行 for 循环
    /// @param begin 起始迭代器
    /// @param end 结束迭代器
    /// @param func 对每个元素执行的函数
    template <typename Iter, typename Func>
    void parallel_for(Iter begin, Iter end, Func&& func) {
        auto distance = std::distance(begin, end);
        if (distance <= 0) return;

        size_t num_threads = workers_.size();
        size_t chunk_size = (distance + num_threads - 1) / num_threads;

        std::vector<std::future<void>> futures;
        futures.reserve(num_threads);

        Iter chunk_begin = begin;
        for (size_t i = 0; i < num_threads && chunk_begin != end; ++i) {
            Iter chunk_end = chunk_begin;
            size_t remaining = std::distance(chunk_begin, end);
            std::advance(chunk_end, std::min(chunk_size, remaining));

            futures.push_back(submit([chunk_begin, chunk_end, &func]() {
                for (Iter it = chunk_begin; it != chunk_end; ++it) {
                    func(*it);
                }
            }));

            chunk_begin = chunk_end;
        }

        // 等待所有块完成
        for (auto& future : futures) {
            future.get();
        }
    }

    /// 并行 for 循环（带索引版本）
    /// @param start 起始索引
    /// @param end 结束索引（不包含）
    /// @param func 函数，接受索引参数
    template <typename Func>
    void parallel_for_index(size_t start, size_t end, Func&& func) {
        if (start >= end) return;

        size_t distance = end - start;
        size_t num_threads = workers_.size();
        size_t chunk_size = (distance + num_threads - 1) / num_threads;

        std::vector<std::future<void>> futures;
        futures.reserve(num_threads);

        for (size_t i = 0; i < num_threads; ++i) {
            size_t chunk_start = start + i * chunk_size;
            size_t chunk_end = std::min(chunk_start + chunk_size, end);

            if (chunk_start >= end) break;

            futures.push_back(submit([chunk_start, chunk_end, &func]() {
                for (size_t j = chunk_start; j < chunk_end; ++j) {
                    func(j);
                }
            }));
        }

        for (auto& future : futures) {
            future.get();
        }
    }

    /// 等待所有任务完成
    void wait_all() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this] { return pending_ == 0; });
    }

    /// 获取线程数量
    [[nodiscard]] uint32_t num_threads() const noexcept {
        return static_cast<uint32_t>(workers_.size());
    }

    /// 获取待处理任务数量
    [[nodiscard]] size_t pending_tasks() const noexcept {
        return pending_.load();
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                if (stop_ && tasks_.empty()) {
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
            }

            task();

            // 任务完成，减少计数
            // 必须在 mutex 下递减，与 wait_all() 的谓词检查同步。
            // 否则 done_cv_.notify_all() 可能在 wait_all() 阻塞前触发，
            // 导致通知丢失 → 死锁。
            bool should_notify = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                should_notify = (--pending_ == 0);
            }
            if (should_notify) {
                done_cv_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable done_cv_;

    bool stop_;
    std::atomic<size_t> pending_;
};

} // namespace util
} // namespace gnfs
