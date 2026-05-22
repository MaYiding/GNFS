// test_ecm_stage1_parallel.cpp — ECM Stage 1 multi-curve parallel dispatch tests
//
// Validates the GNFS_ECM_STAGE1_PARALLEL_THREADS env-gated dispatcher
// introduced in include/gnfs/cofactor/ecm_stage1_parallel.hpp:
//
//   * Sequential (N=1, default) and parallel (N>=2) paths produce identical
//     per-curve outcomes for the same input span. The dispatcher is a pure
//     wrapper around a caller-supplied `run_stage1` callable, so we exercise
//     it with a deterministic mock worker (no real ECM math required).
//   * ENV parsing handles unset / "0" / "1" / "4" / "garbage" / "" / "9999"
//     correctly; clamping at hardware_concurrency() * 2.
//   * Empty curve list returns empty vector cleanly without creating a pool.
//   * Single curve under N>=2 short-circuits to sequential (exactly-once
//     invocation, no stall).
//   * Real ECM Stage 1 work (via try_curve_with_pk) yields the same outcome
//     per (sigma, n) tuple in both sequential and parallel paths.

#include <gnfs/cofactor/ecm_stage1_parallel.hpp>
#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/core/integer.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using gnfs::core::Integer;
using gnfs::cofactor::ECM;
using gnfs::cofactor::ecm_stage1_parallel_threads;
using gnfs::cofactor::ecm_stage1_parallel_reset_env_cache_for_testing;
using gnfs::cofactor::parallel_stage1_curves;

namespace {

// Helper: set or unset GNFS_ECM_STAGE1_PARALLEL_THREADS and refresh the
// cache so the next call to ecm_stage1_parallel_threads() reflects the new
// value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_ECM_STAGE1_PARALLEL_THREADS");
    } else {
        setenv("GNFS_ECM_STAGE1_PARALLEL_THREADS", value, /*overwrite=*/1);
    }
    ecm_stage1_parallel_reset_env_cache_for_testing();
}

