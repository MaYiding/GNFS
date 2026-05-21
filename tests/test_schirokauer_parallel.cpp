// test_schirokauer_parallel.cpp — Per-relation Schirokauer parallel dispatch tests.
//
// Validates the GNFS_SCHIROKAUER_THREADS env-gated dispatcher introduced in
// include/gnfs/linalg/schirokauer_parallel.hpp:
//   * Sequential (N=1, default) and parallel (N>=2) paths produce identical
//     Schirokauer-map outputs (bit-for-bit per index, per column) for the
//     same batch of (a, b) pairs.
//   * ENV parsing handles unset / "" / "0" / "1" / "8" / "9999" / "garbage"
//     correctly; clamping at hardware_concurrency()*2.
//   * Empty batch, single-relation, and many-relation batches all behave
//     deterministically.
//
// SchirokaurMap::compute_flat is a const pure function over (a, b) plus the
// read-only prime_info_ table, so per-relation parallelism is trivially safe
// (no shared mutable state).  These tests pin down that invariant so future
// refactors of either schirokauer.hpp or the parallel dispatcher cannot
// silently break it.
//
// Polynomial setup: reuses the 50-digit deg-4 polynomial from
// test_schirokauer_deg4.cpp for which f mod 2 is UNSPLIT — the same path
// production runs on the GF(2) matrix.  ctor cost is O(few ms), so each
// test case can spin up its own SchirokaurMap without dominating wall time.

#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/linalg/schirokauer.hpp>
#include <gnfs/linalg/schirokauer_parallel.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::linalg::SchirokaurConfig;
using gnfs::linalg::SchirokaurMap;
using gnfs::linalg::compute_schirokauer_flat_batch;
using gnfs::linalg::schirokauer_threads;
using gnfs::linalg::schirokauer_threads_reset_env_cache_for_testing;

namespace {

using ABPair = std::pair<int64_t, uint64_t>;

// 50-digit polynomial (degree 4) — UNSPLIT mod 2.  Same fixture as
// test_schirokauer_deg4 / test_schirokauer_strip.
PolynomialContext build_test_ctx() {
    Integer N("16000000000000004000000216000000000000027000000729");
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(int64_t(5603231353LL)));
    coeffs.push_back(Integer(int64_t(626122691041LL)));
    coeffs.push_back(Integer(int64_t(1000587868LL)));
    coeffs.push_back(Integer(int64_t(1252LL)));
    coeffs.push_back(Integer(int64_t(1)));
    Integer m(int64_t(1999999999687LL));
    PolynomialContext ctx(N.clone(), std::move(coeffs), m.clone(), 1.0);
    assert(ctx.degree() == 4);
    assert(ctx.verify() && "f(m) != 0 mod N");
    return ctx;
}

SchirokaurMap build_test_map(const PolynomialContext& ctx) {
    SchirokaurConfig config;
    config.primes = {2};
    config.exponent_k = 8;
    return SchirokaurMap(ctx, config);
}

// Deterministic small batch (varied a/b, coprime to ℓ=2).  Mirrors
// test_pairs in test_schirokauer_deg4.cpp.
std::vector<ABPair> make_small_batch() {
    return {
        {1, 1}, {3, 2}, {7, 1}, {-1, 4}, {11, 3}, {-5, 2},
        {100, 7}, {-33, 11}, {999, 1}, {1, 999},
        {4095, 2047}, {-4095, 2047}, {1, 2}, {3, 4}, {5, 6},
        {17, 5}, {-23, 9}, {41, 13}, {-67, 21}, {89, 1},
    };
}

// Larger deterministic batch for parity tests.  Many relations exercise
// the ThreadPool dispatch on every worker; 500 keeps test wall time
// comfortably under the instant tier budget.
std::vector<ABPair> make_large_batch(std::size_t n) {
    std::vector<ABPair> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        int64_t a = static_cast<int64_t>(2 * i + 1);  // always odd → coprime to ℓ=2
        uint64_t b = static_cast<uint64_t>(((i * 7) % 113) + 1);  // 1..113
        if ((b & 1u) == 0u) b += 1;  // also force b odd
        out.emplace_back(a, b);
    }
    return out;
}

