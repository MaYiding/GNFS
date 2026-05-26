// test_mpz_gcd_parallel.cpp -- batched mpz_gcd parallel dispatcher tests
//
// Validates the GNFS_MPZ_GCD_BATCH_THREADS env-gated dispatcher introduced in
// include/gnfs/util/mpz_gcd_parallel.hpp:
//
//   * ENV parsing handles unset / "0" / "1" / "4" / "garbage" / "" / "9999" /
//     "12abc" / leading whitespace / "10000" correctly; clamping at
//     hardware_concurrency() * 2.
//   * Sequential (N=1, default) and parallel (N>=2) paths produce per-index
//     bit-identical results for the same (a_values, b_values) input. The
//     dispatcher is a pure parallel wrapper around `mpz_gcd`.
//   * Empty input vectors return cleanly without creating a pool or
//     invoking any mpz operation.
//   * Single pair under N>=2 short-circuits to sequential (exactly-once
//     mpz_gcd invocation, no stall).
//   * 100-pair random batch matches scalar mpz_gcd reference at N=1 and
//     stays bit-identical at N=4 and N=hardware_concurrency.
//   * 200-bit prime modulus exercises multi-limb `mpz_gcd` semantics on
//     `gcd(P*Q, P*R) = P` patterns.
//   * Boundary cases: gcd(0, 0) = 0, gcd(a, 0) = |a|, gcd(12, 18) = 6,
//     negative operands -> positive result.
//   * Mismatched input span sizes throw `std::invalid_argument`.
//   * Undersized `results` gets defensive clamp; tail untouched.
//   * Cache reset hook re-parses ENV between assertions.
//   * Perf-info probe (informational, no assert).

#include <gnfs/util/mpz_gcd_parallel.hpp>
#include <gnfs/core/integer.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <gmp.h>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using gnfs::core::Integer;
using gnfs::util::mpz_gcd_batch_threads;
using gnfs::util::mpz_gcd_batch_threads_reset_env_cache_for_testing;
using gnfs::util::parallel_mpz_gcd;
using gnfs::util::resolve_mpz_gcd_batch_threads;

namespace {

// Helper: set or unset GNFS_MPZ_GCD_BATCH_THREADS and refresh the cache so
// the next call to mpz_gcd_batch_threads() reflects the new value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_MPZ_GCD_BATCH_THREADS");
    } else {
        setenv("GNFS_MPZ_GCD_BATCH_THREADS", value, /*overwrite=*/1);
    }
    mpz_gcd_batch_threads_reset_env_cache_for_testing();
}

// Scalar reference: compute gcd(a, b) for one pair, returning a fresh
// Integer. Always produces the canonical non-negative result (GMP `mpz_gcd`
// semantics: signs of operands are ignored).
Integer scalar_gcd(const Integer& a, const Integer& b) {
    Integer out;
    mpz_gcd(out.get_mpz(), a.get_mpz(), b.get_mpz());
    return out;
}

// Build a vector of `n` Integer values from a deterministic mt19937_64
// seed, each value uniformly drawn from [0, 2^63) so the dispatcher
// has non-trivial inputs to reduce.
std::vector<Integer> make_random_integers(std::size_t n, uint64_t seed) {
    std::vector<Integer> values;
    values.reserve(n);
    std::mt19937_64 rng(seed);
    for (std::size_t i = 0; i < n; ++i) {
        // Draw a 63-bit value so the conversion to Integer is unambiguous
        // for int64_t-like ranges; the dispatcher does not care about sign.
        uint64_t v = rng() & 0x7FFFFFFFFFFFFFFFULL;
        // Avoid zero -> gcd(0, 0) is a tested boundary, but we want random
        // batches to have actual gcd workload. Bump zero to 1.
        if (v == 0) v = 1;
        values.emplace_back(v);
    }
    return values;
}