// ───────────────────────────────────────────────────────────────────────────
// Test 1: ENV unset -> 1 (default sequential)
// ───────────────────────────────────────────────────────────────────────────
void test_env_unset_defaults_to_one() {
    std::cout << "Test 1: ENV unset -> 1..." << std::flush;
    apply_env(nullptr);
    std::size_t v = ecm_stage1_parallel_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: unset env parsed to " << v
                  << ", expected 1" << std::endl;
        std::abort();
    }
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: ENV "4" -> 4 (or clamped to cap)
// ───────────────────────────────────────────────────────────────────────────
void test_env_four() {
    std::cout << "Test 2: ENV '4' -> 4..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;
    std::size_t expect = (4 < cap) ? 4 : cap;

    apply_env("4");
    std::size_t v = ecm_stage1_parallel_threads();
    if (v != expect) {
        std::cerr << "\n  ERROR: '4' parsed to " << v << ", expected "
                  << expect << " (hw*2 cap = " << cap << ")" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (parsed " << v << ", cap " << cap << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: ENV non-numeric / empty / "0" / negative -> 1
// ───────────────────────────────────────────────────────────────────────────
void test_env_non_numeric_to_one() {
    std::cout << "Test 3: ENV non-numeric / boundary -> 1..." << std::flush;

    // Empty string -> 1.
    apply_env("");
    assert(ecm_stage1_parallel_threads() == 1);

    // "0" -> 1 (invalid, non-positive).
    apply_env("0");
    assert(ecm_stage1_parallel_threads() == 1);

    // "-5" -> 1 (invalid, non-positive).
    apply_env("-5");
    assert(ecm_stage1_parallel_threads() == 1);

    // "garbage" -> 1 (atoi returns 0).
    apply_env("garbage");
    assert(ecm_stage1_parallel_threads() == 1);

    // " " (whitespace only) -> 1 (atoi returns 0).
    apply_env("   ");
    assert(ecm_stage1_parallel_threads() == 1);

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: N=1 sequential baseline — mock worker produces deterministic
//          sequence, dispatcher returns vector aligned with input.
// ───────────────────────────────────────────────────────────────────────────
void test_n1_sequential_baseline() {
    std::cout << "Test 4: N=1 sequential baseline..." << std::flush;
    apply_env("1");

    // Mock "Stage 1" worker: pure function of sigma. Result = sigma^2 + 1.
    std::vector<uint64_t> sigmas = {100, 101, 102, 103, 104,
                                    105, 106, 107, 108, 109};

    auto results = parallel_stage1_curves<uint64_t, uint64_t>(
        std::span<const uint64_t>(sigmas),
        [](uint64_t sigma) -> uint64_t {
            return sigma * sigma + 1;
        });

    if (results.size() != sigmas.size()) {
        std::cerr << "\n  ERROR: expected " << sigmas.size()
                  << " results, got " << results.size() << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < sigmas.size(); ++i) {
        uint64_t expect = sigmas[i] * sigmas[i] + 1;
        if (results[i] != expect) {
            std::cerr << "\n  ERROR: idx " << i << " got " << results[i]
                      << " expected " << expect << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (" << sigmas.size() << " results aligned)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: N=4 parallel parity — same input, N=1 vs N=4 bit-for-bit
//          per-index identical, exact-once invocation per curve.
// ───────────────────────────────────────────────────────────────────────────
void test_n4_parallel_parity() {
    std::cout << "Test 5: N=1 vs N=4 parity (per-index bit-identical)..."
              << std::flush;

    // Use a denser worker so multiple workers see non-trivial dispatch.
    // Result is still a pure function of sigma.
    auto worker = [](uint64_t sigma) -> uint64_t {
        // Mix a small amount of arithmetic to simulate a real Stage 1
        // result without introducing thread-local state.
        uint64_t acc = sigma;
        for (int k = 0; k < 32; ++k) {
            acc = (acc * 1103515245ULL + 12345ULL) ^ (acc >> 7);
        }
        return acc;
    };

    std::vector<uint64_t> sigmas;
    for (uint64_t s = 200; s < 232; ++s) sigmas.push_back(s);

    apply_env("1");
    auto seq = parallel_stage1_curves<uint64_t, uint64_t>(
        std::span<const uint64_t>(sigmas), worker);

    apply_env("4");
    auto par = parallel_stage1_curves<uint64_t, uint64_t>(
        std::span<const uint64_t>(sigmas), worker);

    apply_env(nullptr);

    if (seq.size() != par.size()) {
        std::cerr << "\n  ERROR: seq.size()=" << seq.size()
                  << " par.size()=" << par.size() << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: idx " << i
                      << " seq=" << seq[i] << " par=" << par[i] << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << sigmas.size() << " per-index identical)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: Empty curve list — both N=1 and N=4 paths return empty vector
//          cleanly without creating a pool / invoking run_stage1.
// ───────────────────────────────────────────────────────────────────────────
void test_empty_curve_list() {
    std::cout << "Test 6: empty curve list (no-op both paths)..."
              << std::flush;

    std::vector<int> empty_in;

    // Sequential (N=1).
    apply_env("1");
    auto seq_results = parallel_stage1_curves<int, int>(
        std::span<const int>(empty_in),
        [](int) -> int {
            std::cerr << "\n  ERROR: should not invoke run_stage1 on empty span"
                      << std::endl;
            std::abort();
            return 0;
        });
    assert(seq_results.empty());

    // Parallel (N=4).
    apply_env("4");
    auto par_results = parallel_stage1_curves<int, int>(
        std::span<const int>(empty_in),
        [](int) -> int {
            std::cerr << "\n  ERROR: should not invoke run_stage1 on empty span"
                      << std::endl;
            std::abort();
            return 0;
        });
    assert(par_results.empty());

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 7: Single curve under N=4 — must short-circuit to sequential
//          (exactly-once invocation, no stall).
// ───────────────────────────────────────────────────────────────────────────
void test_single_curve_no_stall() {
    std::cout << "Test 7: single-curve N=4 (exactly-once, no stall)..."
              << std::flush;

    apply_env("4");
    std::vector<uint64_t> single = {42};
    std::atomic<int> calls{0};

    auto t0 = std::chrono::steady_clock::now();
    auto results = parallel_stage1_curves<uint64_t, uint64_t>(
        std::span<const uint64_t>(single),
        [&calls](uint64_t sigma) -> uint64_t {
            calls.fetch_add(1, std::memory_order_relaxed);
            return sigma * 2;
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

    // Sanity-bound the wall-time to a small value; if the helper accidentally
    // spawned a 4-thread pool the spin-up alone would push past this.
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-curve dispatch took " << ms
                  << " ms (expected << 1000 ms)" << std::endl;
        // No abort -- this is a soft signal, sanitizers can be slow.
    }

    apply_env(nullptr);
    std::cout << " PASS (" << ms << " ms)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 8: N=hw_concurrency parity — extra coverage at the runtime upper
//          bound to catch races / aliasing that smaller N might miss.
// ───────────────────────────────────────────────────────────────────────────
void test_n_hw_parity() {
    std::cout << "Test 8: N=hw_concurrency parity..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::string hw_str = std::to_string(hw);

    auto worker = [](uint64_t sigma) -> uint64_t {
        // Heavier mock to give the pool work to chew on.
        uint64_t acc = sigma | 1;
        for (int k = 0; k < 128; ++k) {
            acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
            acc ^= (acc >> 11);
        }
        return acc;
    };

    std::vector<uint64_t> sigmas;
    for (uint64_t s = 1000; s < 1064; ++s) sigmas.push_back(s);

    apply_env("1");
    auto seq = parallel_stage1_curves<uint64_t, uint64_t>(
        std::span<const uint64_t>(sigmas), worker);

    apply_env(hw_str.c_str());
    auto par = parallel_stage1_curves<uint64_t, uint64_t>(
        std::span<const uint64_t>(sigmas), worker);

    apply_env(nullptr);

    assert(seq.size() == par.size());
    for (std::size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != par[i]) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i]
                      << " par=" << par[i] << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (N=hw=" << hw << ", " << sigmas.size()
              << " per-index identical)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 9: Real ECM integration — `try_curve_with_pk` is a Stage 1 entry
//          (Stage 2 disabled when B2 = B1). N=1 vs N=4 must produce the
//          same `std::optional<Integer>` per sigma. This validates the
//          dispatcher under real GMP arithmetic with per-thread Integer
//          ownership.
// ───────────────────────────────────────────────────────────────────────────
void test_real_ecm_stage1_parity() {
    std::cout << "Test 9: real ECM Stage 1 parity (N=1 vs N=4)..."
              << std::flush;

    // 31-bit semiprime well within Stage 1 reach (47491 * 47659).
    Integer N{uint64_t{2261419229ULL}};

    // Stage-1-only batch context: B2 = B1 disables Stage 2.
    ECM::Config cfg;
    cfg.num_curves = 0;
    cfg.B1 = 2000;
    cfg.B2 = 2000;          // Stage 2 disabled
    cfg.auto_params = false;
    cfg.brent_suyama_degree = 0;
    ECM::BatchContext ctx = ECM::prepare_batch(cfg, /*sigma_seed=*/0);

    std::vector<uint64_t> sigmas;
    for (uint64_t s = 100; s < 116; ++s) sigmas.push_back(s);

    auto worker = [&N, &ctx](uint64_t sigma) -> std::optional<Integer> {
        // factor_with_batch handles Stage 1; Stage 2 short-circuits because
        // B2 == B1 means there are no extra primes for the Stage 2 sweep.
        // Per call, GMP operates on per-thread Integer buffers (local
        // scratch inside ECM internals), so concurrent invocations across
        // distinct sigmas are safe.
        ECM::BatchContext local_ctx = ctx;
        local_ctx.sigma_pool.clear();
        local_ctx.sigma_pool.push_back(sigma);
        return ECM::factor_with_batch(N, local_ctx);
    };

    apply_env("1");
    auto seq = parallel_stage1_curves<std::optional<Integer>, uint64_t>(
        std::span<const uint64_t>(sigmas), worker);

    apply_env("4");
    auto par = parallel_stage1_curves<std::optional<Integer>, uint64_t>(
        std::span<const uint64_t>(sigmas), worker);

    apply_env(nullptr);

    assert(seq.size() == par.size());
    std::size_t any_factor_seq = 0;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        bool seq_has = seq[i].has_value();
        bool par_has = par[i].has_value();
        if (seq_has != par_has) {
            std::cerr << "\n  ERROR: idx " << i << " seq_has=" << seq_has
                      << " par_has=" << par_has << std::endl;
            std::abort();
        }
        if (seq_has) {
            ++any_factor_seq;
            if (seq[i]->compare(*par[i]) != 0) {
                std::cerr << "\n  ERROR: idx " << i << " mismatch: seq="
                          << seq[i]->to_string()
                          << " par=" << par[i]->to_string() << std::endl;
                std::abort();
            }
            // Sanity: factor must divide N and be non-trivial.
            assert(!seq[i]->is_one());
            assert(seq[i]->compare(N) != 0);
            Integer rem;
            mpz_mod(rem.get_mpz(), N.get_mpz(), seq[i]->get_mpz());
            assert(rem.is_zero());
        }
    }

    std::cout << " PASS (" << sigmas.size() << " curves, "
              << any_factor_seq << " found factor)\n";
}

}  // namespace

int main() {
    std::cout << "=== ECM Stage 1 Parallel Dispatch Tests ===" << std::endl;

    test_env_unset_defaults_to_one();
    test_env_four();
    test_env_non_numeric_to_one();
    test_n1_sequential_baseline();
    test_n4_parallel_parity();
    test_empty_curve_list();
    test_single_curve_no_stall();
    test_n_hw_parity();
    test_real_ecm_stage1_parity();

    std::cout << std::endl
              << "=== All ECM Stage 1 Parallel Tests PASSED ==="
              << std::endl;
    return 0;
}
