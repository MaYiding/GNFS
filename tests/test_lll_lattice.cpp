// test_lll_lattice.cpp — F-K 2005 LLL lattice basis reduction tests
//
// Verifies the LLL implementation in include/gnfs/sieve/lattice_basis.hpp:
//   1. Mathematical invariants (size-reduced + Lovasz)
//   2. Determinant preservation (|det| == q)
//   3. Caller API stability (e0/f0 = shorter, e1/f1 = longer)
//   4. LLL never worse than Gauss in |b0|^2 + |b1|^2
//   5. Asymmetric r ~ q/2 cases (LLL expected to outperform Gauss)
//   6. Boundary cases: r = 0, r = 1, r = q-1
//   7. Large q close to uint32_t upper bound
//   8. Cross-check verify_ab on both basis vectors

#include "gnfs/core/integer.hpp"
#include "gnfs/sieve/lattice_basis.hpp"
#include "gnfs/sieve/special_q.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace gnfs::sieve;

namespace {

using gnfs::core::Integer;

[[noreturn]] void fail_check(const char* message) {
    std::cerr << "  [FAIL] " << message << std::endl;
    std::abort();
}

void check(bool condition, const char* message) {
    if (!condition)
        fail_check(message);
}

[[nodiscard]] Integer exact_norm_sq(int64_t a, int64_t b) {
    const Integer exact_a(a);
    const Integer exact_b(b);
    return exact_a * exact_a + exact_b * exact_b;
}

[[nodiscard]] Integer exact_norm_sq(const Integer& a, const Integer& b) {
    return a * a + b * b;
}

[[nodiscard]] Integer exact_dot(int64_t a0, int64_t b0, int64_t a1, int64_t b1) {
    return Integer(a0) * Integer(a1) + Integer(b0) * Integer(b1);
}

[[nodiscard]] Integer exact_from_u128(detail::LbU128 value) {
    Integer result(value.hi);
    result *= Integer("18446744073709551616"); // 2^64
    result += Integer(value.lo);
    return result;
}

[[nodiscard]] Integer exact_from_i128(detail::LbI128 value) {
    Integer result = exact_from_u128(value.magnitude);
    if (value.negative)
        result.negate();
    return result;
}

[[nodiscard]] bool same_basis(const LatticeBasis& lhs, const LatticeBasis& rhs) noexcept {
    return lhs.e0 == rhs.e0 && lhs.f0 == rhs.f0 && lhs.e1 == rhs.e1 && lhs.f1 == rhs.f1 &&
           lhs.q == rhs.q && lhs.r == rhs.r;
}

void require_same_basis(const LatticeBasis& actual, const LatticeBasis& expected,
                        const char* context) {
    if (same_basis(actual, expected))
        return;
    std::cerr << "  [FAIL] " << context << "\n"
              << "    actual:   (" << actual.e0 << ", " << actual.f0 << "), (" << actual.e1 << ", "
              << actual.f1 << "), q=" << actual.q << ", r=" << actual.r << "\n"
              << "    expected: (" << expected.e0 << ", " << expected.f0 << "), (" << expected.e1
              << ", " << expected.f1 << "), q=" << expected.q << ", r=" << expected.r << std::endl;
    std::abort();
}

void print_exact(const Integer& value) {
    std::cerr << value;
}

/// Verify the size-reduction invariant: |2 * (v0.v1)| <= |v0|^2.
/// Equivalent to |mu| = |v0.v1/|v0|^2| <= 1/2.
[[nodiscard]] bool is_size_reduced(const LatticeBasis& basis) {
    // basis.e0/f0 is the shorter v0, basis.e1/f1 is the longer v1
    const Integer n0 = exact_norm_sq(basis.e0, basis.f0);
    if (n0 == 0)
        return true; // degenerate
    Integer d = exact_dot(basis.e0, basis.f0, basis.e1, basis.f1);
    d.abs();
    d *= 2;
    // |2*d| <= n0
    return d <= n0;
}

/// Verify Lovasz condition with delta = 1 (LLL strict optimal in 2D).
/// |v1|^2 >= |v0|^2 (after size-reduction; v0 = shorter).
[[nodiscard]] bool satisfies_lovasz(const LatticeBasis& basis) {
    const Integer n0 = exact_norm_sq(basis.e0, basis.f0);
    const Integer n1 = exact_norm_sq(basis.e1, basis.f1);
    return n1 >= n0;
}

/// Caller API: e0/f0 = shorter, e1/f1 = longer.
[[nodiscard]] bool e0_is_shorter(const LatticeBasis& basis) {
    return exact_norm_sq(basis.e0, basis.f0) <= exact_norm_sq(basis.e1, basis.f1);
}

[[nodiscard]] bool det_equals_q(const LatticeBasis& basis) {
    int64_t det = basis.determinant();
    int64_t q = static_cast<int64_t>(basis.q);
    return det == q || det == -q;
}

[[nodiscard]] SpecialQ make_sq(uint32_t q, uint32_t r) {
    SpecialQ sq;
    sq.q = q;
    sq.r = r;
    sq.index = 0;
    return sq;
}

[[nodiscard]] int64_t exact_round_div(const Integer& numerator, const Integer& denominator) {
    check(denominator.is_positive(), "GMP oracle received a non-positive denominator");

    Integer magnitude = numerator.clone();
    const bool negative = magnitude.is_negative();
    magnitude.abs();

    Integer quotient;
    Integer remainder;
    Integer::divmod(quotient, remainder, magnitude, denominator);
    remainder *= 2;
    if (remainder >= denominator)
        quotient += 1;
    if (negative)
        quotient.negate();

    check(quotient.fits_int64(), "GMP oracle quotient did not fit int64_t");
    return quotient.to_int64();
}

void exact_reduce_gauss(Integer& v0_a, Integer& v0_b, Integer& v1_a, Integer& v1_b) {
    constexpr int MAX_GAUSSIAN_ITERS = 64;
    bool changed = true;
    int iters = 0;
    while (changed && iters < MAX_GAUSSIAN_ITERS) {
        changed = false;
        ++iters;

        if (exact_norm_sq(v0_a, v0_b) < exact_norm_sq(v1_a, v1_b)) {
            std::swap(v0_a, v1_a);
            std::swap(v0_b, v1_b);
        }

        const Integer dot = v0_a * v1_a + v0_b * v1_b;
        const Integer norm = v1_a * v1_a + v1_b * v1_b;
        if (!norm.is_zero()) {
            const int64_t mu = exact_round_div(dot, norm);
            if (mu != 0) {
                const Integer exact_mu(mu);
                v0_a -= exact_mu * v1_a;
                v0_b -= exact_mu * v1_b;
                changed = true;
            }
        }
    }

    if (exact_norm_sq(v0_a, v0_b) < exact_norm_sq(v1_a, v1_b)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }
}

void exact_reduce_lll(Integer& v0_a, Integer& v0_b, Integer& v1_a, Integer& v1_b) {
    constexpr int MAX_LLL_ITERS = 128;
    if (exact_norm_sq(v0_a, v0_b) > exact_norm_sq(v1_a, v1_b)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }

    int iters = 0;
    while (iters < MAX_LLL_ITERS) {
        ++iters;
        const Integer norm0 = exact_norm_sq(v0_a, v0_b);
        if (norm0.is_zero())
            break;

        const Integer dot = v0_a * v1_a + v0_b * v1_b;
        const int64_t mu = exact_round_div(dot, norm0);
        if (mu != 0) {
            const Integer exact_mu(mu);
            v1_a -= exact_mu * v0_a;
            v1_b -= exact_mu * v0_b;
        }

        const Integer norm1 = exact_norm_sq(v1_a, v1_b);
        if (norm1 >= norm0)
            break;
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }

    if (exact_norm_sq(v0_a, v0_b) < exact_norm_sq(v1_a, v1_b)) {
        std::swap(v0_a, v1_a);
        std::swap(v0_b, v1_b);
    }
}

[[nodiscard]] LatticeBasis exact_oracle_basis(uint32_t q, uint32_t r,
                                              LatticeReductionMethod method) {
    Integer v0_a(static_cast<uint64_t>(q));
    Integer v0_b(0);
    Integer v1_a(static_cast<uint64_t>(r));
    Integer v1_b(1);

    if (method == LatticeReductionMethod::Gauss) {
        exact_reduce_gauss(v0_a, v0_b, v1_a, v1_b);
    } else {
        check(method == LatticeReductionMethod::LLL,
              "GMP oracle only supports exact Gauss and LLL paths");
        exact_reduce_lll(v0_a, v0_b, v1_a, v1_b);
    }

    check(v0_a.fits_int64() && v0_b.fits_int64() && v1_a.fits_int64() && v1_b.fits_int64(),
          "GMP oracle basis coordinate did not fit int64_t");
    return LatticeBasis{
        .e0 = v1_a.to_int64(),
        .f0 = v1_b.to_int64(),
        .e1 = v0_a.to_int64(),
        .f1 = v0_b.to_int64(),
        .q = q,
        .r = r,
    };
}

} // anonymous namespace

