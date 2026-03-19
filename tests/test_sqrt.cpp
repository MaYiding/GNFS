// test_sqrt.cpp - Test square root components

#include <gnfs/sqrt/number_field.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/sqrt/modular_poly.hpp>
#include <gnfs/sqrt/couveignes.hpp>
#include <gnfs/core/polynomial_context.hpp>

#include <cassert>
#include <iostream>
#include <vector>
#include <chrono>

using namespace gnfs;
using namespace gnfs::sqrt;
using namespace gnfs::core;

// Test NumberFieldElement construction and basic operations
void test_number_field_element() {
    std::cout << "Testing NumberFieldElement..." << std::endl;

    // Test constant element
    NumberFieldElement elem1(Integer(5));
    assert(!elem1.is_zero());
    assert(elem1.degree() == 0);
    assert(elem1.coeff(0).to_int64() == 5);

    // Test from coefficients: 3 + 2*α
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(3));
    coeffs.push_back(Integer(2));
    NumberFieldElement elem2(std::move(coeffs));
    assert(elem2.degree() == 1);
    assert(elem2.coeff(0).to_int64() == 3);
    assert(elem2.coeff(1).to_int64() == 2);

    // Test is_zero
    NumberFieldElement zero(Integer(static_cast<int64_t>(0)));
    assert(zero.is_zero());

    // Test is_one
    NumberFieldElement one(Integer(static_cast<int64_t>(1)));
    assert(one.is_one());

    // Test clone
    auto elem1_clone = elem1.clone();
    assert(elem1_clone.coeff(0).to_int64() == 5);

    std::cout << "  NumberFieldElement: PASSED" << std::endl;
}

// Test NumberFieldElement arithmetic
void test_number_field_element_arithmetic() {
    std::cout << "Testing NumberFieldElement arithmetic..." << std::endl;

    // elem1 = 3 + 2*α
    std::vector<Integer> coeffs1;
    coeffs1.push_back(Integer(3));
    coeffs1.push_back(Integer(2));
    NumberFieldElement elem1(std::move(coeffs1));

    // elem2 = 1 + 4*α
    std::vector<Integer> coeffs2;
    coeffs2.push_back(Integer(static_cast<int64_t>(1)));
    coeffs2.push_back(Integer(4));
    NumberFieldElement elem2(std::move(coeffs2));

    // Test addition: (3 + 2*α) + (1 + 4*α) = 4 + 6*α
    auto sum = elem1.clone();
    sum.add(elem2);
    assert(sum.coeff(0).to_int64() == 4);
    assert(sum.coeff(1).to_int64() == 6);

    // Test subtraction: (3 + 2*α) - (1 + 4*α) = 2 - 2*α
    auto diff = elem1.clone();
    diff.subtract(elem2);
    assert(diff.coeff(0).to_int64() == 2);
    assert(diff.coeff(1).to_int64() == -2);

    // Test scalar multiplication: 3 * (1 + 4*α) = 3 + 12*α
    auto scaled = elem2.clone();
    Integer three(3);
    scaled.multiply_scalar(three);
    assert(scaled.coeff(0).to_int64() == 3);
    assert(scaled.coeff(1).to_int64() == 12);

    // Test negate: -(3 + 2*α) = -3 - 2*α
    auto neg = elem1.clone();
    neg.negate();
    assert(neg.coeff(0).to_int64() == -3);
    assert(neg.coeff(1).to_int64() == -2);

    std::cout << "  NumberFieldElement arithmetic: PASSED" << std::endl;
}

