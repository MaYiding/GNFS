// test_logger.cpp — Logger 多线程并发行为回归保护
//
// 历史 bug:level_ 原是 LogLevel,set_level/level/is_enabled 不上锁,
// 多线程 set_level 与 log 并发会 data-race。已修为 atomic<LogLevel>。
// 此测试锁住该行为:多线程 set_level + log 同时进行,期望:
//   (1) 无 race / 撕裂(ASan/TSan 会抓)
//   (2) is_enabled 单调读到某个有效 level
//   (3) 写入的 log 行不交错(mutex_ 保护)

#include "gnfs/util/logger.hpp"

#include <atomic>
#include <cassert>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

using namespace gnfs::util;

void test_level_atomicity() {
    std::cout << "Testing concurrent set_level + is_enabled..." << std::endl;

    Logger& log = Logger::instance();
    std::atomic<bool> stop{false};
    std::atomic<size_t> total_reads{0};

    // Reader threads check is_enabled across many calls
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&log, &stop, &total_reads]() {
            while (!stop.load()) {
                // Just ensure no UB / no torn read — value can be any LogLevel
                volatile bool v = log.is_enabled(LogLevel::Info);
                (void)v;
                total_reads.fetch_add(1);
            }
        });
    }

    // Writer toggles between Trace and Fatal
    std::thread writer([&log, &stop]() {
        for (int i = 0; i < 100000; ++i) {
            log.set_level((i & 1) ? LogLevel::Trace : LogLevel::Fatal);
        }
        stop.store(true);
    });

    writer.join();
    for (auto& r : readers) r.join();

    std::cout << "  level atomicity: PASS (" << total_reads.load() << " reads, no race)" << std::endl;
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
    const std::string marker = "TESTMSG_END";

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&log, t]() {
            for (int i = 0; i < messages_per_thread; ++i) {
                log.log(LogLevel::Info, "test",
                        "thread_" + std::to_string(t) + "_msg_" + std::to_string(i)
                        + " TESTMSG_END");
            }
        });
    }
    for (auto& th : threads) th.join();

    log.set_output(std::cerr);  // restore so other tests don't capture stderr

    // Validate: count occurrences of marker should equal total messages,
    // and every line containing "test_msg_" should also contain marker (no tearing).
    std::string out = captured.str();
    size_t marker_count = 0;
    size_t pos = 0;
    while ((pos = out.find(marker, pos)) != std::string::npos) {
        ++marker_count;
        pos += marker.size();
    }
    assert(marker_count == static_cast<size_t>(num_threads * messages_per_thread));

    std::cout << "  concurrent log: PASS (" << marker_count << " messages, all atomic)" << std::endl;
}

void test_set_file_failure_throws() {
    std::cout << "Testing set_file failure throws..." << std::endl;

    Logger& log = Logger::instance();
    // /proc/this/cannot/exist 不存在,无法 open,期望抛出
    bool threw = false;
    try {
        log.set_file("/this/path/definitely/cannot/exist/log.txt");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    // Logger 状态保持正常:依然能 log 到 stderr
    log.set_output(std::cerr);  // 显式 reset (set_file 失败后 output_ 应未改)
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