// ─── Test 0: exact cross-platform wide arithmetic ───────────────────

void test_exact_wide_arithmetic() {
    std::cout << "Testing exact portable lattice arithmetic..." << std::endl;

    const uint64_t max_u64 = std::numeric_limits<uint64_t>::max();
    const std::vector<std::pair<uint64_t, uint64_t>> products = {
        {0, max_u64},
        {1, max_u64},
        {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()},
        {max_u64, max_u64},
        {0xFEDCBA9876543210ULL, 0x123456789ABCDEF0ULL},
    };
    for (const auto [lhs, rhs] : products) {
        Integer expected(lhs);
        expected *= Integer(rhs);
        check(exact_from_u128(detail::lb_mul_u64_portable(lhs, rhs)) == expected,
              "portable 32-bit-limb multiplication disagreed with GMP");
        check(exact_from_u128(detail::lb_mul_u64(lhs, rhs)) == expected,
              "selected 64x64 multiplication backend disagreed with GMP");
    }

    const int64_t min_i64 = std::numeric_limits<int64_t>::min();
    check(detail::lb_abs_i64(min_i64) == (uint64_t{1} << 63),
          "INT64_MIN absolute magnitude was not represented exactly");
    check(exact_from_u128(detail::lb_norm_sq(min_i64, 0)) == exact_norm_sq(min_i64, 0),
          "INT64_MIN one-coordinate norm disagreed with GMP");
    const auto two_min_norm = detail::lb_norm_sq(min_i64, min_i64);
    check(two_min_norm == detail::LbU128{uint64_t{1} << 63, 0},
          "two-coordinate INT64_MIN norm did not reach 2^127 exactly");
    check(exact_from_u128(two_min_norm) == exact_norm_sq(min_i64, min_i64),
          "two-coordinate INT64_MIN norm disagreed with GMP");

    struct DotCase {
        int64_t a0;
        int64_t b0;
        int64_t a1;
        int64_t b1;
    };
    const std::vector<DotCase> dot_cases = {
        {2, 3, 4, 5},
        {-2, 3, 4, 5},
        {2, -3, 4, 5},
        {-2, -3, 4, 5},
        {min_i64, min_i64, 1, -1},
        {min_i64, min_i64, min_i64, min_i64},
        {min_i64, min_i64, std::numeric_limits<int64_t>::max(),
         std::numeric_limits<int64_t>::max()},
    };
    for (const auto& dot_case : dot_cases) {
        const auto actual = detail::lb_dot(dot_case.a0, dot_case.b0, dot_case.a1, dot_case.b1);
        const Integer expected = exact_dot(dot_case.a0, dot_case.b0, dot_case.a1, dot_case.b1);
        check(exact_from_i128(actual) == expected, "signed lattice dot disagreed with GMP");
    }
    const auto cancelled_dot = detail::lb_dot(min_i64, min_i64, 1, -1);
    check(detail::lb_is_zero(cancelled_dot.magnitude) && !cancelled_dot.negative,
          "zero dot product retained a negative sign");
    const auto positive_2_to_127 = detail::lb_dot(min_i64, min_i64, min_i64, min_i64);
    check(!positive_2_to_127.negative &&
              positive_2_to_127.magnitude == detail::LbU128{uint64_t{1} << 63, 0},
          "positive 2^127 dot boundary was not represented exactly");
    const detail::LbI128 negative_2_to_126{{uint64_t{1} << 62, 0}, true};
    const auto negative_2_to_127 = detail::lb_add_i128(negative_2_to_126, negative_2_to_126);
    Integer negative_2_to_127_oracle = exact_from_u128({uint64_t{1} << 63, 0});
    negative_2_to_127_oracle.negate();
    check(exact_from_i128(negative_2_to_127) == negative_2_to_127_oracle,
          "negative 2^127 signed-magnitude boundary was not represented exactly");

    const auto near_half_dot = detail::lb_dot(300'000'000, 0, 200'000'000, 0);
    const auto near_half_norm = detail::lb_norm_sq(200'000'000, 1);
    check(detail::lb_int_round_div(near_half_dot, near_half_norm) == 1,
          "just-below-half reducer quotient rounded upward");
    const auto negative_near_half_dot = detail::lb_dot(-300'000'000, 0, 200'000'000, 0);
    check(detail::lb_int_round_div(negative_near_half_dot, near_half_norm) == -1,
          "negative just-below-half reducer quotient rounded away from zero");

    const detail::LbU128 exact_tie_denominator{0, 40'000'000'000'000'000ULL};
    const detail::LbI128 exact_tie_numerator{{0, 60'000'000'000'000'000ULL}, false};
    check(detail::lb_int_round_div(exact_tie_numerator, exact_tie_denominator) == 2,
          "positive halfway quotient did not round away from zero");
    check(detail::lb_int_round_div(detail::LbI128{exact_tie_numerator.magnitude, true},
                                   exact_tie_denominator) == -2,
          "negative halfway quotient did not round away from zero");

    const detail::LbU128 odd_denominator{0, 5};
    check(detail::lb_int_round_div(detail::LbI128{{0, 7}, false}, odd_denominator) == 1,
          "odd-denominator just-below-half quotient rounded upward");
    check(detail::lb_int_round_div(detail::LbI128{{0, 7}, true}, odd_denominator) == -1,
          "negative odd-denominator just-below-half quotient rounded away from zero");
    check(detail::lb_int_round_div(detail::LbI128{{0, 8}, false}, odd_denominator) == 2,
          "odd-denominator just-above-half quotient rounded downward");

    const detail::LbU128 one{0, 1};
    const detail::LbU128 two_to_63{0, uint64_t{1} << 63};
    check(detail::lb_int_round_div(detail::LbI128{two_to_63, false}, one) ==
              std::numeric_limits<int64_t>::max(),
          "positive 2^63 quotient did not saturate at INT64_MAX");
    check(detail::lb_int_round_div(detail::LbI128{two_to_63, true}, one) ==
              std::numeric_limits<int64_t>::min(),
          "negative 2^63 quotient did not produce INT64_MIN");
    check(detail::lb_int_round_div(detail::LbI128{{1, 0}, true}, one) ==
              std::numeric_limits<int64_t>::min(),
          "negative quotient below INT64_MIN did not saturate");

    const detail::LbU128 high_denominator{uint64_t{1} << 36, 0}; // 2^100
    const detail::LbI128 high_just_below{
        {(uint64_t{3} << 35) - 1, std::numeric_limits<uint64_t>::max()}, false};
    const detail::LbI128 high_exact_tie{{uint64_t{3} << 35, 0}, false};
    check(detail::lb_int_round_div(high_just_below, high_denominator) == 1,
          "high-limb just-below-half quotient rounded upward");
    check(detail::lb_int_round_div(high_exact_tie, high_denominator) == 2,
          "high-limb halfway quotient did not round upward");

    struct DivisionCase {
        detail::LbU128 numerator;
        detail::LbU128 denominator;
    };
    const std::vector<DivisionCase> divisions = {
        {{0, 123'456'789}, {0, 12'345}},
        {{1, 0}, {0, 3}},
        {{max_u64, max_u64}, {0, max_u64}},
        {{max_u64, max_u64}, {uint64_t{1} << 63, 0}},
        {{uint64_t{1} << 32, 9}, {uint64_t{1} << 16, 7}},
        {{1, 0}, {2, 0}},
    };
    for (const auto& division_case : divisions) {
        const auto actual =
            detail::lb_divmod_u128(division_case.numerator, division_case.denominator);
        const Integer numerator = exact_from_u128(division_case.numerator);
        const Integer denominator = exact_from_u128(division_case.denominator);
        Integer oracle_quotient;
        Integer oracle_remainder;
        Integer::divmod(oracle_quotient, oracle_remainder, numerator, denominator);
        check(exact_from_u128(actual.quotient) == oracle_quotient,
              "128-bit division quotient disagreed with GMP");
        check(exact_from_u128(actual.remainder) == oracle_remainder,
              "128-bit division remainder disagreed with GMP");
        const Integer reconstructed =
            exact_from_u128(actual.quotient) * denominator + exact_from_u128(actual.remainder);
        check(reconstructed == numerator, "128-bit division violated q*d+r=n identity");
        check(exact_from_u128(actual.remainder) < denominator,
              "128-bit division remainder was not less than its denominator");
    }

    struct GoldenBasis {
        uint32_t q;
        uint32_t r;
        LatticeReductionMethod method;
        int64_t e0;
        int64_t f0;
        int64_t e1;
        int64_t f1;
    };
    const std::vector<GoldenBasis> golden_cases = {
        {300'000'000u, 200'000'000u, LatticeReductionMethod::Gauss, 0, 3, 100'000'000, -1},
        {300'000'000u, 200'000'000u, LatticeReductionMethod::LLL, 0, 3, 100'000'000, -1},
        {4'294'967'291u, 1u, LatticeReductionMethod::Gauss, 1, 1, 2'147'483'646, -2'147'483'645},
        {4'294'967'291u, 1u, LatticeReductionMethod::LLL, 1, 1, 2'147'483'645, -2'147'483'646},
        {4'294'967'291u, 2u, LatticeReductionMethod::Gauss, 2, 1, 858'993'459, -1'717'986'916},
        {4'294'967'291u, 2u, LatticeReductionMethod::LLL, 2, 1, 858'993'459, -1'717'986'916},
        {4'294'967'295u, 2'863'311'530u, LatticeReductionMethod::Gauss, 0, 3, 1'431'655'765, -1},
        {4'294'967'295u, 2'863'311'530u, LatticeReductionMethod::LLL, 0, 3, 1'431'655'765, -1},
    };

    for (const auto& golden : golden_cases) {
        const LatticeBasis expected{
            .e0 = golden.e0,
            .f0 = golden.f0,
            .e1 = golden.e1,
            .f1 = golden.f1,
            .q = golden.q,
            .r = golden.r,
        };
        const auto oracle = exact_oracle_basis(golden.q, golden.r, golden.method);
        const auto actual = compute_lattice_basis(make_sq(golden.q, golden.r), golden.method);
        require_same_basis(oracle, expected, "GMP oracle disagreed with a fixed golden basis");
        require_same_basis(actual, oracle, "production basis disagreed with GMP oracle");
    }

    std::cout << "  PASS (portable/native limbs, exact rounding, 8 GMP-oracle goldens)"
              << std::endl;
}

void test_checked_lattice_projection_contract() {
    std::cout << "Testing checked lattice projection contract..." << std::endl;

    constexpr int64_t high_limb = std::numeric_limits<int64_t>::max();
    const LatticeBasis cancellation_basis{
        .e0 = high_limb,
        .f0 = high_limb - 1,
        .e1 = high_limb - 1,
        .f1 = high_limb - 2,
        .q = 1,
        .r = 0,
    };
    check(cancellation_basis.try_determinant() == -1,
          "high-limb determinant cancellation was not exact");
    check(cancellation_basis.determinant() == -1,
          "checked determinant changed a representable result");

    const auto max_i64 = std::numeric_limits<int64_t>::max();
    const auto min_i64 = std::numeric_limits<int64_t>::min();
    int64_t checked_value = 0;
    check(detail::lb_try_linear_combination(1, max_i64, 0, 0, checked_value) &&
              checked_value == max_i64,
          "exact INT64_MAX projection was rejected");
    check(detail::lb_try_linear_combination(1, min_i64, 0, 0, checked_value) &&
              checked_value == min_i64,
          "exact INT64_MIN projection was rejected");
    check(!detail::lb_try_linear_combination(1, max_i64, 1, 1, checked_value),
          "INT64_MAX+1 projection was accepted");
    check(!detail::lb_try_linear_combination(1, min_i64, -1, 1, checked_value),
          "INT64_MIN-1 projection was accepted");

    int64_t update_a = max_i64;
    int64_t update_b = 17;
    bool update_threw = false;
    try {
        detail::lb_reduce_vector_checked(update_a, update_b, -1, 1, 0);
    } catch (const std::overflow_error&) {
        update_threw = true;
    }
    check(update_threw && update_a == max_i64 && update_b == 17,
          "overflowing basis update did not fail transactionally");

    const LatticeBasis overflowing_determinant{
        .e0 = max_i64,
        .f0 = max_i64,
        .e1 = min_i64,
        .f1 = max_i64,
        .q = 1,
        .r = 0,
    };
    check(!overflowing_determinant.try_determinant().has_value(),
          "non-representable determinant was accepted");
    bool determinant_threw = false;
    try {
        (void)overflowing_determinant.determinant();
    } catch (const std::overflow_error&) {
        determinant_threw = true;
    }
    check(determinant_threw, "determinant overflow did not fail closed");

    const auto max_u32 = std::numeric_limits<uint32_t>::max();
    const LatticeBasis extreme_skew_basis{
        .e0 = static_cast<int64_t>(max_u32),
        .f0 = 0,
        .e1 = -static_cast<int64_t>(max_u32 - 1U),
        .f1 = 1,
        .q = max_u32,
        .r = 1,
    };
    check(extreme_skew_basis.determinant() == static_cast<int64_t>(max_u32),
          "extreme skew fixture determinant was invalid");
    check(extreme_skew_basis.verify_ab(extreme_skew_basis.e0, extreme_skew_basis.f0) &&
              extreme_skew_basis.verify_ab(extreme_skew_basis.e1, extreme_skew_basis.f1),
          "extreme skew fixture did not span the requested lattice");

    int64_t b_residue = min_i64 % static_cast<int64_t>(max_u32);
    if (b_residue < 0) {
        b_residue += static_cast<int64_t>(max_u32);
    }
    check(extreme_skew_basis.verify_ab(b_residue, min_i64),
          "modular membership check overflowed for an extreme b value");
    LatticeBasis zero_modulus = extreme_skew_basis;
    zero_modulus.q = 0;
    check(!zero_modulus.verify_ab(0, 0), "zero-modulus lattice membership was accepted");

    const SieveRegion unsafe_region{
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min(),
    };
    check(!extreme_skew_basis.try_to_ab(unsafe_region.i_min, unsafe_region.j_min).has_value(),
          "overflowing coordinate projection was accepted");
    check(!lattice_projection_fits_int64(extreme_skew_basis, unsafe_region),
          "overflowing region projection was accepted");
    bool projection_threw = false;
    try {
        (void)extreme_skew_basis.to_ab(unsafe_region.i_min, unsafe_region.j_min);
    } catch (const std::overflow_error&) {
        projection_threw = true;
    }
    check(projection_threw, "coordinate projection overflow did not fail closed");

    const SieveRegion safe_region{-16, 15, 1, 16};
    check(lattice_projection_fits_int64(extreme_skew_basis, safe_region),
          "representable region projection was rejected");
    check(extreme_skew_basis.try_to_ab(safe_region.i_max, safe_region.j_max).has_value(),
          "representable coordinate projection was rejected");

    constexpr int64_t corner_scale = 5'000'000'000'000'000'000LL;
    const LatticeBasis off_diagonal_overflow_basis{
        .e0 = corner_scale,
        .f0 = 1,
        .e1 = -corner_scale - 2,
        .f1 = -1,
        .q = 2,
        .r = 0,
    };
    const SieveRegion off_diagonal_overflow_region{1, 3, 1, 3};
    check(off_diagonal_overflow_basis.try_to_ab(1, 1).has_value() &&
              off_diagonal_overflow_basis.try_to_ab(3, 3).has_value(),
          "representable diagonal corners were rejected");
    check(!off_diagonal_overflow_basis.try_to_ab(1, 3).has_value() &&
              !off_diagonal_overflow_basis.try_to_ab(3, 1).has_value(),
          "overflowing off-diagonal corners were accepted");
    check(!lattice_projection_fits_int64(off_diagonal_overflow_basis, off_diagonal_overflow_region),
          "region projection checked only the diagonal corners");

    std::cout << "  PASS (projection, determinant, and membership fail closed)" << std::endl;
}

// ─── Test 1: basic LLL correctness on small primes ───────────────────

void test_lll_basic() {
    std::cout << "Testing LLL basic correctness..." << std::endl;

    // Small primes covering diverse r values
    std::vector<std::pair<uint32_t, uint32_t>> cases = {
        {2, 1},  {3, 1},  {5, 2},    {7, 3},      {11, 5},
        {13, 4}, {17, 8}, {101, 42}, {1009, 500}, {99991, 12345},
    };

    for (auto [q, r] : cases) {
        SpecialQ sq = make_sq(q, r);
        auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);

        // determinant invariant
        assert(det_equals_q(basis));
        // both basis vectors must satisfy a - b*r ≡ 0 (mod q)
        assert(basis.verify_ab(basis.e0, basis.f0));
        assert(basis.verify_ab(basis.e1, basis.f1));
        // caller API: e0 = shorter
        assert(e0_is_shorter(basis));
        // size-reduced
        assert(is_size_reduced(basis));
        // Lovasz (delta=1, 2D optimal)
        assert(satisfies_lovasz(basis));
    }

    std::cout << "  PASS (10 small primes, all invariants hold)" << std::endl;
}

// ─── Test 2: large q close to uint32 upper bound ────────────────────

void test_lll_large_q() {
    std::cout << "Testing LLL with large q (close to 2^32)..." << std::endl;

    // q chosen to stress the __int128_t intermediate computation
    // (q^2 ~ 2^64 > 2^63, so int64_t alone would overflow).
    std::vector<std::pair<uint32_t, uint32_t>> cases = {
        {2'000'003u, 1'000'001u},       // 2M
        {16'777'259u, 8'388'629u},      // 16M ~ 2^24
        {268'435'459u, 134'217'729u},   // 268M ~ 2^28
        {1'073'741'827u, 536'870'913u}, // 2^30
        // Largest case below 2^32 (q^2 fits in __int128_t but not int64_t)
        {2'147'483'647u, 1'073'741'823u}, // 2^31 - 1 (Mersenne prime)
    };

    for (auto [q, r] : cases) {
        SpecialQ sq = make_sq(q, r);
        auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);

        assert(det_equals_q(basis));
        assert(basis.verify_ab(basis.e0, basis.f0));
        assert(basis.verify_ab(basis.e1, basis.f1));
        assert(e0_is_shorter(basis));
        assert(is_size_reduced(basis));
        assert(satisfies_lovasz(basis));

        // |b0|^2 should be O(q), not O(q^2). For ideal LLL, |b0| ~ sqrt(q).
        const Integer n0 = exact_norm_sq(basis.e0, basis.f0);
        Integer twice_q(static_cast<uint64_t>(q));
        twice_q *= 2;
        // |b0|^2 should be <= 2*q (loose upper bound, theory: <= 4/3 * q)
        if (n0 > twice_q) {
            std::cerr << "    [WARN] q=" << q << " r=" << r << " |b0|^2=";
            print_exact(n0);
            std::cerr << " > 2q=" << (2 * q) << std::endl;
            // Don't assert — this is a quality metric, not correctness
        }
    }

    std::cout << "  PASS (5 large q values up to 2^31)" << std::endl;
}

// ─── Test 3: asymmetric r ~ q/2 (LLL key benefit) ───────────────────

void test_lll_asymmetric_r() {
    std::cout << "Testing LLL on asymmetric r ~ q/2 (key F-K 2005 benefit)..." << std::endl;

    // Asymmetric special-q where Gauss may produce suboptimal basis
    std::vector<uint32_t> qs = {1009, 10007, 100003, 1'000'003u, 10'000'019u};
    int lll_beats_gauss = 0;
    int gauss_better = 0;
    int equal = 0;

    for (uint32_t q : qs) {
        // Test multiple r values near q/2
        std::vector<uint32_t> rs = {
            q / 2, q / 2 - 1, q / 2 + 1, q / 2 - 7, q / 2 + 13,
        };
        for (uint32_t r : rs) {
            if (r == 0 || r >= q)
                continue;

            SpecialQ sq = make_sq(q, r);
            auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            auto gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);

            // Both must be valid
            assert(det_equals_q(lll));
            assert(det_equals_q(gauss));

            const Integer lll_total = exact_norm_sq(lll.e0, lll.f0) + exact_norm_sq(lll.e1, lll.f1);
            const Integer gauss_total =
                exact_norm_sq(gauss.e0, gauss.f0) + exact_norm_sq(gauss.e1, gauss.f1);

            if (lll_total < gauss_total)
                ++lll_beats_gauss;
            else if (lll_total > gauss_total)
                ++gauss_better;
            else
                ++equal;

            // Critical invariant: LLL never produces strictly worse basis
            // (because Gauss is also size-reduced + 2D Lovasz subset).
            // If gauss_total < lll_total, we have a bug.
            // Allow equality (most common case in 2D).
            if (gauss_total < lll_total) {
                std::cerr << "    [FAIL] q=" << q << " r=" << r << " gauss=";
                print_exact(gauss_total);
                std::cerr << " lll=";
                print_exact(lll_total);
                std::cerr << " (LLL is worse)" << std::endl;
                assert(false && "LLL produced worse basis than Gauss");
            }
        }
    }

    std::cout << "  LLL_better=" << lll_beats_gauss << " equal=" << equal
              << " gauss_better=" << gauss_better << " (LLL must be >= Gauss; allow equal in 2D)"
              << std::endl;
    std::cout << "  PASS (asymmetric r ~ q/2 invariants hold)" << std::endl;
}

// ─── Test 4: boundary cases — r = 0, r = 1, r = q-1 ─────────────────

void test_lll_boundary_cases() {
    std::cout << "Testing LLL boundary cases (r=0, r=1, r=q-1)..." << std::endl;

    std::vector<uint32_t> qs = {3, 5, 7, 11, 97, 1009, 99991, 1'000'003u};

    for (uint32_t q : qs) {
        // Case r = 0: lattice is { (a, b) : a ≡ 0 (mod q) }
        // Optimal basis: (0, 1), (q, 0). |b0|^2 = 1, |b1|^2 = q^2.
        {
            SpecialQ sq = make_sq(q, 0);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            assert(det_equals_q(basis));
            assert(basis.verify_ab(basis.e0, basis.f0));
            assert(basis.verify_ab(basis.e1, basis.f1));
            assert(e0_is_shorter(basis));
            assert(is_size_reduced(basis));
            assert(satisfies_lovasz(basis));
            // For r=0, optimal |b0|^2 = 1 (the (0, 1) vector).
            const Integer n0 = exact_norm_sq(basis.e0, basis.f0);
            assert(n0 == 1);
        }

        // Case r = 1: lattice is { (a, b) : a ≡ b (mod q) }
        // Optimal basis: (1, 1), then second short vector.
        {
            SpecialQ sq = make_sq(q, 1);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            assert(det_equals_q(basis));
            assert(basis.verify_ab(basis.e0, basis.f0));
            assert(basis.verify_ab(basis.e1, basis.f1));
            assert(e0_is_shorter(basis));
            assert(is_size_reduced(basis));
            assert(satisfies_lovasz(basis));
            // For r=1, |b0|^2 should be very small (= 2 for (1,1) vector).
            const Integer n0 = exact_norm_sq(basis.e0, basis.f0);
            assert(n0 <= Integer(4)); // (1,1) → 2, or some near-equivalent
        }

        // Case r = q-1: known to oscillate in legacy Gauss path (BACKLOG P2).
        // LLL must still terminate and produce valid basis.
        {
            SpecialQ sq = make_sq(q, q - 1);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            // Determinant must hold (it is the strongest mathematical invariant)
            assert(det_equals_q(basis));
            // Caller API: shorter first
            assert(e0_is_shorter(basis));
            // Lattice membership (a - b*r ≡ 0 mod q)
            assert(basis.verify_ab(basis.e0, basis.f0));
            assert(basis.verify_ab(basis.e1, basis.f1));
            // size-reduction + Lovasz must hold for LLL (this is the
            // critical improvement over Gauss for r=q-1)
            assert(is_size_reduced(basis));
            assert(satisfies_lovasz(basis));
        }
    }

    std::cout << "  PASS (8 q values x 3 boundary cases, all invariants)" << std::endl;
}

// ─── Test 5: LLL <= Gauss in basis quality ──────────────────────────

void test_lll_dominates_gauss() {
    std::cout << "Testing LLL never produces worse basis than Gauss..." << std::endl;

    // Systematic sweep
    int total = 0;
    int lll_strictly_better = 0;
    int equal = 0;

    std::vector<uint32_t> qs = {1009, 10007, 100003, 1'000'003u};
    for (uint32_t q : qs) {
        // Sample diverse r values
        std::vector<uint32_t> sample_rs;
        for (uint32_t frac = 1; frac <= 9; ++frac) {
            uint32_t r = (q * frac) / 10;
            if (r > 0 && r < q)
                sample_rs.push_back(r);
        }
        // Add small / large extremes
        sample_rs.push_back(1);
        sample_rs.push_back(q - 1);
        sample_rs.push_back(2);
        sample_rs.push_back(q / 2);

        for (uint32_t r : sample_rs) {
            if (r == 0 || r >= q)
                continue;
            SpecialQ sq = make_sq(q, r);
            auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            auto gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);

            const Integer lll_total = exact_norm_sq(lll.e0, lll.f0) + exact_norm_sq(lll.e1, lll.f1);
            const Integer gauss_total =
                exact_norm_sq(gauss.e0, gauss.f0) + exact_norm_sq(gauss.e1, gauss.f1);

            ++total;
            if (lll_total < gauss_total) {
                ++lll_strictly_better;
            } else if (lll_total == gauss_total) {
                ++equal;
            } else {
                std::cerr << "    [FAIL] q=" << q << " r=" << r << " gauss=";
                print_exact(gauss_total);
                std::cerr << " lll=";
                print_exact(lll_total);
                std::cerr << " (LLL is worse)" << std::endl;
                assert(false && "LLL must dominate Gauss");
            }
        }
    }

    std::cout << "  total=" << total << " lll_strictly_better=" << lll_strictly_better
              << " equal=" << equal << " (LLL >= Gauss invariant holds)" << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ─── Test 6: ENV gate (GNFS_LATTICE_LLL) ────────────────────────────

void test_lll_env_gate() {
    std::cout << "Testing GNFS_LATTICE_LLL ENV gate..." << std::endl;

    SpecialQ sq = make_sq(99991u, 12345u);

    // Default (no ENV) = LLL
    unsetenv("GNFS_LATTICE_LLL");
    {
        auto basis_default = compute_lattice_basis(sq);
        auto basis_lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        // Should be byte-identical (same algorithm)
        assert(basis_default.e0 == basis_lll.e0);
        assert(basis_default.f0 == basis_lll.f0);
        assert(basis_default.e1 == basis_lll.e1);
        assert(basis_default.f1 == basis_lll.f1);
    }

    // GNFS_LATTICE_LLL=0 -> Gauss
    setenv("GNFS_LATTICE_LLL", "0", 1);
    {
        auto basis_env_gauss = compute_lattice_basis(sq);
        auto basis_gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);
        assert(basis_env_gauss.e0 == basis_gauss.e0);
        assert(basis_env_gauss.f0 == basis_gauss.f0);
        assert(basis_env_gauss.e1 == basis_gauss.e1);
        assert(basis_env_gauss.f1 == basis_gauss.f1);
    }

    // GNFS_LATTICE_LLL=1 -> LLL
    setenv("GNFS_LATTICE_LLL", "1", 1);
    {
        auto basis_env_lll = compute_lattice_basis(sq);
        auto basis_lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        assert(basis_env_lll.e0 == basis_lll.e0);
        assert(basis_env_lll.f0 == basis_lll.f0);
        assert(basis_env_lll.e1 == basis_lll.e1);
        assert(basis_env_lll.f1 == basis_lll.f1);
    }

    // Restore default
    unsetenv("GNFS_LATTICE_LLL");

    std::cout << "  PASS (default=LLL, ENV=0->Gauss, ENV=1->LLL)" << std::endl;
}

// ─── Test 7: random property sweep (fuzz-style) ─────────────────────

void test_lll_random_sweep() {
    std::cout << "Testing LLL random property sweep (200 random q,r pairs)..." << std::endl;

    // Deterministic small-LCG (no <random> for portability)
    uint64_t state = 0xC0FFEE12345ull;
    auto next = [&]() -> uint64_t {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 32;
    };

    int count = 0;
    for (int i = 0; i < 200; ++i) {
        uint32_t q = static_cast<uint32_t>((next() % 10'000'000u) + 1009u);
        uint32_t r = static_cast<uint32_t>(next() % q);
        if (r == 0)
            r = 1;

        SpecialQ sq = make_sq(q, r);
        auto basis = compute_lattice_basis(sq, LatticeReductionMethod::LLL);

        assert(det_equals_q(basis));
        assert(basis.verify_ab(basis.e0, basis.f0));
        assert(basis.verify_ab(basis.e1, basis.f1));
        assert(e0_is_shorter(basis));
        assert(is_size_reduced(basis));
        assert(satisfies_lovasz(basis));
        ++count;
    }

    std::cout << "  PASS (" << count << "/200 random pairs, all invariants)" << std::endl;
}

// ─── Test 8: norm reduction quality benchmark ───────────────────────

void test_lll_norm_quality() {
    std::cout << "Testing LLL norm quality (avg |b0|^2 + |b1|^2)..." << std::endl;

    std::vector<uint32_t> qs = {100003u, 1'000'003u, 10'000'019u};

    for (uint32_t q : qs) {
        double total_lll = 0.0;
        double total_gauss = 0.0;
        int n = 0;

        // Sample 20 r values evenly spaced
        for (uint32_t k = 1; k < 20; ++k) {
            uint32_t r = (q / 20) * k;
            if (r == 0 || r >= q)
                continue;

            SpecialQ sq = make_sq(q, r);
            auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
            auto gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);

            const Integer lll_total = exact_norm_sq(lll.e0, lll.f0) + exact_norm_sq(lll.e1, lll.f1);
            const Integer gauss_total =
                exact_norm_sq(gauss.e0, gauss.f0) + exact_norm_sq(gauss.e1, gauss.f1);

            total_lll += lll_total.to_double();
            total_gauss += gauss_total.to_double();
            ++n;
        }

        double avg_lll = total_lll / n;
        double avg_gauss = total_gauss / n;
        double ratio = avg_gauss / avg_lll;
        std::cout << "  q=" << q << " avg |b0|^2+|b1|^2 (LLL)=" << avg_lll
                  << " (Gauss)=" << avg_gauss << " ratio Gauss/LLL=" << ratio << std::endl;
        // LLL never worse (ratio >= 1, with floating tolerance)
        assert(ratio >= 0.999);
    }

    std::cout << "  PASS (quality ratio Gauss/LLL >= 1.0 for all q)" << std::endl;
}

// ─── Test 9: SkewLLL invariants ─────────────────────────────────────

void test_skew_lll_invariants() {
    std::cout << "Testing SkewLLL invariants (skewness sweep)..." << std::endl;

    // Skewness range typical for GNFS polynomials: 1.0 to ~5000.
    std::vector<double> skewnesses = {
        1.0, // unskewed (equivalent to LLL)
        2.0, 5.0, 10.0, 100.0, 1000.0, 5000.0,
    };

    std::vector<std::pair<uint32_t, uint32_t>> cases = {
        {1009, 500},
        {99991, 12345},
        {1'000'003u, 500'000u},
        {10'000'019u, 5'000'000u},
    };

    int total = 0;
    int skew_better = 0;
    int equal_skew = 0;
    int skew_worse = 0;

    for (double s : skewnesses) {
        for (auto [q, r] : cases) {
            SpecialQ sq = make_sq(q, r);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::SkewLLL, s);
            auto plain_lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);

            // Core invariants (must hold for any reduction):
            check(det_equals_q(basis), "SkewLLL did not preserve determinant");
            check(basis.verify_ab(basis.e0, basis.f0),
                  "SkewLLL first vector left the special-q lattice");
            check(basis.verify_ab(basis.e1, basis.f1),
                  "SkewLLL second vector left the special-q lattice");

            // For s=1.0, SkewLLL should be bit-identical to LLL (dispatched).
            if (s == 1.0) {
                require_same_basis(basis, plain_lll,
                                   "unit-skew SkewLLL was not bit-identical to exact LLL");
            }

            // Quality metric: skew norm² (a² + s²·b²)
            // SkewLLL should produce smaller skew norms than plain LLL when s != 1.
            auto skew_n = [s](int64_t a, int64_t b) {
                double da = static_cast<double>(a), db = static_cast<double>(b);
                return da * da + s * s * db * db;
            };
            double skew_total_skewlll = skew_n(basis.e0, basis.f0) + skew_n(basis.e1, basis.f1);
            double skew_total_plain =
                skew_n(plain_lll.e0, plain_lll.f0) + skew_n(plain_lll.e1, plain_lll.f1);

            ++total;
            if (skew_total_skewlll < skew_total_plain * 0.9999)
                ++skew_better;
            else if (skew_total_skewlll > skew_total_plain * 1.0001)
                ++skew_worse;
            else
                ++equal_skew;

            // Hard invariant: SkewLLL never substantially worse in skew norm
            // (allow small double error: ratio < 1.01)
            check(skew_total_skewlll <= skew_total_plain * 1.01,
                  "non-unit SkewLLL baseline regressed in skew norm");
        }
    }

    check(skew_better > 0, "non-unit SkewLLL baselines did not exercise a changed basis");
    check(skew_worse == 0, "non-unit SkewLLL baseline was measurably worse than exact LLL");

    std::cout << "  total=" << total << " skew_better=" << skew_better << " equal=" << equal_skew
              << " skew_worse=" << skew_worse << " (SkewLLL <= LLL in skew norm)" << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ─── Test 10: SkewLLL boundary cases ────────────────────────────────

void test_skew_lll_boundary() {
    std::cout << "Testing SkewLLL boundary cases (large skewness, r=0/q-1)..." << std::endl;

    // Very large skewness (5000+, e.g., 50d high-skew polynomial)
    std::vector<double> extreme_s = {0.5, 1.0, 1.1, 100.0, 5000.0, 50000.0};
    std::vector<std::pair<uint32_t, uint32_t>> cases = {
        {1009, 0},
        {1009, 1},
        {1009, 1008},
        {99991, 0},
        {99991, 1},
        {99991, 99990},
        {1'000'003u, 0},
        {1'000'003u, 500'001u},
        {1'000'003u, 1'000'002u},
    };

    for (double s : extreme_s) {
        for (auto [q, r] : cases) {
            SpecialQ sq = make_sq(q, r);
            auto basis = compute_lattice_basis(sq, LatticeReductionMethod::SkewLLL, s);

            assert(det_equals_q(basis));
            assert(basis.verify_ab(basis.e0, basis.f0));
            assert(basis.verify_ab(basis.e1, basis.f1));
        }
    }

    const SpecialQ sq = make_sq(1009, 500);
    const std::array<double, 6> invalid_skewnesses{
        0.0,
        -1.0,
        std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    for (const double skewness : invalid_skewnesses) {
        bool rejected = false;
        try {
            (void)compute_lattice_basis(sq, LatticeReductionMethod::SkewLLL, skewness);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "SkewLLL accepted a non-representable skewness");
    }

    const double high_finite_skewness = std::sqrt(std::numeric_limits<double>::max()) * 0.75;
    const auto high_finite_basis =
        compute_lattice_basis(sq, LatticeReductionMethod::SkewLLL, high_finite_skewness);
    check(det_equals_q(high_finite_basis), "high finite SkewLLL changed the determinant");

    int64_t v0_a = 1;
    int64_t v0_b = 2;
    int64_t v1_a = 1;
    int64_t v1_b = 1;
    bool metric_overflow_rejected = false;
    try {
        detail::lb_reduce_skew_lll(v0_a, v0_b, v1_a, v1_b, high_finite_skewness);
    } catch (const std::overflow_error&) {
        metric_overflow_rejected = true;
    }
    check(metric_overflow_rejected, "SkewLLL accepted a non-finite intermediate metric");

    bool quotient_overflow_rejected = false;
    try {
        (void)detail::lb_checked_round_to_i64(0x1p63);
    } catch (const std::overflow_error&) {
        quotient_overflow_rejected = true;
    }
    check(quotient_overflow_rejected, "SkewLLL accepted a finite quotient outside int64_t");
    check(detail::lb_checked_round_to_i64(-0x1p63) == std::numeric_limits<int64_t>::min(),
          "SkewLLL rejected the exact INT64_MIN quotient boundary");

    bool config_nan_rejected = false;
    try {
        (void)compute_lattice_basis_with_skewness(
            sq, std::numeric_limits<double>::quiet_NaN(),
            LatticeBasisReductionConfig{LatticeReductionMethod::LLL, true});
    } catch (const std::invalid_argument&) {
        config_nan_rejected = true;
    }
    check(config_nan_rejected, "skew-enabled configuration silently accepted NaN");

    std::cout << "  PASS (finite metrics, invalid inputs, and all invariants)" << std::endl;
}

// ─── Test 11: compute_lattice_basis_with_skewness dispatch ──────────

void test_skew_lll_dispatch() {
    std::cout << "Testing compute_lattice_basis_with_skewness ENV interaction..." << std::endl;

    SpecialQ sq = make_sq(1'000'003u, 500'000u);

    // Clean ENV first
    unsetenv("GNFS_LATTICE_LLL");
    unsetenv("GNFS_LATTICE_SKEW");

    // ENV default (SKEW unset) + skewness=1.0 -> LLL (no skew upgrade)
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 1.0);
        auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        assert(basis.e0 == lll.e0 && basis.e1 == lll.e1);
    }

    // ENV default (SKEW unset) + skewness=10.0 -> still LLL (default OFF for skew)
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 10.0);
        auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        assert(basis.e0 == lll.e0 && basis.e1 == lll.e1);
    }

    // SKEW=1 + skewness=10.0 -> SkewLLL
    setenv("GNFS_LATTICE_SKEW", "1", 1);
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 10.0);
        auto skew = compute_lattice_basis(sq, LatticeReductionMethod::SkewLLL, 10.0);
        assert(basis.e0 == skew.e0 && basis.e1 == skew.e1);
    }

    // SKEW=1 + skewness=1.0 -> LLL (skew=1 degenerate)
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 1.0);
        auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL);
        assert(basis.e0 == lll.e0 && basis.e1 == lll.e1);
    }
    unsetenv("GNFS_LATTICE_SKEW");

    // SKEW=1 + LLL=0 (Gauss) + skewness=10.0 -> Gauss (LLL gate dominates, skew ignored)
    setenv("GNFS_LATTICE_SKEW", "1", 1);
    setenv("GNFS_LATTICE_LLL", "0", 1);
    {
        auto basis = compute_lattice_basis_with_skewness(sq, 10.0);
        auto gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss);
        assert(basis.e0 == gauss.e0 && basis.e1 == gauss.e1);
    }
    unsetenv("GNFS_LATTICE_LLL");
    unsetenv("GNFS_LATTICE_SKEW");

    std::cout << "  PASS (dispatch: default off, ENV opt-in, LLL gate dominates)" << std::endl;
}

