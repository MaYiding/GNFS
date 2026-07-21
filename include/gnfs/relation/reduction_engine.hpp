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

/// Complete reduction statistics excluding wall-clock timing.
struct RelationReductionStats {
    ReductionStrategy strategy = ReductionStrategy::NoLargePrimes;
    size_t input_relations = 0;
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

        RelationFilter filter(config.filter);
        auto filtered = filter.filter(std::move(snapshot.relations));
        stats.filter = filter.stats();

        if (!config.large_primes_enabled) {
            stats.output_relations = filtered.size();
            stats.output_lp_columns = count_unique_lp_keys(filtered);
            return RelationReductionResult(snapshot.generation, std::move(filtered),
                                           std::move(stats));
        }

        stats.pre_merge_lp_histogram = count_lp_key_weights(filtered);
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
        return RelationReductionResult(snapshot.generation, std::move(relations), std::move(stats));
    }

private:
    static void validate_config(const RelationReductionConfig& config) {
        switch (config.strategy) {
        case ReductionStrategy::NoLargePrimes:
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
        if (config.large_primes_enabled && config.merge_rounds == 0) {
            throw std::invalid_argument("large-prime merge rounds must be nonzero");
        }
    }
};

} // namespace gnfs::relation
