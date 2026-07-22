// test_siqs_shadow_linear_algebra.cpp - deterministic shadow-matrix and dependency contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/post_merge_dependency.hpp>
#include <gnfs/siqs/shadow_matrix.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

using gnfs::core::Integer;
using gnfs::siqs::extract_siqs_post_merge_factor;
using gnfs::siqs::SIQSFactorPower;
using gnfs::siqs::SIQSPostMergeDependencyResult;
using gnfs::siqs::SIQSPostMergeDependencyStatus;
using gnfs::siqs::SIQSPostMergeFactorResult;
using gnfs::siqs::SIQSPostMergeFactorStatus;
using gnfs::siqs::SIQSPostMergeRow;
using gnfs::siqs::SIQSShadowMatrixOptions;
using gnfs::siqs::SIQSShadowMatrixResult;
using gnfs::siqs::SIQSShadowMatrixSolution;
using gnfs::siqs::SIQSShadowMatrixStatus;
using gnfs::siqs::SIQSShadowRow;
using gnfs::siqs::SIQSShadowRowOrigin;
using gnfs::siqs::SIQSSourceId;
using gnfs::siqs::solve_siqs_shadow_matrix;
using gnfs::siqs::verify_siqs_post_merge_dependency;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

const Integer oracle_modulus(91);
const std::vector<uint32_t> oracle_factor_base{0, 2, 3, 5};

[[nodiscard]] SIQSShadowRow make_shadow_row(SIQSShadowRowOrigin origin, int64_t x_modulus,
                                            bool q_negative,
                                            std::vector<SIQSFactorPower> factor_powers,
                                            std::vector<uint64_t> large_prime_sqrt_factors,
                                            std::vector<SIQSSourceId> source_ids) {
    return SIQSShadowRow{
        origin, SIQSPostMergeRow{Integer(x_modulus), q_negative, std::move(factor_powers),
                                 std::move(large_prime_sqrt_factors), std::move(source_ids)}};
}

[[nodiscard]] std::vector<SIQSShadowRow> make_oracle_rows() {
    std::vector<SIQSShadowRow> rows;
    rows.push_back(make_shadow_row(SIQSShadowRowOrigin::raw_full, 1, true, {{1, 1}, {2, 2}, {3, 1}},
                                   {}, {{0}}));
    rows.push_back(make_shadow_row(SIQSShadowRowOrigin::large_prime_cycle, 38, false,
                                   {{1, 26}, {2, 4}, {3, 2}}, {11, 29, 41}, {{10}, {11}, {12}}));
    rows.push_back(make_shadow_row(SIQSShadowRowOrigin::large_prime_cycle, 62, true,
                                   {{1, 505}, {2, 2}, {3, 1}}, {29}, {{7}, {8}}));
    return rows;
}

void check_matrix_result(const SIQSShadowMatrixResult& result,
                         SIQSShadowMatrixStatus expected_status) {
    CHECK(result.status() == expected_status);
    CHECK(result.solution().has_value() == (expected_status == SIQSShadowMatrixStatus::valid));
    CHECK(result.is_valid() == (expected_status == SIQSShadowMatrixStatus::valid));
}

void check_dependency_result(const SIQSPostMergeDependencyResult& result,
                             SIQSPostMergeDependencyStatus expected_status) {
    CHECK(result.status() == expected_status);
    CHECK(result.verified().has_value() ==
          (expected_status == SIQSPostMergeDependencyStatus::valid));
    CHECK(result.is_valid() == (expected_status == SIQSPostMergeDependencyStatus::valid));
}

void check_factor_result(const SIQSPostMergeFactorResult& result,
                         SIQSPostMergeFactorStatus expected_status) {
    CHECK(result.status() == expected_status);
    CHECK(result.factors().has_value() ==
          (expected_status == SIQSPostMergeFactorStatus::factor_found));
    CHECK(result.is_valid() == (expected_status == SIQSPostMergeFactorStatus::factor_found));
}

template <class Result> void self_copy_assign(Result& result) {
    const Result* alias = &result;
    result = *alias;
}

