// test_mpz_invert_parallel.cpp -- batched mpz_invert parallel dispatcher tests
//
// Validates the GNFS_MPZ_INVERT_BATCH_THREADS env-gated dispatcher introduced
// in include/gnfs/util/mpz_invert_parallel.hpp (W12 T3, sister of W11 T3):
//
//   * ENV parsing handles unset / "0" / "1" / "4" / "garbage" / "" / "9999"
//     / "12abc" correctly; clamping at hardware_concurrency() * 2.
//   * Sequential (N=1, default) and parallel (N>=2) paths produce per-index
//     bit-identical (results, success) pairs for the same (bases, modulus)
//     input. The dispatcher is a pure parallel wrapper around `mpz_invert`,
//     so a 200-bit prime modulus + integer bases drive the parity assertions.
//   * Empty bases vector returns cleanly without creating a pool or invoking
//     any mpz operation.
//   * Single base under N>=2 short-circuits to sequential (exactly-once
//     mpz_invert invocation, no stall).
//   * 100-base random batch matches scalar mpz_invert reference at N=1 and
//     stays bit-identical at N=4 and N=hardware_concurrency.
//   * Failure case (gcd != 1) flagged correctly in both sequential and
//     parallel paths; callers can inspect the gcd using the original inputs.
//   * Output/modulus aliasing, forced output growth, and in-place batches
//     preserve the original modulus and leave failed slots unchanged.
//   * Cache reset hook re-parses ENV between assertions.

#include "support/test_check.hpp"
#include <gnfs/core/integer.hpp>
#include <gnfs/util/mpz_invert_parallel.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <gmp.h>
#include <initializer_list>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using gnfs::core::Integer;
using gnfs::util::mpz_invert_batch_threads;
using gnfs::util::mpz_invert_batch_threads_reset_env_cache_for_testing;
using gnfs::util::parallel_mpz_invert;
using gnfs::util::resolve_mpz_invert_batch_threads;

namespace {

// Helper: set or unset GNFS_MPZ_INVERT_BATCH_THREADS and refresh the cache so
// the next call to mpz_invert_batch_threads() reflects the new value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_MPZ_INVERT_BATCH_THREADS");
    } else {
        setenv("GNFS_MPZ_INVERT_BATCH_THREADS", value, /*overwrite=*/1);
    }
    mpz_invert_batch_threads_reset_env_cache_for_testing();
}

// Scalar reference: compute b^{-1} mod n for one base. Returns (inverse,
// success), where success==false means gcd(b, n) > 1 and inverse is the
// default-constructed Integer(0).
struct ScalarResult {
    Integer inverse;
    bool success = false;
};

ScalarResult scalar_invert(const Integer& base, const Integer& modulus) {
    ScalarResult r;
    int rc = mpz_invert(r.inverse.get_mpz(), base.get_mpz(), modulus.get_mpz());
    r.success = (rc != 0);
    return r;
}