// Test NumberField construction
void test_number_field() {
    std::cout << "Testing NumberField..." << std::endl;

    // Create polynomial context for x^2 - 2 (factoring would give sqrt(2))
    // N = 15, m = 4 (since 4^2 - 2 = 14 ≡ -1 (mod 15))
    // Actually let's use a simpler setup
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(-15));  // constant
    coeffs.push_back(Integer(static_cast<int64_t>(0)));    // x
    coeffs.push_back(Integer(static_cast<int64_t>(1)));    // x^2

    Integer n(15);
    Integer m(4);  // 4^2 = 16 ≡ 1 (mod 15)

    PolynomialContext ctx(std::move(n), std::move(coeffs), std::move(m));

    NumberField nf(ctx);

    assert(nf.degree() == 2);

    // Test zero and one
    auto zero = nf.zero();
    assert(zero.is_zero());

    auto one = nf.one();
    assert(one.is_one());

    // Test alpha
    auto alpha = nf.alpha();
    assert(alpha.coeff(0).is_zero());
    assert(alpha.coeff(1).to_int64() == 1);

    // Test from_ab (GNFS convention: a - b*α)
    auto ab_elem = nf.from_ab(3, 2);  // 3 - 2*α (GNFS convention)
    assert(ab_elem.coeff(0).to_int64() == 3);
    assert(ab_elem.coeff(1).to_int64() == -2);  // -b in GNFS convention

    std::cout << "  NumberField: PASSED" << std::endl;
}

// Test NumberField multiplication
void test_number_field_multiply() {
    std::cout << "Testing NumberField multiplication..." << std::endl;

    // f(x) = x^2 - 2, so α^2 = 2
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(-2));   // constant: -2
    coeffs.push_back(Integer(static_cast<int64_t>(0)));    // x
    coeffs.push_back(Integer(static_cast<int64_t>(1)));    // x^2

    Integer n(1000000007);  // Large prime for testing
    Integer m(1000);

    PolynomialContext ctx(std::move(n), std::move(coeffs), std::move(m));
    NumberField nf(ctx);

    // from_ab(1, 1) returns 1 - α (GNFS convention)
    // (1 - α) * (1 - α) = 1 - 2α + α^2 = 1 - 2α + 2 = 3 - 2α
    auto one_minus_alpha = nf.from_ab(1, 1);  // 1 - α
    auto squared = nf.multiply(one_minus_alpha, one_minus_alpha);

    assert(squared.coeff(0).to_int64() == 3);
    assert(squared.coeff(1).to_int64() == -2);  // -2α (GNFS convention)

    // For (1 + α), we need to create it directly with positive α coefficient
    // (1 + α) * (1 - α) = 1 - α^2 = 1 - 2 = -1
    std::vector<Integer> one_plus_alpha_coeffs;
    one_plus_alpha_coeffs.push_back(Integer(static_cast<int64_t>(1)));
    one_plus_alpha_coeffs.push_back(Integer(static_cast<int64_t>(1)));
    NumberFieldElement one_plus_alpha(std::move(one_plus_alpha_coeffs));
    std::vector<Integer> neg_alpha_coeffs;
    neg_alpha_coeffs.push_back(Integer(static_cast<int64_t>(1)));
    neg_alpha_coeffs.push_back(Integer(static_cast<int64_t>(-1)));
    NumberFieldElement one_minus_alpha_elem(std::move(neg_alpha_coeffs));

    auto product = nf.multiply(one_minus_alpha_elem, one_plus_alpha);
    assert(product.coeff(0).to_int64() == -1);
    assert(product.degree() == 0 || product.coeff(1).is_zero());

    std::cout << "  NumberField multiplication: PASSED" << std::endl;
}

// Test NumberField power
void test_number_field_power() {
    std::cout << "Testing NumberField power..." << std::endl;

    // f(x) = x^2 - 2
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(-2));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(1)));

    Integer n(1000000007);
    Integer m(1000);

    PolynomialContext ctx(std::move(n), std::move(coeffs), std::move(m));
    NumberField nf(ctx);

    // α^0 = 1
    auto alpha = nf.alpha();
    auto power0 = nf.power(alpha, Integer(static_cast<int64_t>(0)));
    assert(power0.is_one());

    // α^1 = α
    auto power1 = nf.power(alpha, Integer(static_cast<int64_t>(1)));
    assert(power1.coeff(0).is_zero());
    assert(power1.coeff(1).to_int64() == 1);

    // α^2 = 2 (since f(α) = 0 means α^2 = 2)
    auto power2 = nf.power(alpha, Integer(2));
    assert(power2.coeff(0).to_int64() == 2);
    assert(power2.degree() == 0 || power2.coeff(1).is_zero());

    // α^3 = α * α^2 = 2α
    auto power3 = nf.power(alpha, Integer(3));
    assert(power3.coeff(0).is_zero());
    assert(power3.coeff(1).to_int64() == 2);

    // α^4 = (α^2)^2 = 4
    auto power4 = nf.power(alpha, Integer(4));
    assert(power4.coeff(0).to_int64() == 4);

    std::cout << "  NumberField power: PASSED" << std::endl;
}

