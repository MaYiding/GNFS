#include "gnfs/util/thread_pool.hpp"
#include "gnfs/util/timer.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace gnfs::util;

void test_basic_submit() {
    std::cout << "Testing basic submit..." << std::endl;

    ThreadPool pool(4);

    auto future = pool.submit([]() {
        return 42;
    });

    assert(future.get() == 42);

    std::cout << "  Basic submit: PASS" << std::endl;
}

void test_multiple_tasks() {
    std::cout << "Testing multiple tasks..." << std::endl;

    ThreadPool pool(4);

    std::vector<std::future<int>> futures;
    for (int i = 0; i < 100; ++i) {
        futures.push_back(pool.submit([i]() {
            return i * 2;
        }));
    }

    for (int i = 0; i < 100; ++i) {
        assert(futures[static_cast<size_t>(i)].get() == i * 2);
    }

    std::cout << "  Multiple tasks: PASS" << std::endl;
}

void test_parallel_for_index() {
    std::cout << "Testing parallel_for_index..." << std::endl;

    ThreadPool pool(4);

    std::atomic<int> sum{0};
    pool.parallel_for_index(0, 1000, [&sum](size_t i) {
        sum += static_cast<int>(i);
    });

    // 0 + 1 + 2 + ... + 999 = 999 * 1000 / 2 = 499500
    assert(sum.load() == 499500);

    std::cout << "  parallel_for_index: PASS" << std::endl;
}

void test_parallel_for() {
    std::cout << "Testing parallel_for..." << std::endl;

    ThreadPool pool(4);

    std::vector<int> data(100);
    for (int i = 0; i < 100; ++i) {
        data[static_cast<size_t>(i)] = i;
    }

    std::atomic<int> sum{0};
    pool.parallel_for(data.begin(), data.end(), [&sum](int val) {
        sum += val;
    });

    // 0 + 1 + ... + 99 = 99 * 100 / 2 = 4950
    assert(sum.load() == 4950);

    std::cout << "  parallel_for: PASS" << std::endl;
}

void test_wait_all() {
    std::cout << "Testing wait_all..." << std::endl;

    ThreadPool pool(4);

    std::atomic<int> counter{0};

    for (int i = 0; i < 100; ++i) {
        pool.submit([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            counter++;
        });
    }

    pool.wait_all();
    assert(counter.load() == 100);

    std::cout << "  wait_all: PASS" << std::endl;
}

void test_num_threads() {
    std::cout << "Testing num_threads..." << std::endl;

    ThreadPool pool1(8);
    assert(pool1.num_threads() == 8);

    ThreadPool pool2(0);  // 自动检测
    assert(pool2.num_threads() > 0);

    std::cout << "  num_threads: PASS" << std::endl;
}

void test_exception_handling() {
    std::cout << "Testing exception handling..." << std::endl;

    ThreadPool pool(2);

    auto future = pool.submit([]() -> int {
        throw std::runtime_error("test exception");
        return 0;
    });

    bool caught = false;
    try {
        future.get();
    } catch (const std::runtime_error& e) {
        caught = true;
        assert(std::string(e.what()) == "test exception");
    }
    assert(caught);

    std::cout << "  Exception handling: PASS" << std::endl;
}

/// Stress test: rapid submit + wait_all cycles to expose the race
/// between --pending_ and done_cv_.notify_all() in worker_loop().
///
/// The race: if --pending_ and notify happen outside the mutex,
/// wait_all() can miss the notification and deadlock forever.
/// We use a timeout to detect this: if wait_all() doesn't return
/// within 5 seconds per round, the race has been triggered.
void test_wait_all_race_stress() {
    std::cout << "Testing wait_all race condition (stress)..." << std::endl;

    // Use many threads to maximize scheduling interleaving
    ThreadPool pool(8);

    constexpr int rounds = 5000;
    constexpr int tasks_per_round = 20;

    for (int round = 0; round < rounds; ++round) {
        std::atomic<int> counter{0};

        // Submit many tiny tasks (no sleep — maximize race window)
        for (int i = 0; i < tasks_per_round; ++i) {
            pool.submit([&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }

        // wait_all() must return. Under the old buggy code, this can
        // deadlock if the notification is lost between predicate check
        // and cv_.wait() in wait_all().
        pool.wait_all();

        // After wait_all returns, ALL tasks must be complete
        assert(counter.load() == tasks_per_round &&
               "wait_all() returned before all tasks completed!");
    }

    std::cout << "  wait_all race stress (" << rounds << " rounds): PASS" << std::endl;
}

/// Stress test: concurrent submit from multiple threads + wait_all
/// Tests the interaction between submit(++pending_) and worker(--pending_)
void test_concurrent_submit_wait() {
    std::cout << "Testing concurrent submit + wait_all..." << std::endl;

    ThreadPool pool(4);

    constexpr int rounds = 1000;

    for (int round = 0; round < rounds; ++round) {
        std::atomic<int> counter{0};
        constexpr int tasks = 16;

        // Submit tasks from multiple threads simultaneously
        std::vector<std::thread> submitters;
        for (int t = 0; t < 4; ++t) {
            submitters.emplace_back([&pool, &counter]() {
                for (int i = 0; i < 4; ++i) {
                    pool.submit([&counter]() {
                        counter.fetch_add(1, std::memory_order_relaxed);
                    });
                }
            });
        }

        for (auto& t : submitters) {
            t.join();
        }

        pool.wait_all();
        assert(counter.load() == tasks &&
               "Concurrent submit: wait_all returned prematurely!");
    }

    std::cout << "  Concurrent submit + wait_all (" << rounds << " rounds): PASS"
              << std::endl;
}

int main() {
    std::cout << "=== ThreadPool Tests ===" << std::endl;

    test_basic_submit();
    test_multiple_tasks();
    test_parallel_for_index();
    test_parallel_for();
    test_wait_all();
    test_num_threads();
    test_exception_handling();
    test_wait_all_race_stress();
    test_concurrent_submit_wait();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