[[nodiscard]] uint64_t parity_mask(const SIQSPostMergeRow& row) {
    uint64_t mask = row.q_negative ? uint64_t{1} : uint64_t{0};
    for (const SIQSFactorPower& power : row.factor_powers) {
        if ((power.exponent & uint32_t{1}) != 0 && power.factor_base_index < 64) {
            mask ^= uint64_t{1} << power.factor_base_index;
        }
    }
    return mask;
}

[[nodiscard]] bool dependency_xor_is_zero(std::span<const SIQSShadowRow> rows,
                                          std::span<const size_t> dependency) {
    if (dependency.empty()) {
        return false;
    }
    uint64_t combined = 0;
    size_t previous = 0;
    bool have_previous = false;
    for (const size_t row_index : dependency) {
        if (row_index >= rows.size() || (have_previous && row_index <= previous)) {
            return false;
        }
        combined ^= parity_mask(rows[row_index].row);
        previous = row_index;
        have_previous = true;
    }
    return combined == 0;
}

void check_all_dependencies(const SIQSShadowMatrixSolution& solution,
                            std::span<const SIQSShadowRow> rows) {
    CHECK(solution.row_count == rows.size());
    for (const auto& dependency : solution.dependencies) {
        CHECK(dependency_xor_is_zero(rows, dependency));
    }
}

[[nodiscard]] bool contains_dependency(const SIQSShadowMatrixSolution& solution,
                                       const std::vector<size_t>& expected) {
    return std::find(solution.dependencies.begin(), solution.dependencies.end(), expected) !=
           solution.dependencies.end();
}

void test_oracle_matrix_and_parallel_determinism() {
    const auto rows = make_oracle_rows();
    const auto baseline = solve_siqs_shadow_matrix(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()),
        std::span<const uint32_t>(oracle_factor_base.data(), oracle_factor_base.size()),
        oracle_modulus);
    check_matrix_result(baseline, SIQSShadowMatrixStatus::valid);
    if (!baseline.solution()) {
        return;
    }

    const auto& solution = *baseline.solution();
    CHECK(solution.row_count == 3);
    CHECK(solution.column_count == 4);
    CHECK(solution.dependencies.size() == 2);
    CHECK(contains_dependency(solution, {1}));
    CHECK(contains_dependency(solution, {0, 2}));
    check_all_dependencies(solution, rows);

    for (const uint32_t workers : {1U, 2U, 4U}) {
        const auto candidate = solve_siqs_shadow_matrix(
            std::span<const SIQSShadowRow>(rows.data(), rows.size()),
            std::span<const uint32_t>(oracle_factor_base.data(), oracle_factor_base.size()),
            oracle_modulus, SIQSShadowMatrixOptions{64, workers, 0});
        check_matrix_result(candidate, SIQSShadowMatrixStatus::valid);
        if (candidate.solution()) {
            CHECK(*candidate.solution() == solution);
            check_all_dependencies(*candidate.solution(), rows);
        }
    }

    const auto capped = solve_siqs_shadow_matrix(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()),
        std::span<const uint32_t>(oracle_factor_base.data(), oracle_factor_base.size()),
        oracle_modulus, SIQSShadowMatrixOptions{1, 4, 0});
    check_matrix_result(capped, SIQSShadowMatrixStatus::valid);
    if (capped.solution()) {
        CHECK(capped.solution()->dependencies.size() == 1);
        check_all_dependencies(*capped.solution(), rows);
    }
}

