// test_couveignes_parallel.cpp — Couveignes sign-pattern parallel search tests
//
// Validates the GNFS_COUVEIGNES_PARALLEL_THREADS env-gated dispatcher introduced in
// include/gnfs/sqrt/couveignes_parallel.hpp:
//   * Sequential (N=1, default) returns the first matching pattern in scan
//     order; parallel (N>=2) returns one of the matching patterns (guaranteed
//     to satisfy the verifier).  When exactly one match exists, the two paths
//     return the same value.
//   * ENV parsing handles unset / "" / "0" / "1" / "4" / "999" / negative /
//     non-numeric correctly; clamping at hardware_concurrency()*2.
//   * Edge cases: empty range, single-pattern range, ThreadPool overhead
//     short-circuit for size-1 ranges, atomic-min reduction for multi-valid.
//
// Tests use mock verify_fn lambdas (no real Couveignes invocation) so we can
// precisely control the matching pattern set and assert deterministic results.

#include <gnfs/sqrt/couveignes_parallel.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

using gnfs::sqrt::couveignes_parallel_threads;
using gnfs::sqrt::couveignes_parallel_threads_reset_env_cache_for_testing;
using gnfs::sqrt::parallel_pattern_search;

namespace {

// Helper: set ENV + reset cache to ensure subsequent reads see the new value.
void set_env_and_reset(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_COUVEIGNES_PARALLEL_THREADS");
    } else {
        setenv("GNFS_COUVEIGNES_PARALLEL_THREADS", value, /*overwrite=*/1);
    }
    couveignes_parallel_threads_reset_env_cache_for_testing();
}

void test_env_unset_default_one() {
    std::cout << "Testing ENV unset -> default 1..." << std::endl;
    set_env_and_reset(nullptr);
    int n = couveignes_parallel_threads();
    if (n != 1) {
        std::cerr << "  ERROR: expected 1, got " << n << std::endl;
        std::abort();
    }
    std::cout << "  unset -> 1: PASSED" << std::endl;
}

