// test_squfof.cpp — deterministic SQUFOF factorization regression tests

#include <gnfs/cofactor/squfof.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

using gnfs::cofactor::SQUFOF;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        ++tests_failed; \
        return; \
    } \
} while (0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    ++tests_passed; \
} while (0)

// Deliberately independent of SQUFOF: bounded trial division is the reference
// oracle for the small exhaustive corpus and for the fixed semiprime metadata.
uint64_t smallest_factor_by_trial_division(uint64_t n) {
    if (n < 2) return 0;
    if ((n & 1) == 0) return 2;
    for (uint64_t divisor = 3; divisor <= n / divisor; divisor += 2) {
        if (n % divisor == 0) return divisor;
    }
    return n;
}

bool is_prime_by_trial_division(uint64_t n) {
    return n >= 2 && smallest_factor_by_trial_division(n) == n;
}

bool is_proper_factor(uint64_t n, uint64_t factor) {
    return factor > 1 && factor < n && n % factor == 0;
}

void test_api_boundaries() {
    TEST_ASSERT(SQUFOF::factor(0) == 1, "factor(0) must report failure");
    TEST_ASSERT(SQUFOF::factor(1) == 1, "factor(1) must report failure");
    TEST_ASSERT(SQUFOF::factor(2) == 2, "factor(2) must return 2");
    TEST_ASSERT(SQUFOF::factor(UINT64_C(1) << 62) == 2,
                "the supported-range upper boundary must take the even fast path");
    TEST_ASSERT(SQUFOF::factor((UINT64_C(1) << 62) - 1, 1) == 1,
                "near-uint64 discriminant must preserve the bounded failure path");

    TEST_PASS("API boundaries");
}

void test_exhaustive_small_composites() {
    constexpr uint64_t upper_bound = 4095;
    size_t checked = 0;

    for (uint64_t n = 4; n <= upper_bound; ++n) {
        const uint64_t reference_factor = smallest_factor_by_trial_division(n);
        if (reference_factor == n) continue;

        const uint64_t factor = SQUFOF::factor(n);
        TEST_ASSERT(is_proper_factor(n, factor),
                    "invalid factor " << factor << " for small composite " << n
                    << " (oracle factor " << reference_factor << ")");
        ++checked;
    }

    TEST_ASSERT(checked == 3530, "small-composite corpus unexpectedly changed");
    TEST_PASS("exhaustive composites through 4095");
}

void test_fixed_semiprime_corpus() {
    struct SemiprimeCase {
        uint64_t p;
        uint64_t q;
    };

    // Exact factor pairs span the tiny path through 60-bit cofactors. Close and
    // skewed pairs exercise different continued-fraction periods.
    constexpr std::array<SemiprimeCase, 12> cases{{
        {3, 5},
        {7, 11},
        {11, 13},
        {17, 19},
        {97, 103},
        {307, 313},
        {1009, 1000003},
        {10007, 10009},
        {65537, 1000003},
        {1000003, 1000033},
        {10000019, 10000079},
        {1000000007, 1000000009},
    }};

    for (const auto& test_case : cases) {
        TEST_ASSERT(is_prime_by_trial_division(test_case.p),
                    "invalid corpus metadata: p=" << test_case.p << " is not prime");
        TEST_ASSERT(is_prime_by_trial_division(test_case.q),
                    "invalid corpus metadata: q=" << test_case.q << " is not prime");
        TEST_ASSERT(test_case.p <= std::numeric_limits<uint64_t>::max() / test_case.q,
                    "semiprime corpus multiplication overflow");

        const uint64_t n = test_case.p * test_case.q;
        const uint64_t factor = SQUFOF::factor(n);
        TEST_ASSERT(factor == test_case.p || factor == test_case.q,
                    "unexpected factor " << factor << " for semiprime " << n
                    << " = " << test_case.p << " * " << test_case.q);

        const uint64_t repeated_factor = SQUFOF::factor(n);
        TEST_ASSERT(repeated_factor == factor,
                    "non-deterministic result for semiprime " << n << ": "
                    << factor << " then " << repeated_factor);
    }

    TEST_PASS("fixed semiprimes through 60 bits");
}

void test_prime_corpus() {
    constexpr std::array<uint64_t, 9> primes{{
        3,
        5,
        7,
        11,
        97,
        65537,
        1000003,
        1000000007,
        2147483647,
    }};

    for (uint64_t prime : primes) {
        TEST_ASSERT(is_prime_by_trial_division(prime),
                    "invalid prime corpus metadata: " << prime);
        TEST_ASSERT(SQUFOF::factor(prime) == 1,
                    "prime " << prime << " must report failure");
    }

    TEST_PASS("prime failure corpus");
}

void test_perfect_squares_and_powers() {
    constexpr std::array<uint64_t, 7> odd_roots{{
        3,
        5,
        7,
        11,
        65537,
        1000003,
        2147483647,
    }};

    for (uint64_t root : odd_roots) {
        TEST_ASSERT(root <= std::numeric_limits<uint64_t>::max() / root,
                    "square corpus multiplication overflow");
        const uint64_t square = root * root;
        TEST_ASSERT(SQUFOF::factor(square) == root,
                    "odd square " << square << " must return its exact root " << root);
    }

    TEST_ASSERT(SQUFOF::factor(8) == 2, "8 = 2^3 must return 2");
    const uint64_t factor_27 = SQUFOF::factor(27);
    TEST_ASSERT(factor_27 == 3 || factor_27 == 9,
                "27 = 3^3 must return 3 or 9");
    TEST_ASSERT(SQUFOF::factor(625) == 25, "625 = 5^4 must return its square root");

    TEST_PASS("perfect squares and prime powers");
}

void test_iteration_budget_contract() {
    constexpr std::array<uint64_t, 4> odd_non_squares{{
        15,
        77,
        9991,
        UINT64_C(1000003) * UINT64_C(1000033),
    }};

    // One forward iteration never reaches the even-step square check. These
    // exact failures protect the public iteration cap during loop unrolling.
    for (uint64_t n : odd_non_squares) {
        TEST_ASSERT(SQUFOF::factor(n, 1) == 1,
                    "one-iteration budget must fail for odd non-square " << n);
        TEST_ASSERT(is_proper_factor(n, SQUFOF::factor(n)),
                    "default budget must still factor regression input " << n);
    }

    // Preprocessing is intentionally outside the forward-iteration budget.
    TEST_ASSERT(SQUFOF::factor(14, 1) == 2,
                "even fast path must ignore the forward-iteration budget");
    TEST_ASSERT(SQUFOF::factor(49, 1) == 7,
                "square fast path must ignore the forward-iteration budget");

    TEST_PASS("iteration-budget failure and preprocessing contract");
}

}  // namespace

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  SQUFOF Deterministic Regression Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_api_boundaries();
    test_exhaustive_small_composites();
    test_fixed_semiprime_corpus();
    test_prime_corpus();
    test_perfect_squares_and_powers();
    test_iteration_budget_contract();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
