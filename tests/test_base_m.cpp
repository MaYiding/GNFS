// Unit tests for base-m polynomial selection (BaseMSelector)
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/core/polynomial_context.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace gnfs::polynomial;
using namespace gnfs::core;

// ─── helpers ───────────────────────────────────────────────

static Integer make_int(long long v) { return Integer(static_cast<int64_t>(v)); }

// ─── tests ───────────────────────────────────────────────────

void test_select_degree3_small() {
    std::cout << "Testing degree-3 selection for small N..." << std::endl;
    // N = 143 = 11 * 13
    Integer n = make_int(143);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);
    assert(result.degree == 3);
    assert(result.f.degree() == 3);
    std::cout << "  PASS" << std::endl;
}

void test_select_degree3_medium() {
    std::cout << "Testing degree-3 selection for medium N..." << std::endl;
    // N = 100160063 = 10007 * 10009
    Integer n = make_int(100160063LL);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);
    assert(result.degree == 3);
    assert(result.f.degree() == 3);
    std::cout << "  PASS" << std::endl;
}

void test_fm_equals_n() {
    std::cout << "Testing f(m) = N exactly..." << std::endl;
    // The critical invariant: base-m expansion f(m) must equal N, not just 0 mod N
    for (long long n_val : {143LL, 9991LL, 10403LL, 100160063LL}) {
        Integer n = make_int(n_val);
        auto result = BaseMSelector::select(n, 3);
        assert(result.success);

        // Evaluate f(m) directly using IntPolynomial
        Integer fm = result.f.evaluate(result.m);
        assert(fm == n);
    }
    std::cout << "  PASS" << std::endl;
}

void test_correct_degree_produced() {
    std::cout << "Testing correct degree produced..." << std::endl;
    // For N = 143, degree-3 should give exactly degree-3 polynomial
    Integer n = make_int(143);
    auto r3 = BaseMSelector::select(n, 3);
    assert(r3.success);
    assert(r3.f.degree() == 3);

    // For larger N, degree-4
    Integer n2("1000000000000000"); // ~50 bits
    auto r4 = BaseMSelector::select(n2, 4);
    assert(r4.success);
    assert(r4.f.degree() == 4);
    std::cout << "  PASS" << std::endl;
}

void test_m_approximation() {
    std::cout << "Testing m ≈ N^(1/degree)..." << std::endl;
    Integer n = make_int(100160063LL);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    // m should be close to cube root of N ≈ 464.7
    // Check m is in range [460, 470]
    int64_t m_val = result.m.to_int64();
    assert(m_val >= 455 && m_val <= 475);
    std::cout << "  PASS" << std::endl;
}

void test_create_context_verifies() {
    std::cout << "Testing create_context gives verifiable context..." << std::endl;
    Integer n = make_int(9991LL);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);
    // The context must satisfy f(m) ≡ 0 mod n
    assert(ctx.verify());
    assert(ctx.degree() == 3);
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
    assert(threw);
    std::cout << "  PASS" << std::endl;
}

void test_select_poly_api() {
    std::cout << "Testing select_poly() old API..." << std::endl;
    Integer n = make_int(10403LL);
    BaseMSelector selector(n);
    auto ctx = selector.select_poly(3);
    assert(ctx.degree() == 3);
    assert(ctx.verify());
    std::cout << "  PASS" << std::endl;
}

void test_convenience_function() {
    std::cout << "Testing select_base_m_polynomial() convenience function..." << std::endl;
    Integer n = make_int(143LL);
    auto ctx = select_base_m_polynomial(n, 3);
    assert(ctx.degree() == 3);
    assert(ctx.verify());
    std::cout << "  PASS" << std::endl;
}

void test_leading_coeff_positive() {
    std::cout << "Testing leading coefficient is positive..." << std::endl;
    // base-m expansion always yields non-negative leading coefficient
    // (the remainder from iterated division)
    for (long long n_val : {143LL, 9991LL, 100160063LL}) {
        Integer n = make_int(n_val);
        auto result = BaseMSelector::select(n, 3);
        assert(result.success);
        Integer lc = result.f[result.f.degree()];
        assert(!lc.is_negative() && !lc.is_zero());
    }
    std::cout << "  PASS" << std::endl;
}

void test_context_coefficients_match_polynomial() {
    std::cout << "Testing context coefficients match selection result..." << std::endl;
    Integer n = make_int(9991LL);
    auto result = BaseMSelector::select(n, 3);
    auto ctx = BaseMSelector::create_context(n, result);

    for (uint32_t i = 0; i <= result.f.degree(); ++i) {
        assert(ctx.coeff(i) == result.f[i]);
    }
    std::cout << "  PASS" << std::endl;
}

void test_degree4_selection() {
    std::cout << "Testing degree-4 selection for 50-bit N..." << std::endl;
    // N = 2^50 - 27 (semiprime-like, just testing API)
    Integer n("1125899906842597"); // 2^50 + some offset
    auto result = BaseMSelector::select(n, 4);
    assert(result.success);
    assert(result.degree == 4);
    // f(m) = N
    assert(result.f.evaluate(result.m) == n);
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== BaseMSelector Unit Tests ===" << std::endl;

    test_select_degree3_small();
    test_select_degree3_medium();
    test_fm_equals_n();
    test_correct_degree_produced();
    test_m_approximation();
    test_create_context_verifies();
    test_create_context_failed_result_throws();
    test_select_poly_api();
    test_convenience_function();
    test_leading_coeff_positive();
    test_context_coefficients_match_polynomial();
    test_degree4_selection();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