// Build a vector of `n` Integer bases from a deterministic mt19937_64 seed,
// each base value uniformly drawn from [1, modulus - 1] so mpz_invert has
// non-trivial inputs to chew on. With a prime modulus every drawn value has
// gcd 1, so success will be true on every slot.
std::vector<Integer> make_random_bases(std::size_t n, uint64_t seed, const Integer& modulus) {
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
    int v = mpz_invert_batch_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: unset env parsed to " << v << ", expected 1" << std::endl;
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
    int v = mpz_invert_batch_threads();
    if (v != 1) {
        std::cerr << "\n  ERROR: '0' parsed to " << v << ", expected 1" << std::endl;
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
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (4 < cap) ? 4 : cap;

    apply_env("4");
    int v = mpz_invert_batch_threads();
    if (v != expect) {
        std::cerr << "\n  ERROR: '4' parsed to " << v << ", expected " << expect
                  << " (hw*2 cap = " << cap << ")" << std::endl;
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
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;

    apply_env("10000");
    int v = mpz_invert_batch_threads();
    if (v != cap) {
        std::cerr << "\n  ERROR: '10000' parsed to " << v << ", expected cap=" << cap << std::endl;
        std::abort();
    }
    apply_env(nullptr);
    std::cout << " PASS (cap=" << cap << ")\n";
}

// ---------------------------------------------------------------------------
// Test 5: ENV "garbage" / "" / "-5" / "   " / "12abc" -> partial-parse rules
// match W11 T3 semantics (std::atoi accepts numeric prefix).
// ---------------------------------------------------------------------------
void test_env_non_numeric() {
    std::cout << "Test 5: ENV non-numeric / boundary -> 1..." << std::flush;

    apply_env("");
    GNFS_TEST_CHECK(mpz_invert_batch_threads() == 1);

    apply_env("-5");
    GNFS_TEST_CHECK(mpz_invert_batch_threads() == 1);

    apply_env("garbage");
    GNFS_TEST_CHECK(mpz_invert_batch_threads() == 1);

    apply_env("   ");
    GNFS_TEST_CHECK(mpz_invert_batch_threads() == 1);

    // "12abc" -> 12 (std::atoi accepts a leading numeric prefix). Document
    // this behaviour explicitly so future readers know it is intentional
    // (matches W11 T3 / W10 T4 / W7-W11 family semantics).
    apply_env("12abc");
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (12 < cap) ? 12 : cap;
    int got = mpz_invert_batch_threads();
    if (got != expect) {
        std::cerr << "\n  ERROR: '12abc' parsed to " << got << ", expected " << expect
                  << " (atoi prefix semantics)" << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 6: Empty bases vector - no-op, no pool, no writes; returns empty
// success vector.
// ---------------------------------------------------------------------------
void test_empty_bases() {
    std::cout << "Test 6: empty bases (no-op)..." << std::flush;

    const std::vector<Integer> bases;
    for (const char* threads : {"1", "4"}) {
        apply_env(threads);
        for (const int modulus_value : {0, 257}) {
            const Integer modulus(modulus_value);
            for (const std::size_t output_count : {0u, 2u}) {
                std::vector<Integer> results(output_count, Integer(901));
                const auto original = results;
                const auto capacity = results.capacity();
                const auto address = reinterpret_cast<std::uintptr_t>(results.data());
                const auto success = parallel_mpz_invert(bases, modulus, results);
                GNFS_TEST_CHECK(success.empty());
                GNFS_TEST_CHECK(results == original);
                GNFS_TEST_CHECK(results.capacity() == capacity);
                GNFS_TEST_CHECK(reinterpret_cast<std::uintptr_t>(results.data()) == address);
            }
        }
    }

    // resolve_mpz_invert_batch_threads on empty batch returns 0.
    GNFS_TEST_CHECK(resolve_mpz_invert_batch_threads(0) == 0);

    apply_env(nullptr);
    std::cout << " PASS\n";
}

void test_zero_modulus_preserves_outputs() {
    std::cout << "Testing zero modulus rejection before output changes..." << std::flush;
    const std::vector<Integer> bases{Integer(1), Integer(2), Integer(3)};
    const Integer zero_modulus(0);
    for (const char* threads : {"1", "4"}) {
        apply_env(threads);
        for (const std::size_t output_count : {0u, 1u, 3u, 5u}) {
            std::vector<Integer> results(output_count, Integer(901));
            const auto original = results;
            const auto capacity = results.capacity();
            const auto address = reinterpret_cast<std::uintptr_t>(results.data());
            bool threw = false;
            try {
                (void)parallel_mpz_invert(bases, zero_modulus, results);
            } catch (const std::domain_error&) {
                threw = true;
            }
            GNFS_TEST_CHECK(threw);
            GNFS_TEST_CHECK(results == original);
            GNFS_TEST_CHECK(results.capacity() == capacity);
            GNFS_TEST_CHECK(reinterpret_cast<std::uintptr_t>(results.data()) == address);
        }

        // The modulus can refer to an output element that resize would invalidate.
        std::vector<Integer> results{Integer(0)};
        const auto capacity = results.capacity();
        const auto address = reinterpret_cast<std::uintptr_t>(results.data());
        bool threw = false;
        try {
            (void)parallel_mpz_invert(bases, results.front(), results);
        } catch (const std::domain_error&) {
            threw = true;
        }
        GNFS_TEST_CHECK(threw);
        GNFS_TEST_CHECK(results.size() == 1);
        GNFS_TEST_CHECK(results.front().is_zero());
        GNFS_TEST_CHECK(results.capacity() == capacity);
        GNFS_TEST_CHECK(reinterpret_cast<std::uintptr_t>(results.data()) == address);
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

void test_nonzero_modulus_boundaries() {
    std::cout << "Testing signed and unit modulus boundaries..." << std::flush;
    const std::vector<Integer> bases{Integer(2), Integer(-2), Integer(0), Integer(5)};
    for (const char* threads : {"1", "4"}) {
        apply_env(threads);
        for (const int modulus_value : {5, -5, 1, -1}) {
            std::vector<Integer> results(bases.size() + 1, Integer(901));
            const auto success = parallel_mpz_invert(bases, Integer(modulus_value), results);
            GNFS_TEST_CHECK(success.size() == bases.size());
            GNFS_TEST_CHECK(results.size() == bases.size() + 1);
            GNFS_TEST_CHECK(results.back() == Integer(901));
            if (modulus_value == 1 || modulus_value == -1) {
                for (std::size_t i = 0; i < bases.size(); ++i) {
                    GNFS_TEST_CHECK(success[i]);
                    GNFS_TEST_CHECK(results[i].is_zero());
                }
            } else {
                GNFS_TEST_CHECK(success[0] && success[1]);
                GNFS_TEST_CHECK(!success[2] && !success[3]);
                GNFS_TEST_CHECK(results[0] == Integer(3));
                GNFS_TEST_CHECK(results[1] == Integer(2));
            }
        }
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

std::vector<Integer> integer_values(std::initializer_list<int64_t> values) {
    std::vector<Integer> result;
    result.reserve(values.size());
    for (const int64_t value : values)
        result.emplace_back(value);
    return result;
}

void check_inverse_products(const std::vector<Integer>& original_bases,
                            const Integer& original_modulus, const std::vector<Integer>& results,
                            const std::vector<bool>& success) {
    GNFS_TEST_CHECK(success.size() == original_bases.size());
    GNFS_TEST_CHECK(results.size() >= original_bases.size());
    Integer magnitude(original_modulus);
    magnitude.abs();
    const Integer expected_residue = Integer(1) % magnitude;
    for (std::size_t i = 0; i < original_bases.size(); ++i) {
        if (!success[i])
            continue;
        GNFS_TEST_CHECK(!results[i].is_negative());
        GNFS_TEST_CHECK(results[i] < magnitude);
        Integer residue = (original_bases[i] * results[i]) % magnitude;
        if (residue.is_negative())
            residue += magnitude;
        GNFS_TEST_CHECK(residue == expected_residue);
    }
}

void test_modulus_aliases_output() {
    std::cout << "Testing modulus aliases an output slot..." << std::flush;
    const auto bases = integer_values({2, 3});
    for (const char* threads : {"1", "4"}) {
        apply_env(threads);
        for (const int modulus_value : {5, -5}) {
            const Integer original_modulus(modulus_value);
            for (const bool trailing_slot : {false, true}) {
                auto results = integer_values({modulus_value, 777});
                auto expected = integer_values({3, 2});
                if (trailing_slot) {
                    results.emplace_back(999);
                    expected.emplace_back(999);
                }
                const auto success = parallel_mpz_invert(bases, results.front(), results);
                GNFS_TEST_CHECK(success == std::vector<bool>({true, true}));
                GNFS_TEST_CHECK(results == expected);
                check_inverse_products(bases, original_modulus, results, success);
            }

            const std::vector<Integer> empty_bases;
            auto results = integer_values({modulus_value, 777});
            const auto original = results;
            const auto success = parallel_mpz_invert(empty_bases, results.front(), results);
            GNFS_TEST_CHECK(success.empty());
            GNFS_TEST_CHECK(results == original);
        }
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

void test_modulus_alias_survives_resize() {
    std::cout << "Testing output growth with an aliased modulus..." << std::flush;
    for (const char* threads : {"1", "4"}) {
        apply_env(threads);
        for (const int modulus_value : {5, -5}) {
            auto results = integer_values({modulus_value});
            const std::size_t count = results.capacity() + 1;
            std::vector<Integer> bases(count, Integer(2));
            bases.back() = int64_t{3};
            const Integer original_modulus(results.front());
            const auto success = parallel_mpz_invert(bases, results.front(), results);
            GNFS_TEST_CHECK(results.size() == count);
            GNFS_TEST_CHECK(success == std::vector<bool>(count, true));
            for (std::size_t i = 0; i < count; ++i)
                GNFS_TEST_CHECK(results[i] == Integer(i + 1 == count ? 2 : 3));
            check_inverse_products(bases, original_modulus, results, success);
        }
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

void test_in_place_inverse_ownership() {
    std::cout << "Testing in-place inverse ownership..." << std::flush;
    const auto original = integer_values({2, 3, 4, 5, 7, 0, 10, -2});
    const auto expected = integer_values({8, 3, 4, 5, 13, 0, 10, 7});
    const std::vector<bool> expected_success{true, false, true, false, true, false, false, true};
    for (const char* threads : {"1", "4"}) {
        apply_env(threads);
        for (const int modulus_value : {15, -15}) {
            auto values = original;
            const Integer modulus(modulus_value);
            const auto success = parallel_mpz_invert(values, modulus, values);
            GNFS_TEST_CHECK(success == expected_success);
            GNFS_TEST_CHECK(values == expected);
            check_inverse_products(original, modulus, values, success);
        }
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

void test_three_way_inverse_alias() {
    std::cout << "Testing shared bases, results, and modulus storage..." << std::flush;
    for (const char* threads : {"1", "4"}) {
        apply_env(threads);
        for (const int modulus_value : {15, -15, 1, -1}) {
            auto values = integer_values({modulus_value, 2, 3, 4, 7});
            const auto original = values;
            const Integer original_modulus(values.front());
            const auto success = parallel_mpz_invert(values, values.front(), values);
            if (modulus_value == 1 || modulus_value == -1) {
                // Here the first successful output replaces the modulus with zero.
                GNFS_TEST_CHECK(success == std::vector<bool>(original.size(), true));
                GNFS_TEST_CHECK(values == std::vector<Integer>(original.size()));
            } else {
                GNFS_TEST_CHECK(success == std::vector<bool>({false, true, false, true, true}));
                GNFS_TEST_CHECK(values == integer_values({modulus_value, 8, 3, 4, 13}));
            }
            check_inverse_products(original, original_modulus, values, success);
        }
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

void test_failed_inverse_slots_preserve_sentinels() {
    std::cout << "Testing successful commits preserve failed slots..." << std::flush;
    const auto bases = integer_values({2, 3, 0, 4, 5, 7, 10, -2});
    const auto expected = integer_values({8, 777, 777, 4, 777, 13, 777, 7});
    const std::vector<bool> expected_success{true, false, false, true, false, true, false, true};
    for (const char* threads : {"1", "4"}) {
        apply_env(threads);
        for (const int modulus_value : {15, -15}) {
            const Integer modulus(modulus_value);
            std::vector<Integer> results(bases.size(), Integer(777));
            const auto success = parallel_mpz_invert(bases, modulus, results);
            GNFS_TEST_CHECK(success == expected_success);
            GNFS_TEST_CHECK(results == expected);
            check_inverse_products(bases, modulus, results, success);

            const auto grow_bases = integer_values({3, 0, 2});
            auto grow_results = integer_values({777});
            const auto grow_success = parallel_mpz_invert(grow_bases, modulus, grow_results);
            GNFS_TEST_CHECK(grow_success == std::vector<bool>({false, false, true}));
            GNFS_TEST_CHECK(grow_results == integer_values({777, 0, 8}));
            check_inverse_products(grow_bases, modulus, grow_results, grow_success);
        }
    }
    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 7: Single base at N=1 -> correct b^{-1} mod n.
// 7^{-1} mod 257: 7 * x = 1 + 257*k. x = 110 (since 7 * 110 = 770 = 3*257 - 1
// ... let's verify by mpz_invert itself, no hand calc).
// ---------------------------------------------------------------------------
void test_single_base_n1() {
    std::cout << "Test 7: single base N=1 -> correct..." << std::flush;
    apply_env("1");

    Integer base(7);
    Integer modulus(257); // small prime

    std::vector<Integer> bases{base};
    std::vector<Integer> results(1);

    auto succ = parallel_mpz_invert(bases, modulus, results);
    GNFS_TEST_CHECK(succ.size() == 1);
    GNFS_TEST_CHECK(succ[0]); // gcd(7, 257) = 1, inverse exists.

    ScalarResult expect = scalar_invert(base, modulus);
    GNFS_TEST_CHECK(expect.success);
    if (results[0].to_uint64() != expect.inverse.to_uint64()) {
        std::cerr << "\n  ERROR: got " << results[0].to_string() << " expected "
                  << expect.inverse.to_string() << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS (7^{-1} mod 257 = " << results[0].to_string() << ")\n";
}

// ---------------------------------------------------------------------------
// Test 8: Single base at N=4 - exactly-once invocation, no stall, correct.
// ---------------------------------------------------------------------------
void test_single_base_n4_no_stall() {
    std::cout << "Test 8: single base N=4 (no stall, correct)..." << std::flush;
    apply_env("4");

    Integer base(5);
    Integer modulus(257);

    std::vector<Integer> bases{base};
    std::vector<Integer> results(1);

    auto t0 = std::chrono::steady_clock::now();
    auto succ = parallel_mpz_invert(bases, modulus, results);
    auto t1 = std::chrono::steady_clock::now();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    GNFS_TEST_CHECK(succ.size() == 1);
    GNFS_TEST_CHECK(succ[0]);

    ScalarResult expect = scalar_invert(base, modulus);
    if (results[0].to_uint64() != expect.inverse.to_uint64()) {
        std::cerr << "\n  ERROR: got " << results[0].to_string() << " expected "
                  << expect.inverse.to_string() << std::endl;
        std::abort();
    }

    // Sanity-bound the wall-time: if the helper accidentally spawned a
    // 4-thread pool the spin-up alone would push past this.
    if (ms > 1000) {
        std::cerr << "\n  WARN: single-base dispatch took " << ms << " ms (expected << 1000 ms)"
                  << std::endl;
        // No abort -- soft signal, sanitizers can be slow.
    }

    apply_env(nullptr);
    std::cout << " PASS (5^{-1} mod 257 = " << results[0].to_string() << ", " << ms << " ms)\n";
}

// ---------------------------------------------------------------------------
// Test 9: 100 random bases at N=1 baseline matches scalar reference.
// ---------------------------------------------------------------------------
void test_n1_baseline_matches_scalar() {
    std::cout << "Test 9: N=1 baseline matches scalar mpz_invert..." << std::flush;
    apply_env("1");

    Integer modulus(257); // small prime
    auto bases = make_random_bases(100, /*seed=*/12345ULL, modulus);

    std::vector<Integer> results(bases.size());
    auto succ = parallel_mpz_invert(bases, modulus, results);
    GNFS_TEST_CHECK(succ.size() == bases.size());

    for (std::size_t i = 0; i < bases.size(); ++i) {
        ScalarResult expect = scalar_invert(bases[i], modulus);
        // With prime modulus all bases in [1, p-1] are invertible.
        GNFS_TEST_CHECK(expect.success);
        if (!succ[i]) {
            std::cerr << "\n  ERROR: idx " << i << " success flag false but "
                      << "scalar reference succeeded" << std::endl;
            std::abort();
        }
        if (results[i].to_uint64() != expect.inverse.to_uint64()) {
            std::cerr << "\n  ERROR: idx " << i << " got " << results[i].to_string() << " expected "
                      << expect.inverse.to_string() << std::endl;
            std::abort();
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (" << bases.size() << " bases)\n";
}

// ---------------------------------------------------------------------------
// Test 10: 100 random bases at N=1 vs N=4 -- per-index bit-identical
// (results AND success bits).
// ---------------------------------------------------------------------------
void test_n1_vs_n4_parity() {
    std::cout << "Test 10: N=1 vs N=4 parity (per-index bit-identical)..." << std::flush;

    // 200-bit prime modulus to exercise multi-limb arithmetic.
    Integer modulus("1606938044258990275541962092341162602522202993782792835301301", 10);
    if (mpz_probab_prime_p(modulus.get_mpz(), 25) == 0) {
        std::cerr << "\n  ERROR: chosen modulus is composite (test bug)" << std::endl;
        std::abort();
    }

    // Hand-built 100-base spread spanning small / limb-edge / multi-limb.
    std::vector<Integer> bases;
    bases.reserve(100);
    std::mt19937_64 rng(0xDEADBEEFULL);
    for (std::size_t i = 0; i < 100; ++i) {
        Integer x(uint64_t{rng()});
        Integer chunk(uint64_t{rng()});
        Integer two64("18446744073709551616", 10); // 2^64
        x = x * two64 + chunk;
        chunk = Integer(uint64_t{rng()});
        x = x * two64 + chunk;
        x = x % modulus;
        if (mpz_cmp_ui(x.get_mpz(), 0) == 0) {
            x = Integer(uint64_t{1});
        }
        bases.push_back(std::move(x));
    }

    apply_env("1");
    std::vector<Integer> seq(bases.size());
    auto seq_succ = parallel_mpz_invert(bases, modulus, seq);

    apply_env("4");
    std::vector<Integer> par(bases.size());
    auto par_succ = parallel_mpz_invert(bases, modulus, par);

    apply_env(nullptr);

    GNFS_TEST_CHECK(seq_succ.size() == bases.size());
    GNFS_TEST_CHECK(par_succ.size() == bases.size());

    for (std::size_t i = 0; i < bases.size(); ++i) {
        if (seq_succ[i] != par_succ[i]) {
            std::cerr << "\n  ERROR: idx " << i << " success seq=" << static_cast<int>(seq_succ[i])
                      << " par=" << static_cast<int>(par_succ[i]) << std::endl;
            std::abort();
        }
        if (mpz_cmp(seq[i].get_mpz(), par[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i].to_string()
                      << " par=" << par[i].to_string() << std::endl;
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
    if (hw == 0)
        hw = 4;
    std::string hw_str = std::to_string(hw);

    // 200-bit prime modulus (same as Test 10 for consistency).
    Integer modulus("1606938044258990275541962092341162602522202993782792835301301", 10);

    std::vector<Integer> bases;
    bases.reserve(100);
    std::mt19937_64 rng(0xCAFEBABEULL);
    for (std::size_t i = 0; i < 100; ++i) {
        Integer x(uint64_t{rng()});
        Integer chunk(uint64_t{rng()});
        Integer two64("18446744073709551616", 10);
        x = x * two64 + chunk;
        x = x % modulus;
        if (mpz_cmp_ui(x.get_mpz(), 0) == 0) {
            x = Integer(uint64_t{1});
        }
        bases.push_back(std::move(x));
    }

    apply_env("1");
    std::vector<Integer> seq(bases.size());
    auto seq_succ = parallel_mpz_invert(bases, modulus, seq);

    apply_env(hw_str.c_str());
    std::vector<Integer> par(bases.size());
    auto par_succ = parallel_mpz_invert(bases, modulus, par);

    apply_env(nullptr);

    for (std::size_t i = 0; i < bases.size(); ++i) {
        if (seq_succ[i] != par_succ[i]) {
            std::cerr << "\n  ERROR: idx " << i << " success mismatch" << std::endl;
            std::abort();
        }
        if (mpz_cmp(seq[i].get_mpz(), par[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: idx " << i << " seq=" << seq[i].to_string()
                      << " par=" << par[i].to_string() << std::endl;
            std::abort();
        }
    }

    std::cout << " PASS (N=hw=" << hw << ", " << bases.size() << " per-index identical)\n";
}

// ---------------------------------------------------------------------------
// Test 12: Failure case (gcd != 1). Use a COMPOSITE modulus n = p*q so
// bases that share a factor with n produce mpz_invert failure. Verify
// success bit is false at exactly the gcd-collision indices in both
// sequential and parallel paths, and that results at failing slots are
// untouched (still hold the default-constructed Integer(0) value).
// ---------------------------------------------------------------------------
void test_failure_case_gcd_nontrivial() {
    std::cout << "Test 12: failure case (gcd != 1, both N=1 and N=4)..." << std::flush;

    // n = 15 = 3 * 5 (small composite, easy to reason about).
    Integer modulus(15);

    // Bases: mix of invertible and non-invertible values.
    // Invertible (gcd(b, 15) = 1): 1, 2, 4, 7, 8, 11, 13, 14
    // Non-invertible (gcd > 1): 3, 5, 6, 9, 10, 12
    std::vector<Integer> bases;
    bases.push_back(Integer(1));  // gcd=1, expect success
    bases.push_back(Integer(3));  // gcd=3, expect FAILURE
    bases.push_back(Integer(2));  // gcd=1, expect success
    bases.push_back(Integer(5));  // gcd=5, expect FAILURE
    bases.push_back(Integer(4));  // gcd=1, expect success
    bases.push_back(Integer(6));  // gcd=3, expect FAILURE
    bases.push_back(Integer(7));  // gcd=1, expect success
    bases.push_back(Integer(10)); // gcd=5, expect FAILURE
    bases.push_back(Integer(11)); // gcd=1, expect success
    bases.push_back(Integer(12)); // gcd=3, expect FAILURE

    // Expected success pattern at each index.
    std::vector<bool> expected_succ{true,  false, true,  false, true,
                                    false, true,  false, true,  false};

    // --- Sequential (N=1) ---
    apply_env("1");
    // Pre-fill with a sentinel value so we can verify failed slots are
    // untouched. After dispatch, failed slots should still read 0xDEADBEEF.
    std::vector<Integer> seq(bases.size());
    for (auto& r : seq)
        r = Integer(uint64_t{0xDEADBEEFULL});
    auto seq_succ = parallel_mpz_invert(bases, modulus, seq);

    GNFS_TEST_CHECK(seq_succ.size() == bases.size());
    for (std::size_t i = 0; i < bases.size(); ++i) {
        if (seq_succ[i] != expected_succ[i]) {
            std::cerr << "\n  ERROR: seq idx " << i << " base " << bases[i].to_string()
                      << " success=" << static_cast<int>(seq_succ[i]) << " expected "
                      << static_cast<int>(expected_succ[i]) << std::endl;
            std::abort();
        }
        // Failed slots: helper guarantees we do NOT write, so sentinel
        // remains. Successful slots: must hold the correct inverse.
        if (!seq_succ[i]) {
            if (seq[i].to_uint64() != 0xDEADBEEFULL) {
                std::cerr << "\n  ERROR: seq idx " << i << " failed slot was overwritten, got "
                          << seq[i].to_string() << std::endl;
                std::abort();
            }
        } else {
            ScalarResult expect = scalar_invert(bases[i], modulus);
            GNFS_TEST_CHECK(expect.success);
            if (mpz_cmp(seq[i].get_mpz(), expect.inverse.get_mpz()) != 0) {
                std::cerr << "\n  ERROR: seq idx " << i << " got " << seq[i].to_string()
                          << " expected " << expect.inverse.to_string() << std::endl;
                std::abort();
            }
        }
    }

    // --- Parallel (N=4) ---
    apply_env("4");
    std::vector<Integer> par(bases.size());
    for (auto& r : par)
        r = Integer(uint64_t{0xDEADBEEFULL});
    auto par_succ = parallel_mpz_invert(bases, modulus, par);

    for (std::size_t i = 0; i < bases.size(); ++i) {
        if (par_succ[i] != expected_succ[i]) {
            std::cerr << "\n  ERROR: par idx " << i << " base " << bases[i].to_string()
                      << " success=" << static_cast<int>(par_succ[i]) << " expected "
                      << static_cast<int>(expected_succ[i]) << std::endl;
            std::abort();
        }
        if (!par_succ[i]) {
            if (par[i].to_uint64() != 0xDEADBEEFULL) {
                std::cerr << "\n  ERROR: par idx " << i << " failed slot was overwritten, got "
                          << par[i].to_string() << std::endl;
                std::abort();
            }
        } else {
            ScalarResult expect = scalar_invert(bases[i], modulus);
            GNFS_TEST_CHECK(expect.success);
            if (mpz_cmp(par[i].get_mpz(), expect.inverse.get_mpz()) != 0) {
                std::cerr << "\n  ERROR: par idx " << i << " got " << par[i].to_string()
                          << " expected " << expect.inverse.to_string() << std::endl;
                std::abort();
            }
        }
    }

    apply_env(nullptr);
    std::cout << " PASS (10 bases, 4 success / 6 fail, seq + par agree)\n";
}

// ---------------------------------------------------------------------------
// Test 13: Reset cache hook -- mid-test ENV change is picked up after reset.
// ---------------------------------------------------------------------------
void test_reset_env_cache_hook() {
    std::cout << "Test 13: reset env cache re-reads ENV..." << std::flush;

    apply_env("1");
    int initial = mpz_invert_batch_threads();
    if (initial != 1) {
        std::cerr << "\n  ERROR: pre-reset value " << initial << " (expected 1)" << std::endl;
        std::abort();
    }

    // Without reset, a fresh setenv would NOT be picked up (call_once seals
    // the cache). The reset hook is the only way to re-resolve mid-test.
    setenv("GNFS_MPZ_INVERT_BATCH_THREADS", "4", /*overwrite=*/1);
    int stale = mpz_invert_batch_threads();
    if (stale != 1) {
        std::cerr << "\n  ERROR: cache not stable before reset, got " << stale << " (expected 1)"
                  << std::endl;
        std::abort();
    }

    // After reset, the new value resolves.
    mpz_invert_batch_threads_reset_env_cache_for_testing();
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    int cap = static_cast<int>(hw) * 2;
    int expect = (4 < cap) ? 4 : cap;
    if (mpz_invert_batch_threads() != expect) {
        std::cerr << "\n  ERROR: post-reset value " << mpz_invert_batch_threads() << " expected "
                  << expect << std::endl;
        std::abort();
    }

    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 14: perf-info probe (informational, no parity-skipped) -- measure
// hw_concurrency wall vs N=1 wall on 100 multi-limb mpz_invert calls. Strict
// parity check still mandatory even on perf probes.
// ---------------------------------------------------------------------------
void test_perf_info_100_bases() {
    std::cout << "Test 14: perf info (100 bases, 200-bit modulus)..." << std::flush;

    Integer modulus("1606938044258990275541962092341162602522202993782792835301301", 10);

    // 100 random bases with multi-limb width.
    std::vector<Integer> bases;
    bases.reserve(100);
    std::mt19937_64 rng(0xFEEDBEEFCAFED00DULL);
    for (std::size_t i = 0; i < 100; ++i) {
        Integer x(uint64_t{rng()});
        Integer chunk(uint64_t{rng()});
        Integer two64("18446744073709551616", 10);
        x = x * two64 + chunk;
        chunk = Integer(uint64_t{rng()});
        x = x * two64 + chunk;
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
    auto seq_succ = parallel_mpz_invert(bases, modulus, seq);
    auto t1 = std::chrono::steady_clock::now();
    long long us_seq = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    // N=hw parallel.
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    apply_env(std::to_string(hw).c_str());
    std::vector<Integer> par(bases.size());
    auto t2 = std::chrono::steady_clock::now();
    auto par_succ = parallel_mpz_invert(bases, modulus, par);
    auto t3 = std::chrono::steady_clock::now();
    long long us_par = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();

    apply_env(nullptr);

    // Strict parity check is still required even on a perf-info probe.
    for (std::size_t i = 0; i < bases.size(); ++i) {
        if (seq_succ[i] != par_succ[i]) {
            std::cerr << "\n  ERROR: perf probe success-bit parity break at idx " << i << std::endl;
            std::abort();
        }
        if (mpz_cmp(seq[i].get_mpz(), par[i].get_mpz()) != 0) {
            std::cerr << "\n  ERROR: perf probe parity break at idx " << i << std::endl;
            std::abort();
        }
    }

    double speedup = (us_par > 0) ? static_cast<double>(us_seq) / static_cast<double>(us_par) : 0.0;
    std::cout << " INFO seq=" << us_seq << " us, N=" << hw << " par=" << us_par
              << " us, speedup=" << speedup << "x (parity verified)\n";
}

} // namespace

int main() {
    std::cout << "=== Batched mpz_invert Parallel Dispatch Tests ===\n";

    test_env_unset_defaults_to_one();
    test_env_zero_to_one();
    test_env_four();
    test_env_clamp();
    test_env_non_numeric();
    test_empty_bases();
    test_zero_modulus_preserves_outputs();
    test_nonzero_modulus_boundaries();
    test_modulus_aliases_output();
    test_modulus_alias_survives_resize();
    test_in_place_inverse_ownership();
    test_three_way_inverse_alias();
    test_failed_inverse_slots_preserve_sentinels();
    test_single_base_n1();
    test_single_base_n4_no_stall();
    test_n1_baseline_matches_scalar();
    test_n1_vs_n4_parity();
    test_n1_vs_n_hw_parity();
    test_failure_case_gcd_nontrivial();
    test_reset_env_cache_hook();
    test_perf_info_100_bases();

    std::cout << "\n=== All mpz_invert Parallel Tests PASSED ===\n";
    return 0;
}
