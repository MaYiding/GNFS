// Unit tests for base-m polynomial selection (BaseMSelector)
#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "support/test_assert.hpp"
#include "support/test_check.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using gnfs::test::run_case;
using namespace gnfs::polynomial;
using namespace gnfs::core;

// ─── helpers ───────────────────────────────────────────────

static Integer make_int(long long v) {
    return Integer(static_cast<int64_t>(v));
}

/// Exact irreducibility test over Q for a quadratic a2·x² + a1·x + a0: it is
/// reducible iff its discriminant a1² - 4·a2·a0 is a non-negative perfect
/// square. Independent of the selector's own Eisenstein and mod-p tests.
static bool quadratic_is_irreducible_over_Q(const IntPolynomial& f) {
    GNFS_TEST_CHECK(f.degree() == 2);
    Integer disc = f[1] * f[1];
    Integer four_ac = f[2] * f[0];
    four_ac *= static_cast<int64_t>(4);
    disc -= four_ac;
    if (disc.is_negative())
        return true;
    Integer root = gnfs::core::sqrt(disc);
    root *= root;
    return disc != root;
}

// ─── tests ───────────────────────────────────────────────────

/// 稳定性快照:同一 N 多次 select 必须返回相同 (f, m),验证 Murphy
/// 排名 + tie-breaking 确定性,避免不同 build/run 产生不同多项式。
void test_select_stability() {
    std::cout << "Testing selection stability across runs..." << std::endl;

    // 60-bit N(走 Murphy 排名路径):4 次 select 应完全一致
    Integer n("1000000016000000063");
    auto first = BaseMSelector::select(n, 3);
    TEST_ASSERT(first.success);

    for (int rep = 1; rep < 4; ++rep) {
        auto next = BaseMSelector::select(n, 3);
        TEST_ASSERT(next.success);
        TEST_ASSERT(next.degree == first.degree);
        TEST_ASSERT(next.m == first.m); // 同一 m
        TEST_ASSERT(next.f.degree() == first.f.degree());
        for (uint32_t i = 0; i <= first.f.degree(); ++i) {
            TEST_ASSERT(next.f[i] == first.f[i]); // 同一系数
        }
    }

    // 大 N(走标准 Murphy)
    Integer big_n("100000000000000001500000000000000005049");
    auto big_first = BaseMSelector::select(big_n, 4);
    TEST_ASSERT(big_first.success);
    auto big_second = BaseMSelector::select(big_n, 4);
    TEST_ASSERT(big_second.success);
    TEST_ASSERT(big_first.m == big_second.m);
    for (uint32_t i = 0; i <= big_first.f.degree(); ++i) {
        TEST_ASSERT(big_first.f[i] == big_second.f[i]);
    }

    std::cout << "  PASS (deterministic selection across 4+2 reps)" << std::endl;
}

void test_select_degree3_small() {
    std::cout << "Testing degree-3 selection for small N..." << std::endl;
    // N = 143 = 11 * 13
    Integer n = make_int(143);
    auto result = BaseMSelector::select(n, 3);
    TEST_ASSERT(result.success);
    TEST_ASSERT(result.degree == 3);
    TEST_ASSERT(result.f.degree() == 3);
    std::cout << "  PASS" << std::endl;
}

void test_select_degree3_medium() {
    std::cout << "Testing degree-3 selection for medium N..." << std::endl;
    // N = 100160063 = 10007 * 10009
    Integer n = make_int(100160063LL);
    auto result = BaseMSelector::select(n, 3);
    TEST_ASSERT(result.success);
    TEST_ASSERT(result.degree == 3);
    TEST_ASSERT(result.f.degree() == 3);
    std::cout << "  PASS" << std::endl;
}

