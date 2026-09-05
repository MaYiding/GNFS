// test_sieve_apply_tile_parallel.cpp - sieve apply-tile parallel
// dispatcher tests.
//
// Validates the GNFS_SIEVE_APPLY_TILE_THREADS env-gated dispatcher
// introduced in include/gnfs/sieve/apply_tile_parallel.hpp. The helper
// is the sixth member of the parallel-dispatcher family that already
// includes Hensel slot (W7), ECM Stage 1/2 (W8 T1 / W9 T1), LP merger
// (W10 T4), mpz_powm (W11 T3) and lattice basis (W11 T4) dispatchers.
//
// Coverage:
//   * ENV parsing handles unset / "0" / "4" / "10000" / "12abc" and extreme
//     positive values with the
//     same clamping mirror as the W7/W8/W9/W10 T4/W11 T3/W11 T4 helpers
//     (default 1, cap at hardware_concurrency * 2, leading numeric
//     prefix accepted).
//   * Sequential (N=1, default) and parallel (N>=2) paths produce
//     identical per-tile outcomes for the same input. The dispatcher is
//     a pure wrapper around a caller-supplied `tile_fn` callable, so a
//     deterministic mock workload (no real sieve fixture required)
//     drives the parity assertions.
//   * Empty tile_count returns empty vector cleanly without creating a
//     pool or invoking the tile functor.
//   * Single tile under N>=2 short-circuits to sequential (exactly-once
//     invocation, no stall).
//   * 100-tile parity across N=1 vs N=4 vs N=hw with a simple Result =
//     int workload and a heavier Result struct {int + std::vector<int>}.
//   * Move-only Result type (carrying std::unique_ptr) flows through
//     the dispatcher without lost ownership or double-free.
//   * tile_fn throwing propagates the first exception under both N=1
//     and N=4 dispatch.
//   * `resolve_sieve_apply_tile_threads(tile_count)` correctly clamps
//     by tile_count under sequential / parallel envs.
//   * Cache reset hook re-reads ENV.

#include <gnfs/sieve/apply_tile_parallel.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using gnfs::sieve::parallel_apply_tiles;
using gnfs::sieve::resolve_sieve_apply_tile_threads;
using gnfs::sieve::sieve_apply_tile_threads;
using gnfs::sieve::sieve_apply_tile_threads_reset_env_cache_for_testing;

namespace {

// Mock per-tile workload result. Stand-in for a real candidate-emit
// buffer; using a tiny POD whose value is a deterministic function of
// tile_index keeps per-index parity byte-exact.
struct MockTileResult {
    uint64_t value;

    bool operator==(const MockTileResult& other) const noexcept {
        return value == other.value;
    }
};

// Heavier mock workload result. Carries an `int` summary plus a
// per-tile std::vector so the dispatcher's per-slot move assignment is
// stressed with a non-trivial type.
struct HeavyTileResult {
    int summary = 0;
    std::vector<int> data;

    bool operator==(const HeavyTileResult& other) const noexcept {
        return summary == other.summary && data == other.data;
    }
};

// Deterministic per-index "scan" - emulates the per-tile tile_fn the
// helper will dispatch in a future wire-in. Pure function of
// tile_index, no shared state, no GMP. Choice of multiplier 1009 and
// per-bit XOR mix is arbitrary but stable across runs so N=1 vs N>=2
// paths compare bit-for-bit.
MockTileResult mock_scan(std::size_t i) {
    uint64_t v = static_cast<uint64_t>(i) * 1009ULL + 7ULL;
    v ^= (v >> 13);
    v *= 0x9E3779B97F4A7C15ULL;
    v ^= (v >> 11);
    return MockTileResult{v};
}

// Heavier mock scan - emits a (i+1)-element vector populated with
// tile-derived values. Mirrors a real apply-phase tile that emits a
// vector of candidates whose count depends on the tile contents.
HeavyTileResult heavy_scan(std::size_t i) {
    HeavyTileResult r;
    r.summary = static_cast<int>(i * 7 + 3);
    const std::size_t count = (i % 5) + 1;
    r.data.reserve(count);
    for (std::size_t k = 0; k < count; ++k) {
        r.data.push_back(static_cast<int>(i * 31 + k));
    }
    return r;
}

// Helper: set or unset the env var and refresh the cached parse result
// so the next call to sieve_apply_tile_threads() reflects the new
// value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_SIEVE_APPLY_TILE_THREADS");
    } else {
        setenv("GNFS_SIEVE_APPLY_TILE_THREADS", value, /*overwrite=*/1);
    }
    sieve_apply_tile_threads_reset_env_cache_for_testing();
}

