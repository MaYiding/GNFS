#pragma once

/// @file two_large_prime_adapter.hpp
/// @brief Validate and prepare raw SIQS relations for the 2LP graph boundary.

#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/two_large_prime.hpp>
#include <gnfs/siqs/two_large_prime_graph.hpp>
#include <gnfs/siqs/two_large_prime_materializer.hpp>
#include <gnfs/util/primes.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using std::size_t;

struct TwoLargePrimeAdapterStats {
    size_t input_relations = 0;
    size_t full_relations = 0;
    size_t accepted_one_lp = 0;
    size_t accepted_two_lp = 0;
    size_t rejected_relations = 0;
    size_t malformed_source_shape = 0;
    size_t unsupported_encoding = 0;
    size_t invalid_one_large_prime = 0;
    size_t invalid_two_large_prime_split = 0;
    size_t exact_duplicate = 0;

    [[nodiscard]] constexpr size_t typed_rejections() const noexcept {
        return malformed_source_shape + unsupported_encoding +
               invalid_one_large_prime + invalid_two_large_prime_split +
               exact_duplicate;
    }

    [[nodiscard]] friend constexpr bool operator==(
        const TwoLargePrimeAdapterStats&,
        const TwoLargePrimeAdapterStats&) = default;
};

struct PreparedTwoLargePrimeCorpus {
    std::vector<TwoLargePrimeEdge> edges;
    std::vector<TwoLargePrimeCycleSource> sources;
    TwoLargePrimeAdapterStats stats;
};

namespace two_large_prime_adapter_detail {

enum class RejectionReason {
    malformed_source_shape,
    unsupported_encoding,
    invalid_one_large_prime,
    invalid_two_large_prime_split,
    exact_duplicate,
};

inline void record_rejection(TwoLargePrimeAdapterStats& stats,
                             RejectionReason reason) noexcept {
    ++stats.rejected_relations;
    switch (reason) {
    case RejectionReason::malformed_source_shape:
        ++stats.malformed_source_shape;
        break;
    case RejectionReason::unsupported_encoding:
        ++stats.unsupported_encoding;
        break;
    case RejectionReason::invalid_one_large_prime:
        ++stats.invalid_one_large_prime;
        break;
    case RejectionReason::invalid_two_large_prime_split:
        ++stats.invalid_two_large_prime_split;
        break;
    case RejectionReason::exact_duplicate:
        ++stats.exact_duplicate;
        break;
    }
}

struct AcceptedRelation {
    const SIQSRelation* relation;
    uint64_t p;
    uint64_t q;
};

[[nodiscard]] inline bool has_valid_raw_shape(
        const SIQSRelation& relation,
        size_t factor_base_size) {
    if (factor_base_size == 0 ||
        !relation.merge_lps.empty() ||
        relation.exponents.size() != factor_base_size ||
        relation.exponents[0] != 0) {
        return false;
    }

    // fb_indices is a sparse set describing exactly the nonzero entries in
    // the dense exponent vector. Accept any input order, but reject duplicate,
    // out-of-range, missing, or spurious indices.
    std::vector<uint32_t> indices = relation.fb_indices;
    std::sort(indices.begin(), indices.end());
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] == 0 ||
            static_cast<size_t>(indices[i]) >= factor_base_size ||
            (i > 0 && indices[i - 1] == indices[i])) {
            return false;
        }
    }

    size_t sparse_position = 0;
    for (size_t exponent_index = 0;
         exponent_index < factor_base_size;
         ++exponent_index) {
        const bool is_listed =
            sparse_position < indices.size() &&
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

[[nodiscard]] inline bool accepted_relation_less(
        const AcceptedRelation& lhs,
        const AcceptedRelation& rhs) {
    if (lhs.p != rhs.p) {
        return lhs.p < rhs.p;
    }
    if (lhs.q != rhs.q) {
        return lhs.q < rhs.q;
    }

    const int value_order = lhs.relation->value.compare(rhs.relation->value);
    if (value_order != 0) {
        return value_order < 0;
    }
    if (lhs.relation->negative != rhs.relation->negative) {
        return !lhs.relation->negative;
    }
    return std::lexicographical_compare(
        lhs.relation->exponents.begin(), lhs.relation->exponents.end(),
        rhs.relation->exponents.begin(), rhs.relation->exponents.end());
}

[[nodiscard]] inline bool accepted_relation_equal(
        const AcceptedRelation& lhs,
        const AcceptedRelation& rhs) {
    return lhs.p == rhs.p &&
           lhs.q == rhs.q &&
           lhs.relation->value == rhs.relation->value &&
           lhs.relation->negative == rhs.relation->negative &&
           lhs.relation->exponents == rhs.relation->exponents;
}

[[nodiscard]] inline std::vector<uint32_t> widen_exponents(
        const std::vector<uint8_t>& exponents) {
    std::vector<uint32_t> widened;
    widened.reserve(exponents.size());
    for (const uint8_t exponent : exponents) {
        widened.push_back(static_cast<uint32_t>(exponent));
    }
    return widened;
}

} // namespace two_large_prime_adapter_detail

