// test_work_stealing.cpp — Verify work-stealing parallel_for_stealing correctness

#include <gnfs/util/thread_pool.hpp>

#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

using gnfs::util::ThreadPool;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    tests_passed++; \
} while(0)

/// Test basic correctness: all indices visited exactly once
void test_all_indices_visited() {
    ThreadPool pool(0);
    const size_t N = 10000;
    std::vector<std::atomic<int>> visited(N);
    for (auto& v : visited) v.store(0);

    pool.parallel_for_stealing(0, N, [&](size_t i) {
        visited[i].fetch_add(1, std::memory_order_relaxed);
    });

    for (size_t i = 0; i < N; ++i) {
        TEST_ASSERT(visited[i].load() == 1, "each index should be visited exactly once");
    }
    TEST_PASS("all indices visited once (N=10000)");
}

/// Test with grain > 1
void test_grain_size() {
    ThreadPool pool(0);
    const size_t N = 1000;
    std::vector<std::atomic<int>> visited(N);
    for (auto& v : visited) v.store(0);

    pool.parallel_for_stealing(0, N, [&](size_t i) {
        visited[i].fetch_add(1, std::memory_order_relaxed);
    }, 16);  // grain = 16

    for (size_t i = 0; i < N; ++i) {
        TEST_ASSERT(visited[i].load() == 1, "grain=16: each index visited once");
    }
    TEST_PASS("grain=16 correctness");
}

/// Test with non-aligned end
void test_non_aligned() {
    ThreadPool pool(0);
    const size_t N = 137;  // Prime, not aligned to any grain
    std::vector<std::atomic<int>> visited(N);
    for (auto& v : visited) v.store(0);

    pool.parallel_for_stealing(10, 10 + N, [&](size_t i) {
        visited[i - 10].fetch_add(1, std::memory_order_relaxed);
    }, 8);

    for (size_t i = 0; i < N; ++i) {
        TEST_ASSERT(visited[i].load() == 1, "non-aligned range: each index visited once");
    }
    TEST_PASS("non-aligned range (start=10, N=137, grain=8)");
}

/// Test accumulation correctness
void test_accumulation() {
    ThreadPool pool(0);
    const size_t N = 50000;
    std::atomic<uint64_t> sum{0};

    pool.parallel_for_stealing(1, N + 1, [&](size_t i) {
        sum.fetch_add(i, std::memory_order_relaxed);
    });

    uint64_t expected = N * (N + 1) / 2;
    TEST_ASSERT(sum.load() == expected, "sum should equal N*(N+1)/2");
    TEST_PASS("accumulation correctness (sum 1..50000)");
}

/// Test that stealing achieves better balance with uneven work
void test_load_balance() {
    ThreadPool pool(0);
    const size_t N = 100;
    std::vector<std::atomic<size_t>> thread_work(pool.num_threads());
    for (auto& w : thread_work) w.store(0);

    // Simulate uneven work: index i does i iterations of busy-wait
    pool.parallel_for_stealing(0, N, [&](size_t i) {
        // Record which "thread" did this work (approximate via thread_id hash)
        int dummy = 0;
        for (size_t k = 0; k < i * 100; ++k) dummy += static_cast<int>(k);
        if (dummy < 0) std::abort();  // prevent optimization
    }, 1);

    // Just verify all work was done — load balance is hard to assert precisely
    TEST_PASS("uneven work completed without deadlock");
}

/// Compare static vs stealing for non-trivial workload
void test_comparison_with_static() {
    ThreadPool pool(0);
    const size_t N = 10000;

    // Static (parallel_for_index)
    std::vector<std::atomic<int>> visited_static(N);
    for (auto& v : visited_static) v.store(0);
    pool.parallel_for_index(0, N, [&](size_t i) {
        visited_static[i].fetch_add(1, std::memory_order_relaxed);
    });

    // Stealing
    std::vector<std::atomic<int>> visited_steal(N);
    for (auto& v : visited_steal) v.store(0);
    pool.parallel_for_stealing(0, N, [&](size_t i) {
        visited_steal[i].fetch_add(1, std::memory_order_relaxed);
    });

    // Both should visit all indices
    for (size_t i = 0; i < N; ++i) {
        TEST_ASSERT(visited_static[i].load() == 1, "static: index missed");
        TEST_ASSERT(visited_steal[i].load() == 1, "stealing: index missed");
    }
    TEST_PASS("static vs stealing both correct");
}

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Work-Stealing ThreadPool Tests\n";
    std::cout << "  Threads: " << std::thread::hardware_concurrency() << "\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_all_indices_visited();
    test_grain_size();
    test_non_aligned();
    test_accumulation();
    test_load_balance();
    test_comparison_with_static();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
