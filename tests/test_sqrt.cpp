// test_sqrt.cpp - Test square root components

#include <gnfs/sqrt/number_field.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/core/polynomial_context.hpp>

#include <cassert>
#include <iostream>
#include <vector>

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

    std::cout << std::endl;
    std::cout << "=== All Square Root Tests PASSED ===" << std::endl;

    return 0;
}
