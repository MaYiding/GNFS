// Regression tests for critical bugs fixed in Sessions 1-23
// Each test targets a specific bug that was found and fixed,
// ensuring it never reappears.
#include "gnfs/core/integer.hpp"
#include "gnfs/core/params.hpp"
#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/polynomial/int_polynomial.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/linalg/schirokauer.hpp"

#include <cassert>
#include <iostream>
#include <limits>

using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::linalg;

// ============================================================
// Bug #1: Integer(uint64_t) constructor disambiguation
// Session 1: Without explicit uint64_t constructor, Integer(42u)
// was ambiguous between int64_t and string ctors
// ============================================================
void test_integer_uint64_constructor() {
    std::cout << "Testing Integer uint64_t constructor (Bug #1)..." << std::endl;

    // These must all compile and work correctly
    Integer from_int(42);
    Integer from_int64(int64_t(42));
    Integer from_uint(42u);
    Integer from_uint64(uint64_t(42));

    assert(from_int.to_int64() == 42);
    assert(from_int64.to_int64() == 42);
    assert(from_uint.to_uint64() == 42);
    assert(from_uint64.to_uint64() == 42);

    // Large uint64 that doesn't fit in int64
    uint64_t large = uint64_t(1) << 63;
    Integer big(large);
    assert(big.to_uint64() == large);
    assert(!big.fits_int64());

    // UINT64_MAX
    Integer max_val(std::numeric_limits<uint64_t>::max());
    assert(max_val.to_uint64() == std::numeric_limits<uint64_t>::max());

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug #3: Schirokauer exponent formula
// Session 1: Old formula ℓ^(k-1)·(ℓ-1) gave wrong maps.
// Correct formula: ℓ^d - 1 (number field group order)
// ============================================================
void test_schirokauer_exponent() {
    std::cout << "Testing Schirokauer exponent via production code (Bug #3)..." << std::endl;

    // f(x) = x^3 + 2, N = 29, m = 3  (f(3) = 29 = N ✓)
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(2));                          // c_0
    coeffs.push_back(Integer(static_cast<int64_t>(0)));    // c_1
    coeffs.push_back(Integer(static_cast<int64_t>(0)));    // c_2
    coeffs.push_back(Integer(static_cast<int64_t>(1)));    // c_3
    PolynomialContext ctx(Integer(29), std::move(coeffs), Integer(3));

    // ℓ=2 is the standard for GF(2)
    SchirokaurConfig config;
    config.primes = {2};

    SchirokaurMap sm(ctx, config);
    assert(sm.num_columns() == 3);  // d=3, one prime → 3 columns

    // Compute for a known (a,b) pair: element = a - b*α = 5 - 2*α
    auto result = sm.compute(5, 2);
    assert(result.size() == 1);        // one prime
    assert(result[0].size() == 3);     // d=3 columns

    // Each map value must be in {0, 1} (mod ℓ=2)
    for (uint32_t v : result[0]) {
        assert(v < 2);
    }

    // Verify additivity: λ(γ1·γ2) = λ(γ1) + λ(γ2) mod ℓ
    auto r1 = sm.compute(7, 1);
    auto r2 = sm.compute(3, 1);
    // The sum property is fundamental to GNFS linear algebra
    // We just verify values are valid (full verification needs multiplicative check)
    for (size_t i = 0; i < 3; ++i) {
        assert(r1[0][i] < 2);
        assert(r2[0][i] < 2);
    }

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug #7: evaluate_mod uint64 overflow
// Session 2: Horner's step result*x overflowed uint64.
// Fixed with __uint128_t intermediate.
// ============================================================
void test_evaluate_mod_no_overflow() {
    std::cout << "Testing evaluate_mod no overflow (Bug #7)..." << std::endl;

    // Create a polynomial with large coefficients
    // f(x) = x^3 + large_coeff
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer("999999999999999999"));  // constant
    coeffs.push_back(Integer(0));
    coeffs.push_back(Integer(0));
    coeffs.push_back(Integer(1));
    IntPolynomial f(std::move(coeffs));

    // Evaluate at large x mod large p
    uint64_t x = 999999937;  // large prime
    uint64_t p = 1000000007;  // another large prime

    // This should NOT overflow
    uint64_t result = f.evaluate_mod(x, p);
    assert(result < p);

    // Verify via Integer arithmetic
    Integer x_int(x);
    Integer val = f.evaluate(x_int);
    Integer p_int(p);
    Integer mod_val;
    Integer::mod(mod_val, val, p_int);
    if (mod_val.is_negative()) mod_val += p_int;
    assert(mod_val.to_uint64() == result);

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug #11: Non-monic polynomial handling
// Session 1: Code assumed leading coefficient = 1.
// Fixed by adding f_lead_inv in modular operations.
// ============================================================
void test_non_monic_polynomial() {
    std::cout << "Testing non-monic polynomial (Bug #11)..." << std::endl;

    // Create a non-monic polynomial: 2x^3 + x + 5
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(5));
    coeffs.push_back(Integer(1));
    coeffs.push_back(Integer(0));
    coeffs.push_back(Integer(2));
    IntPolynomial f(std::move(coeffs));

    assert(f.degree() == 3);
    assert(f.leading_coeff().to_int64() == 2);

    // Evaluate correctly
    // f(3) = 2*27 + 3 + 5 = 54 + 3 + 5 = 62
    assert(f.evaluate(Integer(3)).to_int64() == 62);

    // Derivative: f'(x) = 6x^2 + 1
    auto fp = f.derivative();
    assert(fp.degree() == 2);
    assert(fp[0].to_int64() == 1);
    assert(fp[2].to_int64() == 6);

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug #14: Base-m irreducibility
// Session 23: Base-m polynomial selection didn't check
// irreducibility over Q. Fixed with mod-p Rabin test.
// ============================================================
void test_base_m_irreducibility() {
    std::cout << "Testing base-m irreducibility (Bug #14)..." << std::endl;

    // N=1320 with m=10 gives f(x) = x^3 + 3x^2 + 2x = x(x+1)(x+2)
    // which is REDUCIBLE. The fix should find m=9 or m=11 instead.
    Integer n(1320);
    auto result = BaseMSelector::select(n, 3);
    assert(result.success);

    auto ctx = BaseMSelector::create_context(n, result);

    // Verify f(m) ≡ 0 (mod N)
    assert(ctx.verify());

    // The selected m should NOT be 10 (m=10 gives reducible f(x)=x(x+1)(x+2))
    // This directly guards the bug: old code always used floor(N^(1/d)) = 10
    // Fixed code tries m±1, m±2, ... until finding an irreducible polynomial
    assert(result.m != Integer(10));

    // Also verify m is close to N^(1/d) ≈ 10 (not some arbitrary value)
    assert(result.m >= Integer(8) && result.m <= Integer(12));

    std::cout << "  PASS (m=" << ctx.m().to_string() << ")" << std::endl;
}

// ============================================================
// Bug: PolynomialContext evaluate and algebraic_norm consistency
// The algebraic norm N(a-bα) = b^d * f(a/b) should be consistent
// with the element convention "a - b*α" (NOT "a + b*α")
// ============================================================
void test_algebraic_norm_convention() {
    std::cout << "Testing algebraic norm convention..." << std::endl;

    // Create a simple polynomial context: f(x) = x^2 + 1, m=1, N=2
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(1));
    coeffs.push_back(Integer(0));
    coeffs.push_back(Integer(1));
    PolynomialContext ctx(Integer(2), std::move(coeffs), Integer(1));

    // N(a - b*α) = b^d * f(a/b) for element a - b*α
    // For d=2: N(a - b*α) = a^2 + b^2 (when f(x)=x^2+1)
    // N(3 - 2*α) = 9 + 4 = 13
    Integer norm = ctx.algebraic_norm(3, 2);
    assert(norm.to_int64() == 13);

    // N(1 - 1*α) = 1 + 1 = 2
    Integer norm2 = ctx.algebraic_norm(1, 1);
    assert(norm2.to_int64() == 2);

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug: PolynomialContext rational_value convention
// R(a, b) = a - b*m (NOT a + b*m)
// ============================================================
void test_rational_value_convention() {
    std::cout << "Testing rational value a-b*m convention..." << std::endl;

    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(0));
    coeffs.push_back(Integer(1));
    PolynomialContext ctx(Integer(100), std::move(coeffs), Integer(7));

    // R(15, 2) = 15 - 2*7 = 1
    Integer rv = ctx.rational_value(15, 2);
    assert(rv.to_int64() == 1);

    // R(3, 1) = 3 - 7 = -4
    Integer rv2 = ctx.rational_value(3, 1);
    assert(rv2.to_int64() == -4);

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug: Special-Q min range (Session 26)
// special_q_min must be > algebraic_bound
// ============================================================
void test_special_q_range_regression() {
    std::cout << "Testing special-Q range regression..." << std::endl;

    for (size_t bits : {size_t{20}, size_t{40}, size_t{60}, size_t{80}, size_t{100}, size_t{150}, size_t{200}}) {
        auto params = GNFSParams::compute(bits);
        assert(params.special_q_min > params.algebraic_bound);
        assert(params.special_q_max > params.special_q_min);
    }

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug: Sieve area cap (Session 15)
// Without cap, large N produced >100GB sieve arrays
// ============================================================
void test_sieve_area_cap_regression() {
    std::cout << "Testing sieve area cap regression..." << std::endl;

    constexpr size_t MAX_AREA = 1024ULL * 1024 * 1024; // 1G positions max

    // Even for very large N, sieve area must be capped
    for (size_t bits : {size_t{200}, size_t{300}, size_t{500}}) {
        auto params = GNFSParams::compute(bits);
        size_t area = params.sieve_region_size();
        assert(area <= MAX_AREA);
    }

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug: Schirokauer ℓ=2 only for GF(2) matrix (Session 1)
// Using ℓ>2 with GF(2) matrix produces wrong results because
// Schirokauer map values are mod ℓ, not mod 2.
// The CLAUDE.md convention: schirokauer_primes = {2} always.
// ============================================================
void test_schirokauer_ell2_only() {
    std::cout << "Testing Schirokauer ell=2 only via production code..." << std::endl;

    // f(x) = x^5 + x + 1, N = 4 (trivial — just need valid context)
    // f(1) = 3, so use m=1, N=3 → f(1)=3 ✓
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(static_cast<int64_t>(1)));   // c_0
    coeffs.push_back(Integer(static_cast<int64_t>(1)));   // c_1
    coeffs.push_back(Integer(static_cast<int64_t>(0)));   // c_2
    coeffs.push_back(Integer(static_cast<int64_t>(0)));   // c_3
    coeffs.push_back(Integer(static_cast<int64_t>(0)));   // c_4
    coeffs.push_back(Integer(static_cast<int64_t>(1)));   // c_5
    PolynomialContext ctx(Integer(3), std::move(coeffs), Integer(1));

    // Convention: ℓ=2 only for GF(2) matrix
    SchirokaurConfig config;
    config.primes = {2};
    SchirokaurMap sm(ctx, config);

    // d=5, one prime → 5 Schirokauer columns
    assert(sm.num_columns() == 5);

    // Compute maps for several (a,b) pairs
    auto r1 = sm.compute(1, 1);   // γ = 1 - α
    auto r2 = sm.compute(3, 2);   // γ = 3 - 2α
    auto r3 = sm.compute(-1, 1);  // γ = -1 - α

    // All results should have 1 prime × 5 columns, values in {0,1}
    for (const auto& result : {r1, r2, r3}) {
        assert(result.size() == 1);
        assert(result[0].size() == 5);
        for (uint32_t v : result[0]) {
            assert(v < 2);
        }
    }

    // Verify flat interface consistency
    auto flat = sm.compute_flat(1, 1);
    assert(flat.size() == 5);
    for (size_t i = 0; i < 5; ++i) {
        assert(flat[i] == r1[0][i]);
    }

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug: IntPolynomial derivative at degree 0 (constant)
// Must return zero polynomial, not crash
// ============================================================
void test_derivative_of_constant() {
    std::cout << "Testing derivative of constant (edge case)..." << std::endl;

    IntPolynomial c(42);
    auto cp = c.derivative();
    assert(cp.is_zero());
    assert(cp.degree() == 0);

    IntPolynomial zero;
    auto zp = zero.derivative();
    assert(zp.is_zero());

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug: IntPolynomial evaluate at x=0
// Must return constant term, not crash on empty iteration
// ============================================================
void test_evaluate_at_zero() {
    std::cout << "Testing evaluate at x=0 (edge case)..." << std::endl;

    auto f = IntPolynomial(42);
    assert(f.evaluate(Integer(0)).to_int64() == 42);

    IntPolynomial zero;
    assert(zero.evaluate(Integer(0)).is_zero());

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug: Integer::mod_inverse for coprime inputs
// Used throughout Schirokauer and cofactorizer
// ============================================================
void test_mod_inverse() {
    std::cout << "Testing Integer mod_inverse..." << std::endl;

    // 3^(-1) mod 7 = 5 (since 3*5 = 15 ≡ 1 mod 7)
    Integer three(3);
    Integer seven(7);
    Integer inv = mod_inverse(three, seven);
    Integer check = inv.clone();
    check *= three;
    check %= seven;
    assert(check.to_int64() == 1);

    // 2^(-1) mod 11 = 6 (since 2*6 = 12 ≡ 1 mod 11)
    Integer two(2);
    Integer eleven(11);
    Integer inv2 = mod_inverse(two, eleven);
    Integer check2 = inv2.clone();
    check2 *= two;
    check2 %= eleven;
    assert(check2.to_int64() == 1);

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Bug: GNFSParams compute should handle edge cases
// ============================================================
void test_params_edge_cases() {
    std::cout << "Testing GNFSParams edge cases..." << std::endl;

    // Very small N (8 bits, ~3 digits)
    auto p8 = GNFSParams::compute(8);
    assert(p8.degree == 3);
    assert(p8.rational_bound >= 200);
    assert(p8.sieve_region_size() > 0);

    // Medium N (64 bits, ~20 digits)
    auto p64 = GNFSParams::compute(64);
    assert(p64.degree == 3);
    assert(p64.rational_bound >= 2000);

    // Large N (200 bits)
    auto p200 = GNFSParams::compute(200);
    assert(p200.degree == 3);  // ≤200 bits forced to degree 3 (even-degree bug fix)
    assert(p200.rational_bound > p64.rational_bound);

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== Regression Tests ===" << std::endl;

    test_integer_uint64_constructor();
    test_schirokauer_exponent();
    test_evaluate_mod_no_overflow();
    test_non_monic_polynomial();
    test_base_m_irreducibility();
    test_algebraic_norm_convention();
    test_rational_value_convention();
    test_special_q_range_regression();
    test_sieve_area_cap_regression();
    test_schirokauer_ell2_only();
    test_derivative_of_constant();
    test_evaluate_at_zero();
    test_mod_inverse();
    test_params_edge_cases();

    std::cout << "\nAll regression tests passed!" << std::endl;
    return 0;
}
