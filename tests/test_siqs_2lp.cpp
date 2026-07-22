// test_siqs_2lp.cpp - strict SIQS two-large-prime normalization contracts

#include <gnfs/siqs/two_large_prime.hpp>
#include <gnfs/siqs/siqs.hpp>

#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace {

using gnfs::core::Integer;
using gnfs::siqs::FBPrime;
using gnfs::siqs::nonnegative_mpz_to_uint64_checked;
using gnfs::siqs::normalize_two_large_prime;
using gnfs::siqs::sieve_polynomial;
using gnfs::siqs::SIQSPoly;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::split_cofactor_64;
using gnfs::siqs::TwoLargePrimeFactors;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (condition) {                                                        \
            ++checks_passed;                                                    \
        } else {                                                                \
            ++checks_failed;                                                    \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':'       \
                      << __LINE__ << '\n';                                      \
        }                                                                       \
    } while (false)

void check_factors(const std::optional<TwoLargePrimeFactors>& result,
                   uint64_t expected_p,
                   uint64_t expected_q) {
    CHECK(result.has_value());
    if (result) {
        CHECK(result->p == expected_p);
        CHECK(result->q == expected_q);
    }
}

void test_existing_splitter_accepts_exact_semiprimes() {
    check_factors(normalize_two_large_prime(7 * 13, 13, split_cofactor_64(7 * 13)),
                  7,
                  13);

    constexpr uint64_t repeated_prime = 97;
    constexpr uint64_t square = repeated_prime * repeated_prime;
    check_factors(normalize_two_large_prime(
                      square, repeated_prime, split_cofactor_64(square)),
                  repeated_prime,
                  repeated_prime);
}

void test_candidate_order_is_canonicalized() {
    check_factors(normalize_two_large_prime(7 * 13, 13, {13, 7}), 7, 13);
}

void test_non_semiprimes_are_rejected() {
    CHECK(!normalize_two_large_prime(0, 100, {0, 0}));
    CHECK(!normalize_two_large_prime(1, 100, {0, 0}));

    constexpr uint64_t prime = 97;
    CHECK(!normalize_two_large_prime(prime, prime, split_cofactor_64(prime)));

    // split_cofactor_64(30) returns an exact factor pair, but one side is
    // composite. A three-prime cofactor must not cross the 2LP boundary.
    CHECK(!normalize_two_large_prime(2 * 3 * 5, 15, split_cofactor_64(2 * 3 * 5)));

    // This composite is a strong pseudoprime for several small bases. The
    // project-wide deterministic uint64 primality predicate must reject it.
    constexpr uint64_t pseudoprime = UINT64_C(341550071728321);
    constexpr uint64_t pseudoprime_product = 2 * pseudoprime;
    CHECK(!normalize_two_large_prime(
        pseudoprime_product, pseudoprime, {2, pseudoprime}));
}

void test_bounds_and_exact_product_are_required() {
    CHECK(!normalize_two_large_prime(7 * 13, 12, {7, 13}));
    CHECK(!normalize_two_large_prime(7 * 13, 13, {7, 11}));
    CHECK(!normalize_two_large_prime(7 * 13, 13, {0, 0}));
    CHECK(!normalize_two_large_prime(7 * 13, 91, {1, 91}));
    CHECK(!normalize_two_large_prime(4 * 9, 9, {4, 9}));

    // A forged pair whose multiplication would wrap must still fail closed.
    CHECK(!normalize_two_large_prime(
        15,
        std::numeric_limits<uint64_t>::max(),
        {std::numeric_limits<uint64_t>::max(), 15}));
}

