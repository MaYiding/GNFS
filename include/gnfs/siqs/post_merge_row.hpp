#pragma once

/// @file post_merge_row.hpp
/// @brief Canonical, wide SIQS rows after full/large-prime materialization.

#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/two_large_prime_congruence.hpp>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using std::size_t;

struct SIQSFactorPower {
    uint32_t factor_base_index;
    uint32_t exponent;

    [[nodiscard]] friend constexpr bool operator==(const SIQSFactorPower&,
                                                   const SIQSFactorPower&) = default;
};

struct SIQSSourceId {
    uint64_t value;

    [[nodiscard]] friend constexpr auto operator<=>(const SIQSSourceId&,
                                                    const SIQSSourceId&) = default;
};

/// Owning arithmetic and provenance payload consumed after relation merging.
///
/// Factor-base slot zero is deliberately absent from factor_powers: q_negative
/// owns the sign column.  large_prime_sqrt_factors is a sorted multiset, so a
/// repeated entry is meaningful and must not be deduplicated.
struct SIQSPostMergeRow {
    core::Integer x_modulus;
    bool q_negative;
    std::vector<SIQSFactorPower> factor_powers;
    std::vector<uint64_t> large_prime_sqrt_factors;
    std::vector<SIQSSourceId> source_ids;
};

enum class SIQSPostMergeRowStatus : uint8_t {
    valid,
    invalid_modulus,
    invalid_factor_base,
    invalid_source_relation,
    invalid_materialized_cycle,
    invalid_post_merge_row,
    invalid_source_ids,
    row_identity_mismatch,
};

namespace post_merge_row_detail {
struct SIQSPostMergeRowResultFactory;
}

/// Invariant-safe result: a row is present exactly when status() is valid.
class SIQSPostMergeRowResult {
public:
    SIQSPostMergeRowResult(const SIQSPostMergeRowResult&) = default;
    SIQSPostMergeRowResult& operator=(const SIQSPostMergeRowResult&) = default;

    SIQSPostMergeRowResult(SIQSPostMergeRowResult&& other) noexcept
        : status_(other.status_), row_(std::move(other.row_)) {
        other.status_ = SIQSPostMergeRowStatus::invalid_post_merge_row;
        other.row_.reset();
    }

    SIQSPostMergeRowResult& operator=(SIQSPostMergeRowResult&& other) noexcept {
        if (this != &other) {
            status_ = other.status_;
            row_ = std::move(other.row_);
            other.status_ = SIQSPostMergeRowStatus::invalid_post_merge_row;
            other.row_.reset();
        }
        return *this;
    }

    [[nodiscard]] SIQSPostMergeRowStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<SIQSPostMergeRow>& row() const noexcept {
        return row_;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return status_ == SIQSPostMergeRowStatus::valid && row_.has_value();
    }

private:
    friend struct post_merge_row_detail::SIQSPostMergeRowResultFactory;

    SIQSPostMergeRowResult(SIQSPostMergeRowStatus status, std::optional<SIQSPostMergeRow> row)
        : status_(status), row_(std::move(row)) {}

    SIQSPostMergeRowStatus status_;
    std::optional<SIQSPostMergeRow> row_;
};

namespace post_merge_row_detail {

struct SIQSPostMergeRowResultFactory {
    [[nodiscard]] static SIQSPostMergeRowResult failure(SIQSPostMergeRowStatus status) {
        if (status == SIQSPostMergeRowStatus::valid) {
            status = SIQSPostMergeRowStatus::invalid_post_merge_row;
        }
        return SIQSPostMergeRowResult(status, std::nullopt);
    }

