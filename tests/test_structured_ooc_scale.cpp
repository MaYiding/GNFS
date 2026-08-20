#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"
#include "gnfs/relation/reduction_engine.hpp"
#include "gnfs/relation/structured_filter_profile.hpp"
#include "gnfs/relation/structured_reduction_telemetry.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/process_memory.hpp"
#include "gnfs/util/temp_path.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::CollectorConfig;
using gnfs::relation::CollectorUniqueOOCPrefixSource;
using gnfs::relation::CorpusDigest;
using gnfs::relation::CorpusDigestAccumulator;
using gnfs::relation::OOCCleanupPolicy;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::OOCSnapshotDescriptor;
using gnfs::relation::RawRelationSnapshot;
using gnfs::relation::ReductionStrategy;
using gnfs::relation::RelationCollector;
using gnfs::relation::RelationCorpus;
using gnfs::relation::RelationReductionConfig;
using gnfs::relation::RelationReductionEngine;
using gnfs::relation::RelationReductionResult;
using gnfs::relation::RelationReductionStats;
using gnfs::relation::RelationStorageKind;
using gnfs::relation::StructuredReductionStopReason;
using gnfs::relation::StructuredReductionTelemetry;
using gnfs::relation::StructuredReductionTelemetryRecord;
using gnfs::relation::StructuredTelemetryCheckpoint;
using gnfs::relation::StructuredTelemetryReadPhase;
using gnfs::util::process_memory_backend_name;
using gnfs::util::process_memory_snapshot;
using gnfs::util::ProcessMemoryBackend;
using gnfs::util::ProcessMemorySnapshot;

namespace {

#ifndef GNFS_STRUCTURED_OOC_SCALE_BUILD_TYPE
#define GNFS_STRUCTURED_OOC_SCALE_BUILD_TYPE "unknown"
#endif

constexpr std::string_view STRUCTURED_OOC_SCALE_BUILD_TYPE = GNFS_STRUCTURED_OOC_SCALE_BUILD_TYPE;

size_t checks = 0;
std::string_view current_case;

[[noreturn]] void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed in ") + std::string(current_case) +
                             " at line " + std::to_string(line) + ": " + expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition))                                                                          \
            check_failed(#condition, __LINE__);                                                    \
    } while (false)

class ArtifactCleanup final {
public:
    void add(std::string base_path) {
        base_paths_.push_back(std::move(base_path));
    }

    ~ArtifactCleanup() {
        for (const std::string& base_path : base_paths_) {
            std::error_code ignored;
            std::filesystem::remove(base_path + ".relidx", ignored);
            ignored.clear();
            std::filesystem::remove(base_path + ".reldata", ignored);
            ignored.clear();
            std::filesystem::remove(base_path + ".gnfs-ooc-cleanup-v1.lock", ignored);
            ignored.clear();
            std::filesystem::remove_all(base_path + ".gnfs-sink-lease", ignored);
            ignored.clear();
            std::filesystem::remove(base_path + ".gnfs-sink-lease.gnfs-ooc-cleanup-v1.lock",
                                    ignored);
        }
    }

private:
    std::vector<std::string> base_paths_;
};

[[nodiscard]] std::string unique_ooc_base(std::string_view label, size_t row_count,
                                          uint32_t workers = 0) {
    static uint64_t sequence = 0;
    return gnfs::util::temp_path("gnfs_structured_ooc_scale_" + std::string(label) + "_" +
                                 std::to_string(row_count) + "_w" + std::to_string(workers) + "_" +
                                 std::to_string(gnfs::util::process_id()) + "_" +
                                 std::to_string(sequence++));
}

[[nodiscard]] std::string private_sink_base(const std::string& requested_base) {
    return (std::filesystem::path(requested_base + ".gnfs-sink-lease") / "corpus").string();
}

[[nodiscard]] bool ordinary_artifacts_exist(const std::string& base_path) {
    return std::filesystem::exists(base_path + ".relidx") &&
           std::filesystem::exists(base_path + ".reldata");
}

[[nodiscard]] bool ordinary_artifacts_absent(const std::string& base_path) {
    return !std::filesystem::exists(base_path + ".relidx") &&
           !std::filesystem::exists(base_path + ".reldata");
}

[[nodiscard]] bool private_sink_exists(const std::string& requested_base) {
    return std::filesystem::is_directory(requested_base + ".gnfs-sink-lease") &&
           ordinary_artifacts_exist(private_sink_base(requested_base));
}

[[nodiscard]] bool private_sink_absent(const std::string& requested_base) {
    return !std::filesystem::exists(requested_base + ".gnfs-sink-lease");
}

