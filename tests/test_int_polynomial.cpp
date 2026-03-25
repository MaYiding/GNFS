// Unit tests for IntPolynomial — integer polynomial arithmetic
#include "gnfs/polynomial/int_polynomial.hpp"

#include <cassert>
#include <iostream>

using namespace gnfs::polynomial;
using gnfs::core::Integer;

// Helper: create polynomial from int64 coefficients
IntPolynomial make_poly(std::initializer_list<int64_t> coeffs) {
    std::vector<Integer> v;
    for (auto c : coeffs) v.push_back(Integer(c));
    return IntPolynomial(std::move(v));
}

void test_construction() {
    std::cout << "Testing construction..." << std::endl;

    // Default: zero polynomial
    IntPolynomial zero;
    assert(zero.is_zero());
    assert(zero.degree() == 0);

    // From constant
    IntPolynomial c(42);
    assert(!c.is_zero());
    assert(c.degree() == 0);
    assert(c[0].to_int64() == 42);

    // From coefficients: 3x^2 + 2x + 1
    auto p = make_poly({1, 2, 3});
    assert(p.degree() == 2);
    assert(p[0].to_int64() == 1);
    assert(p[1].to_int64() == 2);
    assert(p[2].to_int64() == 3);

    // Leading zeros are trimmed
    auto p2 = make_poly({1, 2, 0, 0});
    assert(p2.degree() == 1);

    // All zeros → zero polynomial
    auto p3 = make_poly({0, 0, 0});
    assert(p3.is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_access() {
    std::cout << "Testing coefficient access..." << std::endl;

    auto p = make_poly({5, -3, 7});

    // In-bounds access
    assert(p[0].to_int64() == 5);
    assert(p[1].to_int64() == -3);
    assert(p[2].to_int64() == 7);

    // Out-of-bounds const access returns 0
    const auto& cp = p;
    assert(cp[10].is_zero());

    // Leading coefficient
    assert(p.leading_coeff().to_int64() == 7);

    // coefficients() accessor
    assert(p.coefficients().size() == 3);

    std::cout << "  PASS" << std::endl;
}

void test_clone() {
    std::cout << "Testing clone..." << std::endl;

    auto p = make_poly({1, 2, 3});
    auto q = p.clone();

    assert(q.degree() == p.degree());
    for (uint32_t i = 0; i <= q.degree(); ++i) {
        assert(q[i].compare(p[i]) == 0);
    }

    std::cout << "  PASS" << std::endl;
}

void test_evaluate() {
    std::cout << "Testing evaluate..." << std::endl;

    // f(x) = 3x^2 + 2x + 1
    auto f = make_poly({1, 2, 3});

    // f(0) = 1
    assert(f.evaluate(Integer(0)).to_int64() == 1);

    // f(1) = 1 + 2 + 3 = 6
    assert(f.evaluate(Integer(1)).to_int64() == 6);

    // f(2) = 1 + 4 + 12 = 17
    assert(f.evaluate(Integer(2)).to_int64() == 17);

    // f(-1) = 1 - 2 + 3 = 2
    assert(f.evaluate(Integer(-1)).to_int64() == 2);

    // f(10) = 1 + 20 + 300 = 321
    assert(f.evaluate(Integer(10)).to_int64() == 321);

    // Zero polynomial
    IntPolynomial zero;
    assert(zero.evaluate(Integer(42)).is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_evaluate_double() {
    std::cout << "Testing evaluate_double..." << std::endl;

    auto f = make_poly({1, 2, 3});
    double val = f.evaluate_double(2.0);
    assert(std::abs(val - 17.0) < 1e-10);

    val = f.evaluate_double(0.5);
    // f(0.5) = 1 + 1 + 0.75 = 2.75
    assert(std::abs(val - 2.75) < 1e-10);

    std::cout << "  PASS" << std::endl;
}

void test_evaluate_mod() {
    std::cout << "Testing evaluate_mod..." << std::endl;

    // f(x) = x^2 + x + 1
    auto f = make_poly({1, 1, 1});

    // f(0) mod 7 = 1
    assert(f.evaluate_mod(0, 7) == 1);

    // f(1) mod 7 = 3
    assert(f.evaluate_mod(1, 7) == 3);

    // f(2) mod 7 = 7 mod 7 = 0
    assert(f.evaluate_mod(2, 7) == 0);

    // f(4) mod 7 = 21 mod 7 = 0
    assert(f.evaluate_mod(4, 7) == 0);

    // With negative coefficients: f(x) = x^2 - 3x + 2
    auto g = make_poly({2, -3, 1});
    // g(0) = 2, g(1) = 0, g(2) = 0
    assert(g.evaluate_mod(0, 11) == 2);
    assert(g.evaluate_mod(1, 11) == 0);
    assert(g.evaluate_mod(2, 11) == 0);

    std::cout << "  PASS" << std::endl;
}

void test_roots_mod_p() {
    std::cout << "Testing roots_mod_p..." << std::endl;

    // f(x) = x^2 - 1 = (x-1)(x+1)
    auto f = make_poly({-1, 0, 1});

    // Roots mod 7: x=1 and x=6 (≡ -1)
    auto roots7 = f.roots_mod_p(7);
    assert(roots7.size() == 2);
    for (auto r : roots7) {
        assert(f.evaluate_mod(r, 7) == 0);
    }

    // f(x) = x^2 + 1: no roots mod 3
    auto g = make_poly({1, 0, 1});
    auto roots3 = g.roots_mod_p(3);
    assert(roots3.empty());

    // f(x) = x^2 + 1: roots mod 5 are x=2,3
    auto roots5 = g.roots_mod_p(5);
    assert(roots5.size() == 2);
    for (auto r : roots5) {
        assert(g.evaluate_mod(r, 5) == 0);
    }

    // Linear: f(x) = x - 3, root mod 7 = 3
    auto h = make_poly({-3, 1});
    auto roots = h.roots_mod_p(7);
    assert(roots.size() == 1);
    assert(roots[0] == 3);

    std::cout << "  PASS" << std::endl;
}

void test_addition() {
    std::cout << "Testing addition..." << std::endl;

    // (3x^2 + 2x + 1) + (x + 5) = 3x^2 + 3x + 6
    auto a = make_poly({1, 2, 3});
    auto b = make_poly({5, 1});
    a += b;
    assert(a.degree() == 2);
    assert(a[0].to_int64() == 6);
    assert(a[1].to_int64() == 3);
    assert(a[2].to_int64() == 3);

    // Addition that reduces degree: (x^2 + 1) + (-x^2 + 2) = 3
    auto c = make_poly({1, 0, 1});
    auto d = make_poly({2, 0, -1});
    c += d;
    assert(c.degree() == 0);
    assert(c[0].to_int64() == 3);

    std::cout << "  PASS" << std::endl;
}

void test_subtraction() {
    std::cout << "Testing subtraction..." << std::endl;

    // (3x^2 + 2x + 1) - (x^2 + x + 1) = 2x^2 + x
    auto a = make_poly({1, 2, 3});
    auto b = make_poly({1, 1, 1});
    a -= b;
    assert(a.degree() == 2);
    assert(a[0].to_int64() == 0);
    assert(a[1].to_int64() == 1);
    assert(a[2].to_int64() == 2);

    // Self-subtraction → zero
    auto c = make_poly({1, 2, 3});
    auto d = c.clone();
    c -= d;
    assert(c.is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_scalar_multiply() {
    std::cout << "Testing scalar multiplication..." << std::endl;

    // (2x + 3) × 5 = 10x + 15
    auto a = make_poly({3, 2});
    a *= 5;
    assert(a[0].to_int64() == 15);
    assert(a[1].to_int64() == 10);

    // × 0 → zero
    auto b = make_poly({1, 2, 3});
    b *= 0;
    assert(b.is_zero());

    // × (-1) negates
    auto c = make_poly({1, -2, 3});
    c *= -1;
    assert(c[0].to_int64() == -1);
    assert(c[1].to_int64() == 2);
    assert(c[2].to_int64() == -3);

    std::cout << "  PASS" << std::endl;
}

void test_polynomial_multiply() {
    std::cout << "Testing polynomial multiplication..." << std::endl;

    // (x + 1)(x - 1) = x^2 - 1
    auto a = make_poly({1, 1});
    auto b = make_poly({-1, 1});
    auto c = a * b;
    assert(c.degree() == 2);
    assert(c[0].to_int64() == -1);
    assert(c[1].to_int64() == 0);
    assert(c[2].to_int64() == 1);

    // (x + 2)(x + 3) = x^2 + 5x + 6
    auto d = make_poly({2, 1});
    auto e = make_poly({3, 1});
    auto f = d * e;
    assert(f.degree() == 2);
    assert(f[0].to_int64() == 6);
    assert(f[1].to_int64() == 5);
    assert(f[2].to_int64() == 1);

    // Multiply by zero
    IntPolynomial zero;
    auto z = a * zero;
    assert(z.is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_derivative() {
    std::cout << "Testing derivative..." << std::endl;

    // f(x) = 3x^3 + 2x^2 + x + 5
    // f'(x) = 9x^2 + 4x + 1
    auto f = make_poly({5, 1, 2, 3});
    auto fp = f.derivative();
    assert(fp.degree() == 2);
    assert(fp[0].to_int64() == 1);
    assert(fp[1].to_int64() == 4);
    assert(fp[2].to_int64() == 9);

    // Constant → 0
    auto c = make_poly({42});
    auto cp = c.derivative();
    assert(cp.is_zero());

    // Linear → constant
    auto l = make_poly({3, 7});
    auto lp = l.derivative();
    assert(lp.degree() == 0);
    assert(lp[0].to_int64() == 7);

    std::cout << "  PASS" << std::endl;
}

void test_translate() {
    std::cout << "Testing translate (Taylor shift)..." << std::endl;

    // f(x) = x^2, f(x+1) = x^2 + 2x + 1
    auto f = make_poly({0, 0, 1});
    auto g = f.translate(1);
    assert(g.degree() == 2);
    assert(g[0].to_int64() == 1);
    assert(g[1].to_int64() == 2);
    assert(g[2].to_int64() == 1);

    // f(x) = x + 1, f(x+3) = x + 4
    auto h = make_poly({1, 1});
    auto h3 = h.translate(3);
    assert(h3.degree() == 1);
    assert(h3[0].to_int64() == 4);
    assert(h3[1].to_int64() == 1);

    // f(x+0) = f(x)
    auto f0 = f.translate(0);
    assert(f0.degree() == f.degree());
    for (uint32_t i = 0; i <= f.degree(); ++i) {
        assert(f0[i].compare(f[i]) == 0);
    }

    // Verify: f(x+t) evaluated at x should equal f(x+t)
    auto p = make_poly({1, -3, 2, 1});  // x^3 + 2x^2 - 3x + 1
    auto shifted = p.translate(5);
    // shifted(0) should equal p(5)
    Integer p_at_5 = p.evaluate(Integer(5));
    Integer shifted_at_0 = shifted.evaluate(Integer(0));
    assert(p_at_5.compare(shifted_at_0) == 0);

    // shifted(2) should equal p(7)
    Integer p_at_7 = p.evaluate(Integer(7));
    Integer shifted_at_2 = shifted.evaluate(Integer(2));
    assert(p_at_7.compare(shifted_at_2) == 0);

    std::cout << "  PASS" << std::endl;
}

void test_discriminant() {
    std::cout << "Testing discriminant..." << std::endl;

    // Linear: discriminant = 1
    auto l = make_poly({-3, 1});
    assert(l.discriminant().to_int64() == 1);

    // Quadratic: ax^2 + bx + c, disc = b^2 - 4ac
    // x^2 - 5x + 6 = (x-2)(x-3), disc = 25 - 24 = 1
    auto q1 = make_poly({6, -5, 1});
    assert(q1.discriminant().to_int64() == 1);

    // x^2 + 1, disc = 0 - 4 = -4
    auto q2 = make_poly({1, 0, 1});
    assert(q2.discriminant().to_int64() == -4);

    // 2x^2 + 3x + 1, disc = 9 - 8 = 1
    auto q3 = make_poly({1, 3, 2});
    assert(q3.discriminant().to_int64() == 1);

    std::cout << "  PASS" << std::endl;
}

void test_edge_cases() {
    std::cout << "Testing edge cases..." << std::endl;

    // Mutable out-of-bounds access resizes
    auto p = make_poly({1});
    p[5] = Integer(42);
    assert(p.degree() == 5);
    assert(p[5].to_int64() == 42);

    // Large coefficients
    auto big = make_poly({0, 0, 1});
    Integer large_x("1000000000000000");
    Integer result = big.evaluate(large_x);
    // x^2 at x=10^15 = 10^30
    Integer expected("1000000000000000000000000000000");
    assert(result.compare(expected) == 0);

    // Negative coefficients with evaluate_mod
    auto neg = make_poly({-7, 1});
    // f(x) = x - 7, f(3) mod 11 = -4 mod 11 = 7
    assert(neg.evaluate_mod(3, 11) == 7);

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
    test_edge_cases();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
