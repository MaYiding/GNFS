#pragma once

#include "clique_merger.hpp"
#include "filter.hpp"
#include "relation_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gnfs::relation {

enum class ReductionStrategy {
    NoLargePrimes,
    FilterOnly,
    StandardV0,
    StandardV0WithV3,
    CliqueV0,
};

/// Generation identity plus the raw corpus owned by one reduction.
///
/// The snapshot is intentionally move-only so the corpus is consumed by move,
/// and the generation is carried into the distinct result type.
struct RawRelationSnapshot final {
    uint64_t generation;
    std::vector<core::Relation> relations;

    explicit RawRelationSnapshot(uint64_t snapshot_generation,
                                 std::vector<core::Relation> snapshot_relations)
        : generation(snapshot_generation), relations(std::move(snapshot_relations)) {
        if (generation == 0) {
            throw std::invalid_argument("relation snapshot generation must be nonzero");
        }
    }

    RawRelationSnapshot(const RawRelationSnapshot&) = delete;
    RawRelationSnapshot& operator=(const RawRelationSnapshot&) = delete;
    RawRelationSnapshot(RawRelationSnapshot&&) noexcept = default;
    RawRelationSnapshot& operator=(RawRelationSnapshot&&) noexcept = default;
};

struct RelationReductionConfig {
    FilterConfig filter{};
    bool large_primes_enabled = false;
    size_t merge_rounds = 10;
    ReductionStrategy strategy = ReductionStrategy::NoLargePrimes;
};

/// Cross-platform, order-sensitive digest of a relation corpus.
///
/// This is a replay/equivalence digest, not a corpus identity, checkpoint format,
/// or cryptographic authentication tag. Its byte contract is versioned below and
/// never hashes native object layouts or Relation::serialize() output.
struct CorpusDigest final {
    uint64_t low = 0;
    uint64_t high = 0;

    [[nodiscard]] constexpr bool operator==(const CorpusDigest&) const noexcept = default;
};

namespace detail {

enum class CorpusDigestTag : uint8_t {
    CorpusBegin = 0x01,
    RelationBegin = 0x02,
    RelationA = 0x10,
    RelationB = 0x11,
    RationalFactors = 0x20,
    AlgebraicFactors = 0x21,
    RationalLargePrimes = 0x30,
    AlgebraicLargePrimes = 0x31,
    PrimePower = 0x32,
    ExtraAbPairs = 0x40,
    ExtraAbPair = 0x41,
    RelationEnd = 0x7e,
    CorpusEnd = 0x7f,
};

class CorpusDigestBuilder final {
public:
    void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);

        high_ ^= static_cast<uint64_t>(value) + UINT64_C(0x9e3779b97f4a7c15);
        high_ *= UINT64_C(0xc2b2ae3d27d4eb4f);
        high_ ^= high_ >> 29U;
    }

    void append_tag(CorpusDigestTag tag) noexcept {
        append_byte(static_cast<uint8_t>(tag));
    }

    void append_u32_le(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    void append_u64_le(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    [[nodiscard]] CorpusDigest finish() const noexcept {
        return {low_, high_};
    }

private:
    uint64_t low_ = UINT64_C(14695981039346656037);
    uint64_t high_ = UINT64_C(0x84222325cbf29ce4);
};

} // namespace detail

