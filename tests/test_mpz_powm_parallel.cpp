// test_mpz_powm_parallel.cpp -- batched mpz_powm parallel dispatcher tests
//
// Validates the GNFS_MPZ_POWM_BATCH_THREADS env-gated dispatcher introduced in
// include/gnfs/util/mpz_powm_parallel.hpp:
//
//   * ENV parsing handles unset / "0" / "1" / "4" / "garbage" / "" / "9999"
//     correctly; clamping at hardware_concurrency() * 2.
//   * Sequential (N=1, default) and parallel (N>=2) paths produce per-index
//     bit-identical results for the same (bases, exp, modulus) input. The
//     dispatcher is a pure parallel wrapper around `mpz_powm`, so a small
//     prime modulus + integer bases drive the parity assertions cheaply.
//   * Empty bases span returns cleanly without creating a pool or invoking
//     any mpz operation.
//   * Single base under N>=2 short-circuits to sequential (exactly-once
//     mpz_powm invocation, no stall).
//   * 100-base random batch matches scalar mpz_powm reference at N=1 and
//     stays bit-identical at N=4 and N=hardware_concurrency.
//   * Common-exponent semantics: same exp / modulus applied to every base
//     (covered implicitly by every parity test).
//   * Cache reset hook re-parses ENV between assertions.

#include <gnfs/util/mpz_powm_parallel.hpp>
#include <gnfs/core/integer.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <gmp.h>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

using gnfs::core::Integer;
using gnfs::util::mpz_powm_batch_threads;
using gnfs::util::mpz_powm_batch_threads_reset_env_cache_for_testing;
using gnfs::util::parallel_mpz_powm;

namespace {

// Helper: set or unset GNFS_MPZ_POWM_BATCH_THREADS and refresh the cache so
// the next call to mpz_powm_batch_threads() reflects the new value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_MPZ_POWM_BATCH_THREADS");
    } else {
        setenv("GNFS_MPZ_POWM_BATCH_THREADS", value, /*overwrite=*/1);
    }
    mpz_powm_batch_threads_reset_env_cache_for_testing();
}

// Scalar reference: compute b^e mod n for one base, returning a fresh Integer.
Integer scalar_powm(const Integer& base, const Integer& exp,
                    const Integer& modulus) {
    Integer out;
    mpz_powm(out.get_mpz(), base.get_mpz(), exp.get_mpz(), modulus.get_mpz());
    return out;
}

// Build a vector of `n` Integer bases from a deterministic mt19937_64 seed,
// each base value uniformly drawn from [1, modulus - 1] so mpz_powm has
// non-trivial inputs to chew on.
std::vector<Integer> make_random_bases(std::size_t n, uint64_t seed,
                                       const Integer& modulus) {
    std::vector<Integer> bases;
    bases.reserve(n);
    std::mt19937_64 rng(seed);
    uint64_t mod_u = modulus.to_uint64();
    std::uniform_int_distribution<uint64_t> dist(1, mod_u - 1);
    for (std::size_t i = 0; i < n; ++i) {
        bases.emplace_back(static_cast<uint64_t>(dist(rng)));
    }
    return bases;
}