void test_dependency_verification_and_factor_extraction() {
    const auto rows = make_oracle_rows();
    const std::vector<size_t> singleton{1};
    const auto singleton_result = verify_siqs_post_merge_dependency(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()), singleton,
        std::span<const uint32_t>(oracle_factor_base.data(), oracle_factor_base.size()),
        oracle_modulus);
    check_dependency_result(singleton_result, SIQSPostMergeDependencyStatus::valid);
    if (singleton_result.verified()) {
        CHECK(singleton_result.verified()->dependency == singleton);
        CHECK(singleton_result.verified()->x_modulus == Integer(38));
        CHECK(singleton_result.verified()->y_modulus == Integer(25));
        CHECK(singleton_result.verified()->square_modulus == oracle_modulus);
    }

    std::vector<SIQSPostMergeRow> direct_rows;
    direct_rows.reserve(rows.size());
    for (const SIQSShadowRow& row : rows) {
        direct_rows.push_back(row.row);
    }
    const std::vector<size_t> mixed_input{2, 0, 1};
    const std::vector<size_t> mixed_canonical{0, 1, 2};
    auto mixed_result = verify_siqs_post_merge_dependency(
        std::span<const SIQSPostMergeRow>(direct_rows.data(), direct_rows.size()), mixed_input,
        std::span<const uint32_t>(oracle_factor_base.data(), oracle_factor_base.size()),
        oracle_modulus);
    check_dependency_result(mixed_result, SIQSPostMergeDependencyStatus::valid);
    if (mixed_result.verified()) {
        CHECK(mixed_result.verified()->dependency == mixed_canonical);
        CHECK(mixed_result.verified()->x_modulus == Integer(81));
        CHECK(mixed_result.verified()->y_modulus == Integer(3));
    }

    const auto factor_result = extract_siqs_post_merge_factor(mixed_result, oracle_modulus);
    check_factor_result(factor_result, SIQSPostMergeFactorStatus::factor_found);
    if (factor_result.factors()) {
        CHECK(factor_result.factors()->factor == Integer(7));
        CHECK(factor_result.factors()->cofactor == Integer(13));
        CHECK(factor_result.factors()->factor * factor_result.factors()->cofactor ==
              oracle_modulus);
    }

    const std::vector<size_t> trivial_dependency{0, 2};
    const auto trivial = verify_siqs_post_merge_dependency(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()), trivial_dependency,
        std::span<const uint32_t>(oracle_factor_base.data(), oracle_factor_base.size()),
        oracle_modulus);
    check_dependency_result(trivial, SIQSPostMergeDependencyStatus::valid);
    if (trivial.verified()) {
        CHECK(trivial.verified()->x_modulus == Integer(62));
        CHECK(trivial.verified()->y_modulus == Integer(62));
    }
    check_factor_result(extract_siqs_post_merge_factor(trivial, oracle_modulus),
                        SIQSPostMergeFactorStatus::no_factor);
}