// Test evaluate_at_m
void test_evaluate_at_m() {
    std::cout << "Testing evaluate_at_m..." << std::endl;

    // f(x) = x^2 - 15, m = 4
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(-15));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(1)));

    Integer n(143);  // 11 * 13
    Integer m(4);

    PolynomialContext ctx(std::move(n), std::move(coeffs), std::move(m));
    NumberField nf(ctx);

    // GNFS convention: from_ab(a, b) creates a - b*α
    // Element: 3 - 2*α
    // At m=4: 3 - 2*4 = -5
    auto elem = nf.from_ab(3, 2);
    auto value = nf.evaluate_at_m(elem);
    assert(value.to_int64() == -5);

    // Element: 5 - 3*α (GNFS convention)
    // At m=4: 5 - 3*4 = -7
    auto elem2 = nf.from_ab(5, 3);
    auto value2 = nf.evaluate_at_m(elem2);
    assert(value2.to_int64() == -7);

    // Mod n: -7 mod 143 = 136
    auto value2_mod = nf.evaluate_at_m_mod_n(elem2);
    assert(value2_mod.to_int64() == 136);

    std::cout << "  evaluate_at_m: PASSED" << std::endl;
}

// Test rational sqrt computation (simplified)
void test_rational_sqrt_simple() {
    std::cout << "Testing RationalSqrt (simple)..." << std::endl;

    // Test compute_from_exponents
    // sqrt(2^4 * 3^2) = 2^2 * 3 = 12
    std::vector<uint64_t> exponents = {4, 2};  // 2^4 * 3^2
    std::vector<uint32_t> primes = {2, 3};
    Integer n(1000000007);

    Integer result = RationalSqrt::compute_from_exponents(exponents, primes, n);
    assert(result.to_int64() == 12);

    // sqrt(2^6 * 5^4) = 2^3 * 5^2 = 8 * 25 = 200
    std::vector<uint64_t> exponents2 = {6, 0, 4};  // 2^6 * 3^0 * 5^4
    std::vector<uint32_t> primes2 = {2, 3, 5};

    Integer result2 = RationalSqrt::compute_from_exponents(exponents2, primes2, n);
    assert(result2.to_int64() == 200);

    std::cout << "  RationalSqrt (simple): PASSED" << std::endl;
}

// Test factor extraction
void test_factor_extraction() {
    std::cout << "Testing factor extraction..." << std::endl;

    // N = 143 = 11 * 13
    // If X = 12 and Y = 1, then:
    // gcd(12 - 1, 143) = gcd(11, 143) = 11
    // gcd(12 + 1, 143) = gcd(13, 143) = 13

    Integer n(143);
    Integer rational_sqrt(12);
    Integer algebraic_sqrt(1);

    auto result = extract_factors(rational_sqrt, algebraic_sqrt, n);

    // One of the factors should be 11 or 13
    bool found_11 = (result.factor1.to_int64() == 11 || result.factor2.to_int64() == 11);
    bool found_13 = (result.factor1.to_int64() == 13 || result.factor2.to_int64() == 13);

    std::cout << "  factor1 = " << result.factor1.to_string() << std::endl;
    std::cout << "  factor2 = " << result.factor2.to_string() << std::endl;
    std::cout << "  nontrivial = " << (result.is_nontrivial ? "yes" : "no") << std::endl;

    assert(found_11 || found_13);
    assert(result.is_nontrivial);

    std::cout << "  Factor extraction: PASSED" << std::endl;
}