bool flat_vectors_equal(const std::vector<std::vector<uint32_t>>& a,
                        const std::vector<std::vector<uint32_t>>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].size() != b[i].size()) return false;
        for (std::size_t j = 0; j < a[i].size(); ++j) {
            if (a[i][j] != b[i][j]) return false;
        }
    }
    return true;
}

void print_mismatch(const std::vector<ABPair>& pairs,
                    const std::vector<std::vector<uint32_t>>& seq,
                    const std::vector<std::vector<uint32_t>>& par,
                    const char* label) {
    std::cerr << "MISMATCH (" << label << "): batch size " << pairs.size()
              << ", seq.size()=" << seq.size() << " par.size()=" << par.size()
              << std::endl;
    std::size_t shown = 0;
    for (std::size_t i = 0; i < pairs.size() && shown < 5; ++i) {
        if (i >= seq.size() || i >= par.size() || seq[i] != par[i]) {
            std::cerr << "  i=" << i << " (a=" << pairs[i].first << ", b="
                      << pairs[i].second << ") seq=[";
            for (std::size_t j = 0; j < seq[i].size(); ++j) {
                if (j) std::cerr << ',';
                std::cerr << seq[i][j];
            }
            std::cerr << "] par=[";
            for (std::size_t j = 0; j < par[i].size(); ++j) {
                if (j) std::cerr << ',';
                std::cerr << par[i][j];
            }
            std::cerr << "]" << std::endl;
            ++shown;
        }
    }
}

// Set GNFS_SCHIROKAUER_THREADS=value (or unset if nullptr) and reset the
// cached parse so the next call to schirokauer_threads() picks it up.
void set_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_SCHIROKAUER_THREADS");
    } else {
        setenv("GNFS_SCHIROKAUER_THREADS", value, /*overwrite=*/1);
    }
    schirokauer_threads_reset_env_cache_for_testing();
}

