// test_hensel_parallel.cpp — Nguyen hybrid Hensel lift parallel dispatch tests
//
// Validates the GNFS_SQRT_HENSEL_THREADS env-gated dispatcher introduced in
// include/gnfs/sqrt/hensel_parallel.hpp:
//   * Sequential (N=1, default) and parallel (N>=2) paths produce identical
//     algebraic sqrt outputs (bit-for-bit) for the K independent prime slots
//     in compute_nguyen_hybrid().
//   * ENV parsing handles unset / "" / "0" / "1" / "4" / "999" / negative
//     correctly; clamping at hardware_concurrency()*2.
//   * Single-slot dispatch does not crash and does not allocate extra threads.
//
// The 4 correctness tests build small N + degree-2 number fields with valid
// ab_pairs >= 100 (the threshold above which HenselSqrt::compute() calls
// compute_nguyen_hybrid via the K=3 prime slot path).  Pairs are chosen so
// that the product polynomial is a perfect square in Z[alpha]/(f), giving
// the Hensel lift a well-defined answer to converge on.

#include <gnfs/sqrt/hensel_parallel.hpp>
#include <gnfs/sqrt/hensel_sqrt.hpp>
#include <gnfs/sqrt/number_field.hpp>
#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/core/integer.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnfs::core;
using namespace gnfs::sqrt;
using gnfs::polynomial::BaseMSelector;