/// Validate raw SIQS relation encodings and prepare accepted partials for the
/// graph and materialization primitives.
///
/// `splitter` must be a deterministic pure function of the cofactor: accepted
/// records are sorted after normalization, but call-order-dependent factor
/// candidates would still change which relations cross the validation boundary.
/// This adapter is intentionally not connected to the production SIQS pipeline
/// yet.
template <class Splitter>
[[nodiscard]] std::optional<PreparedTwoLargePrimeCorpus>
prepare_two_large_prime_corpus(
        std::span<const SIQSRelation> relations,
        size_t factor_base_size,
        uint64_t large_prime_bound,
        Splitter&& splitter) {
    using two_large_prime_adapter_detail::AcceptedRelation;

    if (factor_base_size == 0 || large_prime_bound < 2) {
        return std::nullopt;
    }

    PreparedTwoLargePrimeCorpus corpus;
    corpus.stats.input_relations = relations.size();

    std::vector<AcceptedRelation> accepted;
    accepted.reserve(relations.size());

    for (const SIQSRelation& relation : relations) {
        if (!two_large_prime_adapter_detail::has_valid_raw_shape(
                relation, factor_base_size)) {
            two_large_prime_adapter_detail::record_rejection(
                corpus.stats,
                two_large_prime_adapter_detail::RejectionReason::malformed_source_shape);
            continue;
        }

        if (relation.large_prime == 0 && relation.large_prime2 == 0) {
            ++corpus.stats.full_relations;
            continue;
        }

        if (relation.large_prime > 1 && relation.large_prime2 == 0) {
            if (relation.large_prime <= large_prime_bound &&
                gnfs::util::is_prime_u64(relation.large_prime)) {
                accepted.push_back(AcceptedRelation{
                    &relation, 0, relation.large_prime});
            } else {
                two_large_prime_adapter_detail::record_rejection(
                    corpus.stats,
                    two_large_prime_adapter_detail::RejectionReason::invalid_one_large_prime);
            }
            continue;
        }

        if (relation.large_prime > 1 && relation.large_prime2 == 1) {
            const auto candidate = std::invoke(splitter, relation.large_prime);
            const auto factors = normalize_two_large_prime(
                relation.large_prime, large_prime_bound, candidate);
            if (factors) {
                accepted.push_back(AcceptedRelation{
                    &relation, factors->p, factors->q});
            } else {
                two_large_prime_adapter_detail::record_rejection(
                    corpus.stats,
                    two_large_prime_adapter_detail::RejectionReason::invalid_two_large_prime_split);
            }
            continue;
        }

        // Pre-split lp1/lp2 records and all other encodings are outside this
        // raw adapter's accepted input contract.
        two_large_prime_adapter_detail::record_rejection(
            corpus.stats,
            two_large_prime_adapter_detail::RejectionReason::unsupported_encoding);
    }

    std::sort(accepted.begin(), accepted.end(),
              two_large_prime_adapter_detail::accepted_relation_less);

    corpus.edges.reserve(accepted.size());
    corpus.sources.reserve(accepted.size());
    const AcceptedRelation* previous_relation = nullptr;
    for (const AcceptedRelation& accepted_relation : accepted) {
        if (previous_relation != nullptr &&
            two_large_prime_adapter_detail::accepted_relation_equal(
                *previous_relation, accepted_relation)) {
            two_large_prime_adapter_detail::record_rejection(
                corpus.stats,
                two_large_prime_adapter_detail::RejectionReason::exact_duplicate);
            continue;
        }
        previous_relation = &accepted_relation;

        if (accepted_relation.p == 0) {
            ++corpus.stats.accepted_one_lp;
        } else {
            ++corpus.stats.accepted_two_lp;
        }

        const size_t relation_index = corpus.sources.size();
        const SIQSRelation& relation = *accepted_relation.relation;

        corpus.edges.push_back(TwoLargePrimeEdge{
            accepted_relation.p,
            accepted_relation.q,
            relation_index,
        });

        // Copy the arbitrary-precision value exactly once into the accepted
        // source. The temporary source is then moved into reserved storage.
        TwoLargePrimeCycleSource source{
            relation_index,
            relation.value,
            relation.negative,
            two_large_prime_adapter_detail::widen_exponents(relation.exponents),
            accepted_relation.p,
            accepted_relation.q,
        };
        corpus.sources.push_back(std::move(source));
    }

    return std::optional<PreparedTwoLargePrimeCorpus>{std::move(corpus)};
}

} // namespace gnfs::siqs