void clear_env() {
    unsetenv("GNFS_SCHIROKAUER_THREADS");
    schirokauer_threads_reset_env_cache_for_testing();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// 1. Baseline N=1: batch path must equal per-call compute_flat results.
//    Pins down "sequential path is bit-identical to calling compute_flat
//    one at a time" — the load-bearing invariant.
void test_baseline_n1_matches_per_call() {
    std::cout << "Testing baseline N=1 batch == per-call sequential..." << std::endl;
    auto ctx = build_test_ctx();
    auto smap = build_test_map(ctx);
    auto pairs = make_small_batch();

    set_env("1");
    auto batch = compute_schirokauer_flat_batch(smap, pairs);

    std::vector<std::vector<uint32_t>> expected;
    expected.reserve(pairs.size());
    for (auto [a, b] : pairs) {
        expected.push_back(smap.compute_flat(a, b));
    }

    if (!flat_vectors_equal(batch, expected)) {
        print_mismatch(pairs, expected, batch, "N=1 vs per-call");
        std::abort();
    }
    // Sanity: every column ∈ {0, 1} for ℓ=2.
    for (const auto& row : batch) {
        for (uint32_t v : row) {
            assert(v < 2 && "Schirokauer ℓ=2 column must be 0 or 1");
        }
    }
    clear_env();
    std::cout << "  baseline N=1: PASSED (" << pairs.size() << " pairs, "
              << batch[0].size() << " columns/row)" << std::endl;
}

// 2. Parity N=1 vs N=4: bit-for-bit per cell.
void test_parity_n1_vs_n4() {
    std::cout << "Testing parity N=1 vs N=4 (large batch)..." << std::endl;
    auto ctx = build_test_ctx();
    auto smap = build_test_map(ctx);
    auto pairs = make_large_batch(500);

    set_env("1");
    auto seq = compute_schirokauer_flat_batch(smap, pairs);

    set_env("4");
    auto par = compute_schirokauer_flat_batch(smap, pairs);

    if (!flat_vectors_equal(seq, par)) {
        print_mismatch(pairs, seq, par, "N=1 vs N=4");
        std::abort();
    }
    clear_env();
    std::cout << "  parity N=1 vs N=4: PASSED (" << pairs.size() << " pairs)" << std::endl;
}

// 3. Parity N=1 vs N=hw_concurrency: all worker counts must agree.
void test_parity_n1_vs_hw() {
    std::cout << "Testing parity N=1 vs N=hw_concurrency (large batch)..." << std::endl;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::string hw_str = std::to_string(hw);

    auto ctx = build_test_ctx();
    auto smap = build_test_map(ctx);
    auto pairs = make_large_batch(500);

    set_env("1");
    auto seq = compute_schirokauer_flat_batch(smap, pairs);

    set_env(hw_str.c_str());
    auto par = compute_schirokauer_flat_batch(smap, pairs);

    if (!flat_vectors_equal(seq, par)) {
        print_mismatch(pairs, seq, par, "N=1 vs N=hw");
        std::abort();
    }
    clear_env();
    std::cout << "  parity N=1 vs N=" << hw << ": PASSED ("
              << pairs.size() << " pairs)" << std::endl;
}

// 4. ENV parsing across all documented forms.
void test_env_parsing() {
    std::cout << "Testing GNFS_SCHIROKAUER_THREADS env parsing..." << std::endl;

    // unset → 1
    set_env(nullptr);
    if (schirokauer_threads() != 1) {
        std::cerr << "  ERROR: unset → expected 1, got "
                  << schirokauer_threads() << std::endl;
        std::abort();
    }

    // empty string → 1
    set_env("");
    if (schirokauer_threads() != 1) {
        std::cerr << "  ERROR: \"\" → expected 1, got "
                  << schirokauer_threads() << std::endl;
        std::abort();
    }

    // "0" → 1 (treated as invalid <=0)
    set_env("0");
    if (schirokauer_threads() != 1) {
        std::cerr << "  ERROR: \"0\" → expected 1, got "
                  << schirokauer_threads() << std::endl;
        std::abort();
    }

    // "1" → 1
    set_env("1");
    if (schirokauer_threads() != 1) {
        std::cerr << "  ERROR: \"1\" → expected 1, got "
                  << schirokauer_threads() << std::endl;
        std::abort();
    }

    // "8" → 8 (clamped to hw*2 which is >= 8 on any modern machine; if
    // the runner has hw=2 the cap is 4 and we'd see 4 instead — handle
    // both paths).
    set_env("8");
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::size_t cap = static_cast<std::size_t>(hw) * 2;
    std::size_t expect_8 = (cap < 8) ? cap : 8;
    if (schirokauer_threads() != expect_8) {
        std::cerr << "  ERROR: \"8\" → expected " << expect_8 << ", got "
                  << schirokauer_threads() << std::endl;
        std::abort();
    }

    // "garbage" → 1 (atoi parses leading numeric prefix; "garbage" has
    // none → 0 → treated as invalid → 1).
    set_env("garbage");
    if (schirokauer_threads() != 1) {
        std::cerr << "  ERROR: \"garbage\" → expected 1, got "
                  << schirokauer_threads() << std::endl;
        std::abort();
    }

    // "-5" → 1 (negative is invalid).
    set_env("-5");
    if (schirokauer_threads() != 1) {
        std::cerr << "  ERROR: \"-5\" → expected 1, got "
                  << schirokauer_threads() << std::endl;
        std::abort();
    }

    // "9999" → clamp to hw*2.
    set_env("9999");
    if (schirokauer_threads() != cap) {
        std::cerr << "  ERROR: \"9999\" → expected " << cap << " (clamp), got "
                  << schirokauer_threads() << std::endl;
        std::abort();
    }

    clear_env();
    std::cout << "  env parsing (unset/\"\"/0/1/8/garbage/-5/9999): PASSED" << std::endl;
}

// 5. Empty relation set: N=1 and N=4 both return empty output, no crash,
//    no ThreadPool allocation.
void test_empty_batch() {
    std::cout << "Testing empty batch (N=1 and N=4)..." << std::endl;
    auto ctx = build_test_ctx();
    auto smap = build_test_map(ctx);
    std::vector<ABPair> empty;

    set_env("1");
    auto seq = compute_schirokauer_flat_batch(smap, empty);
    assert(seq.empty() && "empty input should give empty output (N=1)");

    set_env("4");
    auto par = compute_schirokauer_flat_batch(smap, empty);
    assert(par.empty() && "empty input should give empty output (N=4)");

    clear_env();
    std::cout << "  empty batch (N=1 and N=4): PASSED" << std::endl;
}

// 6. Single relation: N=4 must produce the same answer as per-call.
//    The dispatcher short-circuits to sequential for n==1 to avoid
//    ThreadPool overhead; verify the correctness invariant holds anyway.
void test_single_relation() {
    std::cout << "Testing single-relation batch (N=4)..." << std::endl;
    auto ctx = build_test_ctx();
    auto smap = build_test_map(ctx);

    std::vector<ABPair> one_pair = {{42, 7}};

    set_env("1");
    auto seq = compute_schirokauer_flat_batch(smap, one_pair);

    set_env("4");
    auto par = compute_schirokauer_flat_batch(smap, one_pair);

    assert(seq.size() == 1);
    assert(par.size() == 1);
    if (seq[0] != par[0]) {
        std::cerr << "  ERROR: single-relation N=1 vs N=4 mismatch" << std::endl;
        std::abort();
    }
    // Must also match the per-call API.
    auto direct = smap.compute_flat(one_pair[0].first, one_pair[0].second);
    assert(seq[0] == direct);
    assert(par[0] == direct);

    clear_env();
    std::cout << "  single-relation (N=1 and N=4 vs compute_flat): PASSED" << std::endl;
}

// 7. Perf info: report N=1 vs N=4 timing on the larger batch.  Not
//    asserted (M-series cores can pre-compute everything in microseconds
//    and ThreadPool overhead may dominate at this size).
void test_perf_info() {
    std::cout << "Schirokauer parallel perf info: N=1 vs N=4..." << std::endl;
    auto ctx = build_test_ctx();
    auto smap = build_test_map(ctx);
    auto pairs = make_large_batch(2000);

    set_env("1");
    auto t0 = std::chrono::steady_clock::now();
    auto seq = compute_schirokauer_flat_batch(smap, pairs);
    auto t1 = std::chrono::steady_clock::now();
    long long seq_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    set_env("4");
    t0 = std::chrono::steady_clock::now();
    auto par = compute_schirokauer_flat_batch(smap, pairs);
    t1 = std::chrono::steady_clock::now();
    long long par_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    // Correctness re-check.
    if (!flat_vectors_equal(seq, par)) {
        print_mismatch(pairs, seq, par, "perf-info parity");
        std::abort();
    }
    std::cout << "  N=1: " << seq_us << " µs" << std::endl;
    std::cout << "  N=4: " << par_us << " µs" << std::endl;
    if (seq_us > 0 && par_us > 0) {
        double speedup = static_cast<double>(seq_us) / static_cast<double>(par_us);
        std::cout << "  speedup (informational): " << speedup << "x" << std::endl;
    }
    clear_env();
    std::cout << "  PASSED (info-only, no speedup assert)" << std::endl;
}

}  // namespace

int main() {
    std::cout << "=== Schirokauer Parallel Dispatch Tests ===" << std::endl
              << std::endl;

    // Tests intentionally reset the env between cases via clear_env().
    test_baseline_n1_matches_per_call();
    test_parity_n1_vs_n4();
    test_parity_n1_vs_hw();
    test_env_parsing();
    test_empty_batch();
    test_single_relation();
    test_perf_info();

    std::cout << std::endl
              << "=== All Schirokauer Parallel Tests PASSED ===" << std::endl;
    return 0;
}
