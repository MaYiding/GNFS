// Unit tests for PolynomialOptimizer — derivative, translate, rotate, skewness, etc.
#include "gnfs/core/polynomial.hpp"
#include "gnfs/polynomial/polynomial_optimizer.hpp"
#include "gnfs/polynomial/rotation_alpha.hpp"
#include "support/test_assert.hpp"
#include "support/test_check.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using gnfs::test::run_case;
using namespace gnfs::polynomial;
using namespace gnfs::core;

// ─── helpers ───────────────────────────────────────────────

static Integer I(long long v) {
    return Integer(static_cast<int64_t>(v));
}

// Build IntPolynomial from initializer list {c0, c1, ..., cd}
static IntPolynomial make_poly(std::initializer_list<long long> coeffs) {
    std::vector<Integer> c;
    c.reserve(coeffs.size());
    for (long long v : coeffs)
        c.push_back(I(v));
    return IntPolynomial(std::move(c));
}

// ─── derivative ───────────────────────────────────────────────

void test_derivative_constant() {
    std::cout << "Testing derivative of constant..." << std::endl;
    auto f = make_poly({42});
    auto df = PolynomialOptimizer::derivative(f);
    TEST_ASSERT(df.degree() == 0);
    TEST_ASSERT(df[0] == I(0));
    std::cout << "  PASS" << std::endl;
}

void test_derivative_linear() {
    std::cout << "Testing derivative of linear polynomial..." << std::endl;
    // f(x) = 3x + 7 → f'(x) = 3
    auto f = make_poly({7, 3});
    auto df = PolynomialOptimizer::derivative(f);
    TEST_ASSERT(df.degree() == 0);
    TEST_ASSERT(df[0] == I(3));
    std::cout << "  PASS" << std::endl;
}

void test_derivative_quadratic() {
    std::cout << "Testing derivative of quadratic..." << std::endl;
    // f(x) = 5x^2 + 3x + 1 → f'(x) = 10x + 3
    auto f = make_poly({1, 3, 5});
    auto df = PolynomialOptimizer::derivative(f);
    TEST_ASSERT(df.degree() == 1);
    TEST_ASSERT(df[0] == I(3));  // constant term
    TEST_ASSERT(df[1] == I(10)); // x coefficient
    std::cout << "  PASS" << std::endl;
}

void test_derivative_cubic() {
    std::cout << "Testing derivative of cubic..." << std::endl;
    // f(x) = x^3 + x + 1 → f'(x) = 3x^2 + 1
    auto f = make_poly({1, 1, 0, 1});
    auto df = PolynomialOptimizer::derivative(f);
    TEST_ASSERT(df.degree() == 2);
    TEST_ASSERT(df[0] == I(1)); // constant
    TEST_ASSERT(df[1] == I(0)); // x
    TEST_ASSERT(df[2] == I(3)); // x^2
    std::cout << "  PASS" << std::endl;
}

// ─── translate ───────────────────────────────────────────────

void test_translate_by_zero() {
    std::cout << "Testing translate by 0 is identity..." << std::endl;
    auto f = make_poly({1, 2, 3}); // 3x^2 + 2x + 1
    auto g = PolynomialOptimizer::translate(f, 0);
    TEST_ASSERT(g.degree() == f.degree());
    for (uint32_t i = 0; i <= f.degree(); ++i) {
        TEST_ASSERT(g[i] == f[i]);
    }
    std::cout << "  PASS" << std::endl;
}

void test_translate_linear_by_1() {
    std::cout << "Testing translate x by 1: f(x+1)..." << std::endl;
    // f(x) = x + 3 → f(x+1) = (x+1) + 3 = x + 4
    auto f = make_poly({3, 1});
    auto g = PolynomialOptimizer::translate(f, 1);
    TEST_ASSERT(g.degree() == 1);
    TEST_ASSERT(g[0] == I(4));
    TEST_ASSERT(g[1] == I(1));
    std::cout << "  PASS" << std::endl;
}

void test_translate_quadratic_by_1() {
    std::cout << "Testing translate x^2 by 1: f(x+1) = x^2+2x+1..." << std::endl;
    // f(x) = x^2 → f(x+1) = x^2 + 2x + 1
    auto f = make_poly({0, 0, 1});
    auto g = PolynomialOptimizer::translate(f, 1);
    TEST_ASSERT(g.degree() == 2);
    TEST_ASSERT(g[0] == I(1)); // constant
    TEST_ASSERT(g[1] == I(2)); // x
    TEST_ASSERT(g[2] == I(1)); // x^2
    std::cout << "  PASS" << std::endl;
}

