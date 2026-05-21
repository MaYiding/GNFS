// test_ecm_stage2_parallel.cpp — ECM Stage 2 multi-curve parallel dispatch tests
//
// Validates the GNFS_ECM_STAGE2_PARALLEL env-gated dispatcher introduced in
// include/gnfs/cofactor/ecm_stage2_parallel.hpp:
//
//   * Sequential (N=1, default) and parallel (N>=2) paths produce identical
//     factor sets for the same (input N, curve sigma list) tuple. Per-curve
//     Stage 2 arithmetic is a pure function of (sigma, n, B1, B2, post-
//     Stage-1 state), so dispatch order cannot change individual outcomes.
//   * ENV parsing handles unset / "0" / "1" / "8" / "garbage" / "9999"
//     correctly; clamping at hardware_concurrency() * 2.
//   * Single-curve dispatch does not crash or stall under N >= 2.
//   * Empty curve list returns empty vector cleanly.

#include <gnfs/cofactor/ecm_stage2_parallel.hpp>
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
using gnfs::cofactor::ecm_stage2_parallel_threads;
using gnfs::cofactor::ecm_stage2_parallel_reset_env_cache_for_testing;
using gnfs::cofactor::parallel_stage2_curves;

namespace {

// Helper: set or unset GNFS_ECM_STAGE2_PARALLEL and refresh the cache so the
// next call to ecm_stage2_parallel_threads() reflects the new value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_ECM_STAGE2_PARALLEL");
    } else {
        setenv("GNFS_ECM_STAGE2_PARALLEL", value, /*overwrite=*/1);
    }
    ecm_stage2_parallel_reset_env_cache_for_testing();
}

// Helper: build a deterministic sigma list (each sigma >= 6, all distinct).
std::vector<uint64_t> make_sigmas(uint64_t start, std::size_t count) {
    std::vector<uint64_t> sg;
    sg.reserve(count);
    for (std::size_t i = 0; i < count; ++i) sg.push_back(start + i);
    return sg;
}

// Helper: build a single-sigma BatchContext that exercises the full Stage 1 +
// Stage 2 path for one specific sigma. We reuse ECM::prepare_batch to share
// the prime cache construction; the sigma_pool is then overwritten with a
// single deterministic sigma so factor_with_batch runs exactly one curve.
//
// Bound parameters match the small-cofactor regime
// (10-25 digit semiprimes the test factors). B1 = 2000 / B2 = 50000 reaches
// these reliably without depending on Brent-Suyama opt-in.
ECM::BatchContext make_single_curve_ctx(uint64_t sigma) {
    ECM::Config cfg;
    cfg.num_curves = 0;     // sigma_pool set explicitly below
    cfg.B1 = 2000;
    cfg.B2 = 50000;
    cfg.auto_params = false;
    cfg.brent_suyama_degree = 0;  // classical BSGS path
    ECM::BatchContext ctx = ECM::prepare_batch(cfg, /*sigma_seed=*/0);
    ctx.sigma_pool.clear();
    ctx.sigma_pool.push_back(sigma);
    return ctx;
}

// Helper: run the dispatcher with the given env value and sigma list. Each
// "curve" task invokes factor_with_batch on a single-sigma context against N.
// Returns the per-curve outcomes in input order.
std::vector<std::optional<Integer>>
run_dispatch_with_threads(const Integer& N,
                          const std::vector<uint64_t>& sigmas,
                          const char* env_value) {
    apply_env(env_value);

    // We materialize a vector of `uint64_t` to take as a span. The lambda
    // captures N by reference (read-only) and builds the single-sigma ctx
    // inside the worker — Integer construction is per-thread, so no
    // shared mutable state.
    std::vector<uint64_t> sigma_buf = sigmas;
    auto results = parallel_stage2_curves<Integer, uint64_t>(
        std::span<uint64_t>(sigma_buf),
        [&N](uint64_t sigma, std::size_t /*idx*/) -> std::optional<Integer> {
            ECM::BatchContext ctx = make_single_curve_ctx(sigma);
            return ECM::factor_with_batch(N, ctx);
        });

    // Restore default before returning so subsequent tests start clean.
    apply_env(nullptr);
    return results;
}

// Helper: collapse a per-curve result vector to a sorted set of distinct
// non-trivial factor strings, for comparison between sequential and
// parallel paths.
std::set<std::string>
factor_set(const std::vector<std::optional<Integer>>& results) {
    std::set<std::string> out;
    for (const auto& r : results) {
        if (r) {
            out.insert(r->to_string());
        }
    }
    return out;
}