void test_dependency_and_factor_fail_closed_contracts() {
    const auto rows = make_oracle_rows();
    const auto row_span = std::span<const SIQSShadowRow>(rows.data(), rows.size());
    const auto factor_base_span =
        std::span<const uint32_t>(oracle_factor_base.data(), oracle_factor_base.size());

    const std::vector<size_t> odd{0};
    const auto odd_result =
        verify_siqs_post_merge_dependency(row_span, odd, factor_base_span, oracle_modulus);
    check_dependency_result(odd_result, SIQSPostMergeDependencyStatus::dependency_not_square);
    const auto invalid_factor = extract_siqs_post_merge_factor(odd_result, oracle_modulus);
    check_factor_result(invalid_factor, SIQSPostMergeFactorStatus::invalid_verified_dependency);

    const std::vector<size_t> empty;
    check_dependency_result(
        verify_siqs_post_merge_dependency(row_span, empty, factor_base_span, oracle_modulus),
        SIQSPostMergeDependencyStatus::invalid_dependency);
    const std::vector<size_t> duplicate{0, 0};
    check_dependency_result(
        verify_siqs_post_merge_dependency(row_span, duplicate, factor_base_span, oracle_modulus),
        SIQSPostMergeDependencyStatus::invalid_dependency);
    const std::vector<size_t> out_of_bounds{3};
    check_dependency_result(verify_siqs_post_merge_dependency(row_span, out_of_bounds,
                                                              factor_base_span, oracle_modulus),
                            SIQSPostMergeDependencyStatus::invalid_dependency);

    auto mismatched_rows = rows;
    mismatched_rows[1].row.x_modulus = Integer(39);
    const std::vector<size_t> mismatched_dependency{1};
    check_dependency_result(
        verify_siqs_post_merge_dependency(
            std::span<const SIQSShadowRow>(mismatched_rows.data(), mismatched_rows.size()),
            mismatched_dependency, factor_base_span, oracle_modulus),
        SIQSPostMergeDependencyStatus::row_identity_mismatch);

    auto malformed_rows = rows;
    malformed_rows[0].row.source_ids.clear();
    check_dependency_result(
        verify_siqs_post_merge_dependency(
            std::span<const SIQSShadowRow>(malformed_rows.data(), malformed_rows.size()), odd,
            factor_base_span, oracle_modulus),
        SIQSPostMergeDependencyStatus::invalid_row);

    check_dependency_result(
        verify_siqs_post_merge_dependency(row_span, odd, factor_base_span, Integer(1)),
        SIQSPostMergeDependencyStatus::invalid_modulus);
    const std::vector<uint32_t> invalid_factor_base{0, 3, 2, 5};
    check_dependency_result(
        verify_siqs_post_merge_dependency(
            row_span, odd,
            std::span<const uint32_t>(invalid_factor_base.data(), invalid_factor_base.size()),
            oracle_modulus),
        SIQSPostMergeDependencyStatus::invalid_factor_base);

    const std::vector<size_t> mixed{0, 1, 2};
    auto valid =
        verify_siqs_post_merge_dependency(row_span, mixed, factor_base_span, oracle_modulus);
    check_dependency_result(valid, SIQSPostMergeDependencyStatus::valid);

    SIQSPostMergeDependencyResult copied_valid(valid);
    check_dependency_result(copied_valid, SIQSPostMergeDependencyStatus::valid);
    if (copied_valid.verified() && valid.verified()) {
        CHECK(copied_valid.verified()->dependency == valid.verified()->dependency);
        CHECK(copied_valid.verified()->x_modulus == valid.verified()->x_modulus);
        CHECK(copied_valid.verified()->y_modulus == valid.verified()->y_modulus);
        CHECK(copied_valid.verified()->square_modulus == valid.verified()->square_modulus);
    }
    SIQSPostMergeDependencyResult copied_failure(odd_result);
    check_dependency_result(copied_failure, SIQSPostMergeDependencyStatus::dependency_not_square);
    copied_failure = valid;
    check_dependency_result(copied_failure, SIQSPostMergeDependencyStatus::valid);
    CHECK(copied_failure.verified() && valid.verified() &&
          copied_failure.verified()->dependency == valid.verified()->dependency);
    copied_valid = odd_result;
    check_dependency_result(copied_valid, SIQSPostMergeDependencyStatus::dependency_not_square);
    self_copy_assign(copied_failure);
    check_dependency_result(copied_failure, SIQSPostMergeDependencyStatus::valid);
    self_copy_assign(copied_valid);
    check_dependency_result(copied_valid, SIQSPostMergeDependencyStatus::dependency_not_square);

    SIQSPostMergeDependencyResult moved = std::move(valid);
    check_dependency_result(moved, SIQSPostMergeDependencyStatus::valid);
    check_dependency_result(valid, SIQSPostMergeDependencyStatus::invalid_dependency);

    check_factor_result(extract_siqs_post_merge_factor(moved, Integer(1)),
                        SIQSPostMergeFactorStatus::invalid_target);
    check_factor_result(extract_siqs_post_merge_factor(moved, Integer(10)),
                        SIQSPostMergeFactorStatus::target_not_divisor);

    auto factor = extract_siqs_post_merge_factor(moved, oracle_modulus);
    check_factor_result(factor, SIQSPostMergeFactorStatus::factor_found);

    SIQSPostMergeFactorResult copied_factor(factor);
    check_factor_result(copied_factor, SIQSPostMergeFactorStatus::factor_found);
    if (copied_factor.factors() && factor.factors()) {
        CHECK(copied_factor.factors()->factor == factor.factors()->factor);
        CHECK(copied_factor.factors()->cofactor == factor.factors()->cofactor);
    }
    SIQSPostMergeFactorResult copied_factor_failure(invalid_factor);
    check_factor_result(copied_factor_failure,
                        SIQSPostMergeFactorStatus::invalid_verified_dependency);
    copied_factor_failure = factor;
    check_factor_result(copied_factor_failure, SIQSPostMergeFactorStatus::factor_found);
    CHECK(copied_factor_failure.factors() && factor.factors() &&
          copied_factor_failure.factors()->factor == factor.factors()->factor &&
          copied_factor_failure.factors()->cofactor == factor.factors()->cofactor);
    copied_factor = invalid_factor;
    check_factor_result(copied_factor, SIQSPostMergeFactorStatus::invalid_verified_dependency);
    self_copy_assign(copied_factor_failure);
    check_factor_result(copied_factor_failure, SIQSPostMergeFactorStatus::factor_found);
    self_copy_assign(copied_factor);
    check_factor_result(copied_factor, SIQSPostMergeFactorStatus::invalid_verified_dependency);

    SIQSPostMergeFactorResult moved_factor = std::move(factor);
    check_factor_result(moved_factor, SIQSPostMergeFactorStatus::factor_found);
    check_factor_result(factor, SIQSPostMergeFactorStatus::invalid_verified_dependency);
}

