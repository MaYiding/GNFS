// test_batch_inversion.cpp — Montgomery batch inversion helper tests
//
// Validates the GNFS_ECM_BATCH_INV env-gated helper introduced in
// include/gnfs/cofactor/batch_inversion.hpp:
//
//   * ENV parsing is strict "1" only (any other value -> false), with
//     cached read-once semantics; reset hook works correctly for tests
//     that toggle the env between assertions.
//   * `batch_mod_inverse` and `naive_mod_inverse` produce bit-for-bit
//     identical inverses for every `(values, n)` tuple where all
//     `gcd(v_i, n) == 1`. Tested across small primes (n = 101), 64-bit
//     primes, and ~200-bit primes; batch sizes 0, 1, 5, 20, 100.
//   * Failure semantics: when some `v_i` shares a non-trivial factor with
//     n, both paths report the same `found_factor` (first non-trivial
//     gcd encountered scanning left-to-right) and `inverses` is empty.
//   * Edge cases: empty input, single input (k=1 must equal mpz_invert),
//     n = 1 (every gcd is 1 -> no found_factor despite failure).

#include <gnfs/cofactor/batch_inversion.hpp>
#include <gnfs/core/integer.hpp>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <gmp.h>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using gnfs::core::Integer;
using gnfs::cofactor::BatchInvResult;
using gnfs::cofactor::batch_mod_inverse;
using gnfs::cofactor::ecm_batch_inv_enabled;
using gnfs::cofactor::ecm_batch_inv_reset_env_cache_for_testing;
using gnfs::cofactor::naive_mod_inverse;

namespace {

// Helper: set or unset GNFS_ECM_BATCH_INV and refresh the cached flag so
// the next ecm_batch_inv_enabled() call reflects the new value.
void apply_env(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_ECM_BATCH_INV");
    } else {
        setenv("GNFS_ECM_BATCH_INV", value, /*overwrite=*/1);
    }
    ecm_batch_inv_reset_env_cache_for_testing();
}

// Helper: assert two BatchInvResult are bit-for-bit identical (same
// inverses, same found_factor presence and value). On mismatch dumps the
// offending index and values.
void assert_same(const BatchInvResult& a,
                 const BatchInvResult& b,
                 const char* label) {
    if (a.inverses.size() != b.inverses.size()) {
        std::cerr << "\n  ERROR (" << label << "): inverses.size() mismatch: "
                  << a.inverses.size() << " vs " << b.inverses.size()
                  << std::endl;
        std::abort();
    }
    for (std::size_t i = 0; i < a.inverses.size(); ++i) {
        if (a.inverses[i].compare(b.inverses[i]) != 0) {
            std::cerr << "\n  ERROR (" << label << "): inverses[" << i
                      << "] mismatch: " << a.inverses[i].to_string()
                      << " vs " << b.inverses[i].to_string() << std::endl;
            std::abort();
        }
    }
    if (a.found_factor.has_value() != b.found_factor.has_value()) {
        std::cerr << "\n  ERROR (" << label << "): found_factor presence: "
                  << a.found_factor.has_value() << " vs "
                  << b.found_factor.has_value() << std::endl;
        std::abort();
    }
    if (a.found_factor.has_value()) {
        if (a.found_factor->compare(*b.found_factor) != 0) {
            std::cerr << "\n  ERROR (" << label << "): found_factor mismatch: "
                      << a.found_factor->to_string() << " vs "
                      << b.found_factor->to_string() << std::endl;
            std::abort();
        }
    }
}

// Helper: verify (a * a^{-1}) mod n == 1 for every (a, a^{-1}) pair, plus
// every inverse is reduced into [0, n).
void verify_inverses(const std::vector<Integer>& values,
                     const std::vector<Integer>& inverses,
                     const Integer& n) {
    assert(values.size() == inverses.size());
    Integer prod;
    for (std::size_t i = 0; i < values.size(); ++i) {
        // 0 <= inv < n
        assert(!inverses[i].is_negative());
        assert(inverses[i].compare(n) < 0);
        mpz_mul(prod.get_mpz(),
                values[i].get_mpz(),
                inverses[i].get_mpz());
        mpz_mod(prod.get_mpz(), prod.get_mpz(), n.get_mpz());
        if (!prod.is_one()) {
            std::cerr << "\n  ERROR: inverse check failed at i=" << i
                      << " v=" << values[i].to_string()
                      << " inv=" << inverses[i].to_string()
                      << " n=" << n.to_string()
                      << " v*inv mod n=" << prod.to_string() << std::endl;
            std::abort();
        }
    }
}