[[nodiscard]] bool relation_equal(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

constexpr size_t DUPLICATE_PERIOD = 257;
constexpr size_t COMPONENT_STRIDE = 4'096;

[[nodiscard]] size_t duplicate_count(size_t raw_row_count) noexcept {
    return raw_row_count / DUPLICATE_PERIOD;
}

[[nodiscard]] size_t unique_row_count(size_t raw_row_count) noexcept {
    return raw_row_count - duplicate_count(raw_row_count);
}

[[nodiscard]] size_t complete_component_groups(size_t unique_rows) noexcept {
    return unique_rows < 5 ? 0 : 1 + (unique_rows - 5) / COMPONENT_STRIDE;
}

[[nodiscard]] Relation make_unique_relation(size_t unique_ordinal, size_t unique_rows) {
    const uint64_t magnitude = static_cast<uint64_t>(unique_ordinal) + 1'000'001;
    const int64_t a = (unique_ordinal & 1U) == 0 ? static_cast<int64_t>(magnitude)
                                                 : -static_cast<int64_t>(magnitude);
    // Consecutive magnitudes keep every raw fixture admissible through the
    // production collector's gcd(a, b) validation while retaining varied AB
    // identities and stable duplicate rows.
    Relation relation(a, magnitude + 1);
    relation.rational_factors.push_back(static_cast<uint32_t>(2 + (unique_ordinal * 17) % 65'521));
    relation.algebraic_factors.push_back(static_cast<uint32_t>(3 + (unique_ordinal * 29) % 65'519));
    if (unique_ordinal % 11 == 0) {
        relation.rational_factors.push_back(
            static_cast<uint32_t>(67'000 + unique_ordinal % 10'000));
    }

    // Every complete stride starts with two isolated components: a weight-2
    // rational key and a weight-3 algebraic key. The remaining rows are full.
    // At 200K this creates tens, not hundreds of thousands, of components:
    // enough to exercise repeated conflict-free batches and parallel
    // preparation while keeping the scale gate bounded.
    const size_t component_base = unique_ordinal - unique_ordinal % COMPONENT_STRIDE;
    if (component_base + 4 < unique_rows) {
        const size_t position = unique_ordinal - component_base;
        const uint64_t component = static_cast<uint64_t>(component_base / COMPONENT_STRIDE);
        if (position < 2) {
            relation.rational_large_prime.emplace_back(UINT64_C(1'000'000'007) + component * 32 + 2,
                                                       uint8_t{1});
        } else if (position < 5) {
            relation.algebraic_large_prime.emplace_back(
                UINT64_C(2'000'000'011) + component * 32 + 4,
                UINT64_C(700'000'003) + component * 16 + 1, uint8_t{1});
        }
    }
    return relation;
}

[[nodiscard]] Relation make_raw_relation(size_t raw_ordinal, size_t raw_row_count) {
    const bool duplicate = raw_ordinal % DUPLICATE_PERIOD == DUPLICATE_PERIOD - 1;
    const size_t unique_ordinal =
        raw_ordinal - raw_ordinal / DUPLICATE_PERIOD - (duplicate ? 1 : 0);
    Relation relation = make_unique_relation(unique_ordinal, unique_row_count(raw_row_count));
    if (duplicate) {
        // Stable ABPair de-duplication must preserve the first payload rather
        // than this deliberately different duplicate payload.
        relation.algebraic_factors.push_back(static_cast<uint32_t>(90'000 + raw_ordinal % 10'000));
    }
    return relation;
}

// This cardinality anchor matches the complete 50-digit first-round row, key,
// and incidence counts without claiming to reproduce that corpus's LP-weight
// distribution. Singleton spokes peel completely. The remaining 9-regular
// core is above the structured planner's weight-eight pivot cap, so the full
// observed direct route reaches NoCandidates without constructing hundreds of
// thousands of merge plans in repeated waves.
constexpr size_t DENSE_STAGE_ROWS = 618'449;
constexpr size_t DENSE_STAGE_KEYS = 576'189;
constexpr size_t DENSE_STAGE_INCIDENCES = 1'236'898;
constexpr size_t DENSE_STAGE_KEYS_PER_ROW = 2;
constexpr size_t DENSE_STAGE_SINGLETON_KEYS = 493'601;
constexpr size_t DENSE_STAGE_CONSUMED_DEGREE_9_KEYS = 54'839;
constexpr size_t DENSE_STAGE_CONSUMED_DEGREE_10_KEYS = 5;
constexpr size_t DENSE_STAGE_CONSUMED_HUB_KEYS =
    DENSE_STAGE_CONSUMED_DEGREE_9_KEYS + DENSE_STAGE_CONSUMED_DEGREE_10_KEYS;
constexpr size_t DENSE_STAGE_CORE_KEYS = 27'744;
constexpr size_t DENSE_STAGE_CORE_ROWS = DENSE_STAGE_CORE_KEYS * 9 / 2;
constexpr size_t DENSE_STAGE_WEIGHT_4PLUS_KEYS =
    DENSE_STAGE_CONSUMED_HUB_KEYS + DENSE_STAGE_CORE_KEYS;
constexpr size_t DENSE_STAGE_CORE_OFFSET_ROWS = DENSE_STAGE_CORE_KEYS * 4;
constexpr size_t DENSE_STAGE_CORE_MATCHING_ROWS = DENSE_STAGE_CORE_KEYS / 2;
constexpr CorpusDigest DENSE_STAGE_EXPECTED_RAW_DIGEST{
    UINT64_C(12026999194036640146),
    UINT64_C(18236242084170576750),
};
constexpr CorpusDigest DENSE_STAGE_EXPECTED_OUTPUT_DIGEST{
    UINT64_C(16663096013469244788),
    UINT64_C(7825550821674115266),
};

static_assert(DENSE_STAGE_SINGLETON_KEYS + DENSE_STAGE_WEIGHT_4PLUS_KEYS == DENSE_STAGE_KEYS);
static_assert(DENSE_STAGE_SINGLETON_KEYS + DENSE_STAGE_CORE_ROWS == DENSE_STAGE_ROWS);
static_assert(DENSE_STAGE_ROWS * DENSE_STAGE_KEYS_PER_ROW == DENSE_STAGE_INCIDENCES);
static_assert(DENSE_STAGE_KEYS_PER_ROW <= Relation::MAX_SERIALIZED_LARGE_PRIMES);
static_assert(DENSE_STAGE_CONSUMED_DEGREE_9_KEYS * 9 + DENSE_STAGE_CONSUMED_DEGREE_10_KEYS * 10 ==
              DENSE_STAGE_SINGLETON_KEYS);
static_assert(DENSE_STAGE_CORE_OFFSET_ROWS + DENSE_STAGE_CORE_MATCHING_ROWS ==
              DENSE_STAGE_CORE_ROWS);

[[nodiscard]] constexpr uint64_t dense_stage_key_value(size_t key_ordinal) noexcept {
    // These are deterministic structural tokens. The reduction fixture does
    // not make a primality claim about the synthetic LargePrimeKey values.
    return UINT64_C(4'000'000'007) + static_cast<uint64_t>(key_ordinal) * 2;
}

[[nodiscard]] std::pair<size_t, size_t> dense_stage_row_key_ordinals(size_t raw_ordinal) {
    if (raw_ordinal >= DENSE_STAGE_ROWS) {
        throw std::out_of_range("dense structured stage row ordinal is out of range");
    }

    if (raw_ordinal < DENSE_STAGE_SINGLETON_KEYS) {
        size_t hub_ordinal = 0;
        const size_t degree_9_rows = DENSE_STAGE_CONSUMED_DEGREE_9_KEYS * 9;
        if (raw_ordinal < degree_9_rows) {
            hub_ordinal = raw_ordinal / 9;
        } else {
            hub_ordinal = DENSE_STAGE_CONSUMED_DEGREE_9_KEYS + (raw_ordinal - degree_9_rows) / 10;
        }
        return {raw_ordinal, DENSE_STAGE_SINGLETON_KEYS + hub_ordinal};
    }

    const size_t core_row = raw_ordinal - DENSE_STAGE_SINGLETON_KEYS;
    size_t lhs = 0;
    size_t rhs = 0;
    if (core_row < DENSE_STAGE_CORE_OFFSET_ROWS) {
        const size_t offset = core_row / DENSE_STAGE_CORE_KEYS + 1;
        lhs = core_row % DENSE_STAGE_CORE_KEYS;
        rhs = (lhs + offset) % DENSE_STAGE_CORE_KEYS;
    } else {
        lhs = core_row - DENSE_STAGE_CORE_OFFSET_ROWS;
        rhs = lhs + DENSE_STAGE_CORE_KEYS / 2;
    }

    const size_t core_key_begin = DENSE_STAGE_SINGLETON_KEYS + DENSE_STAGE_CONSUMED_HUB_KEYS;
    return {core_key_begin + std::min(lhs, rhs), core_key_begin + std::max(lhs, rhs)};
}

[[nodiscard]] Relation make_dense_stage_relation(size_t raw_ordinal) {
    const uint64_t magnitude = static_cast<uint64_t>(raw_ordinal) + 3'000'001;
    const int64_t signed_magnitude = static_cast<int64_t>(magnitude);
    const int64_t a = (raw_ordinal & 1U) == 0 ? signed_magnitude : -signed_magnitude;
    Relation relation(a, magnitude + 1);

    auto [lhs, rhs] = dense_stage_row_key_ordinals(raw_ordinal);
    if (rhs < lhs)
        std::swap(lhs, rhs);
    relation.rational_large_prime.emplace_back(dense_stage_key_value(lhs), uint8_t{1});
    relation.rational_large_prime.emplace_back(dense_stage_key_value(rhs), uint8_t{1});
    return relation;
}

struct BuiltOOCSource final {
    std::unique_ptr<OOCRelationWriter> writer;
    OOCSnapshotDescriptor descriptor;
    CorpusDigest raw_digest;
    size_t rows_written = 0;
};

struct BuiltCollectorSource final {
    CorpusDigest raw_digest;
    size_t rows_written = 0;
    size_t duplicates_rejected = 0;
};

struct BuiltDenseStageSource final {
    CorpusDigest raw_digest;
    CorpusDigest expected_output_digest;
    size_t rows_written = 0;
};

[[nodiscard]] BuiltOOCSource build_ooc_source(const std::string& base_path, size_t row_count) {
    auto writer = std::make_unique<OOCRelationWriter>(base_path);
    CorpusDigestAccumulator digest(row_count);
    for (size_t raw_ordinal = 0; raw_ordinal < row_count; ++raw_ordinal) {
        const Relation relation = make_raw_relation(raw_ordinal, row_count);
        digest.append(relation);
        CHECK(writer->write(relation) == raw_ordinal);
    }
    const OOCSnapshotDescriptor descriptor = writer->finalize();
    CHECK(descriptor.count == row_count);
    CHECK(writer->count() == row_count);
    return {std::move(writer), descriptor, digest.finish(), row_count};
}

[[nodiscard]] BuiltCollectorSource build_collector_source(RelationCollector& collector,
                                                          size_t row_count) {
    const size_t expected_unique_rows = unique_row_count(row_count);
    CorpusDigestAccumulator digest(expected_unique_rows);
    size_t rows_written = 0;
    size_t duplicates_rejected = 0;
    for (size_t raw_ordinal = 0; raw_ordinal < row_count; ++raw_ordinal) {
        Relation relation = make_raw_relation(raw_ordinal, row_count);
        const bool duplicate = raw_ordinal % DUPLICATE_PERIOD == DUPLICATE_PERIOD - 1;
        if (!duplicate) {
            digest.append(relation);
        }
        const bool added = collector.add(std::move(relation));
        CHECK(added != duplicate);
        rows_written += added ? 1U : 0U;
        duplicates_rejected += added ? 0U : 1U;
    }

    const auto stats = collector.stats();
    CHECK(rows_written == expected_unique_rows);
    CHECK(duplicates_rejected == duplicate_count(row_count));
    CHECK(collector.size() == rows_written);
    CHECK(stats.total_relations == rows_written);
    CHECK(stats.duplicates_rejected == duplicates_rejected);
    return {digest.finish(), rows_written, duplicates_rejected};
}

[[nodiscard]] BuiltDenseStageSource build_dense_stage_source(RelationCollector& collector) {
    CorpusDigestAccumulator raw_digest(DENSE_STAGE_ROWS);
    CorpusDigestAccumulator output_digest(DENSE_STAGE_CORE_ROWS);
    size_t rows_written = 0;
    for (size_t raw_ordinal = 0; raw_ordinal < DENSE_STAGE_ROWS; ++raw_ordinal) {
        Relation relation = make_dense_stage_relation(raw_ordinal);
        raw_digest.append(relation);
        if (raw_ordinal >= DENSE_STAGE_SINGLETON_KEYS)
            output_digest.append(relation);
        CHECK(collector.add(std::move(relation)));
        ++rows_written;
    }

    const auto stats = collector.stats();
    CHECK(rows_written == DENSE_STAGE_ROWS);
    CHECK(collector.size() == DENSE_STAGE_ROWS);
    CHECK(stats.total_relations == DENSE_STAGE_ROWS);
    CHECK(stats.full_relations == 0);
    CHECK(stats.partial_1lp == 0);
    CHECK(stats.partial_2lp == DENSE_STAGE_ROWS);
    CHECK(stats.duplicates_rejected == 0);
    CHECK(stats.invalid_rejected == 0);
    const CorpusDigest observed_raw_digest = raw_digest.finish();
    const CorpusDigest observed_output_digest = output_digest.finish();
    CHECK(observed_raw_digest == DENSE_STAGE_EXPECTED_RAW_DIGEST);
    CHECK(observed_output_digest == DENSE_STAGE_EXPECTED_OUTPUT_DIGEST);
    return {observed_raw_digest, observed_output_digest, rows_written};
}

[[nodiscard]] size_t verify_ooc_source_payload(const std::string& base_path,
                                               const BuiltOOCSource& source) {
    OOCRelationReader reader(base_path, source.descriptor);
    CHECK(reader.count() == source.rows_written);
    CorpusDigestAccumulator observed(source.rows_written);
    size_t rows_read = 0;
    for (size_t raw_ordinal = 0; raw_ordinal < source.rows_written; ++raw_ordinal) {
        const Relation relation = reader.read(raw_ordinal);
        CHECK(relation_equal(relation, make_raw_relation(raw_ordinal, source.rows_written)));
        observed.append(relation);
        ++rows_read;
    }
    CHECK(rows_read == source.rows_written);
    CHECK(observed.finish() == source.raw_digest);
    return rows_read;
}

[[nodiscard]] size_t verify_collector_source_payload(const RelationCorpus& corpus,
                                                     const BuiltCollectorSource& source) {
    CHECK(corpus.storage_kind() == RelationStorageKind::FinalizedOOC);
    CHECK(corpus.count() == source.rows_written);
    CorpusDigestAccumulator observed(source.rows_written);
    size_t rows_read = 0;
    for (size_t raw_ordinal = 0; raw_ordinal < source.rows_written; ++raw_ordinal) {
        const Relation relation = corpus.read(raw_ordinal);
        CHECK(relation_equal(relation, make_unique_relation(raw_ordinal, source.rows_written)));
        observed.append(relation);
        ++rows_read;
    }
    CHECK(rows_read == source.rows_written);
    CHECK(observed.finish() == source.raw_digest);
    return rows_read;
}

[[nodiscard]] RelationReductionConfig structured_config(size_t row_count, uint32_t workers) {
    RelationReductionConfig config;
    config.large_primes_enabled = true;
    config.merge_rounds = 0;
    config.strategy = ReductionStrategy::Structured;
    config.structured =
        gnfs::relation::make_structured_filter_experimental_config(row_count, workers);
    return config;
}

[[nodiscard]] RelationReductionStats normalized_stats(const RelationReductionStats& stats) {
    RelationReductionStats normalized = stats;
    // These two fields intentionally describe execution shape, not semantic
    // output, so normalize them before cross-worker equivalence comparison.
    normalized.structured_incidence.requested_worker_count = 0;
    normalized.structured_incidence.peak_worker_count = 0;
    return normalized;
}

void check_common_result(const RelationReductionResult& result, size_t input_rows,
                         size_t unique_rows, size_t duplicates_removed, uint32_t workers,
                         const CorpusDigest& raw_digest) {
    const size_t component_groups = complete_component_groups(unique_rows);
    const size_t expected_output_rows = unique_rows - 2 * component_groups;
    const size_t shard_rows = std::min<size_t>(
        std::max<size_t>(unique_rows, 1),
        gnfs::relation::StructuredFilterExperimentalCaps::max_rows_per_incidence_shard);

    CHECK(result.stats.strategy == ReductionStrategy::Structured);
    CHECK(result.stats.input_relations == input_rows);
    CHECK(result.stats.raw_duplicates_removed == duplicates_removed);
    CHECK(result.stats.raw_input_digest == raw_digest);
    CHECK(result.stats.deduplicated_input_lp_histogram.weight_1 == 0);
    CHECK(result.stats.deduplicated_input_lp_histogram.weight_2 == component_groups);
    CHECK(result.stats.deduplicated_input_lp_histogram.weight_3 == component_groups);
    CHECK(result.stats.deduplicated_input_lp_histogram.weight_4plus == 0);
    CHECK(result.stats.deduplicated_input_lp_histogram.unique_keys == 2 * component_groups);
    CHECK(result.stats.pre_merge_lp_histogram == gnfs::relation::LpKeyWeightHistogram{});
    CHECK(result.stats.structured.input_rows == unique_rows);
    CHECK(result.stats.structured.output_rows == expected_output_rows);
    CHECK(result.stats.structured_run.stop_reason == StructuredReductionStopReason::NoCandidates);
    CHECK(result.stats.structured_run.commits > 0);
    CHECK(result.stats.structured_run.emitted_rows > 0);
    CHECK(result.stats.structured.two_way_merges > 0);
    CHECK(result.stats.structured.tree_basis_batches > 0);
    CHECK(result.stats.structured.candidate_plans_considered > 0);
    CHECK(result.stats.singleton_rows_removed == 0);
    CHECK(result.stats.merged_relations == 3 * component_groups);
    CHECK(result.stats.output_relations == expected_output_rows);
    CHECK(result.stats.output_lp_columns == 0);
    CHECK(result.size() == expected_output_rows);
    CHECK(result.stats.structured_incidence.shard_count ==
          (unique_rows + shard_rows - 1) / shard_rows);
    CHECK(result.stats.structured_incidence.peak_shard_rows == std::min(shard_rows, unique_rows));
    CHECK(result.stats.structured_incidence.total_incidence_entries == 5 * component_groups);
    CHECK(result.stats.structured_incidence.peak_shard_incidence_entries > 0);
    CHECK(result.stats.structured_incidence.requested_worker_count == workers);
    CHECK(result.stats.structured_incidence.peak_worker_count ==
          std::min<uint32_t>(workers, static_cast<uint32_t>(std::min(shard_rows, unique_rows))));
}

void check_dense_stage_result(const RelationReductionResult& result,
                              const BuiltDenseStageSource& source, uint32_t workers) {
    constexpr size_t shard_rows =
        gnfs::relation::StructuredFilterExperimentalCaps::max_rows_per_incidence_shard;
    constexpr size_t expected_shards = (DENSE_STAGE_ROWS + shard_rows - 1) / shard_rows;

    CHECK(result.stats.strategy == ReductionStrategy::Structured);
    CHECK(result.stats.input_relations == DENSE_STAGE_ROWS);
    CHECK(result.stats.raw_duplicates_removed == 0);
    CHECK(result.stats.raw_input_digest == source.raw_digest);
    CHECK(result.stats.deduplicated_input_lp_histogram.weight_1 == DENSE_STAGE_SINGLETON_KEYS);
    CHECK(result.stats.deduplicated_input_lp_histogram.weight_2 == 0);
    CHECK(result.stats.deduplicated_input_lp_histogram.weight_3 == 0);
    CHECK(result.stats.deduplicated_input_lp_histogram.weight_4plus ==
          DENSE_STAGE_WEIGHT_4PLUS_KEYS);
    CHECK(result.stats.deduplicated_input_lp_histogram.unique_keys == DENSE_STAGE_KEYS);
    CHECK(result.stats.pre_merge_lp_histogram == gnfs::relation::LpKeyWeightHistogram{});

    CHECK(result.stats.structured.input_rows == DENSE_STAGE_ROWS);
    CHECK(result.stats.structured.singleton_rows_removed == DENSE_STAGE_SINGLETON_KEYS);
    CHECK(result.stats.structured.two_way_merges == 0);
    CHECK(result.stats.structured.tree_basis_batches == 0);
    CHECK(result.stats.structured.budgeted_runs == 1);
    CHECK(result.stats.structured.planning_passes == 1);
    CHECK(result.stats.structured.candidate_plans_considered == 0);
    CHECK(result.stats.structured.output_rows == DENSE_STAGE_CORE_ROWS);
    CHECK(result.stats.structured.stop_reason == StructuredReductionStopReason::NoCandidates);
    CHECK(result.stats.structured_run.singleton_rows_removed == DENSE_STAGE_SINGLETON_KEYS);
    CHECK(result.stats.structured_run.commits == 0);
    CHECK(result.stats.structured_run.emitted_rows == 0);
    CHECK(result.stats.structured_run.lp_fill_growth == 0);
    CHECK(result.stats.structured_run.stop_reason == StructuredReductionStopReason::NoCandidates);

    CHECK(result.stats.singleton_rows_removed == DENSE_STAGE_SINGLETON_KEYS);
    CHECK(result.stats.merged_relations == 0);
    CHECK(result.stats.output_relations == DENSE_STAGE_CORE_ROWS);
    CHECK(result.stats.output_lp_columns == DENSE_STAGE_CORE_KEYS);
    CHECK(result.stats.output_digest == source.expected_output_digest);
    CHECK(result.size() == DENSE_STAGE_CORE_ROWS);

    CHECK(result.stats.structured_incidence.shard_count == expected_shards);
    CHECK(result.stats.structured_incidence.peak_shard_rows == shard_rows);
    CHECK(result.stats.structured_incidence.peak_shard_incidence_entries ==
          shard_rows * DENSE_STAGE_KEYS_PER_ROW);
    CHECK(result.stats.structured_incidence.total_incidence_entries == DENSE_STAGE_INCIDENCES);
    CHECK(result.stats.structured_incidence.requested_worker_count == workers);
    CHECK(result.stats.structured_incidence.peak_worker_count == workers);
}

struct ScaleOracle final {
    RelationReductionStats stats;
    std::vector<Relation> output_rows;
};

[[nodiscard]] ScaleOracle run_memory_oracle(size_t raw_row_count, uint64_t generation,
                                            const CorpusDigest& raw_digest) {
    std::vector<Relation> input;
    input.reserve(raw_row_count);
    for (size_t ordinal = 0; ordinal < raw_row_count; ++ordinal) {
        input.push_back(make_raw_relation(ordinal, raw_row_count));
    }

    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(generation, std::move(input)),
                                                  structured_config(raw_row_count, 1));
    CHECK(result.storage_kind() == RelationStorageKind::InMemory);
    check_common_result(result, raw_row_count, unique_row_count(raw_row_count),
                        duplicate_count(raw_row_count), 1, raw_digest);
    RelationReductionStats stats = normalized_stats(result.stats);
    std::vector<Relation> output_rows = std::move(result).take_relations();
    CHECK(output_rows.size() == stats.output_relations);
    return {std::move(stats), std::move(output_rows)};
}

[[nodiscard]] ScaleOracle run_unique_memory_oracle(size_t unique_rows, uint64_t generation,
                                                   const CorpusDigest& raw_digest) {
    std::vector<Relation> input;
    input.reserve(unique_rows);
    for (size_t ordinal = 0; ordinal < unique_rows; ++ordinal) {
        input.push_back(make_unique_relation(ordinal, unique_rows));
    }

    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(generation, std::move(input)),
                                                  structured_config(unique_rows, 1));
    CHECK(result.storage_kind() == RelationStorageKind::InMemory);
    check_common_result(result, unique_rows, unique_rows, 0, 1, raw_digest);
    RelationReductionStats stats = normalized_stats(result.stats);
    std::vector<Relation> output_rows = std::move(result).take_relations();
    CHECK(output_rows.size() == stats.output_relations);
    return {std::move(stats), std::move(output_rows)};
}

void run_owning_ooc_case(size_t raw_row_count, uint64_t generation, const std::string& input_base,
                         const BuiltOOCSource& source, const ScaleOracle& oracle, uint32_t workers,
                         bool remove_input_artifacts) {
    const std::string working_base = unique_ooc_base("working", raw_row_count, workers);
    const std::string output_base = unique_ooc_base("output", raw_row_count, workers);
    ArtifactCleanup cleanup;
    cleanup.add(working_base);
    cleanup.add(output_base);

    RelationCorpus corpus = RelationCorpus::from_owned_finalized_ooc(
        generation, *source.writer,
        remove_input_artifacts ? OOCCleanupPolicy::RemoveArtifacts : OOCCleanupPolicy::Preserve);
    CHECK(corpus.storage_kind() == RelationStorageKind::FinalizedOOC);
    CHECK(corpus.count() == raw_row_count);

    RelationReductionConfig config = structured_config(raw_row_count, workers);
    config.structured->deduplicated_ooc_base_path = working_base;
    config.structured->output_ooc_base_path = output_base;
    config.structured->output_ooc_cleanup = OOCCleanupPolicy::RemoveArtifacts;

    {
        auto result =
            RelationReductionEngine::reduce(RawRelationSnapshot(std::move(corpus)), config);
        CHECK(result.storage_kind() == RelationStorageKind::FinalizedOOC);
        CHECK(private_sink_exists(output_base));
        CHECK(private_sink_absent(working_base));
        CHECK(normalized_stats(result.stats) == oracle.stats);
        check_common_result(result, raw_row_count, unique_row_count(raw_row_count),
                            duplicate_count(raw_row_count), workers, source.raw_digest);

        CHECK(result.size() == oracle.output_rows.size());
        CorpusDigestAccumulator observed_output(result.size());
        size_t output_rows_read = 0;
        for (size_t ordinal = 0; ordinal < result.size(); ++ordinal) {
            const Relation relation = result.read(ordinal);
            CHECK(relation_equal(relation, oracle.output_rows[ordinal]));
            observed_output.append(relation);
            ++output_rows_read;
        }
        CHECK(output_rows_read == oracle.output_rows.size());
        CHECK(observed_output.finish() == result.stats.output_digest);

        std::cout << "[structured-ooc-scale] rows=" << raw_row_count << " workers=" << workers
                  << " source_rows=" << source.rows_written
                  << " deduplicated_source_rows=" << unique_row_count(raw_row_count)
                  << " output_rows_read=" << result.size() << " incidence_entries="
                  << result.stats.structured_incidence.total_incidence_entries
                  << " commits=" << result.stats.structured_run.commits
                  << " source_backend=finalized-ooc" << " output_backend=finalized-ooc\n";
    }
    CHECK(private_sink_absent(output_base));
    CHECK(private_sink_absent(working_base));
    if (remove_input_artifacts) {
        CHECK(ordinary_artifacts_absent(input_base));
    } else {
        CHECK(ordinary_artifacts_exist(input_base));
    }
}

[[nodiscard]] OOCSnapshotDescriptor
run_borrowed_ooc_case(size_t raw_row_count, uint64_t generation, const std::string& input_base,
                      RelationCollector& collector, const BuiltCollectorSource& source,
                      const ScaleOracle& oracle, uint32_t workers) {
    const std::string output_base = unique_ooc_base("output", raw_row_count, workers);
    ArtifactCleanup cleanup;
    cleanup.add(output_base);

    RelationReductionConfig config = structured_config(source.rows_written, workers);
    config.structured->output_ooc_base_path = output_base;
    config.structured->output_ooc_cleanup = OOCCleanupPolicy::RemoveArtifacts;

    OOCSnapshotDescriptor source_descriptor;
    {
        auto run =
            collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& prefix) {
                auto result = RelationReductionEngine::reduce_direct_borrowed_structured(
                    generation, prefix, config);
                return std::pair(std::move(result), prefix.descriptor());
            });

        // The parallel reducer and every worker finish inside the callback.
        // with_unique_ooc_prefix then destroys the raw reader and exactly
        // resumes the collector before it exposes this owning output result.
        auto& result = run.first;
        source_descriptor = run.second;
        CHECK(source_descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(source_descriptor.store_id != 0);
        CHECK(source_descriptor.generation != 0);
        CHECK(source_descriptor.count == source.rows_written);
        CHECK(source_descriptor.data_end > OOCRelationWriter::DATA_HEADER_BYTES);
        CHECK(collector.size() == source.rows_written);
        CHECK(ordinary_artifacts_exist(input_base));
        CHECK(result.storage_kind() == RelationStorageKind::FinalizedOOC);
        CHECK(private_sink_exists(output_base));
        CHECK(normalized_stats(result.stats) == oracle.stats);
        check_common_result(result, source.rows_written, source.rows_written, 0, workers,
                            source.raw_digest);

        CHECK(result.size() == oracle.output_rows.size());
        CorpusDigestAccumulator observed_output(result.size());
        size_t output_rows_read = 0;
        for (size_t ordinal = 0; ordinal < result.size(); ++ordinal) {
            const Relation relation = result.read(ordinal);
            CHECK(relation_equal(relation, oracle.output_rows[ordinal]));
            observed_output.append(relation);
            ++output_rows_read;
        }
        CHECK(output_rows_read == oracle.output_rows.size());
        CHECK(observed_output.finish() == result.stats.output_digest);

        std::cout << "[structured-ooc-scale] rows=" << raw_row_count << " workers=" << workers
                  << " source_rows=" << source.rows_written
                  << " collector_duplicates_rejected=" << source.duplicates_rejected
                  << " output_rows_read=" << result.size() << " incidence_entries="
                  << result.stats.structured_incidence.total_incidence_entries
                  << " commits=" << result.stats.structured_run.commits
                  << " source_backend=collector-direct-borrowed-prefix"
                  << " output_backend=finalized-ooc\n";
    }
    CHECK(private_sink_absent(output_base));
    CHECK(ordinary_artifacts_exist(input_base));
    CHECK(collector.size() == source.rows_written);
    return source_descriptor;
}

void run_owning_finalized_smoke() {
    current_case = "structured owning finalized OOC smoke";
    constexpr size_t raw_row_count = 1'025;
    constexpr uint64_t generation = 72'000;
    const std::string input_base = unique_ooc_base("owning_input", raw_row_count);
    ArtifactCleanup cleanup;
    cleanup.add(input_base);

    const BuiltOOCSource source = build_ooc_source(input_base, raw_row_count);
    CHECK(ordinary_artifacts_exist(input_base));
    CHECK(verify_ooc_source_payload(input_base, source) == raw_row_count);
    const ScaleOracle oracle = run_memory_oracle(raw_row_count, generation, source.raw_digest);
    run_owning_ooc_case(raw_row_count, generation, input_base, source, oracle, 1, true);
    CHECK(ordinary_artifacts_absent(input_base));
}

void run_scale(size_t raw_row_count, uint64_t generation) {
    current_case = "structured OOC scale";
    const std::string input_base = unique_ooc_base("input", raw_row_count);
    ArtifactCleanup cleanup;
    cleanup.add(input_base);

    CollectorConfig collector_config;
    collector_config.check_duplicates = true;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = input_base;
    RelationCollector collector(collector_config);
    const BuiltCollectorSource source = build_collector_source(collector, raw_row_count);
    CHECK(ordinary_artifacts_exist(input_base));
    const ScaleOracle oracle =
        run_unique_memory_oracle(source.rows_written, generation, source.raw_digest);

    constexpr std::array<uint32_t, 3> worker_counts{1, 2, 4};
    OOCSnapshotDescriptor previous_descriptor;
    bool have_previous_descriptor = false;
    for (size_t index = 0; index < worker_counts.size(); ++index) {
        const uint32_t workers = worker_counts[index];
#ifndef NDEBUG
        // Debug retains the full 5K cross-worker oracle while running one
        // representative threaded pass at the larger source sizes. Release
        // executes the complete 5K/50K/200K x 1/2/4 matrix.
        if (raw_row_count > 5'000 && workers != 4) {
            continue;
        }
#endif
        const OOCSnapshotDescriptor descriptor = run_borrowed_ooc_case(
            raw_row_count, generation, input_base, collector, source, oracle, workers);
        if (have_previous_descriptor) {
            CHECK(descriptor.store_id == previous_descriptor.store_id);
            CHECK(descriptor.generation > previous_descriptor.generation);
            CHECK(descriptor.count == previous_descriptor.count);
            CHECK(descriptor.data_end == previous_descriptor.data_end);
        }
        previous_descriptor = descriptor;
        have_previous_descriptor = true;
    }
    CHECK(have_previous_descriptor);
    CHECK(collector.size() == source.rows_written);
    CHECK(ordinary_artifacts_exist(input_base));

    {
        RelationCorpus raw_corpus =
            collector.handoff_ooc_corpus(generation, OOCCleanupPolicy::RemoveArtifacts);
        const auto raw_scope = raw_corpus.ooc_artifact_scope();
        CHECK(raw_scope.has_value());
        CHECK(raw_scope->base_path == std::filesystem::weakly_canonical(input_base).string());
        CHECK(raw_scope->descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(raw_scope->descriptor.store_id == previous_descriptor.store_id);
        CHECK(raw_scope->descriptor.generation > previous_descriptor.generation);
        CHECK(raw_scope->descriptor.count == previous_descriptor.count);
        CHECK(raw_scope->descriptor.data_end == previous_descriptor.data_end);
        CHECK(verify_collector_source_payload(raw_corpus, source) == source.rows_written);
    }
    CHECK(ordinary_artifacts_absent(input_base));
}

struct RssCaseArguments final {
    size_t rows = 0;
    uint32_t workers = 0;
};

struct DenseStageCaseArguments final {
    uint32_t workers = 0;
};

[[nodiscard]] std::optional<uint64_t> parse_unsigned(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<RssCaseArguments> parse_rss_case_arguments(int argc,
                                                                       char* argv[]) noexcept {
    if (argc != 4 || std::string_view(argv[1]) != "--rss-case") {
        return std::nullopt;
    }

    const auto parsed_rows = parse_unsigned(argv[2]);
    const auto parsed_workers = parse_unsigned(argv[3]);
    if (!parsed_rows.has_value() || !parsed_workers.has_value()) {
        return std::nullopt;
    }

    constexpr std::array<uint64_t, 3> allowed_rows{5'000, 50'000, 200'000};
    constexpr std::array<uint64_t, 3> allowed_workers{1, 2, 4};
    if (std::find(allowed_rows.begin(), allowed_rows.end(), *parsed_rows) == allowed_rows.end() ||
        std::find(allowed_workers.begin(), allowed_workers.end(), *parsed_workers) ==
            allowed_workers.end()) {
        return std::nullopt;
    }

    return RssCaseArguments{static_cast<size_t>(*parsed_rows),
                            static_cast<uint32_t>(*parsed_workers)};
}

[[nodiscard]] std::optional<DenseStageCaseArguments>
parse_dense_stage_case_arguments(int argc, char* argv[]) noexcept {
    if (argc != 3 || std::string_view(argv[1]) != "--dense-stage-case") {
        return std::nullopt;
    }

    const auto parsed_workers = parse_unsigned(argv[2]);
    if (!parsed_workers.has_value() || (*parsed_workers != 1 && *parsed_workers != 4)) {
        return std::nullopt;
    }
    return DenseStageCaseArguments{static_cast<uint32_t>(*parsed_workers)};
}

[[nodiscard]] std::string optional_metric(const std::optional<uint64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "na";
}

[[nodiscard]] std::optional<uint64_t>
checked_peak_growth(const ProcessMemorySnapshot& baseline,
                    const ProcessMemorySnapshot& final) noexcept {
    if (baseline.backend == ProcessMemoryBackend::Unsupported ||
        final.backend != baseline.backend || !baseline.lifetime_peak_rss_bytes.has_value() ||
        !final.lifetime_peak_rss_bytes.has_value() ||
        *final.lifetime_peak_rss_bytes < *baseline.lifetime_peak_rss_bytes) {
        return std::nullopt;
    }
    return *final.lifetime_peak_rss_bytes - *baseline.lifetime_peak_rss_bytes;
}

constexpr std::array<StructuredTelemetryReadPhase, 4> DENSE_STAGE_READ_PHASES{
    StructuredTelemetryReadPhase::InitialScan,
    StructuredTelemetryReadPhase::IncidenceBuild,
    StructuredTelemetryReadPhase::Reducer,
    StructuredTelemetryReadPhase::FreshValidation,
};

constexpr std::array<StructuredTelemetryCheckpoint, 10> DENSE_STAGE_CHECKPOINTS{
    StructuredTelemetryCheckpoint::ScanBegin,
    StructuredTelemetryCheckpoint::ScanCompleteBeforeAbRelease,
    StructuredTelemetryCheckpoint::AfterAbRelease,
    StructuredTelemetryCheckpoint::IncidenceReceiptBuilt,
    StructuredTelemetryCheckpoint::ReducerConstructed,
    StructuredTelemetryCheckpoint::ReductionComplete,
    StructuredTelemetryCheckpoint::OutputMaterialized,
    StructuredTelemetryCheckpoint::OutputFinalized,
    StructuredTelemetryCheckpoint::ReducerReleased,
    StructuredTelemetryCheckpoint::FreshValidationComplete,
};

static_assert(DENSE_STAGE_READ_PHASES.size() ==
              gnfs::relation::structured_telemetry_read_phase_count);
static_assert(DENSE_STAGE_CHECKPOINTS.size() ==
              gnfs::relation::structured_telemetry_checkpoint_count);

[[nodiscard]] constexpr const char* bool_token(bool value) noexcept {
    return value ? "true" : "false";
}

[[nodiscard]] std::string_view
dense_stage_stop_reason_name(StructuredReductionStopReason reason) noexcept {
    switch (reason) {
    case StructuredReductionStopReason::NotStarted:
        return "not_started";
    case StructuredReductionStopReason::NoCandidates:
        return "no_candidates";
    case StructuredReductionStopReason::BudgetLimit:
        return "budget_limit";
    case StructuredReductionStopReason::PersistenceLimit:
        return "persistence_limit";
    }
    return "unknown";
}

void check_dense_stage_telemetry(const StructuredReductionTelemetryRecord& record,
                                 uint64_t generation) {
    CHECK(record.schema_version == StructuredReductionTelemetryRecord::current_schema_version);
    CHECK(record.generation == generation);
    CHECK(record.source_rows == DENSE_STAGE_ROWS);
    CHECK(record.incidence_rows == DENSE_STAGE_ROWS);
    CHECK(record.incidence_unique_keys == DENSE_STAGE_KEYS);
    CHECK(record.incidence_entries == DENSE_STAGE_INCIDENCES);
    CHECK(record.completed);
    CHECK(record.succeeded);
    CHECK(record.failure_stage == gnfs::relation::StructuredTelemetryFailureStage::None);
    CHECK(record.last_checkpoint == StructuredTelemetryCheckpoint::FreshValidationComplete);
    CHECK(!record.counter_overflow);
    CHECK(record.clock_monotone);
    CHECK(record.peak_monotone);
    CHECK(record.clock_provider_failures == 0);
    CHECK(record.memory_provider_failures == 0);

    const auto check_reads = [&](StructuredTelemetryReadPhase phase, uint64_t expected) {
        const auto& counters = record.reads[static_cast<size_t>(phase)];
        CHECK(counters.attempts == expected);
        CHECK(counters.successes == expected);
        CHECK(counters.failures == 0);
    };
    check_reads(StructuredTelemetryReadPhase::InitialScan, DENSE_STAGE_ROWS);
    check_reads(StructuredTelemetryReadPhase::IncidenceBuild, 0);
    check_reads(StructuredTelemetryReadPhase::Reducer, DENSE_STAGE_CORE_ROWS);
    check_reads(StructuredTelemetryReadPhase::FreshValidation, DENSE_STAGE_ROWS);

    uint64_t previous_wall_ns = 0;
    const auto memory_backend = record.checkpoints.front().memory.backend;
    for (size_t index = 0; index < DENSE_STAGE_CHECKPOINTS.size(); ++index) {
        const auto& sample = record.checkpoints[index];
        CHECK(sample.observed);
        CHECK(sample.wall_supported);
        CHECK(sample.elapsed_wall_ns >= previous_wall_ns);
        CHECK(sample.memory.backend == memory_backend);
        previous_wall_ns = sample.elapsed_wall_ns;
    }
    CHECK(record.checkpoints.front().elapsed_wall_ns == 0);
}

void print_dense_stage_record(uint32_t workers, uint64_t generation,
                              const RelationReductionStats& stats,
                              const StructuredReductionTelemetryRecord& telemetry) {
    std::ostringstream record;
    record << "GNFS_STRUCTURED_OOC_DENSE_STAGE_V1";
    record << " schema=1";
    record << " status=pass";
    record << " scope=direct_observed_route";
    record << " process_rss_scope=self_lifetime";
    record << " fixture=synthetic_cardinality_anchor";
    record << " topology=singleton_spokes_degree9_circulant_v1";
    record << " build_type=" << STRUCTURED_OOC_SCALE_BUILD_TYPE;
    record << " generation=" << generation;
    record << " workers=" << workers;
    record << " source_rows=" << DENSE_STAGE_ROWS;
    record << " incidence_rows=" << DENSE_STAGE_ROWS;
    record << " incidence_unique_keys=" << DENSE_STAGE_KEYS;
    record << " incidence_entries=" << DENSE_STAGE_INCIDENCES;
    record << " input_lp_w1=" << DENSE_STAGE_SINGLETON_KEYS;
    record << " input_lp_w2=0";
    record << " input_lp_w3=0";
    record << " input_lp_w4plus=" << DENSE_STAGE_WEIGHT_4PLUS_KEYS;
    record << " incidence_shards=" << stats.structured_incidence.shard_count;
    record << " peak_shard_rows=" << stats.structured_incidence.peak_shard_rows;
    record << " peak_shard_entries=" << stats.structured_incidence.peak_shard_incidence_entries;
    record << " peak_incidence_workers=" << stats.structured_incidence.peak_worker_count;
    record << " singleton_rows_removed=" << stats.singleton_rows_removed;
    record << " core_rows=" << DENSE_STAGE_CORE_ROWS;
    record << " structured_stop=" << dense_stage_stop_reason_name(stats.structured_run.stop_reason);
    record << " commits=" << stats.structured_run.commits;
    record << " emitted_rows=" << stats.structured_run.emitted_rows;
    record << " output_rows=" << stats.output_relations;
    record << " output_lp_columns=" << stats.output_lp_columns;
    record << " raw_digest_low=" << stats.raw_input_digest.low;
    record << " raw_digest_high=" << stats.raw_input_digest.high;
    record << " output_digest_low=" << stats.output_digest.low;
    record << " output_digest_high=" << stats.output_digest.high;
    record << " source_backend=collector_direct_borrowed_prefix";
    record << " output_backend=finalized_ooc";
    record << " output_published=true";
    record << " output_lease_removed=true";
    record << " source_resumed=true";
    record << " source_pair_removed=true";
    record << " telemetry_completed=" << bool_token(telemetry.completed);
    record << " telemetry_succeeded=" << bool_token(telemetry.succeeded);
    record << " telemetry_failure_stage="
           << gnfs::relation::structured_telemetry_failure_stage_name(telemetry.failure_stage);
    record << " telemetry_last_checkpoint="
           << (telemetry.last_checkpoint.has_value()
                   ? gnfs::relation::structured_telemetry_checkpoint_name(
                         *telemetry.last_checkpoint)
                   : std::string_view("none"));
    record << " telemetry_counter_overflow=" << bool_token(telemetry.counter_overflow);
    record << " telemetry_clock_monotone=" << bool_token(telemetry.clock_monotone);
    record << " telemetry_peak_monotone=" << bool_token(telemetry.peak_monotone);
    record << " telemetry_clock_provider_failures=" << telemetry.clock_provider_failures;
    record << " telemetry_memory_provider_failures=" << telemetry.memory_provider_failures;
    record << " memory_backend="
           << process_memory_backend_name(telemetry.checkpoints.front().memory.backend);

    for (const auto phase : DENSE_STAGE_READ_PHASES) {
        const std::string_view name = gnfs::relation::structured_telemetry_read_phase_name(phase);
        const auto& counters = telemetry.reads[static_cast<size_t>(phase)];
        record << " read_" << name << "_attempts=" << counters.attempts << " read_" << name
               << "_successes=" << counters.successes << " read_" << name
               << "_failures=" << counters.failures;
    }

    for (const auto checkpoint : DENSE_STAGE_CHECKPOINTS) {
        const std::string_view name =
            gnfs::relation::structured_telemetry_checkpoint_name(checkpoint);
        const auto& sample = telemetry.checkpoints[static_cast<size_t>(checkpoint)];
        record << " cp_" << name << "_observed=" << bool_token(sample.observed) << " cp_" << name
               << "_wall_supported=" << bool_token(sample.wall_supported) << " cp_" << name
               << "_wall_ns=" << sample.elapsed_wall_ns << " cp_" << name
               << "_current_rss=" << optional_metric(sample.memory.current_rss_bytes) << " cp_"
               << name << "_peak_rss=" << optional_metric(sample.memory.lifetime_peak_rss_bytes);
    }
    std::cout << record.str() << '\n';
}

void run_dense_stage_case(uint32_t workers) {
    current_case = "structured dense stage replay";
    constexpr uint64_t generation = 73'002;
    const std::string input_base = unique_ooc_base("dense_stage_input", DENSE_STAGE_ROWS, workers);
    const std::string output_base =
        unique_ooc_base("dense_stage_output", DENSE_STAGE_ROWS, workers);
    ArtifactCleanup cleanup;
    cleanup.add(input_base);
    cleanup.add(output_base);

    CollectorConfig collector_config;
    collector_config.check_duplicates = true;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = input_base;
    RelationCollector collector(collector_config);
    const BuiltDenseStageSource source = build_dense_stage_source(collector);
    CHECK(ordinary_artifacts_exist(input_base));

    RelationReductionConfig config = structured_config(DENSE_STAGE_ROWS, workers);
    config.structured->output_ooc_base_path = output_base;
    config.structured->output_ooc_cleanup = OOCCleanupPolicy::RemoveArtifacts;

    StructuredReductionTelemetry telemetry;
    OOCSnapshotDescriptor source_descriptor;
    RelationReductionStats result_stats;
    StructuredReductionTelemetryRecord telemetry_record;
    {
        auto run =
            collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& prefix) {
                auto result = RelationReductionEngine::reduce_direct_borrowed_structured_observed(
                    generation, prefix, config, telemetry);
                return std::pair(std::move(result), prefix.descriptor());
            });

        auto& result = run.first;
        source_descriptor = run.second;
        CHECK(source_descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(source_descriptor.store_id != 0);
        CHECK(source_descriptor.generation != 0);
        CHECK(source_descriptor.count == DENSE_STAGE_ROWS);
        CHECK(source_descriptor.data_end > OOCRelationWriter::DATA_HEADER_BYTES);
        CHECK(collector.size() == DENSE_STAGE_ROWS);
        CHECK(ordinary_artifacts_exist(input_base));

        CHECK(result.generation == generation);
        CHECK(result.storage_kind() == RelationStorageKind::FinalizedOOC);
        CHECK(private_sink_exists(output_base));
        check_dense_stage_result(result, source, workers);
        const auto output_scope = result.relation_corpus().ooc_artifact_scope();
        CHECK(output_scope.has_value());
        CHECK(output_scope->base_path ==
              std::filesystem::weakly_canonical(private_sink_base(output_base)).string());
        CHECK(output_scope->descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(output_scope->descriptor.count == DENSE_STAGE_CORE_ROWS);

        telemetry_record = telemetry.snapshot();
        check_dense_stage_telemetry(telemetry_record, generation);
        result_stats = result.stats;
    }
    CHECK(private_sink_absent(output_base));
    CHECK(ordinary_artifacts_exist(input_base));
    CHECK(collector.size() == DENSE_STAGE_ROWS);

    {
        RelationCorpus raw_corpus =
            collector.handoff_ooc_corpus(generation, OOCCleanupPolicy::RemoveArtifacts);
        const auto raw_scope = raw_corpus.ooc_artifact_scope();
        CHECK(raw_scope.has_value());
        CHECK(raw_scope->base_path == std::filesystem::weakly_canonical(input_base).string());
        CHECK(raw_scope->descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(raw_scope->descriptor.store_id == source_descriptor.store_id);
        CHECK(raw_scope->descriptor.generation > source_descriptor.generation);
        CHECK(raw_scope->descriptor.count == source_descriptor.count);
        CHECK(raw_scope->descriptor.data_end == source_descriptor.data_end);
        CHECK(raw_corpus.count() == DENSE_STAGE_ROWS);
    }
    CHECK(ordinary_artifacts_absent(input_base));
    print_dense_stage_record(workers, generation, result_stats, telemetry_record);
}

void run_rss_case(size_t raw_row_count, uint32_t workers) {
    current_case = "structured direct OOC RSS case";
    constexpr uint64_t generation = 73'001;
    const std::string input_base = unique_ooc_base("rss_input", raw_row_count, workers);
    const std::string output_base = unique_ooc_base("rss_output", raw_row_count, workers);
    ArtifactCleanup cleanup;
    cleanup.add(input_base);
    cleanup.add(output_base);

    CollectorConfig collector_config;
    collector_config.check_duplicates = true;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = input_base;
    RelationCollector collector(collector_config);
    const BuiltCollectorSource source = build_collector_source(collector, raw_row_count);
    CHECK(ordinary_artifacts_exist(input_base));

    RelationReductionConfig config = structured_config(source.rows_written, workers);
    config.structured->output_ooc_base_path = output_base;
    config.structured->output_ooc_cleanup = OOCCleanupPolicy::RemoveArtifacts;

    const ProcessMemorySnapshot baseline_memory = process_memory_snapshot();
    const auto wall_begin = std::chrono::steady_clock::now();
    OOCSnapshotDescriptor source_descriptor;
    size_t output_rows = 0;
    {
        auto run =
            collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& prefix) {
                auto result = RelationReductionEngine::reduce_direct_borrowed_structured(
                    generation, prefix, config);
                return std::pair(std::move(result), prefix.descriptor());
            });

        auto& result = run.first;
        source_descriptor = run.second;
        CHECK(source_descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(source_descriptor.store_id != 0);
        CHECK(source_descriptor.generation != 0);
        CHECK(source_descriptor.count == source.rows_written);
        CHECK(source_descriptor.data_end > OOCRelationWriter::DATA_HEADER_BYTES);
        CHECK(collector.size() == source.rows_written);
        CHECK(ordinary_artifacts_exist(input_base));

        CHECK(result.generation == generation);
        CHECK(result.storage_kind() == RelationStorageKind::FinalizedOOC);
        CHECK(private_sink_exists(output_base));
        check_common_result(result, source.rows_written, source.rows_written, 0, workers,
                            source.raw_digest);
        output_rows = result.size();

        const auto output_scope = result.relation_corpus().ooc_artifact_scope();
        CHECK(output_scope.has_value());
        CHECK(output_scope->base_path ==
              std::filesystem::weakly_canonical(private_sink_base(output_base)).string());
        CHECK(output_scope->descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(output_scope->descriptor.count == output_rows);
    }
    CHECK(private_sink_absent(output_base));
    CHECK(ordinary_artifacts_exist(input_base));
    CHECK(collector.size() == source.rows_written);

    const auto wall_end = std::chrono::steady_clock::now();
    const ProcessMemorySnapshot final_memory = process_memory_snapshot();
    const auto peak_growth = checked_peak_growth(baseline_memory, final_memory);
    const bool supported = peak_growth.has_value();
    const auto wall_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_begin).count();

    {
        RelationCorpus raw_corpus =
            collector.handoff_ooc_corpus(generation, OOCCleanupPolicy::RemoveArtifacts);
        const auto raw_scope = raw_corpus.ooc_artifact_scope();
        CHECK(raw_scope.has_value());
        CHECK(raw_scope->base_path == std::filesystem::weakly_canonical(input_base).string());
        CHECK(raw_scope->descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
        CHECK(raw_scope->descriptor.store_id == source_descriptor.store_id);
        CHECK(raw_scope->descriptor.generation > source_descriptor.generation);
        CHECK(raw_scope->descriptor.count == source_descriptor.count);
        CHECK(raw_scope->descriptor.data_end == source_descriptor.data_end);
        CHECK(raw_corpus.count() == source.rows_written);
    }
    CHECK(ordinary_artifacts_absent(input_base));

    std::cout << "GNFS_RESOURCE_V1" << " scope=self" << " unit=bytes"
              << " backend=" << process_memory_backend_name(baseline_memory.backend)
              << " supported=" << (supported ? "true" : "false") << " rows=" << raw_row_count
              << " workers=" << workers << " source_rows=" << source.rows_written
              << " output_rows=" << output_rows
              << " baseline_current=" << optional_metric(baseline_memory.current_rss_bytes)
              << " baseline_peak=" << optional_metric(baseline_memory.lifetime_peak_rss_bytes)
              << " final_current=" << optional_metric(final_memory.current_rss_bytes)
              << " final_peak=" << optional_metric(final_memory.lifetime_peak_rss_bytes)
              << " peak_growth=" << optional_metric(peak_growth) << " wall_ns=" << wall_ns
              << " source_backend=collector-direct-borrowed-prefix"
              << " output_backend=finalized-ooc\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 1) {
        const auto dense_stage_case = parse_dense_stage_case_arguments(argc, argv);
        if (dense_stage_case.has_value()) {
            if (STRUCTURED_OOC_SCALE_BUILD_TYPE != "Release") {
                std::cerr << "dense structured stage replay requires a Release build\n";
                return 2;
            }
            try {
                run_dense_stage_case(dense_stage_case->workers);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                return 1;
            }
            return 0;
        }

        const auto rss_case = parse_rss_case_arguments(argc, argv);
        if (!rss_case.has_value()) {
            std::cerr << "Usage: " << argv[0]
                      << " [--rss-case <5000|50000|200000> <1|2|4> | "
                         "--dense-stage-case <1|4>]\n";
            return 2;
        }

        try {
            run_rss_case(rss_case->rows, rss_case->workers);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
        return 0;
    }

    try {
        run_owning_finalized_smoke();
        run_scale(5'000, 72'001);
        run_scale(50'000, 72'002);
        run_scale(200'000, 72'003);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "Structured OOC source scale/determinism tests passed (" << checks << " checks)\n";
    return 0;
}