void test_nonnegative_mpz_to_uint64_checked() {
    const Integer negative("-1");
    CHECK(!nonnegative_mpz_to_uint64_checked(negative.get_mpz()).has_value());

    const Integer zero(0);
    const auto zero_value = nonnegative_mpz_to_uint64_checked(zero.get_mpz());
    CHECK(zero_value.has_value());
    CHECK(zero_value.value_or(1) == 0);

    constexpr uint64_t above_u32_expected = UINT64_C(4294967311);
    const Integer above_u32("4294967311");
    const auto above_u32_value = nonnegative_mpz_to_uint64_checked(above_u32.get_mpz());
    CHECK(above_u32_value.has_value());
    CHECK(above_u32_value.value_or(0) == above_u32_expected);

    const Integer uint64_max("18446744073709551615");
    const auto uint64_max_value = nonnegative_mpz_to_uint64_checked(uint64_max.get_mpz());
    CHECK(uint64_max_value.has_value());
    CHECK(uint64_max_value.value_or(0) == std::numeric_limits<uint64_t>::max());

    const Integer above_uint64("18446744073709551616");
    CHECK(!nonnegative_mpz_to_uint64_checked(above_uint64.get_mpz()).has_value());
}

std::vector<SIQSRelation> collect_gmp_fallback_relations(const char* cofactor_text) {
    const Integer cofactor(cofactor_text);
    Integer q_value;
    mpz_mul_2exp(q_value.get_mpz(), cofactor.get_mpz(), 100);

    // With A=1 and B=q_value, the x=0 candidate has
    // Q(x)=B^2-N=q_value. Its >127-bit size forces the GMP fallback even
    // on platforms with native __int128 support. The x=-1 candidate leaves
    // the >64-bit residual q_value-1 and is rejected.
    Integer modulus;
    mpz_mul(modulus.get_mpz(), q_value.get_mpz(), q_value.get_mpz());
    mpz_sub(modulus.get_mpz(), modulus.get_mpz(), q_value.get_mpz());

    SIQSPoly poly;
    mpz_set_ui(poly.A.get_mpz(), 1);
    mpz_set(poly.B.get_mpz(), q_value.get_mpz());
    const uint32_t no_solution = std::numeric_limits<uint32_t>::max();
    poly.solns.assign(2, {no_solution, no_solution});

    const std::vector<FBPrime> factor_base = {
        {0, 0, 0},
        {2, 1, 1},
    };
    std::vector<SIQSRelation> relations;
    std::mutex relations_mutex;
    std::vector<uint8_t> sieve_buffer;
    std::vector<uint8_t> exponent_buffer(factor_base.size(), 0);

    sieve_polynomial(poly,
                     modulus,
                     factor_base,
                     1,
                     0,
                     3,
                     std::numeric_limits<uint32_t>::max(),
                     std::numeric_limits<uint64_t>::max(),
                     relations,
                     relations_mutex,
                     sieve_buffer,
                     exponent_buffer);
    return relations;
}

void test_llp64_gmp_fallback_classification() {
    // These values sit just above UINT32_MAX. On Windows LLP64, converting
    // either through unsigned long truncates it to the parenthesized value.
    // Prime 4294967311 (15) must be rejected as a 2LP composite candidate.
    CHECK(collect_gmp_fallback_relations("4294967311").empty());

    // Composite 4294967299 = 7 * 613566757 (3) must be retained for exact
    // 2LP normalization rather than misclassified from its low 32 bits.
    const auto relations = collect_gmp_fallback_relations("4294967299");
    CHECK(relations.size() == 1);
    for (const SIQSRelation& relation : relations) {
        CHECK(relation.large_prime == UINT64_C(4294967299));
        CHECK(relation.large_prime2 == 1);
        CHECK(relation.exponents.size() == 2);
        if (relation.exponents.size() == 2) {
            CHECK(relation.exponents[1] == 100);
        }
    }
}

} // namespace

int main() {
    test_existing_splitter_accepts_exact_semiprimes();
    test_candidate_order_is_canonicalized();
    test_non_semiprimes_are_rejected();
    test_bounds_and_exact_product_are_required();
    test_nonnegative_mpz_to_uint64_checked();
    test_llp64_gmp_fallback_classification();

    std::cout << checks_passed << " checks passed, " << checks_failed
              << " checks failed\n";
    return checks_failed == 0 ? 0 : 1;
}