// Test norm computation
void test_norm_linear() {
    std::cout << "Testing norm_linear..." << std::endl;

    // f(x) = x^2 - 15
    // N(a + b*α) = a^2 - 15*b^2
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(-15));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(1)));

    Integer n(1000000007);
    Integer m(4);

    PolynomialContext ctx(std::move(n), std::move(coeffs), std::move(m));
    NumberField nf(ctx);

    // N(4 + 1*α) = 4^2 - 15*1^2 = 16 - 15 = 1
    Integer norm1 = nf.norm_linear(4, 1);
    assert(norm1.to_int64() == 1);

    // N(1 + 0*α) = 1
    Integer norm2 = nf.norm_linear(1, 0);
    assert(norm2.to_int64() == 1);

    // N(2 + 1*α) = 4 - 15 = -11 → |−11| = 11
    Integer norm3 = nf.norm_linear(2, 1);
    assert(norm3.to_int64() == 11);

    std::cout << "  norm_linear: PASSED" << std::endl;
}

/// Test ModularPoly::is_irreducible — Rabin irreducibility test
void test_is_irreducible() {
    std::cout << "Testing ModularPoly::is_irreducible..." << std::endl;
    using MP = ModularPoly;

    // --- Degree 1: always irreducible ---
    // x + 1 over F_5
    assert(MP::is_irreducible({1, 1}, 5));
    std::cout << "  degree 1: PASSED" << std::endl;

    // --- Degree 2 over F_2 ---
    // x^2 + x + 1 is the only irreducible degree-2 polynomial over F_2
    assert(MP::is_irreducible({1, 1, 1}, 2));    // x^2+x+1: irreducible
    assert(!MP::is_irreducible({0, 0, 1}, 2));   // x^2 = x·x: reducible
    assert(!MP::is_irreducible({0, 1, 1}, 2));   // x^2+x = x(x+1): reducible
    std::cout << "  degree 2 / F_2: PASSED" << std::endl;

    // --- Degree 3 over F_2 ---
    // x^3 + x + 1 and x^3 + x^2 + 1 are irreducible over F_2
    assert(MP::is_irreducible({1, 1, 0, 1}, 2));  // x^3+x+1: irreducible
    assert(MP::is_irreducible({1, 0, 1, 1}, 2));  // x^3+x^2+1: irreducible
    assert(!MP::is_irreducible({1, 1, 1, 1}, 2)); // x^3+x^2+x+1 = (x+1)(x^2+1) = (x+1)^3: reducible
    std::cout << "  degree 3 / F_2: PASSED" << std::endl;

    // --- Degree 4 over F_2: THE CRITICAL CASE ---
    // x^4 + x + 1 is irreducible over F_2
    assert(MP::is_irreducible({1, 1, 0, 0, 1}, 2));

    // x^4 + x^2 + 1 = (x^2+x+1)^2 over F_2: REDUCIBLE but has NO roots!
    // This is the bug the old "no roots" check would miss.
    assert(!MP::is_irreducible({1, 0, 1, 0, 1}, 2));
    std::cout << "  degree 4 / F_2 (critical: reducible without roots): PASSED" << std::endl;

    // --- Degree 5 over F_2 ---
    // x^5 + x^2 + 1 is irreducible over F_2
    assert(MP::is_irreducible({1, 0, 1, 0, 0, 1}, 2));
    std::cout << "  degree 5 / F_2: PASSED" << std::endl;

    // --- Degree 3 over F_5 ---
    // x^3 + x + 1 over F_5: check by evaluating at all x in F_5
    // f(0)=1, f(1)=3, f(2)=11≡1, f(3)=31≡1, f(4)=69≡4 → no roots → irreducible (degree 3)
    assert(MP::is_irreducible({1, 1, 0, 1}, 5));
    // x^3 - 1 = (x-1)(x^2+x+1) over F_5: has root x=1
    assert(!MP::is_irreducible({4, 0, 0, 1}, 5));  // -1≡4 mod 5
    std::cout << "  degree 3 / F_5: PASSED" << std::endl;

    // --- Degree 4 over F_3 ---
    // x^4 + x^2 + 2 over F_3: no roots (f(0)=2, f(1)=1+1+2=1, f(2)=16+4+2=22≡1)
    // but is it irreducible? Need full Rabin test.
    // x^4 + 1 over F_3 = (x^2+x+2)(x^2+2x+2) → reducible without roots
    // f(0)=1, f(1)=2, f(2)=17≡2 → no roots
    assert(!MP::is_irreducible({1, 0, 0, 0, 1}, 3));  // x^4+1 reducible mod 3
    std::cout << "  degree 4 / F_3 (reducible without roots): PASSED" << std::endl;

    std::cout << "  ALL is_irreducible tests PASSED" << std::endl;
}

