#pragma once

#include "gnfs/util/cpu_intrin.hpp"

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

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace gnfs::util {

/// P3-1 / doctrine §7.2 第 3 条 — QoS class for thread scheduling.
/// macOS: 调度 hint 到 P-core (UserInitiated) / E-core (Background).
/// Linux: no-op (内核调度自由).
enum class QoSClass {
    UserInitiated, ///< 性能优先, hint P-core. **基准/perf 默认**.
    Utility,       ///< 中等优先, 允许 E-core (节能).
    Background,    ///< 后台优先, 常驻 E-core.
    Unspecified,   ///< 不设置, 系统默认 (legacy 行为).
};

/// 设置当前线程 QoS class (静态 helper, main thread + worker 都用).
/// macOS: 调用 pthread_set_qos_class_self_np.
/// Linux: no-op.
inline void set_current_thread_qos(QoSClass qos) noexcept {
#if defined(__APPLE__)
    qos_class_t cls;
    switch (qos) {
    case QoSClass::UserInitiated:
        cls = QOS_CLASS_USER_INITIATED;
        break;
    case QoSClass::Utility:
        cls = QOS_CLASS_UTILITY;
        break;
    case QoSClass::Background:
        cls = QOS_CLASS_BACKGROUND;
        break;
    case QoSClass::Unspecified:
        return;
    }
    pthread_set_qos_class_self_np(cls, 0);
#else
    (void)qos;
#endif
}

/// 高性能线程池
class ThreadPool {
public:
    /// 构造函数
    /// @param num_threads 线程数量，0 表示使用硬件并发数
    /// @param qos worker thread QoS class (macOS only, Linux no-op).
    ///            默认 UserInitiated — hint scheduler 优先 P-core.
    ///            doctrine §7.2 第 3 条: 基准用 P-core 强制.
    explicit ThreadPool(uint32_t num_threads = 0, QoSClass qos = QoSClass::UserInitiated)
        : qos_(qos), stop_(false), pending_(0), queue_size_(0) {
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) {
                num_threads = 4; // 默认值
            }
        }

        workers_.reserve(num_threads);
        try {
            for (uint32_t i = 0; i < num_threads; ++i) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            // A partially constructed vector of joinable std::threads would
            // call std::terminate while unwinding this constructor. Stop and
            // join every worker that was created before propagating the
            // resource failure.
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
            throw;
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

    /// P1.B-1c: spin budget for worker idle-wait — short busy-wait before falling
    /// back to cv_wait. M5 P-core ~4.6 GHz, `yield` ≈ 1-2 ns/iter → 2000 ≈ 2-4 μs.
    /// Covers burst-submit patterns (Gaussian col-by-col, SpMV per-iter chunks)
    /// where next wave arrives within μs of wait_all.
    static constexpr int kSpinBudget = 2000;

    // 禁止拷贝和移动
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// 提交任务
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> result = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) {
                throw std::runtime_error("submit on stopped ThreadPool");
            }
            tasks_.emplace([task]() { (*task)(); });
            ++pending_;
            queue_size_.fetch_add(1, std::memory_order_release);
        }
        cv_.notify_one();

        return result;
    }

    /// 并行 for 循环
    /// @param begin 起始迭代器
    /// @param end 结束迭代器
    /// @param func 对每个元素执行的函数
    template <typename Iter, typename Func> void parallel_for(Iter begin, Iter end, Func&& func) {
        auto distance = std::distance(begin, end);
        if (distance <= 0)
            return;

        size_t num_threads = workers_.size();
        size_t total = static_cast<size_t>(distance);
        size_t chunk_size = (total + num_threads - 1) / num_threads;

        std::vector<std::future<void>> futures;
        futures.reserve(num_threads);

        Iter chunk_begin = begin;
        for (size_t i = 0; i < num_threads && chunk_begin != end; ++i) {
            Iter chunk_end = chunk_begin;
            size_t remaining = static_cast<size_t>(std::distance(chunk_begin, end));
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
    template <typename Func> void parallel_for_index(size_t start, size_t end, Func&& func) {
        if (start >= end)
            return;

        size_t distance = end - start;
        size_t num_threads = workers_.size();
        size_t chunk_size = (distance + num_threads - 1) / num_threads;

        std::vector<std::future<void>> futures;
        futures.reserve(num_threads);

        for (size_t i = 0; i < num_threads; ++i) {
            size_t chunk_start = start + i * chunk_size;
            size_t chunk_end = std::min(chunk_start + chunk_size, end);

            if (chunk_start >= end)
                break;

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

    /// Work-stealing parallel for: dynamic task assignment via atomic counter.
    /// Instead of static chunks, each thread grabs the next small chunk atomically.
    /// Automatically balances load when per-index work varies (e.g., SQ sieving).
    /// @param start  Start index (inclusive)
    /// @param end    End index (exclusive)
    /// @param func   Callable taking size_t index
    /// @param grain  Chunk granularity (default: 1 for finest balancing)
    template <typename Func>
    void parallel_for_stealing(size_t start, size_t end, Func&& func, size_t grain = 1) {
        if (start >= end)
            return;
        if (grain == 0)
            grain = 1;

        std::atomic<size_t> next_idx{start};

        size_t num_threads = workers_.size();
        std::vector<std::future<void>> futures;
        futures.reserve(num_threads);

        for (size_t t = 0; t < num_threads; ++t) {
            futures.push_back(submit([&next_idx, end, grain, &func]() {
                while (true) {
                    size_t chunk_start = next_idx.load(std::memory_order_relaxed);
                    bool claimed = false;
                    while (chunk_start < end) {
                        // Claim no more than the remaining range. Besides
                        // avoiding an extra partial chunk, this keeps the
                        // cursor from wrapping when end == SIZE_MAX.
                        const size_t remaining = end - chunk_start;
                        const size_t chunk_len = std::min(grain, remaining);
                        if (next_idx.compare_exchange_weak(chunk_start, chunk_start + chunk_len,
                                                           std::memory_order_relaxed,
                                                           std::memory_order_relaxed)) {
                            const size_t chunk_end = chunk_start + chunk_len;
                            for (size_t i = chunk_start; i < chunk_end; ++i) {
                                func(i);
                            }
                            claimed = true;
                            break;
                        }
                    }
                    if (!claimed)
                        break;
                }
            }));
        }

        for (auto& f : futures) {
            f.get();
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
    [[nodiscard]] size_t pending_tasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_;
    }

private:
    /// Hint to the CPU we are in a spin loop — on ARM `yield` lowers SMT priority
    /// (M5 P-core has no SMT but the instruction still puts the pipeline in a
    /// low-power hint state). On x86 emit `pause`. Delegates to the
    /// cross-compiler wrapper in `gnfs/util/cpu_intrin.hpp` so MSVC builds
    /// pick up the `_mm_pause`/`__yield` intrinsics path instead of GCC inline
    /// assembly which MSVC does not accept. The non-arm/non-x86 fallback path
    /// uses `std::this_thread::yield()` here (heavier than the no-op in
    /// `cpu_pause`) because spin-then-cv worker loops benefit from the
    /// scheduler hint when the underlying CPU has neither pause nor yield.
    static inline void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(__arm__) || defined(__x86_64__) || defined(__i386__) ||        \
    defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64) || defined(_M_ARM)
        ::gnfs::util::cpu_pause();
#else
        std::this_thread::yield();
#endif
    }

    void worker_loop() {
        // P3-1: 每个 worker thread 启动时 set QoS class. macOS hint scheduler
        // 优先 P-core; Linux no-op. doctrine §7.2 第 3 条.
        set_current_thread_qos(qos_);

        // P1.B-1c: spin-then-cv. Worker spins atomic-load on queue_size_ for a
        // short budget before falling back to cv_.wait. Covers Gaussian/SpMV
        // burst-submit (next wave arrives in μs of wait_all) without burning
        // CPU for long-idle periods.
        int spin = 0;
        while (true) {
            // Fast path: atomic peek — if work is available, lock+grab.
            if (queue_size_.load(std::memory_order_acquire) > 0) {
                std::function<void()> task;
                bool got_task = false;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (!tasks_.empty()) {
                        task = std::move(tasks_.front());
                        tasks_.pop();
                        queue_size_.fetch_sub(1, std::memory_order_release);
                        got_task = true;
                    }
                }
                if (got_task) {
                    task();
                    bool should_notify = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        should_notify = (--pending_ == 0);
                    }
                    if (should_notify) {
                        done_cv_.notify_all();
                    }
                    spin = 0;
                    continue;
                }
                // Another worker stole it — fall through to spin/cv path.
            }

            // No work seen. Short spin before paying cv_wait syscall.
            if (spin < kSpinBudget) {
                cpu_relax();
                ++spin;
                continue;
            }

            // Spin budget exhausted — long idle, fall back to cv_wait.
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                if (stop_ && tasks_.empty()) {
                    return;
                }

                task = std::move(tasks_.front());
                tasks_.pop();
                queue_size_.fetch_sub(1, std::memory_order_release);
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
            spin = 0;
        }
    }

    // P3-1: QoS class for worker threads (set in worker_loop entry).
    // 放在最前 const-after-ctor, 但因构造函数 init list 顺序限制需在 stop_ 之前.
    QoSClass qos_;

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable done_cv_;

    bool stop_;
    // pending_ is always read/written under mutex_ — even pending_tasks() locks.
    // 不用 atomic 是为了让心智模型一致:所有同步状态都走 mutex_/cv,不混 atomic 语义。
    size_t pending_;

    // P1.B-1c: atomic view of tasks_.size() — worker spin path reads without lock.
    // Always updated together with tasks_ under mutex_; release/acquire seqs
    // ensure spin-path readers see consistent task availability without lock.
    std::atomic<size_t> queue_size_;
};

} // namespace gnfs::util