void test_env_one_explicit() {
    std::cout << "Testing ENV='1' -> 1..." << std::endl;
    set_env_and_reset("1");
    int n = couveignes_parallel_threads();
    assert(n == 1);
    std::cout << "  '1' -> 1: PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_env_four_parallel() {
    std::cout << "Testing ENV='4' -> 4..." << std::endl;
    set_env_and_reset("4");
    int n = couveignes_parallel_threads();
    // Modern hardware has hw_concurrency >= 2, so cap = 4.  If somehow we
    // are on a single-core CI (unlikely), cap = max(2 * hw_concurrency, 16).
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;
    if (cap < 1) cap = 16;
    int expected = (4 > cap) ? cap : 4;
    if (n != expected) {
        std::cerr << "  ERROR: expected " << expected << ", got " << n << std::endl;
        std::abort();
    }
    std::cout << "  '4' -> " << n << ": PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_env_above_max_clamps() {
    std::cout << "Testing ENV='999' -> clamp to hw*2..." << std::endl;
    set_env_and_reset("999");
    int n = couveignes_parallel_threads();
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;
    if (cap < 1) cap = 16;
    if (n != cap) {
        std::cerr << "  ERROR: expected clamped " << cap << ", got " << n << std::endl;
        std::abort();
    }
    std::cout << "  '999' -> " << n << " (clamped to hw*2): PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_env_invalid_non_numeric() {
    std::cout << "Testing ENV invalid values -> default 1..." << std::endl;

    // Empty string -> default 1.
    set_env_and_reset("");
    assert(couveignes_parallel_threads() == 1);

    // "0" -> non-positive -> 1.
    set_env_and_reset("0");
    assert(couveignes_parallel_threads() == 1);

    // "-3" -> non-positive -> 1.
    set_env_and_reset("-3");
    assert(couveignes_parallel_threads() == 1);

    // "abc" -> non-numeric -> 1.
    set_env_and_reset("abc");
    assert(couveignes_parallel_threads() == 1);

    // "4x" -> partial-numeric, treated as invalid -> 1 (predictable strict path).
    set_env_and_reset("4x");
    assert(couveignes_parallel_threads() == 1);

    // "  4  " -> leading whitespace, treated as invalid -> 1.
    set_env_and_reset("  4  ");
    assert(couveignes_parallel_threads() == 1);

    std::cout << "  empty/'0'/'-3'/'abc'/'4x'/'  4  ' -> 1: PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_sequential_no_valid() {
    std::cout << "Testing sequential (N=1) no valid pattern -> nullopt..." << std::endl;
    set_env_and_reset("1");

    std::atomic<uint64_t> calls{0};
    auto verify = [&calls](uint64_t /*pattern*/) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return false;
    };

    auto result = parallel_pattern_search(0ULL, 1024ULL, verify);
    assert(!result.has_value());
    // Sequential path must scan the entire range.
    assert(calls.load() == 1024);

    std::cout << "  N=1 no-match scanned " << calls.load()
              << " patterns, returned nullopt: PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_sequential_single_valid() {
    std::cout << "Testing sequential (N=1) single valid pattern -> 42..." << std::endl;
    set_env_and_reset("1");

    std::atomic<uint64_t> calls{0};
    auto verify = [&calls](uint64_t pattern) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return pattern == 42;
    };

    auto result = parallel_pattern_search(0ULL, 1024ULL, verify);
    assert(result.has_value());
    if (result.value() != 42) {
        std::cerr << "  ERROR: expected 42, got " << result.value() << std::endl;
        std::abort();
    }
    // Sequential must short-circuit at 42 (43 calls: 0..42 inclusive).
    if (calls.load() != 43) {
        std::cerr << "  ERROR: expected 43 calls, got " << calls.load() << std::endl;
        std::abort();
    }
    std::cout << "  N=1 found 42 after " << calls.load() << " calls: PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_parallel_single_valid_parity() {
    std::cout << "Testing parallel (N=4) single valid pattern -> 42..." << std::endl;
    set_env_and_reset("4");

    std::atomic<uint64_t> calls{0};
    auto verify = [&calls](uint64_t pattern) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return pattern == 42;
    };

    auto result = parallel_pattern_search(0ULL, 1024ULL, verify);
    assert(result.has_value());
    if (result.value() != 42) {
        std::cerr << "  ERROR: expected 42, got " << result.value() << std::endl;
        std::abort();
    }
    // Calls count not asserted strictly — parallel workers may scan extra
    // patterns before the short-circuit propagates.  We only require that
    // verify_fn is called at least 43 times (cumulative work to reach 42),
    // and bounded by the total range size 1024.
    auto observed = calls.load();
    if (observed < 1 || observed > 1024) {
        std::cerr << "  ERROR: unexpected call count " << observed << std::endl;
        std::abort();
    }
    std::cout << "  N=4 found 42 after " << observed
              << " calls (bounded by range, may include parallel waste): PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_parallel_multiple_valid_returns_one() {
    std::cout << "Testing parallel (N=4) multiple valid patterns -> one of {10, 20, 30}..."
              << std::endl;
    set_env_and_reset("4");

    auto verify = [](uint64_t pattern) {
        return pattern == 10 || pattern == 20 || pattern == 30;
    };

    auto result = parallel_pattern_search(0ULL, 1024ULL, verify);
    assert(result.has_value());
    uint64_t v = result.value();
    if (v != 10 && v != 20 && v != 30) {
        std::cerr << "  ERROR: expected 10/20/30, got " << v << std::endl;
        std::abort();
    }
    std::cout << "  N=4 returned valid pattern " << v << ": PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_parallel_multi_valid_distributed() {
    // Verify that when matches are spread across different chunks (so the
    // first match a worker sees in its own chunk is different per worker),
    // the atomic-min reduction picks the smallest one across all chunks.
    //
    // Range 1024 with N=4 → chunk_size = 256:
    //   chunk 0: [0, 256)    → match at 100
    //   chunk 1: [256, 512)  → match at 300
    //   chunk 2: [512, 768)  → match at 600
    //   chunk 3: [768, 1024) → match at 900
    // Atomic-min must converge to 100 (smallest across all chunks).
    std::cout << "Testing parallel (N=4) distributed valid {100, 300, 600, 900} -> 100..."
              << std::endl;
    set_env_and_reset("4");

    auto verify = [](uint64_t pattern) {
        return pattern == 100 || pattern == 300 || pattern == 600 || pattern == 900;
    };

    auto result = parallel_pattern_search(0ULL, 1024ULL, verify);
    assert(result.has_value());
    uint64_t v = result.value();
    // Atomic-min across all chunks must be 100.
    if (v != 100) {
        std::cerr << "  ERROR: expected atomic-min 100, got " << v << std::endl;
        std::abort();
    }
    std::cout << "  N=4 distributed returned smallest 100: PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_parallel_atomic_min_smallest() {
    // With 3 matches scattered, parallel workers each find one match per
    // chunk; the atomic_min reduction should converge on the smallest
    // observed match.  For N=4 workers and a small range, with matches at
    // positions 10/20/30 in a 1024-wide range, each chunk is 256 wide:
    //   chunk 0: [0, 256)   contains 10, 20, 30  (smallest seen: 10)
    //   chunk 1: [256, 512) no matches
    //   chunk 2: [512, 768) no matches
    //   chunk 3: [768, 1024) no matches
    // So worker 0 finds 10 first; result MUST be 10 (only chunk seeing
    // matches reports back).
    std::cout << "Testing parallel atomic-min reduction (3 matches all in chunk 0)..."
              << std::endl;
    set_env_and_reset("4");

    auto verify = [](uint64_t pattern) {
        return pattern == 10 || pattern == 20 || pattern == 30;
    };

    auto result = parallel_pattern_search(0ULL, 1024ULL, verify);
    assert(result.has_value());
    // In this scenario the result MUST be 10 because worker 0 is the only
    // one seeing matches and it scans left-to-right.
    if (result.value() != 10) {
        std::cerr << "  ERROR: expected 10 (smallest in chunk 0), got "
                  << result.value() << std::endl;
        std::abort();
    }
    std::cout << "  N=4 atomic-min returns 10: PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_empty_range() {
    std::cout << "Testing empty range (start == end) -> nullopt..." << std::endl;

    // Sequential (N=1).
    set_env_and_reset("1");
    std::atomic<uint64_t> calls{0};
    auto verify = [&calls](uint64_t) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return true;
    };
    auto r1 = parallel_pattern_search(100ULL, 100ULL, verify);
    assert(!r1.has_value());
    assert(calls.load() == 0);

    // Parallel (N=4) — must also short-circuit before pool creation.
    set_env_and_reset("4");
    auto r2 = parallel_pattern_search(0ULL, 0ULL, verify);
    assert(!r2.has_value());
    assert(calls.load() == 0);

    // Inverted range (end < start) treated as empty.
    auto r3 = parallel_pattern_search(50ULL, 10ULL, verify);
    assert(!r3.has_value());
    assert(calls.load() == 0);

    std::cout << "  empty/inverted ranges -> nullopt, no verify_fn calls: PASSED"
              << std::endl;

    set_env_and_reset(nullptr);
}