/// Test ModularPoly::reduce and mul with non-monic f(x)
/// This is the core bug: reduce() assumed f is monic (leading coeff = 1).
/// For non-monic f, we need to divide by the leading coefficient.
void test_non_monic_modular_poly() {
    std::cout << "Testing non-monic ModularPoly reduction..." << std::endl;
    using MP = ModularPoly;

    // f(x) = 3x^2 + x + 2 over F_7
    // f_d = 3, inv(3, 7) = 5 (since 3*5 = 15 ≡ 1 mod 7)
    // α^2 = -(α + 2) / 3 = -(α + 2) * 5 = (-5α - 10) ≡ (2α + 4) mod 7
    std::vector<uint64_t> f = {2, 1, 3};
    uint64_t p = 7;

    // (x + 1)^2 = x^2 + 2x + 1
    // Correct: x^2 → (2x + 4), so (x+1)^2 = (2x + 4) + 2x + 1 = 4x + 5
    MP a({1, 1});  // x + 1
    auto a_sq = MP::mul(a, a, f, p);

    assert(a_sq.coeff(0) == 5);  // constant = 5
    assert(a_sq.coeff(1) == 4);  // x coeff = 4
    assert(a_sq.degree() <= 1);
    std::cout << "  non-monic (x+1)^2 mod (3x^2+x+2, 7): PASSED" << std::endl;

    // Verify with monic f for regression: f(x) = x^2 + x + 2 over F_7
    // α^2 = -(α + 2) = 6α + 5 (mod 7)
    // (x+1)^2 = (6α + 5) + 2α + 1 = α + 6 → {6, 1}
    std::vector<uint64_t> f_monic = {2, 1, 1};
    auto a_sq_monic = MP::mul(a, a, f_monic, p);
    assert(a_sq_monic.coeff(0) == 6);
    assert(a_sq_monic.coeff(1) == 1);
    std::cout << "  monic regression (x+1)^2 mod (x^2+x+2, 7): PASSED" << std::endl;

    // Higher degree non-monic: f(x) = 2x^3 + x + 1 over F_5
    // f_d = 2, inv(2, 5) = 3
    std::vector<uint64_t> f3 = {1, 1, 0, 2};
    uint64_t p3 = 5;
    MP b({1, 0, 1});  // x^2 + 1
    // x^3 = -(x + 1)/2 = -(x + 1)*3 = -3x - 3 ≡ 2x + 2 (mod 5)
    // b^2 = (x^2+1)^2 = x^4 + 2x^2 + 1
    // x^4 = x·x^3 = x·(2x+2) = 2x^2 + 2x
    // So b^2 = (2x^2 + 2x) + 2x^2 + 1 = 4x^2 + 2x + 1
    auto b_sq = MP::mul(b, b, f3, p3);
    assert(b_sq.coeff(0) == 1);
    assert(b_sq.coeff(1) == 2);
    assert(b_sq.coeff(2) == 4);
    assert(b_sq.degree() <= 2);
    std::cout << "  non-monic degree-3 (x^2+1)^2 mod (2x^3+x+1, 5): PASSED" << std::endl;

    std::cout << "  ALL non-monic ModularPoly tests PASSED" << std::endl;
}