void test_factor_choice_is_globally_canonical() {
    const Integer modulus(120);
    const std::vector<uint32_t> factor_base{0, 2, 5};
    const std::vector<SIQSShadowRow> rows{
        make_shadow_row(SIQSShadowRowOrigin::raw_full, 50, false, {{1, 2}, {2, 2}}, {}, {{0}})};
    const std::vector<size_t> dependency{0};
    const auto verified = verify_siqs_post_merge_dependency(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()), dependency,
        std::span<const uint32_t>(factor_base.data(), factor_base.size()), modulus);
    check_dependency_result(verified, SIQSPostMergeDependencyStatus::valid);
    if (verified.verified()) {
        CHECK(verified.verified()->x_modulus == Integer(50));
        CHECK(verified.verified()->y_modulus == Integer(10));
    }

    // gcd(X-Y, N)=40 and gcd(X+Y, N)=60. Each candidate must first be
    // normalized with its cofactor before choosing the global minimum pair.
    const auto factor = extract_siqs_post_merge_factor(verified, modulus);
    check_factor_result(factor, SIQSPostMergeFactorStatus::factor_found);
    if (factor.factors()) {
        CHECK(factor.factors()->factor == Integer(2));
        CHECK(factor.factors()->cofactor == Integer(60));
    }
}