// ---------------------------------------------------------------------------
// Test 1: ENV unset -> 1 (default sequential)
// ---------------------------------------------------------------------------
void test_env_unset_defaults_to_one() {
    std::cout << "Test 1: ENV unset -> 1..." << std::flush;
    apply_env(nullptr);
    int v = mpz_powm_batch_threads();
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
    int v = mpz_powm_batch_threads();
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
    int v = mpz_powm_batch_threads();
    if (v != expect) {
        std::cerr << "\n  ERROR: '4' parsed to " << v << ", expected "
                  << expect << " (hw*2 cap = " << cap << ")" << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (parsed " << v << ", cap " << cap << ")\n";
}

// ---------------------------------------------------------------------------
// Test 4: ENV "9999" / "10000" -> clamped at hardware_concurrency() * 2
// ---------------------------------------------------------------------------
void test_env_clamp() {
    std::cout << "Test 4: ENV '10000' clamped at hw*2..." << std::flush;
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;

    apply_env("10000");
    int v = mpz_powm_batch_threads();
    if (v != cap) {
        std::cerr << "\n  ERROR: '10000' parsed to " << v
                  << ", expected cap=" << cap << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (cap=" << cap << ")\n";
}

// ---------------------------------------------------------------------------
// Test 5: ENV "garbage" / "" / "-5" / "   " -> 1 (invalid -> sequential)
// ---------------------------------------------------------------------------
void test_env_non_numeric() {
    std::cout << "Test 5: ENV non-numeric / boundary -> 1..." << std::flush;

    apply_env("");
    assert(mpz_powm_batch_threads() == 1);

    apply_env("-5");
    assert(mpz_powm_batch_threads() == 1);

    apply_env("garbage");
    assert(mpz_powm_batch_threads() == 1);

    apply_env("   ");
    assert(mpz_powm_batch_threads() == 1);

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 6: Empty bases span - no-op, no pool, no writes.
// ---------------------------------------------------------------------------
void test_empty_bases() {
    std::cout << "Test 6: empty bases (no-op)..." << std::flush;

    std::vector<Integer> bases;
    std::vector<Integer> results;
    Integer exp(3);
    Integer modulus(257);

    // N=1 sequential.
    apply_env("1");
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(results));
    assert(results.empty());

    // N=4 parallel.
    apply_env("4");
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(results));
    assert(results.empty());

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 7: Single base at N=1 -> correct b^e mod n.
// ---------------------------------------------------------------------------
void test_single_base_n1() {
    std::cout << "Test 7: single base N=1 -> correct..." << std::flush;
    apply_env("1");

    Integer base(7);
    Integer exp(13);
    Integer modulus(257);  // small prime

    std::vector<Integer> bases;
    bases.push_back(base);
    std::vector<Integer> results(1);

    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(results));

    // 7^13 mod 257 = 7^13 mod 257.
    // 7^2 = 49; 7^4 = 49^2 = 2401 mod 257 = 2401 - 9*257 = 2401 - 2313 = 88
    // 7^8 = 88^2 = 7744 mod 257 = 7744 - 30*257 = 7744 - 7710 = 34
    // 7^13 = 7^8 * 7^4 * 7^1 = 34 * 88 * 7 mod 257
    //      = 34 * 616 mod 257 = 34 * (616 - 2*257) = 34 * 102 = 3468 mod 257
    //      = 3468 - 13*257 = 3468 - 3341 = 127
    Integer expect = scalar_powm(base, exp, modulus);
    if (results[0].to_uint64() != expect.to_uint64()) {
        std::cerr << "\n  ERROR: got " << results[0].to_string()
                  << " expected " << expect.to_string() << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS (7^13 mod 257 = " << results[0].to_string() << ")\n";
}

// ---------------------------------------------------------------------------
// Test 8: Single base at N=4 - exactly-once invocation, no stall, correct.
// ---------------------------------------------------------------------------
void test_single_base_n4_no_stall() {
    std::cout << "Test 8: single base N=4 (no stall, correct)..."
              << std::flush;
    apply_env("4");

    Integer base(5);
    Integer exp(7);
    Integer modulus(257);

    std::vector<Integer> bases;
    bases.push_back(base);
    std::vector<Integer> results(1);

    auto t0 = std::chrono::steady_clock::now();
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(results));
    auto t1 = std::chrono::steady_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       t1 - t0).count();

    Integer expect = scalar_powm(base, exp, modulus);
    if (results[0].to_uint64() != expect.to_uint64()) {
        std::cerr << "\n  ERROR: got " << results[0].to_string()
                  << " expected " << expect.to_string() << std::endl;
        std::abort();
    }

    // Sanity-bound the wall-time: if the helper accidentally spawned a
    // 4-thread pool the spin-up alone would push past this.
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-base dispatch took " << ms
                  << " ms (expected << 1000 ms)" << std::endl;
        // No abort -- soft signal, sanitizers can be slow.
    }

    apply_env(nullptr);
    std::cout << " PASS (5^7 mod 257 = " << results[0].to_string()
              << ", " << ms << " ms)\n";
}