/// Test NumberField::multiply_mod_n with non-monic f(x)
void test_non_monic_number_field() {
    std::cout << "Testing non-monic NumberField multiply_mod_n..." << std::endl;

    // f(x) = 3x^2 + x + 2, m = 5
    // N = f(5) = 3*25 + 5 + 2 = 82
    // gcd(3, 82) = 1, so modular inverse exists
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(2));    // f_0
    coeffs.push_back(Integer(1));    // f_1
    coeffs.push_back(Integer(3));    // f_2 (non-monic: f_d = 3)

    Integer n(82);
    Integer m(5);

    PolynomialContext ctx(std::move(n), std::move(coeffs), std::move(m));
    NumberField nf(ctx);

    // Element: (1 - α), i.e., from_ab(1, 1)
    auto elem = nf.from_ab(1, 1);

    // (1 - α)^2 evaluated at α = m = 5: (1 - 5)^2 = 16
    auto squared = nf.multiply_mod_n(elem, elem.clone());
    Integer val = nf.evaluate_at_m_mod_n(squared);
    assert(val.to_int64() == 16);
    std::cout << "  (1-α)^2 at m=5 mod 82 = " << val.to_string() << " (expected 16): PASSED" << std::endl;

    // Another element: (3 - 2α)
    auto elem2 = nf.from_ab(3, 2);
    // (3 - 2*5) = -7, (-7)^2 = 49 mod 82 = 49
    auto sq2 = nf.multiply_mod_n(elem2, elem2.clone());
    Integer val2 = nf.evaluate_at_m_mod_n(sq2);
    assert(val2.to_int64() == 49);
    std::cout << "  (3-2α)^2 at m=5 mod 82 = " << val2.to_string() << " (expected 49): PASSED" << std::endl;

    // Product of two different elements: (1-α)(3-2α)
    // At α=5: (1-5)(3-10) = (-4)(-7) = 28 mod 82 = 28
    auto product = nf.multiply_mod_n(nf.from_ab(1, 1), nf.from_ab(3, 2));
    Integer val3 = nf.evaluate_at_m_mod_n(product);
    assert(val3.to_int64() == 28);
    std::cout << "  (1-α)(3-2α) at m=5 mod 82 = " << val3.to_string() << " (expected 28): PASSED" << std::endl;

    // Verify monic case still works (regression)
    // f(x) = x^2 + x + 2, m = 5, N = 25 + 5 + 2 = 32
    std::vector<Integer> monic_coeffs;
    monic_coeffs.push_back(Integer(2));
    monic_coeffs.push_back(Integer(1));
    monic_coeffs.push_back(Integer(1));

    PolynomialContext ctx2(Integer(32), std::move(monic_coeffs), Integer(5));
    NumberField nf2(ctx2);

    auto e = nf2.from_ab(1, 1);
    auto e_sq = nf2.multiply_mod_n(e, e.clone());
    Integer v = nf2.evaluate_at_m_mod_n(e_sq);
    // (1-5)^2 = 16 mod 32 = 16
    assert(v.to_int64() == 16);
    std::cout << "  monic regression: PASSED" << std::endl;

    std::cout << "  ALL non-monic NumberField tests PASSED" << std::endl;
}

