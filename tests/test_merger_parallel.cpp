// test_merger_parallel.cpp — partial relation merger parallel dispatcher tests
//
// Validates the GNFS_FILTER_MERGE_THREADS env-gated dispatcher introduced in
// include/gnfs/relation/merger_parallel.hpp:
//
//   * ENV parsing handles unset / "0" / "1" / "4" / "garbage" / "" / "9999"
//     correctly; clamping at hardware_concurrency() * 2.
//   * Sequential (N=1, default) and parallel (N>=2) paths produce identical
//     per-bucket outcomes for the same input span. The dispatcher is a pure
//     wrapper around a caller-supplied `merge_fn` callable, so deterministic
//     mock buckets (no real merger algorithm required) drive the parity
//     assertions.
//   * Empty bucket list returns empty vector cleanly without creating a pool
//     or invoking the merge functor.
//   * Single bucket under N>=2 short-circuits to sequential (exactly-once
//     invocation, no stall).
//   * Non-trivial Result type (`std::vector<int>`) is moved/copied correctly
//     without race or lost elements.
//   * Merge functor throwing propagates the first exception to the caller.

#include <gnfs/relation/merger_parallel.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using gnfs::relation::filter_merge_threads;
using gnfs::relation::filter_merge_threads_reset_env_cache_for_testing;
using gnfs::relation::parallel_merge_partials;

namespace {

// Helper: set or unset GNFS_FILTER_MERGE_THREADS and refresh the cache so the
// next call to filter_merge_threads() reflects the new value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_FILTER_MERGE_THREADS");
    } else {
        setenv("GNFS_FILTER_MERGE_THREADS", value, /*overwrite=*/1);
    }
    filter_merge_threads_reset_env_cache_for_testing();
}

