#pragma once

/// @file two_large_prime_materializer.hpp
/// @brief Materialize one validated SIQS two-large-prime graph cycle.

#include <gnfs/core/integer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using std::size_t;

/// Relation data needed to materialize a selected 1LP/2LP graph cycle.
struct TwoLargePrimeCycleSource {
    size_t relation_index;
    core::Integer value;
    bool negative;
    std::vector<uint32_t> factor_base_exponents;
    uint64_t p;
    uint64_t q;
};

/// Combined data produced from one valid 1LP/2LP graph cycle.
struct MaterializedTwoLargePrimeCycle {
    core::Integer value_modulus;
    bool negative;
    std::vector<uint32_t> factor_base_exponents;
    std::vector<uint64_t> large_prime_square_roots;
    std::vector<size_t> relation_indices;
};

enum class TwoLargePrimeMaterializationStatus : uint8_t {
    valid,
    invalid_modulus,
    invalid_source_catalog,
    invalid_cycle_support,
    invalid_source_shape,
    size_overflow,
    exponent_overflow,
    odd_large_prime_degree,
    internal_invariant_failure,
};

namespace two_large_prime_materializer_detail {
struct TwoLargePrimeMaterializationResultFactory;
}

/// Invariant-safe result: a materialized cycle is present exactly when
/// status() is valid.
class TwoLargePrimeMaterializationResult {
public:
    TwoLargePrimeMaterializationResult(const TwoLargePrimeMaterializationResult&) = default;
    TwoLargePrimeMaterializationResult& operator=(const TwoLargePrimeMaterializationResult& other) {
        if (this != &other) {
            TwoLargePrimeMaterializationResult replacement(other);
            *this = std::move(replacement);
        }
        return *this;
    }

    TwoLargePrimeMaterializationResult(TwoLargePrimeMaterializationResult&& other) noexcept
        : status_(other.status_), materialized_cycle_(std::move(other.materialized_cycle_)) {
        other.status_ = TwoLargePrimeMaterializationStatus::internal_invariant_failure;
        other.materialized_cycle_.reset();
    }

    TwoLargePrimeMaterializationResult&
    operator=(TwoLargePrimeMaterializationResult&& other) noexcept {
        if (this != &other) {
            status_ = other.status_;
            materialized_cycle_ = std::move(other.materialized_cycle_);
            other.status_ = TwoLargePrimeMaterializationStatus::internal_invariant_failure;
            other.materialized_cycle_.reset();
        }
        return *this;
    }

    [[nodiscard]] TwoLargePrimeMaterializationStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<MaterializedTwoLargePrimeCycle>&
    materialized_cycle() const& noexcept {
        return materialized_cycle_;
    }

    /// Transfer a checked payload out of a temporary result.
    ///
    /// The consumed result is reset to internal_invariant_failure so the
    /// payload/status invariant remains true even if the moved-from object is
    /// observed.
    [[nodiscard]] std::optional<MaterializedTwoLargePrimeCycle> materialized_cycle() && noexcept {
        std::optional<MaterializedTwoLargePrimeCycle> result(std::move(materialized_cycle_));
        materialized_cycle_.reset();
        status_ = TwoLargePrimeMaterializationStatus::internal_invariant_failure;
        return result;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return status_ == TwoLargePrimeMaterializationStatus::valid &&
               materialized_cycle_.has_value();
    }

private:
    friend struct two_large_prime_materializer_detail::TwoLargePrimeMaterializationResultFactory;

    TwoLargePrimeMaterializationResult(
        TwoLargePrimeMaterializationStatus status,
        std::optional<MaterializedTwoLargePrimeCycle> materialized_cycle)
        : status_(status), materialized_cycle_(std::move(materialized_cycle)) {}

    TwoLargePrimeMaterializationStatus status_;
    std::optional<MaterializedTwoLargePrimeCycle> materialized_cycle_;
};

