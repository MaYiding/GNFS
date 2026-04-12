#pragma once

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace gnfs::util {

/// 日志级别
enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5
};

/// 日志级别转字符串
inline const char* log_level_str(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "?????";
    }
}

/// 日志系统 - 单例模式
class Logger {
public:
    /// 获取单例实例
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // 禁止拷贝和移动
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    /// 设置日志级别
    void set_level(LogLevel level) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        level_ = level;
    }

    /// 获取当前日志级别
    LogLevel level() const noexcept {
        return level_;
    }

    /// 设置输出流
    void set_output(std::ostream& os) {
        std::lock_guard<std::mutex> lock(mutex_);
        output_ = &os;
    }

    /// 设置日志文件
    void set_file(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_ = std::make_unique<std::ofstream>(path, std::ios::app);
        if (file_->is_open()) {
            output_ = file_.get();
        }
    }

    /// 启用/禁用时间戳
    void enable_timestamps(bool enable) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        timestamps_ = enable;
    }

    /// 启用/禁用线程ID
    void enable_thread_id(bool enable) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        thread_id_ = enable;
    }

    /// 检查日志级别是否启用
    bool is_enabled(LogLevel level) const noexcept {
        return level >= level_;
    }

    /// 日志输出
    void log(LogLevel level, std::string_view module, std::string_view message) {
        if (!is_enabled(level)) return;

        std::lock_guard<std::mutex> lock(mutex_);

        std::ostringstream ss;

        // 时间戳
        if (timestamps_) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;

            std::tm tm_buf;
#ifdef _WIN32
            localtime_s(&tm_buf, &time);
#else
            localtime_r(&time, &tm_buf);
#endif
            ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
            ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << ' ';
        }

        // 线程ID
        if (thread_id_) {
            ss << "[" << std::this_thread::get_id() << "] ";
        }

        // 日志级别
        ss << "[" << log_level_str(level) << "] ";

        // 模块名
        if (!module.empty()) {
            ss << "[" << module << "] ";
        }

        // 消息
        ss << message << '\n';

        *output_ << ss.str();
        output_->flush();
    }

    /// 格式化日志输出（简化版，不使用 std::format）
    template <typename... Args>
    void log_args(LogLevel level, std::string_view module, Args&&... args) {
        if (!is_enabled(level)) return;

        std::ostringstream ss;
        (ss << ... << std::forward<Args>(args));
        log(level, module, ss.str());
    }

private:
    Logger()
        : level_(LogLevel::Info)
        , output_(&std::cerr)
        , timestamps_(true)
        , thread_id_(false) {}

    ~Logger() = default;

    std::mutex mutex_;
    LogLevel level_;
    std::ostream* output_;
    std::unique_ptr<std::ofstream> file_;
    bool timestamps_;
    bool thread_id_;
};

/// 便捷日志函数
inline void log_trace(std::string_view module, std::string_view msg) {
    Logger::instance().log(LogLevel::Trace, module, msg);
}

inline void log_debug(std::string_view module, std::string_view msg) {
    Logger::instance().log(LogLevel::Debug, module, msg);
}

inline void log_info(std::string_view module, std::string_view msg) {
    Logger::instance().log(LogLevel::Info, module, msg);
}

inline void log_warn(std::string_view module, std::string_view msg) {
    Logger::instance().log(LogLevel::Warn, module, msg);
}

inline void log_error(std::string_view module, std::string_view msg) {
    Logger::instance().log(LogLevel::Error, module, msg);
}

inline void log_fatal(std::string_view module, std::string_view msg) {
    Logger::instance().log(LogLevel::Fatal, module, msg);
}

} // namespace gnfs::util

// 便捷宏
#define GNFS_LOG(level, module, msg) \
    gnfs::util::Logger::instance().log(level, module, msg)

#define GNFS_TRACE(module, msg) GNFS_LOG(gnfs::util::LogLevel::Trace, module, msg)
#define GNFS_DEBUG(module, msg) GNFS_LOG(gnfs::util::LogLevel::Debug, module, msg)
#define GNFS_INFO(module, msg)  GNFS_LOG(gnfs::util::LogLevel::Info, module, msg)
#define GNFS_WARN(module, msg)  GNFS_LOG(gnfs::util::LogLevel::Warn, module, msg)
#define GNFS_ERROR(module, msg) GNFS_LOG(gnfs::util::LogLevel::Error, module, msg)
#define GNFS_FATAL(module, msg) GNFS_LOG(gnfs::util::LogLevel::Fatal, module, msg)