/// Compute the fixed V1 corpus digest.
///
/// Encoding contract:
///   - fixed domain bytes `GNFS-RDG` followed by version byte 1;
///   - one-byte field tags from CorpusDigestTag;
///   - every corpus/vector length and element index as little-endian uint64_t;
///   - signed a values converted modulo 2^64, then encoded little-endian;
///   - uint32_t factors as little-endian uint32_t;
///   - PrimePower fields as little-endian p/r uint64_t followed by e uint8_t.
[[nodiscard]] inline CorpusDigest
corpus_digest(const std::vector<core::Relation>& relations) noexcept {
    detail::CorpusDigestBuilder builder;
    constexpr uint8_t domain[] = {'G', 'N', 'F', 'S', '-', 'R', 'D', 'G', 1};
    for (uint8_t byte : domain) {
        builder.append_byte(byte);
    }

    builder.append_tag(detail::CorpusDigestTag::CorpusBegin);
    builder.append_u64_le(static_cast<uint64_t>(relations.size()));

    for (size_t relation_index = 0; relation_index < relations.size(); ++relation_index) {
        const auto& relation = relations[relation_index];
        builder.append_tag(detail::CorpusDigestTag::RelationBegin);
        builder.append_u64_le(static_cast<uint64_t>(relation_index));

        builder.append_tag(detail::CorpusDigestTag::RelationA);
        builder.append_u64_le(static_cast<uint64_t>(relation.a));
        builder.append_tag(detail::CorpusDigestTag::RelationB);
        builder.append_u64_le(relation.b);

        auto append_factors = [&](detail::CorpusDigestTag tag,
                                  const std::vector<uint32_t>& factors) {
            builder.append_tag(tag);
            builder.append_u64_le(static_cast<uint64_t>(factors.size()));
            for (size_t index = 0; index < factors.size(); ++index) {
                builder.append_u64_le(static_cast<uint64_t>(index));
                builder.append_u32_le(factors[index]);
            }
        };
        append_factors(detail::CorpusDigestTag::RationalFactors, relation.rational_factors);
        append_factors(detail::CorpusDigestTag::AlgebraicFactors, relation.algebraic_factors);

        auto append_prime_powers = [&](detail::CorpusDigestTag tag,
                                       const core::Relation::LargePrimeList& prime_powers) {
            builder.append_tag(tag);
            builder.append_u64_le(static_cast<uint64_t>(prime_powers.size()));
            for (size_t index = 0; index < prime_powers.size(); ++index) {
                const auto& prime_power = prime_powers[index];
                builder.append_tag(detail::CorpusDigestTag::PrimePower);
                builder.append_u64_le(static_cast<uint64_t>(index));
                builder.append_u64_le(prime_power.p);
                builder.append_u64_le(prime_power.r);
                builder.append_byte(prime_power.e);
            }
        };
        append_prime_powers(detail::CorpusDigestTag::RationalLargePrimes,
                            relation.rational_large_prime);
        append_prime_powers(detail::CorpusDigestTag::AlgebraicLargePrimes,
                            relation.algebraic_large_prime);

        builder.append_tag(detail::CorpusDigestTag::ExtraAbPairs);
        builder.append_u64_le(static_cast<uint64_t>(relation.extra_ab_pairs.size()));
        for (size_t index = 0; index < relation.extra_ab_pairs.size(); ++index) {
            const auto& [a, b] = relation.extra_ab_pairs[index];
            builder.append_tag(detail::CorpusDigestTag::ExtraAbPair);
            builder.append_u64_le(static_cast<uint64_t>(index));
            builder.append_u64_le(static_cast<uint64_t>(a));
            builder.append_u64_le(b);
        }
        builder.append_tag(detail::CorpusDigestTag::RelationEnd);
    }

    builder.append_tag(detail::CorpusDigestTag::CorpusEnd);
    return builder.finish();
}

/// Complete reduction statistics excluding wall-clock timing.
struct RelationReductionStats {
    ReductionStrategy strategy = ReductionStrategy::NoLargePrimes;
    size_t input_relations = 0;
    size_t raw_duplicates_removed = 0;
    CorpusDigest raw_input_digest{};
    CorpusDigest output_digest{};
    FilterStats filter{};
    LpKeyWeightHistogram pre_merge_lp_histogram{};
    size_t separated_full_relations = 0;
    size_t separated_partial_relations = 0;
    PartialRelationMerger::MergeStats standard_v0{};
    CliqueStats clique_v0{};
    CliqueStats v3{};
    size_t v3_relations_added = 0;
    size_t v3_duplicates_skipped = 0;
    size_t merged_relations = 0;
    size_t output_relations = 0;
    size_t output_lp_columns = 0;
};

/// A reduced corpus tied to the generation of its consumed raw snapshot.
struct RelationReductionResult final {
    uint64_t generation;
    std::vector<core::Relation> relations;
    RelationReductionStats stats;

    RelationReductionResult(uint64_t result_generation,
                            std::vector<core::Relation> result_relations,
                            RelationReductionStats result_stats)
        : generation(result_generation), relations(std::move(result_relations)),
          stats(std::move(result_stats)) {
        if (generation == 0) {
            throw std::invalid_argument("relation reduction generation must be nonzero");
        }
    }

    RelationReductionResult(const RelationReductionResult&) = delete;
    RelationReductionResult& operator=(const RelationReductionResult&) = delete;
    RelationReductionResult(RelationReductionResult&&) noexcept = default;
    RelationReductionResult& operator=(RelationReductionResult&&) noexcept = default;

    [[nodiscard]] size_t size() const noexcept {
        return relations.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return relations.empty();
    }
};