void test_matrix_fail_closed_and_move_contracts() {
    const auto rows = make_oracle_rows();
    const auto row_span = std::span<const SIQSShadowRow>(rows.data(), rows.size());
    const auto factor_base_span =
        std::span<const uint32_t>(oracle_factor_base.data(), oracle_factor_base.size());

    check_matrix_result(solve_siqs_shadow_matrix(row_span, factor_base_span, Integer(1)),
                        SIQSShadowMatrixStatus::invalid_modulus);
    const std::vector<uint32_t> invalid_factor_base{0, 3, 2, 5};
    check_matrix_result(
        solve_siqs_shadow_matrix(
            row_span,
            std::span<const uint32_t>(invalid_factor_base.data(), invalid_factor_base.size()),
            oracle_modulus),
        SIQSShadowMatrixStatus::invalid_factor_base);
    const auto invalid_options = solve_siqs_shadow_matrix(
        row_span, factor_base_span, oracle_modulus, SIQSShadowMatrixOptions{0, 1, 0});
    check_matrix_result(invalid_options, SIQSShadowMatrixStatus::invalid_options);
    check_matrix_result(solve_siqs_shadow_matrix(row_span, factor_base_span, oracle_modulus,
                                                 SIQSShadowMatrixOptions{64, 0, 0}),
                        SIQSShadowMatrixStatus::invalid_options);

    auto unknown_origin = rows;
    unknown_origin[0].origin = static_cast<SIQSShadowRowOrigin>(255);
    check_matrix_result(solve_siqs_shadow_matrix(std::span<const SIQSShadowRow>(
                                                     unknown_origin.data(), unknown_origin.size()),
                                                 factor_base_span, oracle_modulus),
                        SIQSShadowMatrixStatus::invalid_row);

    auto malformed = rows;
    malformed[0].row.source_ids.clear();
    check_matrix_result(
        solve_siqs_shadow_matrix(std::span<const SIQSShadowRow>(malformed.data(), malformed.size()),
                                 factor_base_span, oracle_modulus),
        SIQSShadowMatrixStatus::invalid_row);

    auto mismatched = rows;
    mismatched[1].row.x_modulus = Integer(39);
    check_matrix_result(solve_siqs_shadow_matrix(
                            std::span<const SIQSShadowRow>(mismatched.data(), mismatched.size()),
                            factor_base_span, oracle_modulus),
                        SIQSShadowMatrixStatus::row_identity_mismatch);

    auto valid = solve_siqs_shadow_matrix(row_span, factor_base_span, oracle_modulus);
    check_matrix_result(valid, SIQSShadowMatrixStatus::valid);

    SIQSShadowMatrixResult copied_valid(valid);
    check_matrix_result(copied_valid, SIQSShadowMatrixStatus::valid);
    CHECK(copied_valid.solution() && valid.solution() &&
          *copied_valid.solution() == *valid.solution());
    SIQSShadowMatrixResult copied_failure(invalid_options);
    check_matrix_result(copied_failure, SIQSShadowMatrixStatus::invalid_options);
    copied_failure = valid;
    check_matrix_result(copied_failure, SIQSShadowMatrixStatus::valid);
    CHECK(copied_failure.solution() && valid.solution() &&
          *copied_failure.solution() == *valid.solution());
    copied_valid = invalid_options;
    check_matrix_result(copied_valid, SIQSShadowMatrixStatus::invalid_options);
    self_copy_assign(copied_failure);
    check_matrix_result(copied_failure, SIQSShadowMatrixStatus::valid);
    self_copy_assign(copied_valid);
    check_matrix_result(copied_valid, SIQSShadowMatrixStatus::invalid_options);

    SIQSShadowMatrixResult moved = std::move(valid);
    check_matrix_result(moved, SIQSShadowMatrixStatus::valid);
    check_matrix_result(valid, SIQSShadowMatrixStatus::internal_invariant_failure);

    auto assignment_target = solve_siqs_shadow_matrix(row_span, factor_base_span, oracle_modulus,
                                                      SIQSShadowMatrixOptions{0, 1, 0});
    assignment_target = std::move(moved);
    check_matrix_result(assignment_target, SIQSShadowMatrixStatus::valid);
    check_matrix_result(moved, SIQSShadowMatrixStatus::internal_invariant_failure);
}

struct BruteForceDimensions {
    size_t rank;
    size_t nullity;
};

[[nodiscard]] BruteForceDimensions brute_force_dimensions(std::span<const SIQSShadowRow> rows) {
    if (rows.size() >= 64) {
        return {0, 0};
    }
    const uint64_t subset_count = uint64_t{1} << rows.size();
    uint64_t zero_subsets = 0;
    for (uint64_t subset = 0; subset < subset_count; ++subset) {
        uint64_t combined = 0;
        for (size_t row = 0; row < rows.size(); ++row) {
            if ((subset & (uint64_t{1} << row)) != 0) {
                combined ^= parity_mask(rows[row].row);
            }
        }
        if (combined == 0) {
            ++zero_subsets;
        }
    }

    size_t nullity = 0;
    uint64_t expected_zero_subsets = 1;
    while (expected_zero_subsets < zero_subsets) {
        expected_zero_subsets <<= 1U;
        ++nullity;
    }
    CHECK(expected_zero_subsets == zero_subsets);
    return {rows.size() - nullity, nullity};
}

