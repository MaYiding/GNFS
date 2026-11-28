// test_schirokauer_precision.cpp — Schirokauer columns of a linear factor
// against an exact 2-adic reference.
//
// When f has a linear factor modulo 2, SchirokaurMap takes the split path and
// computes the column of that factor from γ = a - b·r modulo 2^k, where r is
// the root of the factor lifted modulo 2^k. The column is bit v+1 of the
// integer a - b·r, with v = v₂(a - b·r). The lift knows γ only modulo 2^k, so
// the map returns that bit while v + 2 ≤ k and zero columns otherwise. The
// latter is a precision fallback: the row then carries a wrong constraint
// whenever the true bit is 1.
//
// The reference shares no code with the map. It lifts the root to modulo 2^64
// by Newton iteration on exact Integer values, forms a - b·r exactly, and
// reads bit v+1 directly. A negative integer is read in the 2-adic sense, as
// an infinite two's complement, which is what Integer::test_bit implements.
//
// Fixture: the polynomial that the default selector picks for the 25-digit
// semiprime N = 1869045589843040925386929 = 1309931996651 × 1426826426579:
// f = x^3 + 148x^2 + 24945452x + 19508769 with m = 123179884 and f(m) = N.
// f ≡ x^3 + 1 = (x + 1)(x^2 + x + 1) (mod 2): one linear factor, with root 1.
// tests/test_schirokauer_precision_e2e.cpp factors N through Pipeline::run(),
// which selects this polynomial.

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

// f(x) = Σ kCoefficients[i] x^i, low degree first.
constexpr std::array<int64_t, 4> kCoefficients{19508769, 24945452, 148, 1};
constexpr int64_t kM = 123179884;
// The default exponent. It covers every pair with v ≤ 37, since v + 2 ≤ 39.
constexpr uint32_t kDefaultExponent = 40;
constexpr uint32_t kMaxCoveredValuation = 37;
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

PolynomialContext make_context() {
    std::vector<Integer> coefficients;
    coefficients.reserve(kCoefficients.size());
    for (const int64_t value : kCoefficients)
        coefficients.emplace_back(value);
    PolynomialContext ctx(Integer("1869045589843040925386929"), std::move(coefficients),
                          Integer(kM));
    TEST_ASSERT(ctx.degree() == 3);
    TEST_ASSERT(ctx.verify());
    return ctx;
}

// f(x) at an exact integer argument, by Horner's rule from the top coefficient.
Integer evaluate(const Integer& x) {
    Integer result(0);
    for (size_t i = kCoefficients.size(); i-- > 0;) {
        result *= x;
        result += kCoefficients[i];
    }
    return result;
}

