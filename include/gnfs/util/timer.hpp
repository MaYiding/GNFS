#pragma once

#include <chrono>

namespace gnfs::util {

/// 高精度计时器
class Timer {
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::nanoseconds;

    /// 构造函数
    Timer() noexcept : accumulated_(0), running_(false) {}

    /// 开始计时
    void start() noexcept {
        if (!running_) {
            start_ = Clock::now();
            running_ = true;
        }
    }

    /// 停止计时
    void stop() noexcept {
        if (running_) {
            auto end = Clock::now();
            accumulated_ += std::chrono::duration_cast<Duration>(end - start_);
            running_ = false;
        }
    }

    /// 重置计时器
    void reset() noexcept {
        accumulated_ = Duration(0);
        running_ = false;
    }

    /// 重新开始（reset + start）
    void restart() noexcept {
        reset();
        start();
    }

    /// 获取经过的秒数
    [[nodiscard]] double elapsed_seconds() const noexcept {
        auto total = accumulated_;
        if (running_) {
            total += std::chrono::duration_cast<Duration>(Clock::now() - start_);
        }
        return std::chrono::duration<double>(total).count();
    }

    /// 获取经过的毫秒数
    [[nodiscard]] double elapsed_ms() const noexcept {
        return elapsed_seconds() * 1000.0;
    }

    /// 获取经过的微秒数
    [[nodiscard]] double elapsed_us() const noexcept {
        return elapsed_seconds() * 1000000.0;
    }

    /// 获取经过的纳秒数
    [[nodiscard]] int64_t elapsed_ns() const noexcept {
        auto total = accumulated_;
        if (running_) {
            total += std::chrono::duration_cast<Duration>(Clock::now() - start_);
        }
        return total.count();
    }

    /// 是否正在运行
    [[nodiscard]] bool is_running() const noexcept {
        return running_;
    }

    /// RAII 计时辅助类 - 使用方式: Timer::ScopedTimer guard(timer);
    class ScopedTimer {
    public:
        explicit ScopedTimer(Timer& timer) noexcept : timer_(&timer), active_(true) {
            timer_->start();
        }

        ~ScopedTimer() {
            if (active_ && timer_) {
                timer_->stop();
            }
        }

        // 禁止拷贝
        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;

        // 允许移动
        ScopedTimer(ScopedTimer&& other) noexcept
            : timer_(other.timer_), active_(other.active_) {
            other.active_ = false;
        }

        ScopedTimer& operator=(ScopedTimer&& other) noexcept {
            if (this != &other) {
                if (active_ && timer_) {
                    timer_->stop();
                }
                timer_ = other.timer_;
                active_ = other.active_;
                other.active_ = false;
            }
            return *this;
        }

    private:
        Timer* timer_;
        bool active_;
    };

    /// 创建 RAII 计时器
    [[nodiscard]] ScopedTimer scoped() noexcept {
        return ScopedTimer(*this);
    }

private:
    TimePoint start_;
    Duration accumulated_;
    bool running_;
};

/// 一次性计时器 - 构造时开始，elapsed() 返回从构造开始的时间
class Stopwatch {
public:
    using Clock = std::chrono::high_resolution_clock;

    Stopwatch() noexcept : start_(Clock::now()) {}

    /// 重新开始
    void restart() noexcept {
        start_ = Clock::now();
    }

    /// 获取经过的秒数
    [[nodiscard]] double elapsed_seconds() const noexcept {
        auto now = Clock::now();
        return std::chrono::duration<double>(now - start_).count();
    }

    /// 获取经过的毫秒数
    [[nodiscard]] double elapsed_ms() const noexcept {
        return elapsed_seconds() * 1000.0;
    }

private:
    Clock::time_point start_;
};

} // namespace gnfs::util