/// Pure in-memory implementation of the legacy relation reduction paths.
class RelationReductionEngine final {
public:
    [[nodiscard]] static RelationReductionResult reduce(RawRelationSnapshot&& snapshot,
                                                        const RelationReductionConfig& config) {
        validate_config(config);
        if (snapshot.generation == 0) {
            throw std::invalid_argument("relation snapshot generation must be nonzero");
        }

        RelationReductionStats stats;
        stats.strategy = config.strategy;
        stats.input_relations = snapshot.relations.size();
        validate_raw_relations(snapshot.relations);
        stats.raw_input_digest = corpus_digest(snapshot.relations);

        auto raw_relations =
            deduplicate_raw_relations(std::move(snapshot.relations), stats.raw_duplicates_removed);

        RelationFilter filter(config.filter);
        auto filtered = filter.filter(std::move(raw_relations));
        stats.filter = filter.stats();

        if (!config.large_primes_enabled) {
            stats.output_relations = filtered.size();
            stats.output_lp_columns = count_unique_lp_keys(filtered);
            stats.output_digest = corpus_digest(filtered);
            return RelationReductionResult(snapshot.generation, std::move(filtered),
                                           std::move(stats));
        }

        stats.pre_merge_lp_histogram = count_lp_key_weights(filtered);
        if (config.strategy == ReductionStrategy::FilterOnly) {
            stats.output_relations = filtered.size();
            stats.output_lp_columns = count_unique_lp_keys(filtered);
            stats.output_digest = corpus_digest(filtered);
            return RelationReductionResult(snapshot.generation, std::move(filtered),
                                           std::move(stats));
        }

        auto separated = separate_relations(std::move(filtered));
        stats.separated_full_relations = separated.full.size();
        stats.separated_partial_relations = separated.partial.size();

        std::vector<core::Relation> partial_copy_for_v3;
        if (config.strategy == ReductionStrategy::StandardV0WithV3) {
            partial_copy_for_v3 = separated.partial;
        }

        std::vector<core::Relation> merged;
        if (config.strategy == ReductionStrategy::CliqueV0) {
            merged =
                CliqueRelationMerger::merge_cliques(std::move(separated.partial), &stats.clique_v0);
        } else {
            merged = PartialRelationMerger::merge_all(std::move(separated.partial),
                                                      config.merge_rounds, &stats.standard_v0);
        }

        stats.merged_relations = merged.size();
        auto relations = std::move(separated.full);
        relations.reserve(relations.size() + merged.size());
        relations.insert(relations.end(), std::make_move_iterator(merged.begin()),
                         std::make_move_iterator(merged.end()));

        if (config.strategy == ReductionStrategy::StandardV0WithV3 &&
            !partial_copy_for_v3.empty()) {
            auto v3_merged =
                CliqueRelationMerger::merge_cliques(std::move(partial_copy_for_v3), &stats.v3);

            std::unordered_set<RelationSourceCombination, RelationSourceCombinationHash>
                existing_combinations;
            existing_combinations.reserve(relations.size());
            for (const auto& relation : relations) {
                if (relation.is_merged()) {
                    existing_combinations.insert(relation_source_combination(relation));
                }
            }

            relations.reserve(relations.size() + v3_merged.size());
            for (auto& relation : v3_merged) {
                if (!relation.is_merged() ||
                    existing_combinations.insert(relation_source_combination(relation)).second) {
                    relations.push_back(std::move(relation));
                    ++stats.v3_relations_added;
                } else {
                    ++stats.v3_duplicates_skipped;
                }
            }
            stats.merged_relations += stats.v3_relations_added;
        }

        stats.output_relations = relations.size();
        stats.output_lp_columns = count_unique_lp_keys(relations);
        stats.output_digest = corpus_digest(relations);
        return RelationReductionResult(snapshot.generation, std::move(relations), std::move(stats));
    }

private:
    static void validate_raw_relations(const std::vector<core::Relation>& relations) {
        for (const auto& relation : relations) {
            if (relation.is_merged()) {
                throw std::invalid_argument(
                    "relation reduction snapshot contains a merged relation");
            }
        }
    }

    [[nodiscard]] static std::vector<core::Relation>
    deduplicate_raw_relations(std::vector<core::Relation>&& relations, size_t& duplicates_removed) {
        std::unordered_set<core::ABPair, core::ABPairHash> seen;
        seen.reserve(relations.size());

        std::vector<core::Relation> deduplicated;
        deduplicated.reserve(relations.size());
        for (auto& relation : relations) {
            if (seen.insert(relation.ab()).second) {
                deduplicated.push_back(std::move(relation));
            } else {
                ++duplicates_removed;
            }
        }
        return deduplicated;
    }

    static void validate_config(const RelationReductionConfig& config) {
        switch (config.strategy) {
        case ReductionStrategy::NoLargePrimes:
        case ReductionStrategy::FilterOnly:
        case ReductionStrategy::StandardV0:
        case ReductionStrategy::StandardV0WithV3:
        case ReductionStrategy::CliqueV0:
            break;
        default:
            throw std::invalid_argument("unknown relation reduction strategy");
        }

        const bool no_lp_strategy = config.strategy == ReductionStrategy::NoLargePrimes;
        if (config.large_primes_enabled == no_lp_strategy) {
            throw std::invalid_argument(
                "large-prime mode and relation reduction strategy are inconsistent");
        }
        const bool merges_large_primes = config.strategy != ReductionStrategy::NoLargePrimes &&
                                         config.strategy != ReductionStrategy::FilterOnly;
        if (merges_large_primes && config.merge_rounds == 0) {
            throw std::invalid_argument("large-prime merge rounds must be nonzero");
        }
    }
};

} // namespace gnfs::relation