void test_translate_evaluate_consistency() {
    std::cout << "Testing translate: g(x) = f(x+t) so g(0) = f(t)..." << std::endl;
    // f(x) = x^3 + 2x + 5; t = 3
    // g(x) = f(x+3); g(0) should equal f(3) = 27 + 6 + 5 = 38
    auto f = make_poly({5, 2, 0, 1});
    auto g = PolynomialOptimizer::translate(f, 3);
    Integer g0 = g.evaluate(I(0));
    Integer f3 = f.evaluate(I(3));
    TEST_ASSERT(g0 == f3);
    TEST_ASSERT(g0 == I(38));
    std::cout << "  PASS" << std::endl;
}

// ─── rotate ───────────────────────────────────────────────────

void test_rotate_by_zero() {
    std::cout << "Testing rotate by k=0 is identity..." << std::endl;
    auto f = make_poly({1, 2, 3});
    auto h = make_poly({-5, 1}); // h(x) = x - 5
    auto g = PolynomialOptimizer::rotate(f, h, 0);
    for (uint32_t i = 0; i <= 2; ++i)
        TEST_ASSERT(g[i] == f[i]);
    std::cout << "  PASS" << std::endl;
}

void test_rotate_linear_preserves_root() {
    std::cout << "Testing rotate_linear preserves f(m) mod n..." << std::endl;
    // f(x) = x^3 + x + 1, m = 5, f(5) = 131
    auto f = make_poly({1, 1, 0, 1});
    Integer m(5LL);
    // After rotation by k: g(x) = f(x) + k*(x - m)
    // g(m) = f(m) + k*(m - m) = f(m) — so the value at m is unchanged
    for (int k : {1, -1, 5, -10, 100}) {
        auto g = PolynomialOptimizer::rotate_linear(f, m, static_cast<int64_t>(k));
        Integer gm = g.evaluate(m);
        Integer fm = f.evaluate(m);
        TEST_ASSERT(gm == fm);
    }
    std::cout << "  PASS" << std::endl;
}

void test_rotate_changes_coefficients() {
    std::cout << "Testing rotate_linear changes coefficients..." << std::endl;
    // f(x) = x^3 + x + 1, rotate by k=2, m=5
    // g(x) = f(x) + 2*(x - 5) = x^3 + 3x - 9
    auto f = make_poly({1, 1, 0, 1});
    auto g = PolynomialOptimizer::rotate_linear(f, I(5), 2);
    TEST_ASSERT(g[0] == I(1 - 10)); // 1 + 2*(-5) = -9
    TEST_ASSERT(g[1] == I(1 + 2));  // 1 + 2 = 3
    TEST_ASSERT(g[2] == I(0));
    TEST_ASSERT(g[3] == I(1));
    std::cout << "  PASS" << std::endl;
}

void test_rotate_preserves_full_int64_coefficient() {
    std::cout << "Testing rotate with full int64 coefficients..." << std::endl;
    auto f = make_poly({7, -3, 2});
    auto h = make_poly({-11, 5, 1});

    for (const int64_t k :
         {std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()}) {
        auto g = PolynomialOptimizer::rotate(f, h, k);
        const Integer multiplier(k);
        for (uint32_t i = 0; i <= 2; ++i) {
            Integer expected = f[i] + h[i] * multiplier;
            TEST_ASSERT(g[i] == expected);
        }
    }
    std::cout << "  PASS" << std::endl;
}

void test_rotate_linear_preserves_full_int64_coefficient() {
    std::cout << "Testing rotate_linear with full int64 coefficients..." << std::endl;
    auto f = make_poly({7, -3, 2});
    const Integer m(13);

    for (const int64_t k :
         {std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()}) {
        auto g = PolynomialOptimizer::rotate_linear(f, m, k);
        const Integer multiplier(k);
        TEST_ASSERT(g[0] == f[0] - m * multiplier);
        TEST_ASSERT(g[1] == f[1] + multiplier);
        TEST_ASSERT(g[2] == f[2]);
    }
    std::cout << "  PASS" << std::endl;
}

// ─── skewness & size ───────────────────────────────────────────

void test_estimate_skewness_basic() {
    std::cout << "Testing estimate_skewness..." << std::endl;
    // f(x) = x^3 + 1000; c0 = 1000, cd = 1, s = (1000/1)^(1/3) ≈ 10
    auto f = make_poly({1000, 0, 0, 1});
    double s = PolynomialOptimizer::estimate_skewness(f);
    TEST_ASSERT(s > 8.0 && s < 12.0);
    std::cout << "  PASS" << std::endl;
}

void test_estimate_skewness_monic_balanced() {
    std::cout << "Testing estimate_skewness for balanced poly..." << std::endl;
    // f(x) = x^3 + x + 1; c0 = c3 = 1, s ≈ 1.0
    auto f = make_poly({1, 1, 0, 1});
    double s = PolynomialOptimizer::estimate_skewness(f);
    TEST_ASSERT(s > 0.5 && s < 2.0);
    std::cout << "  PASS" << std::endl;
}

