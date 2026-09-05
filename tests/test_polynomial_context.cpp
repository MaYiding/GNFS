// Unit tests for PolynomialContext — evaluate, norms, verify, edge cases
#include "gnfs/core/polynomial_context.hpp"
#include "support/test_check.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace gnfs::core;

// ─── helpers ───────────────────────────────────────────────

// f(x) = x^3 + x + 1;  f(5)=131 (prime).  m=5, n=131.
static PolynomialContext make_cubic_131() {
    std::vector<Integer> c = {Integer(1LL), Integer(1LL), Integer(0LL), Integer(1LL)};
    return PolynomialContext(Integer(131LL), std::move(c), Integer(5LL));
}

// f(x) = x^4 - 10x^2 + 1;  f(3) = 81-90+1 = -8 → not useful.
// Instead: f(x) = 2x^3 - 3x^2 + x - 6;  f(3) = 54-27+3-6 = 24.  n=24, m=3.
static PolynomialContext make_cubic_24() {
    std::vector<Integer> c = {Integer(-6LL), Integer(1LL), Integer(-3LL), Integer(2LL)};
    return PolynomialContext(Integer(24LL), std::move(c), Integer(3LL));
}

// ─── tests ───────────────────────────────────────────────────

void test_construction_and_degree() {
    std::cout << "Testing construction and degree..." << std::endl;
    auto ctx = make_cubic_131();
    assert(ctx.degree() == 3);
    assert(ctx.n() == Integer(131LL));
    assert(ctx.m() == Integer(5LL));
    std::cout << "  PASS" << std::endl;
}

void test_coeff_access() {
    std::cout << "Testing coeff access..." << std::endl;
    auto ctx = make_cubic_131();
    // f(x) = 1*x^3 + 0*x^2 + 1*x + 1
    assert(ctx.coeff(0) == Integer(1LL)); // constant
    assert(ctx.coeff(1) == Integer(1LL)); // x
    assert(ctx.coeff(2) == Integer(0LL)); // x^2
    assert(ctx.coeff(3) == Integer(1LL)); // x^3
    // Out-of-range returns zero
    assert(ctx.coeff(99) == Integer(0LL));
    std::cout << "  PASS" << std::endl;
}

void test_leading_coeff() {
    std::cout << "Testing leading_coeff..." << std::endl;
    auto ctx = make_cubic_131();
    assert(ctx.leading_coeff() == Integer(1LL)); // monic

    auto ctx2 = make_cubic_24();
    assert(ctx2.leading_coeff() == Integer(2LL)); // 2x^3 + ...
    std::cout << "  PASS" << std::endl;
}

void test_evaluate_horner() {
    std::cout << "Testing evaluate (Horner)..." << std::endl;
    auto ctx = make_cubic_131();
    // f(0) = 1
    assert(ctx.evaluate(Integer(0LL)) == Integer(1LL));
    // f(1) = 1+0+1+1 = 3
    assert(ctx.evaluate(Integer(1LL)) == Integer(3LL));
    // f(5) = 125+5+1 = 131
    assert(ctx.evaluate(Integer(5LL)) == Integer(131LL));
    // f(-1) = -1+0-1+1 = -1
    assert(ctx.evaluate(Integer(-1LL)) == Integer(-1LL));
    std::cout << "  PASS" << std::endl;
}

void test_evaluate_mod() {
    std::cout << "Testing evaluate_mod..." << std::endl;
    auto ctx = make_cubic_131();
    // f(5) mod 131 = 0
    assert(ctx.evaluate_mod(5, 131) == 0);
    // f(0) mod 131 = 1
    assert(ctx.evaluate_mod(0, 131) == 1);
    // f(1) mod 7 = 3 mod 7 = 3
    assert(ctx.evaluate_mod(1, 7) == 3);
    // With coefficients that need mod-reduction
    auto ctx2 = make_cubic_24();
    // f(3) = 24 ≡ 0 mod 24
    assert(ctx2.evaluate_mod(3, 24) == 0);
    // A zero modulus has no residue; the API uses the same sentinel as
    // IntPolynomial::evaluate_mod instead of evaluating a division by zero.
    GNFS_TEST_CHECK(ctx.evaluate_mod(5, 0) == 0);
    std::cout << "  PASS" << std::endl;
}

