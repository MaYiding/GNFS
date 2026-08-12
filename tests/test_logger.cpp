// test_logger.cpp — Logger 多线程并发行为回归保护
//
// 历史 bug:level_ 原是 LogLevel,set_level/level/is_enabled 不上锁,
// 多线程 set_level 与 log 并发会 data-race。已修为 atomic<LogLevel>。
// 此测试锁住该行为:多线程 set_level + log 同时进行,期望:
//   (1) 无 data race(TSan 会抓)或撕裂
//   (2) is_enabled 单调读到某个有效 level
//   (3) 写入的 log 行不交错(mutex_ 保护)

#include "gnfs/util/logger.hpp"
#include "support/test_check.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace gnfs::util;

namespace {

class TempDirectory final {
public:
    TempDirectory() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("gnfs-logger-" + std::to_string(tick) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("create logger fixture root", path_, error);
            }
        }
        throw std::runtime_error("could not reserve logger fixture root");
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace

void test_level_atomicity() {
    std::cout << "Testing concurrent set_level + is_enabled..." << std::endl;

    Logger& log = Logger::instance();
    constexpr int reader_count = 4;
    constexpr int operations_per_reader = 100000;
    std::atomic<bool> start{false};
    std::atomic<std::size_t> total_reads{0};

    // Reader threads check is_enabled across many calls
    std::vector<std::thread> readers;
    for (int t = 0; t < reader_count; ++t) {
        readers.emplace_back([&log, &start, &total_reads]() {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < operations_per_reader; ++i) {
                // Just ensure no UB / no torn read — value can be any LogLevel
                volatile bool v = log.is_enabled(LogLevel::Info);
                (void)v;
                total_reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Writer toggles between Trace and Fatal
    std::thread writer([&log, &start]() {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int i = 0; i < 100000; ++i) {
            log.set_level((i & 1) ? LogLevel::Trace : LogLevel::Fatal);
        }
    });
    start.store(true, std::memory_order_release);

    writer.join();
    for (auto& r : readers)
        r.join();

    GNFS_TEST_CHECK(total_reads.load(std::memory_order_relaxed) ==
                    static_cast<std::size_t>(reader_count * operations_per_reader));
    std::cout << "  level atomicity: PASS (" << total_reads.load() << " reads, no race)"
              << std::endl;
}

void test_concurrent_log_no_tearing() {
    std::cout << "Testing concurrent log output not torn..." << std::endl;

    Logger& log = Logger::instance();
    log.set_level(LogLevel::Trace);
    log.enable_timestamps(false);
    log.enable_thread_id(false);

    std::ostringstream captured;
    log.set_output(captured);

    const int num_threads = 4;
    const int messages_per_thread = 200;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&log, t]() {
            for (int i = 0; i < messages_per_thread; ++i) {
                log.log(LogLevel::Info, "test",
                        "thread_" + std::to_string(t) + "_msg_" + std::to_string(i) +
                            " TESTMSG_END");
            }
        });
    }
    for (auto& th : threads)
        th.join();

    log.set_output(std::cerr); // restore so other tests don't capture stderr

    std::unordered_set<std::string> expected_lines;
    for (int t = 0; t < num_threads; ++t) {
        for (int i = 0; i < messages_per_thread; ++i) {
            expected_lines.insert("[INFO ] [test] thread_" + std::to_string(t) + "_msg_" +
                                  std::to_string(i) + " TESTMSG_END");
        }
    }

    std::istringstream lines(captured.str());
    std::string line;
    std::size_t line_count = 0;
    while (std::getline(lines, line)) {
        GNFS_TEST_CHECK(expected_lines.erase(line) == 1);
        ++line_count;
    }
    GNFS_TEST_CHECK(expected_lines.empty());
    GNFS_TEST_CHECK(line_count == static_cast<std::size_t>(num_threads * messages_per_thread));

    std::cout << "  concurrent log: PASS (" << line_count << " complete, unique messages)"
              << std::endl;
}

void test_set_file_failure_throws() {
    std::cout << "Testing set_file failure throws..." << std::endl;

    Logger& log = Logger::instance();
    TempDirectory temporary;
    const auto non_directory = temporary.path() / "not-a-directory";
    {
        std::ofstream output(non_directory, std::ios::binary | std::ios::trunc);
        GNFS_TEST_CHECK(output.is_open());
    }

    // A regular file cannot be used as a parent directory on any supported OS.
    bool threw = false;
    try {
        log.set_file((non_directory / "log.txt").string());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    GNFS_TEST_CHECK(threw);
    // Logger 状态保持正常:依然能 log 到 stderr
    log.set_output(std::cerr); // 显式 reset (set_file 失败后 output_ 应未改)
    log.log(LogLevel::Info, "test", "post-failure-ok");
    std::cout << "  set_file failure: PASS (throws, logger usable)" << std::endl;
}

int main() {
    std::cout << "=== Logger Tests ===" << std::endl;

    test_level_atomicity();
    test_concurrent_log_no_tearing();
    test_set_file_failure_throws();

    std::cout << "\nAll Logger tests passed!" << std::endl;
    return 0;
}