// ─── Test 12: explicit config isolation and legacy parity ───────────

void test_explicit_lattice_basis_config() {
    std::cout << "Testing explicit lattice config isolation and legacy parity..." << std::endl;

    SpecialQ sq = make_sq(1'000'003u, 500'000u);
    constexpr double skewness = 10.0;
    const auto lll = compute_lattice_basis(sq, LatticeReductionMethod::LLL, skewness);
    const auto skew_lll = compute_lattice_basis(sq, LatticeReductionMethod::SkewLLL, skewness);
    const auto gauss = compute_lattice_basis(sq, LatticeReductionMethod::Gauss, skewness);

    const LatticeBasisReductionConfig lll_without_skew{};
    setenv("GNFS_LATTICE_LLL", "0", 1);
    setenv("GNFS_LATTICE_SKEW", "1", 1);
    require_same_basis(compute_lattice_basis_with_skewness(sq, skewness, lll_without_skew), lll,
                       "explicit LLL/skew-off ignored opposite Gauss+skew ENV");

    setenv("GNFS_LATTICE_LLL", "1", 1);
    setenv("GNFS_LATTICE_SKEW", "0", 1);
    require_same_basis(compute_lattice_basis_with_skewness(sq, skewness, lll_without_skew), lll,
                       "explicit LLL/skew-off changed after ENV mutation");

    const LatticeBasisReductionConfig lll_with_skew{
        LatticeReductionMethod::LLL,
        true,
    };
    setenv("GNFS_LATTICE_LLL", "0", 1);
    setenv("GNFS_LATTICE_SKEW", "0", 1);
    require_same_basis(compute_lattice_basis_with_skewness(sq, skewness, lll_with_skew), skew_lll,
                       "explicit LLL/skew-on ignored opposite Gauss+skew-off ENV");

    setenv("GNFS_LATTICE_LLL", "1", 1);
    setenv("GNFS_LATTICE_SKEW", "1", 1);
    require_same_basis(compute_lattice_basis_with_skewness(sq, skewness, lll_with_skew), skew_lll,
                       "explicit LLL/skew-on changed after ENV mutation");

    const LatticeBasisReductionConfig gauss_with_skew{
        LatticeReductionMethod::Gauss,
        true,
    };
    require_same_basis(compute_lattice_basis_with_skewness(sq, skewness, gauss_with_skew), gauss,
                       "explicit Gauss was upgraded when skew was enabled");

    unsetenv("GNFS_LATTICE_LLL");
    unsetenv("GNFS_LATTICE_SKEW");
    require_same_basis(compute_lattice_basis_with_skewness(sq, skewness),
                       compute_lattice_basis_with_skewness(sq, skewness, lll_without_skew),
                       "legacy default did not match explicit LLL/skew-off");

    setenv("GNFS_LATTICE_SKEW", "1", 1);
    require_same_basis(compute_lattice_basis_with_skewness(sq, skewness),
                       compute_lattice_basis_with_skewness(sq, skewness, lll_with_skew),
                       "legacy skew opt-in did not match explicit LLL/skew-on");

    setenv("GNFS_LATTICE_LLL", "0", 1);
    require_same_basis(
        compute_lattice_basis_with_skewness(sq, skewness),
        compute_lattice_basis_with_skewness(
            sq, skewness, LatticeBasisReductionConfig{LatticeReductionMethod::Gauss, false}),
        "legacy Gauss gate did not match explicit Gauss");

    unsetenv("GNFS_LATTICE_LLL");
    unsetenv("GNFS_LATTICE_SKEW");

    std::cout << "  PASS (explicit config ignores ENV changes; legacy parity preserved)"
              << std::endl;
}

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "  F-K 2005 LLL Lattice Reduction Tests" << std::endl;
    std::cout << "===========================================" << std::endl;

    test_exact_wide_arithmetic();
    test_checked_lattice_projection_contract();
    test_lll_basic();
    test_lll_large_q();
    test_lll_asymmetric_r();
    test_lll_boundary_cases();
    test_lll_dominates_gauss();
    test_lll_env_gate();
    test_lll_random_sweep();
    test_lll_norm_quality();
    test_skew_lll_invariants();
    test_skew_lll_boundary();
    test_skew_lll_dispatch();
    test_explicit_lattice_basis_config();

    std::cout << "===========================================" << std::endl;
    std::cout << "  All LLL lattice tests passed!" << std::endl;
    std::cout << "===========================================" << std::endl;
    return 0;
}
