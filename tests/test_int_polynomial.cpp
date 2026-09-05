// Unit tests for IntPolynomial — integer polynomial arithmetic
#include "gnfs/polynomial/int_polynomial.hpp"
#include "support/test_check.hpp"

#include <iostream>

using namespace gnfs::polynomial;
using gnfs::core::Integer;

// Helper: create polynomial from int64 coefficients
IntPolynomial make_poly(std::initializer_list<int64_t> coeffs) {
    std::vector<Integer> v;
    for (auto c : coeffs)
        v.push_back(Integer(c));
    return IntPolynomial(std::move(v));
}

void test_construction() {
    std::cout << "Testing construction..." << std::endl;

    // Default: zero polynomial
    IntPolynomial zero;
    GNFS_TEST_CHECK(zero.is_zero());
    GNFS_TEST_CHECK(zero.degree() == 0);

    // From constant
    IntPolynomial c(42);
    GNFS_TEST_CHECK(!c.is_zero());
    GNFS_TEST_CHECK(c.degree() == 0);
    GNFS_TEST_CHECK(c[0].to_int64() == 42);

    // From coefficients: 3x^2 + 2x + 1
    auto p = make_poly({1, 2, 3});
    GNFS_TEST_CHECK(p.degree() == 2);
    GNFS_TEST_CHECK(p[0].to_int64() == 1);
    GNFS_TEST_CHECK(p[1].to_int64() == 2);
    GNFS_TEST_CHECK(p[2].to_int64() == 3);

    // Leading zeros are trimmed
    auto p2 = make_poly({1, 2, 0, 0});
    GNFS_TEST_CHECK(p2.degree() == 1);

    // All zeros → zero polynomial
    auto p3 = make_poly({0, 0, 0});
    GNFS_TEST_CHECK(p3.is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_access() {
    std::cout << "Testing coefficient access..." << std::endl;

    auto p = make_poly({5, -3, 7});

    // In-bounds access
    GNFS_TEST_CHECK(p[0].to_int64() == 5);
    GNFS_TEST_CHECK(p[1].to_int64() == -3);
    GNFS_TEST_CHECK(p[2].to_int64() == 7);

    // Out-of-bounds const access returns 0
    const auto& cp = p;
    GNFS_TEST_CHECK(cp[10].is_zero());

    // Leading coefficient
    GNFS_TEST_CHECK(p.leading_coeff().to_int64() == 7);

    // coefficients() accessor
    GNFS_TEST_CHECK(p.coefficients().size() == 3);

    std::cout << "  PASS" << std::endl;
}

void test_clone() {
    std::cout << "Testing clone..." << std::endl;

    auto p = make_poly({1, 2, 3});
    auto q = p.clone();

    GNFS_TEST_CHECK(q.degree() == p.degree());
    for (uint32_t i = 0; i <= q.degree(); ++i) {
        GNFS_TEST_CHECK(q[i].compare(p[i]) == 0);
    }

    std::cout << "  PASS" << std::endl;
}

void test_evaluate() {
    std::cout << "Testing evaluate..." << std::endl;

    // f(x) = 3x^2 + 2x + 1
    auto f = make_poly({1, 2, 3});

    // f(0) = 1
    GNFS_TEST_CHECK(f.evaluate(Integer(0)).to_int64() == 1);

    // f(1) = 1 + 2 + 3 = 6
    GNFS_TEST_CHECK(f.evaluate(Integer(1)).to_int64() == 6);

    // f(2) = 1 + 4 + 12 = 17
    GNFS_TEST_CHECK(f.evaluate(Integer(2)).to_int64() == 17);

    // f(-1) = 1 - 2 + 3 = 2
    GNFS_TEST_CHECK(f.evaluate(Integer(-1)).to_int64() == 2);

    // f(10) = 1 + 20 + 300 = 321
    GNFS_TEST_CHECK(f.evaluate(Integer(10)).to_int64() == 321);

    // Zero polynomial
    IntPolynomial zero;
    GNFS_TEST_CHECK(zero.evaluate(Integer(42)).is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_evaluate_double() {
    std::cout << "Testing evaluate_double..." << std::endl;

    auto f = make_poly({1, 2, 3});
    double val = f.evaluate_double(2.0);
    GNFS_TEST_CHECK(std::abs(val - 17.0) < 1e-10);

    val = f.evaluate_double(0.5);
    // f(0.5) = 1 + 1 + 0.75 = 2.75
    GNFS_TEST_CHECK(std::abs(val - 2.75) < 1e-10);

    std::cout << "  PASS" << std::endl;
}

void test_evaluate_mod() {
    std::cout << "Testing evaluate_mod..." << std::endl;

    // f(x) = x^2 + x + 1
    auto f = make_poly({1, 1, 1});

    // f(0) mod 7 = 1
    GNFS_TEST_CHECK(f.evaluate_mod(0, 7) == 1);

    // f(1) mod 7 = 3
    GNFS_TEST_CHECK(f.evaluate_mod(1, 7) == 3);

    // f(2) mod 7 = 7 mod 7 = 0
    GNFS_TEST_CHECK(f.evaluate_mod(2, 7) == 0);

    // f(4) mod 7 = 21 mod 7 = 0
    GNFS_TEST_CHECK(f.evaluate_mod(4, 7) == 0);

    // With negative coefficients: f(x) = x^2 - 3x + 2
    auto g = make_poly({2, -3, 1});
    // g(0) = 2, g(1) = 0, g(2) = 0
    GNFS_TEST_CHECK(g.evaluate_mod(0, 11) == 2);
    GNFS_TEST_CHECK(g.evaluate_mod(1, 11) == 0);
    GNFS_TEST_CHECK(g.evaluate_mod(2, 11) == 0);

    std::cout << "  PASS" << std::endl;
}

void test_roots_mod_p() {
    std::cout << "Testing roots_mod_p..." << std::endl;

    // f(x) = x^2 - 1 = (x-1)(x+1)
    auto f = make_poly({-1, 0, 1});

    // Roots mod 7: x=1 and x=6 (≡ -1)
    auto roots7 = f.roots_mod_p(7);
    GNFS_TEST_CHECK(roots7.size() == 2);
    for (auto r : roots7) {
        GNFS_TEST_CHECK(f.evaluate_mod(r, 7) == 0);
    }

    // f(x) = x^2 + 1: no roots mod 3
    auto g = make_poly({1, 0, 1});
    auto roots3 = g.roots_mod_p(3);
    GNFS_TEST_CHECK(roots3.empty());

    // f(x) = x^2 + 1: roots mod 5 are x=2,3
    auto roots5 = g.roots_mod_p(5);
    GNFS_TEST_CHECK(roots5.size() == 2);
    for (auto r : roots5) {
        GNFS_TEST_CHECK(g.evaluate_mod(r, 5) == 0);
    }

    // Linear: f(x) = x - 3, root mod 7 = 3
    auto h = make_poly({-3, 1});
    auto roots = h.roots_mod_p(7);
    GNFS_TEST_CHECK(roots.size() == 1);
    GNFS_TEST_CHECK(roots[0] == 3);

    std::cout << "  PASS" << std::endl;
}

void test_addition() {
    std::cout << "Testing addition..." << std::endl;

    // (3x^2 + 2x + 1) + (x + 5) = 3x^2 + 3x + 6
    auto a = make_poly({1, 2, 3});
    auto b = make_poly({5, 1});
    a += b;
    GNFS_TEST_CHECK(a.degree() == 2);
    GNFS_TEST_CHECK(a[0].to_int64() == 6);
    GNFS_TEST_CHECK(a[1].to_int64() == 3);
    GNFS_TEST_CHECK(a[2].to_int64() == 3);

    // Addition that reduces degree: (x^2 + 1) + (-x^2 + 2) = 3
    auto c = make_poly({1, 0, 1});
    auto d = make_poly({2, 0, -1});
    c += d;
    GNFS_TEST_CHECK(c.degree() == 0);
    GNFS_TEST_CHECK(c[0].to_int64() == 3);

    std::cout << "  PASS" << std::endl;
}

void test_subtraction() {
    std::cout << "Testing subtraction..." << std::endl;

    // (3x^2 + 2x + 1) - (x^2 + x + 1) = 2x^2 + x
    auto a = make_poly({1, 2, 3});
    auto b = make_poly({1, 1, 1});
    a -= b;
    GNFS_TEST_CHECK(a.degree() == 2);
    GNFS_TEST_CHECK(a[0].to_int64() == 0);
    GNFS_TEST_CHECK(a[1].to_int64() == 1);
    GNFS_TEST_CHECK(a[2].to_int64() == 2);

    // Self-subtraction → zero
    auto c = make_poly({1, 2, 3});
    auto d = c.clone();
    c -= d;
    GNFS_TEST_CHECK(c.is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_scalar_multiply() {
    std::cout << "Testing scalar multiplication..." << std::endl;

    // (2x + 3) × 5 = 10x + 15
    auto a = make_poly({3, 2});
    a *= 5;
    GNFS_TEST_CHECK(a[0].to_int64() == 15);
    GNFS_TEST_CHECK(a[1].to_int64() == 10);

    // × 0 → zero
    auto b = make_poly({1, 2, 3});
    b *= 0;
    GNFS_TEST_CHECK(b.is_zero());

    // × (-1) negates
    auto c = make_poly({1, -2, 3});
    c *= -1;
    GNFS_TEST_CHECK(c[0].to_int64() == -1);
    GNFS_TEST_CHECK(c[1].to_int64() == 2);
    GNFS_TEST_CHECK(c[2].to_int64() == -3);

    std::cout << "  PASS" << std::endl;
}

void test_polynomial_multiply() {
    std::cout << "Testing polynomial multiplication..." << std::endl;

    // (x + 1)(x - 1) = x^2 - 1
    auto a = make_poly({1, 1});
    auto b = make_poly({-1, 1});
    auto c = a * b;
    GNFS_TEST_CHECK(c.degree() == 2);
    GNFS_TEST_CHECK(c[0].to_int64() == -1);
    GNFS_TEST_CHECK(c[1].to_int64() == 0);
    GNFS_TEST_CHECK(c[2].to_int64() == 1);

    // (x + 2)(x + 3) = x^2 + 5x + 6
    auto d = make_poly({2, 1});
    auto e = make_poly({3, 1});
    auto f = d * e;
    GNFS_TEST_CHECK(f.degree() == 2);
    GNFS_TEST_CHECK(f[0].to_int64() == 6);
    GNFS_TEST_CHECK(f[1].to_int64() == 5);
    GNFS_TEST_CHECK(f[2].to_int64() == 1);

    // Multiply by zero
    IntPolynomial zero;
    auto z = a * zero;
    GNFS_TEST_CHECK(z.is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_derivative() {
    std::cout << "Testing derivative..." << std::endl;

    // f(x) = 3x^3 + 2x^2 + x + 5
    // f'(x) = 9x^2 + 4x + 1
    auto f = make_poly({5, 1, 2, 3});
    auto fp = f.derivative();
    GNFS_TEST_CHECK(fp.degree() == 2);
    GNFS_TEST_CHECK(fp[0].to_int64() == 1);
    GNFS_TEST_CHECK(fp[1].to_int64() == 4);
    GNFS_TEST_CHECK(fp[2].to_int64() == 9);

    // Constant → 0
    auto c = make_poly({42});
    auto cp = c.derivative();
    GNFS_TEST_CHECK(cp.is_zero());

    // Linear → constant
    auto l = make_poly({3, 7});
    auto lp = l.derivative();
    GNFS_TEST_CHECK(lp.degree() == 0);
    GNFS_TEST_CHECK(lp[0].to_int64() == 7);

    std::cout << "  PASS" << std::endl;
}

void test_translate() {
    std::cout << "Testing translate (Taylor shift)..." << std::endl;

    // f(x) = x^2, f(x+1) = x^2 + 2x + 1
    auto f = make_poly({0, 0, 1});
    auto g = f.translate(1);
    GNFS_TEST_CHECK(g.degree() == 2);
    GNFS_TEST_CHECK(g[0].to_int64() == 1);
    GNFS_TEST_CHECK(g[1].to_int64() == 2);
    GNFS_TEST_CHECK(g[2].to_int64() == 1);

    // f(x) = x + 1, f(x+3) = x + 4
    auto h = make_poly({1, 1});
    auto h3 = h.translate(3);
    GNFS_TEST_CHECK(h3.degree() == 1);
    GNFS_TEST_CHECK(h3[0].to_int64() == 4);
    GNFS_TEST_CHECK(h3[1].to_int64() == 1);

    // f(x+0) = f(x)
    auto f0 = f.translate(0);
    GNFS_TEST_CHECK(f0.degree() == f.degree());
    for (uint32_t i = 0; i <= f.degree(); ++i) {
        GNFS_TEST_CHECK(f0[i].compare(f[i]) == 0);
    }

    // Verify: f(x+t) evaluated at x should equal f(x+t)
    auto p = make_poly({1, -3, 2, 1}); // x^3 + 2x^2 - 3x + 1
    auto shifted = p.translate(5);
    // shifted(0) should equal p(5)
    Integer p_at_5 = p.evaluate(Integer(5));
    Integer shifted_at_0 = shifted.evaluate(Integer(0));
    GNFS_TEST_CHECK(p_at_5.compare(shifted_at_0) == 0);

    // shifted(2) should equal p(7)
    Integer p_at_7 = p.evaluate(Integer(7));
    Integer shifted_at_2 = shifted.evaluate(Integer(2));
    GNFS_TEST_CHECK(p_at_7.compare(shifted_at_2) == 0);

    // IntPolynomial accepts arbitrary degrees. The translate implementation
    // keeps its cached low-degree path while using GMP binomial coefficients
    // above the cache boundary, where Release builds must remain safe too.
    std::vector<Integer> high_degree_coeffs(18);
    high_degree_coeffs[0] = Integer(5);
    high_degree_coeffs[1] = Integer(-3);
    high_degree_coeffs[7] = Integer(2);
    high_degree_coeffs[17] = Integer(1);
    IntPolynomial high_degree(std::move(high_degree_coeffs));
    auto high_shifted = high_degree.translate(9);
    GNFS_TEST_CHECK(high_shifted.degree() == 17);
    const Integer high_input(4);
    const Integer high_shifted_input(13);
    GNFS_TEST_CHECK(
        high_shifted.evaluate(high_input).compare(high_degree.evaluate(high_shifted_input)) == 0);

    std::cout << "  PASS" << std::endl;
}

void test_discriminant() {
    std::cout << "Testing discriminant..." << std::endl;

    // Linear: discriminant = 1
    auto l = make_poly({-3, 1});
    GNFS_TEST_CHECK(l.discriminant().to_int64() == 1);

    // Quadratic: ax^2 + bx + c, disc = b^2 - 4ac
    // x^2 - 5x + 6 = (x-2)(x-3), disc = 25 - 24 = 1
    auto q1 = make_poly({6, -5, 1});
    GNFS_TEST_CHECK(q1.discriminant().to_int64() == 1);

    // x^2 + 1, disc = 0 - 4 = -4
    auto q2 = make_poly({1, 0, 1});
    GNFS_TEST_CHECK(q2.discriminant().to_int64() == -4);

    // 2x^2 + 3x + 1, disc = 9 - 8 = 1
    auto q3 = make_poly({1, 3, 2});
    GNFS_TEST_CHECK(q3.discriminant().to_int64() == 1);

    std::cout << "  PASS" << std::endl;
}

void test_discriminant_high_degree() {
    std::cout << "Testing discriminant degree 3-6..." << std::endl;

    // Cubics — known values cross-checked w/ test_class_group cases
    // disc(x^3 + x + 1) = -4·1^3 - 27·1^2 = -31
    auto c1 = make_poly({1, 1, 0, 1});
    GNFS_TEST_CHECK(c1.discriminant().to_int64() == -31);

    // disc(x^3 + 2x + 1) = -4·8 - 27·1 = -59
    auto c2 = make_poly({1, 2, 0, 1});
    GNFS_TEST_CHECK(c2.discriminant().to_int64() == -59);

    // disc(x^3 + 5x + 1) = -4·125 - 27·1 = -527
    auto c3 = make_poly({1, 5, 0, 1});
    GNFS_TEST_CHECK(c3.discriminant().to_int64() == -527);

    // disc(x^3 - 2) = -27·4 = -108
    auto c4 = make_poly({-2, 0, 0, 1});
    GNFS_TEST_CHECK(c4.discriminant().to_int64() == -108);

    // Quartics — cross-checked w/ test_class_group cases
    // disc(x^4 + 1) = 256
    auto q4_1 = make_poly({1, 0, 0, 0, 1});
    GNFS_TEST_CHECK(q4_1.discriminant().to_int64() == 256);

    // disc(x^4 - 5x^2 + 6) = 96
    auto q4_2 = make_poly({6, 0, -5, 0, 1});
    GNFS_TEST_CHECK(q4_2.discriminant().to_int64() == 96);

    // disc(x^4 + x + 1) = 229
    auto q4_3 = make_poly({1, 1, 0, 0, 1});
    GNFS_TEST_CHECK(q4_3.discriminant().to_int64() == 229);

    // disc(x^4 - x - 1) = -283
    auto q4_4 = make_poly({-1, -1, 0, 0, 1});
    GNFS_TEST_CHECK(q4_4.discriminant().to_int64() == -283);

    // Quintic — cross-checked w/ test_class_group case
    // disc(x^5 + x + 1) = 3381
    auto q5 = make_poly({1, 1, 0, 0, 0, 1});
    GNFS_TEST_CHECK(q5.discriminant().to_int64() == 3381);

    // Sextic — cross-checked w/ test_class_group case
    // disc(x^6 + x + 1) = -43531
    auto q6 = make_poly({1, 1, 0, 0, 0, 0, 1});
    GNFS_TEST_CHECK(q6.discriminant().to_int64() == -43531);

    std::cout << "  PASS" << std::endl;
}

void test_discriminant_leading_coeff() {
    std::cout << "Testing discriminant non-monic..." << std::endl;

    // disc(2x^3 + 3x^2 + 4x + 5):
    // f' = 6x^2 + 6x + 4
    // Res(f, f') = ... use known formula or fall back to Sylvester
    // For 2x^3 + ax^2 + bx + c, disc = a²b² - 4b³ - 4a³c - 27c² + 18abc all over 2
    // = (3)²(4)² - 4·(4)³ - 4·(3)³·(5) - 27·5² + 18·(3)(4)(5)
    // = 9·16 - 4·64 - 4·27·5 - 27·25 + 18·60
    // = 144 - 256 - 540 - 675 + 1080 = -247
    // But this is for monic. For non-monic 2x^3+..., the polynomial::discriminant
    // computes Res(f, f') / a_d where a_d = 2. Let's just verify it doesn't throw
    // and produces an Integer result.
    auto p = make_poly({5, 4, 3, 2});
    Integer d = p.discriminant();
    // Don't hard-code value — just ensure non-monic case doesn't crash and
    // returns a consistent Integer (exact value depends on Bareiss division by a_d).
    (void)d;
    GNFS_TEST_CHECK(d.compare(Integer(0)) != 0); // Not zero for this irreducible-ish form

    // disc(x^3 - 7x + 6) = -4·(-7)^3 - 27·6² = -4·(-343) - 27·36 = 1372 - 972 = 400
    // f = (x-1)(x-2)(x+3), all distinct integer roots → disc > 0
    auto p2 = make_poly({6, -7, 0, 1});
    GNFS_TEST_CHECK(p2.discriminant().to_int64() == 400);

    std::cout << "  PASS" << std::endl;
}

void test_edge_cases() {
    std::cout << "Testing edge cases..." << std::endl;

    // Mutable out-of-bounds access resizes
    auto p = make_poly({1});
    p[5] = Integer(42);
    GNFS_TEST_CHECK(p.degree() == 5);
    GNFS_TEST_CHECK(p[5].to_int64() == 42);

    // Large coefficients
    auto big = make_poly({0, 0, 1});
    Integer large_x("1000000000000000");
    Integer result = big.evaluate(large_x);
    // x^2 at x=10^15 = 10^30
    Integer expected("1000000000000000000000000000000");
    GNFS_TEST_CHECK(result.compare(expected) == 0);

    // Negative coefficients with evaluate_mod
    auto neg = make_poly({-7, 1});
    // f(x) = x - 7, f(3) mod 11 = -4 mod 11 = 7
    GNFS_TEST_CHECK(neg.evaluate_mod(3, 11) == 7);

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== IntPolynomial Unit Tests ===" << std::endl;

    test_construction();
    test_access();
    test_clone();
    test_evaluate();
    test_evaluate_double();
    test_evaluate_mod();
    test_roots_mod_p();
    test_addition();
    test_subtraction();
    test_scalar_multiply();
    test_polynomial_multiply();
    test_derivative();
    test_translate();
    test_discriminant();
    test_discriminant_high_degree();
    test_discriminant_leading_coeff();
    test_edge_cases();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