/// An immutable source corpus whose relation identifiers are exactly [0, size).
///
/// Construction performs the only full-corpus validation pass. The private
/// constructor prevents callers from asserting the indexed layout without that
/// pass. The corpus owns its source storage so validation cannot be invalidated by
/// a caller mutating, reallocating, or destroying the original corpus.
///
/// Only relation identity is a corpus-wide invariant. Endpoint and exponent
/// validation remains scoped to sources selected by a materialized cycle, so a
/// malformed unselected source has the same behavior as in the generic API.
class IndexedTwoLargePrimeCycleSources final {
public:
    IndexedTwoLargePrimeCycleSources(const IndexedTwoLargePrimeCycleSources&) = delete;
    IndexedTwoLargePrimeCycleSources& operator=(const IndexedTwoLargePrimeCycleSources&) = delete;
    IndexedTwoLargePrimeCycleSources(IndexedTwoLargePrimeCycleSources&&) noexcept = default;
    IndexedTwoLargePrimeCycleSources&
    operator=(IndexedTwoLargePrimeCycleSources&&) noexcept = default;

    [[nodiscard]] static std::optional<IndexedTwoLargePrimeCycleSources>
    try_create(std::vector<TwoLargePrimeCycleSource> sources) noexcept {
        for (size_t relation_index = 0; relation_index < sources.size(); ++relation_index) {
            if (sources[relation_index].relation_index != relation_index) {
                return std::nullopt;
            }
        }
        return IndexedTwoLargePrimeCycleSources(std::move(sources));
    }

    [[nodiscard]] size_t size() const noexcept {
        return sources_.size();
    }

    [[nodiscard]] const TwoLargePrimeCycleSource* source_at(size_t relation_index) const noexcept {
        return relation_index < sources_.size() ? &sources_[relation_index] : nullptr;
    }

private:
    explicit IndexedTwoLargePrimeCycleSources(
        std::vector<TwoLargePrimeCycleSource> sources) noexcept
        : sources_(std::move(sources)) {}

    std::vector<TwoLargePrimeCycleSource> sources_;
};

namespace two_large_prime_materializer_detail {

struct TwoLargePrimeMaterializationResultFactory {
    [[nodiscard]] static TwoLargePrimeMaterializationResult
    failure(TwoLargePrimeMaterializationStatus status) {
        if (status == TwoLargePrimeMaterializationStatus::valid) {
            status = TwoLargePrimeMaterializationStatus::internal_invariant_failure;
        }
        return TwoLargePrimeMaterializationResult(status, std::nullopt);
    }

    [[nodiscard]] static TwoLargePrimeMaterializationResult
    success(MaterializedTwoLargePrimeCycle materialized_cycle) {
        return TwoLargePrimeMaterializationResult(TwoLargePrimeMaterializationStatus::valid,
                                                  std::move(materialized_cycle));
    }
};

[[nodiscard]] inline TwoLargePrimeMaterializationResult materialize_selected_two_large_prime_cycle(
    std::span<const TwoLargePrimeCycleSource* const> selected_sources,
    std::vector<size_t> relation_indices, const core::Integer& modulus) {
    using Factory = TwoLargePrimeMaterializationResultFactory;

    if (!modulus.is_positive() || modulus.is_one()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_modulus);
    }
    if (selected_sources.empty()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_cycle_support);
    }
    if (selected_sources.size() != relation_indices.size()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::internal_invariant_failure);
    }
    if (selected_sources.front() == nullptr) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::internal_invariant_failure);
    }

    const size_t exponent_count = selected_sources.front()->factor_base_exponents.size();
    std::vector<uint32_t> factor_base_exponents;
    if (exponent_count > factor_base_exponents.max_size()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::size_overflow);
    }
    factor_base_exponents.resize(exponent_count, 0);

    std::vector<uint64_t> large_prime_incidence;
    if (selected_sources.size() > large_prime_incidence.max_size() / 2) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::size_overflow);
    }
    large_prime_incidence.reserve(selected_sources.size() * 2);

    core::Integer value_modulus(1);
    bool negative = false;
    for (const auto* source : selected_sources) {
        if (source == nullptr) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::internal_invariant_failure);
        }
        if (source->factor_base_exponents.size() != exponent_count) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_source_shape);
        }

        const uint64_t lower = std::min(source->p, source->q);
        const uint64_t upper = std::max(source->p, source->q);
        if ((lower == 0 && upper < 2) || (lower != 0 && lower < 2)) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_source_shape);
        }

        for (size_t i = 0; i < exponent_count; ++i) {
            const uint32_t addend = source->factor_base_exponents[i];
            if (addend > std::numeric_limits<uint32_t>::max() - factor_base_exponents[i]) {
                return Factory::failure(TwoLargePrimeMaterializationStatus::exponent_overflow);
            }
            factor_base_exponents[i] += addend;
        }

        mpz_mul(value_modulus.get_mpz(), value_modulus.get_mpz(), source->value.get_mpz());
        // Integer::mod uses truncating remainder. Materialized values use the
        // canonical non-negative residue required by modular arithmetic.
        mpz_mod(value_modulus.get_mpz(), value_modulus.get_mpz(), modulus.get_mpz());

        negative = negative != source->negative;
        if (source->p != 0) {
            large_prime_incidence.push_back(source->p);
        }
        if (source->q != 0) {
            // For a self-loop p == q this deliberately records two incidences.
            large_prime_incidence.push_back(source->q);
        }
    }

    std::sort(large_prime_incidence.begin(), large_prime_incidence.end());
    std::vector<uint64_t> large_prime_square_roots;
    if (large_prime_incidence.size() / 2 > large_prime_square_roots.max_size()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::size_overflow);
    }
    large_prime_square_roots.reserve(large_prime_incidence.size() / 2);
    for (size_t begin = 0; begin < large_prime_incidence.size();) {
        size_t end = begin + 1;
        while (end < large_prime_incidence.size() &&
               large_prime_incidence[end] == large_prime_incidence[begin]) {
            ++end;
        }

        const size_t degree = end - begin;
        if ((degree & 1U) != 0) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::odd_large_prime_degree);
        }
        large_prime_square_roots.insert(large_prime_square_roots.end(), degree / 2,
                                        large_prime_incidence[begin]);
        begin = end;
    }

    return Factory::success(MaterializedTwoLargePrimeCycle{
        std::move(value_modulus), negative, std::move(factor_base_exponents),
        std::move(large_prime_square_roots), std::move(relation_indices)});
}

} // namespace two_large_prime_materializer_detail

