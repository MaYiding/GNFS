// test_schirokauer_precision.cpp — Schirokauer columns against exact
// references at the production precision
//
// When f has a linear factor modulo 2, SchirokaurMap takes the split path and
// computes the column of that factor from γ = a - b·r modulo 2^k, where r is
// the root of the factor lifted modulo 2^k. The column is bit v+1 of the
// integer a - b·r, with v = v₂(a - b·r). The lift knows γ only modulo 2^k, so
// the map returns that bit while v + 2 ≤ k and zero columns otherwise. The
// latter is a precision fallback: the row then carries a wrong constraint
// whenever the true bit is 1.
//
// Two references share no code with the map, and both use exact Integer
// values.
//
//   Linear factor. The root is lifted to modulo 2^64 by Newton iteration,
//   a - b·r is formed exactly, and bit v+1 is read directly. A negative
//   integer is read in the 2-adic sense, as an infinite two's complement,
//   which is what Integer::test_bit implements.
//
//   Unsplit path and factors of degree two or more. Multiplication by α in
//   O/(f_i) is the companion matrix C of the monic modulus f_i, γ = a - bα
//   acts as aI - bC, and the coefficient vector of γ^e is the first column of
//   (aI - bC)^e. The matrix power is taken over exact integers, and the
//   extraction copies the production convention. For a split polynomial the
//   moduli are the lifted factors of the map. The test certifies them first:
//   each reduces to its irreducible factor modulo 2, and their product is the
//   normalized polynomial modulo 2^k.
//
// Fixtures. Only the reduction modulo 2 matters to the map.
//   L  x^3 + 148x^2 + 24945452x + 19508769, m = 123179884:
//      f ≡ x^3 + 1 = (x + 1)(x^2 + x + 1) (mod 2)
//   U  3x^3 + 55x^2 + 14x + 71, m = 316:
//      f ≡ x^3 + x^2 + 1 (mod 2), irreducible, so the unsplit path
//   C  x^4 + 3x^3 + x^2 + 5, m = 10:
//      f ≡ (x + 1)(x^3 + x + 1) (mod 2), a cubic factor
// L is the polynomial that the default selector picks for the 25-digit
// semiprime N = 1869045589843040925386929 = 1309931996651 × 1426826426579,
// with f(m) = N. tests/test_schirokauer_precision_e2e.cpp factors N with it.

#include <gnfs/core/integer.hpp>
#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/core/relation.hpp>
#include <gnfs/factor_base/factor_base.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/schirokauer.hpp>

#include "support/test_assert.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::linalg::SchirokaurConfig;
using gnfs::linalg::SchirokaurMap;
using gnfs::test::run_case;