void test_fm_equals_n() {
    std::cout << "Testing f(m) = N exactly..." << std::endl;
    // The critical invariant: base-m expansion f(m) must equal N, not just 0 mod N
    for (long long n_val : {143LL, 9991LL, 10403LL, 100160063LL}) {
        Integer n = make_int(n_val);
        auto result = BaseMSelector::select(n, 3);
        TEST_ASSERT(result.success);

        // Evaluate f(m) directly using IntPolynomial
        Integer fm = result.f.evaluate(result.m);
        TEST_ASSERT(fm == n);
    }
    std::cout << "  PASS" << std::endl;
}

void test_correct_degree_produced() {
    std::cout << "Testing correct degree produced..." << std::endl;
    // For N = 143, degree-3 should give exactly degree-3 polynomial
    Integer n = make_int(143);
    auto r3 = BaseMSelector::select(n, 3);
    TEST_ASSERT(r3.success);
    TEST_ASSERT(r3.f.degree() == 3);

    // For larger N, degree-4
    Integer n2("1000000000000000"); // ~50 bits
    auto r4 = BaseMSelector::select(n2, 4);
    TEST_ASSERT(r4.success);
    TEST_ASSERT(r4.f.degree() == 4);
    std::cout << "  PASS" << std::endl;
}

void test_m_approximation() {
    std::cout << "Testing m ≈ N^(1/degree)..." << std::endl;
    Integer n = make_int(100160063LL);
    auto result = BaseMSelector::select(n, 3);
    TEST_ASSERT(result.success);

    // m should be close to cube root of N ≈ 464.7
    // Check m is in range [460, 470]
    int64_t m_val = result.m.to_int64();
    TEST_ASSERT(m_val >= 455 && m_val <= 475);
    std::cout << "  PASS" << std::endl;
}

void test_create_context_verifies() {
    std::cout << "Testing create_context gives verifiable context..." << std::endl;
    Integer n = make_int(9991LL);
    auto result = BaseMSelector::select(n, 3);
    TEST_ASSERT(result.success);

    auto ctx = BaseMSelector::create_context(n, result);
    // The context must satisfy f(m) ≡ 0 mod n
    TEST_ASSERT(ctx.verify());
    TEST_ASSERT(ctx.degree() == 3);
    std::cout << "  PASS" << std::endl;
}

