#pragma once

#include "clique_merger.hpp"
#include "filter.hpp"
#include "relation_corpus.hpp"
#include "relation_identity.hpp"
#include "relation_sequence_receipt.hpp"
#include "relation_sink.hpp"
#include "relation_source.hpp"
#include "structured_filter_policy.hpp"
#include "structured_incidence_builder.hpp"
#include "structured_reduction.hpp"
#include "structured_reduction_telemetry.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gnfs::relation {

class CollectorUniqueOOCPrefixSource;

enum class ReductionStrategy {
    NoLargePrimes,
    FilterOnly,
    StandardV0,
    StandardV0WithV3,
    CliqueV0,
    Structured,
};

/// Overlay a pre-snapshot structured-filter decision on the caller's named
/// legacy strategy. Keeping this mapping pure lets every production and test
/// route preserve its existing OFF behavior without copying switch logic.
[[nodiscard]] inline ReductionStrategy
select_reduction_strategy(const StructuredFilterPolicyDecision& decision,
                          ReductionStrategy legacy_strategy) {
    if (legacy_strategy == ReductionStrategy::Structured) {
        throw std::invalid_argument("structured strategy cannot be its own legacy fallback");
    }
    switch (decision.selection) {
    case StructuredFilterSelection::Legacy:
        return legacy_strategy;
    case StructuredFilterSelection::Structured:
        return ReductionStrategy::Structured;
    }
    throw std::invalid_argument("unknown structured-filter strategy selection");
}

/// Generation identity plus the raw corpus owned by one reduction.
///
/// The snapshot is intentionally move-only so the corpus is consumed by move,
/// and the generation is carried into the distinct result type.
struct RawRelationSnapshot final {
    uint64_t generation;
    RelationCorpus corpus;

    explicit RawRelationSnapshot(uint64_t snapshot_generation,
                                 std::vector<core::Relation> snapshot_relations)
        : generation(snapshot_generation),
          corpus(
              RelationCorpus::from_in_memory(snapshot_generation, std::move(snapshot_relations))) {
        if (generation == 0) {
            throw std::invalid_argument("relation snapshot generation must be nonzero");
        }
    }

    explicit RawRelationSnapshot(RelationCorpus snapshot_corpus)
        : generation(snapshot_corpus.logical_generation()), corpus(std::move(snapshot_corpus)) {}

    RawRelationSnapshot(const RawRelationSnapshot&) = delete;
    RawRelationSnapshot& operator=(const RawRelationSnapshot&) = delete;
    RawRelationSnapshot(RawRelationSnapshot&&) noexcept = default;
    RawRelationSnapshot& operator=(RawRelationSnapshot&&) noexcept = default;

    [[nodiscard]] size_t size() const {
        return corpus.count();
    }

    [[nodiscard]] core::Relation read(size_t ordinal) const {
        return corpus.read(ordinal);
    }

    [[nodiscard]] std::vector<core::Relation> take_relations() && {
        if (corpus.storage_kind() == RelationStorageKind::InMemory) {
            return std::move(corpus).take_in_memory();
        }
        auto relations = corpus.materialize_all();
        RelationCorpus consumed = std::move(corpus);
        (void)consumed;
        return relations;
    }

    [[nodiscard]] RelationCorpus take_corpus() && {
        return std::move(corpus);
    }
};

struct RelationReductionConfig {
    struct StructuredExecutionConfig final {
        StructuredReductionBudget budget;
        StructuredParallelReductionOptions parallel{};
        StructuredIncidenceBuildOptions incidence{};
        TreeBasisPlanner planner = TreeBasisPlanner::DeterministicMst;
        /// Final structured payload backend. In-memory is the compatibility
        /// default; OOC requires an explicit exclusive staging base path.
        std::string output_ooc_base_path;
        OOCCleanupPolicy output_ooc_cleanup = OOCCleanupPolicy::RemoveArtifacts;
        /// Required working store for a finalized-OOC raw snapshot. Stable
        /// ABPair de-duplication streams accepted rows here without retaining a
        /// second relation vector. The engine always removes this private store.
        std::string deduplicated_ooc_base_path;
    };

    FilterConfig filter{};
    bool large_primes_enabled = false;
    size_t merge_rounds = 10;
    RelationMergePolicy merge_policy = relation_merge_policy_from_environment();
    ReductionStrategy strategy = ReductionStrategy::NoLargePrimes;
    /// Required only for Structured. Legacy strategies reject a populated
    /// value so research limits can never be accepted and then ignored.
    std::optional<StructuredExecutionConfig> structured;
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

/// Incremental fixed-V1 corpus digest.
///
/// The total row count is encoded before row payloads, so callers freeze it at
/// construction and append exactly that many relations in ordinal order. This
/// permits OOC and sink-backed paths to prove byte-identical output without a
/// full vector materialization.
class CorpusDigestAccumulator final {
public:
    explicit CorpusDigestAccumulator(size_t relation_count) : expected_relations_(relation_count) {
        constexpr uint8_t domain[] = {'G', 'N', 'F', 'S', '-', 'R', 'D', 'G', 1};
        for (uint8_t byte : domain) {
            builder_.append_byte(byte);
        }
        builder_.append_tag(detail::CorpusDigestTag::CorpusBegin);
        builder_.append_u64_le(static_cast<uint64_t>(relation_count));
    }

    void append(const core::Relation& relation) {
        if (appended_relations_ >= expected_relations_) {
            throw std::logic_error("corpus digest received more relations than declared");
        }
        const size_t relation_index = appended_relations_;
        auto& builder = builder_;
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
        ++appended_relations_;
    }