void test_evaluate_double() {
    std::cout << "Testing evaluate_double..." << std::endl;
    auto ctx = make_cubic_131();
    // f(5.0) ≈ 131.0
    double result = ctx.evaluate_double(5.0);
    assert(result > 130.9 && result < 131.1);
    // f(0.0) = 1.0
    assert(ctx.evaluate_double(0.0) == 1.0);
    std::cout << "  PASS" << std::endl;
}

void test_verify() {
    std::cout << "Testing verify (f(m) ≡ 0 mod n)..." << std::endl;
    auto ctx = make_cubic_131();
    assert(ctx.verify()); // f(5) = 131 ≡ 0 mod 131

    auto ctx2 = make_cubic_24();
    assert(ctx2.verify()); // f(3) = 24 ≡ 0 mod 24
    std::cout << "  PASS" << std::endl;
}

void test_rational_value() {
    std::cout << "Testing rational_value (a - b*m)..." << std::endl;
    auto ctx = make_cubic_131();
    // m = 5
    // R(10, 2) = 10 - 2*5 = 0
    auto rv = ctx.rational_value(10, 2);
    assert(rv == Integer(0LL));
    // R(7, 1) = 7 - 5 = 2
    rv = ctx.rational_value(7, 1);
    assert(rv == Integer(2LL));
    // R(0, 3) = 0 - 15 = -15
    rv = ctx.rational_value(0, 3);
    assert(rv == Integer(-15LL));
    std::cout << "  PASS" << std::endl;
}

void test_algebraic_norm() {
    std::cout << "Testing algebraic_norm..." << std::endl;
    auto ctx = make_cubic_131();
    // N(a - bα) = b^3 * f(a/b)
    // For b=1: N(a - α) = f(a) = a^3 + a + 1
    // a=2, b=1: N = f(2) = 8+2+1 = 11
    auto norm = ctx.algebraic_norm(2, 1);
    assert(norm == Integer(11LL));
    // a=1, b=1: N = f(1) = 3
    norm = ctx.algebraic_norm(1, 1);
    assert(norm == Integer(3LL));
    // a=0, b=1: N = f(0) = 1
    norm = ctx.algebraic_norm(0, 1);
    assert(norm == Integer(1LL));
    std::cout << "  PASS" << std::endl;
}

void test_clone_is_independent() {
    std::cout << "Testing clone independence..." << std::endl;
    auto ctx = make_cubic_131();
    auto ctx2 = ctx.clone();
    // Both should verify
    assert(ctx.verify());
    assert(ctx2.verify());
    // Same coefficients
    for (uint32_t i = 0; i <= 3; ++i) {
        assert(ctx.coeff(i) == ctx2.coeff(i));
    }
    std::cout << "  PASS" << std::endl;
}

void test_trailing_zero_removal() {
    std::cout << "Testing trailing-zero normalization..." << std::endl;
    // Coefficients: [5, 0, 0, 0] → should produce degree 0 polynomial
    std::vector<Integer> c = {Integer(5LL), Integer(0LL), Integer(0LL), Integer(0LL)};
    PolynomialContext ctx(Integer(5LL), std::move(c), Integer(0LL));
    assert(ctx.degree() == 0);
    assert(ctx.coeff(0) == Integer(5LL));
    std::cout << "  PASS" << std::endl;
}

void test_empty_poly_throws() {
    std::cout << "Testing empty polynomial throws..." << std::endl;
    bool threw = false;
    try {
        std::vector<Integer> empty;
        PolynomialContext ctx(Integer(1LL), std::move(empty), Integer(0LL));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    std::cout << "  PASS" << std::endl;
}

void test_n_digits() {
    std::cout << "Testing n_digits..." << std::endl;
    auto ctx = make_cubic_131();
    // 131 has 3 decimal digits
    assert(ctx.n_digits() == 3);
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== PolynomialContext Unit Tests ===" << std::endl;

    test_construction_and_degree();
    test_coeff_access();
    test_leading_coeff();
    test_evaluate_horner();
    test_evaluate_mod();
    test_evaluate_double();
    test_verify();
    test_rational_value();
    test_algebraic_norm();
    test_clone_is_independent();
    test_trailing_zero_removal();
    test_empty_poly_throws();
    test_n_digits();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
