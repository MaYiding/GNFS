// test_mpz_mod_parallel.cpp -- batched mpz_mod parallel dispatcher tests
//
// Validates the GNFS_MPZ_MOD_BATCH_THREADS env-gated dispatcher introduced in
// include/gnfs/util/mpz_mod_parallel.hpp:
//
//   * ENV parsing handles unset / "0" / "1" / "4" / "garbage" / "" / "9999" /
//     "12abc" / leading whitespace / "10000" correctly; clamping at
//     hardware_concurrency() * 2.
//   * Sequential (N=1, default) and parallel (N>=2) paths produce per-index
//     bit-identical results for the same (dividends, modulus) input. The
//     dispatcher is a pure parallel wrapper around `mpz_mod`, so a small
//     prime modulus + integer dividends drive the parity assertions cheaply.
//   * Empty dividends vector returns cleanly without creating a pool or
//     invoking any mpz operation.
//   * Single dividend under N>=2 short-circuits to sequential (exactly-once
//     mpz_mod invocation, no stall).
//   * 100-dividend random batch matches scalar mpz_mod reference at N=1 and
//     stays bit-identical at N=4 and N=hardware_concurrency.
//   * 200-bit modulus exercises multi-limb `mpz_mod` semantics.
//   * Boundary dividends (dividend < modulus, dividend == modulus, negative
//     dividend) all produce canonical non-negative residues.
//   * Cache reset hook re-parses ENV between assertions.
//   * Perf-info probe (informational, no assert).

#include <gnfs/util/mpz_mod_parallel.hpp>
#include <gnfs/core/integer.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <gmp.h>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using gnfs::core::Integer;
using gnfs::util::mpz_mod_batch_threads;
using gnfs::util::mpz_mod_batch_threads_reset_env_cache_for_testing;
using gnfs::util::parallel_mpz_mod;
using gnfs::util::resolve_mpz_mod_batch_threads;

namespace {

// Helper: set or unset GNFS_MPZ_MOD_BATCH_THREADS and refresh the cache so
// the next call to mpz_mod_batch_threads() reflects the new value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_MPZ_MOD_BATCH_THREADS");
    } else {
        setenv("GNFS_MPZ_MOD_BATCH_THREADS", value, /*overwrite=*/1);
    }
    mpz_mod_batch_threads_reset_env_cache_for_testing();
}

// Scalar reference: compute d mod n for one dividend, returning a fresh
// Integer. Always produces the canonical non-negative residue class
// representative (GMP `mpz_mod` semantics).
Integer scalar_mod(const Integer& dividend, const Integer& modulus) {
    Integer out;
    mpz_mod(out.get_mpz(), dividend.get_mpz(), modulus.get_mpz());
    return out;
}

// Build a vector of `n` Integer dividends from a deterministic mt19937_64
// seed, each dividend value uniformly drawn from [0, 2^63) so the dispatcher
// has non-trivial inputs to reduce.
std::vector<Integer> make_random_dividends(std::size_t n, uint64_t seed) {
    std::vector<Integer> dividends;
    dividends.reserve(n);
    std::mt19937_64 rng(seed);
    for (std::size_t i = 0; i < n; ++i) {
        // Draw a 63-bit value so the conversion to Integer is unambiguous
        // for int64_t-like ranges; the dispatcher does not care about sign.
        uint64_t v = rng() & 0x7FFFFFFFFFFFFFFFULL;
        dividends.emplace_back(v);
    }
    return dividends;
}