// f'(x) at an exact integer argument.
Integer evaluate_derivative(const Integer& x) {
    Integer result(0);
    for (size_t i = kCoefficients.size() - 1; i-- > 0;) {
        result *= x;
        result += static_cast<int64_t>(i + 1) * kCoefficients[i + 1];
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
Integer lift_root(uint32_t bits) {
    Integer root(1);
    TEST_ASSERT(evaluate(root).is_even());
    TEST_ASSERT(evaluate_derivative(root).is_odd());
    uint32_t precision = 1;
    while (precision < bits) {
        precision = std::min<uint32_t>(2 * precision, bits);
        const Integer modulus = power_of_two(precision);
        const Integer value = residue(evaluate(root), modulus);
        const Integer derivative = residue(evaluate_derivative(root), modulus);
        const Integer inverse = gnfs::core::mod_inverse(derivative, modulus);
        TEST_ASSERT(!inverse.is_zero());
        Integer correction = value * inverse;
        correction %= modulus;
        root -= correction;
        root = residue(root, modulus);
    }
    TEST_ASSERT(residue(evaluate(root), power_of_two(bits)).is_zero());
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

// Pairs that cover v = 0 to kMaxCoveredValuation with both column values. For
// each v and each t, the residue a of r + (1 + 2t)·2^v modulo 2^(v+2) gives
// v₂(a - r) = v and column t with b = 1. The same a as a negative number and
// the analogue with b = 3 repeat the pair. Even b with odd a are units and
// add the v = 0 cases that the linear path meets in production.
std::vector<Pair> valuation_pairs(const Integer& root) {
    std::vector<Pair> pairs;
    for (uint32_t v = 0; v <= kMaxCoveredValuation; ++v) {
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

// Compare the map at one exponent with the reference on every pair. Returns
// the number of fallbacks through the out parameters, and how many of those
// have reference column 1.
void check_map(const PolynomialContext& ctx, const Integer& root, uint32_t exponent_k,
               size_t& fallbacks, size_t& fallback_ones) {
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
    std::cout << "Default exponent against the exact reference...\n";
    const PolynomialContext ctx = make_context();
    const Integer root = lift_root(kReferenceBits);
    size_t fallbacks = 0;
    size_t fallback_ones = 0;
    check_map(ctx, root, kDefaultExponent, fallbacks, fallback_ones);
    TEST_ASSERT(fallbacks == 0);
    TEST_ASSERT(SchirokaurConfig{}.exponent_k == kDefaultExponent);
    std::cout << "  default exponent: PASSED\n";
}

void test_old_exponent_falls_back() {
    std::cout << "Old exponent k = 8 against the exact reference...\n";
    const PolynomialContext ctx = make_context();
    const Integer root = lift_root(kReferenceBits);
    size_t fallbacks = 0;
    size_t fallback_ones = 0;
    check_map(ctx, root, kOldExponent, fallbacks, fallback_ones);
    // Every pair with v ≥ k - 1 = 7 falls back: three pairs for each of the
    // valuations from 7 to 37. Each returns zero in place of a column that the
    // reference sets to 1 for exactly half of them.
    const size_t fallback_valuations =
        static_cast<size_t>(kMaxCoveredValuation - (kOldExponent - 1) + 1);
    const size_t expected_fallbacks = 6 * fallback_valuations;
    TEST_ASSERT(fallbacks == expected_fallbacks);
    TEST_ASSERT(2 * fallback_ones == fallbacks);
    std::cout << "  old exponent: PASSED\n";
}

// Without a fallback the columns do not depend on k: the map depends on γ
// modulo 2^(v+2) only. The same rows that k = 8 computes correctly therefore
// carry identical columns at k = 40, quadratic factor included.
void test_columns_independent_of_exponent_without_fallback() {
    std::cout << "Columns independent of k where no fallback occurs...\n";
    const PolynomialContext ctx = make_context();
    const Integer root = lift_root(kReferenceBits);
    SchirokaurConfig old_config;
    old_config.exponent_k = kOldExponent;
    SchirokaurConfig new_config;
    new_config.exponent_k = kDefaultExponent;
    const SchirokaurMap old_map(ctx, old_config);
    const SchirokaurMap new_map(ctx, new_config);

    size_t compared = 0;
    for (const Pair& pair : valuation_pairs(root)) {
        uint64_t old_fallbacks = 0;
        const auto old_columns = old_map.compute_flat(pair.a, pair.b, old_fallbacks);
        if (old_fallbacks != 0)
            continue;
        uint64_t new_fallbacks = 0;
        const auto new_columns = new_map.compute_flat(pair.a, pair.b, new_fallbacks);
        TEST_ASSERT(new_fallbacks == 0);
        TEST_ASSERT(old_columns == new_columns);
        ++compared;
    }
    TEST_ASSERT(compared > 0);
    std::cout << "  " << compared << " pairs identical at k = 8 and k = 40: PASSED\n";
}

// MatrixBuilder uses the default exponent. Its rows must carry the reference
// column of the linear factor and report no fallback for these pairs.
void test_matrix_builder_rows_match_reference() {
    std::cout << "MatrixBuilder rows against the exact reference...\n";
    const PolynomialContext ctx = make_context();
    const Integer root = lift_root(kReferenceBits);
    const auto pairs = valuation_pairs(root);

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
    std::cout << "=== Schirokauer precision: linear factor modulo 2 ===\n";

    failed += !run_case(test_default_exponent_matches_reference);
    failed += !run_case(test_old_exponent_falls_back);
    failed += !run_case(test_columns_independent_of_exponent_without_fallback);
    failed += !run_case(test_matrix_builder_rows_match_reference);

    if (failed != 0) {
        std::cout << failed << " test case(s) failed\n";
        return 1;
    }
    std::cout << "=== All Schirokauer precision tests PASSED ===\n";
    return 0;
}
