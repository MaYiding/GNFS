#pragma once

/// @file shadow_proof_observe.hpp
/// @brief Allocation-free naming and scalar telemetry for SIQS shadow proofs.

#include <gnfs/siqs/shadow_proof_runner.hpp>
#include <gnfs/util/process_memory.hpp>

#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace gnfs::siqs {

using std::size_t;

inline constexpr char SIQS_SHADOW_PROOF_OBSERVE_ENV[] = "GNFS_SIQS_SHADOW_PROOF";
inline constexpr uint32_t SIQS_SHADOW_PROOF_OBSERVE_SCHEMA_VERSION = 1;
inline constexpr char SIQS_SHADOW_PROOF_OBSERVE_PREFIX[] = "GNFS_SIQS_SHADOW_PROOF_OBSERVE_V1";

enum class SIQSShadowProofObserveMode : uint8_t {
    off,
    observe,
};

/// Parse the exact public environment contract. A missing value and `0` are
/// off; all spellings other than `observe` are rejected.
[[nodiscard]] inline SIQSShadowProofObserveMode
parse_siqs_shadow_proof_observe_mode(const char* value) {
    if (value == nullptr || std::string_view(value) == "0") {
        return SIQSShadowProofObserveMode::off;
    }
    if (std::string_view(value) == "observe") {
        return SIQSShadowProofObserveMode::observe;
    }
    throw std::invalid_argument(
        "GNFS_SIQS_SHADOW_PROOF must be unset or exactly one of: 0, observe");
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_observe_mode_name(SIQSShadowProofObserveMode mode) noexcept {
    switch (mode) {
    case SIQSShadowProofObserveMode::off:
        return "off";
    case SIQSShadowProofObserveMode::observe:
        return "observe";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_terminal_status_name(SIQSShadowProofTerminalStatus status) noexcept {
    switch (status) {
    case SIQSShadowProofTerminalStatus::factor_found:
        return "factor_found";
    case SIQSShadowProofTerminalStatus::no_factor:
        return "no_factor";
    case SIQSShadowProofTerminalStatus::bounded_fallback:
        return "bounded_fallback";
    case SIQSShadowProofTerminalStatus::invalid_input:
        return "invalid_input";
    case SIQSShadowProofTerminalStatus::stage_failure:
        return "stage_failure";
    case SIQSShadowProofTerminalStatus::resource_exhausted:
        return "resource_exhausted";
    case SIQSShadowProofTerminalStatus::exception_failure:
        return "exception_failure";
    case SIQSShadowProofTerminalStatus::internal_invariant_failure:
        return "internal_invariant_failure";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_stage_name(SIQSShadowProofStage stage) noexcept {
    switch (stage) {
    case SIQSShadowProofStage::not_started:
        return "not_started";
    case SIQSShadowProofStage::input_validation:
        return "input_validation";
    case SIQSShadowProofStage::payload_accounting:
        return "payload_accounting";
    case SIQSShadowProofStage::adapter_preflight:
        return "adapter_preflight";
    case SIQSShadowProofStage::graph_preflight:
        return "graph_preflight";
    case SIQSShadowProofStage::assembly:
        return "assembly";
    case SIQSShadowProofStage::matrix:
        return "matrix";
    case SIQSShadowProofStage::dependency_verification:
        return "dependency_verification";
    case SIQSShadowProofStage::factor_extraction:
        return "factor_extraction";
    case SIQSShadowProofStage::complete:
        return "complete";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_fallback_reason_name(SIQSShadowProofFallbackReason reason) noexcept {
    switch (reason) {
    case SIQSShadowProofFallbackReason::none:
        return "none";
    case SIQSShadowProofFallbackReason::raw_relation_limit:
        return "raw_relation_limit";
    case SIQSShadowProofFallbackReason::raw_payload_limit:
        return "raw_payload_limit";
    case SIQSShadowProofFallbackReason::graph_edge_limit:
        return "graph_edge_limit";
    case SIQSShadowProofFallbackReason::graph_cycle_limit:
        return "graph_cycle_limit";
    case SIQSShadowProofFallbackReason::graph_incidence_limit:
        return "graph_incidence_limit";
    case SIQSShadowProofFallbackReason::row_candidate_limit:
        return "row_candidate_limit";
    case SIQSShadowProofFallbackReason::pretrim_row_limit:
        return "pretrim_row_limit";
    case SIQSShadowProofFallbackReason::insufficient_rows:
        return "insufficient_rows";
    case SIQSShadowProofFallbackReason::matrix_resource_limit:
        return "matrix_resource_limit";
    case SIQSShadowProofFallbackReason::matrix_backend_unavailable:
        return "matrix_backend_unavailable";
    }
    return "unknown";
}

enum class SIQSShadowProofObserveSetupFailure : uint8_t {
    resource_exhausted,
    exception_failure,
};

/// Fixed, owning, scalar schema. Optional measurements are represented by a
/// support bit plus a zero value so every emitted record has the same fields.
struct SIQSShadowProofObserveRecord final {
    uint32_t schema_version = SIQS_SHADOW_PROOF_OBSERVE_SCHEMA_VERSION;
    bool proof_attempted = false;
    SIQSShadowProofTerminalStatus terminal_status =
        SIQSShadowProofTerminalStatus::internal_invariant_failure;
    SIQSShadowProofStage stage = SIQSShadowProofStage::not_started;
    SIQSShadowProofFallbackReason fallback_reason = SIQSShadowProofFallbackReason::none;
    bool factor_found = false;
    uint64_t observe_wall_ns = 0;

    size_t raw_relations = 0;
    bool raw_payload_supported = false;
    size_t raw_payload_bytes = 0;
    size_t factor_base_columns = 0;
    uint64_t large_prime_bound = 0;

    size_t raw_relation_cap = 0;
    size_t raw_payload_cap_bytes = 0;
    size_t graph_edge_cap = 0;
    size_t graph_cycle_cap = 0;
    size_t graph_incidence_cap = 0;
    size_t row_candidate_cap = 0;
    size_t pretrim_row_cap = 0;
    size_t minimum_row_excess = 0;
    size_t trim_excess_rows = 0;
    uint32_t assembly_workers = 0;
    size_t matrix_max_dependencies = 0;
    uint32_t matrix_workers = 0;
    size_t matrix_parallel_column_threshold = 0;
    size_t matrix_dense_bytes_cap = 0;
    size_t matrix_dense_variable_cap = 0;

    size_t adapter_input_relations = 0;
    size_t adapter_full_relations = 0;
    size_t adapter_accepted_one_lp = 0;
    size_t adapter_accepted_two_lp = 0;
    size_t adapter_rejected_relations = 0;
    size_t adapter_malformed_source_shape = 0;
    size_t adapter_unsupported_encoding = 0;
    size_t adapter_invalid_one_large_prime = 0;
    size_t adapter_invalid_two_large_prime_split = 0;
    size_t adapter_exact_duplicate = 0;

    bool graph_evidence_supported = false;
    size_t graph_vertices = 0;
    size_t graph_edges = 0;
    size_t graph_components = 0;
    size_t graph_cycles = 0;
    size_t graph_cycle_incidences = 0;
    size_t graph_max_cycle_length = 0;
    size_t row_candidate_upper = 0;

    bool assembly_evidence_supported = false;
    size_t assembly_pretrim_rows = 0;
    size_t assembly_selected_rows = 0;
    size_t assembly_selected_full_rows = 0;
    size_t assembly_selected_cycle_rows = 0;
    size_t assembly_trimmed_rows = 0;
    bool assembly_fingerprint_supported = false;
    uint64_t assembly_source_fingerprint_low = 0;
    uint64_t assembly_source_fingerprint_high = 0;
    uint64_t assembly_pretrim_fingerprint_low = 0;
    uint64_t assembly_pretrim_fingerprint_high = 0;
    uint64_t assembly_selected_fingerprint_low = 0;
    uint64_t assembly_selected_fingerprint_high = 0;

    bool projected_dense_bytes_supported = false;
    size_t projected_dense_bytes = 0;
    bool matrix_evidence_supported = false;
    size_t matrix_rows = 0;
    size_t matrix_columns = 0;
    size_t minimum_nullity = 0;

    size_t dependencies_returned = 0;
    size_t dependencies_examined = 0;
    size_t dependencies_verified = 0;
    size_t no_factor_count = 0;
    size_t factor_found_count = 0;
    bool dependency_cap_reached = false;
    bool dependency_fingerprint_supported = false;
    uint64_t dependency_fingerprint_low = 0;
    uint64_t dependency_fingerprint_high = 0;
    bool first_failed_dependency_supported = false;
    size_t first_failed_dependency = 0;
    bool winning_dependency_supported = false;
    size_t winning_dependency = 0;
    bool winning_dependency_size_supported = false;
    size_t winning_dependency_size = 0;

    util::ProcessMemorySnapshot before_memory{};
    util::ProcessMemorySnapshot after_memory{};
    bool peak_growth_supported = false;
    uint64_t peak_growth_bytes = 0;
};

namespace shadow_proof_observe_detail {

inline void copy_profile(SIQSShadowProofObserveRecord& record,
                         const SIQSShadowProofOptions& options) noexcept {
    record.raw_relation_cap = options.limits.max_raw_relations;
    record.raw_payload_cap_bytes = options.limits.max_raw_payload_bytes;
    record.graph_edge_cap = options.limits.graph.max_edges;
    record.graph_cycle_cap = options.limits.graph.max_cycles;
    record.graph_incidence_cap = options.limits.graph.max_cycle_incidences;
    record.row_candidate_cap = options.limits.max_row_candidates;
    record.pretrim_row_cap = options.limits.max_pretrim_rows;
    record.minimum_row_excess = options.limits.minimum_row_excess;
    record.trim_excess_rows = options.assembly.trim_excess_rows;
    record.assembly_workers = options.assembly.materialization_workers;
    record.matrix_max_dependencies = options.matrix.max_dependencies;
    record.matrix_workers = options.matrix.elimination_workers;
    record.matrix_parallel_column_threshold = options.matrix.parallel_column_threshold;
    record.matrix_dense_bytes_cap = options.matrix.max_dense_matrix_bytes;
    record.matrix_dense_variable_cap = options.matrix.max_dense_variable_count;
}

inline void copy_memory(SIQSShadowProofObserveRecord& record,
                        const util::ProcessMemorySnapshot& before,
                        const util::ProcessMemorySnapshot& after) noexcept {
    record.before_memory = before;
    record.after_memory = after;
    if (before.backend != util::ProcessMemoryBackend::Unsupported &&
        before.backend == after.backend && before.lifetime_peak_rss_bytes.has_value() &&
        after.lifetime_peak_rss_bytes.has_value() &&
        *after.lifetime_peak_rss_bytes >= *before.lifetime_peak_rss_bytes) {
        record.peak_growth_supported = true;
        record.peak_growth_bytes = *after.lifetime_peak_rss_bytes - *before.lifetime_peak_rss_bytes;
    }
}

[[nodiscard]] constexpr const char* bool_name(bool value) noexcept {
    return value ? "true" : "false";
}

[[nodiscard]] constexpr uint64_t
optional_u64_or_zero(const std::optional<uint64_t>& value) noexcept {
    return value.value_or(0);
}

[[nodiscard]] inline uint64_t
elapsed_wall_ns(std::chrono::steady_clock::time_point before,
                std::chrono::steady_clock::time_point after) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
    if (elapsed <= 0) {
        return 0;
    }
    using Elapsed = decltype(elapsed);
    if constexpr (std::numeric_limits<Elapsed>::max() > std::numeric_limits<uint64_t>::max()) {
        if (elapsed > static_cast<Elapsed>(std::numeric_limits<uint64_t>::max())) {
            return std::numeric_limits<uint64_t>::max();
        }
    }
    return static_cast<uint64_t>(elapsed);
}

template <class SnapshotProvider>
[[nodiscard]] util::ProcessMemorySnapshot
safe_memory_snapshot(SnapshotProvider& snapshot_provider) noexcept {
    try {
        return std::invoke(snapshot_provider);
    } catch (...) {
        return {};
    }
}

} // namespace shadow_proof_observe_detail

[[nodiscard]] inline SIQSShadowProofObserveRecord make_siqs_shadow_proof_observe_record_from_result(
    const SIQSShadowProofResult& result, const util::ProcessMemorySnapshot& before,
    const util::ProcessMemorySnapshot& after) noexcept {
    const SIQSShadowProofEvidence& evidence = result.evidence();
    SIQSShadowProofObserveRecord record;
    record.proof_attempted = true;
    record.terminal_status = result.status();
    record.stage = result.stage();
    record.fallback_reason = result.fallback_reason();
    record.factor_found = result.has_factor();

    record.raw_relations = evidence.raw_relations;
    record.raw_payload_supported = evidence.raw_payload_bytes.has_value();
    record.raw_payload_bytes = evidence.raw_payload_bytes.value_or(0);
    record.factor_base_columns = evidence.factor_base_columns;
    record.large_prime_bound = evidence.large_prime_bound;
    shadow_proof_observe_detail::copy_profile(record, evidence.options);

    record.adapter_input_relations = evidence.adapter.input_relations;
    record.adapter_full_relations = evidence.adapter.full_relations;
    record.adapter_accepted_one_lp = evidence.adapter.accepted_one_lp;
    record.adapter_accepted_two_lp = evidence.adapter.accepted_two_lp;
    record.adapter_rejected_relations = evidence.adapter.rejected_relations;
    record.adapter_malformed_source_shape = evidence.adapter.malformed_source_shape;
    record.adapter_unsupported_encoding = evidence.adapter.unsupported_encoding;
    record.adapter_invalid_one_large_prime = evidence.adapter.invalid_one_large_prime;
    record.adapter_invalid_two_large_prime_split = evidence.adapter.invalid_two_large_prime_split;
    record.adapter_exact_duplicate = evidence.adapter.exact_duplicate;

    record.graph_evidence_supported = evidence.graph_status == TwoLargePrimeCycleBasisStatus::valid;
    record.graph_vertices = evidence.graph_vertices;
    record.graph_edges = evidence.graph_edges;
    record.graph_components = evidence.graph_components;
    record.graph_cycles = evidence.graph_cycles;
    record.graph_cycle_incidences = evidence.graph_cycle_incidences;
    record.graph_max_cycle_length = evidence.graph_max_cycle_length;
    record.row_candidate_upper = evidence.row_candidate_upper;

    record.assembly_evidence_supported =
        evidence.assembly_status == SIQSShadowAssemblyStatus::valid;
    record.assembly_pretrim_rows = evidence.assembly.pretrim_rows;
    record.assembly_selected_rows = evidence.assembly.selected_rows;
    record.assembly_selected_full_rows = evidence.assembly.selected_full_rows;
    record.assembly_selected_cycle_rows = evidence.assembly.selected_cycle_rows;
    record.assembly_trimmed_rows = evidence.assembly.trimmed_rows;
    record.assembly_fingerprint_supported =
        evidence.assembly_status == SIQSShadowAssemblyStatus::valid;
    record.assembly_source_fingerprint_low = evidence.assembly_fingerprints.source_catalog.low;
    record.assembly_source_fingerprint_high = evidence.assembly_fingerprints.source_catalog.high;
    record.assembly_pretrim_fingerprint_low = evidence.assembly_fingerprints.pretrim_rows.low;
    record.assembly_pretrim_fingerprint_high = evidence.assembly_fingerprints.pretrim_rows.high;
    record.assembly_selected_fingerprint_low = evidence.assembly_fingerprints.selected_rows.low;
    record.assembly_selected_fingerprint_high = evidence.assembly_fingerprints.selected_rows.high;

    record.projected_dense_bytes_supported = evidence.projected_dense_matrix_bytes.has_value();
    record.projected_dense_bytes = evidence.projected_dense_matrix_bytes.value_or(0);
    record.matrix_evidence_supported = evidence.matrix_status == SIQSShadowMatrixStatus::valid;
    record.matrix_rows = evidence.matrix_rows;
    record.matrix_columns = evidence.matrix_columns;
    record.minimum_nullity = evidence.minimum_nullity;

    record.dependencies_returned = evidence.dependencies_returned;
    record.dependencies_examined = evidence.dependencies_examined;
    record.dependencies_verified = evidence.dependencies_verified;
    record.no_factor_count = evidence.no_factor_count;
    record.factor_found_count = evidence.factor_found_count;
    record.dependency_cap_reached = evidence.dependency_cap_reached;
    record.dependency_fingerprint_supported = evidence.dependency_fingerprint.has_value();
    if (evidence.dependency_fingerprint.has_value()) {
        record.dependency_fingerprint_low = evidence.dependency_fingerprint->low;
        record.dependency_fingerprint_high = evidence.dependency_fingerprint->high;
    }
    record.first_failed_dependency_supported = evidence.first_failed_dependency.has_value();
    record.first_failed_dependency = evidence.first_failed_dependency.value_or(0);
    record.winning_dependency_supported = evidence.winning_dependency.has_value();
    record.winning_dependency = evidence.winning_dependency.value_or(0);
    record.winning_dependency_size_supported = evidence.winning_dependency_size.has_value();
    record.winning_dependency_size = evidence.winning_dependency_size.value_or(0);
    shadow_proof_observe_detail::copy_memory(record, before, after);
    return record;
}

[[nodiscard]] inline SIQSShadowProofObserveRecord
make_siqs_shadow_proof_observe_record(const SIQSShadowProofResult& result,
                                      const util::ProcessMemorySnapshot& before,
                                      const util::ProcessMemorySnapshot& after) noexcept {
    return make_siqs_shadow_proof_observe_record_from_result(result, before, after);
}

[[nodiscard]] inline SIQSShadowProofObserveRecord make_siqs_shadow_proof_observe_setup_failure(
    SIQSShadowProofObserveSetupFailure failure, size_t raw_count, size_t factor_base_columns,
    uint64_t large_prime_bound, const SIQSShadowProofOptions& options,
    const util::ProcessMemorySnapshot& before, const util::ProcessMemorySnapshot& after) noexcept {
    SIQSShadowProofObserveRecord record;
    record.proof_attempted = false;
    record.terminal_status = failure == SIQSShadowProofObserveSetupFailure::resource_exhausted
                                 ? SIQSShadowProofTerminalStatus::resource_exhausted
                                 : SIQSShadowProofTerminalStatus::exception_failure;
    record.stage = SIQSShadowProofStage::not_started;
    record.fallback_reason = SIQSShadowProofFallbackReason::none;
    record.raw_relations = raw_count;
    record.factor_base_columns = factor_base_columns;
    record.large_prime_bound = large_prime_bound;
    shadow_proof_observe_detail::copy_profile(record, options);
    shadow_proof_observe_detail::copy_memory(record, before, after);
    return record;
}

/// Execute setup and proof under one no-throw observation boundary. The
/// operation should own all potentially-throwing setup (including construction
/// of the factor-base view) and return an owning SIQSShadowProofResult.
template <class Operation, class SnapshotProvider>
[[nodiscard]] SIQSShadowProofObserveRecord
observe_siqs_shadow_proof(size_t raw_count, size_t factor_base_columns, uint64_t large_prime_bound,
                          const SIQSShadowProofOptions& options, Operation&& operation,
                          SnapshotProvider&& snapshot_provider) noexcept {
    auto&& provider = snapshot_provider;
    const auto wall_before = std::chrono::steady_clock::now();
    const util::ProcessMemorySnapshot before =
        shadow_proof_observe_detail::safe_memory_snapshot(provider);

    SIQSShadowProofObserveRecord record;
    try {
        SIQSShadowProofResult result = std::invoke(std::forward<Operation>(operation));
        const util::ProcessMemorySnapshot after =
            shadow_proof_observe_detail::safe_memory_snapshot(provider);
        record = make_siqs_shadow_proof_observe_record_from_result(result, before, after);
    } catch (const std::bad_alloc&) {
        const util::ProcessMemorySnapshot after =
            shadow_proof_observe_detail::safe_memory_snapshot(provider);
        record = make_siqs_shadow_proof_observe_setup_failure(
            SIQSShadowProofObserveSetupFailure::resource_exhausted, raw_count, factor_base_columns,
            large_prime_bound, options, before, after);
    } catch (...) {
        const util::ProcessMemorySnapshot after =
            shadow_proof_observe_detail::safe_memory_snapshot(provider);
        record = make_siqs_shadow_proof_observe_setup_failure(
            SIQSShadowProofObserveSetupFailure::exception_failure, raw_count, factor_base_columns,
            large_prime_bound, options, before, after);
    }
    record.observe_wall_ns =
        shadow_proof_observe_detail::elapsed_wall_ns(wall_before, std::chrono::steady_clock::now());
    return record;
}

template <class Operation>
[[nodiscard]] SIQSShadowProofObserveRecord
observe_siqs_shadow_proof(size_t raw_count, size_t factor_base_columns, uint64_t large_prime_bound,
                          const SIQSShadowProofOptions& options, Operation&& operation) noexcept {
    const auto snapshot_provider = []() noexcept { return util::process_memory_snapshot(); };
    return observe_siqs_shadow_proof(raw_count, factor_base_columns, large_prime_bound, options,
                                     std::forward<Operation>(operation), snapshot_provider);
}

/// Emit and commit exactly one stable schema-v1 line. A write or flush error is
/// reported to the caller and never throws.
[[nodiscard]] inline bool
emit_siqs_shadow_proof_observe_record(std::FILE* output,
                                      const SIQSShadowProofObserveRecord& record) noexcept {
    if (output == nullptr) {
        return false;
    }

    const std::string_view terminal =
        siqs_shadow_proof_terminal_status_name(record.terminal_status);
    const std::string_view stage = siqs_shadow_proof_stage_name(record.stage);
    const std::string_view fallback =
        siqs_shadow_proof_fallback_reason_name(record.fallback_reason);
    const std::string_view before_backend =
        util::process_memory_backend_name(record.before_memory.backend);
    const std::string_view after_backend =
        util::process_memory_backend_name(record.after_memory.backend);

    const int result = std::fprintf(
        output,
        "%s schema_version=%" PRIu32 " mode=observe route=legacy_continue rss_scope=self_lifetime"
        " proof_attempted=%s terminal=%.*s stage=%.*s fallback=%.*s"
        " factor_found=%s observe_wall_ns=%" PRIu64
        " raw_relations=%zu raw_payload_supported=%s raw_payload_bytes=%zu"
        " factor_base_columns=%zu large_prime_bound=%" PRIu64
        " raw_relation_cap=%zu raw_payload_cap_bytes=%zu graph_edge_cap=%zu"
        " graph_cycle_cap=%zu graph_incidence_cap=%zu row_candidate_cap=%zu"
        " pretrim_row_cap=%zu minimum_row_excess=%zu trim_excess_rows=%zu"
        " assembly_workers=%" PRIu32 " matrix_max_dependencies=%zu matrix_workers=%" PRIu32
        " matrix_parallel_column_threshold=%zu matrix_dense_bytes_cap=%zu"
        " matrix_dense_variable_cap=%zu adapter_input_relations=%zu"
        " adapter_full_relations=%zu adapter_accepted_one_lp=%zu"
        " adapter_accepted_two_lp=%zu adapter_rejected_relations=%zu"
        " adapter_malformed_source_shape=%zu adapter_unsupported_encoding=%zu"
        " adapter_invalid_one_large_prime=%zu adapter_invalid_two_large_prime_split=%zu"
        " adapter_exact_duplicate=%zu graph_evidence_supported=%s graph_vertices=%zu"
        " graph_edges=%zu graph_components=%zu graph_cycles=%zu"
        " graph_cycle_incidences=%zu graph_max_cycle_length=%zu row_candidate_upper=%zu"
        " assembly_evidence_supported=%s assembly_pretrim_rows=%zu"
        " assembly_selected_rows=%zu assembly_selected_full_rows=%zu"
        " assembly_selected_cycle_rows=%zu assembly_trimmed_rows=%zu"
        " assembly_fingerprint_supported=%s assembly_source_fingerprint_low=%" PRIu64
        " assembly_source_fingerprint_high=%" PRIu64 " assembly_pretrim_fingerprint_low=%" PRIu64
        " assembly_pretrim_fingerprint_high=%" PRIu64 " assembly_selected_fingerprint_low=%" PRIu64
        " assembly_selected_fingerprint_high=%" PRIu64
        " projected_dense_bytes_supported=%s projected_dense_bytes=%zu"
        " matrix_evidence_supported=%s matrix_rows=%zu matrix_columns=%zu"
        " minimum_nullity=%zu dependencies_returned=%zu dependencies_examined=%zu"
        " dependencies_verified=%zu no_factor_count=%zu factor_found_count=%zu"
        " dependency_cap_reached=%s dependency_fingerprint_supported=%s"
        " dependency_fingerprint_low=%" PRIu64 " dependency_fingerprint_high=%" PRIu64
        " first_failed_dependency_supported=%s first_failed_dependency=%zu"
        " winning_dependency_supported=%s winning_dependency=%zu"
        " winning_dependency_size_supported=%s winning_dependency_size=%zu"
        " before_rss_backend=%.*s"
        " before_current_rss_supported=%s before_current_rss_bytes=%" PRIu64
        " before_peak_rss_supported=%s before_peak_rss_bytes=%" PRIu64
        " after_rss_backend=%.*s after_current_rss_supported=%s"
        " after_current_rss_bytes=%" PRIu64
        " after_peak_rss_supported=%s after_peak_rss_bytes=%" PRIu64
        " peak_growth_supported=%s peak_growth_bytes=%" PRIu64 " promotion=false\n",
        SIQS_SHADOW_PROOF_OBSERVE_PREFIX, record.schema_version,
        shadow_proof_observe_detail::bool_name(record.proof_attempted),
        static_cast<int>(terminal.size()), terminal.data(), static_cast<int>(stage.size()),
        stage.data(), static_cast<int>(fallback.size()), fallback.data(),
        shadow_proof_observe_detail::bool_name(record.factor_found), record.observe_wall_ns,
        record.raw_relations, shadow_proof_observe_detail::bool_name(record.raw_payload_supported),
        record.raw_payload_bytes, record.factor_base_columns, record.large_prime_bound,
        record.raw_relation_cap, record.raw_payload_cap_bytes, record.graph_edge_cap,
        record.graph_cycle_cap, record.graph_incidence_cap, record.row_candidate_cap,
        record.pretrim_row_cap, record.minimum_row_excess, record.trim_excess_rows,
        record.assembly_workers, record.matrix_max_dependencies, record.matrix_workers,
        record.matrix_parallel_column_threshold, record.matrix_dense_bytes_cap,
        record.matrix_dense_variable_cap, record.adapter_input_relations,
        record.adapter_full_relations, record.adapter_accepted_one_lp,
        record.adapter_accepted_two_lp, record.adapter_rejected_relations,
        record.adapter_malformed_source_shape, record.adapter_unsupported_encoding,
        record.adapter_invalid_one_large_prime, record.adapter_invalid_two_large_prime_split,
        record.adapter_exact_duplicate,
        shadow_proof_observe_detail::bool_name(record.graph_evidence_supported),
        record.graph_vertices, record.graph_edges, record.graph_components, record.graph_cycles,
        record.graph_cycle_incidences, record.graph_max_cycle_length, record.row_candidate_upper,
        shadow_proof_observe_detail::bool_name(record.assembly_evidence_supported),
        record.assembly_pretrim_rows, record.assembly_selected_rows,
        record.assembly_selected_full_rows, record.assembly_selected_cycle_rows,
        record.assembly_trimmed_rows,
        shadow_proof_observe_detail::bool_name(record.assembly_fingerprint_supported),
        record.assembly_source_fingerprint_low, record.assembly_source_fingerprint_high,
        record.assembly_pretrim_fingerprint_low, record.assembly_pretrim_fingerprint_high,
        record.assembly_selected_fingerprint_low, record.assembly_selected_fingerprint_high,
        shadow_proof_observe_detail::bool_name(record.projected_dense_bytes_supported),
        record.projected_dense_bytes,
        shadow_proof_observe_detail::bool_name(record.matrix_evidence_supported),
        record.matrix_rows, record.matrix_columns, record.minimum_nullity,
        record.dependencies_returned, record.dependencies_examined, record.dependencies_verified,
        record.no_factor_count, record.factor_found_count,
        shadow_proof_observe_detail::bool_name(record.dependency_cap_reached),
        shadow_proof_observe_detail::bool_name(record.dependency_fingerprint_supported),
        record.dependency_fingerprint_low, record.dependency_fingerprint_high,
        shadow_proof_observe_detail::bool_name(record.first_failed_dependency_supported),
        record.first_failed_dependency,
        shadow_proof_observe_detail::bool_name(record.winning_dependency_supported),
        record.winning_dependency,
        shadow_proof_observe_detail::bool_name(record.winning_dependency_size_supported),
        record.winning_dependency_size, static_cast<int>(before_backend.size()),
        before_backend.data(),
        shadow_proof_observe_detail::bool_name(record.before_memory.current_rss_bytes.has_value()),
        shadow_proof_observe_detail::optional_u64_or_zero(record.before_memory.current_rss_bytes),
        shadow_proof_observe_detail::bool_name(
            record.before_memory.lifetime_peak_rss_bytes.has_value()),
        shadow_proof_observe_detail::optional_u64_or_zero(
            record.before_memory.lifetime_peak_rss_bytes),
        static_cast<int>(after_backend.size()), after_backend.data(),
        shadow_proof_observe_detail::bool_name(record.after_memory.current_rss_bytes.has_value()),
        shadow_proof_observe_detail::optional_u64_or_zero(record.after_memory.current_rss_bytes),
        shadow_proof_observe_detail::bool_name(
            record.after_memory.lifetime_peak_rss_bytes.has_value()),
        shadow_proof_observe_detail::optional_u64_or_zero(
            record.after_memory.lifetime_peak_rss_bytes),
        shadow_proof_observe_detail::bool_name(record.peak_growth_supported),
        record.peak_growth_bytes);
    if (result < 0) {
        return false;
    }
    return std::fflush(output) == 0 && std::ferror(output) == 0;
}

[[nodiscard]] inline bool
emit_siqs_shadow_proof_observe_record(const SIQSShadowProofObserveRecord& record) noexcept {
    return emit_siqs_shadow_proof_observe_record(stderr, record);
}

} // namespace gnfs::siqs