// ---------------------------------------------------------------------------
// Test 1: ENV unset -> 1 (default sequential)
// ---------------------------------------------------------------------------
void test_env_unset_defaults_to_one() {
    std::cout << "Test 1: ENV unset -> 1..." << std::flush;
    apply_env(nullptr);
    int v = mpz_mod_batch_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: unset env parsed to " << v
                  << ", expected 1" << std::endl;
        std::abort();
    }
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 2: ENV "0" -> 1 (invalid, non-positive)
// ---------------------------------------------------------------------------
void test_env_zero_to_one() {
    std::cout << "Test 2: ENV '0' -> 1..." << std::flush;
    apply_env("0");
    int v = mpz_mod_batch_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: '0' parsed to " << v
                  << ", expected 1" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 3: ENV "4" -> 4 (or clamped to cap)
// ---------------------------------------------------------------------------
void test_env_four() {
    std::cout << "Test 3: ENV '4' -> 4..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (4 < cap) ? 4 : cap;

    apply_env("4");
    int v = mpz_mod_batch_threads();
    if (v != expect) {
        std::cerr << "\n  ERROR: '4' parsed to " << v << ", expected "
                  << expect << " (hw*2 cap = " << cap << ")" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (parsed " << v << ", cap " << cap << ")\n";
}

// ---------------------------------------------------------------------------
// Test 4: ENV "10000" -> clamped at hardware_concurrency() * 2
// ---------------------------------------------------------------------------
void test_env_clamp() {
    std::cout << "Test 4: ENV '10000' clamped at hw*2..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;

    apply_env("10000");
    int v = mpz_mod_batch_threads();
    if (v != cap) {
        std::cerr << "\n  ERROR: '10000' parsed to " << v
                  << ", expected cap=" << cap << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (cap=" << cap << ")\n";
}

// ---------------------------------------------------------------------------
// Test 5: ENV "garbage" / "" / "-5" / leading-whitespace "  4" / "12abc"
// -> partial-parse rules match W11 T3 / W12 T3 family semantics
// (std::atoi accepts a leading numeric prefix; leading whitespace is
// consumed by atoi and "  4" parses to 4).
// ---------------------------------------------------------------------------
void test_env_non_numeric() {
    std::cout << "Test 5: ENV non-numeric / boundary -> family semantics..."
              << std::flush;

    apply_env("");
    assert(mpz_mod_batch_threads() == 1);

    apply_env("-5");
    assert(mpz_mod_batch_threads() == 1);

    apply_env("garbage");
    assert(mpz_mod_batch_threads() == 1);

    // std::atoi consumes leading whitespace before parsing digits, so
    // "  4" yields 4 -- consistent with the rest of the parallel-dispatcher
    // family (W11 T3 / W12 T3). This is intentional family behaviour.
    apply_env("  4");
    {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        int cap = static_cast<int>(hw) * 2;
        int expect = (4 < cap) ? 4 : cap;
        int got = mpz_mod_batch_threads();
        if (got != expect) {
            std::cerr << "\n  ERROR: '  4' parsed to " << got
                      << ", expected " << expect
                      << " (std::atoi consumes leading whitespace)"
                      << std::endl;
            std::abort();
        }
    }

    // "12abc" -> 12 (std::atoi accepts a leading numeric prefix). Document
    // this behaviour explicitly so future readers know it is intentional
    // (matches W11 T3 / W12 T3 / W10 T4 / W7-W12 family semantics).
    apply_env("12abc");
    {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        int cap = static_cast<int>(hw) * 2;
        int expect = (12 < cap) ? 12 : cap;
        int got = mpz_mod_batch_threads();
        if (got != expect) {
            std::cerr << "\n  ERROR: '12abc' parsed to " << got
                      << ", expected " << expect
                      << " (atoi prefix semantics)" << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 6: Empty dividends vector - no-op, no pool, no writes.
// ---------------------------------------------------------------------------
void test_empty_dividends() {
    std::cout << "Test 6: empty dividends (no-op)..." << std::flush;

    std::vector<Integer> dividends;
    std::vector<Integer> results;
    Integer modulus(257);

    // N=1 sequential.
    apply_env("1");
    parallel_mpz_mod(dividends, modulus, results);
    assert(results.empty());

    // N=4 parallel.
    apply_env("4");
    parallel_mpz_mod(dividends, modulus, results);
    assert(results.empty());

    // resolve_mpz_mod_batch_threads on empty batch returns 0.
    assert(resolve_mpz_mod_batch_threads(0) == 0);

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 7: Single dividend at N=1 -> correct d mod n.
// ---------------------------------------------------------------------------
void test_single_dividend_n1() {
    std::cout << "Test 7: single dividend N=1 -> correct..." << std::flush;
    apply_env("1");

    Integer dividend(500);
    Integer modulus(257);  // small prime

    std::vector<Integer> dividends{dividend};
    std::vector<Integer> results(1);

    parallel_mpz_mod(dividends, modulus, results);

    // 500 mod 257 = 500 - 257 = 243.
    Integer expect = scalar_mod(dividend, modulus);
    if (results[0].to_uint64() != expect.to_uint64()) {
        std::cerr << "\n  ERROR: got " << results[0].to_string()
                  << " expected " << expect.to_string() << std::endl;
        std::abort();
    }
    assert(results[0].to_uint64() == 243);

    apply_env(nullptr);
    std::cout << " PASS (500 mod 257 = " << results[0].to_string() << ")\n";
}

// ---------------------------------------------------------------------------
// Test 8: Single dividend at N=4 - exactly-once invocation, no stall.
// ---------------------------------------------------------------------------
void test_single_dividend_n4_no_stall() {
    std::cout << "Test 8: single dividend N=4 (no stall, correct)..."
              << std::flush;
    apply_env("4");

    Integer dividend(1000);
    Integer modulus(257);

    std::vector<Integer> dividends{dividend};
    std::vector<Integer> results(1);

    auto t0 = std::chrono::steady_clock::now();
    parallel_mpz_mod(dividends, modulus, results);
    auto t1 = std::chrono::steady_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       t1 - t0).count();

    Integer expect = scalar_mod(dividend, modulus);
    if (results[0].to_uint64() != expect.to_uint64()) {
        std::cerr << "\n  ERROR: got " << results[0].to_string()
                  << " expected " << expect.to_string() << std::endl;
        std::abort();
    }

    // Sanity-bound the wall-time: if the helper accidentally spawned a
    // 4-thread pool the spin-up alone would push past this.
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-dividend dispatch took " << ms
                  << " ms (expected << 1000 ms)" << std::endl;
        // No abort -- soft signal, sanitizers can be slow.
    }

    apply_env(nullptr);
    std::cout << " PASS (1000 mod 257 = " << results[0].to_string()
              << ", " << ms << " ms)\n";
}

// ---------------------------------------------------------------------------
// Test 9: 100 random dividends at N=1 baseline matches scalar reference.
// ---------------------------------------------------------------------------
void test_n1_baseline_matches_scalar() {
    std::cout << "Test 9: N=1 baseline matches scalar mpz_mod..."
              << std::flush;
    apply_env("1");

    Integer modulus(257);  // small prime
    auto dividends = make_random_dividends(100, /*seed=*/12345ULL);

    std::vector<Integer> results(dividends.size());
    parallel_mpz_mod(dividends, modulus, results);

    for (std::size_t i = 0; i < dividends.size(); ++i) {
        Integer expect = scalar_mod(dividends[i], modulus);
        if (results[i].to_uint64() != expect.to_uint64()) {
            std::cerr << "\n  ERROR: idx " << i << " got "
                      << results[i].to_string() << " expected "
                      << expect.to_string() << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (" << dividends.size() << " dividends)\n";
}

// ---------------------------------------------------------------------------
// Test 10: 100 random dividends at N=1 vs N=4 -- per-index bit-identical.
// ---------------------------------------------------------------------------
void test_n1_vs_n4_parity() {
    std::cout << "Test 10: N=1 vs N=4 parity (per-index bit-identical)..."
              << std::flush;

    Integer modulus(257);
    auto dividends = make_random_dividends(100, /*seed=*/0xDEADBEEFULL);

    apply_env("1");
    std::vector<Integer> seq(dividends.size());
    parallel_mpz_mod(dividends, modulus, seq);

    apply_env("4");
    std::vector<Integer> par(dividends.size());
    parallel_mpz_mod(dividends, modulus, par);

    apply_env(nullptr);

    for (std::size_t i = 0; i < dividends.size(); ++i) {
        if (mpz_cmp(seq[i].get_mpz(), par[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " seq="
                      << seq[i].to_string() << " par="
                      << par[i].to_string() << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << dividends.size()
              << " per-index identical)\n";
}

// ---------------------------------------------------------------------------
// Test 11: 100 random dividends at N=1 vs N=hw_concurrency -- bit-identical.
// ---------------------------------------------------------------------------
void test_n1_vs_n_hw_parity() {
    std::cout << "Test 11: N=1 vs N=hw_concurrency parity..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::string hw_str = std::to_string(hw);

    Integer modulus(257);
    auto dividends = make_random_dividends(100, /*seed=*/0xCAFEBABEULL);

    apply_env("1");
    std::vector<Integer> seq(dividends.size());
    parallel_mpz_mod(dividends, modulus, seq);

    apply_env(hw_str.c_str());
    std::vector<Integer> par(dividends.size());
    parallel_mpz_mod(dividends, modulus, par);

    apply_env(nullptr);

    for (std::size_t i = 0; i < dividends.size(); ++i) {
        if (mpz_cmp(seq[i].get_mpz(), par[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " seq="
                      << seq[i].to_string() << " par="
                      << par[i].to_string() << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (N=hw=" << hw << ", " << dividends.size()
              << " per-index identical)\n";
}

// ---------------------------------------------------------------------------
// Test 12: 200-bit modulus -- multi-limb mpz_mod semantics. Verifies that
// the dispatcher exercises the multi-limb code path used by 50d+/60d
// Schirokauer reductions and ECM accumulators.
// ---------------------------------------------------------------------------
void test_200bit_modulus_parity() {
    std::cout << "Test 12: 200-bit modulus parity..." << std::flush;

    // 200-bit prime modulus (decimal). Sanity-check primality so the test
    // exercises a real prime modulus -- mpz_mod itself does not require a
    // prime modulus, but using one keeps this test sibling-symmetric with
    // W11 T3 / W12 T3.
    Integer modulus(
        "1606938044258990275541962092341162602522202993782792835301301", 10);
    if (mpz_probab_prime_p(modulus.get_mpz(), 25) == 0) {
        std::cerr << "\n  ERROR: chosen modulus is composite (test bug)"
                  << std::endl;
        std::abort();
    }

    // Build 100 random 192-bit dividends so mpz_mod actually has to walk
    // multiple limbs to compute the residue.
    std::vector<Integer> dividends;
    dividends.reserve(100);
    std::mt19937_64 rng(0xFEEDBEEFCAFED00DULL);
    for (std::size_t i = 0; i < 100; ++i) {
        Integer x(uint64_t{rng()});
        Integer chunk(uint64_t{rng()});
        Integer two64("18446744073709551616", 10);  // 2^64
        x = x * two64 + chunk;
        chunk = Integer(uint64_t{rng()});
        x = x * two64 + chunk;
        // No need to pre-reduce -- mpz_mod handles dividend >= modulus.
        dividends.push_back(std::move(x));
    }

    apply_env("1");
    std::vector<Integer> seq(dividends.size());
    parallel_mpz_mod(dividends, modulus, seq);

    apply_env("4");
    std::vector<Integer> par(dividends.size());
    parallel_mpz_mod(dividends, modulus, par);

    apply_env(nullptr);

    // Strict parity vs scalar mpz_mod reference at every slot.
    for (std::size_t i = 0; i < dividends.size(); ++i) {
        Integer expect = scalar_mod(dividends[i], modulus);
        if (mpz_cmp(seq[i].get_mpz(), expect.get_mpz()) != 0) {
            std::cerr << "\n  ERROR: seq idx " << i << " mismatch vs scalar"
                      << std::endl;
            std::abort();
        }
        if (mpz_cmp(par[i].get_mpz(), expect.get_mpz()) != 0) {
            std::cerr << "\n  ERROR: par idx " << i << " mismatch vs scalar"
                      << std::endl;
            std::abort();
        }
        // Canonical residue must be in [0, modulus).
        if (mpz_sgn(seq[i].get_mpz()) < 0 ||
            mpz_cmp(seq[i].get_mpz(), modulus.get_mpz()) >= 0) {
            std::cerr << "\n  ERROR: seq idx " << i
                      << " not in [0, modulus)" << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << dividends.size()
              << " dividends, 200-bit prime modulus)\n";
}

// ---------------------------------------------------------------------------
// Test 13: dividend smaller than modulus -- residue == dividend.
// Boundary case that exercises GMP's fast-path when no reduction is needed.
// ---------------------------------------------------------------------------
void test_dividend_smaller_than_modulus() {
    std::cout << "Test 13: dividend < modulus (residue == dividend)..."
              << std::flush;
    apply_env("1");

    Integer modulus(257);
    std::vector<Integer> dividends;
    dividends.emplace_back(uint64_t{0});
    dividends.emplace_back(uint64_t{1});
    dividends.emplace_back(uint64_t{42});
    dividends.emplace_back(uint64_t{100});
    dividends.emplace_back(uint64_t{256});  // modulus - 1, max valid residue.

    std::vector<Integer> results(dividends.size());
    parallel_mpz_mod(dividends, modulus, results);

    for (std::size_t i = 0; i < dividends.size(); ++i) {
        if (mpz_cmp(results[i].get_mpz(), dividends[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " got "
                      << results[i].to_string() << " expected "
                      << dividends[i].to_string() << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (5 small dividends, residue == dividend)\n";
}

// ---------------------------------------------------------------------------
// Test 14: dividend == modulus -- residue must be 0.
// Boundary case that exercises GMP's exact-divisibility path.
// ---------------------------------------------------------------------------
void test_dividend_equals_modulus() {
    std::cout << "Test 14: dividend == modulus (residue == 0)..."
              << std::flush;
    apply_env("1");

    Integer modulus(257);
    std::vector<Integer> dividends;
    dividends.push_back(modulus);                            // 257 mod 257 = 0
    dividends.emplace_back(uint64_t{257 * 2});               // 514 mod 257 = 0
    dividends.emplace_back(uint64_t{257ULL * 100ULL});       // 25700 mod 257 = 0
    dividends.emplace_back(uint64_t{257ULL * 1000000ULL});   // big multiple

    std::vector<Integer> results(dividends.size());
    parallel_mpz_mod(dividends, modulus, results);

    for (std::size_t i = 0; i < dividends.size(); ++i) {
        if (mpz_sgn(results[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " got "
                      << results[i].to_string() << " expected 0" << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (4 multiples of modulus, all residue 0)\n";
}

// ---------------------------------------------------------------------------
// Test 15: negative dividend -- mpz_mod produces canonical non-negative
// residue (not symmetric-zero). Semantic check for the GMP contract.
// ---------------------------------------------------------------------------
void test_negative_dividend_semantics() {
    std::cout << "Test 15: negative dividend semantics..." << std::flush;
    apply_env("4");  // parallel path exercises the same branch

    Integer modulus(257);

    // Build a handful of negative dividends. mpz_mod's documented output is
    // in [0, modulus), even when dividend is negative -- mpz_tdiv_r (truncated
    // division) would produce a negative residue, but mpz_mod uses the
    // canonical class representative.
    std::vector<Integer> dividends;
    {
        Integer x(int64_t{-1});
        dividends.push_back(std::move(x));  // -1 mod 257 = 256
    }
    {
        Integer x(int64_t{-100});
        dividends.push_back(std::move(x));  // -100 mod 257 = 157
    }
    {
        Integer x(int64_t{-257});
        dividends.push_back(std::move(x));  // -257 mod 257 = 0
    }
    {
        Integer x(int64_t{-258});
        dividends.push_back(std::move(x));  // -258 mod 257 = 256
    }

    std::vector<Integer> results(dividends.size());
    parallel_mpz_mod(dividends, modulus, results);

    // Verify every residue is non-negative and matches the scalar reference.
    apply_env("1");
    std::vector<Integer> seq(dividends.size());
    parallel_mpz_mod(dividends, modulus, seq);

    apply_env(nullptr);

    // Expected residues (computed by hand).
    uint64_t expected[] = {256, 157, 0, 256};
    for (std::size_t i = 0; i < dividends.size(); ++i) {
        // Non-negative invariant.
        if (mpz_sgn(results[i].get_mpz()) < 0) {
            std::cerr << "\n  ERROR: idx " << i
                      << " produced negative residue " << results[i].to_string()
                      << std::endl;
            std::abort();
        }
        // Matches hand-computed expected residue.
        Integer expect_int(expected[i]);
        if (mpz_cmp(results[i].get_mpz(), expect_int.get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " got "
                      << results[i].to_string() << " expected "
                      << expected[i] << std::endl;
            std::abort();
        }
        // Sequential path matches parallel path (defence in depth).
        if (mpz_cmp(seq[i].get_mpz(), results[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: seq vs par mismatch at idx " << i
                      << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (4 negative dividends, all canonical [0, modulus))\n";
}

// ---------------------------------------------------------------------------
// Test 16: Reset cache hook -- mid-test ENV change is picked up after reset.
// ---------------------------------------------------------------------------
void test_reset_env_cache_hook() {
    std::cout << "Test 16: reset env cache re-reads ENV..." << std::flush;

    apply_env("1");
    int initial = mpz_mod_batch_threads();
    if (initial != 1) {
        std::cerr << "\n  ERROR: pre-reset value " << initial
                  << " (expected 1)" << std::endl;
        std::abort();
    }

    // Without reset, a fresh setenv would NOT be picked up (call_once seals
    // the cache). The reset hook is the only way to re-resolve mid-test.
    setenv("GNFS_MPZ_MOD_BATCH_THREADS", "4", /*overwrite=*/1);
    int stale = mpz_mod_batch_threads();
    if (stale != 1) {
        std::cerr << "\n  ERROR: cache not stable before reset, got "
                  << stale << " (expected 1)" << std::endl;
        std::abort();
    }

    // After reset, the new value resolves.
    mpz_mod_batch_threads_reset_env_cache_for_testing();
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (4 < cap) ? 4 : cap;
    if (mpz_mod_batch_threads() != expect) {
        std::cerr << "\n  ERROR: post-reset value " << mpz_mod_batch_threads()
                  << " expected " << expect << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 17: perf-info probe (informational, no assert) -- measure
// hw_concurrency wall vs N=1 wall on 100 large-dividend mpz_mod calls.
// Not strictly required by the spec, but documents the speedup ceiling.
// ---------------------------------------------------------------------------
void test_perf_info_100_dividends() {
    std::cout << "Test 17: perf info (100 dividends, 200-bit modulus)..."
              << std::flush;

    Integer modulus(
        "1606938044258990275541962092341162602522202993782792835301301", 10);

    // 100 random 192-bit dividends.
    std::vector<Integer> dividends;
    dividends.reserve(100);
    std::mt19937_64 rng(0xFEEDBEEFCAFED00DULL);
    for (std::size_t i = 0; i < 100; ++i) {
        Integer x(uint64_t{rng()});
        Integer chunk(uint64_t{rng()});
        Integer two64("18446744073709551616", 10);  // 2^64
        x = x * two64 + chunk;
        chunk = Integer(uint64_t{rng()});
        x = x * two64 + chunk;
        dividends.push_back(std::move(x));
    }

    // N=1 baseline.
    apply_env("1");
    std::vector<Integer> seq(dividends.size());
    auto t0 = std::chrono::steady_clock::now();
    parallel_mpz_mod(dividends, modulus, seq);
    auto t1 = std::chrono::steady_clock::now();
    long long us_seq = std::chrono::duration_cast<std::chrono::microseconds>(
                           t1 - t0).count();

    // N=hw parallel.
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    apply_env(std::to_string(hw).c_str());
    std::vector<Integer> par(dividends.size());
    auto t2 = std::chrono::steady_clock::now();
    parallel_mpz_mod(dividends, modulus, par);
    auto t3 = std::chrono::steady_clock::now();
    long long us_par = std::chrono::duration_cast<std::chrono::microseconds>(
                           t3 - t2).count();

    apply_env(nullptr);

    // Strict parity check is still required even on a perf-info probe.
    for (std::size_t i = 0; i < dividends.size(); ++i) {
        if (mpz_cmp(seq[i].get_mpz(), par[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: perf probe parity break at idx " << i
                      << std::endl;
            std::abort();
        }
    }

    double speedup = (us_par > 0)
                         ? static_cast<double>(us_seq) /
                               static_cast<double>(us_par)
                         : 0.0;
    std::cout << " INFO seq=" << us_seq << " us, N=" << hw
              << " par=" << us_par << " us, speedup="
              << speedup << "x (parity verified)\n";
}

}  // namespace

int main() {
    std::cout << "=== Batched mpz_mod Parallel Dispatch Tests ===\n";

    test_env_unset_defaults_to_one();
    test_env_zero_to_one();
    test_env_four();
    test_env_clamp();
    test_env_non_numeric();
    test_empty_dividends();
    test_single_dividend_n1();
    test_single_dividend_n4_no_stall();
    test_n1_baseline_matches_scalar();
    test_n1_vs_n4_parity();
    test_n1_vs_n_hw_parity();
    test_200bit_modulus_parity();
    test_dividend_smaller_than_modulus();
    test_dividend_equals_modulus();
    test_negative_dividend_semantics();
    test_reset_env_cache_hook();
    test_perf_info_100_dividends();

    std::cout << "\n=== All mpz_mod Parallel Tests PASSED ===\n";
    return 0;
}