void test_compute_size_at_skew_1() {
    std::cout << "Testing compute_size at skewness=1 is sum of |coeffs|..." << std::endl;
    // For skewness=1, each term contributes |c_i| * 1^(i - d/2) = |c_i|
    // f(x) = 3x + 7 → size = 7 + 3 = 10
    auto f = make_poly({7, 3});
    double sz = PolynomialOptimizer::compute_size(f, 1.0);
    // d=1, half_d=0.5; terms: 7*s^{0-0.5} + 3*s^{1-0.5} = 7/sqrt(1) + 3*sqrt(1) = 10
    TEST_ASSERT(std::abs(sz - 10.0) < 1e-9);
    std::cout << "  PASS" << std::endl;
}

// ─── golden section ───────────────────────────────────────────

void test_golden_section_finds_minimum() {
    std::cout << "Testing golden_section finds minimum of (x-3)^2..." << std::endl;
    auto scorer = [](double x) { return (x - 3.0) * (x - 3.0); };
    double opt = PolynomialOptimizer::golden_section_skewness(0.0, 10.0, scorer);
    TEST_ASSERT(std::abs(opt - 3.0) < 1e-3);
    std::cout << "  PASS" << std::endl;
}

void test_golden_section_monotone() {
    std::cout << "Testing golden_section on monotone function (min at boundary)..." << std::endl;
    // Monotone decreasing: min at right boundary (b = 5.0)
    auto scorer = [](double x) { return -x; }; // minimum at right end
    double opt = PolynomialOptimizer::golden_section_skewness(1.0, 5.0, scorer);
    TEST_ASSERT(opt > 4.0); // Should be near 5.0
    std::cout << "  PASS" << std::endl;
}

// ─── generate_smooth_numbers ────────────────────────────────────

void test_generate_smooth_1_always_included() {
    std::cout << "Testing generate_smooth_numbers includes 1..." << std::endl;
    std::vector<uint32_t> primes = {2, 3, 5};
    auto smooth = PolynomialOptimizer::generate_smooth_numbers(100, primes, 1000);
    TEST_ASSERT(!smooth.empty());
    TEST_ASSERT(smooth[0] == I(1)); // 1 is always first
    std::cout << "  PASS" << std::endl;
}

void test_generate_smooth_powers_of_2() {
    std::cout << "Testing generate_smooth_numbers with only prime 2..." << std::endl;
    std::vector<uint32_t> primes = {2};
    auto smooth = PolynomialOptimizer::generate_smooth_numbers(32, primes, 100);
    // Should contain 1, 2, 4, 8, 16, 32
    TEST_ASSERT(smooth.size() == 6);
    TEST_ASSERT(smooth[0] == I(1));
    TEST_ASSERT(smooth[1] == I(2));
    TEST_ASSERT(smooth[2] == I(4));
    TEST_ASSERT(smooth[3] == I(8));
    TEST_ASSERT(smooth[4] == I(16));
    TEST_ASSERT(smooth[5] == I(32));
    std::cout << "  PASS" << std::endl;
}

void test_generate_smooth_sorted() {
    std::cout << "Testing generate_smooth_numbers is sorted..." << std::endl;
    std::vector<uint32_t> primes = {2, 3};
    auto smooth = PolynomialOptimizer::generate_smooth_numbers(100, primes, 1000);
    for (size_t i = 1; i < smooth.size(); ++i) {
        TEST_ASSERT(smooth[i - 1] < smooth[i]);
    }
    std::cout << "  PASS" << std::endl;
}

void test_generate_smooth_max_count_respected() {
    std::cout << "Testing generate_smooth_numbers respects max_count..." << std::endl;
    std::vector<uint32_t> primes = {2, 3, 5, 7, 11, 13};
    auto smooth = PolynomialOptimizer::generate_smooth_numbers(1000000, primes, 50);
    TEST_ASSERT(smooth.size() <= 50);
    std::cout << "  PASS" << std::endl;
}

void test_generate_smooth_rejects_invalid_inputs() {
    std::cout << "Testing generate_smooth_numbers handles invalid inputs..." << std::endl;

    // Invalid factors must be ignored rather than making val *= p loop forever.
    const std::vector<uint32_t> primes = {0, 1, 2};
    const auto smooth = PolynomialOptimizer::generate_smooth_numbers(8, primes, 100);
    GNFS_TEST_CHECK(smooth.size() == 4);
    GNFS_TEST_CHECK(smooth[0] == I(1));
    GNFS_TEST_CHECK(smooth[1] == I(2));
    GNFS_TEST_CHECK(smooth[2] == I(4));
    GNFS_TEST_CHECK(smooth[3] == I(8));

    // max_count is an upper bound, including the implicit value 1.
    GNFS_TEST_CHECK(PolynomialOptimizer::generate_smooth_numbers(8, primes, 0).empty());
    GNFS_TEST_CHECK(PolynomialOptimizer::generate_smooth_numbers(0, primes, 100).empty());
    GNFS_TEST_CHECK(PolynomialOptimizer::generate_smooth_numbers(1, primes, 100).empty());

    std::cout << "  PASS" << std::endl;
}

