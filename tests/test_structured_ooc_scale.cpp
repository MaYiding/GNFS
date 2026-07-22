#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/ooc_relation_store.hpp"
#include "gnfs/relation/reduction_engine.hpp"
#include "gnfs/relation/structured_filter_profile.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
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

namespace {

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
            std::filesystem::remove_all(base_path + ".gnfs-sink-lease", ignored);
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

struct BuiltOOCSource final {
    OOCSnapshotDescriptor descriptor;
    CorpusDigest raw_digest;
    size_t rows_written = 0;
};

struct BuiltCollectorSource final {
    CorpusDigest raw_digest;
    size_t rows_written = 0;
    size_t duplicates_rejected = 0;
};

[[nodiscard]] BuiltOOCSource build_ooc_source(const std::string& base_path, size_t row_count) {
    OOCRelationWriter writer(base_path);
    CorpusDigestAccumulator digest(row_count);
    for (size_t raw_ordinal = 0; raw_ordinal < row_count; ++raw_ordinal) {
        const Relation relation = make_raw_relation(raw_ordinal, row_count);
        digest.append(relation);
        CHECK(writer.write(relation) == raw_ordinal);
    }
    const OOCSnapshotDescriptor descriptor = writer.finalize();
    CHECK(descriptor.count == row_count);
    CHECK(writer.count() == row_count);
    return {descriptor, digest.finish(), row_count};
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

    RelationCorpus corpus = RelationCorpus::from_finalized_ooc(
        generation, input_base, source.descriptor,
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
                  << " source_backend=finalized-ooc"
                  << " output_backend=finalized-ooc\n";
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

} // namespace

int main() {
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