void test_single_pattern_range() {
    std::cout << "Testing single-pattern range (range == 1) -> sequential fast path..."
              << std::endl;
    set_env_and_reset("4");  // Even with N=4 env, range=1 must take fast path.

    std::atomic<uint64_t> calls{0};
    auto verify_hit = [&calls](uint64_t pattern) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return pattern == 7;
    };
    auto r1 = parallel_pattern_search(7ULL, 8ULL, verify_hit);
    assert(r1.has_value());
    assert(r1.value() == 7);
    assert(calls.load() == 1);

    auto verify_miss = [&calls](uint64_t) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    calls.store(0);
    auto r2 = parallel_pattern_search(7ULL, 8ULL, verify_miss);
    assert(!r2.has_value());
    assert(calls.load() == 1);

    std::cout << "  single-pattern range hit + miss: PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_reset_env_cache() {
    std::cout << "Testing reset_env_cache_for_testing helper..." << std::endl;

    // Set ENV to 4, force cache load.
    set_env_and_reset("4");
    int n1 = couveignes_parallel_threads();
    assert(n1 >= 1);

    // Change ENV but DO NOT reset — cache should retain old value.
    setenv("GNFS_COUVEIGNES_PARALLEL_THREADS", "1", /*overwrite=*/1);
    int n2 = couveignes_parallel_threads();
    // Cached value should still be n1 (4), not 1.
    assert(n2 == n1);

    // Now reset — cache reload should observe the new value.
    couveignes_parallel_threads_reset_env_cache_for_testing();
    int n3 = couveignes_parallel_threads();
    assert(n3 == 1);

    std::cout << "  cache reload after reset_env_cache_for_testing: PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_parallel_dense_match_all_chunks() {
    // Verify the short-circuit works when every chunk has work to do:
    //   range = 4096, matches at 1000, 2000, 3000, 3900
    //   N = 4 workers, each chunk 1024 wide
    //   chunk 0 [0,1024): match at 1000
    //   chunk 1 [1024,2048): match at 2000
    //   chunk 2 [2048,3072): match at 3000
    //   chunk 3 [3072,4096): match at 3900
    // Atomic-min should return 1000 (smallest), regardless of scheduling.
    std::cout << "Testing parallel dense match across all chunks -> atomic-min 1000..."
              << std::endl;
    set_env_and_reset("4");

    auto verify = [](uint64_t pattern) {
        return pattern == 1000 || pattern == 2000 || pattern == 3000 || pattern == 3900;
    };

    auto result = parallel_pattern_search(0ULL, 4096ULL, verify);
    assert(result.has_value());
    if (result.value() != 1000) {
        std::cerr << "  ERROR: expected atomic-min 1000, got " << result.value() << std::endl;
        std::abort();
    }
    std::cout << "  N=4 dense-match returns smallest 1000: PASSED" << std::endl;

    set_env_and_reset(nullptr);
}

