#include <gnfs/api/pipeline.hpp>

#include <gnfs/api/detail/solver_handoff.hpp>

#include <gnfs/cofactor/candidate_batch.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/factor_base/fb_checkpoint.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/block_wiedemann.hpp>
#include <gnfs/linalg/linalg_mmap_policy.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/mmap_csr_matrix.hpp>
#include <gnfs/linalg/relation_source.hpp>
#include <gnfs/linalg/sge.hpp>
#include <gnfs/linalg/sge_streaming.hpp>
#include <gnfs/polynomial/poly_checkpoint.hpp>
#include <gnfs/polynomial/selector_dispatch.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/ooc_policy.hpp>
#include <gnfs/relation/reduction_engine.hpp>
#include <gnfs/relation/relation_corpus.hpp>
#include <gnfs/relation/structured_filter_profile.hpp>
#include <gnfs/relation/structured_reduction_telemetry.hpp>
#include <gnfs/relation/v0_bfs_policy.hpp>
#include <gnfs/sieve/distributed_sieve.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/sieve/local_thread_budget.hpp>
#include <gnfs/sieve/sieve_checkpoint.hpp>
#include <gnfs/sieve/sieve_run_identity.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/siqs/siqs.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/util/bit_intrin.hpp>
#include <gnfs/util/ordered_parallel_map.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/process_memory.hpp>
#include <gnfs/util/safe_math.hpp>
#include <gnfs/util/temp_path.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>  // fprintf for V3 cascade stderr signal
#include <cstdlib> // getenv for GNFS_CASCADE_V3 flag
#include <cstring> // strlen for SGE-OOC ENV string checks
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace gnfs::api {

bool detail::parse_structured_filter_stage_telemetry(const char* raw_value) {
    if (raw_value == nullptr || std::string_view(raw_value) == "0") {
        return false;
    }
    if (std::string_view(raw_value) == "1") {
        return true;
    }
    throw std::invalid_argument(
        "GNFS_STRUCTURED_FILTER_STAGE_TELEMETRY must be unset, exact 0, or exact 1");
}

namespace {

class FactorStatsRollback final {
public:
    explicit FactorStatsRollback(FactorStats& target) : target_(&target), snapshot_(target) {
        static_assert(std::is_nothrow_move_assignable_v<FactorStats>);
    }

    FactorStatsRollback(const FactorStatsRollback&) = delete;
    FactorStatsRollback& operator=(const FactorStatsRollback&) = delete;

    ~FactorStatsRollback() {
        if (target_ != nullptr) {
            *target_ = std::move(snapshot_);
        }
    }

    void commit() noexcept {
        target_ = nullptr;
    }

private:
    FactorStats* target_;
    FactorStats snapshot_;
};

class FreshOOCExceptionCleanup final {
public:
    explicit FreshOOCExceptionCleanup(relation::RelationCollector& collector,
                                      const bool& preserve_for_resume) noexcept
        : collector_(&collector), preserve_for_resume_(&preserve_for_resume),
          uncaught_at_entry_(std::uncaught_exceptions()) {}

    FreshOOCExceptionCleanup(const FreshOOCExceptionCleanup&) = delete;
    FreshOOCExceptionCleanup& operator=(const FreshOOCExceptionCleanup&) = delete;

    ~FreshOOCExceptionCleanup() {
        if (collector_ != nullptr && preserve_for_resume_ != nullptr && !*preserve_for_resume_ &&
            std::uncaught_exceptions() > uncaught_at_entry_ &&
            !collector_->discard_uncommitted_fresh_ooc_noexcept()) {
            std::fprintf(stderr,
                         "[ooc] preserving an uncommitted fresh store after exception cleanup "
                         "could not prove and remove both owned artifacts\n");
        }
    }

private:
    relation::RelationCollector* collector_;
    const bool* preserve_for_resume_;
    int uncaught_at_entry_;
};

void require_pipeline_context(const Integer& pipeline_n, const PolynomialContext& ctx,
                              const char* phase) {
    if (ctx.n().compare(pipeline_n) != 0) {
        throw std::invalid_argument("Pipeline::" + std::string(phase) +
                                    " received a polynomial context for a different N");
    }
}

} // namespace

void Pipeline::refresh_relation_corpus_checked(relation::RelationCorpus& corpus,
                                               const relation::CorpusDigest& expected,
                                               const char* mismatch_message) {
    const auto scope = corpus.ooc_artifact_scope();
    if (!scope.has_value()) {
        throw std::invalid_argument(
            "Pipeline: fresh relation-corpus validation requires finalized OOC storage");
    }

    // Validate a temporary descriptor-bound reader before replacing the
    // corpus's current mmap. A failed digest check therefore preserves the
    // caller's move-only owner and identity token. On success, the private
    // structural handoff makes this same reader the next phase's source.
    auto candidate = relation::RelationCorpus::from_finalized_ooc(
        corpus.logical_generation(), scope->base_path, scope->descriptor,
        relation::OOCCleanupPolicy::Preserve);
    if (relation::corpus_digest(candidate) != expected) {
        throw std::runtime_error(mismatch_message);
    }
    corpus.replace_ooc_reader_from(std::move(candidate));
}

struct Pipeline::MatrixResult::StructuredRelations final {
    StructuredRelations(relation::RelationCorpus source_corpus,
                        std::vector<size_t> source_row_to_relation) noexcept
        : corpus(std::move(source_corpus)), row_to_relation(std::move(source_row_to_relation)) {}

    relation::RelationCorpus corpus;
    std::vector<size_t> row_to_relation;
};

Pipeline::MatrixResult::MatrixResult() = default;
Pipeline::MatrixResult::MatrixResult(MatrixResult&&) noexcept = default;
Pipeline::MatrixResult& Pipeline::MatrixResult::operator=(MatrixResult&& other) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<decltype(matrix)>);
    static_assert(std::is_nothrow_move_assignable_v<decltype(dependencies)>);
    static_assert(std::is_nothrow_move_assignable_v<decltype(relations)>);
    static_assert(std::is_nothrow_move_assignable_v<decltype(structured_relations_)>);

    if (this == &other) {
        return *this;
    }

    // Keep the target's old structured corpus alive while replacing every
    // public payload that may still refer to it. Replacing the owner last also
    // makes structured-to-legacy assignment release the old corpus safely.
    matrix = std::move(other.matrix);
    dependencies = std::move(other.dependencies);
    relations = std::move(other.relations);
    structured_relations_ = std::move(other.structured_relations_);
    return *this;
}
Pipeline::MatrixResult::~MatrixResult() = default;

size_t Pipeline::MatrixResult::relation_count() const {
    return structured_relations_ != nullptr ? structured_relations_->row_to_relation.size()
                                            : relations.size();
}

bool Pipeline::MatrixResult::owns_relation_corpus() const noexcept {
    return structured_relations_ != nullptr;
}

std::span<const size_t> Pipeline::MatrixResult::structured_row_to_relation() const noexcept {
    if (structured_relations_ == nullptr) {
        return {};
    }
    return structured_relations_->row_to_relation;
}

void Pipeline::MatrixResult::retain_structured_relations(relation::RelationCorpus&& corpus,
                                                         std::vector<size_t> row_to_relation) {
    if (structured_relations_ != nullptr || !relations.empty()) {
        throw std::logic_error(
            "MatrixResult: structured corpus and legacy relations are mutually exclusive");
    }
    if (!corpus.valid()) {
        throw std::invalid_argument("MatrixResult: cannot retain a moved-from relation corpus");
    }
    if (row_to_relation.size() != matrix.num_rows()) {
        throw std::invalid_argument(
            "MatrixResult: structured row mapping does not match matrix row count");
    }
    const size_t corpus_count = corpus.count();
    for (size_t ordinal : row_to_relation) {
        if (ordinal >= corpus_count) {
            throw std::out_of_range(
                "MatrixResult: structured row mapping contains an invalid corpus ordinal");
        }
    }
    // A new-expression allocates before evaluating constructor arguments.
    // Allocation failure therefore leaves the caller's corpus owner intact;
    // the noexcept constructor performs only the final ownership transfer.
    structured_relations_.reset(
        new StructuredRelations(std::move(corpus), std::move(row_to_relation)));
}

const relation::RelationCorpus& Pipeline::MatrixResult::structured_corpus() const {
    if (structured_relations_ == nullptr) {
        throw std::logic_error("MatrixResult: no structured relation corpus");
    }
    if (!relations.empty() || structured_relations_->row_to_relation.size() != matrix.num_rows()) {
        throw std::logic_error("MatrixResult: structured relation ownership invariant violated");
    }
    return structured_relations_->corpus;
}

std::vector<std::vector<bool>> detail::expand_solver_dependencies_checked(
    const linalg::SGEResult& sge_result, const std::vector<std::vector<bool>>& reduced_dependencies,
    size_t expected_matrix_rows) {
    for (const auto& reduced_dependency : reduced_dependencies) {
        if (reduced_dependency.size() != sge_result.reduced_matrix.num_rows()) {
            throw std::invalid_argument(
                "Pipeline::solve_matrix: solver dependency length does not match reduced matrix");
        }
    }
    if (sge_result.original_rows != expected_matrix_rows) {
        throw std::runtime_error(
            "Pipeline::solve_matrix: expanded dependency length does not match full matrix");
    }

    auto expanded = sge_result.expand_dependencies(reduced_dependencies);
    for (const auto& dependency : expanded) {
        if (dependency.size() != expected_matrix_rows) {
            throw std::runtime_error(
                "Pipeline::solve_matrix: expanded dependency length does not match full matrix");
        }
    }
    return expanded;
}

std::optional<std::vector<bool>> detail::xor_dependency_pair_checked(const std::vector<bool>& lhs,
                                                                     const std::vector<bool>& rhs,
                                                                     size_t expected_matrix_rows) {
    if (lhs.size() != expected_matrix_rows || rhs.size() != expected_matrix_rows) {
        throw std::invalid_argument(
            "Pipeline::extract_factors: XOR dependency length does not match matrix rows");
    }

    std::vector<bool> combined(expected_matrix_rows, false);
    size_t combined_weight = 0;
    for (size_t row = 0; row < expected_matrix_rows; ++row) {
        combined[row] = lhs[row] != rhs[row];
        combined_weight += combined[row] ? 1U : 0U;
    }
    if (combined_weight < 2) {
        return std::nullopt;
    }
    return combined;
}

detail::StructuredOOCGenerationPaths
detail::StructuredOOCRunPaths::generation_paths(uint64_t logical_generation) const {
    if (logical_generation == 0) {
        throw std::invalid_argument(
            "structured OOC generation paths require a nonzero logical generation");
    }
    if (run_namespace.empty() || run_namespace.find('\0') != std::string::npos ||
        !std::filesystem::path(run_namespace).is_absolute()) {
        throw std::logic_error("structured OOC run namespace is not a frozen absolute path");
    }

    const std::string generation_namespace =
        run_namespace + ".g" + std::to_string(logical_generation);
    return {
        generation_namespace + ".output",
    };
}