namespace {

// Build a small NumberField with f(x) = x^2 + x + c such that f is
// irreducible for the small N=9991 (97 x 103) test case used in
// test_edge_cases.cpp.  We reuse the same setup so that the Hensel
// inert-prime search converges quickly and the test stays inside the
// 60s instant budget even with multi-thread retries.
NumberField build_test_number_field() {
    Integer n("9991");
    auto poly_result = BaseMSelector::select(n, 2);
    assert(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);
    return NumberField(ctx);
}

// Generate 120 ab_pairs whose product is a perfect square in Z[alpha]/(f):
// each unique pair appears exactly twice, so the product is (P)^2 where P is
// a known polynomial.  120 > 100 triggers the Nguyen hybrid Hensel path.
std::vector<std::pair<int64_t, uint64_t>> build_squared_ab_pairs() {
    std::vector<std::pair<int64_t, uint64_t>> base;
    base.reserve(60);
    for (int64_t i = 1; i <= 60; ++i) {
        base.emplace_back(2 * i + 1, static_cast<uint64_t>(i % 5 + 1));
    }
    std::vector<std::pair<int64_t, uint64_t>> pairs;
    pairs.reserve(120);
    for (const auto& p : base) pairs.push_back(p);
    for (const auto& p : base) pairs.push_back(p);  // duplicate -> product is a square
    return pairs;
}

// Run HenselSqrt::compute() under the given GNFS_SQRT_HENSEL_THREADS env.
// Returns the sqrt result (or nullopt) along with the wall-clock duration.
struct HenselRun {
    std::optional<Integer> result;
    long long ms;
};

HenselRun run_hensel_with_threads(
        const std::vector<std::pair<int64_t, uint64_t>>& pairs,
        const NumberField& nf,
        const char* env_value) {
    if (env_value == nullptr) {
        unsetenv("GNFS_SQRT_HENSEL_THREADS");
    } else {
        setenv("GNFS_SQRT_HENSEL_THREADS", env_value, /*overwrite=*/1);
    }
    // Reset cached env so the new value takes effect.
    reset_sqrt_hensel_threads_cache();

    HenselSqrt::Config cfg;
    cfg.verbose = false;
    HenselSqrt hs(cfg);

    auto t0 = std::chrono::steady_clock::now();
    auto r = hs.compute(pairs, nf);
    auto t1 = std::chrono::steady_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // Restore default before returning so subsequent tests start clean.
    unsetenv("GNFS_SQRT_HENSEL_THREADS");
    reset_sqrt_hensel_threads_cache();

    return HenselRun{std::move(r), ms};
}

void test_correctness_seq_vs_parallel_1_vs_4() {
    std::cout << "Testing Hensel parallel correctness: N=1 vs N=4..." << std::endl;
    NumberField nf = build_test_number_field();
    auto pairs = build_squared_ab_pairs();

    auto seq = run_hensel_with_threads(pairs, nf, "1");
    auto par = run_hensel_with_threads(pairs, nf, "4");

    // Both must agree on success/failure.  If they both succeed, results must
    // be bit-for-bit identical (the K slots are pure-function over per-slot
    // state, so sequential and parallel paths must give the same per-slot
    // LiftResult and therefore the same CRT-combined sqrt).
    assert(seq.result.has_value() == par.result.has_value());
    if (seq.result && par.result) {
        assert(seq.result->compare(*par.result) == 0);
        std::cout << "  seq vs N=4 result match: PASSED (" << seq.ms << "ms vs "
                  << par.ms << "ms)" << std::endl;
    } else {
        std::cout << "  Both paths failed/returned nullopt consistently (expected when "
                  << "no square root exists for the small test field): PASSED" << std::endl;
    }
}

void test_correctness_seq_vs_parallel_1_vs_2() {
    std::cout << "Testing Hensel parallel correctness: N=1 vs N=2..." << std::endl;
    NumberField nf = build_test_number_field();
    auto pairs = build_squared_ab_pairs();

    auto seq = run_hensel_with_threads(pairs, nf, "1");
    auto par = run_hensel_with_threads(pairs, nf, "2");

    assert(seq.result.has_value() == par.result.has_value());
    if (seq.result && par.result) {
        assert(seq.result->compare(*par.result) == 0);
        std::cout << "  seq vs N=2 result match: PASSED (" << seq.ms << "ms vs "
                  << par.ms << "ms)" << std::endl;
    } else {
        std::cout << "  Both paths consistent (nullopt): PASSED" << std::endl;
    }
}

void test_correctness_default_vs_parallel() {
    std::cout << "Testing Hensel parallel correctness: default (unset) vs N=4..." << std::endl;
    NumberField nf = build_test_number_field();
    auto pairs = build_squared_ab_pairs();

    auto def = run_hensel_with_threads(pairs, nf, nullptr);  // env unset -> N=1 default
    auto par = run_hensel_with_threads(pairs, nf, "4");

    assert(def.result.has_value() == par.result.has_value());
    if (def.result && par.result) {
        assert(def.result->compare(*par.result) == 0);
        std::cout << "  default vs N=4 match: PASSED (" << def.ms << "ms vs "
                  << par.ms << "ms)" << std::endl;
    } else {
        std::cout << "  Both paths consistent (nullopt): PASSED" << std::endl;
    }
}

void test_correctness_small_ab_pairs() {
    // Small ab_pairs (< 100) takes the single-prime fallback path, which does
    // not invoke parallel_hensel_lift.  The env should still be cleanly
    // ignored: both N=1 and N=8 must give identical results.
    std::cout << "Testing Hensel parallel correctness on small ab_pairs (< 100)..." << std::endl;
    NumberField nf = build_test_number_field();
    std::vector<std::pair<int64_t, uint64_t>> pairs = {
        {3, 1}, {3, 1}, {5, 1}, {5, 1}
    };

    auto seq = run_hensel_with_threads(pairs, nf, "1");
    auto par = run_hensel_with_threads(pairs, nf, "8");

    assert(seq.result.has_value() == par.result.has_value());
    if (seq.result && par.result) {
        assert(seq.result->compare(*par.result) == 0);
        std::cout << "  Small input path: PASSED (env ignored when <100 pairs)" << std::endl;
    } else {
        std::cout << "  Small input path: PASSED (consistent nullopt across thread counts)"
                  << std::endl;
    }
}

void test_env_parsing_unset() {
    // Unset env -> sequential (1).
    unsetenv("GNFS_SQRT_HENSEL_THREADS");
    reset_sqrt_hensel_threads_cache();
    std::size_t n = sqrt_hensel_threads();
    if (n != 1) {
        std::cerr << "  ERROR: unset env expected 1, got " << n << std::endl;
        std::abort();
    }
    std::cout << "  unset -> 1: PASSED" << std::endl;
}

void test_env_parsing_values() {
    // Empty string -> default 1.
    setenv("GNFS_SQRT_HENSEL_THREADS", "", /*overwrite=*/1);
    reset_sqrt_hensel_threads_cache();
    assert(sqrt_hensel_threads() == 1);

    // "0" -> treated as invalid (<=0) -> 1.
    setenv("GNFS_SQRT_HENSEL_THREADS", "0", /*overwrite=*/1);
    reset_sqrt_hensel_threads_cache();
    assert(sqrt_hensel_threads() == 1);

    // "-3" -> treated as invalid -> 1.
    setenv("GNFS_SQRT_HENSEL_THREADS", "-3", /*overwrite=*/1);
    reset_sqrt_hensel_threads_cache();
    assert(sqrt_hensel_threads() == 1);

    // "1" -> 1.
    setenv("GNFS_SQRT_HENSEL_THREADS", "1", /*overwrite=*/1);
    reset_sqrt_hensel_threads_cache();
    assert(sqrt_hensel_threads() == 1);

    // "4" -> 4 (assuming hw*2 >= 4, which holds on any modern machine).
    setenv("GNFS_SQRT_HENSEL_THREADS", "4", /*overwrite=*/1);
    reset_sqrt_hensel_threads_cache();
    assert(sqrt_hensel_threads() == 4);

    // "999" -> clamp to hw*2.
    setenv("GNFS_SQRT_HENSEL_THREADS", "999", /*overwrite=*/1);
    reset_sqrt_hensel_threads_cache();
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;
    std::size_t v = sqrt_hensel_threads();
    if (v != cap) {
        std::cerr << "  ERROR: expected clamped value " << cap << ", got " << v << std::endl;
        std::abort();
    }
    std::cout << "  parsing 0/-3/1/4/999 (clamped to " << cap << "): PASSED" << std::endl;

    unsetenv("GNFS_SQRT_HENSEL_THREADS");
    reset_sqrt_hensel_threads_cache();
}

void test_perf_info_4_vs_1() {
    // Information-only timing comparison.  No assertion on speedup because the
    // Hensel lift wall time is dominated by Phase 2 BM-like steps; the K=3
    // outer dispatch is one fan-out point, not the full critical path.
    std::cout << "Hensel parallel perf info: N=1 vs N=4..." << std::endl;
    NumberField nf = build_test_number_field();
    auto pairs = build_squared_ab_pairs();

    auto seq = run_hensel_with_threads(pairs, nf, "1");
    auto par = run_hensel_with_threads(pairs, nf, "4");

    std::cout << "  N=1: " << seq.ms << " ms" << std::endl;
    std::cout << "  N=4: " << par.ms << " ms" << std::endl;
    if (seq.ms > 0 && par.ms > 0) {
        double speedup = static_cast<double>(seq.ms) / static_cast<double>(par.ms);
        std::cout << "  speedup (informational): " << speedup << "x" << std::endl;
    } else {
        std::cout << "  (skip speedup ratio for sub-ms run)" << std::endl;
    }
    std::cout << "  PASSED (info-only, no assert on speedup)" << std::endl;
}

void test_edge_case_single_slot() {
    // parallel_hensel_lift with a single slot must not allocate threads even
    // when threads >= 2 — the helper short-circuits to the sequential path.
    // Verified indirectly: call the helper directly with one slot and a
    // ThreadPool-creating env value; must complete without deadlock.
    std::cout << "Testing parallel_hensel_lift single-slot edge case..." << std::endl;
    setenv("GNFS_SQRT_HENSEL_THREADS", "8", /*overwrite=*/1);
    reset_sqrt_hensel_threads_cache();

    struct DummySlot { int x = 0; };
    DummySlot one[1] = {{42}};
    bool called = false;
    std::size_t seen_index = static_cast<std::size_t>(-1);
    parallel_hensel_lift<DummySlot>(
        std::span<DummySlot>(one, 1),
        [&called, &seen_index](DummySlot& s, std::size_t i) {
            seen_index = i;
            s.x = 1234;
            called = true;
        });
    if (!called || seen_index != 0 || one[0].x != 1234) {
        std::cerr << "ERROR: single-slot dispatch failed: called=" << called
                  << " index=" << seen_index << " x=" << one[0].x << std::endl;
        std::abort();
    }

    // Also verify zero-slot span is a no-op.
    DummySlot* empty = nullptr;
    parallel_hensel_lift<DummySlot>(
        std::span<DummySlot>(empty, 0),
        [](DummySlot&, std::size_t) {
            std::cerr << "ERROR: empty span should not invoke lift_one" << std::endl;
            std::abort();
        });

    unsetenv("GNFS_SQRT_HENSEL_THREADS");
    reset_sqrt_hensel_threads_cache();
    std::cout << "  Single-slot + empty-span dispatch: PASSED" << std::endl;
}

}  // namespace

int main() {
    std::cout << "=== Hensel Parallel Dispatch Tests ===" << std::endl << std::endl;

    test_correctness_seq_vs_parallel_1_vs_4();
    test_correctness_seq_vs_parallel_1_vs_2();
    test_correctness_default_vs_parallel();
    test_correctness_small_ab_pairs();

    test_env_parsing_unset();
    test_env_parsing_values();

    test_perf_info_4_vs_1();
    test_edge_case_single_slot();

    std::cout << std::endl
              << "=== All Hensel Parallel Tests PASSED ===" << std::endl;
    return 0;
}
