// test_siqs_2lp.cpp - strict SIQS two-large-prime normalization contracts

#include <gnfs/siqs/two_large_prime.hpp>
#include <gnfs/siqs/siqs.hpp>

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>

namespace {

using gnfs::siqs::normalize_two_large_prime;
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

} // namespace

int main() {
    test_existing_splitter_accepts_exact_semiprimes();
    test_candidate_order_is_canonicalized();
    test_non_semiprimes_are_rejected();
    test_bounds_and_exact_product_are_required();

    std::cout << checks_passed << " checks passed, " << checks_failed
              << " checks failed\n";
    return checks_failed == 0 ? 0 : 1;
}