detail::StructuredOOCRunPaths
detail::make_structured_ooc_run_paths(std::optional<std::string> configured_raw_base_path,
                                      std::string run_identity) {
    if (run_identity.empty() || run_identity.find('\0') != std::string::npos) {
        throw std::invalid_argument("structured OOC run identity must be nonempty");
    }
    for (const char byte : run_identity) {
        const bool path_safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                               (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
        if (!path_safe) {
            throw std::invalid_argument(
                "structured OOC run identity must contain only ASCII letters, digits, '-' or '_'");
        }
    }

    std::string raw_base_path;
    if (configured_raw_base_path.has_value()) {
        if (configured_raw_base_path->empty() ||
            configured_raw_base_path->find('\0') != std::string::npos) {
            throw std::invalid_argument("structured OOC raw base path must be nonempty");
        }
        raw_base_path = std::move(*configured_raw_base_path);
    } else {
        raw_base_path = gnfs::util::temp_path("gnfs_relations_" + run_identity);
    }
    raw_base_path = relation::relation_corpus_detail::freeze_ooc_path(raw_base_path);
    std::string run_namespace = raw_base_path + ".gnfs-structured-run-" + run_identity;

    return {
        raw_base_path,
        std::move(run_identity),
        std::move(run_namespace),
    };
}

// ============================================================
// Fast path: trial division + Pollard rho for small N
// ============================================================

namespace {

/// Return floor(value * tenths / 10), saturating instead of overflowing.
/// The pipeline uses this for stable 1.3x and 1.1x matrix-row policies.
size_t scale_by_tenths_floor(size_t value, size_t tenths) noexcept {
    constexpr size_t denominator = 10;
    const size_t max = std::numeric_limits<size_t>::max();
    if (tenths == 0) {
        return 0;
    }
    if (tenths > max / (denominator - 1)) {
        return max;
    }

    const size_t whole = value / denominator;
    const size_t fractional = ((value % denominator) * tenths) / denominator;
    if (whole > (max - fractional) / tenths) {
        return max;
    }
    return whole * tenths + fractional;
}

[[nodiscard]] uint64_t elapsed_microseconds(std::chrono::steady_clock::time_point start,
                                            std::chrono::steady_clock::time_point finish) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
    return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
}

[[nodiscard]] uint64_t checked_add_u64(uint64_t lhs, uint64_t rhs) {
    if (lhs > std::numeric_limits<uint64_t>::max() - rhs) {
        throw std::overflow_error("telemetry duration exceeds uint64_t");
    }
    return lhs + rhs;
}

[[nodiscard]] int64_t signed_size_delta(size_t lhs, size_t rhs) {
    const std::uintmax_t positive_limit =
        static_cast<std::uintmax_t>(std::numeric_limits<int64_t>::max());
    if (lhs >= rhs) {
        const std::uintmax_t delta = static_cast<std::uintmax_t>(lhs - rhs);
        if (delta > positive_limit) {
            throw std::overflow_error("matrix row-column delta exceeds int64_t");
        }
        return static_cast<int64_t>(delta);
    }

    const std::uintmax_t delta = static_cast<std::uintmax_t>(rhs - lhs);
    const std::uintmax_t negative_limit = positive_limit + std::uintmax_t{1};
    if (delta > negative_limit) {
        throw std::overflow_error("matrix row-column delta exceeds int64_t");
    }
    if (delta == negative_limit) {
        return std::numeric_limits<int64_t>::min();
    }
    return -static_cast<int64_t>(delta);
}

struct StructuredFilterRuntimeTelemetry final {
    std::string_view route;
    uint64_t reduction_engine_wall_us = 0;
    util::ProcessMemorySnapshot before;
    util::ProcessMemorySnapshot after;
};

[[nodiscard]] StructuredFilterRuntimeTelemetry
finish_structured_filter_telemetry(std::string_view route,
                                   std::chrono::steady_clock::time_point start,
                                   util::ProcessMemorySnapshot before) noexcept {
    const auto finish = std::chrono::steady_clock::now();
    return {route, elapsed_microseconds(start, finish), std::move(before),
            util::process_memory_snapshot()};
}

[[nodiscard]] uint64_t optional_bytes(const std::optional<uint64_t>& value) noexcept {
    return value.value_or(0);
}

[[nodiscard]] uint64_t peak_growth_bytes(const StructuredFilterRuntimeTelemetry& telemetry) {
    if (!telemetry.before.lifetime_peak_rss_bytes || !telemetry.after.lifetime_peak_rss_bytes) {
        return 0;
    }
    if (*telemetry.after.lifetime_peak_rss_bytes < *telemetry.before.lifetime_peak_rss_bytes) {
        throw std::logic_error("process lifetime peak RSS decreased during reduction");
    }
    return *telemetry.after.lifetime_peak_rss_bytes - *telemetry.before.lifetime_peak_rss_bytes;
}

// GNFS_CASCADE_V3 modes:
//   unset / "0" / ""     → OFF (V0 only)
//   "1" / "on" / "true"  → ON (V3 every round, original behavior)
//   "auto" / "adaptive"  → AUTO (V3 only Round 2+; Round 1 too few LP overlaps for V3 ROI)
enum class V3Mode { Off, On, Auto };

inline V3Mode cascade_v3_mode() {
    const char* v = std::getenv("GNFS_CASCADE_V3");
    if (v == nullptr || v[0] == '\0' || v[0] == '0')
        return V3Mode::Off;
    if (v[0] == 'a' || v[0] == 'A')
        return V3Mode::Auto; // "auto" / "adaptive"
    return V3Mode::On;
}

inline bool cascade_v3_enabled_for_round(V3Mode mode, int round_index) {
    const V3Mode m = mode;
    if (m == V3Mode::Off)
        return false;
    if (m == V3Mode::On)
        return true;
    // Auto: Round 2+ only (round_index >= 1)
    return round_index >= 1;
}

inline bool cascade_v3_enabled() {
    return cascade_v3_mode() != V3Mode::Off;
}

std::string_view structured_stop_reason_name(relation::StructuredReductionStopReason reason) {
    switch (reason) {
    case relation::StructuredReductionStopReason::NotStarted:
        return "not_started";
    case relation::StructuredReductionStopReason::NoCandidates:
        return "no_candidates";
    case relation::StructuredReductionStopReason::BudgetLimit:
        return "budget_limit";
    case relation::StructuredReductionStopReason::PersistenceLimit:
        return "persistence_limit";
    }
    return "unknown";
}

std::string structured_filter_record(
    const relation::StructuredFilterPolicyDecision& policy,
    const relation::RelationReductionResult& reduction,
    const relation::RelationReductionConfig::StructuredExecutionConfig& execution,
    const StructuredFilterRuntimeTelemetry& telemetry) {
    const auto& stats = reduction.stats;
    const auto& run = stats.structured_run;
    const auto& budget = execution.budget;
    const bool peak_supported = telemetry.before.lifetime_peak_rss_bytes.has_value() &&
                                telemetry.after.lifetime_peak_rss_bytes.has_value();
    const bool current_supported = telemetry.after.current_rss_bytes.has_value();
    const std::string_view source_backend = telemetry.route == "direct_ooc_prefix"
                                                ? "collector_borrowed_ooc_prefix"
                                                : "in_memory_snapshot";
    const std::string_view output_backend =
        reduction.storage_kind() == relation::RelationStorageKind::FinalizedOOC ? "finalized_ooc"
                                                                                : "in_memory";
    return "structured_filter schema=1 reason=\"" + std::string(policy.reason) +
           "\" generation=" + std::to_string(reduction.generation) +
           " route=" + std::string(telemetry.route) +
           " source_backend=" + std::string(source_backend) +
           " output_backend=" + std::string(output_backend) +
           " input_rows=" + std::to_string(stats.input_relations) +
           " raw_duplicates=" + std::to_string(stats.raw_duplicates_removed) +
           " raw_digest_low=" + std::to_string(stats.raw_input_digest.low) +
           " raw_digest_high=" + std::to_string(stats.raw_input_digest.high) +
           " input_lp_cols=" + std::to_string(stats.deduplicated_input_lp_histogram.unique_keys) +
           " input_lp_w1=" + std::to_string(stats.deduplicated_input_lp_histogram.weight_1) +
           " input_lp_w2=" + std::to_string(stats.deduplicated_input_lp_histogram.weight_2) +
           " input_lp_w3=" + std::to_string(stats.deduplicated_input_lp_histogram.weight_3) +
           " input_lp_w4plus=" +
           std::to_string(stats.deduplicated_input_lp_histogram.weight_4plus) +
           " output_rows=" + std::to_string(stats.output_relations) +
           " output_lp_cols=" + std::to_string(stats.output_lp_columns) +
           " commits=" + std::to_string(run.commits) +
           " emitted_rows=" + std::to_string(run.emitted_rows) +
           " lp_fill_growth=" + std::to_string(run.lp_fill_growth) +
           " planning_passes=" + std::to_string(stats.structured.planning_passes) +
           " candidates=" + std::to_string(stats.structured.candidate_plans_considered) +
           " candidate_cap=" + std::to_string(budget.max_candidate_examinations_per_pass) +
           " emitted_cap=" + std::to_string(budget.max_emitted_rows) +
           " commit_cap=" + std::to_string(budget.max_commits) +
           " fill_cap=" + std::to_string(budget.max_total_lp_fill_growth) + " payload_cap=" +
           std::to_string(budget.max_accepted_materialized_payload_entries_per_commit) +
           " source_cap=" + std::to_string(budget.max_source_atoms_per_output) +
           " factor_side_cap=" + std::to_string(budget.max_factor_entries_per_side) +
           " batch_cap=" + std::to_string(execution.parallel.max_batch_candidates) +
           " incidence_shard_cap=" + std::to_string(execution.incidence.max_rows_per_shard) +
           " workers=" + std::to_string(execution.parallel.worker_count) + " reject_pivot=" +
           std::to_string(stats.structured.budget_rejections.pivot_weight_limit) +
           " reject_source=" + std::to_string(stats.structured.budget_rejections.source_limit) +
           " reject_output_lp=" +
           std::to_string(stats.structured.budget_rejections.output_lp_limit) +
           " reject_fill=" + std::to_string(stats.structured.budget_rejections.fill_limit) +
           " reject_emitted=" +
           std::to_string(stats.structured.budget_rejections.emitted_row_limit) +
           " reject_materialization=" +
           std::to_string(stats.structured.budget_rejections.materialization_limit) +
           " digest_low=" + std::to_string(stats.output_digest.low) +
           " digest_high=" + std::to_string(stats.output_digest.high) +
           " stop=" + std::string(structured_stop_reason_name(run.stop_reason)) +
           " reduction_engine_wall_us=" + std::to_string(telemetry.reduction_engine_wall_us) +
           " process_rss_scope=self_lifetime process_rss_backend=" +
           std::string(util::process_memory_backend_name(telemetry.after.backend)) +
           " process_current_rss_supported=" + (current_supported ? "1" : "0") +
           " process_current_rss_bytes=" +
           std::to_string(optional_bytes(telemetry.after.current_rss_bytes)) +
           " process_peak_rss_supported=" + (peak_supported ? "1" : "0") +
           " process_peak_rss_before_bytes=" +
           std::to_string(optional_bytes(telemetry.before.lifetime_peak_rss_bytes)) +
           " process_peak_rss_bytes=" +
           std::to_string(optional_bytes(telemetry.after.lifetime_peak_rss_bytes)) +
           " process_peak_rss_growth_bytes=" + std::to_string(peak_growth_bytes(telemetry));
}

void append_stage_record_field(std::string& record, std::string_view name, std::string_view value) {
    record.push_back(' ');
    record.append(name);
    record.push_back('=');
    record.append(value);
}

void append_stage_record_field(std::string& record, std::string_view name, uint64_t value) {
    append_stage_record_field(record, name, std::to_string(value));
}

void append_stage_record_field(std::string& record, std::string_view name, const char* value) {
    append_stage_record_field(record, name, std::string_view(value));
}

void append_stage_record_field(std::string& record, std::string_view name, bool value) {
    append_stage_record_field(record, name, value ? std::string_view("1") : std::string_view("0"));
}

struct StructuredStageReadSchemaEntry final {
    relation::StructuredTelemetryReadPhase phase;
    std::string_view name;
};

struct StructuredStageCheckpointSchemaEntry final {
    relation::StructuredTelemetryCheckpoint checkpoint;
    std::string_view name;
};

constexpr std::array<StructuredStageReadSchemaEntry, 4> STRUCTURED_STAGE_SCHEMA_1_READ_PHASES{{
    {relation::StructuredTelemetryReadPhase::InitialScan, "initial_scan"},
    {relation::StructuredTelemetryReadPhase::IncidenceBuild, "incidence_build"},
    {relation::StructuredTelemetryReadPhase::Reducer, "reducer"},
    {relation::StructuredTelemetryReadPhase::FreshValidation, "fresh_validation"},
}};

constexpr std::array<StructuredStageCheckpointSchemaEntry, 10>
    STRUCTURED_STAGE_SCHEMA_1_CHECKPOINTS{{
        {relation::StructuredTelemetryCheckpoint::ScanBegin, "scan_begin"},
        {relation::StructuredTelemetryCheckpoint::ScanCompleteBeforeAbRelease,
         "scan_complete_before_ab_release"},
        {relation::StructuredTelemetryCheckpoint::AfterAbRelease, "after_ab_release"},
        {relation::StructuredTelemetryCheckpoint::IncidenceReceiptBuilt, "incidence_receipt_built"},
        {relation::StructuredTelemetryCheckpoint::ReducerConstructed, "reducer_constructed"},
        {relation::StructuredTelemetryCheckpoint::ReductionComplete, "reduction_complete"},
        {relation::StructuredTelemetryCheckpoint::OutputMaterialized, "output_materialized"},
        {relation::StructuredTelemetryCheckpoint::OutputFinalized, "output_finalized"},
        {relation::StructuredTelemetryCheckpoint::ReducerReleased, "reducer_released"},
        {relation::StructuredTelemetryCheckpoint::FreshValidationComplete,
         "fresh_validation_complete"},
    }};

static_assert(relation::StructuredReductionTelemetryRecord::current_schema_version == 1);
static_assert(STRUCTURED_STAGE_SCHEMA_1_READ_PHASES.size() ==
              relation::structured_telemetry_read_phase_count);
static_assert(STRUCTURED_STAGE_SCHEMA_1_CHECKPOINTS.size() ==
              relation::structured_telemetry_checkpoint_count);

std::string
structured_filter_stage_record(const relation::StructuredReductionTelemetryRecord& telemetry) {
    if (telemetry.schema_version !=
        relation::StructuredReductionTelemetryRecord::current_schema_version) {
        throw std::logic_error("unsupported structured filter stage telemetry schema");
    }

    std::string record = "structured_filter_stage schema=1";
    record.reserve(8192);
    append_stage_record_field(record, "generation", telemetry.generation);
    append_stage_record_field(record, "route", "direct_ooc_prefix");
    append_stage_record_field(record, "process_rss_scope", "self_lifetime");
    append_stage_record_field(record, "source_rows", telemetry.source_rows);
    append_stage_record_field(record, "incidence_rows", telemetry.incidence_rows);
    append_stage_record_field(record, "incidence_unique_keys", telemetry.incidence_unique_keys);
    append_stage_record_field(record, "incidence_entries", telemetry.incidence_entries);
    append_stage_record_field(record, "completed", telemetry.completed);
    append_stage_record_field(record, "succeeded", telemetry.succeeded);
    append_stage_record_field(
        record, "failure_stage",
        relation::structured_telemetry_failure_stage_name(telemetry.failure_stage));
    append_stage_record_field(
        record, "last_checkpoint",
        telemetry.last_checkpoint.has_value()
            ? relation::structured_telemetry_checkpoint_name(*telemetry.last_checkpoint)
            : std::string_view("none"));

    for (const auto& entry : STRUCTURED_STAGE_SCHEMA_1_READ_PHASES) {
        const std::string prefix = "read_" + std::string(entry.name);
        const auto& counters = telemetry.reads[static_cast<size_t>(entry.phase)];
        append_stage_record_field(record, prefix + "_attempts", counters.attempts);
        append_stage_record_field(record, prefix + "_successes", counters.successes);
        append_stage_record_field(record, prefix + "_failures", counters.failures);
    }

    for (const auto& entry : STRUCTURED_STAGE_SCHEMA_1_CHECKPOINTS) {
        const std::string prefix = "checkpoint_" + std::string(entry.name);
        const auto& sample = telemetry.checkpoints[static_cast<size_t>(entry.checkpoint)];
        const bool current_supported = sample.memory.current_rss_bytes.has_value();
        const bool peak_supported = sample.memory.lifetime_peak_rss_bytes.has_value();
        append_stage_record_field(record, prefix + "_observed", sample.observed);
        append_stage_record_field(record, prefix + "_wall_supported", sample.wall_supported);
        append_stage_record_field(record, prefix + "_elapsed_wall_ns",
                                  sample.wall_supported ? sample.elapsed_wall_ns : uint64_t{0});
        append_stage_record_field(record, prefix + "_memory_backend",
                                  util::process_memory_backend_name(sample.memory.backend));
        append_stage_record_field(record, prefix + "_current_rss_supported", current_supported);
        append_stage_record_field(record, prefix + "_current_rss_bytes",
                                  sample.memory.current_rss_bytes.value_or(0));
        append_stage_record_field(record, prefix + "_peak_rss_supported", peak_supported);
        append_stage_record_field(record, prefix + "_peak_rss_bytes",
                                  sample.memory.lifetime_peak_rss_bytes.value_or(0));
    }

    append_stage_record_field(record, "counter_overflow", telemetry.counter_overflow);
    append_stage_record_field(record, "clock_monotone", telemetry.clock_monotone);
    append_stage_record_field(record, "peak_monotone", telemetry.peak_monotone);
    append_stage_record_field(record, "clock_provider_failures", telemetry.clock_provider_failures);
    append_stage_record_field(record, "memory_provider_failures",
                              telemetry.memory_provider_failures);
    return record;
}

// Pipeline resume base path (Phase 1+2+3 checkpoints).
//
// Precedence:
//   1. GNFS_RESUME=<base>     — preferred name covering full pipeline
//   2. GNFS_SIEVE_RESUME=<base> — legacy alias (Phase 3 sieve-only)
//
// Returns empty string when neither ENV is set / both empty.
inline std::string pipeline_resume_base_path() {
    if (const char* env = std::getenv("GNFS_RESUME"); env != nullptr && env[0] != '\0') {
        return env;
    }
    if (const char* env = std::getenv("GNFS_SIEVE_RESUME"); env != nullptr && env[0] != '\0') {
        return env;
    }
    return {};
}

std::string allocate_structured_ooc_run_identity() {
    static std::atomic<uint64_t> next_run_ordinal{1};
    uint64_t run_ordinal = next_run_ordinal.load(std::memory_order_relaxed);
    for (;;) {
        if (run_ordinal == 0) {
            throw std::overflow_error("structured OOC run identity counter exhausted");
        }
        const uint64_t successor =
            run_ordinal == std::numeric_limits<uint64_t>::max() ? 0 : run_ordinal + 1;
        if (next_run_ordinal.compare_exchange_weak(run_ordinal, successor,
                                                   std::memory_order_relaxed)) {
            break;
        }
    }
    return "p" + std::to_string(gnfs::util::process_id()) + "-r" + std::to_string(run_ordinal);
}

/// Trial division up to limit. Returns factor or 0.
uint64_t trial_divide(const Integer& n, uint64_t limit) {
    // Small primes
    if (mpz_divisible_ui_p(n.get_mpz(), 2))
        return 2;
    if (mpz_divisible_ui_p(n.get_mpz(), 3))
        return 3;
    // 6k±1 wheel
    for (uint64_t i = 5; i <= limit; i += 6) {
        if (mpz_divisible_ui_p(n.get_mpz(), i))
            return i;
        if (mpz_divisible_ui_p(n.get_mpz(), i + 2))
            return i + 2;
    }
    return 0;
}

// ── Fast 2-limb Pollard rho using GMP mpn_ (N ≤ 2^128) ──
// Uses GMP's optimized assembly for 2-limb arithmetic, bypassing mpz_t overhead.
// ~5-8× faster than mpz-based rho for 65-128 bit numbers.

/// 2-limb Pollard rho. Returns factor as uint64, or 0 if not found.
uint64_t pollard_rho_mpn2(const Integer& n, size_t max_iters) {
    size_t n_size = mpz_size(n.get_mpz());
    if (n_size > 2 || n_size == 0)
        return 0;

    mp_limb_t N[2] = {mpz_getlimbn(n.get_mpz(), 0), n_size > 1 ? mpz_getlimbn(n.get_mpz(), 1) : 0};

    // n_actual_size: 1 or 2 limbs
    mp_size_t nn = (N[1] != 0) ? 2 : 1;

    // Modular square: r = a^2 mod N, where a is nn limbs
    // product = a^2 (2*nn limbs), then tdiv_qr to get remainder
    mp_limb_t prod[4], quot[3]; // max sizes for 2-limb operations
    auto sqrmod = [&](mp_limb_t* r, const mp_limb_t* a) {
        mpn_sqr(prod, a, nn);
        mpn_tdiv_qr(quot, r, 0, prod, 2 * nn, N, nn);
    };

    // Modular multiply: r = a * b mod N
    auto mulmod = [&](mp_limb_t* r, const mp_limb_t* a, const mp_limb_t* b) {
        mpn_mul_n(prod, a, b, nn);
        mpn_tdiv_qr(quot, r, 0, prod, 2 * nn, N, nn);
    };

    // Add mod: r = (a + b) mod N
    auto addmod = [&](mp_limb_t* r, const mp_limb_t* a, const mp_limb_t* b) {
        mp_limb_t carry = mpn_add_n(r, a, b, nn);
        if (carry || mpn_cmp(r, N, nn) >= 0) {
            mpn_sub_n(r, r, N, nn);
        }
    };

    // Sub absolute: r = |a - b|
    auto sub_abs = [&](mp_limb_t* r, const mp_limb_t* a, const mp_limb_t* b) {
        if (mpn_cmp(a, b, nn) >= 0)
            mpn_sub_n(r, a, b, nn);
        else
            mpn_sub_n(r, b, a, nn);
    };

    // GCD with N: compute gcd(a, N), return as uint64 if small
    mpz_t g_mpz, a_mpz, n_mpz;
    mpz_init(g_mpz);
    mpz_init(a_mpz);
    mpz_init(n_mpz);
    mpz_import(n_mpz, static_cast<size_t>(nn), -1, sizeof(mp_limb_t), 0, 0, N);
    auto gcd_with_n = [&](const mp_limb_t* a) -> uint64_t {
        mpz_import(a_mpz, static_cast<size_t>(nn), -1, sizeof(mp_limb_t), 0, 0, a);
        mpz_gcd(g_mpz, a_mpz, n_mpz);
        if (mpz_cmp_ui(g_mpz, 1) > 0 && mpz_cmp(g_mpz, n_mpz) < 0) {
            return mpz_get_ui(g_mpz);
        }
        return (mpz_cmp_ui(g_mpz, 1) > 0) ? 1 : 0; // 1 = factor but doesn't fit uint64
    };

    // (is_one not needed — GCD check handles all cases)

    // RNG
    uint64_t seed = 42;
    auto rng_next = [](uint64_t& s) -> uint64_t {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    };

    mp_limb_t y[2], c[2], x[2], ys[2], q_acc[2], diff[2];
    size_t total_iters = 0; // Track total iterations across all attempts

    for (int attempt = 0; attempt < 20 && total_iters < max_iters; attempt++) {
        y[0] = rng_next(seed);
        y[1] = 0;
        c[0] = rng_next(seed);
        c[1] = 0;
        if (c[0] == 0)
            c[0] = 1;
        // Reduce y, c mod N
        if (nn == 2) {
            mp_limb_t tmpq[2];
            mpn_tdiv_qr(tmpq, y, 0, y, nn, N, nn);
            mpn_tdiv_qr(tmpq, c, 0, c, nn, N, nn);
        } else {
            y[0] %= N[0];
            c[0] %= N[0];
        }

        q_acc[0] = 1;
        q_acc[1] = 0;
        size_t r_val = 1;
        bool found = false;

        while (!found && total_iters < max_iters) {
            x[0] = y[0];
            x[1] = y[1];

            // Phase 1: advance y by r steps
            for (size_t i = 0; i < r_val; i++) {
                sqrmod(y, y);
                addmod(y, y, c);
            }

            // Phase 2: accumulate product in batches
            size_t k = 0;
            while (k < r_val && !found) {
                ys[0] = y[0];
                ys[1] = y[1];
                size_t batch = std::min(size_t(128), r_val - k);
                for (size_t i = 0; i < batch; i++) {
                    sqrmod(y, y);
                    addmod(y, y, c);
                    sub_abs(diff, x, y);
                    if (diff[0] == 0 && (nn < 2 || diff[1] == 0))
                        continue;
                    mulmod(q_acc, q_acc, diff);
                }
                // Check GCD
                uint64_t g = gcd_with_n(q_acc);
                if (g > 1) {
                    // Backtrack to find exact factor
                    for (size_t bt = 0; bt < 256; bt++) {
                        sqrmod(ys, ys);
                        addmod(ys, ys, c);
                        sub_abs(diff, x, ys);
                        g = gcd_with_n(diff);
                        if (g > 1) {
                            mpz_clear(g_mpz);
                            mpz_clear(a_mpz);
                            mpz_clear(n_mpz);
                            return g;
                        }
                    }
                    // g == n case: reset and retry
                    q_acc[0] = 1;
                    q_acc[1] = 0;
                    break;
                }
                k += batch;
                total_iters += batch;
            }
            r_val *= 2;
        }
    }

    mpz_clear(g_mpz);
    mpz_clear(a_mpz);
    mpz_clear(n_mpz);
    return 0;
}

/// Pollard rho with Brent improvement. Works on GMP integers.
/// Returns a non-trivial factor or Integer(0) if not found within max_iters.
Integer pollard_rho_brent(const Integer& n, size_t max_iters = 1000000) {
    if (mpz_cmp_si(n.get_mpz(), 3) <= 0)
        return Integer{};

    // Use GMP directly for speed
    mpz_t y, c, m, g, r, q, x, ys, tmp;
    mpz_init(y);
    mpz_init(c);
    mpz_init(m);
    mpz_init(g);
    mpz_init(r);
    mpz_init(q);
    mpz_init(x);
    mpz_init(ys);
    mpz_init(tmp);

    gmp_randstate_t state;
    gmp_randinit_mt(state);
    gmp_randseed_ui(state, 42);

    const mpz_t& n_mpz = *reinterpret_cast<const mpz_t*>(&n.get_mpz());
    Integer result;

    for (int attempt = 0; attempt < 20 && result.is_zero(); ++attempt) {
        mpz_urandomm(y, state, n_mpz);
        mpz_urandomm(c, state, n_mpz);
        if (mpz_sgn(c) == 0)
            mpz_set_ui(c, 1);
        mpz_set_ui(m, 128);
        mpz_set_ui(g, 1);
        mpz_set_ui(q, 1);
        mpz_set_ui(r, 1);

        size_t iters = 0;

        while (mpz_cmp_ui(g, 1) == 0 && iters < max_iters) {
            mpz_set(x, y);
            unsigned long r_val = mpz_get_ui(r);
            for (unsigned long i = 0; i < r_val; ++i) {
                // y = (y*y + c) mod n
                mpz_mul(tmp, y, y);
                mpz_add(tmp, tmp, c);
                mpz_mod(y, tmp, n_mpz);
            }

            size_t k = 0;
            while (k < r_val && mpz_cmp_ui(g, 1) == 0) {
                mpz_set(ys, y);
                unsigned long m_val = mpz_get_ui(m);
                unsigned long batch = std::min(m_val, r_val - static_cast<unsigned long>(k));
                for (unsigned long i = 0; i < batch; ++i) {
                    // y = (y*y + c) mod n
                    mpz_mul(tmp, y, y);
                    mpz_add(tmp, tmp, c);
                    mpz_mod(y, tmp, n_mpz);
                    // q = q * |x - y| mod n
                    mpz_sub(tmp, x, y);
                    mpz_abs(tmp, tmp);
                    mpz_mul(tmp, q, tmp);
                    mpz_mod(q, tmp, n_mpz);
                }
                mpz_gcd(g, q, n_mpz);
                k += batch;
                iters += batch;
            }

            mpz_mul_ui(r, r, 2);
        }

        if (mpz_cmp(g, n_mpz) == 0) {
            // Backtrack: replay individual steps to isolate factor.
            // Bounded to 256 iterations (> max batch of 128) as safety guard.
            for (size_t bt = 0; bt < 256; ++bt) {
                mpz_mul(tmp, ys, ys);
                mpz_add(tmp, tmp, c);
                mpz_mod(ys, tmp, n_mpz);
                mpz_sub(tmp, x, ys);
                mpz_abs(tmp, tmp);
                mpz_gcd(g, tmp, n_mpz);
                if (mpz_cmp_ui(g, 1) > 0)
                    break;
            }
        }

        if (mpz_cmp_ui(g, 1) > 0 && mpz_cmp(g, n_mpz) < 0) {
            mpz_set(result.get_mpz(), g);
        }
    }

    mpz_clear(y);
    mpz_clear(c);
    mpz_clear(m);
    mpz_clear(g);
    mpz_clear(r);
    mpz_clear(q);
    mpz_clear(x);
    mpz_clear(ys);
    mpz_clear(tmp);
    gmp_randclear(state);

    return result;
}

} // anonymous namespace

// ============================================================
// Method Selection
// ============================================================

std::pair<FactorizationMethod, std::string>
Pipeline::select_method(size_t n_bits, size_t n_digits,
                        std::optional<FactorizationMethod> override) {
    // Manual override
    if (override && *override != FactorizationMethod::Auto) {
        return {*override, "user specified"};
    }

    // Auto selection cascade:
    //
    // Trial division: always first (catches factors ≤ 10^6)
    // Pollard rho: ≤30 digits (≤100 bits) — O(p^{1/2}), fast for balanced ≤30d
    // ECM+SIQS: 25-100 digits — ECM tried first (O(exp(√(2·ln p·ln ln p)))),
    //           SIQS fallback (O(L_N(1/2,1)))
    // GNFS: 101+ digits — O(L_N(1/3,c)), with SIQS probe ≤100d
    //
    // Key insight: ECM depends on smallest factor p, not N.
    // For balanced k-digit semiprimes, p ≈ k/2 digits.
    // ECM beats SIQS up to ~55d (where factors are ~27d).

    // ENV overrides (debugging/experimentation only)
    //   GNFS_FORCE_SIQS=1   → force SIQS path regardless of size (except trial-only ≤6d)
    //   GNFS_DISABLE_SIQS=1 → skip SIQS, fall through to GNFS for ≥25d
    // Both ENVs ignored when user explicitly set Config::method (handled above).
    const char* env_force = std::getenv("GNFS_FORCE_SIQS");
    const char* env_disable = std::getenv("GNFS_DISABLE_SIQS");
    bool force_siqs = (env_force && env_force[0] == '1');
    bool disable_siqs = (env_disable && env_disable[0] == '1');

    if (n_digits <= 6 || n_bits <= 20) {
        return {FactorizationMethod::TrialDivision, std::to_string(n_digits) + "d/" +
                                                        std::to_string(n_bits) +
                                                        "bit: trial division sufficient"};
    }

    if (force_siqs) {
        return {FactorizationMethod::SIQS,
                std::to_string(n_digits) + "d: GNFS_FORCE_SIQS=1 override"};
    }

    if (n_digits <= 24 || n_bits <= 80) {
        return {FactorizationMethod::PollardRho, std::to_string(n_digits) + "d/" +
                                                     std::to_string(n_bits) +
                                                     "bit: Pollard rho O(p^{1/2}) efficient"};
    }

    if (n_digits <= 100 && !disable_siqs) {
        // 25-100d: rho quick probe → ECM → SIQS cascade
        return {FactorizationMethod::SIQS, std::to_string(n_digits) + "d: rho+ECM+SIQS cascade"};
    }

    return {FactorizationMethod::GNFS,
            std::to_string(n_digits) + "d: GNFS O(L_N(1/3,c))" +
                (disable_siqs ? " (SIQS disabled via ENV)" : " required")};
}