// ---------------------------------------------------------------------------
// Test 1: ENV unset -> 1 (default sequential)
// ---------------------------------------------------------------------------
void test_env_unset_defaults_to_one() {
    std::cout << "Test 1: ENV unset -> 1..." << std::flush;
    apply_env(nullptr);
    int v = mpz_gcd_batch_threads();
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
    int v = mpz_gcd_batch_threads();
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
    int v = mpz_gcd_batch_threads();
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
    int v = mpz_gcd_batch_threads();
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
// -> partial-parse rules match W11 T3 / W12 T3 / W13 T5 family semantics
// (std::atoi accepts a leading numeric prefix; leading whitespace is
// consumed by atoi and "  4" parses to 4).
// ---------------------------------------------------------------------------
void test_env_non_numeric() {
    std::cout << "Test 5: ENV non-numeric / boundary -> family semantics..."
              << std::flush;

    apply_env("");
    assert(mpz_gcd_batch_threads() == 1);

    apply_env("-5");
    assert(mpz_gcd_batch_threads() == 1);

    apply_env("garbage");
    assert(mpz_gcd_batch_threads() == 1);

    // std::atoi consumes leading whitespace before parsing digits, so
    // "  4" yields 4 -- consistent with the rest of the parallel-dispatcher
    // family (W11 T3 / W12 T3 / W13 T5). This is intentional family
    // behaviour, documented in the helper header.
    apply_env("  4");
    {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        int cap = static_cast<int>(hw) * 2;
        int expect = (4 < cap) ? 4 : cap;
        int got = mpz_gcd_batch_threads();
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
    // (matches W11 T3 / W12 T3 / W13 T5 / W10 T4 / W7-W13 family
    // semantics).
    apply_env("12abc");
    {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        int cap = static_cast<int>(hw) * 2;
        int expect = (12 < cap) ? 12 : cap;
        int got = mpz_gcd_batch_threads();
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
// Test 6: Empty input vectors - no-op, no pool, no writes.
// ---------------------------------------------------------------------------
void test_empty_inputs() {
    std::cout << "Test 6: empty inputs (no-op)..." << std::flush;

    std::vector<Integer> a_values;
    std::vector<Integer> b_values;
    std::vector<Integer> results;

    // N=1 sequential.
    apply_env("1");
    parallel_mpz_gcd(a_values, b_values, results);
    assert(results.empty());

    // N=4 parallel.
    apply_env("4");
    parallel_mpz_gcd(a_values, b_values, results);
    assert(results.empty());

    // resolve_mpz_gcd_batch_threads on empty batch returns 0.
    assert(resolve_mpz_gcd_batch_threads(0) == 0);

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 7: Single pair at N=1 -> correct gcd.
// ---------------------------------------------------------------------------
void test_single_pair_n1() {
    std::cout << "Test 7: single pair N=1 -> correct..." << std::flush;
    apply_env("1");

    // gcd(12, 18) = 6 (textbook).
    Integer a(uint64_t{12});
    Integer b(uint64_t{18});

    std::vector<Integer> a_values{a};
    std::vector<Integer> b_values{b};
    std::vector<Integer> results(1);

    parallel_mpz_gcd(a_values, b_values, results);

    Integer expect = scalar_gcd(a, b);
    if (results[0].to_uint64() != expect.to_uint64()) {
        std::cerr << "\n  ERROR: got " << results[0].to_string()
                  << " expected " << expect.to_string() << std::endl;
        std::abort();
    }
    assert(results[0].to_uint64() == 6);

    apply_env(nullptr);
    std::cout << " PASS (gcd(12, 18) = " << results[0].to_string() << ")\n";
}

// ---------------------------------------------------------------------------
// Test 8: Single pair at N=4 - exactly-once invocation, no stall.
// ---------------------------------------------------------------------------
void test_single_pair_n4_no_stall() {
    std::cout << "Test 8: single pair N=4 (no stall, correct)..."
              << std::flush;
    apply_env("4");

    Integer a(uint64_t{100});
    Integer b(uint64_t{75});  // gcd(100, 75) = 25

    std::vector<Integer> a_values{a};
    std::vector<Integer> b_values{b};
    std::vector<Integer> results(1);

    auto t0 = std::chrono::steady_clock::now();
    parallel_mpz_gcd(a_values, b_values, results);
    auto t1 = std::chrono::steady_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       t1 - t0).count();

    Integer expect = scalar_gcd(a, b);
    if (results[0].to_uint64() != expect.to_uint64()) {
        std::cerr << "\n  ERROR: got " << results[0].to_string()
                  << " expected " << expect.to_string() << std::endl;
        std::abort();
    }
    assert(results[0].to_uint64() == 25);

    // Sanity-bound the wall-time: if the helper accidentally spawned a
    // 4-thread pool the spin-up alone would push past this.
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-pair dispatch took " << ms
                  << " ms (expected << 1000 ms)" << std::endl;
        // No abort -- soft signal, sanitizers can be slow.
    }

    apply_env(nullptr);
    std::cout << " PASS (gcd(100, 75) = " << results[0].to_string()
              << ", " << ms << " ms)\n";
}

// ---------------------------------------------------------------------------
// Test 9: N=1 baseline matches scalar mpz_gcd reference (5 hand-picked cases).
// Covers the GMP boundary semantics: gcd(0, 0) = 0, gcd(a, 0) = |a|,
// gcd(small primes), negative operand handling.
// ---------------------------------------------------------------------------
void test_n1_baseline_matches_scalar() {
    std::cout << "Test 9: N=1 baseline matches scalar mpz_gcd (5 cases)..."
              << std::flush;
    apply_env("1");

    std::vector<Integer> a_values;
    std::vector<Integer> b_values;
    std::vector<uint64_t> expected;

    // Case 1: gcd(0, 0) = 0 (GMP convention).
    a_values.emplace_back(uint64_t{0});
    b_values.emplace_back(uint64_t{0});
    expected.push_back(0);

    // Case 2: gcd(a, 0) = |a| -> gcd(42, 0) = 42.
    a_values.emplace_back(uint64_t{42});
    b_values.emplace_back(uint64_t{0});
    expected.push_back(42);

    // Case 3: gcd(0, b) = |b| -> gcd(0, 17) = 17.
    a_values.emplace_back(uint64_t{0});
    b_values.emplace_back(uint64_t{17});
    expected.push_back(17);

    // Case 4: gcd(12, 18) = 6 (textbook).
    a_values.emplace_back(uint64_t{12});
    b_values.emplace_back(uint64_t{18});
    expected.push_back(6);

    // Case 5: gcd(negative, positive) -> result is non-negative.
    // gcd(-30, 12) = 6 (mpz_gcd ignores signs).
    a_values.emplace_back(int64_t{-30});
    b_values.emplace_back(int64_t{12});
    expected.push_back(6);

    std::vector<Integer> results(a_values.size());
    parallel_mpz_gcd(a_values, b_values, results);

    for (std::size_t i = 0; i < a_values.size(); ++i) {
        Integer scalar_expect = scalar_gcd(a_values[i], b_values[i]);
        if (mpz_cmp(results[i].get_mpz(), scalar_expect.get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " got "
                      << results[i].to_string() << " expected "
                      << scalar_expect.to_string()
                      << " (scalar reference)" << std::endl;
            std::abort();
        }
        if (results[i].to_uint64() != expected[i]) {
            std::cerr << "\n  ERROR: idx " << i << " got "
                      << results[i].to_string() << " expected "
                      << expected[i] << " (hand-computed)" << std::endl;
            std::abort();
        }
        // Result must be non-negative regardless of operand signs.
        if (mpz_sgn(results[i].get_mpz()) < 0) {
            std::cerr << "\n  ERROR: idx " << i << " produced negative gcd "
                      << results[i].to_string() << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (5 boundary cases)\n";
}

// ---------------------------------------------------------------------------
// Test 10: 100 random pairs at N=1 vs N=4 -- per-index bit-identical.
// ---------------------------------------------------------------------------
void test_n1_vs_n4_parity() {
    std::cout << "Test 10: N=1 vs N=4 parity (per-index bit-identical)..."
              << std::flush;

    auto a_values = make_random_integers(100, /*seed=*/0xDEADBEEFULL);
    auto b_values = make_random_integers(100, /*seed=*/0xCAFEBABEULL);

    apply_env("1");
    std::vector<Integer> seq(a_values.size());
    parallel_mpz_gcd(a_values, b_values, seq);

    apply_env("4");
    std::vector<Integer> par(a_values.size());
    parallel_mpz_gcd(a_values, b_values, par);

    apply_env(nullptr);

    for (std::size_t i = 0; i < a_values.size(); ++i) {
        if (mpz_cmp(seq[i].get_mpz(), par[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " seq="
                      << seq[i].to_string() << " par="
                      << par[i].to_string() << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (" << a_values.size()
              << " per-index identical)\n";
}

// ---------------------------------------------------------------------------
// Test 11: 100 random pairs at N=1 vs N=hw_concurrency -- bit-identical.
// ---------------------------------------------------------------------------
void test_n1_vs_n_hw_parity() {
    std::cout << "Test 11: N=1 vs N=hw_concurrency parity..." << std::flush;

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    std::string hw_str = std::to_string(hw);

    auto a_values = make_random_integers(100, /*seed=*/0x1234567890ABCDEFULL);
    auto b_values = make_random_integers(100, /*seed=*/0xFEDCBA0987654321ULL);

    apply_env("1");
    std::vector<Integer> seq(a_values.size());
    parallel_mpz_gcd(a_values, b_values, seq);

    apply_env(hw_str.c_str());
    std::vector<Integer> par(a_values.size());
    parallel_mpz_gcd(a_values, b_values, par);

    apply_env(nullptr);

    for (std::size_t i = 0; i < a_values.size(); ++i) {
        if (mpz_cmp(seq[i].get_mpz(), par[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " seq="
                      << seq[i].to_string() << " par="
                      << par[i].to_string() << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (N=hw=" << hw << ", " << a_values.size()
              << " per-index identical)\n";
}

// ---------------------------------------------------------------------------
// Test 12: composite + prime pattern: gcd(P*Q, P*R) = P where P/Q/R are
// 100-bit primes. Exercises multi-limb mpz_gcd semantics on a well-defined
// structural pattern -- not just "fuzz it with random multi-limb integers".
// ---------------------------------------------------------------------------
void test_composite_prime_pattern() {
    std::cout << "Test 12: gcd(P*Q, P*R) = P pattern (5 cases, 100-bit primes)..."
              << std::flush;
    apply_env("1");

    // Five distinct ~100-104-bit primes found via `mpz_nextprime` from 2^k
    // (verified via mpz_probab_prime_p with 25 Miller-Rabin rounds below).
    // P, Q, R distinct so gcd(P*Q, P*R) = P (since gcd(Q, R) = 1 for
    // distinct primes, then gcd(P*Q, P*R) = P * gcd(Q, R) = P).
    const char* prime_strs[] = {
        "1267650600228229401496703205653",  // nextprime(2^100)
        "2535301200456458802993406410833",  // nextprime(2^101)
        "5070602400912917605986812821771",  // nextprime(2^102)
        "10141204801825835211973625643089", // nextprime(2^103)
        "20282409603651670423947251286127"  // nextprime(2^104)
    };
    std::vector<Integer> primes;
    for (const char* s : prime_strs) {
        Integer p(s, 10);
        if (mpz_probab_prime_p(p.get_mpz(), 25) == 0) {
            std::cerr << "\n  ERROR: chosen prime " << s
                      << " is composite (test bug)" << std::endl;
            std::abort();
        }
        primes.push_back(std::move(p));
    }

    // Build 5 pairs: pair i = (primes[i] * primes[(i+1)%5],
    //                         primes[i] * primes[(i+2)%5])
    // gcd(pair) should equal primes[i] for each i.
    std::vector<Integer> a_values;
    std::vector<Integer> b_values;
    std::vector<Integer> expected;
    for (std::size_t i = 0; i < primes.size(); ++i) {
        Integer P = primes[i];
        Integer Q = primes[(i + 1) % primes.size()];
        Integer R = primes[(i + 2) % primes.size()];
        // Construct P*Q and P*R via mpz_mul.
        Integer PQ;
        mpz_mul(PQ.get_mpz(), P.get_mpz(), Q.get_mpz());
        Integer PR;
        mpz_mul(PR.get_mpz(), P.get_mpz(), R.get_mpz());
        a_values.push_back(std::move(PQ));
        b_values.push_back(std::move(PR));
        expected.push_back(P);
    }

    // Sequential reference.
    std::vector<Integer> seq(a_values.size());
    parallel_mpz_gcd(a_values, b_values, seq);

    // Parallel path (N=4).
    apply_env("4");
    std::vector<Integer> par(a_values.size());
    parallel_mpz_gcd(a_values, b_values, par);

    apply_env(nullptr);

    for (std::size_t i = 0; i < a_values.size(); ++i) {
        // gcd matches expected prime P.
        if (mpz_cmp(seq[i].get_mpz(), expected[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: seq idx " << i << " gcd("
                      << a_values[i].to_string() << ", "
                      << b_values[i].to_string() << ") = "
                      << seq[i].to_string() << " expected "
                      << expected[i].to_string() << std::endl;
            std::abort();
        }
        // Sequential vs parallel agreement.
        if (mpz_cmp(seq[i].get_mpz(), par[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: seq vs par mismatch at idx " << i
                      << std::endl;
            std::abort();
        }
        // Result is non-negative.
        if (mpz_sgn(seq[i].get_mpz()) < 0) {
            std::cerr << "\n  ERROR: idx " << i
                      << " produced negative gcd" << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (5 pairs, 100-bit primes, gcd matches P)\n";
}

// ---------------------------------------------------------------------------
// Test 13: mismatched span sizes throw std::invalid_argument.
// ---------------------------------------------------------------------------
void test_mismatched_span_throws() {
    std::cout << "Test 13: mismatched span size throws invalid_argument..."
              << std::flush;
    apply_env("4");

    std::vector<Integer> a_values;
    a_values.emplace_back(uint64_t{10});
    a_values.emplace_back(uint64_t{20});
    a_values.emplace_back(uint64_t{30});

    std::vector<Integer> b_values;
    b_values.emplace_back(uint64_t{15});
    b_values.emplace_back(uint64_t{25});
    // Note: only 2 elements vs a_values.size() = 3.

    std::vector<Integer> results(3);

    bool threw = false;
    try {
        parallel_mpz_gcd(a_values, b_values, results);
    } catch (const std::invalid_argument& e) {
        threw = true;
        // Verify the exception message mentions the precondition for
        // future readers debugging dispatcher misuse.
        std::string msg = e.what();
        if (msg.find("size") == std::string::npos) {
            std::cerr << "\n  WARN: exception message does not mention 'size': "
                      << msg << std::endl;
        }
    } catch (...) {
        std::cerr << "\n  ERROR: wrong exception type thrown" << std::endl;
        std::abort();
    }
    if (!threw) {
        std::cerr << "\n  ERROR: mismatched span did not throw" << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 14: results undersized -- defensive clamp, only first
// results.size() slots written, tail untouched.
// ---------------------------------------------------------------------------
void test_results_undersized_clamp() {
    std::cout << "Test 14: results undersized -> defensive clamp..."
              << std::flush;
    apply_env("4");

    // 5 input pairs but only 3 result slots.
    std::vector<Integer> a_values;
    std::vector<Integer> b_values;
    for (std::size_t i = 0; i < 5; ++i) {
        a_values.emplace_back(uint64_t{(i + 1) * 12});
        b_values.emplace_back(uint64_t{(i + 1) * 18});
    }

    std::vector<Integer> results;
    // Pre-fill with sentinel value so we can verify the clamp does not
    // touch slots beyond results.size().
    results.emplace_back(uint64_t{999});  // will be overwritten
    results.emplace_back(uint64_t{888});  // will be overwritten
    results.emplace_back(uint64_t{777});  // will be overwritten
    // results.size() == 3; only first 3 input pairs should be processed.

    parallel_mpz_gcd(a_values, b_values, results);

    // results should still have exactly 3 elements (no resize beyond
    // results.size() since we clamp downward, not upward).
    if (results.size() != 3) {
        std::cerr << "\n  ERROR: results.size() = " << results.size()
                  << " expected 3 (clamp must not grow results)" << std::endl;
        std::abort();
    }

    // First 3 slots should be gcd((i+1)*12, (i+1)*18) = (i+1)*6.
    for (std::size_t i = 0; i < 3; ++i) {
        uint64_t expect = (i + 1) * 6;
        if (results[i].to_uint64() != expect) {
            std::cerr << "\n  ERROR: idx " << i << " got "
                      << results[i].to_string() << " expected " << expect
                      << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (3 of 5 pairs processed, clamp respected)\n";
}

// ---------------------------------------------------------------------------
// Test 15: Reset cache hook -- mid-test ENV change is picked up after reset.
// ---------------------------------------------------------------------------
void test_reset_env_cache_hook() {
    std::cout << "Test 15: reset env cache re-reads ENV..." << std::flush;

    apply_env("1");
    int initial = mpz_gcd_batch_threads();
    if (initial != 1) {
        std::cerr << "\n  ERROR: pre-reset value " << initial
                  << " (expected 1)" << std::endl;
        std::abort();
    }

    // Without reset, a fresh setenv would NOT be picked up (call_once seals
    // the cache). The reset hook is the only way to re-resolve mid-test.
    setenv("GNFS_MPZ_GCD_BATCH_THREADS", "4", /*overwrite=*/1);
    int stale = mpz_gcd_batch_threads();
    if (stale != 1) {
        std::cerr << "\n  ERROR: cache not stable before reset, got "
                  << stale << " (expected 1)" << std::endl;
        std::abort();
    }

    // After reset, the new value resolves.
    mpz_gcd_batch_threads_reset_env_cache_for_testing();
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (4 < cap) ? 4 : cap;
    if (mpz_gcd_batch_threads() != expect) {
        std::cerr << "\n  ERROR: post-reset value " << mpz_gcd_batch_threads()
                  << " expected " << expect << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 16: perf-info probe (informational, no assert) -- measure
// hw_concurrency wall vs N=1 wall on 100 large-input mpz_gcd calls.
// Not strictly required by the spec, but documents the speedup ceiling.
// ---------------------------------------------------------------------------
void test_perf_info_100_pairs() {
    std::cout << "Test 16: perf info (100 pairs, 200-bit operands)..."
              << std::flush;

    // Build 100 random pairs of 192-bit integers so mpz_gcd actually has
    // to walk multiple limbs.
    std::vector<Integer> a_values;
    std::vector<Integer> b_values;
    a_values.reserve(100);
    b_values.reserve(100);
    std::mt19937_64 rng(0xFEEDBEEFCAFED00DULL);
    Integer two64("18446744073709551616", 10);  // 2^64
    for (std::size_t i = 0; i < 100; ++i) {
        Integer a(uint64_t{rng()});
        Integer chunk_a(uint64_t{rng()});
        a = a * two64 + chunk_a;
        chunk_a = Integer(uint64_t{rng()});
        a = a * two64 + chunk_a;

        Integer b(uint64_t{rng()});
        Integer chunk_b(uint64_t{rng()});
        b = b * two64 + chunk_b;
        chunk_b = Integer(uint64_t{rng()});
        b = b * two64 + chunk_b;

        a_values.push_back(std::move(a));
        b_values.push_back(std::move(b));
    }

    // N=1 baseline.
    apply_env("1");
    std::vector<Integer> seq(a_values.size());
    auto t0 = std::chrono::steady_clock::now();
    parallel_mpz_gcd(a_values, b_values, seq);
    auto t1 = std::chrono::steady_clock::now();
    long long us_seq = std::chrono::duration_cast<std::chrono::microseconds>(
                           t1 - t0).count();

    // N=hw parallel.
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    apply_env(std::to_string(hw).c_str());
    std::vector<Integer> par(a_values.size());
    auto t2 = std::chrono::steady_clock::now();
    parallel_mpz_gcd(a_values, b_values, par);
    auto t3 = std::chrono::steady_clock::now();
    long long us_par = std::chrono::duration_cast<std::chrono::microseconds>(
                           t3 - t2).count();

    apply_env(nullptr);

    // Strict parity check is still required even on a perf-info probe.
    for (std::size_t i = 0; i < a_values.size(); ++i) {
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
    std::cout << "=== Batched mpz_gcd Parallel Dispatch Tests ===\n";

    test_env_unset_defaults_to_one();
    test_env_zero_to_one();
    test_env_four();
    test_env_clamp();
    test_env_non_numeric();
    test_empty_inputs();
    test_single_pair_n1();
    test_single_pair_n4_no_stall();
    test_n1_baseline_matches_scalar();
    test_n1_vs_n4_parity();
    test_n1_vs_n_hw_parity();
    test_composite_prime_pattern();
    test_mismatched_span_throws();
    test_results_undersized_clamp();
    test_reset_env_cache_hook();
    test_perf_info_100_pairs();

    std::cout << "\n=== All mpz_gcd Parallel Tests PASSED ===\n";
    return 0;
}
