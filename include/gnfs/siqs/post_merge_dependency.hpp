#pragma once

/// @file post_merge_dependency.hpp
/// @brief Verify sparse-wide SIQS dependencies and extract non-trivial factors.

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/congruence.hpp>
#include <gnfs/siqs/post_merge_row.hpp>
#include <gnfs/siqs/shadow_assembly.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using std::size_t;

enum class SIQSPostMergeDependencyStatus : uint8_t {
    valid,
    invalid_modulus,
    invalid_factor_base,
    invalid_row,
    row_identity_mismatch,
    invalid_dependency,
    exponent_overflow,
    dependency_not_square,
    dependency_mismatch,
};

/// Exact square pair reconstructed from one canonical dependency.
struct VerifiedSIQSPostMergeDependency {
    std::vector<size_t> dependency;
    core::Integer x_modulus;
    core::Integer y_modulus;
    core::Integer square_modulus;
};

/// Invariant-safe result: verified() is present exactly when status() is valid.
class SIQSPostMergeDependencyResult {
public:
    SIQSPostMergeDependencyResult(const SIQSPostMergeDependencyResult&) = default;
    SIQSPostMergeDependencyResult& operator=(const SIQSPostMergeDependencyResult& other) {
        if (this != &other) {
            SIQSPostMergeDependencyResult copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    SIQSPostMergeDependencyResult(SIQSPostMergeDependencyResult&& other) noexcept
        : status_(other.status_), verified_(std::move(other.verified_)) {
        other.status_ = SIQSPostMergeDependencyStatus::invalid_dependency;
        other.verified_.reset();
    }

    SIQSPostMergeDependencyResult& operator=(SIQSPostMergeDependencyResult&& other) noexcept {
        if (this != &other) {
            status_ = other.status_;
            verified_ = std::move(other.verified_);
            other.status_ = SIQSPostMergeDependencyStatus::invalid_dependency;
            other.verified_.reset();
        }
        return *this;
    }

    [[nodiscard]] SIQSPostMergeDependencyStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<VerifiedSIQSPostMergeDependency>& verified() const noexcept {
        return verified_;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return status_ == SIQSPostMergeDependencyStatus::valid && verified_.has_value();
    }

private:
    friend SIQSPostMergeDependencyResult verify_siqs_post_merge_dependency(
        std::span<const SIQSPostMergeRow> rows, std::span<const size_t> dependency,
        std::span<const uint32_t> factor_base_primes, const core::Integer& square_modulus);
    friend SIQSPostMergeDependencyResult verify_siqs_post_merge_dependency(
        std::span<const SIQSShadowRow> rows, std::span<const size_t> dependency,
        std::span<const uint32_t> factor_base_primes, const core::Integer& square_modulus);

    SIQSPostMergeDependencyResult(SIQSPostMergeDependencyStatus status,
                                  std::optional<VerifiedSIQSPostMergeDependency> verified)
        : status_(status), verified_(std::move(verified)) {}

    [[nodiscard]] static SIQSPostMergeDependencyResult
    failure(SIQSPostMergeDependencyStatus status) {
        if (status == SIQSPostMergeDependencyStatus::valid) {
            status = SIQSPostMergeDependencyStatus::invalid_dependency;
        }
        return SIQSPostMergeDependencyResult(status, std::nullopt);
    }

    [[nodiscard]] static SIQSPostMergeDependencyResult
    success(VerifiedSIQSPostMergeDependency verified) {
        return SIQSPostMergeDependencyResult(SIQSPostMergeDependencyStatus::valid,
                                             std::move(verified));
    }

    SIQSPostMergeDependencyStatus status_;
    std::optional<VerifiedSIQSPostMergeDependency> verified_;
};

enum class SIQSPostMergeFactorStatus : uint8_t {
    invalid_verified_dependency,
    invalid_target,
    target_not_divisor,
    no_factor,
    factor_found,
};

struct SIQSPostMergeFactorization {
    core::Integer factor;
    core::Integer cofactor;
};

/// Invariant-safe result: factors() is present exactly for factor_found.
class SIQSPostMergeFactorResult {
public:
    SIQSPostMergeFactorResult(const SIQSPostMergeFactorResult&) = default;
    SIQSPostMergeFactorResult& operator=(const SIQSPostMergeFactorResult& other) {
        if (this != &other) {
            SIQSPostMergeFactorResult copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    SIQSPostMergeFactorResult(SIQSPostMergeFactorResult&& other) noexcept
        : status_(other.status_), factors_(std::move(other.factors_)) {
        other.status_ = SIQSPostMergeFactorStatus::invalid_verified_dependency;
        other.factors_.reset();
    }

    SIQSPostMergeFactorResult& operator=(SIQSPostMergeFactorResult&& other) noexcept {
        if (this != &other) {
            status_ = other.status_;
            factors_ = std::move(other.factors_);
            other.status_ = SIQSPostMergeFactorStatus::invalid_verified_dependency;
            other.factors_.reset();
        }
        return *this;
    }

    [[nodiscard]] SIQSPostMergeFactorStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<SIQSPostMergeFactorization>& factors() const noexcept {
        return factors_;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return status_ == SIQSPostMergeFactorStatus::factor_found && factors_.has_value();
    }

private:
    friend SIQSPostMergeFactorResult
    extract_siqs_post_merge_factor(const SIQSPostMergeDependencyResult& dependency_result,
                                   const core::Integer& gcd_target);

    SIQSPostMergeFactorResult(SIQSPostMergeFactorStatus status,
                              std::optional<SIQSPostMergeFactorization> factors)
        : status_(status), factors_(std::move(factors)) {}

    [[nodiscard]] static SIQSPostMergeFactorResult failure(SIQSPostMergeFactorStatus status) {
        if (status == SIQSPostMergeFactorStatus::factor_found) {
            status = SIQSPostMergeFactorStatus::invalid_verified_dependency;
        }
        return SIQSPostMergeFactorResult(status, std::nullopt);
    }

    [[nodiscard]] static SIQSPostMergeFactorResult success(SIQSPostMergeFactorization factors) {
        return SIQSPostMergeFactorResult(SIQSPostMergeFactorStatus::factor_found,
                                         std::move(factors));
    }

    SIQSPostMergeFactorStatus status_;
    std::optional<SIQSPostMergeFactorization> factors_;
};

namespace post_merge_dependency_detail {

/// Untrusted intermediate returned by the generic arithmetic helper.  Only the
/// two exact public verifier overloads can turn this payload into a verified
/// result, so a caller-supplied accessor cannot forge the proof boundary.
struct DependencyEvaluation {
    SIQSPostMergeDependencyStatus status;
    std::optional<VerifiedSIQSPostMergeDependency> verified;
};

[[nodiscard]] inline DependencyEvaluation dependency_failure(SIQSPostMergeDependencyStatus status) {
    if (status == SIQSPostMergeDependencyStatus::valid) {
        status = SIQSPostMergeDependencyStatus::invalid_dependency;
    }
    return DependencyEvaluation{status, std::nullopt};
}

[[nodiscard]] inline DependencyEvaluation
dependency_success(VerifiedSIQSPostMergeDependency verified) {
    return DependencyEvaluation{SIQSPostMergeDependencyStatus::valid, std::move(verified)};
}

struct DirectRowAccessor {
    [[nodiscard]] const SIQSPostMergeRow& operator()(const SIQSPostMergeRow& row) const noexcept {
        return row;
    }
};

struct ShadowRowAccessor {
    [[nodiscard]] const SIQSPostMergeRow& operator()(const SIQSShadowRow& row) const noexcept {
        return row.row;
    }
};

[[nodiscard]] inline SIQSPostMergeDependencyStatus
map_row_status(SIQSPostMergeRowStatus status) noexcept {
    switch (status) {
    case SIQSPostMergeRowStatus::valid:
        return SIQSPostMergeDependencyStatus::valid;
    case SIQSPostMergeRowStatus::invalid_modulus:
        return SIQSPostMergeDependencyStatus::invalid_modulus;
    case SIQSPostMergeRowStatus::invalid_factor_base:
        return SIQSPostMergeDependencyStatus::invalid_factor_base;
    case SIQSPostMergeRowStatus::row_identity_mismatch:
        return SIQSPostMergeDependencyStatus::row_identity_mismatch;
    case SIQSPostMergeRowStatus::invalid_source_relation:
    case SIQSPostMergeRowStatus::invalid_materialized_cycle:
    case SIQSPostMergeRowStatus::invalid_post_merge_row:
    case SIQSPostMergeRowStatus::invalid_source_ids:
        return SIQSPostMergeDependencyStatus::invalid_row;
    }
    return SIQSPostMergeDependencyStatus::invalid_row;
}

inline void multiply_modulus(core::Integer& product, const core::Integer& factor,
                             const core::Integer& modulus) {
    mpz_mul(product.get_mpz(), product.get_mpz(), factor.get_mpz());
    mpz_mod(product.get_mpz(), product.get_mpz(), modulus.get_mpz());
}

[[nodiscard]] inline core::Integer modular_power(uint64_t base, uint64_t exponent,
                                                 const core::Integer& modulus) {
    return core::powmod(core::Integer(base), core::Integer(exponent), modulus);
}

template <class Row, class Accessor>
[[nodiscard]] DependencyEvaluation
verify_dependency(std::span<const Row> rows, std::span<const size_t> dependency,
                  std::span<const uint32_t> factor_base_primes, const core::Integer& square_modulus,
                  Accessor accessor) {
    if (!post_merge_row_detail::has_valid_modulus(square_modulus)) {
        return dependency_failure(SIQSPostMergeDependencyStatus::invalid_modulus);
    }
    if (!post_merge_row_detail::has_valid_factor_base(factor_base_primes)) {
        return dependency_failure(SIQSPostMergeDependencyStatus::invalid_factor_base);
    }
    if (dependency.empty()) {
        return dependency_failure(SIQSPostMergeDependencyStatus::invalid_dependency);
    }

    std::vector<size_t> canonical_dependency(dependency.begin(), dependency.end());
    std::sort(canonical_dependency.begin(), canonical_dependency.end());
    if (canonical_dependency.back() >= rows.size() ||
        std::adjacent_find(canonical_dependency.begin(), canonical_dependency.end()) !=
            canonical_dependency.end()) {
        return dependency_failure(SIQSPostMergeDependencyStatus::invalid_dependency);
    }

    std::vector<uint64_t> exponent_sums(factor_base_primes.size(), uint64_t{0});
    bool negative = false;
    for (const size_t row_index : canonical_dependency) {
        const SIQSPostMergeRow& row = accessor(rows[row_index]);
        const SIQSPostMergeRowStatus row_status =
            check_siqs_post_merge_row_identity(row, factor_base_primes, square_modulus);
        if (row_status != SIQSPostMergeRowStatus::valid) {
            return dependency_failure(map_row_status(row_status));
        }

        negative = negative != row.q_negative;
        for (const SIQSFactorPower& power : row.factor_powers) {
            uint64_t& sum = exponent_sums[power.factor_base_index];
            const uint64_t addend = static_cast<uint64_t>(power.exponent);
            if (addend > std::numeric_limits<uint64_t>::max() - sum) {
                return dependency_failure(SIQSPostMergeDependencyStatus::exponent_overflow);
            }
            sum += addend;
        }
    }

    if (negative || std::any_of(exponent_sums.begin(), exponent_sums.end(),
                                [](uint64_t exponent) { return (exponent & uint64_t{1}) != 0; })) {
        return dependency_failure(SIQSPostMergeDependencyStatus::dependency_not_square);
    }

    core::Integer x_modulus(1);
    core::Integer y_modulus(1);
    for (const size_t row_index : canonical_dependency) {
        const SIQSPostMergeRow& row = accessor(rows[row_index]);
        multiply_modulus(x_modulus, row.x_modulus, square_modulus);
        for (const uint64_t large_prime : row.large_prime_sqrt_factors) {
            const core::Integer factor(large_prime);
            multiply_modulus(y_modulus, factor, square_modulus);
        }
    }
    for (size_t i = 1; i < exponent_sums.size(); ++i) {
        const uint64_t half_exponent = exponent_sums[i] / 2;
        if (half_exponent == 0) {
            continue;
        }
        auto factor = modular_power(static_cast<uint64_t>(factor_base_primes[i]), half_exponent,
                                    square_modulus);
        multiply_modulus(y_modulus, factor, square_modulus);
    }

    if (!are_congruent_squares(x_modulus, y_modulus, square_modulus)) {
        return dependency_failure(SIQSPostMergeDependencyStatus::dependency_mismatch);
    }

    return dependency_success(
        VerifiedSIQSPostMergeDependency{std::move(canonical_dependency), std::move(x_modulus),
                                        std::move(y_modulus), square_modulus});
}

[[nodiscard]] inline bool is_nontrivial_factor(const core::Integer& factor,
                                               const core::Integer& target) {
    return mpz_cmp_ui(factor.get_mpz(), 1) > 0 && factor < target;
}

} // namespace post_merge_dependency_detail

/// Revalidate and reconstruct a dependency over canonical post-merge rows.
[[nodiscard]] inline SIQSPostMergeDependencyResult verify_siqs_post_merge_dependency(
    std::span<const SIQSPostMergeRow> rows, std::span<const size_t> dependency,
    std::span<const uint32_t> factor_base_primes, const core::Integer& square_modulus) {
    auto evaluation = post_merge_dependency_detail::verify_dependency(
        rows, dependency, factor_base_primes, square_modulus,
        post_merge_dependency_detail::DirectRowAccessor{});
    if (evaluation.status != SIQSPostMergeDependencyStatus::valid || !evaluation.verified) {
        return SIQSPostMergeDependencyResult::failure(evaluation.status);
    }
    return SIQSPostMergeDependencyResult::success(std::move(*evaluation.verified));
}

/// Revalidate and reconstruct a dependency over shadow rows.
[[nodiscard]] inline SIQSPostMergeDependencyResult verify_siqs_post_merge_dependency(
    std::span<const SIQSShadowRow> rows, std::span<const size_t> dependency,
    std::span<const uint32_t> factor_base_primes, const core::Integer& square_modulus) {
    auto evaluation = post_merge_dependency_detail::verify_dependency(
        rows, dependency, factor_base_primes, square_modulus,
        post_merge_dependency_detail::ShadowRowAccessor{});
    if (evaluation.status != SIQSPostMergeDependencyStatus::valid || !evaluation.verified) {
        return SIQSPostMergeDependencyResult::failure(evaluation.status);
    }
    return SIQSPostMergeDependencyResult::success(std::move(*evaluation.verified));
}

/// Extract the smallest canonical non-trivial factor pair from both GCD branches.
///
/// gcd_target may be the square modulus itself or a positive divisor such as
/// the original N when rows were verified modulo kN.
[[nodiscard]] inline SIQSPostMergeFactorResult
extract_siqs_post_merge_factor(const SIQSPostMergeDependencyResult& dependency_result,
                               const core::Integer& gcd_target) {
    if (!dependency_result.is_valid() || !dependency_result.verified()) {
        return SIQSPostMergeFactorResult::failure(
            SIQSPostMergeFactorStatus::invalid_verified_dependency);
    }
    if (!gcd_target.is_positive() || gcd_target.is_one()) {
        return SIQSPostMergeFactorResult::failure(SIQSPostMergeFactorStatus::invalid_target);
    }

    const VerifiedSIQSPostMergeDependency& verified = *dependency_result.verified();
    if (mpz_divisible_p(verified.square_modulus.get_mpz(), gcd_target.get_mpz()) == 0) {
        return SIQSPostMergeFactorResult::failure(SIQSPostMergeFactorStatus::target_not_divisor);
    }

    std::optional<SIQSPostMergeFactorization> smallest_pair;
    const auto consider = [&](core::Integer factor) {
        if (!post_merge_dependency_detail::is_nontrivial_factor(factor, gcd_target)) {
            return;
        }
        core::Integer cofactor = gcd_target / factor;
        if (cofactor < factor) {
            std::swap(factor, cofactor);
        }
        if (!smallest_pair || factor < smallest_pair->factor ||
            (factor == smallest_pair->factor && cofactor < smallest_pair->cofactor)) {
            smallest_pair = SIQSPostMergeFactorization{std::move(factor), std::move(cofactor)};
        }
    };

    core::Integer gcd_operand;
    mpz_sub(gcd_operand.get_mpz(), verified.x_modulus.get_mpz(), verified.y_modulus.get_mpz());
    consider(core::gcd(gcd_operand, gcd_target));
    mpz_add(gcd_operand.get_mpz(), verified.x_modulus.get_mpz(), verified.y_modulus.get_mpz());
    consider(core::gcd(gcd_operand, gcd_target));

    if (!smallest_pair) {
        return SIQSPostMergeFactorResult::failure(SIQSPostMergeFactorStatus::no_factor);
    }
    return SIQSPostMergeFactorResult::success(std::move(*smallest_pair));
}

} // namespace gnfs::siqs