// ============================================================
// Construction
// ============================================================

// Keep an owned Integer copy because callers may release their input after construction.
Pipeline::Pipeline(const Integer& n, const Config& config)
    : n_(n), config_(config), params_(config.apply_to(n)),
      start_time_(std::chrono::high_resolution_clock::now()) {
    uint32_t hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) {
        hardware_threads = 4;
    }
    if (params_.max_local_sieve_threads == 0) {
        params_.max_local_sieve_threads = hardware_threads;
    } else {
        params_.max_local_sieve_threads =
            std::min(params_.max_local_sieve_threads, hardware_threads);
    }
    stats_.n_bits = n.bit_length();
    stats_.n_digits = params_.digits;
    stats_.degree = params_.degree;
    stats_.rational_bound = params_.rational_bound;
    stats_.algebraic_bound = params_.algebraic_bound;
    stats_.large_prime_bound = params_.large_prime_bound;
}

Pipeline::StructuredRouteSnapshot Pipeline::capture_structured_route_snapshot() const {
    const bool stage_telemetry_enabled = detail::parse_structured_filter_stage_telemetry(
        std::getenv("GNFS_STRUCTURED_FILTER_STAGE_TELEMETRY"));
    const auto mode = relation::parse_structured_filter_mode(std::getenv("GNFS_STRUCTURED_FILTER"));
    std::string resume_base_path = pipeline_resume_base_path();
    if (!resume_base_path.empty()) {
        resume_base_path = relation::relation_corpus_detail::freeze_ooc_path(resume_base_path);
    }
    const bool resume_enabled = !resume_base_path.empty();
    const auto ooc_policy =
        relation::decide_ooc_policy(std::getenv("GNFS_OOC_RELATIONS"), params_.large_prime_bound);
    const bool ooc_enabled = resume_enabled || ooc_policy.enabled;
    std::optional<std::string> configured_ooc_base_path;
    if (const char* path_env = std::getenv("GNFS_OOC_BASE_PATH");
        path_env != nullptr && path_env[0] != '\0') {
        configured_ooc_base_path.emplace(path_env);
    }
    const bool large_primes_enabled = params_.large_prime_bound > params_.algebraic_bound;
    auto distributed_config = sieve::parse_distributed_sieve_env();
    const size_t distributed_workers = distributed_config.num_workers;
    if (distributed_workers > 0) {
        distributed_config.base_path =
            relation::relation_corpus_detail::freeze_ooc_path(distributed_config.base_path);
    }
    const bool distributed_size_gate_ok = params_.digits >= 30;
    const char* distributed_force_env = std::getenv("GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL");
    const bool distributed_force_small =
        distributed_force_env != nullptr && distributed_force_env[0] == '1';
    const bool distributed_route_selected = distributed_workers > 0 && !resume_enabled &&
                                            (distributed_size_gate_ok || distributed_force_small);
    const bool supported = relation::structured_filter_route_supported({
        .large_primes_enabled = large_primes_enabled,
        .ooc_enabled = ooc_enabled,
        .ooc_explicitly_enabled = ooc_policy.explicitly_enabled,
        .resume_enabled = resume_enabled,
        .distributed_route = distributed_route_selected,
    });
    const auto policy = relation::decide_structured_filter_policy(mode, supported, false);

    std::optional<detail::StructuredOOCRunPaths> ooc_paths;
    if (ooc_enabled) {
        if (resume_enabled) {
            configured_ooc_base_path = resume_base_path;
        }
        ooc_paths.emplace(detail::make_structured_ooc_run_paths(
            std::move(configured_ooc_base_path), allocate_structured_ooc_run_identity()));
    }

    return {
        policy,
        std::move(resume_base_path),
        std::move(ooc_paths),
        std::string(ooc_policy.reason),
        std::move(distributed_config.base_path),
        large_primes_enabled,
        ooc_enabled,
        distributed_workers,
        distributed_config.sq_per_worker,
        distributed_size_gate_ok,
        distributed_force_small,
        distributed_route_selected,
        distributed_config.worker_timeout_ms,
        stage_telemetry_enabled,
    };
}

// ============================================================
// Progress / Log helpers
// ============================================================

double Pipeline::elapsed_s() const {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double>(now - start_time_).count();
}

uint64_t Pipeline::allocate_relation_generation() {
    if (next_relation_generation_ == 0) {
        throw std::overflow_error("relation generation counter exhausted");
    }
    return next_relation_generation_++;
}

void Pipeline::emit_progress(Phase phase, const std::string& msg, double phase_progress) {
    if (!progress_cb_)
        return;
    ProgressInfo info;
    info.phase = phase;
    info.phase_progress = phase_progress;
    info.elapsed_s = elapsed_s();
    info.message = msg;
    info.relations_found = stats_.relations_found;
    info.relations_target = relations_target_;
    info.special_q_done = stats_.special_q_processed;
    info.matrix_rows = stats_.matrix_rows;
    info.matrix_cols = stats_.matrix_cols;
    info.dependency_index = stats_.dependencies_tried;
    info.dependencies_total = stats_.dependencies_found;
    progress_cb_(info);
}

void Pipeline::emit_log(LogLevel level, Phase phase, const std::string& msg) {
    if (!log_cb_)
        return;
    LogEntry entry;
    entry.level = level;
    entry.phase = phase;
    entry.timestamp_s = elapsed_s();
    entry.message = msg;
    log_cb_(entry);
}

// ============================================================
// Phase 1: Polynomial Selection
// ============================================================

PolynomialContext Pipeline::select_polynomial() {
    return select_polynomial_impl(pipeline_resume_base_path());
}

PolynomialContext Pipeline::select_polynomial_impl(const std::string& resume_base) {
    emit_progress(Phase::PolynomialSelection, "Starting polynomial selection");
    emit_log(LogLevel::Info, Phase::PolynomialSelection,
             "N=" + n_.to_string() + " bits=" + std::to_string(stats_.n_bits) +
                 " degree=" + std::to_string(params_.degree));

    auto t0 = std::chrono::high_resolution_clock::now();

    // ── Phase 1 checkpoint resume (GNFS_RESUME / GNFS_SIEVE_RESUME, 2026-05-21) ──
    // Result-only checkpoint: if <base>.poly_ckpt exists with matching N, load
    // and skip the (potentially hours-long) Kleinjung lattice search.  Selection
    // is multi-threaded random search, so in-flight checkpointing is not viable;
    // we only persist the final (f, g, m) and reuse it across restarts.
    if (!resume_base.empty()) {
        const std::string poly_ckpt = resume_base + ".poly_ckpt";
        if (polynomial::PolyCheckpoint::exists_and_valid(poly_ckpt)) {
            try {
                auto ck = polynomial::PolyCheckpoint::load_for(poly_ckpt, n_);
                auto ctx_resumed = ck.to_context();

                auto t1 = std::chrono::high_resolution_clock::now();
                stats_.timings.poly_s = std::chrono::duration<double>(t1 - t0).count();

                emit_log(LogLevel::Info, Phase::PolynomialSelection,
                         "checkpoint hit: m=" + ctx_resumed.m().to_string() + " degree=" +
                             std::to_string(ctx_resumed.degree()) + " (skipped Kleinjung search)");
                std::fprintf(stderr, "[poly-resume] ckpt=%s degree=%u skew=%g\n", poly_ckpt.c_str(),
                             ctx_resumed.degree(), ctx_resumed.skewness());
                emit_progress(Phase::PolynomialSelection, "Polynomial loaded from checkpoint", 1.0);
                return ctx_resumed;
            } catch (const std::exception& e) {
                emit_log(LogLevel::Warn, Phase::PolynomialSelection,
                         std::string("poly checkpoint load failed (") + e.what() +
                             ") — falling through to fresh selection");
            }
        }
    }

    bool verbose = config_.verbose.value_or(false);
    auto ctx = polynomial::SelectorDispatch::select(n_, params_.degree, verbose);

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.poly_s = std::chrono::duration<double>(t1 - t0).count();

    // Persist Phase 1 result so a follow-up run can skip it.
    if (!resume_base.empty()) {
        const std::string poly_ckpt = resume_base + ".poly_ckpt";
        try {
            auto ck = polynomial::PolyCheckpoint::from_context(ctx);
            ck.save(poly_ckpt);
            emit_log(LogLevel::Info, Phase::PolynomialSelection,
                     "poly checkpoint saved: " + poly_ckpt);
        } catch (const std::exception& e) {
            emit_log(LogLevel::Warn, Phase::PolynomialSelection,
                     std::string("poly checkpoint save failed: ") + e.what());
        }
    }

    emit_log(LogLevel::Info, Phase::PolynomialSelection,
             "m=" + ctx.m().to_string() + " time=" + std::to_string(stats_.timings.poly_s) + "s");
    emit_progress(Phase::PolynomialSelection, "Polynomial selected", 1.0);

    return ctx;
}

// ============================================================
// Phase 2: Factor Base Construction
// ============================================================

FactorBase Pipeline::build_factor_base(const PolynomialContext& ctx) {
    require_pipeline_context(n_, ctx, "build_factor_base");
    return build_factor_base_impl(ctx, pipeline_resume_base_path());
}

FactorBase Pipeline::build_factor_base_impl(const PolynomialContext& ctx,
                                            const std::string& resume_base) {
    emit_progress(Phase::FactorBase, "Building factor base");

    auto t0 = std::chrono::high_resolution_clock::now();

    factor_base::FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params_.rational_bound;
    fb_opts.algebraic_bound = params_.algebraic_bound;
    fb_opts.special_q_bound = params_.special_q_max;
    fb_opts.large_prime_bound = params_.large_prime_bound;
    fb_opts.parallel = true;

    // ── Phase 2 checkpoint resume (GNFS_RESUME / GNFS_SIEVE_RESUME, 2026-05-21) ──
    // Result-only checkpoint: if <base>.fb_ckpt exists and all build params +
    // ctx fingerprint match, rehydrate the FactorBase and skip the parallel
    // Cantor-Zassenhaus root-finding entirely.  Mismatch on any param forces a
    // fresh rebuild and overwrites the stale checkpoint.
    if (!resume_base.empty()) {
        const std::string fb_ckpt = resume_base + ".fb_ckpt";
        if (factor_base::FbCheckpoint::exists_and_valid(fb_ckpt)) {
            try {
                auto ck = factor_base::FbCheckpoint::load(fb_ckpt);
                auto status = ck.matches(ctx, fb_opts.rational_bound, fb_opts.algebraic_bound,
                                         fb_opts.special_q_bound, fb_opts.large_prime_bound,
                                         fb_opts.log_scale);
                if (status == factor_base::FbCheckpoint::MatchStatus::Ok) {
                    auto fb_resumed = ck.to_factor_base();

                    auto t1 = std::chrono::high_resolution_clock::now();
                    stats_.timings.fb_s = std::chrono::duration<double>(t1 - t0).count();
                    stats_.rational_primes = fb_resumed.rational_count();
                    stats_.algebraic_primes = fb_resumed.algebraic_count();

                    emit_log(
                        LogLevel::Info, Phase::FactorBase,
                        "checkpoint hit: rational=" + std::to_string(fb_resumed.rational_count()) +
                            " algebraic=" + std::to_string(fb_resumed.algebraic_count()) +
                            " (skipped Cantor-Zassenhaus)");
                    std::fprintf(stderr, "[fb-resume] ckpt=%s rat=%zu alg=%zu sieve_alg=%zu\n",
                                 fb_ckpt.c_str(), fb_resumed.rational_count(),
                                 fb_resumed.algebraic_count(), fb_resumed.sieve_algebraic_count());
                    emit_progress(Phase::FactorBase, "Factor base loaded from checkpoint", 1.0);
                    return fb_resumed;
                } else {
                    const char* reason =
                        (status == factor_base::FbCheckpoint::MatchStatus::NMismatch) ? "N mismatch"
                        : (status == factor_base::FbCheckpoint::MatchStatus::DegreeMismatch)
                            ? "degree mismatch"
                            : "params mismatch";
                    emit_log(LogLevel::Warn, Phase::FactorBase,
                             std::string("fb checkpoint stale (") + reason + ") — rebuilding");
                }
            } catch (const std::exception& e) {
                emit_log(LogLevel::Warn, Phase::FactorBase,
                         std::string("fb checkpoint load failed (") + e.what() + ") — rebuilding");
            }
        }
    }

    auto fb = factor_base::FactorBaseBuilder::build(ctx, fb_opts);

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.fb_s = std::chrono::duration<double>(t1 - t0).count();
    stats_.rational_primes = fb.rational_count();
    stats_.algebraic_primes = fb.algebraic_count();

    // Persist Phase 2 result for follow-up runs.
    if (!resume_base.empty()) {
        const std::string fb_ckpt = resume_base + ".fb_ckpt";
        try {
            auto ck = factor_base::FbCheckpoint::from_factor_base(fb, ctx, fb_opts.special_q_bound);
            ck.save(fb_ckpt);
            emit_log(LogLevel::Info, Phase::FactorBase, "fb checkpoint saved: " + fb_ckpt);
        } catch (const std::exception& e) {
            emit_log(LogLevel::Warn, Phase::FactorBase,
                     std::string("fb checkpoint save failed: ") + e.what());
        }
    }

    emit_log(LogLevel::Info, Phase::FactorBase,
             "rational=" + std::to_string(fb.rational_count()) +
                 " algebraic=" + std::to_string(fb.algebraic_count()) +
                 " (sieve=" + std::to_string(fb.sieve_algebraic_count()) + ")");
    emit_progress(Phase::FactorBase, "Factor base built", 1.0);

    return fb;
}

// ============================================================
// Phase 3: Sieving and Relation Collection
// ============================================================

relation::RelationReductionResult Pipeline::sieve_and_collect(const PolynomialContext& ctx,
                                                              const FactorBase& fb,
                                                              SieveCollectionOptions options) {
    require_pipeline_context(n_, ctx, "sieve_and_collect");
    if (options.adaptive_round_limit == 0 ||
        options.adaptive_round_limit > DEFAULT_ADAPTIVE_SIEVE_ROUND_LIMIT) {
        throw std::out_of_range("adaptive_round_limit must be in [1, 10]");
    }
    switch (options.legacy_raw_ooc_cleanup) {
    case LegacyRawOOCCleanupPolicy::RetainArtifacts:
    case LegacyRawOOCCleanupPolicy::RemoveArtifacts:
        break;
    default:
        throw std::invalid_argument("invalid legacy raw OOC cleanup policy");
    }
    const auto structured_route = capture_structured_route_snapshot();
    if (structured_route.distributed_route_selected &&
        (options.adaptive_round_limit != DEFAULT_ADAPTIVE_SIEVE_ROUND_LIMIT ||
         options.legacy_raw_ooc_cleanup != LegacyRawOOCCleanupPolicy::RetainArtifacts)) {
        throw std::invalid_argument(
            "bounded local sieve collection options are incompatible with distributed sieving");
    }
    return sieve_and_collect_impl(ctx, fb, structured_route, options);
}