[[nodiscard]] bool dependency_basis_is_independent(const SIQSShadowMatrixSolution& solution) {
    const size_t words_per_vector = (solution.row_count + size_t{63}) / size_t{64};
    std::vector<std::vector<uint64_t>> pivots(solution.row_count);
    for (const auto& dependency : solution.dependencies) {
        if (dependency.empty()) {
            return false;
        }
        std::vector<uint64_t> candidate(words_per_vector, uint64_t{0});
        size_t previous = 0;
        bool have_previous = false;
        for (const size_t row : dependency) {
            if (row >= solution.row_count || (have_previous && row <= previous)) {
                return false;
            }
            candidate[row / size_t{64}] |= uint64_t{1} << (row % size_t{64});
            previous = row;
            have_previous = true;
        }

        bool installed_pivot = false;
        for (size_t bit = 0; bit < solution.row_count; ++bit) {
            if ((candidate[bit / size_t{64}] & (uint64_t{1} << (bit % size_t{64}))) == 0) {
                continue;
            }
            if (pivots[bit].empty()) {
                pivots[bit] = std::move(candidate);
                installed_pivot = true;
                break;
            }
            for (size_t word = 0; word < words_per_vector; ++word) {
                candidate[word] ^= pivots[bit][word];
            }
        }
        if (!installed_pivot) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<SIQSShadowRow> make_mod_two_matrix() {
    // Bits are sign, 3, 5, and 7. Prime two is deliberately absent so every
    // sparse row remains an exact identity modulo two.
    const std::vector<uint8_t> patterns{0b0011, 0b0101, 0b0110, 0b1001,
                                        0b1110, 0b1111, 0b0000, 0b1010};
    std::vector<SIQSShadowRow> rows;
    rows.reserve(patterns.size());
    for (size_t row_index = 0; row_index < patterns.size(); ++row_index) {
        const uint8_t pattern = patterns[row_index];
        std::vector<SIQSFactorPower> powers;
        for (uint32_t odd_prime_bit = 1; odd_prime_bit < 4; ++odd_prime_bit) {
            if ((pattern & (uint8_t{1} << odd_prime_bit)) != 0) {
                powers.push_back(SIQSFactorPower{
                    odd_prime_bit + 1,
                    static_cast<uint32_t>(1 + 2 * ((row_index + odd_prime_bit) % 3))});
            }
        }
        rows.push_back(make_shadow_row(row_index % 2 == 0 ? SIQSShadowRowOrigin::raw_full
                                                          : SIQSShadowRowOrigin::large_prime_cycle,
                                       1, (pattern & uint8_t{1}) != 0, std::move(powers), {},
                                       {{static_cast<uint64_t>(row_index)}}));
    }
    return rows;
}

void test_fixed_matrix_against_brute_force() {
    const Integer modulus(2);
    const std::vector<uint32_t> factor_base{0, 2, 3, 5, 7};
    const auto rows = make_mod_two_matrix();
    const auto expected = brute_force_dimensions(rows);
    const auto result =
        solve_siqs_shadow_matrix(std::span<const SIQSShadowRow>(rows.data(), rows.size()),
                                 std::span<const uint32_t>(factor_base.data(), factor_base.size()),
                                 modulus, SIQSShadowMatrixOptions{64, 4, 0});
    check_matrix_result(result, SIQSShadowMatrixStatus::valid);
    if (!result.solution()) {
        return;
    }

    const auto& solution = *result.solution();
    CHECK(solution.row_count == rows.size());
    CHECK(solution.column_count == factor_base.size());
    CHECK(solution.dependencies.size() == expected.nullity);
    CHECK(solution.row_count - solution.dependencies.size() == expected.rank);
    check_all_dependencies(solution, rows);
    CHECK(dependency_basis_is_independent(solution));
}

[[nodiscard]] std::vector<SIQSShadowRow> make_packed_word_boundary_rows() {
    std::vector<SIQSShadowRow> rows;
    rows.reserve(70);
    for (size_t row = 0; row < 70; ++row) {
        const bool shared_sign_column = row == 0 || row == 69;
        rows.push_back(make_shadow_row(SIQSShadowRowOrigin::raw_full, 1, shared_sign_column, {}, {},
                                       {{static_cast<uint64_t>(row)}}));
    }
    return rows;
}

void test_packed_word_boundary_dependencies() {
    const Integer modulus(2);
    const std::vector<uint32_t> factor_base{0, 2, 3, 5, 7};
    const auto rows = make_packed_word_boundary_rows();
    const auto row_span = std::span<const SIQSShadowRow>(rows.data(), rows.size());
    const auto factor_base_span = std::span<const uint32_t>(factor_base.data(), factor_base.size());

    std::vector<std::vector<size_t>> expected_full_basis;
    expected_full_basis.reserve(69);
    for (size_t row = 1; row < 69; ++row) {
        expected_full_basis.push_back({row});
    }
    expected_full_basis.push_back({0, 69});

    const auto baseline = solve_siqs_shadow_matrix(row_span, factor_base_span, modulus,
                                                   SIQSShadowMatrixOptions{70, 1, 0});
    check_matrix_result(baseline, SIQSShadowMatrixStatus::valid);
    if (!baseline.solution()) {
        return;
    }
    CHECK(baseline.solution()->row_count == 70);
    CHECK(baseline.solution()->column_count == factor_base.size());
    CHECK(baseline.solution()->dependencies == expected_full_basis);
    CHECK(baseline.solution()->dependencies.size() == 69);
    CHECK(contains_dependency(*baseline.solution(), {0, 69}));
    for (size_t row = 1; row < 69; ++row) {
        CHECK(contains_dependency(*baseline.solution(), {row}));
    }
    check_all_dependencies(*baseline.solution(), rows);
    CHECK(dependency_basis_is_independent(*baseline.solution()));

    for (const uint32_t workers : {1U, 2U, 4U}) {
        const auto candidate = solve_siqs_shadow_matrix(row_span, factor_base_span, modulus,
                                                        SIQSShadowMatrixOptions{70, workers, 0});
        check_matrix_result(candidate, SIQSShadowMatrixStatus::valid);
        if (candidate.solution()) {
            CHECK(*candidate.solution() == *baseline.solution());
            check_all_dependencies(*candidate.solution(), rows);
            CHECK(dependency_basis_is_independent(*candidate.solution()));
        }
    }

    std::vector<std::vector<size_t>> expected_capped_basis;
    expected_capped_basis.reserve(64);
    for (size_t row = 1; row <= 64; ++row) {
        expected_capped_basis.push_back({row});
    }
    const auto capped_baseline = solve_siqs_shadow_matrix(row_span, factor_base_span, modulus,
                                                          SIQSShadowMatrixOptions{64, 1, 0});
    check_matrix_result(capped_baseline, SIQSShadowMatrixStatus::valid);
    if (!capped_baseline.solution()) {
        return;
    }
    CHECK(capped_baseline.solution()->dependencies == expected_capped_basis);
    CHECK(capped_baseline.solution()->dependencies.size() == 64);
    check_all_dependencies(*capped_baseline.solution(), rows);
    CHECK(dependency_basis_is_independent(*capped_baseline.solution()));

    for (const uint32_t workers : {1U, 2U, 4U}) {
        const auto candidate = solve_siqs_shadow_matrix(row_span, factor_base_span, modulus,
                                                        SIQSShadowMatrixOptions{64, workers, 0});
        check_matrix_result(candidate, SIQSShadowMatrixStatus::valid);
        if (candidate.solution()) {
            CHECK(*candidate.solution() == *capped_baseline.solution());
            check_all_dependencies(*candidate.solution(), rows);
            CHECK(dependency_basis_is_independent(*candidate.solution()));
        }
    }
}

void test_empty_matrix_is_valid() {
    const Integer modulus(2);
    const std::vector<uint32_t> factor_base{0, 2, 3, 5, 7};
    const std::vector<SIQSShadowRow> rows;
    const auto dimensions = brute_force_dimensions(rows);
    CHECK(dimensions.rank == 0);
    CHECK(dimensions.nullity == 0);

    const auto result =
        solve_siqs_shadow_matrix(std::span<const SIQSShadowRow>(rows.data(), rows.size()),
                                 std::span<const uint32_t>(factor_base.data(), factor_base.size()),
                                 modulus, SIQSShadowMatrixOptions{64, 4, 0});
    check_matrix_result(result, SIQSShadowMatrixStatus::valid);
    if (result.solution()) {
        CHECK(result.solution()->row_count == 0);
        CHECK(result.solution()->column_count == factor_base.size());
        CHECK(result.solution()->dependencies.empty());
    }
}

} // namespace

int main() {
    test_oracle_matrix_and_parallel_determinism();
    test_dependency_verification_and_factor_extraction();
    test_dependency_and_factor_fail_closed_contracts();
    test_factor_choice_is_globally_canonical();
    test_matrix_fail_closed_and_move_contracts();
    test_fixed_matrix_against_brute_force();
    test_packed_word_boundary_dependencies();
    test_empty_matrix_is_valid();

    std::cout << "SIQS shadow linear algebra: " << checks_passed << " checks passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