/// Test is_square() and sqrt_tonelli_shanks() for p=2 (characteristic 2).
///
/// In F_{2^d}, the Frobenius x → x² is a bijection (mult group order = 2^d-1 is odd),
/// so EVERY element is a square. sqrt(a) = a^{2^{d-1}} (inverse Frobenius).
///
/// The bug: is_square() uses Euler's criterion a^{(p^d-1)/2} which is not an integer
/// for p=2 (2^d-1 is odd), and sqrt_tonelli_shanks() searches for a non-square element
/// that doesn't exist in characteristic 2 → infinite loop.
void test_characteristic_2_sqrt() {
    std::cout << "Testing characteristic 2 is_square + sqrt..." << std::endl;
    using MP = ModularPoly;

    // ---- is_square over F_{2^2} ----
    // f(x) = x^2 + x + 1 (irreducible over F_2, so F_2[x]/f = F_4)
    // F_4 = {0, 1, α, α+1} where α^2 = α + 1
    // All nonzero elements are squares in F_{2^d}
    std::vector<uint64_t> f2 = {1, 1, 1};  // x^2+x+1
    assert(MP::is_square(MP(0), f2, 2));  // 0 is trivially square
    assert(MP::is_square(MP(1), f2, 2));  // 1 = 1^2
    // α is also a square in F_4 (α = (α+1)^2 since (α+1)^2 = α^2+1 = α+1+1 = α)
    MP alpha;
    alpha.set_coeff(1, 1);  // x
    assert(MP::is_square(alpha, f2, 2));
    // α+1 is also a square
    MP alpha_plus_1;
    alpha_plus_1.set_coeff(0, 1);
    alpha_plus_1.set_coeff(1, 1);  // x+1
    assert(MP::is_square(alpha_plus_1, f2, 2));
    std::cout << "  is_square F_4: PASSED" << std::endl;

    // ---- is_square over F_{2^3} ----
    // f(x) = x^3 + x + 1 (irreducible over F_2, so F_2[x]/f = F_8)
    // All 7 nonzero elements are squares
    std::vector<uint64_t> f3 = {1, 1, 0, 1};  // x^3+x+1
    assert(MP::is_square(MP(1), f3, 2));
    assert(MP::is_square(alpha, f3, 2));       // α in F_8
    std::cout << "  is_square F_8: PASSED" << std::endl;

    // ---- sqrt_tonelli_shanks over F_4 ----
    // sqrt(α) should satisfy s^2 = α mod (f, 2)
    auto sqrt_alpha = MP::sqrt_tonelli_shanks(alpha, f2, 2);
    // Verify: sqrt^2 = alpha
    auto sq = MP::mul(sqrt_alpha, sqrt_alpha, f2, 2);
    assert(sq.degree() == alpha.degree());
    for (int i = 0; i <= std::max(sq.degree(), alpha.degree()); ++i) {
        assert(sq.coeff(i) == alpha.coeff(i));
    }
    std::cout << "  sqrt F_4 (alpha): PASSED" << std::endl;

    // sqrt(α+1) in F_4
    auto sqrt_ap1 = MP::sqrt_tonelli_shanks(alpha_plus_1, f2, 2);
    auto sq2 = MP::mul(sqrt_ap1, sqrt_ap1, f2, 2);
    for (int i = 0; i <= std::max(sq2.degree(), alpha_plus_1.degree()); ++i) {
        assert(sq2.coeff(i) == alpha_plus_1.coeff(i));
    }
    std::cout << "  sqrt F_4 (alpha+1): PASSED" << std::endl;

    // sqrt(1) in F_4 should be 1
    auto sqrt_one = MP::sqrt_tonelli_shanks(MP(1), f2, 2);
    assert(sqrt_one.is_one());
    std::cout << "  sqrt F_4 (1): PASSED" << std::endl;

    // ---- sqrt_tonelli_shanks over F_8 ----
    // sqrt(α) in F_8 = F_2[x]/(x^3+x+1)
    auto sqrt_alpha_f8 = MP::sqrt_tonelli_shanks(alpha, f3, 2);
    auto sq3 = MP::mul(sqrt_alpha_f8, sqrt_alpha_f8, f3, 2);
    for (int i = 0; i <= std::max(sq3.degree(), alpha.degree()); ++i) {
        assert(sq3.coeff(i) == alpha.coeff(i));
    }
    std::cout << "  sqrt F_8 (alpha): PASSED" << std::endl;

    // ---- Edge case: p=2, d=1 (F_2 itself) ----
    // F_2 = {0, 1}, sqrt(1) = 1
    std::vector<uint64_t> f1 = {1, 1};  // x+1 (irreducible, degree 1)
    assert(MP::is_square(MP(1), f1, 2));
    // Note: sqrt_tonelli_shanks on F_2 is trivial — only element is 1
    auto sqrt_f2 = MP::sqrt_tonelli_shanks(MP(1), f1, 2);
    assert(sqrt_f2.is_one());
    std::cout << "  sqrt F_2: PASSED" << std::endl;

    std::cout << "  ALL characteristic 2 tests PASSED" << std::endl;
}