relation::RelationReductionResult
Pipeline::sieve_and_collect_impl(const PolynomialContext& ctx, const FactorBase& fb,
                                 const StructuredRouteSnapshot& structured_preflight,
                                 SieveCollectionOptions options) {
    auto t0 = std::chrono::high_resolution_clock::now();
    FactorStatsRollback stats_rollback(stats_);
    const int adaptive_round_limit = static_cast<int>(options.adaptive_round_limit);
    stats_.sieve_rounds_completed = 0;
    stats_.sieve_stop_reason = SieveStopReason::NotStarted;
    stats_.special_q_batch_worker_limit = 0;
    stats_.special_q_batch_peak_workers = 0;
    stats_.special_q_batch_count = 0;
    stats_.special_q_batch_peak_size = 0;
    stats_.local_sieve_thread_budget = 0;
    stats_.special_q_batch_peak_assigned_threads = 0;
    stats_.special_q_worker_peak_sieve_threads = 0;
    stats_.candidate_batch_peak_workers = 0;
    stats_.candidate_batch_total_chunks = 0;
    stats_.candidate_batch_peak_chunks = 0;
    stats_.candidate_batch_peak_candidates = 0;
    stats_.candidate_batch_rss_sample_candidates = 0;
    stats_.candidate_batch_after_generation_current_rss_bytes.reset();
    stats_.candidate_batch_after_cofactor_current_rss_bytes.reset();
    stats_.candidate_batch_after_release_current_rss_bytes.reset();
    stats_.timings.candidate_generation_s = 0.0;
    stats_.timings.candidate_cofactor_s = 0.0;

    // Freeze every relation-acceptance/reduction policy before callbacks can
    // mutate process ENV. The same effective values are hashed into the sieve
    // run identity, so restart with a different policy fails before OOC
    // recovery mutation.
    const V3Mode frozen_cascade_v3_mode = cascade_v3_mode();
    const relation::RelationMergePolicy frozen_merge_policy =
        relation::relation_merge_policy_from_environment();
    const sieve::SieveRunPolicyIdentity run_policy_identity{
        .cascade_v3_mode = static_cast<uint8_t>(frozen_cascade_v3_mode),
        .accept_3lp = frozen_merge_policy.accept_3lp,
        .merge_weight3 = frozen_merge_policy.merge_weight3,
        .weight_cutoff = static_cast<uint32_t>(frozen_merge_policy.weight_cutoff),
        .drop_residual = frozen_merge_policy.drop_residual,
        .structured_filter_selection = static_cast<uint8_t>(structured_preflight.policy.selection),
    };

    // Sieve params
    sieve::SieveParams sieve_params;
    sieve_params.rational_threshold = params_.rational_threshold;
    sieve_params.algebraic_threshold = params_.algebraic_threshold;

    sieve::SieveRegion sieve_region;
    sieve_region.i_min = params_.sieve_i_min;
    sieve_region.i_max = params_.sieve_i_max;
    sieve_region.j_min = params_.sieve_j_min;
    sieve_region.j_max = params_.sieve_j_max;

    // Cofactorizer
    cofactor::CofactorizerConfig cofac_config;
    cofac_config.large_prime_bound = fb.params().large_prime_bound;
    cofac_config.allow_1lp = true;
    // 2LP requires SQUFOF which is expensive for large LP ranges.
    // Only enable for ≥50 digits where the LP key space is large enough to benefit.
    cofac_config.allow_2lp = (params_.digits >= 50);
    // 3LP cofactor + filter/merge upgrade (BACKLOG #1 algo breakthrough route).
    // Opt-in via ENV GNFS_3LP=1: 接受 (B², B³] cofactors 为 3LP relations,
    // 拓宽 LP space. 50d β plateau ~121% 主因是 lp_bits=23 时 weight≥3 LP keys 占 30%,
    // 接受 3LP 后 V3 cascade BFS spanning tree 处理 chain merge.
    // 默认 OFF: 零回归. 启用 OPT-IN 时 cofactor + filter + clique_merger 三处同步.
    cofac_config.allow_3lp = frozen_merge_policy.accept_3lp;

    // Special-Q generator
    sieve::SpecialQRange sq_range;
    sq_range.min_q = params_.special_q_min;
    sq_range.max_q = params_.special_q_max;
    sieve::SpecialQGenerator sq_gen(fb, sq_range);

    // Collector
    relation::CollectorConfig coll_config;
    coll_config.check_duplicates = true;

    // Bind every persisted Special-Q cursor to the exact mathematical inputs
    // that produced its relation prefix. This is computed once per sieve run
    // after polynomial and factor-base construction.
    const auto run_identity = sieve::make_sieve_run_identity(ctx, fb, params_, run_policy_identity);

    // ── Sieve mid-flight checkpoint resume (BACKLOG #11e, ENV GNFS_SIEVE_RESUME) ──
    // GNFS_SIEVE_RESUME=<base_path> (or GNFS_RESUME, 2026-05-21 alias covering
    // Phase 1+2+3): enables OOC streaming + sieve checkpoint, base_path acts as
    // both OOC base and checkpoint base. If <base_path>.sieve_ckpt exists → resume,
    // otherwise fresh start. Sieve loop persists state every CHECKPOINT_INTERVAL
    // batches. Normal completion → remove ckpt + flip OOC writer to MAGIC.
    std::string sieve_resume_path = structured_preflight.resume_base_path;
    std::optional<sieve::SieveCheckpoint> prior_ckpt;
    std::optional<sieve::SieveCheckpoint> terminal_checkpoint;
    if (!sieve_resume_path.empty()) {
        const std::string ckpt_file = sieve_resume_path + ".sieve_ckpt";
        if (sieve::SieveCheckpoint::exists(ckpt_file)) {
            // A present but invalid checkpoint is not equivalent to no
            // checkpoint: starting fresh would truncate the relation store and
            // silently discard the last provable paired prefix. Fail closed.
            prior_ckpt = sieve::SieveCheckpoint::load(ckpt_file);
            if (!prior_ckpt->matches_run_identity(run_identity)) {
                throw std::runtime_error(
                    "sieve checkpoint run identity does not match N, polynomial, "
                    "factor base, or sieve parameters");
            }
            const std::string checkpoint_ooc_path =
                relation::relation_corpus_detail::freeze_ooc_path(prior_ckpt->ooc_base_path);
            if (checkpoint_ooc_path != sieve_resume_path) {
                throw std::runtime_error(
                    "sieve checkpoint OOC path does not match the configured resume path");
            }
            if (prior_ckpt->ooc_format_version != relation::OOCRelationWriter::FORMAT_VERSION_V3) {
                throw std::runtime_error(
                    "sieve checkpoint requires paired OOC V3; legacy V2 recovery is unsafe");
            }
            if (prior_ckpt->round < 0 || prior_ckpt->round >= adaptive_round_limit) {
                throw std::runtime_error(
                    "sieve checkpoint round is outside the configured adaptive round limit");
            }

            coll_config.ooc_resume_snapshot = relation::OOCSnapshotDescriptor{
                .format_version = prior_ckpt->ooc_format_version,
                .store_id = prior_ckpt->ooc_store_id,
                .generation = prior_ckpt->ooc_generation,
                .count = prior_ckpt->ooc_relation_count,
                .data_end = prior_ckpt->ooc_data_end,
            };
            coll_config.ooc_resume_sequence_receipt = relation::RelationSequenceReceipt{
                .relation_count = prior_ckpt->ooc_relation_count,
                .low = prior_ckpt->ooc_sequence_receipt_low,
                .high = prior_ckpt->ooc_sequence_receipt_high,
            };
            emit_log(LogLevel::Info, Phase::Sieving,
                     "checkpoint loaded: sq_count=" + std::to_string(prior_ckpt->sq_count) +
                         " idx=" + std::to_string(prior_ckpt->current_index) +
                         " round=" + std::to_string(prior_ckpt->round) +
                         " generation=" + std::to_string(prior_ckpt->ooc_generation));
            std::fprintf(stderr,
                         "[sieve-resume] ckpt=%s sq_count=%llu idx=%u round=%d generation=%llu\n",
                         ckpt_file.c_str(), static_cast<unsigned long long>(prior_ckpt->sq_count),
                         prior_ckpt->current_index, prior_ckpt->round,
                         static_cast<unsigned long long>(prior_ckpt->ooc_generation));
            terminal_checkpoint = *prior_ckpt;
        }
        coll_config.ooc_enabled = true;
        coll_config.ooc_base_path = sieve_resume_path;
        emit_log(LogLevel::Info, Phase::Sieving,
                 "resume enabled: base=" + sieve_resume_path +
                     " ckpt_resume=" + (prior_ckpt ? "yes" : "no"));
    }
    // ── OOC streaming (BACKLOG #11c, ENV GNFS_OOC_RELATIONS=1) ──
    // 50d Round 2 909K relations 时 macOS OOM-killed (2026-05-17 实测).
    // OOC 启用后 collector 流式写盘到系统临时目录的
    // gnfs_relations_<run-id>.{reldata,relidx},
    // 内存只保留 (a,b) seen set, 显著减小 sieve 期间 RAM peak.
    // 不与 GNFS_SIEVE_RESUME / GNFS_RESUME 共存 (resume 已隐含 OOC enable)
    //
    // Size-aware default (BACKLOG #1, 2026-05-18):
    //   lp_bits ≥ 22 (50d+) 默认启用 OOC 防 Round 2+ OOM.
    //   GNFS_OOC_RELATIONS=0 explicit opt-out (e.g. tests / CI).
    //   GNFS_OOC_RELATIONS=1 explicit force-on (no size gate).
    if (sieve_resume_path.empty()) {
        if (structured_preflight.ooc_enabled) {
            coll_config.ooc_enabled = true;
            if (!structured_preflight.ooc_paths.has_value()) {
                throw std::logic_error("OOC route snapshot is missing its frozen path namespace");
            }
            coll_config.ooc_base_path = structured_preflight.ooc_paths->raw_base_path;
        }
    }
    const bool lp_enabled = structured_preflight.large_primes_enabled;
    const size_t distributed_workers = structured_preflight.distributed_workers;
    const bool distributed_size_gate_ok = structured_preflight.distributed_size_gate_ok;
    const bool distributed_force_small = structured_preflight.distributed_force_small;
    const bool distributed_route_selected = structured_preflight.distributed_route_selected;
    const auto& structured_policy = structured_preflight.policy;
    if (distributed_route_selected) {
        // Distributed workers own every relation artifact. Do not reserve an
        // unused master OOC pair: a process-level crash cannot run cleanup and
        // would otherwise strand that empty namespace.
        coll_config.ooc_enabled = false;
        coll_config.ooc_base_path.clear();
    }

    const bool verify_ooc_payload_after_callbacks =
        static_cast<bool>(progress_cb_) || static_cast<bool>(log_cb_);
    bool preserve_ooc_for_resume = terminal_checkpoint.has_value();
    relation::RelationCollector collector(coll_config);
    FreshOOCExceptionCleanup fresh_ooc_exception_cleanup(collector, preserve_ooc_for_resume);
    const bool recovered_finalized_ooc =
        collector.ooc_recovery_outcome() == relation::OOCRecoveryOutcome::FinalizedCorpus;
    const bool recovered_terminal_checkpoint =
        prior_ckpt.has_value() && prior_ckpt->collection_complete;
    if (recovered_finalized_ooc && !recovered_terminal_checkpoint) {
        throw std::runtime_error(
            "finalized OOC recovery requires a terminal collection checkpoint");
    }
    // Reserve/validate the exact OOC pair before the first sieve-stage callback.
    // A direct sieve invocation with a colliding raw base therefore fails with
    // no callback, relation generation, or artifact mutation.
    emit_progress(Phase::Sieving, "Starting sieve");
    if (cofac_config.allow_3lp) {
        emit_log(LogLevel::Info, Phase::Sieving,
                 "GNFS_3LP=1 enabled: cofactorizer accepts 3LP relations");
        std::fprintf(stderr, "[3lp] cofactor + filter accept 3LP (lp_bits=%zu B^3 bound)\n",
                     static_cast<size_t>(gnfs::util::ctz64(params_.large_prime_bound | 1)));
    }
    if (coll_config.ooc_enabled && sieve_resume_path.empty()) {
        const std::string& reason_str = structured_preflight.ooc_reason;
        const size_t lp_bits_est = relation::estimate_lp_bits(params_.large_prime_bound);
        emit_log(LogLevel::Info, Phase::Sieving,
                 std::string("OOC mode enabled (") + reason_str +
                     "): base=" + coll_config.ooc_base_path);
        std::fprintf(stderr, "[ooc] streaming relations to %s.{reldata,relidx} (%s, lp_bits=%zu)\n",
                     coll_config.ooc_base_path.c_str(), reason_str.c_str(), lp_bits_est);
    }
    if (recovered_finalized_ooc) {
        emit_log(LogLevel::Info, Phase::Sieving,
                 "recovered a finalized OOC corpus; skipping further sieve appends");
    }
    // CLAUDE.md 强制约定:拒绝 gcd(a-bm, N)>1 的关系
    collector.set_polynomial_context(ctx.n(), ctx.m());

    // Target
    const size_t factor_base_cols =
        gnfs::util::saturating_size_add(fb.rational_count(), fb.sieve_algebraic_count());
    size_t matrix_cols = gnfs::util::saturating_size_add(factor_base_cols, params_.target_excess);
    size_t initial_target = params_.raw_relation_target(matrix_cols);
    size_t batch_target = initial_target;
    relations_target_ = initial_target;

    auto make_reduction_config =
        [&](size_t input_relations, relation::ReductionStrategy legacy_strategy,
            const detail::StructuredOOCGenerationPaths* ooc_generation_paths) {
            relation::RelationReductionConfig reduction_config;
            reduction_config.filter.remove_singletons = true;
            reduction_config.filter.max_passes = 10;
            reduction_config.large_primes_enabled = lp_enabled;
            reduction_config.merge_rounds = 10;
            reduction_config.merge_policy = frozen_merge_policy;
            reduction_config.strategy =
                relation::select_reduction_strategy(structured_policy, legacy_strategy);
            if (reduction_config.strategy == relation::ReductionStrategy::Structured) {
                reduction_config.structured = relation::make_structured_filter_experimental_config(
                    input_relations, relation::structured_filter_hardware_workers());
                if (ooc_generation_paths != nullptr) {
                    reduction_config.structured->output_ooc_base_path =
                        ooc_generation_paths->output_requested_base;
                    reduction_config.structured->output_ooc_cleanup =
                        relation::OOCCleanupPolicy::RemoveArtifacts;
                }
            } else if (ooc_generation_paths != nullptr) {
                throw std::logic_error(
                    "structured OOC generation paths require the structured strategy");
            }
            return reduction_config;
        };

    auto publish_structured_reduction =
        [&](const relation::RelationReductionResult& reduction,
            const relation::RelationReductionConfig& reduction_config,
            const StructuredFilterRuntimeTelemetry& telemetry) {
            if (reduction_config.strategy == relation::ReductionStrategy::Structured) {
                const std::string record = structured_filter_record(
                    structured_policy, reduction, *reduction_config.structured, telemetry);
                emit_log(LogLevel::Info, Phase::Sieving, record);
                std::fprintf(stderr, "[%s]\n", record.c_str());
            }
        };

    auto reduce_snapshot = [&](relation::RawRelationSnapshot raw_snapshot,
                               relation::ReductionStrategy legacy_strategy) {
        auto reduction_config =
            make_reduction_config(raw_snapshot.size(), legacy_strategy, nullptr);
        if (reduction_config.strategy == relation::ReductionStrategy::Structured) {
            auto memory_before = util::process_memory_snapshot();
            const auto reduction_start = std::chrono::steady_clock::now();
            auto reduction = relation::RelationReductionEngine::reduce(std::move(raw_snapshot),
                                                                       reduction_config);
            const auto telemetry = finish_structured_filter_telemetry(
                "owned_snapshot", reduction_start, std::move(memory_before));
            publish_structured_reduction(reduction, reduction_config, telemetry);
            return reduction;
        }
        auto reduction =
            relation::RelationReductionEngine::reduce(std::move(raw_snapshot), reduction_config);
        return reduction;
    };

    auto reduce_vector = [&](std::vector<Relation> raw_relations,
                             relation::ReductionStrategy legacy_strategy) {
        const uint64_t generation = allocate_relation_generation();
        return reduce_snapshot(relation::RawRelationSnapshot(generation, std::move(raw_relations)),
                               legacy_strategy);
    };

    const bool structured_ooc_route =
        coll_config.ooc_enabled &&
        structured_policy.selection == relation::StructuredFilterSelection::Structured;
    std::optional<relation::OOCSnapshotDescriptor> last_structured_ooc_source;
    std::optional<relation::OOCSnapshotDescriptor> last_legacy_ooc_source;
    auto reduce_collector_snapshot = [&](relation::ReductionStrategy legacy_strategy) {
        if (!structured_ooc_route) {
            if (!coll_config.ooc_enabled) {
                return reduce_vector(collector.snapshot_relations(), legacy_strategy);
            }

            std::pair<std::vector<Relation>, relation::OOCSnapshotDescriptor> snapshot;
            if (recovered_finalized_ooc) {
                const auto descriptor = collector.finalize_ooc();
                if (!descriptor.has_value()) {
                    throw std::logic_error(
                        "recovered legacy OOC collector has no finalized descriptor");
                }
                snapshot = {collector.snapshot_relations(), *descriptor};
            } else {
                snapshot = collector.with_unique_ooc_prefix(
                    [](const relation::CollectorUniqueOOCPrefixSource& source) {
                        return std::pair(source.read_all_verified(), source.descriptor());
                    });
            }

            auto reduction = reduce_vector(std::move(snapshot.first), legacy_strategy);
            if (reduction.stats.input_relations != snapshot.second.count) {
                throw std::logic_error(
                    "legacy OOC reduction input count differs from its raw prefix");
            }
            last_legacy_ooc_source = snapshot.second;
            return reduction;
        }
        if (!structured_preflight.ooc_paths.has_value()) {
            throw std::logic_error("structured OOC reduction is missing its frozen run paths");
        }

        const uint64_t generation = allocate_relation_generation();
        const auto generation_paths = structured_preflight.ooc_paths->generation_paths(generation);
        std::optional<relation::RelationReductionConfig> reduction_config;
        std::optional<StructuredFilterRuntimeTelemetry> telemetry;
        std::optional<relation::StructuredReductionTelemetryRecord> stage_telemetry;
        auto reduction_and_source = collector.with_unique_ooc_prefix(
            [&](const relation::CollectorUniqueOOCPrefixSource& source) {
                reduction_config.emplace(
                    make_reduction_config(source.count(), legacy_strategy, &generation_paths));
                auto memory_before = util::process_memory_snapshot();
                const auto reduction_start = std::chrono::steady_clock::now();
                auto reduction = [&] {
                    if (!structured_preflight.stage_telemetry_enabled) {
                        return relation::RelationReductionEngine::reduce_direct_borrowed_structured(
                            generation, source, *reduction_config);
                    }
                    relation::StructuredReductionTelemetry observer;
                    auto observed = relation::RelationReductionEngine::
                        reduce_direct_borrowed_structured_observed(generation, source,
                                                                   *reduction_config, observer);
                    stage_telemetry.emplace(observer.snapshot());
                    return observed;
                }();
                telemetry.emplace(finish_structured_filter_telemetry(
                    "direct_ooc_prefix", reduction_start, std::move(memory_before)));
                return std::pair(std::move(reduction), source.descriptor());
            });

        if (!reduction_config.has_value() || !telemetry.has_value()) {
            throw std::logic_error("structured OOC reduction did not freeze its configuration");
        }
        auto reduction = std::move(reduction_and_source.first);
        const relation::OOCSnapshotDescriptor source_descriptor = reduction_and_source.second;
        publish_structured_reduction(reduction, *reduction_config, *telemetry);
        if (stage_telemetry.has_value()) {
            const std::string record = structured_filter_stage_record(*stage_telemetry);
            emit_log(LogLevel::Info, Phase::Sieving, record);
            std::fprintf(stderr, "[%s]\n", record.c_str());
        }
        if (reduction.stats.input_relations != source_descriptor.count) {
            throw std::logic_error(
                "structured OOC reduction input count differs from its raw prefix");
        }
        last_structured_ooc_source = source_descriptor;
        return reduction;
    };

    emit_log(LogLevel::Info, Phase::Sieving,
             "target=" + std::to_string(initial_target) +
                 " matrix_cols=" + std::to_string(matrix_cols) + " sq_range=[" +
                 std::to_string(sq_range.min_q) + "," + std::to_string(sq_range.max_q) + "]");

    // Shared AdaptiveBasisManager across all per-thread local_sieve instances
    // so adaptive-lattice telemetry (special_qs, retries, rescues) aggregates
    // across the entire sieve phase. When the manager is disabled (default),
    // all worker calls are zero-overhead.
    sieve::AdaptiveBasisManager adaptive_mgr{};

    size_t sq_count = 0;
    size_t candidates_total = 0;
    size_t max_sq = params_.max_special_q;
    int round_start = 0;

    // Apply checkpoint state if resuming (BACKLOG #11e)
    if (prior_ckpt) {
        sq_count = prior_ckpt->sq_count;
        candidates_total = prior_ckpt->candidates_total;
        batch_target = prior_ckpt->batch_target;
        relations_target_ = batch_target;
        round_start = prior_ckpt->round;
        sq_gen.reset_to(prior_ckpt->current_index);
        emit_log(LogLevel::Info, Phase::Sieving,
                 "resuming sieve from checkpoint: skip " + std::to_string(sq_count) + " prior SQs");
    }

    // ── Distributed sieve dispatch (ENV GNFS_DISTRIBUTED_SIEVE_WORKERS=N) ──
    // When set, replace the in-process adaptive sieve loop with a single
    // distributed run: master forks N child workers, each handles a chunk of
    // the Special-Q index range, writes a per-worker OOC store. Master merges
    // all worker stores into a single relation vector. Downstream filter+merge
    // phases (Phase 4) then process those relations as usual.
    //
    // Limitations:
    //   - One-shot only. The distributed wave processes up to max_special_q
    //     SQs, evenly split across workers. The adaptive multi-round retry
    //     loop is disabled — for under-sized SQ ranges the caller must rerun
    //     with a wider sq_range / larger max_special_q.
    //   - Incompatible with GNFS_SIEVE_RESUME / mid-flight checkpoints
    //     (distributed workers do not write checkpoints — skipped when
    //     sieve_resume_path is non-empty).
    //   - Each worker maintains its own (a, b) seen set; the master dedups
    //     cross-worker duplicates on merge.
    {
        // Size gate: distributed dispatch is only worthwhile for 30+ digit
        // numbers where each worker chunk processes thousands of SQs. Below
        // 30 digits the in-process adaptive loop converges in 10-100 SQs and
        // distributed dispatch wastes work because workers cannot early-stop
        // when the matrix target is already met.
        // ENV GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL=1 overrides the gate (test
        // harness only).
        if (distributed_workers > 0 && !distributed_size_gate_ok && !distributed_force_small) {
            std::fprintf(stderr,
                         "[dist_sieve] skip dispatch: digits=%zu < 30 "
                         "(set GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL=1 to override)\n",
                         params_.digits);
        }
        if (distributed_route_selected) {
            emit_log(LogLevel::Info, Phase::Sieving,
                     "GNFS_DISTRIBUTED_SIEVE_WORKERS=" + std::to_string(distributed_workers) +
                         " — dispatching distributed sieve");
            std::fprintf(stderr, "[dist_sieve] dispatch: workers=%zu sq_range=[%u,%u] max_sq=%zu\n",
                         distributed_workers, sq_range.min_q, sq_range.max_q, max_sq);

            sieve::DistributedSieveConfig dist_cfg{
                .num_workers = distributed_workers,
                .base_path = structured_preflight.distributed_base_path,
                .sq_per_worker = structured_preflight.distributed_sq_per_worker,
                .worker_timeout_ms = structured_preflight.distributed_worker_timeout_ms,
            };
            // Cap each worker at ~max_special_q / num_workers SQs to avoid
            // runaway sieve when the caller-specified sq_range covers vastly
            // more primes than needed.
            if (dist_cfg.sq_per_worker == 0 && max_sq > 0) {
                dist_cfg.sq_per_worker = std::max<size_t>(1, max_sq / distributed_workers);
            }

            std::vector<sieve::DistributedSieveWorkerResult> wstats;
            auto dist_rels =
                sieve::run_distributed_sieve(dist_cfg, ctx, fb, sieve_params, sieve_region,
                                             cofac_config, ctx.n(), ctx.m(), sq_range, &wstats);

            // Sieve done — record stats.
            for (const auto& w : wstats)
                sq_count += w.sq_count;
            stats_.timings.sieve_s =
                std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0)
                    .count();
            stats_.relations_found = dist_rels.size();
            stats_.special_q_processed = sq_count;

            // Run filter+merge once on collected relations (mirrors the
            // adaptive-loop body but only one pass — no adaptive retry).
            auto dist_reduction = reduce_vector(
                std::move(dist_rels), lp_enabled ? relation::ReductionStrategy::StandardV0
                                                 : relation::ReductionStrategy::NoLargePrimes);
            // The distributed workers own every relation payload; the eager
            // master collector is unused. Remove its exact fresh empty pair
            // before any success callback or normal return can strand it and
            // block a same-path retry.
            if (!collector.discard_uncommitted_fresh_ooc_noexcept()) {
                throw std::runtime_error(
                    "distributed sieve could not remove its unused master OOC artifacts");
            }
            stats_.sieve_rounds_completed = 1;
            stats_.sieve_stop_reason = SieveStopReason::DistributedWaveComplete;

            emit_log(LogLevel::Info, Phase::Sieving,
                     "distributed sieve done: raw=" + std::to_string(stats_.relations_found) +
                         " usable=" + std::to_string(dist_reduction.size()) +
                         " sq=" + std::to_string(sq_count));
            std::fprintf(stderr, "[dist_sieve] done: raw=%zu usable=%zu sq=%zu\n",
                         stats_.relations_found, dist_reduction.size(), sq_count);
            emit_progress(Phase::Sieving, "Sieving complete (distributed)", 1.0);

            stats_rollback.commit();
            return dist_reduction;
        }
    }

    // Adaptive sieve-filter-merge loop:
    // Collect raw relations, filter+merge, check if enough usable.
    // If not, increase target and continue sieving.
    std::optional<relation::RelationReductionResult> last_reduction;
    // BACKLOG #11e: checkpoint write 频率. Every N SQ batches (each batch
    // 2-4 SQs) we persist state. N=25 → ~50-100 SQs/checkpoint.
    // Trade-off: 频繁 → 多 disk IO; 稀疏 → resume 时丢更多 SQ.
    constexpr size_t CHECKPOINT_BATCH_INTERVAL = 25;
    size_t last_checkpoint_batch = 0;
    auto publish_sieve_checkpoint = [&](int checkpoint_round, bool terminal_boundary) {
        const auto descriptor = collector.checkpoint_ooc();
        const auto sequence_receipt = collector.ooc_accepted_sequence_receipt();
        if (sequence_receipt.relation_count != descriptor.count) {
            throw std::logic_error("OOC checkpoint receipt count differs from its descriptor");
        }

        sieve::SieveCheckpoint checkpoint;
        checkpoint.sq_count = sq_count;
        checkpoint.current_index = sq_gen.current_index();
        checkpoint.round = checkpoint_round;
        checkpoint.batch_target = batch_target;
        checkpoint.candidates_total = candidates_total;
        checkpoint.run_n = run_identity.run_n;
        checkpoint.run_fingerprint_lo = run_identity.fingerprint_lo;
        checkpoint.run_fingerprint_hi = run_identity.fingerprint_hi;
        checkpoint.ooc_base_path = sieve_resume_path;
        checkpoint.ooc_format_version = descriptor.format_version;
        checkpoint.ooc_store_id = descriptor.store_id;
        checkpoint.ooc_generation = descriptor.generation;
        checkpoint.ooc_relation_count = descriptor.count;
        checkpoint.ooc_data_end = descriptor.data_end;
        checkpoint.ooc_sequence_receipt_low = sequence_receipt.low;
        checkpoint.ooc_sequence_receipt_high = sequence_receipt.high;
        checkpoint.collection_complete = terminal_boundary;

        const std::string checkpoint_path = sieve_resume_path + ".sieve_ckpt";
        const std::string checkpoint_temporary =
            sieve::SieveCheckpoint::temporary_path(checkpoint_path);
        bool checkpoint_published = false;
        bool checkpoint_proven_absent = false;
        bool recovered_postpublication_error = false;
        std::string checkpoint_error;
        // From this point onward, a save failure may mean rename succeeded but
        // the directory durability barrier failed. Preserve the suspended raw
        // prefix until both official checkpoint paths are strictly proven
        // absent.
        preserve_ooc_for_resume = true;
        try {
            checkpoint.save(checkpoint_path);
            checkpoint_published = true;
        } catch (const std::exception& error) {
            checkpoint_error = error.what();
        } catch (...) {
            checkpoint_error = "unknown error";
        }

        if (!checkpoint_published) {
            // POSIX rename precedes the parent-directory fsync. If that
            // durability step reports an error, the intended checkpoint may
            // already be the visible official file.
            try {
                const auto visible = sieve::SieveCheckpoint::load(checkpoint_path);
                if (visible == checkpoint) {
                    checkpoint_published = true;
                    recovered_postpublication_error = true;
                }
            } catch (...) {
                // The intended checkpoint is not provably visible.
            }
        }

        if (!checkpoint_published && !terminal_checkpoint.has_value() &&
            !sieve::SieveCheckpoint::exists(checkpoint_path) &&
            !sieve::SieveCheckpoint::exists(checkpoint_temporary)) {
            checkpoint_proven_absent = true;
        }

        if (!checkpoint_published && !checkpoint_proven_absent) {
            throw std::runtime_error("checkpoint publication outcome is uncertain; preserving the "
                                     "suspended OOC prefix for restart: " +
                                     checkpoint_error);
        }

        if (checkpoint_proven_absent) {
            // Reopen this durable prefix. A periodic checkpoint can retry on a
            // later batch, but the terminal boundary must not finalize a raw
            // prefix that has no exact durable receipt.
            preserve_ooc_for_resume = false;
            try {
                collector.resume_ooc(descriptor);
            } catch (const std::exception& resume_error) {
                throw std::runtime_error("checkpoint save failed (" + checkpoint_error +
                                         ") and OOC append recovery also failed (" +
                                         resume_error.what() + ")");
            }
            if (terminal_boundary) {
                throw std::runtime_error(
                    "terminal checkpoint publication failed before becoming visible; "
                    "refusing to finalize an unbound OOC prefix: " +
                    checkpoint_error);
            }
            emit_log(LogLevel::Warn, Phase::Sieving,
                     "checkpoint publication failed before the intended checkpoint became "
                     "visible; OOC prefix reopened for retry: " +
                         checkpoint_error);
            return;
        }

        // A published checkpoint and its suspended prefix form the durable
        // pair. Periodic publication reopens it for append; terminal
        // publication deliberately remains suspended so finalize commits the
        // exact descriptor generation recorded by the checkpoint.
        terminal_checkpoint = checkpoint;
        if (!terminal_boundary) {
            collector.resume_ooc(descriptor);
        }
        last_checkpoint_batch = 0;
        if (recovered_postpublication_error) {
            emit_log(LogLevel::Warn, Phase::Sieving,
                     "checkpoint save reported a post-publication durability error, but the "
                     "intended checkpoint is visible and valid: " +
                         checkpoint_error);
        }
    };

    const size_t local_sieve_thread_budget = params_.max_local_sieve_threads;
    if (local_sieve_thread_budget == 0) {
        throw std::logic_error("max_local_sieve_threads must be frozen before sieving");
    }
    const size_t configured_batch_workers = params_.max_special_q_batch_workers;
    if (configured_batch_workers < 1 || configured_batch_workers > 4) {
        throw std::logic_error("max_special_q_batch_workers must be in [1, 4]");
    }
    stats_.local_sieve_thread_budget = local_sieve_thread_budget;
    stats_.special_q_batch_worker_limit =
        std::min(local_sieve_thread_budget, configured_batch_workers);

    int last_reduction_round = round_start;
    for (int round = round_start; round < adaptive_round_limit; ++round) {
        last_reduction_round = round;
        // ── Batch SQ processing: sieve + cofac in parallel ──
        // Collect a batch of SQ primes, sieve them in parallel (each thread
        // owns its own LatticeSieve copy), then cofac results in parallel.
        while (!recovered_finalized_ooc && !recovered_terminal_checkpoint && sq_gen.has_next() &&
               collector.size() < batch_target && sq_count < max_sq) {
            // Collect a batch of SQs for parallel processing
            // Batch width freezes membership and checkpoint cadence independently
            // from the configured worker cap. Each active worker owns a sieve array.
            // Larger inputs retain the existing two-SQ memory bound.
            const size_t fixed_batch_width = (params_.digits <= 50) ? 4 : 2;
            const size_t remaining_special_q = max_sq - sq_count;
            const size_t batch_limit = std::min(fixed_batch_width, remaining_special_q);
            std::vector<sieve::SpecialQ> sq_batch;
            sq_batch.reserve(batch_limit);
            while (sq_batch.size() < batch_limit && sq_gen.has_next()) {
                auto sq = sq_gen.next();
                if (!sq)
                    break;
                sq_batch.push_back(*sq);
            }
            if (sq_batch.empty())
                break;

            bool candidate_rss_sample_selected = false;
            size_t candidate_rss_sample_candidates = 0;
            std::optional<uint64_t> candidate_after_generation_current_rss;
            std::optional<uint64_t> candidate_after_cofactor_current_rss;
            {
                // Stage 1 retains one result per canonical special-Q slot. Sieve
                // arrays are worker-local and are destroyed before candidate-level
                // cofactor work begins, so the two parallel regions never overlap.
                // Divide the total compute-lane budget across the active outer
                // workers. A one-lane LatticeSieve executes inline; a multi-lane
                // instance blocks its outer worker while its inner workers run.
                const sieve::LocalSieveThreadPlan thread_plan = sieve::plan_local_sieve_threads(
                    local_sieve_thread_budget, stats_.special_q_batch_worker_limit,
                    sq_batch.size());
                const size_t n_workers = thread_plan.threads_per_worker.size();
                std::vector<size_t> configured_sieve_threads(n_workers, 0);

                ++stats_.special_q_batch_count;
                stats_.special_q_batch_peak_size =
                    std::max(stats_.special_q_batch_peak_size, sq_batch.size());
                stats_.special_q_batch_peak_workers =
                    std::max(stats_.special_q_batch_peak_workers, n_workers);

                // Launch outer batch workers. The lane assignment above bounds
                // their combined local sieve compute parallelism. Dynamic claiming
                // drains every special-Q before the lowest canonical failure is
                // surfaced, while fixed result slots preserve input order.
                const auto candidate_generation_started = std::chrono::high_resolution_clock::now();
                auto batch_sieve_results = util::ordered_work_stealing_map<sieve::SieveResult>(
                    sq_batch.size(), static_cast<uint32_t>(n_workers),
                    [&](size_t worker_index) {
                        auto local_sieve =
                            std::make_unique<sieve::LatticeSieve>(ctx, fb, sieve_params);
                        local_sieve->set_region(sieve_region);
                        local_sieve->set_max_threads(thread_plan.threads_per_worker[worker_index]);
                        configured_sieve_threads[worker_index] =
                            local_sieve->configured_max_threads();
                        local_sieve->set_adaptive_manager(&adaptive_mgr);
                        return local_sieve;
                    },
                    [&](std::unique_ptr<sieve::LatticeSieve>& local_sieve, size_t special_q_index) {
                        return local_sieve->sieve_special_q(sq_batch[special_q_index]);
                    });
                stats_.timings.candidate_generation_s +=
                    std::chrono::duration<double>(std::chrono::high_resolution_clock::now() -
                                                  candidate_generation_started)
                        .count();

                size_t generated_candidates = 0;
                for (const auto& sieve_result : batch_sieve_results) {
                    if (sieve_result.candidates.size() >
                        std::numeric_limits<size_t>::max() - generated_candidates) {
                        throw std::overflow_error("generated candidate total exceeds size_t");
                    }
                    generated_candidates += sieve_result.candidates.size();
                }
                candidate_rss_sample_selected =
                    generated_candidates > stats_.candidate_batch_rss_sample_candidates;
                if (candidate_rss_sample_selected) {
                    candidate_rss_sample_candidates = generated_candidates;
                    candidate_after_generation_current_rss =
                        util::process_memory_snapshot().current_rss_bytes;
                }

                size_t configured_thread_total = 0;
                size_t configured_worker_peak = 0;
                for (size_t worker_index = 0; worker_index < n_workers; ++worker_index) {
                    if (configured_sieve_threads[worker_index] !=
                        thread_plan.threads_per_worker[worker_index]) {
                        throw std::logic_error(
                            "local sieve thread plan was not applied to its worker");
                    }
                    configured_thread_total += configured_sieve_threads[worker_index];
                    configured_worker_peak =
                        std::max(configured_worker_peak, configured_sieve_threads[worker_index]);
                }
                stats_.special_q_batch_peak_assigned_threads =
                    std::max(stats_.special_q_batch_peak_assigned_threads, configured_thread_total);
                stats_.special_q_worker_peak_sieve_threads =
                    std::max(stats_.special_q_worker_peak_sieve_threads, configured_worker_peak);

                // Stage 2 reuses the complete compute-lane budget after every
                // LatticeSieve has been destroyed. Results are folded by special-Q
                // and candidate ordinal, independent of worker completion order.
                cofactor::CandidateBatchOptions cofactor_batch_options;
                cofactor_batch_options.max_workers =
                    static_cast<uint32_t>(local_sieve_thread_budget);
                const auto candidate_cofactor_started = std::chrono::high_resolution_clock::now();
                auto cofactor_batch = cofactor::verify_candidate_batch(
                    ctx, fb, cofac_config, batch_sieve_results, cofactor_batch_options);
                stats_.timings.candidate_cofactor_s +=
                    std::chrono::duration<double>(std::chrono::high_resolution_clock::now() -
                                                  candidate_cofactor_started)
                        .count();
                if (cofactor_batch.total_candidates != generated_candidates) {
                    throw std::logic_error(
                        "candidate batch total differs from generated candidate corpus");
                }
                if (candidate_rss_sample_selected) {
                    candidate_after_cofactor_current_rss =
                        util::process_memory_snapshot().current_rss_bytes;
                }

                stats_.candidate_batch_peak_workers =
                    std::max(stats_.candidate_batch_peak_workers, cofactor_batch.workers_used);
                stats_.candidate_batch_peak_chunks =
                    std::max(stats_.candidate_batch_peak_chunks, cofactor_batch.planned_chunks);
                stats_.candidate_batch_peak_candidates = std::max(
                    stats_.candidate_batch_peak_candidates, cofactor_batch.total_candidates);
                if (cofactor_batch.planned_chunks >
                    std::numeric_limits<size_t>::max() - stats_.candidate_batch_total_chunks) {
                    throw std::overflow_error("cofactor chunk total exceeds size_t");
                }
                stats_.candidate_batch_total_chunks += cofactor_batch.planned_chunks;
                if (cofactor_batch.total_candidates >
                    std::numeric_limits<size_t>::max() - candidates_total) {
                    throw std::overflow_error("candidate total exceeds size_t");
                }
                candidates_total += cofactor_batch.total_candidates;

                // Preserve the historical collector order: special-Q first, then
                // candidate order within that special-Q.
                for (auto& relations : cofactor_batch.relations_by_special_q) {
                    for (auto& rel : relations)
                        collector.add(std::move(rel));
                }
            }
            if (candidate_rss_sample_selected) {
                stats_.candidate_batch_rss_sample_candidates = candidate_rss_sample_candidates;
                const auto candidate_after_release_current_rss =
                    util::process_memory_snapshot().current_rss_bytes;
                if (candidate_after_generation_current_rss &&
                    candidate_after_cofactor_current_rss && candidate_after_release_current_rss) {
                    stats_.candidate_batch_after_generation_current_rss_bytes =
                        candidate_after_generation_current_rss;
                    stats_.candidate_batch_after_cofactor_current_rss_bytes =
                        candidate_after_cofactor_current_rss;
                    stats_.candidate_batch_after_release_current_rss_bytes =
                        candidate_after_release_current_rss;
                } else {
                    stats_.candidate_batch_after_generation_current_rss_bytes.reset();
                    stats_.candidate_batch_after_cofactor_current_rss_bytes.reset();
                    stats_.candidate_batch_after_release_current_rss_bytes.reset();
                }
            }
            sq_count += sq_batch.size();

            // ── Periodic checkpoint write (BACKLOG #11e) ──
            // Persist sieve state every CHECKPOINT_BATCH_INTERVAL batches when
            // GNFS_SIEVE_RESUME enabled. Crash mid-batch → next resume rewinds to
            // last successful checkpoint, drops ≤25 batches of work (acceptable).
            if (!sieve_resume_path.empty()) {
                ++last_checkpoint_batch;
                if (last_checkpoint_batch >= CHECKPOINT_BATCH_INTERVAL) {
                    publish_sieve_checkpoint(round, false);
                }
            }

            // Progress report
            if (sq_count % params_.progress_interval == 0 || sq_count <= 8) {
                auto now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double>(now - t0).count();
                size_t rels_per_sec =
                    (elapsed > 0.01)
                        ? static_cast<size_t>(static_cast<double>(collector.size()) / elapsed)
                        : 0;
                double pct =
                    static_cast<double>(collector.size()) / static_cast<double>(batch_target);
                stats_.relations_found = collector.size();
                stats_.special_q_processed = sq_count;
                relations_target_ = batch_target;
                emit_progress(Phase::Sieving,
                              "SQ=" + std::to_string(sq_count) +
                                  " rels=" + std::to_string(collector.size()) + " " +
                                  std::to_string(rels_per_sec) + "/s",
                              std::min(pct, 1.0));
            }
        }

        if (collector.size() < 10 && !recovered_finalized_ooc && !recovered_terminal_checkpoint) {
            stats_.sieve_stop_reason =
                sq_count >= max_sq
                    ? SieveStopReason::SpecialQBudgetReached
                    : (!sq_gen.has_next() ? SieveStopReason::SpecialQRangeExhausted
                                          : SieveStopReason::InsufficientRawRelations);
            break;
        }

        // Filter + merge a stable snapshot to check usable relation count. OOC
        // collection must remain appendable when another adaptive round is needed.
        const bool use_v3 =
            lp_enabled && cascade_v3_enabled_for_round(frozen_cascade_v3_mode, round);
        const auto strategy = !lp_enabled ? relation::ReductionStrategy::NoLargePrimes
                                          : (use_v3 ? relation::ReductionStrategy::StandardV0WithV3
                                                    : relation::ReductionStrategy::StandardV0);
        // Avoid retaining the previous reduced corpus while materializing the next probe.
        last_reduction.reset();
        last_reduction.emplace(reduce_collector_snapshot(strategy));
        ++stats_.sieve_rounds_completed;
        const auto& reduction_stats = last_reduction->stats;

        // V3 cascade (GNFS_CASCADE_V3=1): engine runs it after V0 on a
        // partial-relation copy; preserve the existing progress diagnostics.
        if (use_v3 && reduction_stats.separated_partial_relations > 0) {
            emit_log(LogLevel::Info, Phase::Sieving,
                     "v3_cascade(sieve_loop): " + reduction_stats.v3.to_string() +
                         " added=" + std::to_string(reduction_stats.v3_relations_added));
            // stderr fallback when log_cb_ not registered (for stress/progressive)
            std::fprintf(stderr, "[v3_cascade.sieve] %s added=%zu\n",
                         reduction_stats.v3.to_string().c_str(),
                         reduction_stats.v3_relations_added);
        }

        // Accurate effective_cols = matrix_cols + actual LP keys
        // (matrix builder will create one column per odd-exp unique LP key).
        // 50d/60d 实测 lp_cols ratio = 64% of usable, far above 旧 5% guess.
        const size_t lp_cols = reduction_stats.output_lp_columns;
        const size_t effective_cols = relation::effective_column_count(matrix_cols, lp_cols);

        // Check: enough usable relations?
        if (recovered_finalized_ooc) {
            stats_.sieve_stop_reason = SieveStopReason::RecoveredFinalizedCorpus;
            break;
        }
        if (recovered_terminal_checkpoint) {
            stats_.sieve_stop_reason = SieveStopReason::RecoveredTerminalCheckpoint;
            break;
        }
        if (relation::has_effective_column_excess(last_reduction->size(), matrix_cols, lp_cols)) {
            stats_.sieve_stop_reason = SieveStopReason::EffectiveColumnExcess;
            break;
        }

        // Not enough — increase target and continue if SQs available
        if (!sq_gen.has_next() || sq_count >= max_sq) {
            stats_.sieve_stop_reason = sq_count >= max_sq ? SieveStopReason::SpecialQBudgetReached
                                                          : SieveStopReason::SpecialQRangeExhausted;
            break;
        }

        double merge_rate = (collector.size() > 0) ? static_cast<double>(last_reduction->size()) /
                                                         static_cast<double>(collector.size())
                                                   : 0.01;
        const size_t target_usable = scale_by_tenths_floor(effective_cols, 11);
        const size_t needed_raw = gnfs::util::size_from_nonnegative_double_floor(
            static_cast<double>(target_usable) / std::max(merge_rate, 0.001));
        // Raise cap: for low merge rates (~2%), need up to 100× initial target
        batch_target =
            std::min(std::max(gnfs::util::saturating_size_product(batch_target, 2), needed_raw),
                     gnfs::util::saturating_size_product(initial_target,
                                                         100)); // generous cap for low merge rates
        relations_target_ = batch_target;

        // β = lp_cols / usable (BACKLOG #1 diagnostic). β << 1 means matrix
        // build has excess and BW can find dependencies; β >= 1 means LP cols
        // dominate matrix and we're in the plateau regime.
        double beta = (last_reduction->size() > 0) ? static_cast<double>(lp_cols) /
                                                         static_cast<double>(last_reduction->size())
                                                   : 0.0;
        emit_log(LogLevel::Info, Phase::Sieving,
                 "round " + std::to_string(round + 1) +
                     ": usable=" + std::to_string(last_reduction->size()) + "/" +
                     std::to_string(matrix_cols) + " lp_cols=" + std::to_string(lp_cols) +
                     " eff_cols=" + std::to_string(effective_cols) +
                     " merge_rate=" + std::to_string(merge_rate) + " beta=" + std::to_string(beta) +
                     " new_target=" + std::to_string(batch_target));
        // stderr fallback for stress/progressive runs (no log_cb_ registered)
        std::fprintf(stderr,
                     "[round %d] usable=%zu/%zu lp_cols=%zu eff_cols=%zu merge_rate=%.4f beta=%.4f "
                     "new_target=%zu\n",
                     round + 1, last_reduction->size(), matrix_cols, lp_cols, effective_cols,
                     merge_rate, beta, batch_target);
    }

    if (stats_.sieve_stop_reason == SieveStopReason::NotStarted) {
        stats_.sieve_stop_reason = SieveStopReason::AdaptiveRoundLimitReached;
    }

    // A tiny or already-exhausted corpus may leave the adaptive loop without
    // probing. Still publish one reduced generation for the current stable
    // raw prefix instead of exposing raw relations through the step API.
    if (!last_reduction) {
        const bool use_v3 = lp_enabled && cascade_v3_enabled_for_round(frozen_cascade_v3_mode,
                                                                       last_reduction_round);
        const auto strategy = !lp_enabled ? relation::ReductionStrategy::NoLargePrimes
                                          : (use_v3 ? relation::ReductionStrategy::StandardV0WithV3
                                                    : relation::ReductionStrategy::StandardV0);
        last_reduction.emplace(reduce_collector_snapshot(strategy));
        ++stats_.sieve_rounds_completed;
    }
    if (!sieve_resume_path.empty() && !recovered_finalized_ooc) {
        // Final magic may be recovered after a process exit but before
        // checkpoint removal. Bind the complete terminal relation sequence and
        // logical sieve cursor first; finalized recovery therefore never
        // adopts an extension beyond the checkpoint receipt.
        publish_sieve_checkpoint(last_reduction_round, true);
    }
    // The adaptive loop is the last append boundary. Finalize OOC storage only
    // after every possible continuation has been decided. A structured OOC
    // result is authoritative only when its last probe describes the exact
    // terminal raw prefix. Prove that while the collector still owns the raw
    // pair; only then transfer and remove those no-longer-needed artifacts.
    const bool remove_legacy_terminal_raw_ooc =
        coll_config.ooc_enabled && !structured_ooc_route &&
        options.legacy_raw_ooc_cleanup == LegacyRawOOCCleanupPolicy::RemoveArtifacts;
    std::optional<relation::OOCSnapshotDescriptor> terminal_ooc_descriptor;
    if (structured_ooc_route) {
        if (!last_structured_ooc_source.has_value()) {
            throw std::logic_error("structured OOC route has no successful raw-prefix probe");
        }
        terminal_ooc_descriptor = collector.finalize_ooc();
        if (!terminal_ooc_descriptor.has_value()) {
            throw std::logic_error("structured OOC route did not finalize an OOC store");
        }
        const auto& probed = *last_structured_ooc_source;
        const bool same_terminal_prefix =
            terminal_ooc_descriptor->format_version == probed.format_version &&
            terminal_ooc_descriptor->store_id == probed.store_id &&
            terminal_ooc_descriptor->count == probed.count &&
            terminal_ooc_descriptor->data_end == probed.data_end;
        const bool generation_transition_valid =
            recovered_finalized_ooc
                ? terminal_ooc_descriptor->generation == probed.generation
                : probed.generation != std::numeric_limits<uint64_t>::max() &&
                      terminal_ooc_descriptor->generation == probed.generation + 1;
        if (!same_terminal_prefix || !generation_transition_valid ||
            last_reduction->stats.input_relations != terminal_ooc_descriptor->count) {
            throw std::logic_error(
                "structured OOC final reduction does not match the terminal raw prefix");
        }
    } else {
        terminal_ooc_descriptor = collector.finalize_ooc();
        if (remove_legacy_terminal_raw_ooc) {
            if (!terminal_ooc_descriptor.has_value()) {
                throw std::logic_error(
                    "legacy raw OOC cleanup requested without a finalized OOC store");
            }
            if (!last_legacy_ooc_source.has_value()) {
                throw std::logic_error(
                    "legacy raw OOC cleanup requested without a proven reduction prefix");
            }
            const auto& probed = *last_legacy_ooc_source;
            const bool same_physical_prefix =
                terminal_ooc_descriptor->format_version == probed.format_version &&
                terminal_ooc_descriptor->store_id == probed.store_id &&
                terminal_ooc_descriptor->count == probed.count &&
                terminal_ooc_descriptor->data_end == probed.data_end;
            const bool generation_transition_valid =
                recovered_finalized_ooc
                    ? terminal_ooc_descriptor->generation == probed.generation
                    : probed.generation != std::numeric_limits<uint64_t>::max() &&
                          terminal_ooc_descriptor->generation == probed.generation + 1;
            if (!same_physical_prefix || !generation_transition_valid ||
                last_reduction->stats.input_relations != terminal_ooc_descriptor->count ||
                collector.size() != terminal_ooc_descriptor->count) {
                throw std::logic_error(
                    "legacy reduction does not match the terminal raw OOC prefix");
            }
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.sieve_s = std::chrono::duration<double>(t1 - t0).count();

    // Collect final stats
    stats_.relations_found = collector.size();
    auto coll_stats = collector.stats();
    stats_.full_relations = coll_stats.full_relations;
    stats_.partial_1lp = coll_stats.partial_1lp;
    stats_.partial_2lp = coll_stats.partial_2lp;
    stats_.special_q_processed = sq_count;
    stats_.candidates_total = candidates_total;

    emit_log(LogLevel::Info, Phase::Sieving,
             "done: sq=" + std::to_string(sq_count) + " raw=" + std::to_string(collector.size()) +
                 " usable=" + std::to_string(last_reduction->size()) +
                 " (full=" + std::to_string(coll_stats.full_relations) +
                 " 1lp=" + std::to_string(coll_stats.partial_1lp) +
                 " 2lp=" + std::to_string(coll_stats.partial_2lp) + ")");

    // Emit BrentPollardRho stats when the ENV-gated path is enabled.
    // `tried==0` if either ENV is unset or every candidate took the SQUFOF
    // fast path; skip the line in those cases to avoid noise.
    if (cofactor::brent_pollard_enabled()) {
        const auto& bp_stats = cofactor::BrentPollardRho::global_stats();
        uint64_t tried = bp_stats.tried.load(std::memory_order_relaxed);
        if (tried > 0) {
            uint64_t succ = bp_stats.succ.load(std::memory_order_relaxed);
            uint64_t total_iter = bp_stats.total_iter.load(std::memory_order_relaxed);
            double avg_iter = static_cast<double>(total_iter) / static_cast<double>(tried);
            std::fprintf(stderr, "[brent_rho] tried=%llu succ=%llu avg_iter=%.1f\n",
                         static_cast<unsigned long long>(tried),
                         static_cast<unsigned long long>(succ), avg_iter);
        }
    }

    // BACKLOG #1: emit warning when sieve loop exits without sufficient usable
    // relations. Two cases: (a) the adaptive round limit was reached without
    // enough excess (β plateau
    // signature), (b) SQs exhausted before target met (sieve depth too small).
    // Phase 5 will then attempt BW thin solve on the under-built matrix.
    const size_t terminal_lp_cols = last_reduction->stats.output_lp_columns;
    if (!relation::has_effective_column_excess(last_reduction->size(), matrix_cols,
                                               terminal_lp_cols)) {
        const std::string_view stop_reason = sieve_stop_reason_name(stats_.sieve_stop_reason);
        std::fprintf(stderr,
                     "[sieve-warn] exit without excess: usable=%zu base_cols=%zu lp_cols=%zu, "
                     "stop=%.*s. Phase 5 will attempt BW thin solve.\n",
                     last_reduction->size(), matrix_cols, terminal_lp_cols,
                     static_cast<int>(stop_reason.size()), stop_reason.data());
        emit_log(LogLevel::Warn, Phase::Sieving,
                 "sieve exit without excess: stop=" + std::string(stop_reason));
    }
    emit_progress(Phase::Sieving, "Sieving complete", 1.0);

    // Adaptive lattice telemetry (only print when manager was enabled this run).
    if (adaptive_mgr.config().enabled) {
        auto al = adaptive_mgr.stats().snapshot();
        std::fprintf(stderr,
                     "[adaptive_lattice] special_qs=%llu retries=%llu rescues=%llu "
                     "low_density=%llu hits=%llu cells=%llu\n",
                     static_cast<unsigned long long>(al.special_qs_processed),
                     static_cast<unsigned long long>(al.retries_attempted),
                     static_cast<unsigned long long>(al.rescues_succeeded),
                     static_cast<unsigned long long>(al.low_density_skipped),
                     static_cast<unsigned long long>(al.total_hits),
                     static_cast<unsigned long long>(al.total_cells));
        emit_log(LogLevel::Info, Phase::Sieving,
                 "adaptive_lattice special_qs=" + std::to_string(al.special_qs_processed) +
                     " retries=" + std::to_string(al.retries_attempted) +
                     " rescues=" + std::to_string(al.rescues_succeeded));
    }

    std::optional<relation::RelationCorpus> terminal_raw_cleanup;
    if (verify_ooc_payload_after_callbacks &&
        last_reduction->storage_kind() == relation::RelationStorageKind::FinalizedOOC) {
        refresh_relation_corpus_checked(
            last_reduction->corpus, last_reduction->stats.output_digest,
            "structured OOC reduction changed after its terminal sieve snapshot");
    }
    if (structured_ooc_route || remove_legacy_terminal_raw_ooc) {
        // Keep the validated, finalized raw pair owned by the collector across
        // every user callback above. If a callback throws, unwinding removes
        // any provisional reduced output but leaves the authoritative raw
        // corpus for diagnosis or retry. Adoption happens only at this final
        // no-callback boundary; failure retains the collector's raw owner. The
        // legacy route enters this branch only through its explicit opt-in
        // cleanup policy. Cleanup is armed only after any paired checkpoint
        // has been removed successfully.
        terminal_raw_cleanup.emplace(collector.handoff_ooc_corpus(
            last_reduction->generation, relation::OOCCleanupPolicy::Preserve));
        if (terminal_raw_cleanup->storage_kind() != relation::RelationStorageKind::FinalizedOOC) {
            throw std::logic_error("terminal raw OOC handoff returned non-OOC storage");
        }
        if (!preserve_ooc_for_resume) {
            // With no paired checkpoint, this handoff is the last owner able to
            // remove a fresh raw pair if validation below throws.
            const bool armed = terminal_raw_cleanup->arm_ooc_cleanup();
            if (!armed && !recovered_finalized_ooc) {
                throw std::logic_error("fresh terminal raw OOC handoff lost cleanup ownership");
            }
            if (!armed) {
                std::fprintf(stderr, "[ooc] recovered terminal raw corpus has no durable cleanup "
                                     "ownership; preserving artifacts\n");
            }
        }
        if (verify_ooc_payload_after_callbacks && relation::corpus_digest(*terminal_raw_cleanup) !=
                                                      last_reduction->stats.raw_input_digest) {
            throw std::runtime_error(
                "terminal raw OOC corpus changed after its reduction snapshot");
        }
    }

    // Final no-callback commit. A checked removal failure leaves the
    // authoritative checkpoint and the preserve-policy raw corpus available.
    if (!sieve_resume_path.empty()) {
        sieve::SieveCheckpoint::remove_checked(
            sieve_resume_path + ".sieve_ckpt",
            terminal_checkpoint.has_value() ? &*terminal_checkpoint : nullptr);
        std::fprintf(stderr, "[sieve-resume] completed checkpoint removed\n");
    }

    if (terminal_raw_cleanup.has_value()) {
        // The storage kind was proven above and no operation can change it.
        // Arming performs no allocation or filesystem access.
        const bool armed = terminal_raw_cleanup->arm_ooc_cleanup();
        if (!armed && !recovered_finalized_ooc) {
            throw std::logic_error("fresh terminal raw OOC corpus lost cleanup ownership");
        }
        if (!armed) {
            std::fprintf(stderr,
                         "[ooc] recovered terminal raw corpus remains preserved because its "
                         "checkpoint did not carry deletion authority\n");
        }
    }
    stats_rollback.commit();
    return std::move(*last_reduction);
}

// ============================================================
// Phase 4: Filtering
// ============================================================

relation::RelationReductionResult Pipeline::filter(std::vector<Relation> relations) {
    // Validate this independent process gate before callbacks or relation
    // generation allocation, even though owned snapshots never publish the
    // direct-OOC stage record.
    (void)detail::parse_structured_filter_stage_telemetry(
        std::getenv("GNFS_STRUCTURED_FILTER_STAGE_TELEMETRY"));
    const auto structured_mode =
        relation::parse_structured_filter_mode(std::getenv("GNFS_STRUCTURED_FILTER"));
    const bool lp_enabled = params_.large_prime_bound > params_.algebraic_bound;
    const auto structured_policy = relation::decide_structured_filter_policy(
        structured_mode,
        relation::structured_filter_route_supported({.large_primes_enabled = lp_enabled}), false);

    emit_progress(Phase::Filtering, "Filtering relations");

    auto t0 = std::chrono::high_resolution_clock::now();

    std::optional<relation::V0BfsPolicy> v0_bfs_policy;
    if (lp_enabled) {
        v0_bfs_policy =
            relation::decide_v0_bfs_policy(std::getenv("GNFS_V0_BFS"), params_.large_prime_bound);
    }

    const bool v0_bfs_mode = v0_bfs_policy.has_value() && v0_bfs_policy->enabled;
    const bool use_v3 = lp_enabled && !v0_bfs_mode && cascade_v3_enabled();
    const auto legacy_strategy =
        !lp_enabled ? relation::ReductionStrategy::NoLargePrimes
                    : (v0_bfs_mode ? relation::ReductionStrategy::CliqueV0
                                   : (use_v3 ? relation::ReductionStrategy::StandardV0WithV3
                                             : relation::ReductionStrategy::StandardV0));
    relation::RelationReductionConfig reduction_config;
    reduction_config.filter.remove_singletons = true;
    reduction_config.filter.max_passes = 10;
    reduction_config.large_primes_enabled = lp_enabled;
    reduction_config.merge_rounds = 10;
    reduction_config.strategy =
        relation::select_reduction_strategy(structured_policy, legacy_strategy);
    if (reduction_config.strategy == relation::ReductionStrategy::Structured) {
        reduction_config.structured = relation::make_structured_filter_experimental_config(
            relations.size(), relation::structured_filter_hardware_workers());
    }

    std::optional<StructuredFilterRuntimeTelemetry> structured_telemetry;
    std::optional<util::ProcessMemorySnapshot> structured_memory_before;
    std::optional<std::chrono::steady_clock::time_point> structured_reduction_start;
    if (reduction_config.strategy == relation::ReductionStrategy::Structured) {
        structured_memory_before.emplace(util::process_memory_snapshot());
        structured_reduction_start.emplace(std::chrono::steady_clock::now());
    }
    auto reduction = relation::RelationReductionEngine::reduce(
        relation::RawRelationSnapshot(allocate_relation_generation(), std::move(relations)),
        reduction_config);
    if (structured_reduction_start.has_value()) {
        structured_telemetry.emplace(finish_structured_filter_telemetry(
            "owned_snapshot", *structured_reduction_start, std::move(*structured_memory_before)));
    }
    const auto& reduction_stats = reduction.stats;

    stats_.singletons_removed = reduction_stats.singleton_rows_removed;
    stats_.merged_relations = reduction_stats.merged_relations;

    if (reduction_config.strategy == relation::ReductionStrategy::Structured) {
        if (!structured_telemetry.has_value()) {
            throw std::logic_error("structured filter is missing runtime telemetry");
        }
        const std::string record = structured_filter_record(
            structured_policy, reduction, *reduction_config.structured, *structured_telemetry);
        emit_log(LogLevel::Info, Phase::Filtering, record);
        std::fprintf(stderr, "[%s]\n", record.c_str());
    } else if (lp_enabled) {
        // LP merge (only when LP is genuinely enabled on a legacy strategy)
        // BACKLOG #1 diagnostic: pre-merge LP-key weight histogram.
        // Plateau analysis hinges on weight distribution:
        //   weight=1 → singleton LP keys (will become LP cols, hurts β)
        //   weight=2 → V0 mergeable (standard PartialRelationMerger handles)
        //   weight≥3 → chain-merge territory (V0_BFS / V3 cascade only)
        const auto& pre_hist = reduction_stats.pre_merge_lp_histogram;
        emit_log(LogLevel::Info, Phase::Filtering,
                 "lp_weights pre-merge: unique=" + std::to_string(pre_hist.unique_keys) +
                     " w1=" + std::to_string(pre_hist.weight_1) +
                     " w2=" + std::to_string(pre_hist.weight_2) +
                     " w3=" + std::to_string(pre_hist.weight_3) +
                     " w4+=" + std::to_string(pre_hist.weight_4plus));
        std::fprintf(stderr, "[lp_weights] pre-merge: unique=%zu w1=%zu w2=%zu w3=%zu w4+=%zu\n",
                     pre_hist.unique_keys, pre_hist.weight_1, pre_hist.weight_2, pre_hist.weight_3,
                     pre_hist.weight_4plus);

        // ── V0 BFS chain merge (BACKLOG #1 step 12: size-aware default-ON) ──
        // V0 主路径用 BFS spanning tree (复用 CliqueRelationMerger 算法) 替代
        // standard Phase 1 + 2 simple match. weight≥3 LP keys 也走 chain merge.
        // 启用时 V3 cascade redundant (V0 already covers); skip V3 cascade.
        //
        // BACKLOG #1 step 11 empirical (PID 69073, 2026-05-18):
        //   50d Round 1 [lp_weights] w3+w4+ = 49% of LP keys. V0 standard misses
        //   half the LP graph. V0_BFS handles weight≥3 chains correctly.
        //
        // Size-aware default (decide_v0_bfs_policy in v0_bfs_policy.hpp):
        //   lp_bits ≥ 22 (50d+): default ON
        //   lp_bits <  22 (25d/81-bit): default OFF (BFS breaks small LP space)
        //   GNFS_V0_BFS=0 explicit opt-out (any size)
        //   GNFS_V0_BFS=1 explicit force-on (still falls back if lp_bits<22)
        if (v0_bfs_policy->env_force_failed) {
            std::fprintf(stderr, "[v0_bfs] %.*s\n", static_cast<int>(v0_bfs_policy->reason.size()),
                         v0_bfs_policy->reason.data());
        }

        if (v0_bfs_mode) {
            emit_log(LogLevel::Info, Phase::Filtering,
                     "v0_bfs (" + std::string(v0_bfs_policy->reason) +
                         "): full=" + std::to_string(reduction_stats.separated_full_relations) +
                         " " + reduction_stats.clique_v0.to_string() +
                         " merged=" + std::to_string(reduction_stats.merged_relations));
            std::fprintf(
                stderr, "[v0_bfs] reason=%.*s %s merged=%zu (V3 cascade skipped)\n",
                static_cast<int>(v0_bfs_policy->reason.size()), v0_bfs_policy->reason.data(),
                reduction_stats.clique_v0.to_string().c_str(), reduction_stats.merged_relations);

            // V3 cascade skipped — V0 BFS already covered weight≥3 chains.
            // Fall through to final stats/return.
            auto t1_bfs = std::chrono::high_resolution_clock::now();
            stats_.timings.filter_s = std::chrono::duration<double>(t1_bfs - t0).count();
            stats_.relations_after_filter = reduction_stats.output_relations;
            emit_log(LogLevel::Info, Phase::Filtering,
                     "after filter: " + std::to_string(reduction.size()) + " relations");
            emit_progress(Phase::Filtering, "Filtering complete", 1.0);
            return reduction;
        }

        emit_log(LogLevel::Info, Phase::Filtering,
                 "merge: full=" + std::to_string(reduction_stats.separated_full_relations) +
                     " 1lp=" + std::to_string(reduction_stats.standard_v0.input_1lp) +
                     " 2lp=" + std::to_string(reduction_stats.standard_v0.input_2lp) +
                     " merged=" + std::to_string(reduction_stats.standard_v0.output_relations));

        // ── V3 cascade (ENV: GNFS_CASCADE_V3=1) — runs AFTER V0 on partial copy ──
        // V0 handles weight=2 LP keys; V3 spans weight≥3 keys via BFS spanning tree.
        // Dedup: exact source combination — an equivalent V3 materialization is dropped.
        if (use_v3 && reduction_stats.separated_partial_relations > 0) {
            emit_log(LogLevel::Info, Phase::Filtering,
                     "v3_cascade: " + reduction_stats.v3.to_string() +
                         " added=" + std::to_string(reduction_stats.v3_relations_added) +
                         " dedup=" + std::to_string(reduction_stats.v3_duplicates_skipped));
            std::fprintf(stderr, "[v3_cascade.filter] %s added=%zu dedup=%zu\n",
                         reduction_stats.v3.to_string().c_str(), reduction_stats.v3_relations_added,
                         reduction_stats.v3_duplicates_skipped);
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.filter_s = std::chrono::duration<double>(t1 - t0).count();
    stats_.relations_after_filter = reduction_stats.output_relations;

    // BACKLOG #1 diagnostic: lp_cols breakdown at filter exit.
    // Caller (Phase 5 matrix builder) creates one column per odd-exp unique
    // LP key; emit count here so 50d/60d plateau analysis has empirical data.
    size_t lp_cols_after_filter = reduction_stats.output_lp_columns;
    emit_log(LogLevel::Info, Phase::Filtering,
             "after filter: " + std::to_string(reduction.size()) + " relations" +
                 " (lp_cols=" + std::to_string(lp_cols_after_filter) + ")");
    // stderr fallback for stress/progressive runs (no log_cb_ registered)
    std::fprintf(stderr, "[filter] after: rels=%zu lp_cols=%zu\n", reduction.size(),
                 lp_cols_after_filter);
    emit_progress(Phase::Filtering, "Filtering complete", 1.0);

    return reduction;
}

// ============================================================
// Phase 5: Linear Algebra
// ============================================================

Pipeline::MatrixResult Pipeline::build_matrix(relation::RelationReductionResult&& reduction,
                                              const FactorBase& fb, const PolynomialContext& ctx) {
    return matrix_phase(reduction, fb, ctx, false);
}

Pipeline::MatrixResult Pipeline::solve_matrix(relation::RelationReductionResult&& reduction,
                                              const FactorBase& fb, const PolynomialContext& ctx) {
    return matrix_phase(reduction, fb, ctx, true);
}

Pipeline::MatrixResult Pipeline::matrix_phase(relation::RelationReductionResult& reduction,
                                              const FactorBase& fb, const PolynomialContext& ctx,
                                              bool solve_dependencies) {
    require_pipeline_context(n_, ctx, solve_dependencies ? "solve_matrix" : "build_matrix");
    if (reduction.generation == 0 || !reduction.corpus.valid()) {
        throw std::invalid_argument("matrix phase requires a valid reduction owner");
    }
    if (reduction.corpus.logical_generation() != reduction.generation) {
        throw std::invalid_argument("matrix phase reduction generation does not match its corpus");
    }
    switch (reduction.stats.strategy) {
    case relation::ReductionStrategy::NoLargePrimes:
    case relation::ReductionStrategy::FilterOnly:
    case relation::ReductionStrategy::StandardV0:
    case relation::ReductionStrategy::StandardV0WithV3:
    case relation::ReductionStrategy::CliqueV0:
    case relation::ReductionStrategy::Structured:
        break;
    default:
        throw std::invalid_argument("matrix phase received an unknown reduction strategy");
    }
    if (reduction.stats.output_relations != reduction.size()) {
        throw std::invalid_argument(
            "matrix phase reduction output count does not match its corpus");
    }
    const bool structured_route =
        reduction.stats.strategy == relation::ReductionStrategy::Structured;
    if (!structured_route && reduction.storage_kind() != relation::RelationStorageKind::InMemory) {
        throw std::invalid_argument("legacy matrix route requires an in-memory reduction corpus");
    }
    const bool structured_ooc =
        structured_route && reduction.storage_kind() == relation::RelationStorageKind::FinalizedOOC;
    if (structured_ooc) {
        refresh_relation_corpus_checked(
            reduction.corpus, reduction.stats.output_digest,
            "structured OOC reduction does not match its matrix-phase digest");
    }
    const bool verify_ooc_payload_after_callbacks =
        structured_ooc && (static_cast<bool>(progress_cb_) || static_cast<bool>(log_cb_));
    FactorStatsRollback stats_rollback(stats_);
    static_assert(std::is_nothrow_move_assignable_v<Relation>);
    static_assert(std::is_nothrow_move_assignable_v<std::vector<Relation>>);

    emit_progress(Phase::LinearAlgebra, "Building matrix");
    if (verify_ooc_payload_after_callbacks) {
        refresh_relation_corpus_checked(
            reduction.corpus, reduction.stats.output_digest,
            "structured OOC reduction changed before the matrix build snapshot");
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    const relation::RelationCorpus* structured_corpus =
        structured_route ? &reduction.corpus : nullptr;
    const std::vector<Relation>* legacy_relations =
        structured_route ? nullptr : &reduction.corpus.borrow_in_memory();
    std::vector<size_t> legacy_trim_order;
    std::vector<Relation> legacy_result_slots;

    // Matrix builder config
    linalg::MatrixBuilderConfig mb_config;
    mb_config.include_sign_column = true;
    mb_config.include_qc_columns = true;
    mb_config.include_class_group = false;
    mb_config.include_schirokauer = true;
    mb_config.num_qc_primes = params_.num_qc_primes;
    mb_config.qc_prime_start = 100;
    mb_config.schirokauer_primes = {2}; // Only ℓ=2 for GF(2) matrix
    mb_config.verbose = false;

    linalg::MatrixBuilder mb(mb_config);

    // Streaming matrix build path (ENV GNFS_SGE_STREAMING).
    //   off / unset (default): existing vector path (zero regression)
    //   "1"  / "on"           : streaming MB over VectorRelationSource(relations)
    //   "auto"                : enable iff GNFS_OOC_RELATIONS / GNFS_SIEVE_RESUME
    //                           is set (these imply we already paid the OOC
    //                           cost upstream, so we want the matching matrix-
    //                           build RAM savings)
    // The streaming path produces a bit-for-bit identical MatrixBuildResult
    // (verified by tests/test_sge_streaming.cpp) — toggling it is safe.
    // RAM savings: the streaming MB never materializes an intermediate
    // vector copy of relations during the parallel row build (the input
    // vector itself still exists at this scope, but the trim path resizes
    // it in place which is the natural place to release memory).
    auto stream_env = std::getenv("GNFS_SGE_STREAMING");
    bool use_streaming_mb = false;
    if (stream_env != nullptr) {
        std::string val(stream_env);
        if (val == "1" || val == "on" || val == "true") {
            use_streaming_mb = true;
        } else if (val == "auto") {
            const char* ooc = std::getenv("GNFS_OOC_RELATIONS");
            // GNFS_RESUME / GNFS_SIEVE_RESUME both imply OOC streaming.
            const bool resume_active = !pipeline_resume_base_path().empty();
            if ((ooc != nullptr && std::string(ooc) == "1") || resume_active) {
                use_streaming_mb = true;
            }
        }
    }

    const auto initial_matrix_build_start = std::chrono::steady_clock::now();
    linalg::MatrixBuildResult build_result;
    if (structured_route) {
        build_result = mb.build_with_qc_streaming(*structured_corpus, fb, ctx);
    } else if (use_streaming_mb) {
        linalg::VectorRelationSource src(*legacy_relations);
        build_result = mb.build_with_qc_streaming(src, fb, ctx);
        std::fprintf(stderr, "[matrix-streaming] matrix built from vector source "
                             "(GNFS_SGE_STREAMING)\n");
    } else {
        build_result = mb.build_with_qc(*legacy_relations, fb, ctx);
    }
    uint64_t matrix_build_wall_us =
        elapsed_microseconds(initial_matrix_build_start, std::chrono::steady_clock::now());

    const auto finish_matrix_result =
        [&](std::vector<std::vector<bool>> dependencies) -> MatrixResult {
        if (verify_ooc_payload_after_callbacks) {
            refresh_relation_corpus_checked(
                reduction.corpus, reduction.stats.output_digest,
                "structured OOC reduction changed after the matrix build snapshot");
        }
        MatrixResult result;
        result.matrix = std::move(build_result.matrix);
        result.dependencies = std::move(dependencies);
        if (structured_route) {
            result.retain_structured_relations(std::move(reduction.corpus),
                                               std::move(build_result.row_to_relation));
        } else {
            auto owned_relations = std::move(reduction).take_relations();
            if (legacy_trim_order.empty()) {
                result.relations = std::move(owned_relations);
            } else {
                for (size_t row = 0; row < legacy_trim_order.size(); ++row) {
                    legacy_result_slots[row] = std::move(owned_relations[legacy_trim_order[row]]);
                }
                result.relations = std::move(legacy_result_slots);
            }
        }
        stats_rollback.commit();
        return result;
    };

    auto matrix_stats = linalg::compute_matrix_stats(build_result.matrix);
    stats_.matrix_rows = matrix_stats.num_rows;
    stats_.matrix_cols = matrix_stats.num_cols;
    stats_.matrix_weight = matrix_stats.total_weight;
    stats_.matrix_excess = signed_size_delta(matrix_stats.num_rows, matrix_stats.num_cols);

    const auto emit_structured_matrix_record = [&](const linalg::MatrixStats& final_stats) {
        if (reduction.stats.strategy != relation::ReductionStrategy::Structured) {
            return;
        }
        const std::string record =
            "structured_filter_matrix generation=" + std::to_string(reduction.generation) +
            " rows=" + std::to_string(final_stats.num_rows) +
            " cols=" + std::to_string(final_stats.num_cols) +
            " excess=" + std::to_string(final_stats.excess) + " row_column_delta=" +
            std::to_string(signed_size_delta(final_stats.num_rows, final_stats.num_cols)) +
            " matrix_build_wall_us=" + std::to_string(matrix_build_wall_us) +
            " nonzeros=" + std::to_string(final_stats.total_weight);
        emit_log(LogLevel::Info, Phase::LinearAlgebra, record);
        std::fprintf(stderr, "[%s]\n", record.c_str());
    };

    emit_log(LogLevel::Info, Phase::LinearAlgebra,
             "matrix: " + std::to_string(matrix_stats.num_rows) + "x" +
                 std::to_string(matrix_stats.num_cols) +
                 " excess=" + std::to_string(matrix_stats.excess));

    // BACKLOG #1 diagnostic (F.1): row/col weight distribution. Reveals
    // sieve gap (empty cols) and SGE-eliminable garbage (singleton cols/rows)
    // before BL/BW kicks in. Cost: one full nnz scan (~50ms at 50d, < 1% of
    // Phase 5 wall-clock).
    {
        const auto diag = linalg::compute_matrix_diagnostics(build_result.matrix);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "[mat-diag] rows: empty=%zu singleton=%zu w_range=[%zu,%zu] avg=%.2f"
                      " | cols: empty=%zu singleton=%zu low(2-4)=%zu max_w=%zu avg=%.2f",
                      diag.empty_rows, diag.singleton_rows, diag.min_row_weight,
                      diag.max_row_weight, matrix_stats.avg_row_weight, diag.empty_cols,
                      diag.singleton_cols, diag.low_weight_cols, diag.max_col_weight,
                      diag.avg_col_weight);
        emit_log(LogLevel::Info, Phase::LinearAlgebra, std::string(buf));
        std::fprintf(stderr, "%s\n", buf);
    }

    if (solve_dependencies && !matrix_stats.has_excess()) {
        // BACKLOG #80 step 7 (2026-05-17): thin matrix (m ≤ n) now solved by
        // block_wiedemann_thin_solve, which uses B'=M^T·M and recovers via
        // u=M·w (strict over GF(2) by associativity). Works on realistic
        // GNFS profile (rank ≈ m), fails gracefully (returns empty) on
        // pathological rank≪m case. Always attempt; no ENV gate needed.
        emit_log(LogLevel::Warn, Phase::LinearAlgebra,
                 "No excess (m ≤ n) — attempting BW thin solve (B'=M^T·M variant)");

        // Opt-out for users wanting prior "abort on no excess" behavior.
        const char* e = std::getenv("GNFS_NO_THIN_SOLVE");
        if (e != nullptr && std::string(e) == "1") {
            emit_log(LogLevel::Error, Phase::LinearAlgebra,
                     "GNFS_NO_THIN_SOLVE=1 — aborting on no excess");
            emit_structured_matrix_record(matrix_stats);
            return finish_matrix_result({});
        }
    }

    // Trim excess rows to improve BL convergence and SGE effectiveness.
    // High excess (>1.3×) causes: (1) BL A-gram persistent non-invertible columns
    // (E/F corrections can only look back 2 steps, so persistent rank deficiency
    // causes orthogonality breakdown); (2) SGE ineffectiveness (avg column weight
    // is too high for w1/w2 elimination).
    // Target: 1.1× cols for optimal SGE + BL. CADO-NFS typically uses 5-10% excess.
    if (matrix_stats.num_rows > scale_by_tenths_floor(matrix_stats.num_cols, 13)) {
        const size_t target_rows = scale_by_tenths_floor(matrix_stats.num_cols, 11);
        emit_log(LogLevel::Info, Phase::LinearAlgebra,
                 "Trimming excess: " + std::to_string(matrix_stats.num_rows) + " rows -> " +
                     std::to_string(target_rows) + " (keep " + std::to_string(target_rows) + "/" +
                     std::to_string(matrix_stats.num_rows) + ")");
        if (verify_ooc_payload_after_callbacks) {
            refresh_relation_corpus_checked(
                reduction.corpus, reduction.stats.output_digest,
                "structured OOC reduction changed before the trimmed matrix snapshot");
        }

        const auto trim_matrix_build_start = std::chrono::steady_clock::now();
        linalg::MatrixBuildResult build2;
        if (structured_route) {
            const auto selection = relation::RelationSelection::deterministic_sample(
                *structured_corpus, target_rows, uint64_t{42});
            const linalg::RelationSelectionSource source(*structured_corpus, selection);
            build2 = mb.build_with_qc_streaming(source, fb, ctx);
        } else {
            // Preserve the legacy vector route and its established shuffle,
            // but leave the caller-owned reduction unchanged until every
            // matrix operation and callback succeeds.
            legacy_trim_order.resize(legacy_relations->size());
            std::iota(legacy_trim_order.begin(), legacy_trim_order.end(), size_t{0});
            std::mt19937 rng(42);
            std::shuffle(legacy_trim_order.begin(), legacy_trim_order.end(), rng);
            legacy_trim_order.resize(target_rows);

            std::vector<Relation> trimmed_relations;
            trimmed_relations.reserve(target_rows);
            for (size_t ordinal : legacy_trim_order) {
                trimmed_relations.push_back((*legacy_relations)[ordinal]);
            }

            // SGE-OOC: rebuild via streaming MB if enabled (same gate as
            // initial build above so the trim path is consistent).
            if (use_streaming_mb) {
                linalg::VectorRelationSource src(trimmed_relations);
                build2 = mb.build_with_qc_streaming(src, fb, ctx);
            } else {
                build2 = mb.build_with_qc(trimmed_relations, fb, ctx);
            }

            // Reserve the exact final ownership payload before any subsequent
            // user callback. The final commit then consists only of noexcept
            // Relation moves into these preallocated slots.
            legacy_result_slots.resize(target_rows);
        }
        matrix_build_wall_us = checked_add_u64(
            matrix_build_wall_us,
            elapsed_microseconds(trim_matrix_build_start, std::chrono::steady_clock::now()));
        build_result = std::move(build2);
        matrix_stats = linalg::compute_matrix_stats(build_result.matrix);
        stats_.matrix_rows = matrix_stats.num_rows;
        stats_.matrix_cols = matrix_stats.num_cols;
        stats_.matrix_weight = matrix_stats.total_weight;
        stats_.matrix_excess = signed_size_delta(matrix_stats.num_rows, matrix_stats.num_cols);

        emit_log(LogLevel::Info, Phase::LinearAlgebra,
                 "Trimmed matrix: " + std::to_string(matrix_stats.num_rows) + "x" +
                     std::to_string(matrix_stats.num_cols) +
                     " excess=" + std::to_string(matrix_stats.excess));

        // Re-emit mat-diag after trim — col-weight distribution changes
        // because some cols lose all support when their rows were dropped.
        {
            const auto diag2 = linalg::compute_matrix_diagnostics(build_result.matrix);
            char buf[512];
            std::snprintf(
                buf, sizeof(buf),
                "[mat-diag post-trim] rows: empty=%zu singleton=%zu w_range=[%zu,%zu] avg=%.2f"
                " | cols: empty=%zu singleton=%zu low(2-4)=%zu max_w=%zu avg=%.2f",
                diag2.empty_rows, diag2.singleton_rows, diag2.min_row_weight, diag2.max_row_weight,
                matrix_stats.avg_row_weight, diag2.empty_cols, diag2.singleton_cols,
                diag2.low_weight_cols, diag2.max_col_weight, diag2.avg_col_weight);
            emit_log(LogLevel::Info, Phase::LinearAlgebra, std::string(buf));
            std::fprintf(stderr, "%s\n", buf);
        }
    }

    // This is the final full matrix handed to SGE and retained in
    // MatrixResult. Emit after deterministic trimming so the stable record is
    // never a stale pre-trim snapshot.
    emit_structured_matrix_record(matrix_stats);

    if (!solve_dependencies) {
        stats_.dependencies_found = 0;
        const auto t1 = std::chrono::high_resolution_clock::now();
        stats_.timings.linalg_s = std::chrono::duration<double>(t1 - t0).count();
        emit_log(LogLevel::Info, Phase::LinearAlgebra,
                 "matrix build complete: rows=" + std::to_string(matrix_stats.num_rows) +
                     " cols=" + std::to_string(matrix_stats.num_cols) +
                     " time=" + std::to_string(stats_.timings.linalg_s) + "s");
        emit_progress(Phase::LinearAlgebra, "Matrix build complete", 1.0);
        return finish_matrix_result({});
    }

    // SGE preprocessing
    emit_progress(Phase::LinearAlgebra, "SGE preprocessing");
    linalg::SGEConfig sge_config;
    sge_config.verbose = false;
    auto sge_result = linalg::SGE::preprocess(build_result.matrix, sge_config);

    {
        const size_t pre_rows = build_result.matrix.num_rows();
        const size_t pre_cols = build_result.matrix.num_cols();
        const size_t post_rows = sge_result.reduced_matrix.num_rows();
        const size_t post_cols = sge_result.reduced_matrix.num_cols();
        const double reduce_pct =
            (pre_rows == 0 || pre_cols == 0)
                ? 0.0
                : 100.0 * (1.0 - static_cast<double>(post_rows * post_cols) /
                                     static_cast<double>(pre_rows * pre_cols));
        char buf[256];
        std::snprintf(buf, sizeof(buf), "SGE: %zux%zu -> %zux%zu (reduce=%.1f%% area)", pre_rows,
                      pre_cols, post_rows, post_cols, reduce_pct);
        // Promote to Info — SGE reduction is a key diagnostic for BACKLOG #1
        // 50d empirical (CLAUDE.md cites 30-60% reduction expectation).
        emit_log(LogLevel::Info, Phase::LinearAlgebra, std::string(buf));
        std::fprintf(stderr, "[sge] %s\n", buf);
    }

    // For thin matrices (rows ≤ cols, BACKLOG #80), BL is known to fail —
    // skip directly to BW. BW finds left kernel via B=M*M^T which works
    // for any m×n matrix when rank-deficient.
    const auto& sge_red = sge_result.reduced_matrix;

    // BACKLOG #1 (F.1 follow-up): emit post-SGE mat-diag. The reduced matrix
    // is what BL/BW actually consumes; its col-weight distribution determines
    // whether BL has any chance to converge. SGE peels off singleton/low-weight
    // cols, so post-SGE singleton/low_weight counts should be ~0 in a healthy
    // pipeline — if they stay > 0 SGE is being defeated by chain residue.
    {
        const auto diag_sge = linalg::compute_matrix_diagnostics(sge_red);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "[mat-diag post-sge] rows: empty=%zu singleton=%zu w_range=[%zu,%zu]"
                      " | cols: empty=%zu singleton=%zu low(2-4)=%zu max_w=%zu avg=%.2f",
                      diag_sge.empty_rows, diag_sge.singleton_rows, diag_sge.min_row_weight,
                      diag_sge.max_row_weight, diag_sge.empty_cols, diag_sge.singleton_cols,
                      diag_sge.low_weight_cols, diag_sge.max_col_weight, diag_sge.avg_col_weight);
        emit_log(LogLevel::Info, Phase::LinearAlgebra, std::string(buf));
        std::fprintf(stderr, "%s\n", buf);
    }

    const bool sge_thin = sge_red.num_rows() <= sge_red.num_cols();
    std::vector<std::vector<bool>> dependencies;

    if (!sge_thin) {
        // Block Lanczos (primary solver)
        emit_progress(Phase::LinearAlgebra, "Block Lanczos");
        linalg::BlockLanczos bl_solver;
        dependencies = bl_solver.find_dependencies(sge_red);
    } else {
        emit_log(LogLevel::Warn, Phase::LinearAlgebra,
                 "SGE-reduced matrix is thin (" + std::to_string(sge_red.num_rows()) +
                     "<=" + std::to_string(sge_red.num_cols()) + ") — skip BL, try BW directly");
    }

    // If BL didn't find deps, or matrix is thin, use streaming Block Wiedemann.
    // BW works for any matrix size with O(m) memory.
    if (dependencies.empty()) {
        if (!sge_thin) {
            emit_log(LogLevel::Warn, Phase::LinearAlgebra,
                     "Block Lanczos returned 0 deps, trying Block Wiedemann");
        }
        emit_progress(Phase::LinearAlgebra, "Block Wiedemann");
        linalg::BlockWiedemann bw_solver;

        // BACKLOG: MmapCSRMatrix Phase 5 integration (CLAUDE.md
        // "Known Limitations" lifted by this commit). Three-state ENV:
        //   GNFS_LINALG_MMAP=off (default): in-memory CSR — today's path.
        //   GNFS_LINALG_MMAP=on            : force MmapCSRMatrix route.
        //   GNFS_LINALG_MMAP=auto          : flip when projected
        //     col_indices bytes ≥ GNFS_LINALG_MMAP_THRESHOLD_BYTES
        //     (default 2 GiB ≈ 500M nnz).
        const linalg::MmapPolicy policy = linalg::linalg_mmap_policy_from_env();
        const std::uint64_t sge_nnz = sge_red.total_weight();
        const bool use_mmap = linalg::should_use_mmap(policy, sge_nnz);

        if (use_mmap) {
            // Disk-resident path. Persist SGE-reduced matrix as a
            // .csrmat file (v2 layout, uint64_t row_offsets), open it
            // as MmapCSRMatrix, and route through the view-based BW
            // entry point that bypasses SparseMatrix internally.
            const std::string mmap_path = gnfs::util::temp_path(
                "gnfs_linalg_" + std::to_string(gnfs::util::process_id()) + ".csrmat");
            char log_buf[512];
            std::snprintf(log_buf, sizeof(log_buf), "[linalg-mmap] policy=%s nnz=%llu path=%s",
                          policy == linalg::MmapPolicy::On ? "on" : "auto",
                          static_cast<unsigned long long>(sge_nnz), mmap_path.c_str());
            emit_log(LogLevel::Info, Phase::LinearAlgebra, std::string(log_buf));
            std::fprintf(stderr, "%s\n", log_buf);

            try {
                linalg::MmapCSRMatrix mmap_csr = linalg::save_sparse_as_mmap(sge_red, mmap_path);
                dependencies = bw_solver.find_dependencies_view(mmap_csr);
            } catch (const std::exception& ex) {
                // mmap path failed (disk full / permission / corruption):
                // fall back to in-memory BW so the pipeline still makes
                // progress. Loud log so the operator can fix the disk.
                emit_log(LogLevel::Warn, Phase::LinearAlgebra,
                         std::string("[linalg-mmap] fallback to in-memory: ") + ex.what());
                std::fprintf(stderr, "[linalg-mmap] fallback to in-memory: %s\n", ex.what());
                dependencies = bw_solver.find_dependencies(sge_red);
            }

            // Best-effort cleanup; ok if already gone.
            std::remove(mmap_path.c_str());
        } else {
            // Default in-memory path — bit-identical to pre-Phase-5 behaviour.
            dependencies = bw_solver.find_dependencies(sge_red);
        }
    }

    // Expand all dependencies back to original matrix after validating the
    // complete SGE provenance transform exactly once for this solver batch.
    dependencies = detail::expand_solver_dependencies_checked(sge_result, dependencies,
                                                              build_result.matrix.num_rows());

    stats_.dependencies_found = dependencies.size();

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.timings.linalg_s = std::chrono::duration<double>(t1 - t0).count();

    emit_log(LogLevel::Info, Phase::LinearAlgebra,
             "deps=" + std::to_string(dependencies.size()) +
                 " time=" + std::to_string(stats_.timings.linalg_s) + "s");
    emit_progress(Phase::LinearAlgebra, "Linear algebra complete", 1.0);

    return finish_matrix_result(std::move(dependencies));
}

// ============================================================
// Phase 6+7: Square Root and Factor Extraction
// ============================================================

// Helper: convert vector<bool> to BitVector
static linalg::BitVector to_bitvector(const std::vector<bool>& vec) {
    linalg::BitVector bv(vec.size());
    for (size_t i = 0; i < vec.size(); ++i) {
        if (vec[i])
            bv.set(i);
    }
    return bv;
}

// Helper: verify dependency (XOR of selected rows = zero)
// Uses uint8_t per-column parity (XOR), 8× more cache-friendly than size_t counter.
static bool verify_dependency(const SparseMatrix& mat, const std::vector<bool>& dep) {
    if (dep.size() != mat.num_rows())
        return false;
    std::vector<uint8_t> col_parity(mat.num_cols(), 0);
    for (size_t row = 0; row < mat.num_rows(); ++row) {
        if (row < dep.size() && dep[row]) {
            for (uint32_t col : mat.row(row).indices()) {
                col_parity[col] ^= 1; // XOR in GF(2)
            }
        }
    }
    for (size_t c = 0; c < col_parity.size(); ++c) {
        if (col_parity[c])
            return false;
    }
    return true;
}

FactorResult Pipeline::extract_factors(const MatrixResult& mr, const FactorBase& fb,
                                       const PolynomialContext& ctx) {
    require_pipeline_context(n_, ctx, "extract_factors");
    emit_progress(Phase::SquareRoot, "Starting factor extraction");

    auto t0_sqrt = std::chrono::high_resolution_clock::now();

    FactorResult result;
    result.n = n_; // Integer op=
    result.stats = stats_;
    result.factors.reserve(2); // success path pushes 2 factors

    if (mr.dependencies.empty()) {
        emit_log(LogLevel::Error, Phase::SquareRoot, "No dependencies to try");
        result.stats.timings.total_s = elapsed_s();
        return result;
    }

    auto is_nontrivial = [this](const Integer& f) -> bool {
        if (f.fits_uint64() && f.to_uint64() == 1)
            return false;
        if (f.compare(n_) == 0)
            return false;
        return true;
    };

    auto try_factor = [&](const Integer& rat_sqrt, const Integer& alg_value) -> bool {
        auto factors = sqrt::extract_factors(rat_sqrt, alg_value, n_);

        if (is_nontrivial(factors.factor1)) {
            Integer f1 = factors.factor1; // copy ctor
            Integer f2 = n_;
            f2 /= f1;
            Integer check = f1;
            check *= f2;
            if (check.compare(n_) == 0 && is_nontrivial(f2)) {
                result.factors.push_back(std::move(f1));
                result.factors.push_back(std::move(f2));
                result.success = true;
                return true;
            }
        }
        if (is_nontrivial(factors.factor2)) {
            Integer f1 = factors.factor2; // copy ctor
            Integer f2 = n_;
            f2 /= f1;
            Integer check = f1;
            check *= f2;
            if (check.compare(n_) == 0 && is_nontrivial(f2)) {
                result.factors.push_back(std::move(f1));
                result.factors.push_back(std::move(f2));
                result.success = true;
                return true;
            }
        }
        return false;
    };

    const relation::RelationCorpus* structured_corpus = nullptr;
    std::span<const size_t> structured_row_to_relation;
    if (mr.owns_relation_corpus()) {
        structured_corpus = &mr.structured_corpus();
        structured_row_to_relation = mr.structured_row_to_relation();
    }

    // A malformed dependency length is a provenance error, not a failed
    // mathematical dependency. Reject it before any row access or square-root
    // materialization, including the XOR-pair path below.
    for (const auto& dependency : mr.dependencies) {
        if (dependency.size() != mr.matrix.num_rows()) {
            throw std::invalid_argument(
                "Pipeline::extract_factors: dependency length does not match matrix rows");
        }
    }

    const auto try_relation_dependency =
        [&](const linalg::BitVector& dependency,
            const std::vector<Relation>& dependency_relations) -> bool {
        auto rat_result =
            sqrt::compute_rational_sqrt(dependency, dependency_relations, fb, n_, ctx.m());
        if (!rat_result.success) {
            return false;
        }

        auto alg_result = sqrt::compute_algebraic_sqrt(dependency, dependency_relations, ctx);
        Integer alg_value = alg_result.success ? alg_result.value : Integer(1); // copy ctor

        if (try_factor(rat_result.value, alg_value)) {
            return true;
        }

        // Try -Y — mpz_sub writes n_ - alg_value directly (skip clone+ -=).
        Integer alg_neg;
        mpz_sub(alg_neg.get_mpz(), n_.get_mpz(), alg_value.get_mpz());
        return try_factor(rat_result.value, alg_neg);
    };

    const auto try_matrix_dependency = [&](const std::vector<bool>& dependency) -> bool {
        // Always validate in matrix coordinates before mapping or materializing.
        if (!verify_dependency(mr.matrix, dependency)) {
            return false;
        }

        if (structured_corpus == nullptr) {
            return try_relation_dependency(to_bitvector(dependency), mr.relations);
        }

        const auto selection = linalg::dependency_to_relation_selection(
            *structured_corpus, structured_row_to_relation, dependency);
        auto selected_relations = relation::materialize_selected(*structured_corpus, selection);

        // The selected vector contains exactly this dependency, so the local
        // square-root coordinate system selects every materialized relation.
        linalg::BitVector local_dependency(selected_relations.size());
        for (size_t index = 0; index < selected_relations.size(); ++index) {
            local_dependency.set(index);
        }
        return try_relation_dependency(local_dependency, selected_relations);
    };

    // Try each dependency
    for (size_t dep_idx = 0; dep_idx < mr.dependencies.size() && !result.success; ++dep_idx) {
        const auto& dep = mr.dependencies[dep_idx];
        stats_.dependencies_tried = dep_idx + 1;

        emit_progress(Phase::SquareRoot,
                      "Trying dependency " + std::to_string(dep_idx + 1) + "/" +
                          std::to_string(mr.dependencies.size()),
                      static_cast<double>(dep_idx) / static_cast<double>(mr.dependencies.size()));

        if (try_matrix_dependency(dep)) {
            break;
        }
    }

    // If no single dep worked, try XOR pairs
    if (!result.success && mr.dependencies.size() >= 2) {
        emit_progress(Phase::FactorExtraction, "Trying XOR combinations");

        size_t limit = std::min(mr.dependencies.size(), size_t(20));
        for (size_t i = 0; i < limit && !result.success; ++i) {
            for (size_t j = i + 1; j < limit && !result.success; ++j) {
                auto combined = detail::xor_dependency_pair_checked(
                    mr.dependencies[i], mr.dependencies[j], mr.matrix.num_rows());
                if (!combined) {
                    continue;
                }

                // XOR is formed and verified in matrix coordinates; the
                // structured route maps and materializes the result once.
                if (try_matrix_dependency(*combined)) {
                    break;
                }
            }
        }
    }

    auto t1_sqrt = std::chrono::high_resolution_clock::now();
    stats_.timings.sqrt_s = std::chrono::duration<double>(t1_sqrt - t0_sqrt).count();

    // Sort factors ascending
    if (result.factors.size() == 2 && result.factors[0].compare(result.factors[1]) > 0) {
        std::swap(result.factors[0], result.factors[1]);
    }

    result.stats = stats_;
    result.stats.timings.total_s = elapsed_s();

    if (result.success) {
        emit_log(LogLevel::Info, Phase::FactorExtraction,
                 "SUCCESS: " + result.factors[0].to_string() + " * " +
                     result.factors[1].to_string());
    } else {
        emit_log(LogLevel::Warn, Phase::FactorExtraction, "No non-trivial factor found");
    }
    emit_progress(Phase::Done, result.success ? "Factorization succeeded" : "Failed", 1.0);

    return result;
}

// ============================================================
// Run: complete pipeline
// ============================================================

FactorResult Pipeline::run() {
    // Validate the relation-filter route before any progress/log callback,
    // checkpoint read/write, fast-method probe, or generation allocation. The
    // strict flag is a process configuration error even when a fast factor
    // would otherwise let this invocation avoid the GNFS phases.
    const auto structured_route = capture_structured_route_snapshot();

    // Input validation.
    // Adaptive Miller-Rabin reps: 5 for small N (fast on trial-division path),
    // 15 for large N (target 2^-30 error rate for crypto-grade composites).
    const int prime_reps = (stats_.n_bits <= 64) ? 5 : 15;
    if (mpz_probab_prime_p(n_.get_mpz(), prime_reps) > 0) {
        emit_log(LogLevel::Error, Phase::PolynomialSelection, "N is prime or probably prime");
        FactorResult r;
        r.n = n_; // Integer op=
        r.stats = stats_;
        r.stats.timings.total_s = elapsed_s();
        return r;
    }
    if (mpz_cmp_si(n_.get_mpz(), 1) <= 0) {
        FactorResult r;
        r.n = n_; // Integer op=
        r.stats = stats_;
        return r;
    }

    // Check for perfect power
    if (mpz_perfect_power_p(n_.get_mpz())) {
        // exp upper bound: for any perfect power n = b^e with b ≥ 2,
        // we have e ≤ log2(n) = n_bits. Capping at 64 missed N = 2^65 etc.
        const unsigned long exp_max = static_cast<unsigned long>(stats_.n_bits);
        for (unsigned long exp = 2; exp <= exp_max; ++exp) {
            Integer root;
            if (mpz_root(root.get_mpz(), n_.get_mpz(), exp)) {
                Integer check;
                mpz_pow_ui(check.get_mpz(), root.get_mpz(), exp);
                if (check.compare(n_) == 0) {
                    FactorResult r;
                    r.n = n_; // Integer op=
                    r.success = true;
                    r.factors.reserve(2);
                    r.factors.push_back(root); // Integer copy ctor
                    r.factors.push_back(n_);

                    r.factors[1] /= root;
                    r.stats = stats_;
                    r.stats.timings.total_s = elapsed_s();
                    r.stats.method_used = FactorizationMethod::TrialDivision;
                    r.stats.method_reason = "perfect power";
                    emit_log(LogLevel::Info, Phase::PolynomialSelection,
                             "Perfect power detected: " + root.to_string() + "^" +
                                 std::to_string(exp));
                    return r;
                }
            }
        }
    }

    // ── Method selection ──
    auto [method, reason] = select_method(stats_.n_bits, stats_.n_digits, config_.method);
    stats_.method_used = method;
    stats_.method_reason = reason;
    emit_log(LogLevel::Info, Phase::PolynomialSelection,
             "Method: " + std::string(method_name(method)) + " (" + reason + ")");

    // Helper: build result from a found factor
    auto make_fast_result = [this](const Integer& f1, FactorizationMethod m,
                                   const std::string& m_reason) -> FactorResult {
        FactorResult r;
        r.n = n_; // Integer op=
        r.success = true;
        Integer f2 = n_; // Integer copy ctor
        f2 /= f1;
        if (f1.compare(f2) <= 0) {
            r.factors.push_back(f1); // Integer copy ctor
            r.factors.push_back(std::move(f2));
        } else {
            r.factors.push_back(f2);
            r.factors.push_back(f1);
        }
        r.stats = stats_;
        r.stats.method_used = m;
        r.stats.method_reason = m_reason;
        r.stats.timings.total_s = elapsed_s();
        return r;
    };

    // ── Phase 0: Trial division (always, instant) ──
    // For ≤12d: thorough trial to 10^6 (factors might be 6-digit).
    // For >12d: quick trial to 10^4 only (rho/SIQS catches the rest).
    // Saves ~3ms of overhead for medium/large N.
    {
        uint64_t td_limit = (stats_.n_digits <= 12) ? 1000000 : 10000;
        uint64_t small_f = trial_divide(n_, td_limit);
        if (small_f > 0) {
            Integer f1(small_f);
            emit_log(LogLevel::Info, Phase::PolynomialSelection,
                     "Trial division found factor: " + std::to_string(small_f));
            return make_fast_result(f1, FactorizationMethod::TrialDivision, "factor ≤ 10^6");
        }
    }

    // If user forced trial-only, stop here
    if (method == FactorizationMethod::TrialDivision) {
        FactorResult r;
        r.n = n_; // Integer op=
        r.stats = stats_;
        r.stats.timings.total_s = elapsed_s();
        return r;
    }

    // ── Phase 1: Pollard rho for small N or quick unbalanced detection ──
    // For N ≤ 128 bits: use mpn2 rho (~12ns/iter).
    // For N > 128 bits: quick GMP rho probe (50K iters) for unbalanced semiprimes.
    // Skip entirely for ≥40d/≥128bit when SIQS is the target (saves ~30ms overhead).
    if (method == FactorizationMethod::PollardRho ||
        ((method == FactorizationMethod::SIQS || method == FactorizationMethod::GNFS) &&
         stats_.n_bits <= 128)) {

        // Fast mpn-based rho: N ≤ 128 bits (~38 digits)
        // Uses GMP mpn_ assembly (no mpz_t overhead): ~8-12ns/iter vs ~30ns/iter
        // Iteration budget: O(p^{1/2}) where p is smallest factor.
        // For balanced k-digit semiprime, p ≈ 10^{k/2}, so iters ≈ 10^{k/4}.
        // Keep budget modest — ECM handles what rho misses.
        if (stats_.n_bits <= 128) {
            // Quick rho probe: catch easy/unbalanced factors.
            // For balanced semiprimes, ECM is usually faster.
            // Budget: generous when rho is target method, minimal when SIQS will handle it.
            // For balanced semiprimes ≥25d, rho needs O(10^{d/4}) iters — too slow.
            // Keep budget small to catch unbalanced cases only.
            size_t rho_limit;
            bool siqs_target =
                (method == FactorizationMethod::SIQS || method == FactorizationMethod::GNFS);
            if (stats_.n_bits <= 40)
                rho_limit = 100000; // ~1ms
            else if (stats_.n_bits <= 50)
                rho_limit = siqs_target ? 100000 : 200000;
            else if (stats_.n_bits <= 64)
                rho_limit = siqs_target ? 200000 : 500000;
            else if (stats_.n_bits <= 80)
                rho_limit = siqs_target ? 100000 : 1000000;
            else
                rho_limit = siqs_target ? 50000 : 1000000;

            uint64_t f128 = pollard_rho_mpn2(n_, rho_limit);
            if (f128 > 1) {
                Integer f1(f128);
                if (f1.compare(n_) != 0) {
                    emit_log(LogLevel::Info, Phase::PolynomialSelection,
                             "mpn2 rho found factor: " + std::to_string(f128));
                    return make_fast_result(f1, FactorizationMethod::PollardRho,
                                            "mpn2 rho (≤128bit)");
                }
            }
        }

        // GMP rho: only when user explicitly forced PollardRho method
        if (method == FactorizationMethod::PollardRho) {
            Integer rho_f = pollard_rho_brent(n_, 100000000);
            if (mpz_cmp_si(rho_f.get_mpz(), 1) > 0 && rho_f.compare(n_) != 0) {
                emit_log(LogLevel::Info, Phase::PolynomialSelection,
                         "Pollard rho found factor: " + rho_f.to_string());
                return make_fast_result(rho_f, FactorizationMethod::PollardRho, "GMP rho fallback");
            }
        }
    }

    // If user forced rho-only, stop here
    if (method == FactorizationMethod::PollardRho) {
        FactorResult r;
        r.n = n_; // Integer op=
        r.stats = stats_;
        r.stats.timings.total_s = elapsed_s();
        return r;
    }

    // ── Phase 1.5: ECM for medium N (factor-size dependent) ──
    // ECM complexity depends on smallest factor p, not N.
    // For balanced k-digit semiprimes, p ≈ k/2 digits.
    // ECM with appropriate B1 finds factors up to ~35 digits efficiently.
    // Run ECM before SIQS for N ≤ 100 digits (factors ≤ ~50 digits).
    // ECM probe: minimal probe for 25-28d to catch unbalanced semiprimes.
    // For balanced semiprimes, ECM is too slow — SIQS handles them faster.
    // Keep ECM cost ≤ 2ms total to minimize overhead.
    if (stats_.n_digits >= 26 && stats_.n_digits <= 28 &&
        method != FactorizationMethod::TrialDivision) {
        size_t expected_factor_bits = stats_.n_bits / 2;

        // Minimal ECM probe: 3 curves at low B1. Cost: ~1ms total.
        cofactor::ECM::Config ecm_config;
        ecm_config.auto_params = false;
        if (expected_factor_bits <= 50) {
            ecm_config.B1 = 2000;
            ecm_config.B2 = 50000;
            ecm_config.num_curves = 3;
        } else {
            ecm_config.B1 = 0;
            ecm_config.num_curves = 0;
        }

        // Skip ECM if configured with 0 curves
        if (ecm_config.num_curves > 0) {
            emit_log(LogLevel::Info, Phase::PolynomialSelection,
                     "ECM probe: " + std::to_string(stats_.n_digits) +
                         "d N, "
                         "expected factor ~" +
                         std::to_string(expected_factor_bits) +
                         " bits, "
                         "B1=" +
                         std::to_string(ecm_config.B1) +
                         " curves=" + std::to_string(ecm_config.num_curves));

            auto ecm_t0 = std::chrono::high_resolution_clock::now();
            auto ecm_f = cofactor::ECM::factor(n_, ecm_config);
            auto ecm_t1 = std::chrono::high_resolution_clock::now();
            double ecm_ms = std::chrono::duration<double, std::milli>(ecm_t1 - ecm_t0).count();

            if (ecm_f && mpz_cmp_si(ecm_f->get_mpz(), 1) > 0 && ecm_f->compare(n_) != 0) {
                emit_log(LogLevel::Info, Phase::PolynomialSelection,
                         "ECM found factor in " + std::to_string(ecm_ms) +
                             "ms: " + ecm_f->to_string());
                return make_fast_result(*ecm_f, FactorizationMethod::SIQS,
                                        "ECM found factor (B1=" + std::to_string(ecm_config.B1) +
                                            ", " + std::to_string(ecm_ms) + "ms)");
            }
        } // if (ecm_config.num_curves > 0)
    }

    // ── Phase 2: SIQS for medium N ──
    // GNFS_DISABLE_SIQS=1 also suppresses the SIQS probe inside GNFS path.
    bool siqs_disabled = []() {
        const char* e = std::getenv("GNFS_DISABLE_SIQS");
        return e && e[0] == '1';
    }();
    if ((method == FactorizationMethod::SIQS ||
         (method == FactorizationMethod::GNFS && stats_.n_digits <= 100)) &&
        !siqs_disabled) {
        emit_log(LogLevel::Info, Phase::PolynomialSelection,
                 "Trying SIQS for " + std::to_string(stats_.n_digits) + "-digit N");

        // Adaptive timeout: generous for forced SIQS, bounded for GNFS-with-SIQS-probe
        size_t siqs_timeout;
        if (method == FactorizationMethod::SIQS) {
            // User selected SIQS: give it plenty of time
            if (stats_.n_digits <= 50)
                siqs_timeout = 60;
            else if (stats_.n_digits <= 60)
                siqs_timeout = 300;
            else if (stats_.n_digits <= 70)
                siqs_timeout = 900;
            else if (stats_.n_digits <= 80)
                siqs_timeout = 1800;
            else if (stats_.n_digits <= 90)
                siqs_timeout = 3600;
            else
                siqs_timeout = 7200;
        } else {
            // Auto/GNFS: SIQS as quick probe before GNFS
            if (stats_.n_digits <= 50)
                siqs_timeout = 30;
            else if (stats_.n_digits <= 60)
                siqs_timeout = 120;
            else if (stats_.n_digits <= 70)
                siqs_timeout = 300;
            else if (stats_.n_digits <= 80)
                siqs_timeout = 900;
            else
                siqs_timeout = 3600;
        }

        auto siqs_result = siqs::factor(n_, siqs_timeout, true);
        if (siqs_result) {
            emit_log(LogLevel::Info, Phase::PolynomialSelection,
                     "SIQS found factor: " + siqs_result->factor1.to_string() + " * " +
                         siqs_result->factor2.to_string());

            FactorResult r;
            r.success = true;
            r.n = n_;                          // Integer op=
            Integer f1 = siqs_result->factor1; // Integer copy ctor
            Integer f2 = siqs_result->factor2;
            if (f1 > f2)
                std::swap(f1, f2);
            r.factors.push_back(std::move(f1));
            r.factors.push_back(std::move(f2));
            r.stats = stats_;
            r.stats.method_used = FactorizationMethod::SIQS;
            r.stats.method_reason = std::to_string(stats_.n_digits) + "d SIQS";
            r.stats.timings.total_s = elapsed_s();
            return r;
        }

        if (method == FactorizationMethod::SIQS) {
            // User forced SIQS only — don't fall through to GNFS
            emit_log(LogLevel::Warn, Phase::PolynomialSelection,
                     "SIQS failed (timeout=" + std::to_string(siqs_timeout) + "s)");
            FactorResult r;
            r.n = n_; // Integer op=
            r.stats = stats_;
            r.stats.timings.total_s = elapsed_s();
            return r;
        }

        emit_log(LogLevel::Info, Phase::PolynomialSelection, "SIQS failed, falling back to GNFS");
    }

    // ── Phase 3: Full GNFS pipeline ──
    stats_.method_used = FactorizationMethod::GNFS;
    stats_.method_reason = std::to_string(stats_.n_digits) + "d GNFS";

    auto ctx = select_polynomial_impl(structured_route.resume_base_path);
    auto fb = build_factor_base_impl(ctx, structured_route.resume_base_path);
    auto reduction = sieve_and_collect_impl(ctx, fb, structured_route, {});

    const size_t factor_base_cols =
        gnfs::util::saturating_size_add(fb.rational_count(), fb.sieve_algebraic_count());
    size_t matrix_cols = gnfs::util::saturating_size_add(factor_base_cols, params_.target_excess);

    // Effective cols includes LP columns matrix_builder will create.
    const size_t post_lp_cols = reduction.stats.output_lp_columns;
    const size_t effective_cols_post = relation::effective_column_count(matrix_cols, post_lp_cols);

    return detail::handoff_after_collection(
        std::move(reduction), effective_cols_post,
        [this, matrix_cols, post_lp_cols](const detail::SolverHandoffInfo& info) {
            emit_log(LogLevel::Warn, Phase::Sieving,
                     "Underbuilt relation reduction: rows=" + std::to_string(info.relation_rows) +
                         " <= estimated_effective_cols=" +
                         std::to_string(info.estimated_effective_columns) +
                         " (matrix_cols=" + std::to_string(matrix_cols) +
                         " + lp_cols=" + std::to_string(post_lp_cols) +
                         "); forwarding to solve_matrix for the actual matrix decision");
        },
        [this, &fb, &ctx](relation::RelationReductionResult handed_off) {
            auto mr = solve_matrix(std::move(handed_off), fb, ctx);
            return extract_factors(mr, fb, ctx);
        });
}

} // namespace gnfs::api