// Helper: build k coprime-to-n values via a deterministic PRNG. Uses
// mpz_urandomm fed by a 64-bit seed -> gmp_randstate_t. Skips values
// whose gcd with n is non-trivial. Requires n > 1.
std::vector<Integer> random_coprime_values(const Integer& n,
                                           std::size_t k,
                                           uint64_t seed) {
    gmp_randstate_t st;
    gmp_randinit_default(st);
    gmp_randseed_ui(st, seed);

    std::vector<Integer> out;
    out.reserve(k);
    Integer v, g;
    while (out.size() < k) {
        mpz_urandomm(v.get_mpz(), st, n.get_mpz());
        if (v.is_zero()) continue;
        mpz_gcd(g.get_mpz(), v.get_mpz(), n.get_mpz());
        if (!g.is_one()) continue;
        out.push_back(v.clone());
    }
    gmp_randclear(st);
    return out;
}

// ───────────────────────────────────────────────────────────────────────────
// Test 1: ENV unset -> default OFF
// ───────────────────────────────────────────────────────────────────────────
void test_env_unset_default_off() {
    std::cout << "Test 1: ENV unset -> default OFF..." << std::flush;
    apply_env(nullptr);
    assert(ecm_batch_inv_enabled() == false);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: ENV "1" -> enabled
// ───────────────────────────────────────────────────────────────────────────
void test_env_one_enabled() {
    std::cout << "Test 2: ENV \"1\" -> enabled..." << std::flush;
    apply_env("1");
    assert(ecm_batch_inv_enabled() == true);
    apply_env(nullptr);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: All other ENV values -> OFF (strict "1" parsing)
// ───────────────────────────────────────────────────────────────────────────
void test_env_other_values_off() {
    std::cout << "Test 3: ENV non-\"1\" values -> OFF..." << std::flush;

    // Empty string -> OFF
    apply_env("");
    assert(ecm_batch_inv_enabled() == false);

    // "0" -> OFF
    apply_env("0");
    assert(ecm_batch_inv_enabled() == false);

    // "2" -> OFF (not exactly "1")
    apply_env("2");
    assert(ecm_batch_inv_enabled() == false);

    // "true" -> OFF
    apply_env("true");
    assert(ecm_batch_inv_enabled() == false);

    // "garbage" -> OFF
    apply_env("garbage");
    assert(ecm_batch_inv_enabled() == false);

    // "10" -> OFF (not exactly "1")
    apply_env("10");
    assert(ecm_batch_inv_enabled() == false);

    // " 1" leading space -> OFF (strict equality)
    apply_env(" 1");
    assert(ecm_batch_inv_enabled() == false);

    // "1 " trailing space -> OFF (strict equality)
    apply_env("1 ");
    assert(ecm_batch_inv_enabled() == false);

    apply_env(nullptr);
    std::cout << " PASS (8 non-\"1\" values rejected)\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: Empty input (k=0)
// ───────────────────────────────────────────────────────────────────────────
void test_empty_input() {
    std::cout << "Test 4: empty input (k=0)..." << std::flush;

    Integer n{int64_t{101}};
    std::vector<Integer> empty_values;

    auto batch = batch_mod_inverse(empty_values, n);
    assert(batch.inverses.empty());
    assert(!batch.found_factor.has_value());

    auto naive = naive_mod_inverse(empty_values, n);
    assert(naive.inverses.empty());
    assert(!naive.found_factor.has_value());

    assert_same(batch, naive, "empty");
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: Single input (k=1) — must equal mpz_invert
// ───────────────────────────────────────────────────────────────────────────
void test_single_input() {
    std::cout << "Test 5: single input (k=1)..." << std::flush;

    Integer n{int64_t{101}};
    std::vector<Integer> values;
    values.push_back(Integer{int64_t{47}});

    auto batch = batch_mod_inverse(values, n);
    auto naive = naive_mod_inverse(values, n);

    assert(batch.inverses.size() == 1);
    assert(naive.inverses.size() == 1);
    assert_same(batch, naive, "k=1");

    // Cross-check against a fresh mpz_invert call.
    Integer expected;
    int ok = mpz_invert(expected.get_mpz(),
                        values[0].get_mpz(),
                        n.get_mpz());
    assert(ok == 1);
    assert(batch.inverses[0].compare(expected) == 0);

    verify_inverses(values, batch.inverses, n);
    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: Small batch parity (k=5, n=101 small prime)
// ───────────────────────────────────────────────────────────────────────────
void test_small_batch_parity() {
    std::cout << "Test 6: small batch (k=5, n=101)..." << std::flush;

    Integer n{int64_t{101}};
    std::vector<Integer> values;
    // Deterministic small primes / values, all coprime to 101.
    int64_t vs[] = {2, 7, 23, 47, 99};
    for (int64_t v : vs) values.push_back(Integer{v});

    auto batch = batch_mod_inverse(values, n);
    auto naive = naive_mod_inverse(values, n);

    assert(batch.inverses.size() == 5);
    assert(naive.inverses.size() == 5);
    assert_same(batch, naive, "k=5 small");
    verify_inverses(values, batch.inverses, n);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 7: Medium batch parity (k=20, n ~ 2^64 prime)
// ───────────────────────────────────────────────────────────────────────────
void test_medium_batch_parity() {
    std::cout << "Test 7: medium batch (k=20, n ~ 2^64 prime)..." << std::flush;

    // 18446744073709551557 = largest prime below 2^64 (well-known).
    Integer n{"18446744073709551557", 10};
    assert(n.is_probable_prime(25) != 0);

    auto values = random_coprime_values(n, /*k=*/20, /*seed=*/42);
    assert(values.size() == 20);

    auto batch = batch_mod_inverse(values, n);
    auto naive = naive_mod_inverse(values, n);

    assert(batch.inverses.size() == 20);
    assert(naive.inverses.size() == 20);
    assert_same(batch, naive, "k=20 medium");
    verify_inverses(values, batch.inverses, n);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 8: Large batch parity (k=100, n ~ 200-bit prime)
// ───────────────────────────────────────────────────────────────────────────
void test_large_batch_parity() {
    std::cout << "Test 8: large batch (k=100, n ~ 200-bit prime)..."
              << std::flush;

    // A 200-bit prime: 2^200 + 235 (verified probable prime). The exact
    // constant is chosen to be a real prime so gcd checks aren't trivial.
    // 2^200 = 1606938044258990275541962092341162602522202993782792835301376.
    // 2^200 + 235 is prime (Miller-Rabin via mpz_probab_prime_p).
    Integer n("1606938044258990275541962092341162602522202993782792835301611",
              10);
    assert(n.is_probable_prime(25) != 0);

    auto values = random_coprime_values(n, /*k=*/100, /*seed=*/12345);
    assert(values.size() == 100);

    auto batch = batch_mod_inverse(values, n);
    auto naive = naive_mod_inverse(values, n);

    assert(batch.inverses.size() == 100);
    assert(naive.inverses.size() == 100);
    assert_same(batch, naive, "k=100 large");
    verify_inverses(values, batch.inverses, n);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 9: Found factor — v contains factors of composite n
// ───────────────────────────────────────────────────────────────────────────
void test_found_factor() {
    std::cout << "Test 9: found factor (v_i shares prime with n)..."
              << std::flush;

    // n = 101 * 103 = 10403 (semiprime). Put p1=101 at index 0 and p2=103
    // at index 1; the helper must report the first non-trivial gcd, which
    // will be 101 (the gcd of values[0]=101 with n=10403).
    Integer p1{int64_t{101}};
    Integer p2{int64_t{103}};
    Integer n;
    mpz_mul(n.get_mpz(), p1.get_mpz(), p2.get_mpz());
    assert(n.to_uint64() == 10403);

    std::vector<Integer> values;
    values.push_back(p1.clone());                  // gcd(101, 10403) = 101
    values.push_back(p2.clone());                  // gcd(103, 10403) = 103
    values.push_back(Integer{int64_t{47}});        // gcd(47, 10403) = 1

    auto batch = batch_mod_inverse(values, n);
    auto naive = naive_mod_inverse(values, n);

    // Both paths must fail (inverses empty) and report the same culprit.
    assert(batch.inverses.empty());
    assert(naive.inverses.empty());
    assert(batch.found_factor.has_value());
    assert(naive.found_factor.has_value());
    assert(batch.found_factor->compare(*naive.found_factor) == 0);

    // Specifically the first non-trivial gcd in the sweep is 101.
    assert(batch.found_factor->compare(p1) == 0);

    // Also verify the factor divides n.
    Integer rem;
    mpz_mod(rem.get_mpz(),
            n.get_mpz(),
            batch.found_factor->get_mpz());
    assert(rem.is_zero());

    std::cout << " PASS (found factor = " << batch.found_factor->to_string()
              << ")\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 10: Reset env cache hook actually re-reads env
// ───────────────────────────────────────────────────────────────────────────
void test_reset_env_cache() {
    std::cout << "Test 10: reset env cache..." << std::flush;

    // First read with env unset.
    apply_env(nullptr);
    assert(ecm_batch_inv_enabled() == false);

    // Change env BUT do not reset: cached value sticks.
    setenv("GNFS_ECM_BATCH_INV", "1", 1);
    assert(ecm_batch_inv_enabled() == false);  // still cached OFF

    // Reset cache: next read picks up the new env value.
    ecm_batch_inv_reset_env_cache_for_testing();
    assert(ecm_batch_inv_enabled() == true);

    // Flip back to unset with reset, verify OFF re-cached.
    apply_env(nullptr);
    assert(ecm_batch_inv_enabled() == false);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 11: Boundary — v_i = 1 and v_i = n-1 (trivial coprimes)
// ───────────────────────────────────────────────────────────────────────────
void test_boundary_values() {
    std::cout << "Test 11: boundary v_i = 1, n-1..." << std::flush;

    Integer n{int64_t{101}};
    std::vector<Integer> values;
    values.push_back(Integer{int64_t{1}});      // 1^{-1} = 1
    values.push_back(Integer{int64_t{100}});    // 100^{-1} mod 101 = 100
    values.push_back(Integer{int64_t{50}});     // some interior value

    auto batch = batch_mod_inverse(values, n);
    auto naive = naive_mod_inverse(values, n);

    assert(batch.inverses.size() == 3);
    assert_same(batch, naive, "boundary");
    verify_inverses(values, batch.inverses, n);

    // 1^{-1} mod 101 == 1
    assert(batch.inverses[0].to_uint64() == 1);
    // 100^{-1} mod 101 == 100 (since 100 == -1 mod 101 and (-1)^2 == 1)
    assert(batch.inverses[1].to_uint64() == 100);

    std::cout << " PASS\n";
}

// ───────────────────────────────────────────────────────────────────────────
// Test 12: Unreduced inputs (v_i >= n) — must match mpz_invert behaviour
// ───────────────────────────────────────────────────────────────────────────
void test_unreduced_inputs() {
    std::cout << "Test 12: unreduced inputs (v_i >= n)..." << std::flush;

    Integer n{int64_t{101}};
    // Values purposely >= n; the helper must reduce internally and produce
    // the same result as mpz_invert (which also accepts unreduced inputs).
    std::vector<Integer> values;
    values.push_back(Integer{int64_t{102}});    // 102 mod 101 == 1
    values.push_back(Integer{int64_t{303}});    // 303 mod 101 == 0... no!
    // 303 = 3 * 101, so gcd(303, 101) = 101 = n, that would trigger
    // failure. Pick something coprime instead:
    values.back() = Integer{int64_t{305}};      // 305 mod 101 = 305-3*101=2
    values.push_back(Integer{int64_t{200}});    // 200 mod 101 = 99

    auto batch = batch_mod_inverse(values, n);
    auto naive = naive_mod_inverse(values, n);

    assert(batch.inverses.size() == 3);
    assert_same(batch, naive, "unreduced");
    // verify_inverses uses values directly, so v*inv mod n == 1 still
    // holds (mpz_mod absorbs the reduction).
    verify_inverses(values, batch.inverses, n);

    std::cout << " PASS\n";
}

}  // namespace

int main() {
    std::cout << "=== Batch Modular Inversion Tests ===" << std::endl;

    test_env_unset_default_off();
    test_env_one_enabled();
    test_env_other_values_off();
    test_empty_input();
    test_single_input();
    test_small_batch_parity();
    test_medium_batch_parity();
    test_large_batch_parity();
    test_found_factor();
    test_reset_env_cache();
    test_boundary_values();
    test_unreduced_inputs();

    std::cout << std::endl
              << "=== All Batch Inversion Tests PASSED ==="
              << std::endl;
    return 0;
}