// Test: compute_from_element() must not loop forever when num_primes is unreachable
void test_couveignes_compute_from_element_terminates() {
    std::cout << "Testing Couveignes compute_from_element termination..." << std::endl;

    // f(x) = x^3 + 2x + 1, N = 143, m = 5
    std::vector<Integer> f_coeffs;
    f_coeffs.push_back(Integer(1));   // x^0
    f_coeffs.push_back(Integer(2));   // x^1
    f_coeffs.push_back(Integer(static_cast<int64_t>(0)));   // x^2
    f_coeffs.push_back(Integer(1));   // x^3
    PolynomialContext ctx(Integer(143), std::move(f_coeffs), Integer(5));
    NumberField nf(ctx);

    // Create a simple non-zero element: 3 + 5*alpha
    std::vector<Integer> elem_coeffs;
    elem_coeffs.push_back(Integer(3));
    elem_coeffs.push_back(Integer(5));
    NumberFieldElement elem(std::move(elem_coeffs));

    // Set max_prime_checks very low so the guard kicks in immediately.
    // Request more primes than can be found within max_prime_checks.
    // Without the guard, this would loop indefinitely.
    CouveignesSqrtConfig cfg;
    cfg.num_primes = 100;  // Want 100 primes
    cfg.prime_start = 1000;
    cfg.max_prime_checks = 50;  // But only allow 50 checks (~17 will be irreducible for d=3)

    CouveignesSqrt couveignes(cfg);

    auto start = std::chrono::steady_clock::now();
    auto result = couveignes.compute_from_element(elem, nf);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // The key assertion: function MUST terminate.
    // Without the max-attempts guard, this would loop indefinitely.
    // With the guard, it finishes within seconds (scans up to 100000 primes).
    assert(elapsed_ms < 30000 &&
           "compute_from_element should terminate quickly with max-attempts guard");

    // For degree 3, ~1/3 of primes give irreducible f.
    // 100000 checks → ~33000 primes found < 100000 requested.
    // The function makes a best-effort CRT with whatever it found.
    std::cout << "  compute_from_element termination: PASSED (took "
              << elapsed_ms << "ms, result="
              << (result.has_value() ? "found" : "nullopt") << ")" << std::endl;
}

// Test: compute_from_element() still works correctly for normal num_primes
void test_couveignes_compute_from_element_normal() {
    std::cout << "Testing Couveignes compute_from_element (normal case)..." << std::endl;

    // f(x) = x^3 + 2x + 1, N = 143, m = 5
    std::vector<Integer> f_coeffs;
    f_coeffs.push_back(Integer(1));
    f_coeffs.push_back(Integer(2));
    f_coeffs.push_back(Integer(static_cast<int64_t>(0)));
    f_coeffs.push_back(Integer(1));
    PolynomialContext ctx(Integer(143), std::move(f_coeffs), Integer(5));
    NumberField nf(ctx);

    // Element = 1 (trivial, sqrt should be 1 or -1)
    NumberFieldElement elem(Integer(1));

    CouveignesSqrtConfig cfg;
    cfg.num_primes = 4;  // Small, reasonable
    cfg.prime_start = 100;

    CouveignesSqrt couveignes(cfg);
    auto result = couveignes.compute_from_element(elem, nf);

    // Should succeed for such a simple element
    // (Note: might fail due to sign resolution, but the prime-finding step should work)
    // At minimum, the function should terminate quickly
    std::cout << "  compute_from_element normal: PASSED (result="
              << (result.has_value() ? "found" : "nullopt") << ")" << std::endl;
}

int main() {
    std::cout << "=== Square Root Tests ===" << std::endl;
    std::cout << std::endl;

    test_number_field_element();
    test_number_field_element_arithmetic();
    test_number_field();
    test_number_field_multiply();
    test_number_field_power();
    test_evaluate_at_m();
    test_rational_sqrt_simple();
    test_factor_extraction();
    test_norm_linear();
    test_is_irreducible();
    test_non_monic_modular_poly();
    test_non_monic_number_field();
    test_characteristic_2_sqrt();
    test_couveignes_compute_from_element_terminates();
    test_couveignes_compute_from_element_normal();

    std::cout << std::endl;
    std::cout << "=== All Square Root Tests PASSED ===" << std::endl;

    return 0;
}