// Helper: every non-null factor in `results` must divide N and be non-trivial.
bool all_factors_valid(const Integer& N,
                       const std::vector<std::optional<Integer>>& results) {
    for (const auto& r : results) {
        if (!r) continue;
        if (r->is_one()) return false;
        if (r->compare(N) == 0) return false;
        Integer rem;
        mpz_mod(rem.get_mpz(), N.get_mpz(), r->get_mpz());
        if (!rem.is_zero()) return false;
    }
    return true;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 1: N=1 baseline — factor a known semiprime, verify factor found
// ───────────────────────────────────────────────────────────────────────────
void test_baseline_seq_finds_factor() {
    std::cout << "Test 1: N=1 baseline finds known factor..." << std::flush;

    // 2261419229 = 47491 * 47659  (~31-bit semiprime; well within B1=2000
    // / B2=50000 reach for at least one of the curves).
    Integer N{uint64_t{2261419229ULL}};
    auto sigmas = make_sigmas(100, 16);

    auto results = run_dispatch_with_threads(N, sigmas, "1");
    assert(results.size() == sigmas.size());
    assert(all_factors_valid(N, results));

    auto factors = factor_set(results);
    if (factors.empty()) {
        std::cerr << "\n  ERROR: no factor found in 16-curve sequential run"
                  << std::endl;
        std::abort();
    }

    // Verify at least one curve found a non-trivial factor.
    std::cout << " PASS (" << factors.size() << " distinct factor(s) found)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: N=4 parity — same N, same sigmas → identical factor set
// ───────────────────────────────────────────────────────────────────────────
void test_parity_seq_vs_n4() {
    std::cout << "Test 2: N=1 vs N=4 parity..." << std::flush;

    Integer N{uint64_t{2261419229ULL}};
    auto sigmas = make_sigmas(100, 16);

    auto seq_results = run_dispatch_with_threads(N, sigmas, "1");
    auto par_results = run_dispatch_with_threads(N, sigmas, "4");

    assert(seq_results.size() == par_results.size());
    assert(all_factors_valid(N, seq_results));
    assert(all_factors_valid(N, par_results));

    // The set of discovered factors must be identical between sequential
    // and parallel paths (pure-function-over-sigma invariant). We compare
    // per-index because each (sigma, N) tuple has a deterministic outcome.
    for (std::size_t i = 0; i < seq_results.size(); ++i) {
        bool seq_has = seq_results[i].has_value();
        bool par_has = par_results[i].has_value();
        if (seq_has != par_has) {
            std::cerr << "\n  ERROR: index " << i << " seq_has=" << seq_has
                      << " par_has=" << par_has << std::endl;
            std::abort();
        }
        if (seq_has) {
            if (seq_results[i]->compare(*par_results[i]) != 0) {
                std::cerr << "\n  ERROR: index " << i << " mismatch: seq="
                          << seq_results[i]->to_string()
                          << " par=" << par_results[i]->to_string()
                          << std::endl;
                std::abort();
            }
        }
    }

    // Sanity: at least one curve must have produced a factor.
    auto seq_factors = factor_set(seq_results);
    assert(!seq_factors.empty());

    std::cout << " PASS (per-index bit-identical, "
              << seq_factors.size() << " distinct factor(s))\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: N=hw parity — same N, same sigmas → identical factor set
// ───────────────────────────────────────────────────────────────────────────
void test_parity_seq_vs_hw() {
    std::cout << "Test 3: N=1 vs N=hw parity..." << std::flush;

    Integer N{uint64_t{2261419229ULL}};
    auto sigmas = make_sigmas(200, 16);

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::string hw_str = std::to_string(hw);

    auto seq_results = run_dispatch_with_threads(N, sigmas, "1");
    auto par_results = run_dispatch_with_threads(N, sigmas, hw_str.c_str());

    assert(seq_results.size() == par_results.size());
    assert(all_factors_valid(N, seq_results));
    assert(all_factors_valid(N, par_results));

    for (std::size_t i = 0; i < seq_results.size(); ++i) {
        bool seq_has = seq_results[i].has_value();
        bool par_has = par_results[i].has_value();
        assert(seq_has == par_has);
        if (seq_has) {
            assert(seq_results[i]->compare(*par_results[i]) == 0);
        }
    }

    std::cout << " PASS (per-index bit-identical, N=hw=" << hw << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: ENV parsing — "0" / "1" / "8" / unset / "garbage" / "9999"
// ───────────────────────────────────────────────────────────────────────────
void test_env_parsing() {
    std::cout << "Test 4: ENV parsing..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;

    // Unset -> 1 (default sequential).
    apply_env(nullptr);
    assert(ecm_stage2_parallel_threads() == 1);

    // Empty string -> 1.
    apply_env("");
    assert(ecm_stage2_parallel_threads() == 1);

    // "0" -> 1 (invalid, non-positive).
    apply_env("0");
    assert(ecm_stage2_parallel_threads() == 1);

    // "-5" -> 1 (invalid, non-positive).
    apply_env("-5");
    assert(ecm_stage2_parallel_threads() == 1);

    // "1" -> 1.
    apply_env("1");
    assert(ecm_stage2_parallel_threads() == 1);

    // "8" -> 8 (assuming hw*2 >= 8 on any modern machine).
    apply_env("8");
    std::size_t v8 = ecm_stage2_parallel_threads();
    // If hw*2 < 8 we expect clamp; otherwise exact 8.
    std::size_t expect8 = (8 < cap) ? 8 : cap;
    if (v8 != expect8) {
        std::cerr << "\n  ERROR: '8' parsed to " << v8 << ", expected "
                  << expect8 << " (hw*2 cap = " << cap << ")" << std::endl;
        std::abort();
    }

    // "garbage" -> 1 (atoi returns 0).
    apply_env("garbage");
    assert(ecm_stage2_parallel_threads() == 1);

    // "9999" -> clamp to hw*2.
    apply_env("9999");
    std::size_t v9999 = ecm_stage2_parallel_threads();
    if (v9999 != cap) {
        std::cerr << "\n  ERROR: '9999' parsed to " << v9999
                  << ", expected clamped value " << cap << std::endl;
        std::abort();
    }

    // Restore default for subsequent tests.
    apply_env(nullptr);

    std::cout << " PASS (cap = " << cap << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: Empty curve list — both paths return empty vector cleanly
// ───────────────────────────────────────────────────────────────────────────
void test_empty_curve_list() {
    std::cout << "Test 5: empty curve list (no-op both paths)..." << std::flush;

    // Sequential (N=1) — empty input must produce empty output cleanly,
    // no thread pool created.
    apply_env("1");
    std::vector<int> empty_in;
    auto seq_results = parallel_stage2_curves<Integer, int>(
        std::span<int>(empty_in),
        [](int, std::size_t) -> std::optional<Integer> {
            std::cerr << "\n  ERROR: should not invoke run_stage2 on empty span"
                      << std::endl;
            std::abort();
            return std::nullopt;
        });
    assert(seq_results.empty());

    // Parallel (N=4) — same invariant. Helper must short-circuit before
    // creating ThreadPool because n == 0.
    apply_env("4");
    auto par_results = parallel_stage2_curves<Integer, int>(
        std::span<int>(empty_in),
        [](int, std::size_t) -> std::optional<Integer> {
            std::cerr << "\n  ERROR: should not invoke run_stage2 on empty span"
                      << std::endl;
            std::abort();
            return std::nullopt;
        });
    assert(par_results.empty());

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: Single curve — N=4 still runs once with no stall, no double-call
// ───────────────────────────────────────────────────────────────────────────
void test_single_curve_no_stall() {
    std::cout << "Test 6: single-curve N=4 (no stall, called exactly once)..."
              << std::flush;

    // Use atomic counter to verify exact-once invocation under N=4.
    apply_env("4");
    std::vector<uint64_t> single = {42};
    std::atomic<int> calls{0};
    std::size_t seen_index = static_cast<std::size_t>(-1);

    auto t0 = std::chrono::steady_clock::now();
    auto results = parallel_stage2_curves<Integer, uint64_t>(
        std::span<uint64_t>(single),
        [&calls, &seen_index](uint64_t sigma,
                              std::size_t idx) -> std::optional<Integer> {
            calls.fetch_add(1, std::memory_order_relaxed);
            seen_index = idx;
            // Return a stub value (sigma itself); we only check call shape.
            return std::optional<Integer>{Integer{sigma}};
        });
    auto t1 = std::chrono::steady_clock::now();
    long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    assert(results.size() == 1);
    assert(results[0].has_value());
    assert(results[0]->to_string() == "42");
    int total_calls = calls.load(std::memory_order_relaxed);
    if (total_calls != 1) {
        std::cerr << "\n  ERROR: expected exactly 1 call, got " << total_calls
                  << std::endl;
        std::abort();
    }
    assert(seen_index == 0);

    // Single-curve must short-circuit to the sequential path (no pool
    // creation). Sanity-bound the wall-time to a small value; if the
    // helper accidentally spawned a 4-thread pool the spin-up alone
    // would push past this. (Generous bound to keep CI sanitizer runs
    // happy.)
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-curve dispatch took " << ms
                  << " ms (expected << 1000 ms)" << std::endl;
        // No abort -- this is a soft signal, sanitizers can be slow.
    }

    apply_env(nullptr);
    std::cout << " PASS (" << ms << " ms)\n";
}

}  // namespace

int main() {
    std::cout << "=== ECM Stage 2 Parallel Dispatch Tests ===" << std::endl;

    test_baseline_seq_finds_factor();
    test_parity_seq_vs_n4();
    test_parity_seq_vs_hw();
    test_env_parsing();
    test_empty_curve_list();
    test_single_curve_no_stall();

    std::cout << std::endl
              << "=== All ECM Stage 2 Parallel Tests PASSED ===" << std::endl;
    return 0;
}