    [[nodiscard]] static SIQSPostMergeRowResult success(SIQSPostMergeRow row) {
        return SIQSPostMergeRowResult(SIQSPostMergeRowStatus::valid, std::move(row));
    }
};

[[nodiscard]] inline bool has_valid_modulus(const core::Integer& modulus) {
    return modulus.is_positive() && !modulus.is_one();
}

[[nodiscard]] inline bool has_valid_factor_base(std::span<const uint32_t> factor_base_primes) {
    if (factor_base_primes.empty() || factor_base_primes.front() != 0 ||
        factor_base_primes.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return false;
    }
    for (size_t i = 1; i < factor_base_primes.size(); ++i) {
        if (factor_base_primes[i] < 2 ||
            (i > 1 && factor_base_primes[i - 1] >= factor_base_primes[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool has_valid_source_ids(std::span<const SIQSSourceId> source_ids) {
    if (source_ids.empty()) {
        return false;
    }
    for (size_t i = 1; i < source_ids.size(); ++i) {
        if (source_ids[i - 1] >= source_ids[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool has_valid_row_shape(const SIQSPostMergeRow& row, size_t factor_base_size,
                                              const core::Integer& modulus) {
    if (row.x_modulus.is_negative() || row.x_modulus >= modulus) {
        return false;
    }

    uint32_t previous_index = 0;
    for (const SIQSFactorPower& power : row.factor_powers) {
        if (power.factor_base_index == 0 ||
            static_cast<size_t>(power.factor_base_index) >= factor_base_size ||
            power.exponent == 0 || power.factor_base_index <= previous_index) {
            return false;
        }
        previous_index = power.factor_base_index;
    }

    for (size_t i = 0; i < row.large_prime_sqrt_factors.size(); ++i) {
        if (row.large_prime_sqrt_factors[i] < 2 ||
            (i > 0 && row.large_prime_sqrt_factors[i - 1] > row.large_prime_sqrt_factors[i])) {
            return false;
        }
    }
    return true;
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

/// Validate one row after the caller has established the shared modulus and
/// factor-base invariants.  Per-row structure, provenance, and exact arithmetic
/// remain fail-closed here; only the shared full scans are omitted.
[[nodiscard]] inline SIQSPostMergeRowStatus
check_siqs_post_merge_row_identity_prevalidated(const SIQSPostMergeRow& row,
                                                std::span<const uint32_t> factor_base_primes,
                                                const core::Integer& modulus) {
    if (!has_valid_source_ids(row.source_ids)) {
        return SIQSPostMergeRowStatus::invalid_source_ids;
    }
    if (!has_valid_row_shape(row, factor_base_primes.size(), modulus)) {
        return SIQSPostMergeRowStatus::invalid_post_merge_row;
    }

    core::Integer left;
    mpz_mul(left.get_mpz(), row.x_modulus.get_mpz(), row.x_modulus.get_mpz());
    mpz_mod(left.get_mpz(), left.get_mpz(), modulus.get_mpz());

    core::Integer right(1);
    for (const SIQSFactorPower& power : row.factor_powers) {
        auto factor =
            modular_power(static_cast<uint64_t>(factor_base_primes[power.factor_base_index]),
                          static_cast<uint64_t>(power.exponent), modulus);
        multiply_modulus(right, factor, modulus);
    }
    for (const uint64_t large_prime : row.large_prime_sqrt_factors) {
        auto factor = modular_power(large_prime, uint64_t{2}, modulus);
        multiply_modulus(right, factor, modulus);
    }
    if (row.q_negative) {
        mpz_neg(right.get_mpz(), right.get_mpz());
        mpz_mod(right.get_mpz(), right.get_mpz(), modulus.get_mpz());
    }

    return left == right ? SIQSPostMergeRowStatus::valid
                         : SIQSPostMergeRowStatus::row_identity_mismatch;
}

[[nodiscard]] inline bool has_valid_full_relation_shape(const SIQSRelation& relation,
                                                        size_t factor_base_size) {
    if (factor_base_size == 0 || relation.large_prime != 0 || relation.large_prime2 != 0 ||
        !relation.merge_lps.empty() || relation.exponents.size() != factor_base_size ||
        relation.exponents.front() != 0) {
        return false;
    }

    std::vector<uint32_t> indices = relation.fb_indices;
    std::sort(indices.begin(), indices.end());
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] == 0 || static_cast<size_t>(indices[i]) >= factor_base_size ||
            (i > 0 && indices[i - 1] == indices[i])) {
            return false;
        }
    }

    size_t sparse_position = 0;
    for (size_t exponent_index = 0; exponent_index < factor_base_size; ++exponent_index) {
        const bool is_listed = sparse_position < indices.size() &&
                               static_cast<size_t>(indices[sparse_position]) == exponent_index;
        if ((relation.exponents[exponent_index] > 0) != is_listed) {
            return false;
        }
        if (is_listed) {
            ++sparse_position;
        }
    }
    return sparse_position == indices.size();
}

template <class Exponents>
[[nodiscard]] inline std::vector<SIQSFactorPower>
compress_factor_powers(const Exponents& exponents) {
    std::vector<SIQSFactorPower> powers;
    if (exponents.size() <= 1) {
        return powers;
    }
    const size_t nonzero_count = static_cast<size_t>(std::count_if(
        exponents.begin() + 1, exponents.end(), [](const auto exponent) { return exponent != 0; }));
    powers.reserve(nonzero_count);
    for (size_t i = 1; i < exponents.size(); ++i) {
        if (exponents[i] != 0) {
            powers.push_back(
                SIQSFactorPower{static_cast<uint32_t>(i), static_cast<uint32_t>(exponents[i])});
        }
    }
    return powers;
}

[[nodiscard]] inline std::optional<std::vector<SIQSSourceId>>
canonical_source_ids(std::span<const SIQSSourceId> mapped_source_ids, size_t expected_size) {
    if (mapped_source_ids.size() != expected_size) {
        return std::nullopt;
    }
    std::vector<SIQSSourceId> source_ids(mapped_source_ids.begin(), mapped_source_ids.end());
    std::sort(source_ids.begin(), source_ids.end());
    if (!has_valid_source_ids(source_ids)) {
        return std::nullopt;
    }
    return source_ids;
}

[[nodiscard]] inline SIQSPostMergeRowStatus
map_materialized_status(TwoLargePrimeCongruenceStatus status) {
    switch (status) {
    case TwoLargePrimeCongruenceStatus::valid:
        return SIQSPostMergeRowStatus::valid;
    case TwoLargePrimeCongruenceStatus::invalid_modulus:
        return SIQSPostMergeRowStatus::invalid_modulus;
    case TwoLargePrimeCongruenceStatus::invalid_factor_base:
        return SIQSPostMergeRowStatus::invalid_factor_base;
    case TwoLargePrimeCongruenceStatus::row_identity_mismatch:
        return SIQSPostMergeRowStatus::row_identity_mismatch;
    case TwoLargePrimeCongruenceStatus::invalid_materialized_cycle:
    case TwoLargePrimeCongruenceStatus::invalid_dependency:
    case TwoLargePrimeCongruenceStatus::exponent_overflow:
    case TwoLargePrimeCongruenceStatus::dependency_not_square:
    case TwoLargePrimeCongruenceStatus::dependency_mismatch:
        return SIQSPostMergeRowStatus::invalid_materialized_cycle;
    }
    return SIQSPostMergeRowStatus::invalid_materialized_cycle;
}

} // namespace post_merge_row_detail

/// Validate the exact signed factorization identity carried by one canonical row.
[[nodiscard]] inline SIQSPostMergeRowStatus
check_siqs_post_merge_row_identity(const SIQSPostMergeRow& row,
                                   std::span<const uint32_t> factor_base_primes,
                                   const core::Integer& modulus) {
    using namespace post_merge_row_detail;

    if (!has_valid_modulus(modulus)) {
        return SIQSPostMergeRowStatus::invalid_modulus;
    }
    if (!has_valid_factor_base(factor_base_primes)) {
        return SIQSPostMergeRowStatus::invalid_factor_base;
    }
    return check_siqs_post_merge_row_identity_prevalidated(row, factor_base_primes, modulus);
}

/// Convert one raw, fully smooth SIQS relation without narrowing its arithmetic.
[[nodiscard]] inline SIQSPostMergeRowResult
make_full_post_merge_row(const SIQSRelation& relation, SIQSSourceId source_id,
                         std::span<const uint32_t> factor_base_primes,
                         const core::Integer& modulus) {
    using post_merge_row_detail::SIQSPostMergeRowResultFactory;

    if (!post_merge_row_detail::has_valid_modulus(modulus)) {
        return SIQSPostMergeRowResultFactory::failure(SIQSPostMergeRowStatus::invalid_modulus);
    }
    if (!post_merge_row_detail::has_valid_factor_base(factor_base_primes)) {
        return SIQSPostMergeRowResultFactory::failure(SIQSPostMergeRowStatus::invalid_factor_base);
    }
    if (!post_merge_row_detail::has_valid_full_relation_shape(relation,
                                                              factor_base_primes.size())) {
        return SIQSPostMergeRowResultFactory::failure(
            SIQSPostMergeRowStatus::invalid_source_relation);
    }

    core::Integer x_modulus;
    mpz_mod(x_modulus.get_mpz(), relation.value.get_mpz(), modulus.get_mpz());
    SIQSPostMergeRow row{std::move(x_modulus),
                         relation.negative,
                         post_merge_row_detail::compress_factor_powers(relation.exponents),
                         {},
                         {source_id}};

    const SIQSPostMergeRowStatus status =
        check_siqs_post_merge_row_identity(row, factor_base_primes, modulus);
    if (status != SIQSPostMergeRowStatus::valid) {
        return SIQSPostMergeRowResultFactory::failure(status);
    }
    return SIQSPostMergeRowResultFactory::success(std::move(row));
}

/// Move-aware full-relation conversion; value is canonicalized in place.
[[nodiscard]] inline SIQSPostMergeRowResult
make_full_post_merge_row(SIQSRelation&& relation, SIQSSourceId source_id,
                         std::span<const uint32_t> factor_base_primes,
                         const core::Integer& modulus) {
    using post_merge_row_detail::SIQSPostMergeRowResultFactory;

    if (!post_merge_row_detail::has_valid_modulus(modulus)) {
        return SIQSPostMergeRowResultFactory::failure(SIQSPostMergeRowStatus::invalid_modulus);
    }
    if (!post_merge_row_detail::has_valid_factor_base(factor_base_primes)) {
        return SIQSPostMergeRowResultFactory::failure(SIQSPostMergeRowStatus::invalid_factor_base);
    }
    if (!post_merge_row_detail::has_valid_full_relation_shape(relation,
                                                              factor_base_primes.size())) {
        return SIQSPostMergeRowResultFactory::failure(
            SIQSPostMergeRowStatus::invalid_source_relation);
    }

    auto powers = post_merge_row_detail::compress_factor_powers(relation.exponents);
    const bool q_negative = relation.negative;
    mpz_mod(relation.value.get_mpz(), relation.value.get_mpz(), modulus.get_mpz());
    SIQSPostMergeRow row{std::move(relation.value), q_negative, std::move(powers), {}, {source_id}};

    const SIQSPostMergeRowStatus status =
        check_siqs_post_merge_row_identity(row, factor_base_primes, modulus);
    if (status != SIQSPostMergeRowStatus::valid) {
        return SIQSPostMergeRowResultFactory::failure(status);
    }
    return SIQSPostMergeRowResultFactory::success(std::move(row));
}

/// Convert one already-materialized 1LP/2LP cycle and remap its provenance.
[[nodiscard]] inline SIQSPostMergeRowResult make_cycle_post_merge_row(
    const MaterializedTwoLargePrimeCycle& cycle, std::span<const SIQSSourceId> mapped_source_ids,
    std::span<const uint32_t> factor_base_primes, const core::Integer& modulus) {
    using post_merge_row_detail::SIQSPostMergeRowResultFactory;

    const auto materialized_status =
        check_materialized_two_large_prime_identity(cycle, factor_base_primes, modulus);
    if (materialized_status != TwoLargePrimeCongruenceStatus::valid) {
        return SIQSPostMergeRowResultFactory::failure(
            post_merge_row_detail::map_materialized_status(materialized_status));
    }

    auto source_ids = post_merge_row_detail::canonical_source_ids(mapped_source_ids,
                                                                  cycle.relation_indices.size());
    if (!source_ids) {
        return SIQSPostMergeRowResultFactory::failure(SIQSPostMergeRowStatus::invalid_source_ids);
    }

    SIQSPostMergeRow row{cycle.value_modulus, cycle.negative,
                         post_merge_row_detail::compress_factor_powers(cycle.factor_base_exponents),
                         cycle.large_prime_square_roots, std::move(*source_ids)};
    const SIQSPostMergeRowStatus status =
        check_siqs_post_merge_row_identity(row, factor_base_primes, modulus);
    if (status != SIQSPostMergeRowStatus::valid) {
        return SIQSPostMergeRowResultFactory::failure(status);
    }
    return SIQSPostMergeRowResultFactory::success(std::move(row));
}

/// Move-aware cycle conversion; the wide value and repeated LP roots are retained.
[[nodiscard]] inline SIQSPostMergeRowResult make_cycle_post_merge_row(
    MaterializedTwoLargePrimeCycle&& cycle, std::span<const SIQSSourceId> mapped_source_ids,
    std::span<const uint32_t> factor_base_primes, const core::Integer& modulus) {
    using post_merge_row_detail::SIQSPostMergeRowResultFactory;

    const auto materialized_status =
        check_materialized_two_large_prime_identity(cycle, factor_base_primes, modulus);
    if (materialized_status != TwoLargePrimeCongruenceStatus::valid) {
        return SIQSPostMergeRowResultFactory::failure(
            post_merge_row_detail::map_materialized_status(materialized_status));
    }

    auto source_ids = post_merge_row_detail::canonical_source_ids(mapped_source_ids,
                                                                  cycle.relation_indices.size());
    if (!source_ids) {
        return SIQSPostMergeRowResultFactory::failure(SIQSPostMergeRowStatus::invalid_source_ids);
    }

    auto powers = post_merge_row_detail::compress_factor_powers(cycle.factor_base_exponents);
    const bool q_negative = cycle.negative;
    SIQSPostMergeRow row{std::move(cycle.value_modulus), q_negative, std::move(powers),
                         std::move(cycle.large_prime_square_roots), std::move(*source_ids)};
    const SIQSPostMergeRowStatus status =
        check_siqs_post_merge_row_identity(row, factor_base_primes, modulus);
    if (status != SIQSPostMergeRowStatus::valid) {
        return SIQSPostMergeRowResultFactory::failure(status);
    }
    return SIQSPostMergeRowResultFactory::success(std::move(row));
}

/// Visit the sign column followed by each odd factor-base column.
///
/// The caller must supply a canonical row returned by a successful conversion
/// or accepted by check_siqs_post_merge_row_identity().  This hot-path helper
/// deliberately does not repeat structural or arithmetic validation.
template <class Visitor>
inline void visit_siqs_post_merge_odd_columns(const SIQSPostMergeRow& row, Visitor&& visitor) {
    if (row.q_negative) {
        std::invoke(visitor, size_t{0});
    }
    for (const SIQSFactorPower& power : row.factor_powers) {
        if ((power.exponent & uint32_t{1}) != 0) {
            std::invoke(visitor, static_cast<size_t>(power.factor_base_index));
        }
    }
}

} // namespace gnfs::siqs