/// Materialize the relation data belonging to one graph cycle from a generic
/// source corpus.
///
/// This is a strict structural and arithmetic boundary, but not a cofactor or
/// congruence validator: endpoint primality and bounds belong to the adapter;
/// call check_materialized_two_large_prime_identity() before matrix admission.
/// Endpoint zero is accepted only as the virtual endpoint of a 1LP edge.
/// All selected sources must come from the same modulus and the same ordered
/// factor base; this function can verify the vector width but not that identity.
///
/// Unselected sources may use different factor-base vector dimensions; only
/// their globally unique relation identifiers are relevant to this operation.
[[nodiscard]] inline TwoLargePrimeMaterializationResult
materialize_two_large_prime_cycle_checked(std::span<const TwoLargePrimeCycleSource> sources,
                                          std::span<const size_t> cycle_relation_indices,
                                          const core::Integer& modulus) {
    using Factory = two_large_prime_materializer_detail::TwoLargePrimeMaterializationResultFactory;

    if (!modulus.is_positive() || modulus.is_one()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_modulus);
    }
    if (cycle_relation_indices.empty()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_cycle_support);
    }

    // Sort source references by identifier so lookup and all later processing
    // are independent of source and cycle input order.
    std::vector<size_t> source_order;
    if (sources.size() > source_order.max_size()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::size_overflow);
    }
    source_order.resize(sources.size());
    std::iota(source_order.begin(), source_order.end(), size_t{0});
    std::sort(source_order.begin(), source_order.end(), [&sources](size_t lhs, size_t rhs) {
        return sources[lhs].relation_index < sources[rhs].relation_index;
    });
    for (size_t i = 1; i < source_order.size(); ++i) {
        if (sources[source_order[i - 1]].relation_index ==
            sources[source_order[i]].relation_index) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_source_catalog);
        }
    }

    std::vector<size_t> relation_indices;
    if (cycle_relation_indices.size() > relation_indices.max_size()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::size_overflow);
    }
    relation_indices.assign(cycle_relation_indices.begin(), cycle_relation_indices.end());
    std::sort(relation_indices.begin(), relation_indices.end());
    if (std::adjacent_find(relation_indices.begin(), relation_indices.end()) !=
        relation_indices.end()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_cycle_support);
    }

    std::vector<const TwoLargePrimeCycleSource*> selected_sources;
    if (relation_indices.size() > selected_sources.max_size()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::size_overflow);
    }
    selected_sources.reserve(relation_indices.size());
    for (const size_t relation_index : relation_indices) {
        const auto position = std::lower_bound(
            source_order.begin(), source_order.end(), relation_index,
            [&sources](size_t source_index, size_t target_relation_index) {
                return sources[source_index].relation_index < target_relation_index;
            });
        if (position == source_order.end() || sources[*position].relation_index != relation_index) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_source_catalog);
        }
        selected_sources.push_back(&sources[*position]);
    }

    return two_large_prime_materializer_detail::materialize_selected_two_large_prime_cycle(
        selected_sources, std::move(relation_indices), modulus);
}