void test_create_context_failed_result_throws() {
    std::cout << "Testing create_context throws on failed result..." << std::endl;
    PolynomialSelectionResult failed_result;
    failed_result.success = false;

    bool threw = false;
    try {
        BaseMSelector::create_context(make_int(100LL), failed_result);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    TEST_ASSERT(threw);
    std::cout << "  PASS" << std::endl;
}

void test_select_poly_api() {
    std::cout << "Testing select_poly() old API..." << std::endl;
    Integer n = make_int(10403LL);
    BaseMSelector selector(n);
    auto ctx = selector.select_poly(3);
    TEST_ASSERT(ctx.degree() == 3);
    TEST_ASSERT(ctx.verify());
    std::cout << "  PASS" << std::endl;
}

void test_convenience_function() {
    std::cout << "Testing select_base_m_polynomial() convenience function..." << std::endl;
    Integer n = make_int(143LL);
    auto ctx = select_base_m_polynomial(n, 3);
    TEST_ASSERT(ctx.degree() == 3);
    TEST_ASSERT(ctx.verify());
    std::cout << "  PASS" << std::endl;
}

void test_leading_coeff_positive() {
    std::cout << "Testing leading coefficient is positive..." << std::endl;
    // base-m expansion always yields non-negative leading coefficient
    // (the remainder from iterated division)
    for (long long n_val : {143LL, 9991LL, 100160063LL}) {
        Integer n = make_int(n_val);
        auto result = BaseMSelector::select(n, 3);
        TEST_ASSERT(result.success);
        Integer lc = result.f[result.f.degree()];
        TEST_ASSERT(!lc.is_negative() && !lc.is_zero());
    }
    std::cout << "  PASS" << std::endl;
}

void test_context_coefficients_match_polynomial() {
    std::cout << "Testing context coefficients match selection result..." << std::endl;
    Integer n = make_int(9991LL);
    auto result = BaseMSelector::select(n, 3);
    auto ctx = BaseMSelector::create_context(n, result);

    for (uint32_t i = 0; i <= result.f.degree(); ++i) {
        TEST_ASSERT(ctx.coeff(i) == result.f[i]);
    }
    std::cout << "  PASS" << std::endl;
}

void test_degree4_selection() {
    std::cout << "Testing degree-4 selection for 50-bit N..." << std::endl;
    // N = 2^50 - 27 (semiprime-like, just testing API)
    Integer n("1125899906842597"); // 2^50 + some offset
    auto result = BaseMSelector::select(n, 4);
    TEST_ASSERT(result.success);
    TEST_ASSERT(result.degree == 4);
    // f(m) = N
    TEST_ASSERT(result.f.evaluate(result.m) == n);
    std::cout << "  PASS" << std::endl;
}

void test_invalid_selection_parameters() {
    std::cout << "Testing invalid base-m selection parameters..." << std::endl;

    const auto zero_n = BaseMSelector::select(Integer(static_cast<int64_t>(0)), 3);
    TEST_ASSERT(!zero_n.success);

    const auto one_n = BaseMSelector::select(Integer(static_cast<int64_t>(1)), 3);
    TEST_ASSERT(!one_n.success);

    const auto zero_degree = BaseMSelector::select(Integer(static_cast<int64_t>(143)), 0);
    TEST_ASSERT(!zero_degree.success);

    std::cout << "  PASS" << std::endl;
}

/// The regular window can hold no irreducible candidate at all. N = 10403 =
/// 101 * 103 with degree 2 gives m_base = 101, and the base-m expansion is
/// reducible for every m in [96, 101]: (x + 5)(x + 7), ..., x(x + 2). The
/// selector used to return the last one, x^2 + 2x, with success = true.
void test_window_without_candidate_widens() {
    std::cout << "Testing selection widens a window without irreducible candidate..." << std::endl;

    Integer n = make_int(10403LL);
    auto result = BaseMSelector::select(n, 2);
    GNFS_TEST_CHECK(result.success);
    GNFS_TEST_CHECK(result.error.empty());
    GNFS_TEST_CHECK(result.degree == 2);

    // The first irreducible expansion outside the window is m = 92 with
    // f = x^2 + 21x + 7 (discriminant 413 = 7 * 59). Every m in [93, 101]
    // stays reducible, and m > 101 lowers the degree.
    GNFS_TEST_CHECK(result.m == make_int(92LL));
    GNFS_TEST_CHECK(result.f.degree() == 2);
    GNFS_TEST_CHECK(result.f[0] == make_int(7LL));
    GNFS_TEST_CHECK(result.f[1] == make_int(21LL));
    GNFS_TEST_CHECK(result.f[2] == make_int(1LL));
    GNFS_TEST_CHECK(result.f.evaluate(result.m) == n);
    GNFS_TEST_CHECK(quadratic_is_irreducible_over_Q(result.f));

    auto ctx = BaseMSelector::create_context(n, result);
    GNFS_TEST_CHECK(ctx.degree() == 2);
    GNFS_TEST_CHECK(ctx.verify());

    std::cout << "  PASS (m = " << ctx.m().to_string() << ", f = x^2 + 21x + 7)" << std::endl;
}

/// No base-m expansion with the requested degree is irreducible for these
/// inputs: N = 4 and N = 16 with degree 2 give x^2, 4x^2 and (x + 1)^2, and
/// N = 8 with degree 3 gives x^3. The regular window already covers every m,
/// so the selection must report failure instead of returning a reducible
/// polynomial.
void test_no_candidate_anywhere_fails() {
    std::cout << "Testing selection fails when every candidate is reducible..." << std::endl;

    struct Case {
        long long n_val;
        uint32_t degree;
    };
    const Case cases[] = {
        {4LL, 2},
        {16LL, 2},
        {8LL, 3},
    };

    for (const auto& c : cases) {
        Integer n = make_int(c.n_val);
        auto result = BaseMSelector::select(n, c.degree);
        GNFS_TEST_CHECK(!result.success);
        GNFS_TEST_CHECK(!result.error.empty());

        bool threw = false;
        try {
            BaseMSelector::create_context(n, result);
        } catch (const std::runtime_error& error) {
            threw = true;
            const std::string message = error.what();
            GNFS_TEST_CHECK(message.find("Cannot create context from failed selection") !=
                            std::string::npos);
            GNFS_TEST_CHECK(message.find(result.error) != std::string::npos);
        }
        GNFS_TEST_CHECK(threw);
    }

    // The old API reports the same failure through its exception.
    BaseMSelector selector(make_int(16LL));
    bool threw = false;
    try {
        (void)selector.select_poly(2);
    } catch (const std::runtime_error& error) {
        threw = true;
        const std::string message = error.what();
        GNFS_TEST_CHECK(message.find("Cannot create context from failed selection") !=
                        std::string::npos);
    }
    GNFS_TEST_CHECK(threw);

    std::cout << "  PASS (N = 4, 16 with degree 2 and N = 8 with degree 3 report failure)"
              << std::endl;
}

/// Inputs whose regular window does hold irreducible candidates must keep
/// their selection. The coefficients are the ones the selector returned
/// before the window widening was added.
void test_window_with_candidate_is_unchanged() {
    std::cout << "Testing selection is unchanged when the window holds a candidate..." << std::endl;

    struct Pinned {
        const char* n_str;
        uint32_t degree;
        const char* m_str;
        std::vector<long long> coeffs; // f[0], f[1], ...
    };
    const Pinned cases[] = {
        // N = 10403 with the degree the default parameters pick for its size.
        {"10403", 3, "16", {3LL, 10LL, 8LL, 2LL}},
        // N = 143 = 11 * 13 with degree 2.
        {"143", 2, "6", {5LL, 5LL, 3LL}},
    };

    for (const auto& c : cases) {
        Integer n(c.n_str);
        auto result = BaseMSelector::select(n, c.degree);
        GNFS_TEST_CHECK(result.success);
        GNFS_TEST_CHECK(result.degree == c.degree);
        GNFS_TEST_CHECK(result.m == Integer(c.m_str));
        GNFS_TEST_CHECK(result.f.degree() == c.degree);
        for (uint32_t i = 0; i <= c.degree; ++i) {
            GNFS_TEST_CHECK(result.f[i] == make_int(c.coeffs[i]));
        }
        GNFS_TEST_CHECK(result.f.evaluate(result.m) == n);
    }

    std::cout << "  PASS (N = 10403 with degree 3 and N = 143 with degree 2 keep m and f)"
              << std::endl;
}

int main() {
    int failed = 0;

    std::cout << "=== BaseMSelector Unit Tests ===" << std::endl;

    failed += !run_case(test_select_degree3_small);
    failed += !run_case(test_select_stability);
    failed += !run_case(test_select_degree3_medium);
    failed += !run_case(test_fm_equals_n);
    failed += !run_case(test_correct_degree_produced);
    failed += !run_case(test_m_approximation);
    failed += !run_case(test_create_context_verifies);
    failed += !run_case(test_create_context_failed_result_throws);
    failed += !run_case(test_select_poly_api);
    failed += !run_case(test_convenience_function);
    failed += !run_case(test_leading_coeff_positive);
    failed += !run_case(test_context_coefficients_match_polynomial);
    failed += !run_case(test_degree4_selection);
    failed += !run_case(test_invalid_selection_parameters);
    failed += !run_case(test_window_without_candidate_widens);
    failed += !run_case(test_no_candidate_anywhere_fails);
    failed += !run_case(test_window_with_candidate_is_unchanged);

    if (failed != 0) {
        std::cout << failed << " test case(s) failed" << std::endl;
        return 1;
    }

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