    [[nodiscard]] CorpusDigest finish() {
        if (appended_relations_ != expected_relations_) {
            throw std::logic_error("corpus digest received fewer relations than declared");
        }
        if (!finished_) {
            builder_.append_tag(detail::CorpusDigestTag::CorpusEnd);
            finished_ = true;
        }
        return builder_.finish();
    }

private:
    detail::CorpusDigestBuilder builder_;
    size_t expected_relations_ = 0;
    size_t appended_relations_ = 0;
    bool finished_ = false;
};

/// Compute the fixed V1 corpus digest.
///
/// Encoding contract:
///   - fixed domain bytes `GNFS-RDG` followed by version byte 1;
///   - one-byte field tags from CorpusDigestTag;
///   - every corpus/vector length and element index as little-endian uint64_t;
///   - signed a values converted modulo 2^64, then encoded little-endian;
///   - uint32_t factors as little-endian uint32_t;
///   - PrimePower fields as little-endian p/r uint64_t followed by e uint8_t.
[[nodiscard]] inline CorpusDigest corpus_digest(const std::vector<core::Relation>& relations) {
    CorpusDigestAccumulator accumulator(relations.size());
    for (const auto& relation : relations) {
        accumulator.append(relation);
    }
    return accumulator.finish();
}

[[nodiscard]] inline CorpusDigest corpus_digest(const RelationCorpus& corpus) {
    CorpusDigestAccumulator accumulator(corpus.count());
    corpus.for_each([&](const core::Relation& relation, size_t) { accumulator.append(relation); });
    return accumulator.finish();
}

/// Complete reduction statistics excluding wall-clock timing.
struct RelationReductionStats {
    ReductionStrategy strategy = ReductionStrategy::NoLargePrimes;
    size_t input_relations = 0;
    size_t raw_duplicates_removed = 0;
    CorpusDigest raw_input_digest{};
    CorpusDigest output_digest{};
    FilterStats filter{};
    /// Strategy-neutral singleton count. Legacy paths mirror
    /// `filter.singletons_removed`; structured paths report reducer peeling.
    size_t singleton_rows_removed = 0;
    /// LP weights after exact raw ABPair de-duplication and before any
    /// strategy-specific singleton policy.
    LpKeyWeightHistogram deduplicated_input_lp_histogram{};
    /// Legacy-only LP weights after RelationFilter and before V0/V3 merging.
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
    StructuredReductionRunResult structured_run{};
    StructuredReductionStats structured{};
    StructuredIncidenceBuildStats structured_incidence{};

    [[nodiscard]] bool operator==(const RelationReductionStats&) const noexcept = default;
};

/// A reduced corpus tied to the generation of its consumed raw snapshot.
struct RelationReductionResult final {
    uint64_t generation;
    RelationCorpus corpus;
    RelationReductionStats stats;

    RelationReductionResult(uint64_t result_generation,
                            std::vector<core::Relation> result_relations,
                            RelationReductionStats result_stats)
        : generation(result_generation),
          corpus(RelationCorpus::from_in_memory(result_generation, std::move(result_relations))),
          stats(std::move(result_stats)) {
        if (generation == 0) {
            throw std::invalid_argument("relation reduction generation must be nonzero");
        }
    }

    RelationReductionResult(RelationCorpus result_corpus, RelationReductionStats result_stats)
        : generation(result_corpus.logical_generation()), corpus(std::move(result_corpus)),
          stats(std::move(result_stats)) {}

    RelationReductionResult(const RelationReductionResult&) = delete;
    RelationReductionResult& operator=(const RelationReductionResult&) = delete;
    RelationReductionResult(RelationReductionResult&&) noexcept = default;
    RelationReductionResult& operator=(RelationReductionResult&&) noexcept = default;

    [[nodiscard]] size_t size() const {
        return corpus.count();
    }
    [[nodiscard]] bool empty() const {
        return corpus.empty();
    }
    [[nodiscard]] core::Relation read(size_t ordinal) const {
        return corpus.read(ordinal);
    }
    [[nodiscard]] RelationStorageKind storage_kind() const {
        return corpus.storage_kind();
    }
    [[nodiscard]] const RelationCorpus& relation_corpus() const noexcept {
        return corpus;
    }
    [[nodiscard]] std::vector<core::Relation> materialize_relations() const {
        return corpus.materialize_all();
    }
    [[nodiscard]] std::vector<core::Relation> take_relations() && {
        if (corpus.storage_kind() == RelationStorageKind::InMemory) {
            return std::move(corpus).take_in_memory();
        }
        auto relations = corpus.materialize_all();
        RelationCorpus consumed = std::move(corpus);
        (void)consumed;
        return relations;
    }
    [[nodiscard]] RelationCorpus take_corpus() && {
        return std::move(corpus);
    }
};

/// Shared reduction engine. Legacy strategies remain vector-backed; structured
/// execution accepts in-memory or finalized-OOC corpora through explicit
/// source, working-corpus, and transactional output boundaries.
class RelationReductionEngine final {
    struct CorpusMetrics final {
        size_t merged_relations = 0;
        size_t unique_lp_columns = 0;
        CorpusDigest digest{};
    };

    struct PreparedStructuredOOCInput final {
        RelationCorpus corpus;
        CorpusDigest raw_digest{};
        size_t duplicates_removed = 0;
        LpKeyWeightHistogram lp_histogram{};
    };

public:
    /// Owning boundary between a synchronously drained borrowed source and the
    /// later parallel structured reduction.
    ///
    /// Construction is engine-only. Dropping an unconsumed token removes its
    /// private working corpus, while moving or successfully reducing it leaves
    /// the source token invalid and fail-closed against reuse.
    class PreparedBorrowedStructuredInput final {
    public:
        PreparedBorrowedStructuredInput(const PreparedBorrowedStructuredInput&) = delete;
        PreparedBorrowedStructuredInput& operator=(const PreparedBorrowedStructuredInput&) = delete;

        PreparedBorrowedStructuredInput(PreparedBorrowedStructuredInput&& other) noexcept
            : generation_(std::exchange(other.generation_, uint64_t{0})),
              corpus_(std::move(other.corpus_)), stats_(std::move(other.stats_)),
              structured_(std::move(other.structured_)) {}

        PreparedBorrowedStructuredInput&
        operator=(PreparedBorrowedStructuredInput&& other) noexcept {
            if (this != &other) {
                generation_ = std::exchange(other.generation_, uint64_t{0});
                corpus_ = std::move(other.corpus_);
                stats_ = std::move(other.stats_);
                structured_ = std::move(other.structured_);
            }
            return *this;
        }

        ~PreparedBorrowedStructuredInput() = default;

        [[nodiscard]] bool valid() const noexcept {
            return generation_ != 0 && corpus_.valid();
        }

        [[nodiscard]] uint64_t generation() const noexcept {
            return generation_;
        }

        [[nodiscard]] size_t input_relations() const noexcept {
            return stats_.input_relations;
        }

    private:
        PreparedBorrowedStructuredInput(
            uint64_t generation, RelationCorpus corpus, RelationReductionStats stats,
            RelationReductionConfig::StructuredExecutionConfig structured)
            : generation_(generation), corpus_(std::move(corpus)), stats_(std::move(stats)),
              structured_(std::move(structured)) {}