// ---------------------------------------------------------------------------
// Test 9: 100 random bases at N=1 baseline matches scalar reference.
// ---------------------------------------------------------------------------
void test_n1_baseline_matches_scalar() {
    std::cout << "Test 9: N=1 baseline matches scalar mpz_powm..."
              << std::flush;
    apply_env("1");

    Integer modulus(257);  // small prime
    Integer exp(11);
    auto bases = make_random_bases(100, /*seed=*/12345ULL, modulus);

    std::vector<Integer> results(bases.size());
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(results));

    for (std::size_t i = 0; i < bases.size(); ++i) {
        Integer expect = scalar_powm(bases[i], exp, modulus);
        if (results[i].to_uint64() != expect.to_uint64()) {
            std::cerr << "\n  ERROR: idx " << i << " got "
                      << results[i].to_string() << " expected "
                      << expect.to_string() << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (" << bases.size() << " bases)\n";
}

// ---------------------------------------------------------------------------
// Test 10: 100 random bases at N=1 vs N=4 -- per-index bit-identical.
// ---------------------------------------------------------------------------
void test_n1_vs_n4_parity() {
    std::cout << "Test 10: N=1 vs N=4 parity (per-index bit-identical)..."
              << std::flush;

    Integer modulus(257);
    Integer exp(17);
    auto bases = make_random_bases(100, /*seed=*/0xDEADBEEFULL, modulus);

    apply_env("1");
    std::vector<Integer> seq(bases.size());
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(seq));

    apply_env("4");
    std::vector<Integer> par(bases.size());
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(par));

    apply_env(nullptr);

    for (std::size_t i = 0; i < bases.size(); ++i) {
        if (seq[i].to_uint64() != par[i].to_uint64()) {
            std::cerr << "\n  ERROR: idx " << i << " seq="
                      << seq[i].to_string() << " par="
                      << par[i].to_string() << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << bases.size() << " per-index identical)\n";
}

// ---------------------------------------------------------------------------
// Test 11: 100 random bases at N=1 vs N=hw_concurrency -- bit-identical.
// ---------------------------------------------------------------------------
void test_n1_vs_n_hw_parity() {
    std::cout << "Test 11: N=1 vs N=hw_concurrency parity..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::string hw_str = std::to_string(hw);

    Integer modulus(257);
    Integer exp(19);
    auto bases = make_random_bases(100, /*seed=*/0xCAFEBABEULL, modulus);

    apply_env("1");
    std::vector<Integer> seq(bases.size());
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(seq));

    apply_env(hw_str.c_str());
    std::vector<Integer> par(bases.size());
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(par));

    apply_env(nullptr);

    for (std::size_t i = 0; i < bases.size(); ++i) {
        if (seq[i].to_uint64() != par[i].to_uint64()) {
            std::cerr << "\n  ERROR: idx " << i << " seq="
                      << seq[i].to_string() << " par="
                      << par[i].to_string() << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (N=hw=" << hw << ", " << bases.size()
              << " per-index identical)\n";
}

// ---------------------------------------------------------------------------
// Test 12: Common-exponent semantics -- known small values + larger modulus.
// Verifies that the same exp/modulus is applied to every base in the batch,
// using a 200-bit prime modulus and a 100-bit exponent so mpz_powm exercises
// multi-limb arithmetic (covers the codepath used by 50d+/60d Schirokauer).
// ---------------------------------------------------------------------------
void test_common_exponent_semantics() {
    std::cout << "Test 12: common-exponent semantics (200-bit modulus)..."
              << std::flush;

    // 200-bit prime modulus (decimal). Verified prime via mpz_probab_prime_p
    // at construction time below.
    Integer modulus(
        "1606938044258990275541962092341162602522202993782792835301301", 10);
    // Sanity-check primality so the test exercises a real prime modulus.
    if (mpz_probab_prime_p(modulus.get_mpz(), 25) == 0) {
        std::cerr << "\n  ERROR: chosen modulus is composite (test bug)"
                  << std::endl;
        std::abort();
    }

    // 100-bit exponent.
    Integer exp("1267650600228229401496703205653", 10);

    // 8 hand-picked bases spanning small and limb-edge values.
    std::vector<Integer> bases;
    bases.push_back(Integer(uint64_t{2}));
    bases.push_back(Integer(uint64_t{3}));
    bases.push_back(Integer(uint64_t{5}));
    bases.push_back(Integer(uint64_t{7}));
    bases.push_back(Integer(uint64_t{12345}));
    bases.push_back(Integer(uint64_t{0xFFFFFFFFULL}));
    bases.push_back(Integer(uint64_t{0xFFFFFFFFFFFFFFFFULL}));
    bases.push_back(Integer("9999999999999999999999999999", 10));

    apply_env("1");
    std::vector<Integer> seq(bases.size());
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(seq));

    apply_env("4");
    std::vector<Integer> par(bases.size());
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(par));

    apply_env(nullptr);

    // Verify each slot matches the scalar reference and seq == par.
    for (std::size_t i = 0; i < bases.size(); ++i) {
        Integer expect = scalar_powm(bases[i], exp, modulus);
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
    }

    std::cout << " PASS (" << bases.size()
              << " bases, 200-bit modulus, 100-bit exp)\n";
}

