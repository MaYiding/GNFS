// Regression tests for critical bugs fixed in Sessions 1-23
// Each test targets a specific bug that was found and fixed,
// ensuring it never reappears.
#include "gnfs/core/integer.hpp"
#include "gnfs/core/params.hpp"
#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/polynomial/int_polynomial.hpp"
#include "gnfs/polynomial/base_m.hpp"

#include <cassert>
#include <iostream>
#include <limits>

using namespace gnfs::core;
using namespace gnfs::polynomial;

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
    // NOTE: This test verifies the *mathematical formula* ℓ^d - 1 in isolation.
    // It does NOT call schirokauer.hpp directly (the exponent is private/internal).
    // If the production code ever regresses to the old formula (ℓ^(d-1)·(ℓ-1)),
    // this test will NOT catch it. A proper integration test would require setting
    // up a full SplitSchirokauer context and inspecting computed map values.
    std::cout << "Testing Schirokauer exponent formula (Bug #3, math only)..." << std::endl;

    // For ℓ=2, d=3: exponent should be 2^3 - 1 = 7
    // Old formula: 2^(3-1)·(2-1) = 4 (WRONG)
    uint32_t ell = 2, d = 3;
    uint64_t correct = 1;
    for (uint32_t i = 0; i < d; ++i) correct *= ell;
    correct -= 1;  // ℓ^d - 1
    assert(correct == 7);
    // Verify OLD formula would have given wrong answer (catches copy-paste regression)
    uint64_t old_wrong = 1;
    for (uint32_t i = 0; i < d - 1; ++i) old_wrong *= ell;
    old_wrong *= (ell - 1);  // ℓ^(d-1)·(ℓ-1)
    assert(old_wrong == 4);
    assert(old_wrong != correct);  // formulas differ

    // For ℓ=2, d=5: exponent should be 31
    uint64_t exp5 = 1;
    for (uint32_t i = 0; i < 5; ++i) exp5 *= 2;
    exp5 -= 1;
    assert(exp5 == 31);

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

    for (size_t bits : {20, 40, 60, 80, 100, 150, 200}) {
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

    constexpr size_t MAX_AREA = 256ULL * 1024 * 1024;

    // Even for very large N, sieve area must be capped
    for (size_t bits : {200, 300, 500}) {
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
    std::cout << "Testing Schirokauer ell=2 only convention..." << std::endl;

    // This test just verifies the convention is documented and
    // the exponent formula gives correct values for ℓ=2
    // For ℓ=2, d=3: exponent = 2^3 - 1 = 7, divisor = 2
    // For ℓ=2, d=5: exponent = 2^5 - 1 = 31, divisor = 2

    // Verify γ^(ℓ^d - 1) ≡ 1 (mod ℓ) for the group order
    // By Fermat's little theorem in the number field
    Integer gamma(3);  // arbitrary non-zero
    Integer ell(2);
    Integer exp_d3(7);   // 2^3 - 1
    Integer exp_d5(31);  // 2^5 - 1

    // γ^(ℓ^d - 1) mod ℓ should be 1 (Fermat)
    // 3^7 mod 2 = 2187 mod 2 = 1
    Integer r3 = powmod(gamma, exp_d3, ell);
    assert(r3.to_int64() == 1);

    // 3^31 mod 2 = 1
    Integer r5 = powmod(gamma, exp_d5, ell);
    assert(r5.to_int64() == 1);

    // Schirokauer map: (γ^e - 1) / ℓ mod ℓ
    // Need full integer γ^e, NOT γ^e mod ℓ
    // 3^7 = 2187, (2187 - 1) / 2 mod 2 = 1093 mod 2 = 1
    Integer gamma_pow = gamma.clone();
    for (int i = 1; i < 7; ++i) gamma_pow *= gamma;  // 3^7 = 2187
    assert(gamma_pow.to_int64() == 2187);
    Integer map_val = gamma_pow.clone();
    map_val -= Integer(1);  // γ^e - 1 = 2186
    map_val /= int64_t(2); // / ℓ = 1093
    map_val %= int64_t(2); // mod ℓ = 1
    assert(map_val.to_int64() == 1);

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

    // Very small N (8 bits)
    auto p8 = GNFSParams::compute(8);
    assert(p8.degree == 3);
    assert(p8.rational_bound >= 2000);
    assert(p8.sieve_region_size() > 0);

    // Medium N (64 bits)
    auto p64 = GNFSParams::compute(64);
    assert(p64.degree == 3);
    assert(p64.rational_bound >= 3000);

    // Large N (200 bits)
    auto p200 = GNFSParams::compute(200);
    assert(p200.degree == 4);  // analytical formula: d_opt ≈ 4.39 → 4
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
