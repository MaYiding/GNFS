// test_squfof.cpp — SQUFOF factorization correctness tests

#include <gnfs/cofactor/squfof.hpp>
#include <iostream>
#include <vector>

using gnfs::cofactor::SQUFOF;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    tests_passed++; \
} while(0)

void test_small_semiprimes() {
    // p × q pairs
    struct Case { uint64_t n; uint64_t p; uint64_t q; };
    std::vector<Case> cases = {
        {15, 3, 5},
        {77, 7, 11},
        {143, 11, 13},
        {323, 17, 19},
        {9991, 97, 103},
        {96091, 307, 313},
        {100160063, 10007, 10009},
    };

    for (const auto& tc : cases) {
        uint64_t f = SQUFOF::factor(tc.n);
        TEST_ASSERT(f != 1 && f != tc.n, "should find non-trivial factor");
        TEST_ASSERT(tc.n % f == 0, "factor should divide n");
        uint64_t other = tc.n / f;
        // One of (f, other) should be (p, q) or (q, p)
        bool match = (f == tc.p && other == tc.q) || (f == tc.q && other == tc.p);
        if (!match) {
            // Factor found but different factorization (shouldn't happen for semiprimes)
            std::cerr << "  Note: " << tc.n << " = " << f << " × " << other
                      << " (expected " << tc.p << " × " << tc.q << ")\n";
        }
    }
    TEST_PASS("small semiprimes (7 cases)");
}

void test_medium_semiprimes() {
    // 30-50 bit semiprimes (typical cofactor range)
    struct Case { uint64_t n; };
    std::vector<Case> cases = {
        {UINT64_C(1000003) * 1000033},      // ~40 bit
        {UINT64_C(10000019) * 10000079},     // ~47 bit
        {UINT64_C(1000000007) * 1000000009}, // ~60 bit
    };

    for (const auto& tc : cases) {
        uint64_t f = SQUFOF::factor(tc.n);
        TEST_ASSERT(f != 1 && f != tc.n, "should find non-trivial factor");
        TEST_ASSERT(tc.n % f == 0, "factor should divide n");
        uint64_t other = tc.n / f;
        // Both factors should be prime-ish
        TEST_ASSERT(f > 1 && other > 1, "both factors should be > 1");
    }
    TEST_PASS("medium semiprimes (40-54 bit)");
}

void test_edge_cases() {
    // Primes should return 1 (failure)
    TEST_ASSERT(SQUFOF::factor(7) == 1 || SQUFOF::factor(7) == 7, "prime should fail or return itself");
    TEST_ASSERT(SQUFOF::factor(2) == 2, "factor(2) = 2");
    TEST_ASSERT(SQUFOF::factor(4) != 1, "4 = 2×2 should factor");

    // Perfect square
    uint64_t f = SQUFOF::factor(49);
    TEST_ASSERT(f == 7, "49 should yield 7");

    TEST_PASS("edge cases (primes, small, perfect square)");
}

void test_prime_powers() {
    // p^k should factor to p
    uint64_t f = SQUFOF::factor(8);  // 2^3
    TEST_ASSERT(f == 2, "8 = 2^3 should yield 2");

    f = SQUFOF::factor(27);  // 3^3
    TEST_ASSERT(f == 3 || f == 9, "27 = 3^3 should yield 3 or 9");

    f = SQUFOF::factor(121);  // 11^2
    TEST_ASSERT(f == 11, "121 = 11^2 should yield 11");

    TEST_PASS("prime powers");
}

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  SQUFOF Unit Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_edge_cases();
    test_small_semiprimes();
    test_medium_semiprimes();
    test_prime_powers();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