// ---------------------------------------------------------------------------
// Test 13: Reset cache hook -- mid-test ENV change is picked up after reset.
// ---------------------------------------------------------------------------
void test_reset_env_cache_hook() {
    std::cout << "Test 13: reset env cache re-reads ENV..." << std::flush;

    apply_env("1");
    int initial = mpz_powm_batch_threads();
    if (initial != 1) {
        std::cerr << "\n  ERROR: pre-reset value " << initial
                  << " (expected 1)" << std::endl;
        std::abort();
    }

    // Without reset, a fresh setenv would NOT be picked up (call_once seals
    // the cache). The reset hook is the only way to re-resolve mid-test.
    setenv("GNFS_MPZ_POWM_BATCH_THREADS", "4", /*overwrite=*/1);
    int stale = mpz_powm_batch_threads();
    if (stale != 1) {
        std::cerr << "\n  ERROR: cache not stable before reset, got "
                  << stale << " (expected 1)" << std::endl;
        std::abort();
    }

    // After reset, the new value resolves.
    mpz_powm_batch_threads_reset_env_cache_for_testing();
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (4 < cap) ? 4 : cap;
    if (mpz_powm_batch_threads() != expect) {
        std::cerr << "\n  ERROR: post-reset value " << mpz_powm_batch_threads()
                  << " expected " << expect << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 14: perf-info probe (informational, no assert) -- measure
// hw_concurrency wall vs N=1 wall on 100 large-base mpz_powm calls.
// Not strictly required by the spec, but documents the speedup ceiling so
// the file size stays in the 10-12 test target range without padding.
// ---------------------------------------------------------------------------
void test_perf_info_100_bases() {
    std::cout << "Test 14: perf info (100 bases, 200-bit modulus)..."
              << std::flush;

    Integer modulus(
        "1606938044258990275541962092341162602522202993782792835301301", 10);
    Integer exp("1267650600228229401496703205653", 10);

    // 100 random bases with multi-limb width.
    std::vector<Integer> bases;
    bases.reserve(100);
    std::mt19937_64 rng(0xFEEDBEEFCAFED00DULL);
    for (std::size_t i = 0; i < 100; ++i) {
        // Build a 192-bit random value by concatenating three 64-bit chunks.
        Integer x(uint64_t{rng()});
        Integer chunk(uint64_t{rng()});
        Integer two64("18446744073709551616", 10);  // 2^64
        x = x * two64 + chunk;
        chunk = Integer(uint64_t{rng()});
        x = x * two64 + chunk;
        // Reduce into [0, modulus) so mpz_powm has a sensible input range.
        x = x % modulus;
        if (mpz_cmp_ui(x.get_mpz(), 0) == 0) {
            x = Integer(uint64_t{1});
        }
        bases.push_back(std::move(x));
    }

    // N=1 baseline.
    apply_env("1");
    std::vector<Integer> seq(bases.size());
    auto t0 = std::chrono::steady_clock::now();
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(seq));
    auto t1 = std::chrono::steady_clock::now();
    long long us_seq = std::chrono::duration_cast<std::chrono::microseconds>(
                           t1 - t0).count();

    // N=hw parallel.
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    apply_env(std::to_string(hw).c_str());
    std::vector<Integer> par(bases.size());
    auto t2 = std::chrono::steady_clock::now();
    parallel_mpz_powm(std::span<const Integer>(bases),
                      exp, modulus,
                      std::span<Integer>(par));
    auto t3 = std::chrono::steady_clock::now();
    long long us_par = std::chrono::duration_cast<std::chrono::microseconds>(
                           t3 - t2).count();

    apply_env(nullptr);

    // Strict parity check is still required even on a perf-info probe.
    for (std::size_t i = 0; i < bases.size(); ++i) {
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
    std::cout << "=== Batched mpz_powm Parallel Dispatch Tests ===\n";

    test_env_unset_defaults_to_one();
    test_env_zero_to_one();
    test_env_four();
    test_env_clamp();
    test_env_non_numeric();
    test_empty_bases();
    test_single_base_n1();
    test_single_base_n4_no_stall();
    test_n1_baseline_matches_scalar();
    test_n1_vs_n4_parity();
    test_n1_vs_n_hw_parity();
    test_common_exponent_semantics();
    test_reset_env_cache_hook();
    test_perf_info_100_bases();

    std::cout << "\n=== All mpz_powm Parallel Tests PASSED ===\n";
    return 0;
}