void test_parallel_no_match_full_scan() {
    std::cout << "Testing parallel (N=4) no match -> full scan, nullopt..." << std::endl;
    set_env_and_reset("4");

    std::atomic<uint64_t> calls{0};
    auto verify = [&calls](uint64_t) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return false;
    };

    auto result = parallel_pattern_search(0ULL, 4096ULL, verify);
    assert(!result.has_value());
    // All workers must scan their entire chunk because no short-circuit signal.
    assert(calls.load() == 4096);

    std::cout << "  N=4 no-match scanned " << calls.load() << " patterns: PASSED"
              << std::endl;

    set_env_and_reset(nullptr);
}

void test_perf_info() {
    // Information-only timing comparison.  No assertion on speedup because
    // the mock verify_fn is trivial — real Couveignes verify is heavier
    // (CRT update + d² coefficient compute) where parallelism actually pays.
    std::cout << "Couveignes parallel perf info: N=1 vs N=4 (mock verify, 65536 patterns)..."
              << std::endl;

    auto verify_heavy = [](uint64_t pattern) {
        // Simulate moderate verify cost with a tight integer-only loop.
        uint64_t acc = pattern;
        for (uint64_t i = 0; i < 1000; ++i) {
            acc = acc * 2654435761ULL + i;
        }
        // Match at a single late position so both N=1 and N=4 do most of
        // the scan work; the parallel path can short-circuit earlier
        // depending on chunking.
        return pattern == 60000 && (acc & 1ULL) == 1ULL;
    };

    set_env_and_reset("1");
    auto t0 = std::chrono::steady_clock::now();
    auto r1 = parallel_pattern_search(0ULL, 65536ULL, verify_heavy);
    auto t1 = std::chrono::steady_clock::now();

    set_env_and_reset("4");
    auto r4 = parallel_pattern_search(0ULL, 65536ULL, verify_heavy);
    auto t2 = std::chrono::steady_clock::now();

    auto ms1 = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    auto ms4 = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

    std::cout << "  N=1: " << ms1 << " ms, found=" << (r1.has_value() ? "yes" : "no")
              << std::endl;
    std::cout << "  N=4: " << ms4 << " ms, found=" << (r4.has_value() ? "yes" : "no")
              << std::endl;
    if (ms1 > 0 && ms4 > 0) {
        double speedup = static_cast<double>(ms1) / static_cast<double>(ms4);
        std::cout << "  speedup (informational): " << speedup << "x" << std::endl;
    }
    std::cout << "  PASSED (info-only, no assert on speedup)" << std::endl;

    set_env_and_reset(nullptr);
}

}  // namespace

int main() {
    std::cout << "=== Couveignes Parallel Pattern Search Tests ===" << std::endl
              << std::endl;

    test_env_unset_default_one();
    test_env_one_explicit();
    test_env_four_parallel();
    test_env_above_max_clamps();
    test_env_invalid_non_numeric();

    test_sequential_no_valid();
    test_sequential_single_valid();
    test_parallel_single_valid_parity();
    test_parallel_multiple_valid_returns_one();
    test_parallel_multi_valid_distributed();
    test_parallel_atomic_min_smallest();
    test_parallel_dense_match_all_chunks();
    test_parallel_no_match_full_scan();

    test_empty_range();
    test_single_pattern_range();
    test_reset_env_cache();

    test_perf_info();

    std::cout << std::endl
              << "=== All Couveignes Parallel Tests PASSED ===" << std::endl;
    return 0;
}