// ───────────────────────────────────────────────────────────────────────
// Test 1: ENV unset -> 1 (default sequential)
// ───────────────────────────────────────────────────────────────────────
void test_env_unset_defaults_to_one() {
    std::cout << "Test 1: ENV unset -> 1..." << std::flush;
    apply_env(nullptr);
    int v = sieve_apply_tile_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: unset env parsed to " << v << ", expected 1" << std::endl;
        std::abort();
    }
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 2: ENV "0" -> 1 (invalid, non-positive -> sequential)
// ───────────────────────────────────────────────────────────────────────
void test_env_zero_to_one() {
    std::cout << "Test 2: ENV '0' -> 1..." << std::flush;
    apply_env("0");
    int v = sieve_apply_tile_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: '0' parsed to " << v << ", expected 1" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 3: ENV "4" -> 4 (or clamped to cap on tiny CI runners)
// ───────────────────────────────────────────────────────────────────────
void test_env_four() {
    std::cout << "Test 3: ENV '4' -> 4..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (4 < cap) ? 4 : cap;

    apply_env("4");
    int v = sieve_apply_tile_threads();
    if (v != expect) {
        std::cerr << "\n  ERROR: '4' parsed to " << v << ", expected " << expect
                  << " (hw*2 cap = " << cap << ")" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (parsed " << v << ", cap " << cap << ")\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 4: ENV "10000" -> clamped at hardware_concurrency() * 2
// ───────────────────────────────────────────────────────────────────────
void test_env_clamp_at_hw_times_two() {
    std::cout << "Test 4: ENV '10000' clamped at hw*2..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;

    apply_env("10000");
    int v = sieve_apply_tile_threads();
    if (v != cap) {
        std::cerr << "\n  ERROR: '10000' parsed to " << v << ", expected cap=" << cap << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (cap=" << cap << ")\n";
}

// -----------------------------------------------------------------------
// Test 5: positive values beyond int/uint64 range still clamp high.
// -----------------------------------------------------------------------
void test_env_extreme_positive_clamps() {
    std::cout << "Test 5: extreme positive ENV values clamp at hw*2..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;

    apply_env("2147483648");
    int v = sieve_apply_tile_threads();
    if (v != cap) {
        std::cerr << "\n  ERROR: positive int-range overflow parsed to " << v
                  << ", expected cap=" << cap << std::endl;
        std::abort();
    }

    apply_env("999999999999999999999999999999");
    v = sieve_apply_tile_threads();
    if (v != cap) {
        std::cerr << "\n  ERROR: huge positive value parsed to " << v << ", expected cap=" << cap
                  << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS (cap=" << cap << ")\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 5: ENV "12abc" -> 12 (std::atoi partial-parse, documented
//          dispatcher-family behaviour)
// ───────────────────────────────────────────────────────────────────────
void test_env_numeric_prefix() {
    std::cout << "Test 5: ENV '12abc' -> 12 (prefix parse)..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (12 < cap) ? 12 : cap;

    apply_env("12abc");
    int v = sieve_apply_tile_threads();
    if (v != expect) {
        std::cerr << "\n  ERROR: '12abc' parsed to " << v << ", expected " << expect
                  << " (hw*2 cap = " << cap << ")" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (parsed " << v << ", cap " << cap << ")\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 6: empty tile_count - both N=1 and N=4 return empty vector
//          cleanly without creating a pool or invoking tile_fn.
// ───────────────────────────────────────────────────────────────────────
void test_empty_tile_count() {
    std::cout << "Test 6: empty tile_count (no-op both paths)..." << std::flush;

    // Sequential (N=1).
    apply_env("1");
    auto seq_results = parallel_apply_tiles<MockTileResult>(
        /*tile_count=*/0, [](std::size_t) -> MockTileResult {
            std::cerr << "\n  ERROR: tile_fn invoked on empty count" << std::endl;
            std::abort();
            return MockTileResult{};
        });
    assert(seq_results.empty());

    // Parallel (N=4).
    apply_env("4");
    auto par_results = parallel_apply_tiles<MockTileResult>(
        /*tile_count=*/0, [](std::size_t) -> MockTileResult {
            std::cerr << "\n  ERROR: tile_fn invoked on empty count" << std::endl;
            std::abort();
            return MockTileResult{};
        });
    assert(par_results.empty());

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 7: single tile, N=1 - result correct, sequential path exercised.
// ───────────────────────────────────────────────────────────────────────
void test_single_tile_n1() {
    std::cout << "Test 7: single tile N=1 (result correct)..." << std::flush;

    apply_env("1");
    auto results = parallel_apply_tiles<MockTileResult>(1, mock_scan);

    assert(results.size() == 1);
    MockTileResult expect = mock_scan(0);
    if (!(results[0] == expect)) {
        std::cerr << "\n  ERROR: got value=" << results[0].value << ", expected " << expect.value
                  << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (value=" << results[0].value << ")\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 8: single tile, N=4 - short-circuit to sequential, exactly-once
//          invocation, no stall on pool spin-up.
// ───────────────────────────────────────────────────────────────────────
void test_single_tile_n4_no_stall() {
    std::cout << "Test 8: single tile N=4 (exactly-once, no stall)..." << std::flush;

    apply_env("4");
    std::atomic<int> calls{0};

    auto t0 = std::chrono::steady_clock::now();
    auto results =
        parallel_apply_tiles<MockTileResult>(1, [&calls](std::size_t i) -> MockTileResult {
            calls.fetch_add(1, std::memory_order_relaxed);
            return mock_scan(i);
        });
    auto t1 = std::chrono::steady_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    assert(results.size() == 1);
    MockTileResult expect = mock_scan(0);
    if (!(results[0] == expect)) {
        std::cerr << "\n  ERROR: got value=" << results[0].value << ", expected " << expect.value
                  << std::endl;
        std::abort();
    }
    int total_calls = calls.load(std::memory_order_relaxed);
    if (total_calls != 1) {
        std::cerr << "\n  ERROR: expected exactly 1 call, got " << total_calls << std::endl;
        std::abort();
    }

    // Sanity-bound the wall-time: if the helper accidentally spawned a
    // 4-thread pool the spin-up alone would push past this.
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-tile dispatch took " << ms << " ms (expected << 1000 ms)"
                  << std::endl;
        // No abort - sanitizers can be slow.
    }

    apply_env(nullptr);
    std::cout << " PASS (" << ms << " ms)\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 9: 100 tiles, N=1 baseline - deterministic mock_scan,
//          dispatcher returns vector aligned with input, exactly-once
//          invocation per tile.
// ───────────────────────────────────────────────────────────────────────
void test_100_tiles_n1_baseline() {
    std::cout << "Test 9: 100 tiles N=1 baseline (identity scan)..." << std::flush;
    apply_env("1");

    auto results = parallel_apply_tiles<MockTileResult>(100, mock_scan);

    if (results.size() != 100) {
        std::cerr << "\n  ERROR: expected 100 results, got " << results.size() << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < 100; ++i) {
        MockTileResult expect = mock_scan(i);
        if (!(results[i] == expect)) {
            std::cerr << "\n  ERROR: idx " << i << " got " << results[i].value << " expected "
                      << expect.value << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (100 tiles)\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 10: N=1 vs N=4 parity (simple Result = MockTileResult)
//           strict per-index bit-identical assertion across the two
//           paths.
// ───────────────────────────────────────────────────────────────────────
void test_n1_vs_n4_parity_simple() {
    std::cout << "Test 10: N=1 vs N=4 parity (simple, 100 tiles)..." << std::flush;

    apply_env("1");
    auto seq = parallel_apply_tiles<MockTileResult>(100, mock_scan);

    apply_env("4");
    auto par = parallel_apply_tiles<MockTileResult>(100, mock_scan);

    apply_env(nullptr);

    if (seq.size() != par.size()) {
        std::cerr << "\n  ERROR: seq.size()=" << seq.size() << " par.size()=" << par.size()
                  << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (!(seq[i] == par[i])) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i].value
                      << " par=" << par[i].value << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << seq.size() << " per-index identical)\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 11: N=1 vs N=hw_concurrency parity (HEAVIER workload). The
//           Result type carries an int summary plus a per-tile
//           std::vector<int> so the dispatcher's per-slot move
//           assignment is exercised with a non-trivial type. This is
//           the "heavier-workload" parity test - the dispatcher must
//           handle vector ownership across the ThreadPool boundary
//           without dropping or shredding elements.
// ───────────────────────────────────────────────────────────────────────
void test_n1_vs_n_hw_parity_heavy() {
    std::cout << "Test 11: N=1 vs N=hw parity (HeavyTileResult)..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    std::string hw_str = std::to_string(hw);

    constexpr std::size_t kTileCount = 100;

    apply_env("1");
    auto t0 = std::chrono::steady_clock::now();
    auto seq = parallel_apply_tiles<HeavyTileResult>(kTileCount, heavy_scan);
    auto t1 = std::chrono::steady_clock::now();
    long long us_seq = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    apply_env(hw_str.c_str());
    auto t2 = std::chrono::steady_clock::now();
    auto par = parallel_apply_tiles<HeavyTileResult>(kTileCount, heavy_scan);
    auto t3 = std::chrono::steady_clock::now();
    long long us_par = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

    apply_env(nullptr);

    assert(seq.size() == par.size());
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (!(seq[i] == par[i])) {
            std::cerr << "\n  ERROR: idx " << i << " seq.summary=" << seq[i].summary
                      << " par.summary=" << par[i].summary << " seq.size=" << seq[i].data.size()
                      << " par.size=" << par[i].data.size() << std::endl;
            std::abort();
        }
        // And cross-check seq[i] against the freshly computed reference.
        HeavyTileResult expect = heavy_scan(i);
        if (!(seq[i] == expect)) {
            std::cerr << "\n  ERROR: idx " << i << " seq does not match heavy_scan reference"
                      << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (N=hw=" << hw << ", " << seq.size()
              << " per-index identical, seq=" << us_seq << "us, par=" << us_par << "us)\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 12: Move-only Result type (std::unique_ptr<int>) - exercises
//           the dispatcher's per-slot std::move flow through the
//           ThreadPool boundary. A move-only Result MUST survive both
//           sequential and parallel dispatch without lost ownership or
//           double-free.
// ───────────────────────────────────────────────────────────────────────
void test_move_only_result_type() {
    std::cout << "Test 12: move-only Result (unique_ptr<int>)..." << std::flush;

    constexpr std::size_t kTileCount = 32;

    auto unique_scan = [](std::size_t i) -> std::unique_ptr<int> {
        return std::make_unique<int>(static_cast<int>(i * 13 + 5));
    };

    // Sequential.
    apply_env("1");
    auto seq = parallel_apply_tiles<std::unique_ptr<int>>(kTileCount, unique_scan);
    assert(seq.size() == kTileCount);
    for (std::size_t i = 0; i < kTileCount; ++i) {
        if (!seq[i]) {
            std::cerr << "\n  ERROR seq: null pointer at idx " << i << std::endl;
            std::abort();
        }
        int expect = static_cast<int>(i * 13 + 5);
        if (*seq[i] != expect) {
            std::cerr << "\n  ERROR seq: idx " << i << " got " << *seq[i] << " expected " << expect
                      << std::endl;
            std::abort();
        }
    }

    // Parallel.
    apply_env("4");
    auto par = parallel_apply_tiles<std::unique_ptr<int>>(kTileCount, unique_scan);
    assert(par.size() == kTileCount);
    for (std::size_t i = 0; i < kTileCount; ++i) {
        if (!par[i]) {
            std::cerr << "\n  ERROR par: null pointer at idx " << i << std::endl;
            std::abort();
        }
        int expect = static_cast<int>(i * 13 + 5);
        if (*par[i] != expect) {
            std::cerr << "\n  ERROR par: idx " << i << " got " << *par[i] << " expected " << expect
                      << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (" << kTileCount << " unique_ptr per index, both paths)\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 13: tile_fn throws - exception propagates to caller (does not
//           swallow). Verified under both N=1 and N=4 paths.
// ───────────────────────────────────────────────────────────────────────
void test_tile_fn_exception_propagates() {
    std::cout << "Test 13: tile_fn exception propagates..." << std::flush;

    constexpr std::size_t kTileCount = 8;

    auto throw_fn = [](std::size_t i) -> MockTileResult {
        if (i == 4) {
            throw std::runtime_error("tile 4 sentinel");
        }
        return MockTileResult{static_cast<uint64_t>(i * 10ULL)};
    };

    // Sequential path: exception must rethrow.
    apply_env("1");
    bool seq_caught = false;
    try {
        auto seq = parallel_apply_tiles<MockTileResult>(kTileCount, throw_fn);
        (void)seq;
    } catch (const std::runtime_error& e) {
        seq_caught = true;
        std::string what = e.what();
        if (what.find("tile 4 sentinel") == std::string::npos) {
            std::cerr << "\n  ERROR: seq path unexpected what(): " << what << std::endl;
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
        auto par = parallel_apply_tiles<MockTileResult>(kTileCount, throw_fn);
        (void)par;
    } catch (const std::runtime_error& e) {
        par_caught = true;
        std::string what = e.what();
        if (what.find("tile 4 sentinel") == std::string::npos) {
            std::cerr << "\n  ERROR: par path unexpected what(): " << what << std::endl;
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

// ───────────────────────────────────────────────────────────────────────
// Test 14: reset_env_cache hook - toggling the env between assertions
//           without resetting the cache should reuse the stale parsed
//           value; after reset, the new value should take effect.
// ───────────────────────────────────────────────────────────────────────
void test_reset_env_cache_hook() {
    std::cout << "Test 14: reset_env_cache hook..." << std::flush;

    // Establish baseline cached value of 1 from unset env.
    apply_env(nullptr);
    int v1 = sieve_apply_tile_threads();
    assert(v1 == 1);

    // Set env to 4, but DO NOT call reset - cache still holds 1.
    setenv("GNFS_SIEVE_APPLY_TILE_THREADS", "4", /*overwrite=*/1);
    int v2 = sieve_apply_tile_threads();
    if (v2 != 1) {
        std::cerr << "\n  ERROR: stale cache should still report 1, got " << v2 << std::endl;
        std::abort();
    }

    // Now reset; the new "4" parse should take effect.
    sieve_apply_tile_threads_reset_env_cache_for_testing();
    int v3 = sieve_apply_tile_threads();
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (4 < cap) ? 4 : cap;
    if (v3 != expect) {
        std::cerr << "\n  ERROR: post-reset got " << v3 << ", expected " << expect << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────
// Test 15: resolve_sieve_apply_tile_threads() - sanity on
//           tile_count=0, single-tile clamp, parallel clamp by
//           tile_count.
// ───────────────────────────────────────────────────────────────────────
void test_resolve_helper() {
    std::cout << "Test 15: resolve_sieve_apply_tile_threads()..." << std::flush;

    // Empty -> 0 (no work).
    apply_env("4");
    int r0 = resolve_sieve_apply_tile_threads(0);
    if (r0 != 0) {
        std::cerr << "\n  ERROR: tile_count=0 should give 0, got " << r0 << std::endl;
        std::abort();
    }

    // Single tile -> 1 (short-circuit even when env=4).
    int r1 = resolve_sieve_apply_tile_threads(1);
    if (r1 != 1) {
        std::cerr << "\n  ERROR: tile_count=1 should give 1, got " << r1 << std::endl;
        std::abort();
    }

    // env=1 / tile_count=100 -> 1 (sequential).
    apply_env("1");
    int rs = resolve_sieve_apply_tile_threads(100);
    if (rs != 1) {
        std::cerr << "\n  ERROR: env=1 / 100 tiles should give 1, got " << rs << std::endl;
        std::abort();
    }

    // env=4 / tile_count=100 -> 4 (env wins).
    apply_env("4");
    int rp = resolve_sieve_apply_tile_threads(100);
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect_4 = (4 < cap) ? 4 : cap;
    if (rp != expect_4) {
        std::cerr << "\n  ERROR: env=4 / 100 tiles should give " << expect_4 << ", got " << rp
                  << std::endl;
        std::abort();
    }

    // env=4 / tile_count=2 -> 2 (tile_count clamps env).
    int rc = resolve_sieve_apply_tile_threads(2);
    int expect_2 = std::min(expect_4, 2);
    if (rc != expect_2) {
        std::cerr << "\n  ERROR: env=4 / 2 tiles should give " << expect_2 << ", got " << rc
                  << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

} // namespace

int main() {
    std::cout << "=== Sieve Apply-Tile Parallel Dispatch Tests ===" << std::endl;

    test_env_unset_defaults_to_one();
    test_env_zero_to_one();
    test_env_four();
    test_env_clamp_at_hw_times_two();
    test_env_extreme_positive_clamps();
    test_env_numeric_prefix();
    test_empty_tile_count();
    test_single_tile_n1();
    test_single_tile_n4_no_stall();
    test_100_tiles_n1_baseline();
    test_n1_vs_n4_parity_simple();
    test_n1_vs_n_hw_parity_heavy();
    test_move_only_result_type();
    test_tile_fn_exception_propagates();
    test_reset_env_cache_hook();
    test_resolve_helper();

    std::cout << std::endl << "=== All Sieve Apply-Tile Parallel Tests PASSED ===" << std::endl;
    return 0;
}