/// Compatibility wrapper for callers that need only success/failure.
[[nodiscard]] inline std::optional<MaterializedTwoLargePrimeCycle>
materialize_two_large_prime_cycle(std::span<const TwoLargePrimeCycleSource> sources,
                                  std::span<const size_t> cycle_relation_indices,
                                  const core::Integer& modulus) {
    auto result =
        materialize_two_large_prime_cycle_checked(sources, cycle_relation_indices, modulus);
    return std::move(result).materialized_cycle();
}

/// Materialize one already-sorted graph cycle from a validated indexed source
/// corpus. Lookup is direct by relation_index and visits only the selected
/// cycle sources. The cycle must be strictly increasing; unsorted, duplicate,
/// out-of-range, or internally inconsistent source identities fail closed.
[[nodiscard]] inline TwoLargePrimeMaterializationResult
materialize_two_large_prime_cycle_checked(const IndexedTwoLargePrimeCycleSources& sources,
                                          std::span<const size_t> sorted_cycle_relation_indices,
                                          const core::Integer& modulus) {
    using Factory = two_large_prime_materializer_detail::TwoLargePrimeMaterializationResultFactory;

    if (!modulus.is_positive() || modulus.is_one()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_modulus);
    }
    if (sorted_cycle_relation_indices.empty()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_cycle_support);
    }

    std::vector<const TwoLargePrimeCycleSource*> selected_sources;
    if (sorted_cycle_relation_indices.size() > selected_sources.max_size()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::size_overflow);
    }
    selected_sources.reserve(sorted_cycle_relation_indices.size());
    std::vector<size_t> relation_indices;
    if (sorted_cycle_relation_indices.size() > relation_indices.max_size()) {
        return Factory::failure(TwoLargePrimeMaterializationStatus::size_overflow);
    }
    relation_indices.reserve(sorted_cycle_relation_indices.size());
    size_t previous_relation_index = 0;
    for (size_t cycle_position = 0; cycle_position < sorted_cycle_relation_indices.size();
         ++cycle_position) {
        const size_t relation_index = sorted_cycle_relation_indices[cycle_position];
        if (cycle_position != 0 && relation_index <= previous_relation_index) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_cycle_support);
        }
        if (relation_index >= sources.size()) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_source_catalog);
        }
        const TwoLargePrimeCycleSource* source = sources.source_at(relation_index);
        if (source == nullptr) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::invalid_source_catalog);
        }
        if (source->relation_index != relation_index) {
            return Factory::failure(TwoLargePrimeMaterializationStatus::internal_invariant_failure);
        }
        selected_sources.push_back(source);
        relation_indices.push_back(relation_index);
        previous_relation_index = relation_index;
    }

    return two_large_prime_materializer_detail::materialize_selected_two_large_prime_cycle(
        selected_sources, std::move(relation_indices), modulus);
}

/// Compatibility wrapper for callers that need only success/failure.
[[nodiscard]] inline std::optional<MaterializedTwoLargePrimeCycle>
materialize_two_large_prime_cycle(const IndexedTwoLargePrimeCycleSources& sources,
                                  std::span<const size_t> sorted_cycle_relation_indices,
                                  const core::Integer& modulus) {
    auto result =
        materialize_two_large_prime_cycle_checked(sources, sorted_cycle_relation_indices, modulus);
    return std::move(result).materialized_cycle();
}

} // namespace gnfs::siqs