// ───────────────────────────────────────────────────────────────────────────
// Test 1: ENV unset -> 1 (default sequential)
// ───────────────────────────────────────────────────────────────────────────
void test_env_unset_defaults_to_one() {
    std::cout << "Test 1: ENV unset -> 1..." << std::flush;
    apply_env(nullptr);
    std::size_t v = filter_merge_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: unset env parsed to " << v
                  << ", expected 1" << std::endl;
        std::abort();
    }
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: ENV "1" -> 1
// ───────────────────────────────────────────────────────────────────────────
void test_env_explicit_one() {
    std::cout << "Test 2: ENV '1' -> 1..." << std::flush;
    apply_env("1");
    std::size_t v = filter_merge_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: '1' parsed to " << v
                  << ", expected 1" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: ENV "4" -> 4 (or clamped to cap)
// ───────────────────────────────────────────────────────────────────────────
void test_env_four() {
    std::cout << "Test 3: ENV '4' -> 4..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;
    std::size_t expect = (4 < cap) ? 4 : cap;

    apply_env("4");
    std::size_t v = filter_merge_threads();
    if (v != expect) {
        std::cerr << "\n  ERROR: '4' parsed to " << v << ", expected "
                  << expect << " (hw*2 cap = " << cap << ")" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (parsed " << v << ", cap " << cap << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: ENV "garbage" / "" / "0" / negative -> 1
// ───────────────────────────────────────────────────────────────────────────
void test_env_non_numeric_to_one() {
    std::cout << "Test 4: ENV non-numeric / boundary -> 1..." << std::flush;

    // Empty string -> 1.
    apply_env("");
    assert(filter_merge_threads() == 1);

    // "0" -> 1 (invalid, non-positive).
    apply_env("0");
    assert(filter_merge_threads() == 1);

    // "-5" -> 1 (invalid, non-positive).
    apply_env("-5");
    assert(filter_merge_threads() == 1);

    // "garbage" -> 1 (atoi returns 0).
    apply_env("garbage");
    assert(filter_merge_threads() == 1);

    // " " (whitespace only) -> 1 (atoi returns 0).
    apply_env("   ");
    assert(filter_merge_threads() == 1);

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: ENV "9999" -> clamped at hardware_concurrency() * 2
// ───────────────────────────────────────────────────────────────────────────
void test_env_clamp_at_hw_times_two() {
    std::cout << "Test 5: ENV '9999' clamped at hw*2..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;

    apply_env("9999");
    std::size_t v = filter_merge_threads();
    if (v != cap) {
        std::cerr << "\n  ERROR: '9999' parsed to " << v
                  << ", expected cap=" << cap << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (cap=" << cap << ")\n";
}

void test_env_huge_positive_clamps_without_signed_overflow() {
    std::cout << "Test 6: huge positive ENV clamps without overflow..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    const std::size_t hw_size = static_cast<std::size_t>(hw);
    const std::size_t cap =
        hw_size > std::numeric_limits<std::size_t>::max() / 2
            ? std::numeric_limits<std::size_t>::max()
            : hw_size * 2;

    apply_env("999999999999999999999999999999999999999999999999999999");
    const std::size_t parsed = filter_merge_threads();
    if (parsed != cap) {
        std::cerr << "\n  ERROR: huge positive env parsed to " << parsed
                  << ", expected cap=" << cap << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (cap=" << cap << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: N=1 sequential baseline — identity merge_fn, dispatcher returns
//          vector aligned with input, exactly-once invocation per bucket.
// ───────────────────────────────────────────────────────────────────────────
void test_n1_sequential_baseline() {
    std::cout << "Test 6: N=1 sequential baseline (identity merge)..."
              << std::flush;
    apply_env("1");

    // Mock "bucket" type = vector<int>; merge produces sum + size pair encoded
    // into a single uint64_t so we can spot per-bucket mismatches cheaply.
    std::vector<std::vector<int>> buckets = {
        {1, 2, 3},
        {10, 20, 30, 40},
        {},
        {7},
        {100, 100, 100, 100, 100},
    };

    auto merge_fn = [](const std::vector<int>& bucket) -> uint64_t {
        uint64_t sum = 0;
        for (int v : bucket) sum += static_cast<uint64_t>(v);
        return (sum << 8) | static_cast<uint64_t>(bucket.size());
    };

    auto results = parallel_merge_partials<uint64_t, std::vector<int>>(
        std::span<const std::vector<int>>(buckets), merge_fn);

    if (results.size() != buckets.size()) {
        std::cerr << "\n  ERROR: expected " << buckets.size()
                  << " results, got " << results.size() << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        uint64_t expect = merge_fn(buckets[i]);
        if (results[i] != expect) {
            std::cerr << "\n  ERROR: idx " << i << " got " << results[i]
                      << " expected " << expect << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (" << buckets.size() << " buckets)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 7: N=1 vs N=4 parity — sum-over-bucket merge_fn, strict per-index
//          bit-identical assertion across the two paths.
// ───────────────────────────────────────────────────────────────────────────
void test_n1_vs_n4_parity() {
    std::cout << "Test 7: N=1 vs N=4 parity (per-index bit-identical)..."
              << std::flush;

    // Build a mid-size set of buckets with varying sizes so multiple pool
    // workers see non-trivial dispatch chunks.
    std::vector<std::vector<int>> buckets;
    buckets.reserve(64);
    for (std::size_t i = 0; i < 64; ++i) {
        std::vector<int> b;
        b.reserve((i % 7) + 1);
        for (std::size_t k = 0; k < (i % 7) + 1; ++k) {
            b.push_back(static_cast<int>(i * 31 + k * 7));
        }
        buckets.push_back(std::move(b));
    }

    auto merge_fn = [](const std::vector<int>& bucket) -> uint64_t {
        // Deterministic mixing function: pure of bucket contents.
        uint64_t acc = static_cast<uint64_t>(bucket.size()) * 1099511628211ULL;
        for (int v : bucket) {
            acc ^= static_cast<uint64_t>(v) + 0x9E3779B97F4A7C15ULL +
                   (acc << 6) + (acc >> 2);
        }
        return acc;
    };

    apply_env("1");
    auto seq = parallel_merge_partials<uint64_t, std::vector<int>>(
        std::span<const std::vector<int>>(buckets), merge_fn);

    apply_env("4");
    auto par = parallel_merge_partials<uint64_t, std::vector<int>>(
        std::span<const std::vector<int>>(buckets), merge_fn);

    apply_env(nullptr);

    if (seq.size() != par.size()) {
        std::cerr << "\n  ERROR: seq.size()=" << seq.size()
                  << " par.size()=" << par.size() << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i]
                      << " par=" << par[i] << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << buckets.size() << " per-index identical)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 8: N=1 vs N=hw_concurrency parity — extra coverage at the runtime
//          upper bound to catch races / aliasing that smaller N might miss.
// ───────────────────────────────────────────────────────────────────────────
void test_n1_vs_n_hw_parity() {
    std::cout << "Test 8: N=1 vs N=hw_concurrency parity..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::string hw_str = std::to_string(hw);

    auto merge_fn = [](const std::vector<int>& bucket) -> uint64_t {
        // Heavier mock to give the pool work to chew on.
        uint64_t acc = static_cast<uint64_t>(bucket.size()) | 1ULL;
        for (int v : bucket) {
            for (int k = 0; k < 32; ++k) {
                acc = acc * 6364136223846793005ULL +
                      1442695040888963407ULL +
                      static_cast<uint64_t>(v);
                acc ^= (acc >> 11);
            }
        }
        return acc;
    };

    std::vector<std::vector<int>> buckets;
    buckets.reserve(96);
    for (std::size_t i = 0; i < 96; ++i) {
        std::vector<int> b;
        b.reserve((i % 5) + 1);
        for (std::size_t k = 0; k < (i % 5) + 1; ++k) {
            b.push_back(static_cast<int>(i * 13 + k * 5));
        }
        buckets.push_back(std::move(b));
    }

    apply_env("1");
    auto seq = parallel_merge_partials<uint64_t, std::vector<int>>(
        std::span<const std::vector<int>>(buckets), merge_fn);

    apply_env(hw_str.c_str());
    auto par = parallel_merge_partials<uint64_t, std::vector<int>>(
        std::span<const std::vector<int>>(buckets), merge_fn);

    apply_env(nullptr);

    assert(seq.size() == par.size());
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i]
                      << " par=" << par[i] << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (N=hw=" << hw << ", " << buckets.size()
              << " per-index identical)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 9: empty bucket span — both N=1 and N=4 return empty vector cleanly
//          without creating a pool or invoking merge_fn.
// ───────────────────────────────────────────────────────────────────────────
void test_empty_bucket_list() {
    std::cout << "Test 9: empty bucket list (no-op both paths)..."
              << std::flush;

    std::vector<int> empty_in;

    // Sequential (N=1).
    apply_env("1");
    auto seq_results = parallel_merge_partials<int, int>(
        std::span<const int>(empty_in),
        [](const int&) -> int {
            std::cerr << "\n  ERROR: should not invoke merge_fn on empty span"
                      << std::endl;
            std::abort();
            return 0;
        });
    assert(seq_results.empty());

    // Parallel (N=4).
    apply_env("4");
    auto par_results = parallel_merge_partials<int, int>(
        std::span<const int>(empty_in),
        [](const int&) -> int {
            std::cerr << "\n  ERROR: should not invoke merge_fn on empty span"
                      << std::endl;
            std::abort();
            return 0;
        });
    assert(par_results.empty());

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 10: single bucket under N=4 — short-circuit to sequential (exactly-
//          once invocation, no stall).
// ───────────────────────────────────────────────────────────────────────────
void test_single_bucket_no_stall() {
    std::cout << "Test 10: single bucket N=4 (exactly-once, no stall)..."
              << std::flush;

    apply_env("4");
    std::vector<int> single = {42};
    std::atomic<int> calls{0};

    auto t0 = std::chrono::steady_clock::now();
    auto results = parallel_merge_partials<int, int>(
        std::span<const int>(single),
        [&calls](const int& v) -> int {
            calls.fetch_add(1, std::memory_order_relaxed);
            return v * 2;
        });
    auto t1 = std::chrono::steady_clock::now();
    long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    assert(results.size() == 1);
    assert(results[0] == 84);
    int total_calls = calls.load(std::memory_order_relaxed);
    if (total_calls != 1) {
        std::cerr << "\n  ERROR: expected exactly 1 call, got " << total_calls
                  << std::endl;
        std::abort();
    }

    // Sanity-bound the wall-time: if the helper accidentally spawned a
    // 4-thread pool the spin-up alone would push past this.
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-bucket dispatch took " << ms
                  << " ms (expected << 1000 ms)" << std::endl;
        // No abort -- this is a soft signal, sanitizers can be slow.
    }

    apply_env(nullptr);
    std::cout << " PASS (" << ms << " ms)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 11: Non-trivial Result type (`std::vector<int>`) — exercises move /
//          copy semantics so a per-bucket vector return doesn't drop or
//          shred elements when assigned into the output slot under either
//          dispatch path.
// ───────────────────────────────────────────────────────────────────────────
void test_non_trivial_result_type() {
    std::cout << "Test 11: non-trivial Result type (vector<int>) parity..."
              << std::flush;

    // Each bucket emits its own (size+1)-element vector populated with
    // bucket index + member index, so we can verify per-slot integrity.
    std::vector<std::vector<int>> buckets;
    buckets.reserve(32);
    for (std::size_t i = 0; i < 32; ++i) {
        std::vector<int> b;
        b.reserve((i % 4) + 1);
        for (std::size_t k = 0; k < (i % 4) + 1; ++k) {
            b.push_back(static_cast<int>(i * 1000 + k));
        }
        buckets.push_back(std::move(b));
    }

    auto merge_fn = [](const std::vector<int>& bucket) -> std::vector<int> {
        // Emit bucket contents prefixed with size; ensures non-trivial move
        // semantics travel through the dispatcher's results[i] = ... write.
        std::vector<int> out;
        out.reserve(bucket.size() + 1);
        out.push_back(static_cast<int>(bucket.size()));
        for (int v : bucket) out.push_back(v + 1);
        return out;
    };

    apply_env("1");
    auto seq = parallel_merge_partials<std::vector<int>, std::vector<int>>(
        std::span<const std::vector<int>>(buckets), merge_fn);

    apply_env("4");
    auto par = parallel_merge_partials<std::vector<int>, std::vector<int>>(
        std::span<const std::vector<int>>(buckets), merge_fn);

    apply_env(nullptr);

    assert(seq.size() == par.size());
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: idx " << i
                      << " seq.size()=" << seq[i].size()
                      << " par.size()=" << par[i].size() << std::endl;
            std::abort();
        }
        // And cross-check seq[i] against the freshly computed reference.
        auto expect = merge_fn(buckets[i]);
        if (seq[i] != expect) {
            std::cerr << "\n  ERROR: idx " << i
                      << " seq does not match merge_fn reference" << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << buckets.size() << " vectors compared)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 12: merge_fn throws — exception propagates to caller (does not swallow).
//          Verified under both N=1 and N=4 paths.
// ───────────────────────────────────────────────────────────────────────────
void test_merge_fn_exception_propagates() {
    std::cout << "Test 12: merge_fn exception propagates..." << std::flush;

    std::vector<int> buckets = {1, 2, 3, 4, 5, 6, 7, 8};

    auto throw_fn = [](const int& v) -> int {
        if (v == 4) {
            throw std::runtime_error("bucket 4 sentinel");
        }
        return v * 10;
    };

    // Sequential path: exception must rethrow.
    apply_env("1");
    bool seq_caught = false;
    try {
        auto seq = parallel_merge_partials<int, int>(
            std::span<const int>(buckets), throw_fn);
        (void)seq;
    } catch (const std::runtime_error& e) {
        seq_caught = true;
        std::string what = e.what();
        if (what.find("bucket 4 sentinel") == std::string::npos) {
            std::cerr << "\n  ERROR: seq path unexpected what(): " << what
                      << std::endl;
            std::abort();
        }
    }
    if (!seq_caught) {
        std::cerr << "\n  ERROR: seq path swallowed exception" << std::endl;
        std::abort();
    }

    // Parallel path: at least one task throws; first observed exception
    // rethrows (any exception of expected type satisfies the contract).
    apply_env("4");
    bool par_caught = false;
    try {
        auto par = parallel_merge_partials<int, int>(
            std::span<const int>(buckets), throw_fn);
        (void)par;
    } catch (const std::runtime_error& e) {
        par_caught = true;
        std::string what = e.what();
        if (what.find("bucket 4 sentinel") == std::string::npos) {
            std::cerr << "\n  ERROR: par path unexpected what(): " << what
                      << std::endl;
            std::abort();
        }
    }
    if (!par_caught) {
        std::cerr << "\n  ERROR: par path swallowed exception" << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS (seq + par both rethrow)\n";
}

}  // namespace

int main() {
    std::cout << "=== Partial Relation Merger Parallel Dispatch Tests ==="
              << std::endl;

    test_env_unset_defaults_to_one();
    test_env_explicit_one();
    test_env_four();
    test_env_non_numeric_to_one();
    test_env_clamp_at_hw_times_two();
    test_env_huge_positive_clamps_without_signed_overflow();
    test_n1_sequential_baseline();
    test_n1_vs_n4_parity();
    test_n1_vs_n_hw_parity();
    test_empty_bucket_list();
    test_single_bucket_no_stall();
    test_non_trivial_result_type();
    test_merge_fn_exception_propagates();

    std::cout << std::endl
              << "=== All Merger Parallel Tests PASSED ===" << std::endl;
    return 0;
}