// ─── newton_root ─────────────────────────────────────────────────

void test_newton_root_known_root() {
    std::cout << "Testing newton_root with known root..." << std::endl;
    // f(x) = x^3 - 8 = 0 has root x=2 over Z
    // But we want f(m) ≡ 0 mod n.
    // Use f(x) = x^2 - 4, root = 2.  f(2) = 0, n = any.
    // Actually newton_root checks f(m) ≡ 0 mod n, so:
    // f(x) = x^2 + 3x - 10, f(2) = 4+6-10 = 0. n = 100, initial = 2.
    auto f = make_poly({-10, 3, 1}); // x^2 + 3x - 10
    Integer n(100LL);
    auto result = PolynomialOptimizer::newton_root(f, I(2), n);
    TEST_ASSERT(result.has_value());
    // Verify f(result) ≡ 0 mod n
    Integer fm = f.evaluate(*result);
    Integer q, r;
    Integer::divmod(q, r, fm, n);
    TEST_ASSERT(r.is_zero());
    std::cout << "  PASS" << std::endl;
}

void test_newton_root_already_at_root() {
    std::cout << "Testing newton_root when initial already satisfies f(m)≡0 mod n..." << std::endl;
    // f(x) = x^3 + x + 1 with m=5, n=131; f(5)=131 ≡ 0 mod 131
    auto f = make_poly({1, 1, 0, 1});
    Integer n(131LL);
    auto result = PolynomialOptimizer::newton_root(f, I(5), n);
    TEST_ASSERT(result.has_value());
    TEST_ASSERT(*result == I(5));
    std::cout << "  PASS" << std::endl;
}

void test_newton_root_zero_tolerance_is_bounded() {
    std::cout << "Testing newton_root with zero tolerance..." << std::endl;
    // tolerance=0 previously produced +inf from -log2(0), followed by an
    // out-of-range floating-to-size_t conversion in the convergence check.
    auto f = make_poly({-11, 1});
    auto result = PolynomialOptimizer::newton_root(f, I(15), Integer(143), 8, 0.0);
    TEST_ASSERT(result.has_value());
    TEST_ASSERT(*result == I(11));
    std::cout << "  PASS" << std::endl;
}

void test_rotation_alpha_bound_rejects_wrap() {
    std::cout << "Testing RotationAlphaTracker rejects wrapping bound..." << std::endl;
    bool threw = false;
    try {
        RotationAlphaTracker tracker((std::numeric_limits<uint32_t>::max)());
        (void)tracker;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TEST_ASSERT(threw);
    std::cout << "  PASS" << std::endl;
}

int main() {
    int failed = 0;

    std::cout << "=== PolynomialOptimizer Unit Tests ===" << std::endl;

    failed += !run_case(test_derivative_constant);
    failed += !run_case(test_derivative_linear);
    failed += !run_case(test_derivative_quadratic);
    failed += !run_case(test_derivative_cubic);
    failed += !run_case(test_translate_by_zero);
    failed += !run_case(test_translate_linear_by_1);
    failed += !run_case(test_translate_quadratic_by_1);
    failed += !run_case(test_translate_evaluate_consistency);
    failed += !run_case(test_rotate_by_zero);
    failed += !run_case(test_rotate_linear_preserves_root);
    failed += !run_case(test_rotate_changes_coefficients);
    failed += !run_case(test_rotate_preserves_full_int64_coefficient);
    failed += !run_case(test_rotate_linear_preserves_full_int64_coefficient);
    failed += !run_case(test_estimate_skewness_basic);
    failed += !run_case(test_estimate_skewness_monic_balanced);
    failed += !run_case(test_compute_size_at_skew_1);
    failed += !run_case(test_golden_section_finds_minimum);
    failed += !run_case(test_golden_section_monotone);
    failed += !run_case(test_generate_smooth_1_always_included);
    failed += !run_case(test_generate_smooth_powers_of_2);
    failed += !run_case(test_generate_smooth_sorted);
    failed += !run_case(test_generate_smooth_max_count_respected);
    failed += !run_case(test_generate_smooth_rejects_invalid_inputs);
    failed += !run_case(test_newton_root_known_root);
    failed += !run_case(test_newton_root_already_at_root);
    failed += !run_case(test_newton_root_zero_tolerance_is_bounded);
    failed += !run_case(test_rotation_alpha_bound_rejects_wrap);

    if (failed != 0) {
        std::cout << failed << " test case(s) failed" << std::endl;
        return 1;
    }

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