namespace {

// The default exponent. A pair with valuation v keeps its exact column while
// v + 2 ≤ k, so the default is exact up to v = 38 and falls back from v = 39.
constexpr uint32_t kDefaultExponent = 40;
constexpr uint32_t kMaxExactValuation = kDefaultExponent - 2;
constexpr uint32_t kFirstFallbackValuation = kDefaultExponent - 1;
// The previous default. Its fallbacks start at v = 7.
constexpr uint32_t kOldExponent = 8;
constexpr uint32_t kReferenceBits = 64;

struct Pair {
    int64_t a;
    uint64_t b;
};

// The exact column of the linear factor for one pair.
struct Reference {
    uint32_t valuation; // v = v₂(a - b·r)
    uint32_t column;    // bit v+1 of a - b·r
};

// A polynomial whose map the test compares with the references.
struct Fixture {
    const char* label;
    std::vector<int64_t> coefficients; // low degree first
    const char* n;                     // f(m) = N
    int64_t m;
    // The irreducible factors of f modulo 2, by degree, low coefficient first.
    std::vector<std::vector<int64_t>> factors_mod_2;
    // The linear factor is x + 1, so Newton's iteration lifts the root from 1
    // modulo 2. False when f has no linear factor.
    bool linear_root_is_one;
};

const Fixture kLinearQuadratic{
    "L: x^3 + 148x^2 + 24945452x + 19508769 = (x + 1)(x^2 + x + 1) modulo 2",
    {19508769, 24945452, 148, 1},
    "1869045589843040925386929",
    123179884,
    {{1, 1}, {1, 1, 1}},
    true};
const Fixture kUnsplit{"U: 3x^3 + 55x^2 + 14x + 71 = x^3 + x^2 + 1 modulo 2, unsplit",
                       {71, 14, 55, 3},
                       "100160063",
                       316,
                       {{1, 0, 1, 1}},
                       false};
const Fixture kLinearCubic{"C: x^4 + 3x^3 + x^2 + 5 = (x + 1)(x^3 + x + 1) modulo 2",
                           {5, 0, 1, 3, 1},
                           "13105",
                           10,
                           {{1, 1}, {1, 1, 0, 1}},
                           true};

std::vector<const Fixture*> fixtures() {
    return {&kLinearQuadratic, &kUnsplit, &kLinearCubic};
}

PolynomialContext make_context(const Fixture& fixture) {
    std::vector<Integer> coefficients;
    coefficients.reserve(fixture.coefficients.size());
    for (const int64_t value : fixture.coefficients)
        coefficients.emplace_back(value);
    PolynomialContext ctx(Integer(fixture.n), std::move(coefficients), Integer(fixture.m));
    TEST_ASSERT(ctx.degree() == fixture.coefficients.size() - 1);
    TEST_ASSERT(ctx.verify());
    return ctx;
}

// f(x) at an exact integer argument, by Horner's rule from the top coefficient.
Integer evaluate(const std::vector<int64_t>& coefficients, const Integer& x) {
    Integer result(0);
    for (size_t i = coefficients.size(); i-- > 0;) {
        result *= x;
        result += coefficients[i];
    }
    return result;
}

// f'(x) at an exact integer argument.
Integer evaluate_derivative(const std::vector<int64_t>& coefficients, const Integer& x) {
    Integer result(0);
    for (size_t i = coefficients.size() - 1; i-- > 0;) {
        result *= x;
        result += static_cast<int64_t>(i + 1) * coefficients[i + 1];
    }
    return result;
}

// Non-negative residue. Integer division truncates, so a negative input needs
// the correction.
Integer residue(const Integer& value, const Integer& modulus) {
    Integer result = value % modulus;
    if (result.is_negative())
        result += modulus;
    return result;
}

Integer power_of_two(uint32_t exponent) {
    return gnfs::core::pow(Integer(2), exponent);
}

// Lift the root r ≡ 1 (mod 2) of f to modulo 2^bits by Newton iteration:
// r ← r - f(r) / f'(r) (mod 2^(2·precision)). f'(r) stays odd because f'(1)
// is odd and r ≡ 1 (mod 2), so every step doubles the precision.
Integer lift_root(const std::vector<int64_t>& coefficients, uint32_t bits) {
    Integer root(1);
    TEST_ASSERT(evaluate(coefficients, root).is_even());
    TEST_ASSERT(evaluate_derivative(coefficients, root).is_odd());
    uint32_t precision = 1;
    while (precision < bits) {
        precision = std::min<uint32_t>(2 * precision, bits);
        const Integer modulus = power_of_two(precision);
        const Integer value = residue(evaluate(coefficients, root), modulus);
        const Integer derivative = residue(evaluate_derivative(coefficients, root), modulus);
        const Integer inverse = gnfs::core::mod_inverse(derivative, modulus);
        TEST_ASSERT(!inverse.is_zero());
        Integer correction = value * inverse;
        correction %= modulus;
        root -= correction;
        root = residue(root, modulus);
    }
    TEST_ASSERT(residue(evaluate(coefficients, root), power_of_two(bits)).is_zero());
    return root;
}

// Bit v+1 of a - b·r after dividing out the 2-part of valuation v. For a
// negative unit the bit is that of its two's complement, so the result equals
// ((u - 1) / 2) mod 2 for u = (a - b·r) / 2^v in every sign case.
Reference reference_column(int64_t a, uint64_t b, const Integer& root) {
    Integer gamma = Integer(a) - Integer(b) * root;
    TEST_ASSERT(!gamma.is_zero());
    uint32_t valuation = 0;
    while (gamma.is_even()) {
        gamma /= static_cast<int64_t>(2);
        ++valuation;
    }
    return {valuation, gamma.test_bit(1) ? 1u : 0u};
}

// Pairs that cover v = 0 to kMaxExactValuation with both column values of the
// linear factor: the range in which the default exponent is exact. For each v
// and each t, the residue a of r + (1 + 2t)·2^v modulo 2^(v+2) gives
// v₂(a - r) = v and column t with b = 1. The same a as a negative number and
// the analogue with b = 3 repeat the pair. Even b with odd a are units and
// add the v = 0 cases that the linear path meets in production. No pair has a
// and b both even.
std::vector<Pair> valuation_pairs(const Integer& root) {
    std::vector<Pair> pairs;
    for (uint32_t v = 0; v <= kMaxExactValuation; ++v) {
        const Integer modulus = power_of_two(v + 2);
        const int64_t signed_modulus = static_cast<int64_t>(1) << (v + 2);
        for (int64_t t = 0; t <= 1; ++t) {
            const Integer offset = Integer(1 + 2 * t) * power_of_two(v);
            const Integer a = residue(root + offset, modulus);
            const Integer a_times_three = residue(Integer(3) * root + offset, modulus);
            TEST_ASSERT(a.fits_int64());
            TEST_ASSERT(a_times_three.fits_int64());
            const int64_t signed_a = a.to_int64();
            const int64_t signed_a_times_three = a_times_three.to_int64();
            const Pair added[] = {
                {signed_a, 1}, {signed_a - signed_modulus, 1}, {signed_a_times_three, 3}};
            for (const Pair& pair : added) {
                const Reference reference = reference_column(pair.a, pair.b, root);
                TEST_ASSERT(reference.valuation == v);
                TEST_ASSERT(reference.column == static_cast<uint32_t>(t));
                pairs.push_back(pair);
            }
        }
    }
    const int64_t odd_a[] = {1, 3, 5, -7, (static_cast<int64_t>(1) << 20) + 1};
    const uint64_t even_b[] = {2, 4, 6, uint64_t{1} << 30};
    for (const int64_t a : odd_a) {
        for (const uint64_t b : even_b) {
            pairs.push_back({a, b});
            const Reference reference = reference_column(a, b, root);
            TEST_ASSERT(reference.valuation == 0);
        }
    }
    return pairs;
}

// A pair with the valuation and the column that the linear factor must have.
struct ValuedPair {
    Pair pair;
    uint32_t valuation;
    uint32_t column;
};

// Pairs with v = kFirstFallbackValuation and the two valuations above it, in
// both column values and in the three variants of valuation_pairs. The default
// exponent cannot compute the linear column of any of them.
std::vector<ValuedPair> fallback_pairs(const Integer& root) {
    std::vector<ValuedPair> pairs;
    for (uint32_t v = kFirstFallbackValuation; v < kFirstFallbackValuation + 3; ++v) {
        const Integer modulus = power_of_two(v + 2);
        const int64_t signed_modulus = static_cast<int64_t>(1) << (v + 2);
        for (int64_t t = 0; t <= 1; ++t) {
            const Integer offset = Integer(1 + 2 * t) * power_of_two(v);
            const Integer a = residue(root + offset, modulus);
            const Integer a_times_three = residue(Integer(3) * root + offset, modulus);
            TEST_ASSERT(a.fits_int64());
            TEST_ASSERT(a_times_three.fits_int64());
            const Pair added[] = {
                {a.to_int64(), 1},
                {a.to_int64() - signed_modulus, 1},
                {a_times_three.to_int64(), 3},
            };
            for (const Pair& pair : added) {
                const Reference reference = reference_column(pair.a, pair.b, root);
                TEST_ASSERT(reference.valuation == v);
                TEST_ASSERT(reference.column == static_cast<uint32_t>(t));
                pairs.push_back({pair, v, static_cast<uint32_t>(t)});
            }
        }
    }
    return pairs;
}

// Pairs for the unsplit fixture: the rows of the companion-matrix cases in
// tests/test_linalg.cpp, then a sweep. No pair has a and b both even, so the
// unsplit path never strips.
std::vector<Pair> unsplit_pairs() {
    const Pair rows[] = {
        {1, 2},     {3, 2},       {-5, 2},
        {7, 16},    {-33, 64},    {100, 13},
        {255, 256}, {-1000, 999}, {1000003, 1000034},
        {0, 1},     {5, 0},       {-1, 2},
        {-60, 1},   {-60, 3},     {-58, 3},
        {-196, 1},
    };
    std::vector<Pair> pairs;
    for (const Pair& row : rows)
        pairs.push_back(row);
    for (int64_t i = 1; i < 60; ++i) {
        pairs.push_back({2 * i + 1, static_cast<uint64_t>(3 * i + 1)});
        pairs.push_back({-(2 * i + 1), static_cast<uint64_t>(5 * i + 2)});
        pairs.push_back({2 * i + 1, static_cast<uint64_t>(6 * i + 2)});
    }
    return pairs;
}

// Exact arithmetic on non-negative integers below `modulus`.
Integer add_mod(const Integer& a, const Integer& b, const Integer& modulus) {
    Integer sum = a + b;
    sum %= modulus;
    return sum;
}

Integer mul_mod(const Integer& a, const Integer& b, const Integer& modulus) {
    Integer product = a * b;
    product %= modulus;
    return product;
}

// The monic polynomial f / f_d modulo 2^k, low coefficient first: the modulus
// that precompute_for_prime builds from an odd leading coefficient.
std::vector<Integer> normalized_modulus(const std::vector<int64_t>& coefficients,
                                        uint32_t exponent_k) {
    const Integer modulus = power_of_two(exponent_k);
    const Integer lead_inverse =
        gnfs::core::mod_inverse(residue(Integer(coefficients.back()), modulus), modulus);
    TEST_ASSERT(!lead_inverse.is_zero());
    std::vector<Integer> monic;
    monic.reserve(coefficients.size());
    for (const int64_t value : coefficients) {
        monic.push_back(
            residue(mul_mod(residue(Integer(value), modulus), lead_inverse, modulus), modulus));
    }
    return monic;
}

// Product of two polynomials modulo `modulus`, low coefficient first.
std::vector<Integer> polynomial_product_mod(const std::vector<Integer>& lhs,
                                            const std::vector<Integer>& rhs,
                                            const Integer& modulus) {
    std::vector<Integer> product(lhs.size() + rhs.size() - 1, Integer(0));
    for (size_t i = 0; i < lhs.size(); ++i) {
        for (size_t j = 0; j < rhs.size(); ++j) {
            product[i + j] = add_mod(product[i + j], mul_mod(lhs[i], rhs[j], modulus), modulus);
        }
    }
    return product;
}

using Matrix = std::vector<std::vector<Integer>>;

Matrix identity_matrix(size_t degree) {
    Matrix result(degree, std::vector<Integer>(degree, Integer(0)));
    for (size_t i = 0; i < degree; ++i)
        result[i][i] = Integer(1);
    return result;
}

Matrix multiply_mod(const Matrix& lhs, const Matrix& rhs, const Integer& modulus) {
    const size_t degree = lhs.size();
    Matrix product(degree, std::vector<Integer>(degree, Integer(0)));
    for (size_t i = 0; i < degree; ++i) {
        for (size_t t = 0; t < degree; ++t) {
            for (size_t j = 0; j < degree; ++j) {
                const Integer term = mul_mod(lhs[i][t], rhs[t][j], modulus);
                product[i][j] = add_mod(product[i][j], term, modulus);
            }
        }
    }
    return product;
}

// The exact columns of one monic modulus `monic` (low coefficient first) for
// the unit γ = a - bα: (aI - bC)^(2^d - 1) is taken with the companion matrix
// C of multiplication by α, and coefficient i of the result gives
// ((c_i - δ_i0) / 2) mod 2, as the production extraction does. γ is a unit,
// which holds unless a and b are both even.
std::vector<uint32_t> companion_columns(const std::vector<Integer>& monic, uint32_t exponent_k,
                                        int64_t a, uint64_t b) {
    const Integer modulus = power_of_two(exponent_k);
    const size_t degree = monic.size() - 1;
    TEST_ASSERT(monic.back().is_one());

    Matrix companion(degree, std::vector<Integer>(degree, Integer(0)));
    for (size_t j = 0; j + 1 < degree; ++j)
        companion[j + 1][j] = Integer(1);
    for (size_t i = 0; i < degree; ++i)
        companion[i][degree - 1] = residue(-monic[i], modulus);

    // aI - bC, written with the non-negative residue of -b.
    const Integer a_residue = residue(Integer(a), modulus);
    const Integer neg_b = residue(-Integer(b), modulus);
    Matrix gamma(degree, std::vector<Integer>(degree, Integer(0)));
    for (size_t i = 0; i < degree; ++i) {
        for (size_t j = 0; j < degree; ++j)
            gamma[i][j] = mul_mod(neg_b, companion[i][j], modulus);
        gamma[i][i] = add_mod(gamma[i][i], a_residue, modulus);
    }

    Matrix power = identity_matrix(degree);
    Matrix base = gamma;
    uint64_t remaining = (uint64_t{1} << degree) - 1;
    while (remaining > 0) {
        if ((remaining & 1u) != 0)
            power = multiply_mod(power, base, modulus);
        base = multiply_mod(base, base, modulus);
        remaining >>= 1;
    }

    std::vector<uint32_t> columns;
    columns.reserve(degree);
    for (size_t i = 0; i < degree; ++i) {
        Integer coeff = power[i][0];
        if (i == 0) {
            if (coeff.is_zero())
                coeff = modulus - Integer(1);
            else
                coeff -= Integer(1);
        }
        // The column is the parity of (c_i - δ_i0) / 2, not its whole value.
        coeff /= static_cast<int64_t>(2);
        columns.push_back(coeff.is_odd() ? 1u : 0u);
    }
    return columns;
}

// The monic moduli of the map for one fixture at exponent `exponent_k`,
// certified by exact arithmetic. For a split polynomial they are the lifted
// factors: each must reduce to its irreducible factor modulo 2, and their
// product must be the normalized polynomial. For an unsplit polynomial the
// one modulus is the normalized polynomial.
std::vector<std::vector<Integer>> certified_moduli(const SchirokaurMap& map, const Fixture& fixture,
                                                   uint32_t exponent_k) {
    const Integer modulus = power_of_two(exponent_k);
    const std::vector<Integer> normalized = normalized_modulus(fixture.coefficients, exponent_k);
    TEST_ASSERT(map.prime_info_.size() == 1);
    const auto& info = map.prime_info_[0];
    TEST_ASSERT(info.ell == 2);
    TEST_ASSERT(info.ell_k == modulus.to_uint64());
    for (size_t i = 0; i < normalized.size(); ++i)
        TEST_ASSERT(info.f_mod[i] == normalized[i].to_uint64());

    std::vector<std::vector<Integer>> moduli;
    if (fixture.factors_mod_2.size() == 1) {
        TEST_ASSERT(!info.is_split);
        moduli.push_back(normalized);
        return moduli;
    }
    TEST_ASSERT(info.is_split);
    TEST_ASSERT(info.factors.size() == fixture.factors_mod_2.size());
    for (size_t i = 0; i < info.factors.size(); ++i) {
        const auto& factor = info.factors[i];
        const std::vector<int64_t>& expected = fixture.factors_mod_2[i];
        TEST_ASSERT(factor.degree == expected.size() - 1);
        TEST_ASSERT(factor.f_mod[factor.degree] == 1);
        TEST_ASSERT(factor.exponent == (uint64_t{1} << factor.degree) - 1);
        std::vector<Integer> lifted;
        for (size_t j = 0; j <= factor.degree; ++j) {
            TEST_ASSERT(factor.f_mod[j] < modulus.to_uint64());
            TEST_ASSERT(factor.f_mod[j] % 2 == static_cast<uint64_t>(expected[j] % 2));
            lifted.emplace_back(factor.f_mod[j]);
        }
        moduli.push_back(std::move(lifted));
    }
    std::vector<Integer> product{Integer(1)};
    for (const std::vector<Integer>& factor : moduli)
        product = polynomial_product_mod(product, factor, modulus);
    TEST_ASSERT(product.size() == normalized.size());
    for (size_t i = 0; i < normalized.size(); ++i)
        TEST_ASSERT(product[i] == normalized[i]);
    return moduli;
}

// The exact columns and fallback count of one pair at exponent `exponent_k`.
struct ReferenceRow {
    std::vector<uint32_t> columns;
    uint64_t fallbacks = 0;
};

ReferenceRow reference_row(const std::vector<std::vector<Integer>>& moduli, const Integer& root,
                           uint32_t exponent_k, const Pair& pair) {
    ReferenceRow row;
    for (const std::vector<Integer>& monic : moduli) {
        if (monic.size() == 2) {
            // Linear factor: bit v+1 of a - b·r, and a fallback when v + 2 > k.
            const Reference reference = reference_column(pair.a, pair.b, root);
            const bool fallback = reference.valuation + 2 > exponent_k;
            row.columns.push_back(fallback ? 0u : reference.column);
            row.fallbacks += fallback ? 1u : 0u;
        } else {
            // No stripping: a and b are not both even.
            TEST_ASSERT(!(pair.a % 2 == 0 && pair.b % 2 == 0));
            const std::vector<uint32_t> columns =
                companion_columns(monic, exponent_k, pair.a, pair.b);
            row.columns.insert(row.columns.end(), columns.begin(), columns.end());
        }
    }
    return row;
}

// The pair scaled by 2^s.
Pair scaled_pair(const Pair& pair, uint32_t s) {
    const int64_t scale = static_cast<int64_t>(1) << s;
    return {pair.a * scale, pair.b * static_cast<uint64_t>(scale)};
}

// Compare the map of fixture L at one exponent with the linear-factor
// reference on every pair. Returns the number of fallbacks through the out
// parameters, and how many of those have reference column 1.
void check_linear_factor(uint32_t exponent_k, size_t& fallbacks, size_t& fallback_ones) {
    const PolynomialContext ctx = make_context(kLinearQuadratic);
    const Integer root = lift_root(kLinearQuadratic.coefficients, kReferenceBits);
    SchirokaurConfig config;
    config.exponent_k = exponent_k;
    const SchirokaurMap map(ctx, config);
    TEST_ASSERT(map.num_columns() == ctx.degree());
    TEST_ASSERT(map.prime_info_.size() == 1);
    const auto& info = map.prime_info_[0];
    TEST_ASSERT(info.ell == 2);
    TEST_ASSERT(info.ell_k == (uint64_t{1} << exponent_k));
    TEST_ASSERT(info.is_split);
    // Factors are sorted by degree, so the linear factor owns column 0.
    TEST_ASSERT(info.factors.size() == 2);
    TEST_ASSERT(info.factors[0].degree == 1);
    TEST_ASSERT(info.factors[1].degree == 2);

    fallbacks = 0;
    fallback_ones = 0;
    for (const Pair& pair : valuation_pairs(root)) {
        const Reference reference = reference_column(pair.a, pair.b, root);
        uint64_t counted = 0;
        const auto columns = map.compute_flat(pair.a, pair.b, counted);
        TEST_ASSERT(columns.size() == ctx.degree());
        for (const uint32_t value : columns)
            TEST_ASSERT(value < 2);
        // The two-argument overload returns the same values.
        TEST_ASSERT(map.compute_flat(pair.a, pair.b) == columns);

        if (reference.valuation + 2 > exponent_k) {
            TEST_ASSERT(counted == 1);
            TEST_ASSERT(columns[0] == 0);
            ++fallbacks;
            fallback_ones += reference.column;
        } else {
            TEST_ASSERT(counted == 0);
            TEST_ASSERT(columns[0] == reference.column);
        }
    }
    std::cout << "  k = " << exponent_k << ": fallbacks " << fallbacks << ", reference column 1 "
              << "for " << fallback_ones << " of them\n";
}

void test_default_exponent_matches_reference() {
    std::cout << "Default exponent against the linear-factor reference...\n";
    size_t fallbacks = 0;
    size_t fallback_ones = 0;
    check_linear_factor(kDefaultExponent, fallbacks, fallback_ones);
    TEST_ASSERT(fallbacks == 0);
    TEST_ASSERT(SchirokaurConfig{}.exponent_k == kDefaultExponent);
    std::cout << "  default exponent: PASSED\n";
}

void test_old_exponent_falls_back() {
    std::cout << "Old exponent k = 8 against the linear-factor reference...\n";
    size_t fallbacks = 0;
    size_t fallback_ones = 0;
    check_linear_factor(kOldExponent, fallbacks, fallback_ones);
    // Every pair with v ≥ k - 1 = 7 falls back: three pairs for each of the
    // valuations from 7 to 38. Each returns zero in place of a column that the
    // reference sets to 1 for exactly half of them.
    const size_t fallback_valuations =
        static_cast<size_t>(kMaxExactValuation - (kOldExponent - 1) + 1);
    const size_t expected_fallbacks = 6 * fallback_valuations;
    TEST_ASSERT(fallbacks == expected_fallbacks);
    TEST_ASSERT(2 * fallback_ones == fallbacks);
    std::cout << "  old exponent: PASSED\n";
}

// The default exponent is exact up to v = 38, which the valuation pairs cover
// with zero fallbacks, and falls back from v = 39: one fallback, and a zero
// column in place of a column that the reference sets to 1 for half of the
// pairs.
void test_default_exponent_falls_back_from_v_39() {
    std::cout << "Default exponent on the valuations it cannot reach...\n";
    SchirokaurConfig old_config;
    old_config.exponent_k = kOldExponent;
    for (const Fixture* fixture : fixtures()) {
        if (!fixture->linear_root_is_one) {
            continue;
        }
        std::cout << "  " << fixture->label << "\n";
        const PolynomialContext ctx = make_context(*fixture);
        const Integer root = lift_root(fixture->coefficients, kReferenceBits);
        const SchirokaurMap map(ctx);
        const SchirokaurMap old_map(ctx, old_config);
        const std::vector<ValuedPair> pairs = fallback_pairs(root);
        size_t ones = 0;
        for (const ValuedPair& valued : pairs) {
            uint64_t counted = 0;
            const std::vector<uint32_t> columns =
                map.compute_flat(valued.pair.a, valued.pair.b, counted);
            const Reference reference = reference_column(valued.pair.a, valued.pair.b, root);
            TEST_ASSERT(reference.valuation == valued.valuation);
            TEST_ASSERT(reference.column == valued.column);
            TEST_ASSERT(counted == 1);
            TEST_ASSERT(columns.size() == ctx.degree());
            TEST_ASSERT(columns[0] == 0);
            uint64_t old_counted = 0;
            TEST_ASSERT(old_map.compute_flat(valued.pair.a, valued.pair.b, old_counted) == columns);
            TEST_ASSERT(old_counted == 1);
            ones += valued.column;
        }
        TEST_ASSERT(2 * ones == pairs.size());
        std::cout << "    " << pairs.size() << " pairs, one fallback each; reference column 1 for "
                  << ones << "\n";
    }
    std::cout << "  default exponent falls back from v = 39: PASSED\n";
}

// Every path at the default exponent: the unsplit path, a quadratic factor,
// and a cubic factor against the companion-matrix reference, the linear
// factor against its own reference, no fallback on any pair, and identical
// columns wherever the old exponent computes without a fallback.
void test_every_path_matches_references_at_default_exponent() {
    std::cout << "Every path against exact references at the default exponent...\n";
    SchirokaurConfig old_config;
    old_config.exponent_k = kOldExponent;
    for (const Fixture* fixture : fixtures()) {
        std::cout << "  " << fixture->label << "\n";
        const PolynomialContext ctx = make_context(*fixture);
        const bool has_linear_factor = fixture->linear_root_is_one;
        const Integer root =
            has_linear_factor ? lift_root(fixture->coefficients, kReferenceBits) : Integer(0);
        const std::vector<Pair> pairs = has_linear_factor ? valuation_pairs(root) : unsplit_pairs();

        const SchirokaurMap map(ctx);
        const SchirokaurMap old_map(ctx, old_config);
        const std::vector<std::vector<Integer>> moduli =
            certified_moduli(map, *fixture, kDefaultExponent);
        TEST_ASSERT(map.prime_info_[0].is_split == (fixture->factors_mod_2.size() > 1));
        if (has_linear_factor) {
            // The lifted linear factor x + c has the root -c, which must agree
            // with the Newton root.
            const Integer modulus = power_of_two(kDefaultExponent);
            TEST_ASSERT(moduli.front().size() == 2);
            TEST_ASSERT(residue(-moduli.front()[0], modulus) == residue(root, modulus));
        }

        size_t compared = 0;
        size_t old_fallbacks = 0;
        for (const Pair& pair : pairs) {
            uint64_t counted = 0;
            const std::vector<uint32_t> columns = map.compute_flat(pair.a, pair.b, counted);
            const ReferenceRow expected = reference_row(moduli, root, kDefaultExponent, pair);
            TEST_ASSERT(columns.size() == ctx.degree());
            TEST_ASSERT(counted == 0);
            TEST_ASSERT(expected.fallbacks == 0);
            TEST_ASSERT(columns == expected.columns);
            for (const uint32_t value : columns)
                TEST_ASSERT(value < 2);

            uint64_t old_counted = 0;
            const std::vector<uint32_t> old_columns =
                old_map.compute_flat(pair.a, pair.b, old_counted);
            if (old_counted == 0) {
                TEST_ASSERT(old_columns == columns);
                ++compared;
            } else {
                // Only the linear factor can fall back on these pairs.
                TEST_ASSERT(has_linear_factor);
                TEST_ASSERT(old_counted == 1);
                TEST_ASSERT(old_columns.size() == columns.size());
                TEST_ASSERT(old_columns[0] == 0);
                TEST_ASSERT(
                    std::equal(columns.begin() + 1, columns.end(), old_columns.begin() + 1));
                ++old_fallbacks;
            }
        }
        TEST_ASSERT(compared > 0);
        std::cout << "    " << pairs.size() << " pairs match the references; " << compared
                  << " are identical at k = 8; " << old_fallbacks << " fall back at k = 8\n";
    }
    std::cout << "  every path: PASSED\n";
}

// The rows of the companion-matrix cases in tests/test_linalg.cpp with their
// unsplit columns, hard-coded there for exponent 8.
struct UnsplitRow {
    Pair pair;
    std::array<uint32_t, 3> columns;
};

const UnsplitRow kUnsplitRows[] = {
    {{1, 2}, {0, 1, 0}},     {{3, 2}, {1, 1, 0}},       {{-5, 2}, {1, 1, 0}},
    {{7, 16}, {1, 0, 0}},    {{-33, 64}, {1, 0, 0}},    {{100, 13}, {0, 0, 0}},
    {{255, 256}, {1, 0, 0}}, {{-1000, 999}, {1, 0, 0}}, {{1000003, 1000034}, {1, 1, 0}},
    {{0, 1}, {0, 0, 0}},     {{5, 0}, {0, 0, 0}},       {{-1, 2}, {1, 1, 0}},
    {{-60, 1}, {0, 0, 0}},   {{-60, 3}, {1, 0, 0}},     {{-58, 3}, {1, 1, 1}},
    {{-196, 1}, {0, 0, 0}},
};

// The unsplit path at the default exponent. A unit gives the same columns at
// every exponent, so the rows hard-coded for exponent 8 must hold unchanged.
// Stripping is exact while s + 2 ≤ k: up to 2^38 at the default, with 2^39 a
// fallback, and only up to 2^6 at exponent 8.
void test_unsplit_path_at_default_exponent() {
    std::cout << "Unsplit path at the default exponent...\n";
    const PolynomialContext ctx = make_context(kUnsplit);
    SchirokaurConfig old_config;
    old_config.exponent_k = kOldExponent;
    const SchirokaurMap map(ctx);
    const SchirokaurMap old_map(ctx, old_config);
    const std::vector<uint32_t> zero_columns(3, 0);

    for (const UnsplitRow& row : kUnsplitRows) {
        uint64_t counted = 0;
        const std::vector<uint32_t> columns = map.compute_flat(row.pair.a, row.pair.b, counted);
        TEST_ASSERT(counted == 0);
        TEST_ASSERT(columns.size() == row.columns.size());
        for (size_t i = 0; i < row.columns.size(); ++i)
            TEST_ASSERT(columns[i] == row.columns[i]);
        uint64_t old_counted = 0;
        TEST_ASSERT(old_map.compute_flat(row.pair.a, row.pair.b, old_counted) == columns);
        TEST_ASSERT(old_counted == 0);
    }

    // λ(2^s·γ) = λ(γ): the map only sees the unit part.
    const Pair units[] = {{1, 2}, {3, 5}, {-7, 9}, {1, 1}};
    for (const Pair& unit : units) {
        uint64_t counted = 0;
        const std::vector<uint32_t> base = map.compute_flat(unit.a, unit.b, counted);
        TEST_ASSERT(counted == 0);
        for (uint32_t s = 0; s <= kMaxExactValuation; ++s) {
            const Pair scaled = scaled_pair(unit, s);
            uint64_t scaled_counted = 0;
            TEST_ASSERT(map.compute_flat(scaled.a, scaled.b, scaled_counted) == base);
            TEST_ASSERT(scaled_counted == 0);
        }
        const Pair beyond = scaled_pair(unit, kFirstFallbackValuation);
        uint64_t beyond_counted = 0;
        TEST_ASSERT(map.compute_flat(beyond.a, beyond.b, beyond_counted) == zero_columns);
        TEST_ASSERT(beyond_counted == 1);
        for (uint32_t s = 0; s + 2 <= kOldExponent; ++s) {
            const Pair scaled = scaled_pair(unit, s);
            uint64_t scaled_counted = 0;
            TEST_ASSERT(old_map.compute_flat(scaled.a, scaled.b, scaled_counted) == base);
            TEST_ASSERT(scaled_counted == 0);
        }
        for (uint32_t s = kOldExponent - 1; s < kOldExponent + 4; ++s) {
            const Pair scaled = scaled_pair(unit, s);
            uint64_t scaled_counted = 0;
            TEST_ASSERT(old_map.compute_flat(scaled.a, scaled.b, scaled_counted) == zero_columns);
            TEST_ASSERT(scaled_counted == 1);
        }
    }
    std::cout << "  " << (sizeof(kUnsplitRows) / sizeof(kUnsplitRows[0]))
              << " rows keep their columns, stripping is exact up to 2^38: PASSED\n";
}

// MatrixBuilder uses the default exponent. Its rows must carry the reference
// column of the linear factor and report no fallback for these pairs.
void test_matrix_builder_rows_match_reference() {
    std::cout << "MatrixBuilder rows against the exact reference...\n";
    const PolynomialContext ctx = make_context(kLinearQuadratic);
    const Integer root = lift_root(kLinearQuadratic.coefficients, kReferenceBits);
    const std::vector<Pair> pairs = valuation_pairs(root);

    std::vector<gnfs::core::Relation> relations;
    relations.reserve(pairs.size());
    for (const Pair& pair : pairs)
        relations.emplace_back(pair.a, pair.b);

    gnfs::linalg::MatrixBuilderConfig config;
    config.include_sign_column = true;
    config.include_qc_columns = false;
    config.include_schirokauer = true;
    config.schirokauer_primes = {2};
    gnfs::linalg::MatrixBuilder builder(config);
    const gnfs::factor_base::FactorBase fb;
    const auto result = builder.build_with_qc(relations, fb, ctx);
    TEST_ASSERT(result.matrix.num_rows() == pairs.size());
    TEST_ASSERT(result.mapping.num_schirokauer_columns == 3);
    TEST_ASSERT(result.schirokauer_precision_fallbacks == 0);
    const auto linear_column = static_cast<uint32_t>(result.mapping.schirokauer_start());
    for (size_t row = 0; row < pairs.size(); ++row) {
        const Reference reference = reference_column(pairs[row].a, pairs[row].b, root);
        TEST_ASSERT(result.matrix.row(row).test(linear_column) == (reference.column == 1));
    }
    std::cout << "  " << pairs.size() << " rows carry the reference column: PASSED\n";
}

} // namespace

int main() {
    int failed = 0;
    std::cout << "=== Schirokauer precision: exact references at the default exponent ===\n";

    failed += !run_case(test_default_exponent_matches_reference);
    failed += !run_case(test_old_exponent_falls_back);
    failed += !run_case(test_default_exponent_falls_back_from_v_39);
    failed += !run_case(test_every_path_matches_references_at_default_exponent);
    failed += !run_case(test_unsplit_path_at_default_exponent);
    failed += !run_case(test_matrix_builder_rows_match_reference);

    if (failed != 0) {
        std::cout << failed << " test case(s) failed\n";
        return 1;
    }
    std::cout << "=== All Schirokauer precision tests PASSED ===\n";
    return 0;
}