        uint64_t generation_;
        RelationCorpus corpus_;
        RelationReductionStats stats_;
        RelationReductionConfig::StructuredExecutionConfig structured_;

        friend class RelationReductionEngine;
    };

    [[nodiscard]] static RelationReductionResult reduce(RawRelationSnapshot&& snapshot,
                                                        const RelationReductionConfig& config) {
        validate_config(config);
        if (snapshot.generation == 0) {
            throw std::invalid_argument("relation snapshot generation must be nonzero");
        }
        if (snapshot.corpus.logical_generation() != snapshot.generation) {
            throw std::invalid_argument(
                "relation snapshot generation does not match its owning corpus");
        }
        const uint64_t generation = snapshot.generation;
        const bool structured_ooc_input =
            config.strategy == ReductionStrategy::Structured &&
            snapshot.corpus.storage_kind() == RelationStorageKind::FinalizedOOC;
        validate_structured_input_storage(config, snapshot.corpus, structured_ooc_input);

        std::optional<RelationSink> structured_sink;
        if (config.strategy == ReductionStrategy::Structured &&
            !config.structured->output_ooc_base_path.empty()) {
            structured_sink.emplace(
                RelationSink::out_of_core(generation, config.structured->output_ooc_base_path,
                                          config.structured->output_ooc_cleanup));
        }

        RelationReductionStats stats;
        stats.strategy = config.strategy;
        stats.input_relations = snapshot.size();

        if (structured_ooc_input) {
            PreparedStructuredOOCInput prepared = prepare_structured_ooc_input(
                snapshot, config.structured->deduplicated_ooc_base_path);
            stats.raw_input_digest = prepared.raw_digest;
            stats.raw_duplicates_removed = prepared.duplicates_removed;
            stats.deduplicated_input_lp_histogram = prepared.lp_histogram;

            RelationReductionResult result =
                reduce_structured_corpus(generation, std::move(prepared.corpus), std::move(stats),
                                         *config.structured, std::move(structured_sink));

            // Only a fully published result consumes the authoritative raw
            // snapshot. Any earlier exception leaves the caller-owned OOC
            // corpus and its cleanup ownership intact for inspection or retry.
            RelationCorpus consumed_input = std::move(snapshot).take_corpus();
            (void)consumed_input;
            return result;
        }

        // Structured reduction owns a richer source-validation contract and
        // reports invalid rows as StructuredReductionError. Legacy routes
        // must fail closed before digesting/filtering rows, while preserving
        // that structured error boundary.
        if (config.strategy == ReductionStrategy::Structured) {
            validate_raw_relations(snapshot.corpus);
        } else {
            validate_legacy_raw_relations(snapshot.corpus);
        }
        stats.raw_input_digest = corpus_digest(snapshot.corpus);

        auto raw_relations = deduplicate_raw_relations(std::move(snapshot).take_relations(),
                                                       stats.raw_duplicates_removed);
        stats.deduplicated_input_lp_histogram = count_lp_key_weights(raw_relations);

        if (config.strategy == ReductionStrategy::Structured) {
            RelationCorpus deduplicated =
                RelationCorpus::from_in_memory(generation, std::move(raw_relations));
            return reduce_structured_corpus(generation, std::move(deduplicated), std::move(stats),
                                            *config.structured, std::move(structured_sink));
        }

        RelationFilter filter(config.filter);
        auto filtered = filter.filter(std::move(raw_relations));
        stats.filter = filter.stats();
        stats.singleton_rows_removed = stats.filter.singletons_removed;

        if (!config.large_primes_enabled) {
            stats.output_relations = filtered.size();
            stats.output_lp_columns = count_unique_lp_keys(filtered);
            stats.output_digest = corpus_digest(filtered);
            return RelationReductionResult(generation, std::move(filtered), std::move(stats));
        }

        stats.pre_merge_lp_histogram = count_lp_key_weights(filtered);
        if (config.strategy == ReductionStrategy::FilterOnly) {
            stats.output_relations = filtered.size();
            stats.output_lp_columns = count_unique_lp_keys(filtered);
            stats.output_digest = corpus_digest(filtered);
            return RelationReductionResult(generation, std::move(filtered), std::move(stats));
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
            merged = CliqueRelationMerger::merge_cliques(std::move(separated.partial),
                                                         &stats.clique_v0, config.merge_policy);
        } else {
            merged =
                PartialRelationMerger::merge_all(std::move(separated.partial), config.merge_rounds,
                                                 &stats.standard_v0, config.merge_policy);
        }

        stats.merged_relations = merged.size();
        auto relations = std::move(separated.full);
        relations.reserve(relations.size() + merged.size());
        relations.insert(relations.end(), std::make_move_iterator(merged.begin()),
                         std::make_move_iterator(merged.end()));

        if (config.strategy == ReductionStrategy::StandardV0WithV3 &&
            !partial_copy_for_v3.empty()) {
            auto v3_merged = CliqueRelationMerger::merge_cliques(std::move(partial_copy_for_v3),
                                                                 &stats.v3, config.merge_policy);

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
        return RelationReductionResult(generation, std::move(relations), std::move(stats));
    }

    /// Synchronously prepare a borrowed indexed source for structured OOC
    /// reduction.
    ///
    /// The source remains authoritative and is never retained, moved, or
    /// deleted. Raw validation, digesting, and stable ABPair de-duplication are
    /// streamed into the private working corpus before this function returns.
    /// This entry intentionally rejects legacy strategies and in-memory output
    /// so an ordinary OOC collector prefix cannot silently fall back to a
    /// vector-backed route.
    template <RelationSource Source>
    [[nodiscard]] static PreparedBorrowedStructuredInput
    prepare_borrowed_structured(uint64_t generation, const Source& source,
                                const RelationReductionConfig& config) {
        validate_config(config);
        if (generation == 0) {
            throw std::invalid_argument("borrowed relation source generation must be nonzero");
        }
        if (config.strategy != ReductionStrategy::Structured) {
            throw std::invalid_argument(
                "borrowed relation source requires the structured reduction strategy");
        }
        validate_structured_path_pair(*config.structured, true, true);
        auto structured = freeze_borrowed_structured_config(*config.structured);
        validate_structured_path_pair(structured, true, true);

        const size_t input_relations = static_cast<size_t>(source.count());
        PreparedStructuredOOCInput prepared = prepare_structured_indexed_input(
            generation, source, input_relations, structured.deduplicated_ooc_base_path);

        RelationReductionStats stats;
        stats.strategy = ReductionStrategy::Structured;
        stats.input_relations = input_relations;
        stats.raw_input_digest = prepared.raw_digest;
        stats.raw_duplicates_removed = prepared.duplicates_removed;
        stats.deduplicated_input_lp_histogram = prepared.lp_histogram;
        return PreparedBorrowedStructuredInput(generation, std::move(prepared.corpus),
                                               std::move(stats), std::move(structured));
    }

    /// Complete a prepared borrowed source after its reader/checkpoint lease has
    /// been released. No borrowed source is retained in or read by this phase.
    [[nodiscard]] static RelationReductionResult
    reduce_prepared_structured(PreparedBorrowedStructuredInput&& prepared) {
        if (!prepared.valid()) {
            throw std::logic_error("prepared borrowed structured input is not valid");
        }
        if (prepared.corpus_.logical_generation() != prepared.generation_) {
            throw std::logic_error(
                "prepared borrowed structured input generation does not match its corpus");
        }

        std::optional<RelationSink> structured_sink;
        structured_sink.emplace(RelationSink::out_of_core(prepared.generation_,
                                                          prepared.structured_.output_ooc_base_path,
                                                          prepared.structured_.output_ooc_cleanup));

        RelationReductionResult result = reduce_structured_corpus(
            prepared.generation_, std::move(prepared.corpus_), std::move(prepared.stats_),
            prepared.structured_, std::move(structured_sink));
        prepared.generation_ = 0;
        return result;
    }

    /// Convenience composition for callers that do not need to release a
    /// collector checkpoint between source preparation and parallel reduction.
    template <RelationSource Source>
    [[nodiscard]] static RelationReductionResult
    reduce_borrowed_structured(uint64_t generation, const Source& source,
                               const RelationReductionConfig& config) {
        auto prepared = prepare_borrowed_structured(generation, source, config);
        return reduce_prepared_structured(std::move(prepared));
    }

    /// Synchronously reduce one collector-proven unique OOC prefix without a
    /// de-duplicated working corpus.
    ///
    /// The concrete source type is a private-construction capability minted by
    /// RelationCollector. A static marker on an arbitrary RelationSource is
    /// deliberately insufficient to select this overload. Every source read,
    /// reducer worker, and output materialization completes before this method
    /// returns, so the result owns no reference to the callback-scoped source.
    /// The route is structured-only, requires transactional OOC output, and
    /// rejects a working path because no intermediate relation payload is
    /// persisted.
    template <typename Source>
        requires std::same_as<std::remove_cvref_t<Source>, CollectorUniqueOOCPrefixSource>
    [[nodiscard]] static RelationReductionResult
    reduce_direct_borrowed_structured(uint64_t generation, const Source& source,
                                      const RelationReductionConfig& config) {
        return reduce_direct_borrowed_structured_impl<false>(generation, source, config, nullptr);
    }

    /// The explicitly observed direct route. Telemetry provider failures are
    /// contained by the recorder, and reduction exceptions are rethrown with
    /// their original type and message.
    template <typename Source>
        requires std::same_as<std::remove_cvref_t<Source>, CollectorUniqueOOCPrefixSource>
    [[nodiscard]] static RelationReductionResult
    reduce_direct_borrowed_structured_observed(uint64_t generation, const Source& source,
                                               const RelationReductionConfig& config,
                                               StructuredReductionTelemetry& telemetry) {
        telemetry.begin(generation);
        try {
            RelationReductionResult result = reduce_direct_borrowed_structured_impl<true>(
                generation, source, config, &telemetry);
            telemetry.finish_success();
            return result;
        } catch (...) {
            telemetry.finish_failure();
            throw;
        }
    }

private:
    template <bool Observe, typename Source>
    [[nodiscard]] static RelationReductionResult
    reduce_direct_borrowed_structured_impl(uint64_t generation, const Source& source,
                                           const RelationReductionConfig& config,
                                           StructuredReductionTelemetry* telemetry) {
        if constexpr (Observe) {
            telemetry->enter_stage(StructuredTelemetryFailureStage::Preflight);
        }
        validate_config(config);
        if (generation == 0) {
            throw std::invalid_argument(
                "direct borrowed relation source generation must be nonzero");
        }
        if (config.strategy != ReductionStrategy::Structured) {
            throw std::invalid_argument(
                "direct borrowed relation source requires the structured reduction strategy");
        }
        if (!source.ab_pairs_unique()) {
            throw std::logic_error(
                "direct borrowed relation source lacks its runtime uniqueness capability");
        }

        auto structured = freeze_direct_borrowed_structured_config(*config.structured);
        if constexpr (Observe) {
            telemetry->enter_stage(StructuredTelemetryFailureStage::OutputReservation);
        }
        std::optional<RelationSink> structured_sink;
        structured_sink.emplace(RelationSink::out_of_core(
            generation, structured.output_ooc_base_path, structured.output_ooc_cleanup));

        const size_t input_relations = static_cast<size_t>(source.count());
        if constexpr (Observe) {
            telemetry->set_source_rows(input_relations);
            telemetry->enter_stage(StructuredTelemetryFailureStage::InitialScan);
            telemetry->checkpoint(StructuredTelemetryCheckpoint::ScanBegin);
        }
        struct DirectSourceScan final {
            CorpusDigest raw_digest;
            std::vector<std::vector<LargePrimeKey>> row_lp_keys;
            std::vector<CorpusDigest> fingerprints;
        };
        DirectSourceScan scan = [&] {
            CorpusDigestAccumulator raw_digest(input_relations);
            RelationSequenceReceiptAccumulator accepted_sequence;
            std::vector<std::vector<LargePrimeKey>> row_lp_keys;
            row_lp_keys.reserve(input_relations);
            std::vector<CorpusDigest> fingerprints;
            fingerprints.reserve(input_relations);

            // The capability is minted before the prefix reader opens. Prove
            // that the bytes visible to this engine still describe exactly the
            // collector's unique AB set, then release this temporary O(N) set
            // before incidence construction adds its O(N) bucket state. The
            // canonical row supports survive the scan so the authoritative
            // payload never needs a second incidence-only read.
            std::unordered_set<core::ABPair, core::ABPairHash> observed_ab_pairs;
            observed_ab_pairs.reserve(input_relations);
            for (size_t ordinal = 0; ordinal < input_relations; ++ordinal) {
                const core::Relation relation = read_direct_source<Observe>(
                    source, ordinal, telemetry, StructuredTelemetryReadPhase::InitialScan);
                validate_trusted_direct_source_relation(source, relation);
                if (!source.contains_proven_ab_pair(relation.ab())) {
                    fail_direct_source_untrusted(
                        source,
                        "direct borrowed source contains an AB pair outside the proven set");
                }
                if (!observed_ab_pairs.insert(relation.ab()).second) {
                    fail_direct_source_untrusted(
                        source, "direct borrowed source contains a duplicate AB pair");
                }
                raw_digest.append(relation);
                accepted_sequence.append(relation);
                row_lp_keys.push_back(odd_large_prime_keys(relation));
                fingerprints.push_back(direct_relation_fingerprint(relation));
            }
            if (accepted_sequence.finish() != source.accepted_sequence_receipt()) {
                fail_direct_source_untrusted(
                    source,
                    "direct borrowed source payload differs from the collector-accepted sequence");
            }
            if constexpr (Observe) {
                telemetry->checkpoint(StructuredTelemetryCheckpoint::ScanCompleteBeforeAbRelease);
            }
            return DirectSourceScan{raw_digest.finish(), std::move(row_lp_keys),
                                    std::move(fingerprints)};
        }();
        if constexpr (Observe) {
            telemetry->checkpoint(StructuredTelemetryCheckpoint::AfterAbRelease);
            telemetry->enter_stage(StructuredTelemetryFailureStage::IncidenceBuild);
        }

        StructuredIncidenceBuildResult incidence = build_structured_incidence_from_row_supports(
            generation, std::move(scan.row_lp_keys), structured.incidence);
        if constexpr (Observe) {
            telemetry->set_incidence(incidence.row_count(), incidence.buckets().size(),
                                     incidence.stats().total_incidence_entries);
            telemetry->checkpoint(StructuredTelemetryCheckpoint::IncidenceReceiptBuilt);
            telemetry->enter_stage(StructuredTelemetryFailureStage::ReducerConstruction);
        }

        RelationReductionStats stats;
        stats.strategy = ReductionStrategy::Structured;
        stats.input_relations = input_relations;
        stats.raw_input_digest = scan.raw_digest;
        stats.deduplicated_input_lp_histogram = lp_histogram_from_incidence(incidence);

        ValidatedDirectBorrowedSource<Source, Source, Observe> validated_source(
            source, source, scan.fingerprints, telemetry, StructuredTelemetryReadPhase::Reducer);
        SourceCorpus borrowed = SourceCorpus::from_validated_borrowed(generation, validated_source);
        if (borrowed.size() != input_relations) {
            throw std::logic_error(
                "direct borrowed relation source count changed during synchronous reduction");
        }
        RelationReductionResult result = reduce_structured_source_with_prebuilt_incidence<Observe>(
            generation, std::move(borrowed), std::move(incidence), std::move(stats), structured,
            std::move(structured_sink), telemetry);
        if constexpr (Observe) {
            telemetry->checkpoint(StructuredTelemetryCheckpoint::ReducerReleased);
            telemetry->enter_stage(StructuredTelemetryFailureStage::FreshValidation);
        }

        // Some successful reducer paths do not need to rematerialize every raw
        // atom. Reopen a fresh view and read the whole immutable prefix after
        // the output commits; an older MAP_PRIVATE view need not observe a
        // legal, same-size payload rewrite. Any failure destroys the local
        // RemoveArtifacts result before the collector observes the callback as
        // failed.
        source.with_fresh_prefix_view([&](const auto& fresh_source) {
            using FreshSource = std::remove_cvref_t<decltype(fresh_source)>;
            ValidatedDirectBorrowedSource<FreshSource, Source, Observe> fresh_validated_source(
                fresh_source, source, scan.fingerprints, telemetry,
                StructuredTelemetryReadPhase::FreshValidation);
            for (size_t ordinal = 0; ordinal < input_relations; ++ordinal) {
                (void)fresh_validated_source.read(ordinal);
            }
        });
        if constexpr (Observe) {
            telemetry->checkpoint(StructuredTelemetryCheckpoint::FreshValidationComplete);
        }
        return result;
    }

    template <bool Observe, typename Source>
    [[nodiscard]] static core::Relation read_direct_source(const Source& source, size_t ordinal,
                                                           StructuredReductionTelemetry* telemetry,
                                                           StructuredTelemetryReadPhase phase) {
        if constexpr (!Observe) {
            return source.read(ordinal);
        } else {
            telemetry->record_read_attempt(phase);
            try {
                core::Relation relation = source.read(ordinal);
                telemetry->record_read_success(phase);
                return relation;
            } catch (...) {
                telemetry->record_read_failure(phase);
                throw;
            }
        }
    }

    template <typename Source>
    [[noreturn]] static void fail_direct_source_untrusted(const Source& source,
                                                          const char* message) {
        try {
            throw std::runtime_error(message);
        } catch (...) {
            source.mark_untrusted(std::current_exception());
            throw;
        }
    }

    template <typename Source>
    static void validate_trusted_direct_source_relation(const Source& source,
                                                        const core::Relation& relation) {
        try {
            validate_direct_structured_source_relation(relation);
        } catch (...) {
            source.mark_untrusted(std::current_exception());
            throw;
        }
    }

    [[nodiscard]] static CorpusDigest direct_relation_fingerprint(const core::Relation& relation) {
        CorpusDigestAccumulator fingerprint(1);
        fingerprint.append(relation);
        return fingerprint.finish();
    }

    [[nodiscard]] static LpKeyWeightHistogram
    lp_histogram_from_incidence(const StructuredIncidenceBuildResult& incidence) {
        LpKeyWeightHistogram histogram;
        histogram.unique_keys = incidence.buckets().size();
        for (const auto& bucket : incidence.buckets()) {
            const size_t weight = bucket.adjacency.size();
            if (weight == 1) {
                ++histogram.weight_1;
            } else if (weight == 2) {
                ++histogram.weight_2;
            } else if (weight == 3) {
                ++histogram.weight_3;
            } else if (weight >= 4) {
                ++histogram.weight_4plus;
            } else {
                throw StructuredReductionError(
                    StructuredReductionErrorCode::InvariantViolation,
                    "structured incidence contains an empty large-prime bucket");
            }
        }
        return histogram;
    }

    struct DisabledDirectReadTelemetry final {};

    struct EnabledDirectReadTelemetry final {
        StructuredReductionTelemetry* telemetry = nullptr;
        StructuredTelemetryReadPhase phase = StructuredTelemetryReadPhase::InitialScan;
    };

    template <bool Observe>
    using DirectReadTelemetryState =
        std::conditional_t<Observe, EnabledDirectReadTelemetry, DisabledDirectReadTelemetry>;

    template <typename ReadSource, typename TrustSource, bool Observe>
    class ValidatedDirectBorrowedSource final {
    public:
        ValidatedDirectBorrowedSource(const ReadSource& read_source,
                                      const TrustSource& trust_source,
                                      const std::vector<CorpusDigest>& fingerprints,
                                      StructuredReductionTelemetry* telemetry,
                                      StructuredTelemetryReadPhase phase) noexcept
            : read_source_(&read_source), trust_source_(&trust_source),
              fingerprints_(&fingerprints) {
            if constexpr (Observe) {
                telemetry_state_.telemetry = telemetry;
                telemetry_state_.phase = phase;
            } else {
                (void)telemetry;
                (void)phase;
            }
        }

        [[nodiscard]] size_t count() const noexcept {
            return fingerprints_->size();
        }

        [[nodiscard]] core::Relation read(size_t ordinal) const {
            if (ordinal >= fingerprints_->size()) {
                throw std::out_of_range("validated direct borrowed source ordinal is out of range");
            }
            core::Relation relation = [&] {
                if constexpr (Observe) {
                    return read_direct_source<true>(
                        *read_source_, ordinal, telemetry_state_.telemetry, telemetry_state_.phase);
                } else {
                    return read_source_->read(ordinal);
                }
            }();
            validate_trusted_direct_source_relation(*trust_source_, relation);
            if (direct_relation_fingerprint(relation) != fingerprints_->at(ordinal)) {
                fail_direct_source_untrusted(
                    *trust_source_, "direct borrowed source payload changed after validation");
            }
            return relation;
        }

    private:
        const ReadSource* read_source_;
        const TrustSource* trust_source_;
        const std::vector<CorpusDigest>* fingerprints_;
        [[no_unique_address]] DirectReadTelemetryState<Observe> telemetry_state_;
    };

    [[nodiscard]] static RelationReductionResult
    reduce_structured_corpus(uint64_t generation, RelationCorpus deduplicated,
                             RelationReductionStats stats,
                             const RelationReductionConfig::StructuredExecutionConfig& structured,
                             std::optional<RelationSink> structured_sink) {
        return reduce_structured_source(generation, SourceCorpus(std::move(deduplicated)),
                                        std::move(stats), structured, std::move(structured_sink));
    }

    [[nodiscard]] static RelationReductionResult
    reduce_structured_source(uint64_t generation, SourceCorpus source, RelationReductionStats stats,
                             const RelationReductionConfig::StructuredExecutionConfig& structured,
                             std::optional<RelationSink> structured_sink) {
        if (source.generation() != generation) {
            throw std::logic_error(
                "structured source generation does not match the reduction generation");
        }
        SequentialStructuredReducer reducer(std::move(source), structured.incidence);
        return finish_structured_reduction<false>(generation, std::move(reducer), std::move(stats),
                                                  structured, std::move(structured_sink), nullptr);
    }

    template <bool Observe>
    [[nodiscard]] static RelationReductionResult reduce_structured_source_with_prebuilt_incidence(
        uint64_t generation, SourceCorpus source, StructuredIncidenceBuildResult&& incidence,
        RelationReductionStats stats,
        const RelationReductionConfig::StructuredExecutionConfig& structured,
        std::optional<RelationSink> structured_sink, StructuredReductionTelemetry* telemetry) {
        if (source.generation() != generation) {
            throw std::logic_error(
                "structured source generation does not match the reduction generation");
        }
        SequentialStructuredReducer reducer(std::move(source), std::move(incidence));
        if constexpr (Observe) {
            telemetry->checkpoint(StructuredTelemetryCheckpoint::ReducerConstructed);
        }
        return finish_structured_reduction<Observe>(generation, std::move(reducer),
                                                    std::move(stats), structured,
                                                    std::move(structured_sink), telemetry);
    }

    template <bool Observe>
    [[nodiscard]] static RelationReductionResult finish_structured_reduction(
        uint64_t generation, SequentialStructuredReducer reducer, RelationReductionStats stats,
        const RelationReductionConfig::StructuredExecutionConfig& structured,
        std::optional<RelationSink> structured_sink, StructuredReductionTelemetry* telemetry) {
        if constexpr (Observe) {
            telemetry->enter_stage(StructuredTelemetryFailureStage::BudgetedReduction);
        }
        stats.structured_run = reducer.reduce_budgeted_parallel(
            structured.budget, structured.parallel, structured.planner);
        if constexpr (Observe) {
            telemetry->checkpoint(StructuredTelemetryCheckpoint::ReductionComplete);
        }
        stats.structured = reducer.stats();
        stats.structured_incidence = reducer.incidence_build_stats();
        stats.singleton_rows_removed = stats.structured_run.singleton_rows_removed;

        if constexpr (Observe) {
            telemetry->enter_stage(StructuredTelemetryFailureStage::OutputMaterialization);
        }
        if (!structured_sink) {
            structured_sink.emplace(
                RelationSink::in_memory(generation, reducer.active_row_count()));
        }

        CorpusMetrics metrics;
        metrics.unique_lp_columns = reducer.active_lp_column_count();
        CorpusDigestAccumulator output_digest(reducer.active_row_count());
        const size_t materialized_rows =
            reducer.materialize_active_to(*structured_sink, [&](const core::Relation& relation) {
                output_digest.append(relation);
                if (relation.is_merged()) {
                    ++metrics.merged_relations;
                }
            });
        if (materialized_rows != reducer.active_row_count()) {
            throw StructuredReductionError(
                StructuredReductionErrorCode::InvariantViolation,
                "structured sink materialization did not cover every active row");
        }
        if (structured_sink->count() != materialized_rows) {
            throw StructuredReductionError(
                StructuredReductionErrorCode::InvariantViolation,
                "structured sink count differs from materialized active rows");
        }
        metrics.digest = output_digest.finish();
        if constexpr (Observe) {
            telemetry->checkpoint(StructuredTelemetryCheckpoint::OutputMaterialized);
            telemetry->enter_stage(StructuredTelemetryFailureStage::OutputFinalize);
        }
        RelationCorpus output = structured_sink->finalize();
        if constexpr (Observe) {
            telemetry->checkpoint(StructuredTelemetryCheckpoint::OutputFinalized);
        }
        stats.merged_relations = metrics.merged_relations;
        stats.output_relations = output.count();
        stats.output_lp_columns = metrics.unique_lp_columns;
        stats.output_digest = metrics.digest;
        return RelationReductionResult(std::move(output), std::move(stats));
    }

    [[nodiscard]] static PreparedStructuredOOCInput
    prepare_structured_ooc_input(const RawRelationSnapshot& snapshot,
                                 const std::string& deduplicated_base_path) {
        return prepare_structured_indexed_input(snapshot.generation, snapshot.corpus,
                                                snapshot.size(), deduplicated_base_path);
    }

    template <RelationSource Source>
    [[nodiscard]] static PreparedStructuredOOCInput
    prepare_structured_indexed_input(uint64_t generation, const Source& source,
                                     size_t input_relations,
                                     const std::string& deduplicated_base_path) {
        RelationSink sink = RelationSink::out_of_core(generation, deduplicated_base_path,
                                                      OOCCleanupPolicy::RemoveArtifacts);
        CorpusDigestAccumulator raw_digest(input_relations);
        LpKeyWeightAccumulator lp_histogram(input_relations);
        std::unordered_set<core::ABPair, core::ABPairHash> seen;
        seen.reserve(input_relations);

        size_t duplicates_removed = 0;
        for (size_t ordinal = 0; ordinal < input_relations; ++ordinal) {
            core::Relation relation = source.read(ordinal);
            validate_raw_relation(relation);
            raw_digest.append(relation);
            if (!seen.insert(relation.ab()).second) {
                ++duplicates_removed;
                continue;
            }
            lp_histogram.append(relation);
            (void)sink.append(std::move(relation));
        }

        RelationCorpus corpus = sink.finalize();
        return {std::move(corpus), raw_digest.finish(), duplicates_removed, lp_histogram.finish()};
    }

    [[nodiscard]] static std::filesystem::path
    structured_sink_lease_root(const std::string& requested_base) {
        return RelationSink::lease_root_for(requested_base);
    }

    [[nodiscard]] static RelationReductionConfig::StructuredExecutionConfig
    freeze_borrowed_structured_config(
        const RelationReductionConfig::StructuredExecutionConfig& requested) {
        auto frozen = requested;
        if (!requested.deduplicated_ooc_base_path.empty()) {
            frozen.deduplicated_ooc_base_path =
                relation_corpus_detail::freeze_ooc_path(requested.deduplicated_ooc_base_path);
        }
        if (!requested.output_ooc_base_path.empty()) {
            frozen.output_ooc_base_path =
                relation_corpus_detail::freeze_ooc_path(requested.output_ooc_base_path);
        }
        return frozen;
    }

    [[nodiscard]] static RelationReductionConfig::StructuredExecutionConfig
    freeze_direct_borrowed_structured_config(
        const RelationReductionConfig::StructuredExecutionConfig& requested) {
        if (!requested.deduplicated_ooc_base_path.empty()) {
            throw std::invalid_argument(
                "direct borrowed structured input rejects a working OOC base path");
        }
        if (requested.output_ooc_base_path.empty()) {
            throw std::invalid_argument(
                "direct borrowed structured input requires an OOC output base path");
        }
        if (requested.output_ooc_cleanup != OOCCleanupPolicy::RemoveArtifacts) {
            throw std::invalid_argument(
                "direct borrowed structured input requires removable OOC output artifacts");
        }
        auto frozen = freeze_borrowed_structured_config(requested);
        validate_structured_path_pair(frozen, false, true);
        return frozen;
    }

    [[nodiscard]] static bool path_contains(const std::filesystem::path& parent,
                                            const std::filesystem::path& child) {
        auto parent_it = parent.begin();
        auto child_it = child.begin();
        for (; parent_it != parent.end() && child_it != child.end(); ++parent_it, ++child_it) {
            if (*parent_it != *child_it) {
                return false;
            }
        }
        return parent_it == parent.end();
    }

    [[nodiscard]] static bool paths_overlap(const std::filesystem::path& lhs,
                                            const std::filesystem::path& rhs) {
        return path_contains(lhs, rhs) || path_contains(rhs, lhs);
    }

    static void validate_structured_input_storage(const RelationReductionConfig& config,
                                                  const RelationCorpus& input,
                                                  bool structured_ooc_input) {
        if (config.strategy != ReductionStrategy::Structured) {
            return;
        }
        const auto& structured = *config.structured;
        if (structured_ooc_input && structured.deduplicated_ooc_base_path.empty()) {
            throw std::invalid_argument(
                "structured OOC input requires a deduplicated working base path");
        }
        if (!structured_ooc_input && !structured.deduplicated_ooc_base_path.empty()) {
            throw std::invalid_argument(
                "structured deduplicated OOC base path requires finalized OOC input");
        }
        if (!structured_ooc_input) {
            return;
        }

        validate_structured_path_pair(structured, true, false);
        const auto working = structured_sink_lease_root(structured.deduplicated_ooc_base_path);
        const std::optional<std::filesystem::path> output =
            structured.output_ooc_base_path.empty()
                ? std::nullopt
                : std::optional<std::filesystem::path>(
                      structured_sink_lease_root(structured.output_ooc_base_path));

        const auto input_scope = input.ooc_artifact_scope();
        if (!input_scope || input_scope->cleanup_directory.empty()) {
            return;
        }
        const auto input_cleanup = std::filesystem::path(input_scope->cleanup_directory);
        if (paths_overlap(input_cleanup, working)) {
            throw std::invalid_argument(
                "structured OOC working lease must not overlap the input cleanup scope");
        }
        if (output && paths_overlap(input_cleanup, *output)) {
            throw std::invalid_argument(
                "structured OOC output lease must not overlap the input cleanup scope");
        }
    }

    static void validate_structured_path_pair(
        const RelationReductionConfig::StructuredExecutionConfig& structured, bool require_working,
        bool require_output) {
        if (require_working && structured.deduplicated_ooc_base_path.empty()) {
            throw std::invalid_argument(
                "structured OOC input requires a deduplicated working base path");
        }
        if (require_output && structured.output_ooc_base_path.empty()) {
            throw std::invalid_argument(
                "borrowed structured input requires an OOC output base path");
        }
        if (structured.deduplicated_ooc_base_path.empty()) {
            return;
        }

        const auto working = structured_sink_lease_root(structured.deduplicated_ooc_base_path);
        if (!structured.output_ooc_base_path.empty()) {
            const auto output = structured_sink_lease_root(structured.output_ooc_base_path);
            if (paths_overlap(working, output)) {
                throw std::invalid_argument(
                    "structured OOC working and output lease roots must not overlap");
            }
        }
    }

    static void validate_raw_relation(const core::Relation& relation) {
        if (relation.is_merged()) {
            throw std::invalid_argument("relation reduction snapshot contains a merged relation");
        }
    }

    static void validate_legacy_relation(const core::Relation& relation) {
        // Keep the Relation persistence contract at the raw-source boundary:
        // it rejects b == 0, malformed extra pairs, and oversized vectors
        // before legacy filtering can materialize or merge the row.
        relation.validate_persistence_limits();

        const auto validate_live_large_prime = [](const core::PrimePower& prime_power,
                                                  bool algebraic) {
            // Zero-exponent entries can remain in raw merged payloads for
            // lossless square-root data. They have no effective LP domain, so
            // only live entries require p/root validation here.
            if (prime_power.e == 0)
                return;
            if (prime_power.p < 2) {
                throw std::invalid_argument(
                    "relation reduction snapshot contains a large-prime p < 2");
            }
            constexpr uint64_t projective_root = std::numeric_limits<uint32_t>::max();
            if (algebraic && prime_power.r != projective_root && prime_power.r >= prime_power.p) {
                throw std::invalid_argument(
                    "relation reduction snapshot contains an algebraic root outside p");
            }
        };

        for (const auto& prime_power : relation.rational_large_prime) {
            validate_live_large_prime(prime_power, false);
        }
        for (const auto& prime_power : relation.algebraic_large_prime) {
            validate_live_large_prime(prime_power, true);
        }

        validate_raw_relation(relation);
    }

    static void validate_direct_structured_source_relation(const core::Relation& relation) {
        validate_raw_relation(relation);
        if (relation.b == 0) {
            throw StructuredReductionError(StructuredReductionErrorCode::InvalidInput,
                                           "source relation has b == 0");
        }
        for (const auto& [a, b] : relation.extra_ab_pairs) {
            (void)a;
            if (b == 0) {
                throw StructuredReductionError(StructuredReductionErrorCode::InvalidInput,
                                               "source relation has an extra pair with b == 0");
            }
        }
        const auto validate_large_prime = [](const core::PrimePower& prime_power, bool algebraic) {
            if (prime_power.p < 2 || prime_power.e == 0) {
                throw StructuredReductionError(
                    StructuredReductionErrorCode::InvalidInput,
                    "source relation contains an invalid large-prime power");
            }
            constexpr uint64_t projective_root = std::numeric_limits<uint32_t>::max();
            if (algebraic && prime_power.r != projective_root && prime_power.r >= prime_power.p) {
                throw StructuredReductionError(StructuredReductionErrorCode::InvalidInput,
                                               "algebraic large-prime power has an invalid root");
            }
        };
        for (const auto& prime_power : relation.rational_large_prime) {
            validate_large_prime(prime_power, false);
        }
        for (const auto& prime_power : relation.algebraic_large_prime) {
            validate_large_prime(prime_power, true);
        }
        try {
            relation.validate_persistence_limits();
        } catch (const std::length_error&) {
            throw StructuredReductionError(StructuredReductionErrorCode::PersistenceLimit,
                                           "source relation exceeds persistence limits");
        }
    }

    static void validate_raw_relations(const RelationCorpus& corpus) {
        corpus.for_each(
            [](const core::Relation& relation, size_t) { validate_raw_relation(relation); });
    }

    static void validate_legacy_raw_relations(const RelationCorpus& corpus) {
        corpus.for_each(
            [](const core::Relation& relation, size_t) { validate_legacy_relation(relation); });
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
        case ReductionStrategy::Structured:
            break;
        default:
            throw std::invalid_argument("unknown relation reduction strategy");
        }

        const bool no_lp_strategy = config.strategy == ReductionStrategy::NoLargePrimes;
        if (config.large_primes_enabled == no_lp_strategy) {
            throw std::invalid_argument(
                "large-prime mode and relation reduction strategy are inconsistent");
        }
        const bool legacy_merges_large_primes =
            config.strategy == ReductionStrategy::StandardV0 ||
            config.strategy == ReductionStrategy::StandardV0WithV3 ||
            config.strategy == ReductionStrategy::CliqueV0;
        if (legacy_merges_large_primes && config.merge_rounds == 0) {
            throw std::invalid_argument("large-prime merge rounds must be nonzero");
        }

        const bool structured_strategy = config.strategy == ReductionStrategy::Structured;
        if (structured_strategy != config.structured.has_value()) {
            throw std::invalid_argument(
                "structured relation strategy and execution config are inconsistent");
        }
        if (!structured_strategy)
            return;

        const auto& structured = *config.structured;
        validate_structured_reduction_budget(structured.budget);
        if (structured.parallel.max_batch_candidates == 0 ||
            structured.parallel.worker_count == 0) {
            throw std::invalid_argument(
                "structured parallel execution requires nonzero batch width and workers");
        }
        if (structured.incidence.max_rows_per_shard == 0 ||
            structured.incidence.worker_count == 0) {
            throw std::invalid_argument(
                "structured incidence execution requires nonzero shard rows and workers");
        }
        if (structured.planner != TreeBasisPlanner::ReferenceStar &&
            structured.planner != TreeBasisPlanner::DeterministicMst) {
            throw std::invalid_argument("unknown structured relation planner");
        }
        if (structured.output_ooc_base_path.find('\0') != std::string::npos) {
            throw std::invalid_argument("structured OOC output base path contains NUL");
        }
        if (structured.deduplicated_ooc_base_path.find('\0') != std::string::npos) {
            throw std::invalid_argument("structured OOC working base path contains NUL");
        }
        if (structured.output_ooc_cleanup != OOCCleanupPolicy::Preserve &&
            structured.output_ooc_cleanup != OOCCleanupPolicy::RemoveArtifacts) {
            throw std::invalid_argument("unknown structured OOC output cleanup policy");
        }
    }
};

} // namespace gnfs::relation
