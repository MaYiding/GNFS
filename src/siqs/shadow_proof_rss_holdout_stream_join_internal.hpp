#pragma once

// Authority-free validation and joining for one sealed SIQS RSS holdout
// transcript. A successful result is still uncommitted: it owns the exact
// child bytes and a canonical typed projection, but it cannot create an
// artifact seal, journal commit, durable receipt, or launch permit.

#include "shadow_proof_rss_holdout_probe_record_codec_internal.hpp"

#include <gnfs/siqs/shadow_matrix.hpp>
#include <gnfs/siqs/shadow_proof_observe_record_codec.hpp>
#include <gnfs/siqs/shadow_proof_rss_campaign_journal.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace gnfs::siqs::shadow_proof_rss_holdout_detail {

inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_PREFIX =
    "GNFS_SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_V1";
inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_SCHEMA_VERSION = 1;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_FACTOR_BASE_COLUMNS = 1601;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS = 1701;
// Outcome-blind, fixture-specific values derived from the current production
// multiplier, factor-base, and one-large-prime profile. They are execution
// contract values, not measurement results and not part of the sealed corpus
// digest. A production profile change must update this table deliberately.
inline constexpr std::array<uint64_t, siqs::SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT>
    SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS{
        UINT64_C(3494760), UINT64_C(3487560), UINT64_C(3560520), UINT64_C(3430920),
        UINT64_C(3507720), UINT64_C(3610680), UINT64_C(3626760), UINT64_C(3593640),
    };

enum class SIQSShadowProofRssHoldoutStreamJoinError : uint8_t {
    none,
    policy_invalid,
    runtime_facts_invalid,
    runtime_facts_mismatch,
    release_ndebug_required,
    slot_invalid,
    stdout_invalid,
    stdout_binding_mismatch,
    stderr_presence_invalid,
    stderr_invalid,
    observe_terminal_invalid,
    observe_profile_invalid,
    observe_adapter_invalid,
    observe_graph_invalid,
    observe_assembly_invalid,
    observe_matrix_invalid,
    observe_dependency_invalid,
    observe_memory_invalid,
    cross_stream_mismatch,
    allocation_failure,
};

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_holdout_stream_join_error_name(
    SIQSShadowProofRssHoldoutStreamJoinError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssHoldoutStreamJoinError::none:
        return "none";
    case SIQSShadowProofRssHoldoutStreamJoinError::policy_invalid:
        return "policy_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::runtime_facts_invalid:
        return "runtime_facts_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::runtime_facts_mismatch:
        return "runtime_facts_mismatch";
    case SIQSShadowProofRssHoldoutStreamJoinError::release_ndebug_required:
        return "release_ndebug_required";
    case SIQSShadowProofRssHoldoutStreamJoinError::slot_invalid:
        return "slot_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::stdout_invalid:
        return "stdout_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::stdout_binding_mismatch:
        return "stdout_binding_mismatch";
    case SIQSShadowProofRssHoldoutStreamJoinError::stderr_presence_invalid:
        return "stderr_presence_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::stderr_invalid:
        return "stderr_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::observe_terminal_invalid:
        return "observe_terminal_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::observe_profile_invalid:
        return "observe_profile_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::observe_adapter_invalid:
        return "observe_adapter_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::observe_graph_invalid:
        return "observe_graph_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::observe_assembly_invalid:
        return "observe_assembly_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::observe_matrix_invalid:
        return "observe_matrix_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::observe_dependency_invalid:
        return "observe_dependency_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::observe_memory_invalid:
        return "observe_memory_invalid";
    case SIQSShadowProofRssHoldoutStreamJoinError::cross_stream_mismatch:
        return "cross_stream_mismatch";
    case SIQSShadowProofRssHoldoutStreamJoinError::allocation_failure:
        return "allocation_failure";
    }
    return "unknown";
}

/// A stable fingerprint of bytes that have not yet crossed a durable
/// publication boundary. The type deliberately has no `committed` member.
struct SIQSShadowProofRssUncommittedArtifactFingerprint final {
    siqs::SIQSShadowProofRssArtifactKind kind = siqs::SIQSShadowProofRssArtifactKind::unknown;
    uint64_t byte_count = 0;
    siqs::SIQSShadowProofRssCorpusDigest digest;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssUncommittedArtifactFingerprint&,
               const SIQSShadowProofRssUncommittedArtifactFingerprint&) noexcept = default;
};

/// Owning evidence draft. Only a later same-child capture and durable artifact
/// publisher may convert these values into a journal commit payload.
struct SIQSShadowProofRssUncommittedSampleDraft final {
    siqs::SIQSShadowProofRssCorpusDigest policy_binding_digest;
    uint32_t slot_number = 0;
    uint32_t fixture_id = 0;
    siqs::SIQSShadowProofRssSampleMode mode = siqs::SIQSShadowProofRssSampleMode::unknown;
    uint32_t ordinal = 0;
    siqs::SIQSShadowProofRssOperatingSystem operating_system =
        siqs::SIQSShadowProofRssOperatingSystem::unknown;
    siqs::SIQSShadowProofRssArchitecture architecture =
        siqs::SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t resolved_production_sieve_workers = 0;
    bool fresh_process = false;
    bool completed = false;
    siqs::SIQSShadowProofRssFactorIdentity factor_identity =
        siqs::SIQSShadowProofRssFactorIdentity::unknown;
    siqs::SIQSShadowProofRssEvidence proof_evidence = siqs::SIQSShadowProofRssEvidence::unknown;
    siqs::SIQSShadowProofRssEvidence matrix_evidence = siqs::SIQSShadowProofRssEvidence::unknown;
    uint64_t relations_found = 0;
    uint64_t polynomials_used = 0;
    uint64_t absolute_peak_rss_bytes = 0;
    std::optional<uint64_t> current_rss_bytes;
    std::optional<uint64_t> peak_growth_bytes;
    uint64_t wall_ns = 0;
    SIQSShadowProofRssUncommittedArtifactFingerprint stdout_fingerprint;
    SIQSShadowProofRssUncommittedArtifactFingerprint stderr_fingerprint;
    std::string stdout_bytes;
    std::string stderr_bytes;
    std::string joined_bytes;
};

struct SIQSShadowProofRssHoldoutStreamJoinResult final {
    std::optional<SIQSShadowProofRssUncommittedSampleDraft> draft;
    SIQSShadowProofRssHoldoutStreamJoinError error = SIQSShadowProofRssHoldoutStreamJoinError::none;
    SIQSShadowProofRssHoldoutProbeRecordCodecDiagnostic stdout_diagnostic;
    siqs::SIQSShadowProofObserveRecordDiagnostic stderr_diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return draft.has_value() && error == SIQSShadowProofRssHoldoutStreamJoinError::none;
    }
};

namespace siqs_shadow_proof_rss_holdout_stream_join_detail {

[[nodiscard]] constexpr SIQSShadowProofRssHoldoutStreamJoinResult
failure(SIQSShadowProofRssHoldoutStreamJoinError error) noexcept {
    SIQSShadowProofRssHoldoutStreamJoinResult result;
    result.error = error;
    return result;
}

[[nodiscard]] constexpr bool checked_add(std::size_t left, std::size_t right,
                                         std::size_t& result) noexcept {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] constexpr bool checked_double(std::size_t value, std::size_t& result) noexcept {
    return checked_add(value, value, result);
}

[[nodiscard]] constexpr bool checked_multiply(std::size_t left, std::size_t right,
                                              std::size_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] constexpr siqs::SIQSShadowProofRssSampleMode
gate_mode(SIQSShadowProofRssHoldoutProbeMode mode) noexcept {
    switch (mode) {
    case SIQSShadowProofRssHoldoutProbeMode::off:
        return siqs::SIQSShadowProofRssSampleMode::off;
    case SIQSShadowProofRssHoldoutProbeMode::observe:
        return siqs::SIQSShadowProofRssSampleMode::observe;
    case SIQSShadowProofRssHoldoutProbeMode::unknown:
        return siqs::SIQSShadowProofRssSampleMode::unknown;
    }
    return siqs::SIQSShadowProofRssSampleMode::unknown;
}

[[nodiscard]] constexpr SIQSShadowProofRssUncommittedArtifactFingerprint
fingerprint(siqs::SIQSShadowProofRssArtifactKind kind, std::string_view bytes) noexcept {
    return {
        kind,
        static_cast<uint64_t>(bytes.size()),
        siqs::shadow_proof_rss_campaign_journal_detail::artifact_digest(kind, bytes),
    };
}

[[nodiscard]] constexpr bool fingerprint_is_nonzero(uint64_t low, uint64_t high) noexcept {
    return low != 0 || high != 0;
}

[[nodiscard]] constexpr bool
observe_profile_is_valid(const siqs::SIQSShadowProofObserveRecord& record,
                         uint32_t fixture_id) noexcept {
    const siqs::SIQSShadowProofOptions defaults{};
    return fixture_id != 0 &&
           fixture_id <= SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS.size() &&
           record.factor_base_columns == SIQS_SHADOW_PROOF_RSS_HOLDOUT_FACTOR_BASE_COLUMNS &&
           record.large_prime_bound ==
               SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS[fixture_id - 1] &&
           record.raw_relation_cap == defaults.limits.max_raw_relations &&
           record.raw_payload_cap_bytes == defaults.limits.max_raw_payload_bytes &&
           record.graph_edge_cap == defaults.limits.graph.max_edges &&
           record.graph_cycle_cap == defaults.limits.graph.max_cycles &&
           record.graph_incidence_cap == defaults.limits.graph.max_cycle_incidences &&
           record.row_candidate_cap == defaults.limits.max_row_candidates &&
           record.pretrim_row_cap == defaults.limits.max_pretrim_rows &&
           record.minimum_row_excess == defaults.limits.minimum_row_excess &&
           record.trim_excess_rows == defaults.assembly.trim_excess_rows &&
           record.assembly_workers == defaults.assembly.materialization_workers &&
           record.matrix_max_dependencies == defaults.matrix.max_dependencies &&
           record.matrix_workers == defaults.matrix.elimination_workers &&
           record.matrix_parallel_column_threshold == defaults.matrix.parallel_column_threshold &&
           record.matrix_dense_bytes_cap == defaults.matrix.max_dense_matrix_bytes &&
           record.matrix_dense_variable_cap == defaults.matrix.max_dense_variable_count;
}

[[nodiscard]] constexpr bool
observe_adapter_is_valid(const siqs::SIQSShadowProofObserveRecord& record) noexcept {
    if (!record.raw_payload_supported || record.raw_relations == 0 ||
        record.raw_relations > record.raw_relation_cap || record.raw_payload_bytes == 0 ||
        record.raw_payload_bytes > record.raw_payload_cap_bytes ||
        record.adapter_input_relations != record.raw_relations ||
        record.adapter_accepted_two_lp != 0 || record.adapter_malformed_source_shape != 0 ||
        record.adapter_unsupported_encoding != 0 || record.adapter_invalid_one_large_prime != 0 ||
        record.adapter_invalid_two_large_prime_split != 0) {
        return false;
    }
    std::size_t dispositions = 0;
    std::size_t rejection_taxonomy = 0;
    return checked_add(record.adapter_full_relations, record.adapter_accepted_one_lp,
                       dispositions) &&
           checked_add(dispositions, record.adapter_accepted_two_lp, dispositions) &&
           checked_add(dispositions, record.adapter_rejected_relations, dispositions) &&
           dispositions == record.adapter_input_relations &&
           checked_add(record.adapter_malformed_source_shape, record.adapter_unsupported_encoding,
                       rejection_taxonomy) &&
           checked_add(rejection_taxonomy, record.adapter_invalid_one_large_prime,
                       rejection_taxonomy) &&
           checked_add(rejection_taxonomy, record.adapter_invalid_two_large_prime_split,
                       rejection_taxonomy) &&
           checked_add(rejection_taxonomy, record.adapter_exact_duplicate, rejection_taxonomy) &&
           rejection_taxonomy == record.adapter_rejected_relations;
}

[[nodiscard]] constexpr bool
observe_graph_is_valid(const siqs::SIQSShadowProofObserveRecord& record) noexcept {
    if (!record.graph_evidence_supported || record.graph_edges != record.adapter_accepted_one_lp ||
        record.graph_components > record.graph_vertices ||
        record.graph_cycles > record.graph_edges || record.graph_edges > record.graph_edge_cap ||
        record.graph_cycles > record.graph_cycle_cap ||
        record.graph_cycle_incidences > record.graph_incidence_cap) {
        return false;
    }
    std::size_t edge_components = 0;
    std::size_t vertex_cycles = 0;
    if (!checked_add(record.graph_edges, record.graph_components, edge_components) ||
        !checked_add(record.graph_vertices, record.graph_cycles, vertex_cycles) ||
        edge_components != vertex_cycles) {
        return false;
    }
    if (record.graph_edges == 0) {
        if (record.graph_vertices != 0 || record.graph_components != 0 ||
            record.graph_cycles != 0 || record.graph_cycle_incidences != 0 ||
            record.graph_max_cycle_length != 0) {
            return false;
        }
    } else if (record.graph_components != 1) {
        return false;
    }
    if (record.graph_cycles == 0) {
        if (record.graph_cycle_incidences != 0 || record.graph_max_cycle_length != 0) {
            return false;
        }
    } else {
        std::size_t twice_cycles = 0;
        std::size_t maximum_incidences = 0;
        if (!checked_double(record.graph_cycles, twice_cycles) ||
            !checked_multiply(record.graph_cycles, record.graph_max_cycle_length,
                              maximum_incidences) ||
            record.graph_cycle_incidences < twice_cycles || record.graph_max_cycle_length < 2 ||
            record.graph_max_cycle_length > record.graph_cycle_incidences ||
            record.graph_cycle_incidences > maximum_incidences) {
            return false;
        }
        // The frozen production profile admits one-large-prime relations only.
        // Every edge therefore meets the sentinel vertex, and each fundamental
        // cycle is exactly one pair of parallel edges.
        if (record.graph_cycle_incidences != twice_cycles || record.graph_max_cycle_length != 2) {
            return false;
        }
    }
    std::size_t expected_row_upper = 0;
    return checked_add(record.adapter_full_relations, record.graph_cycles, expected_row_upper) &&
           record.row_candidate_upper == expected_row_upper &&
           record.row_candidate_upper <= record.row_candidate_cap;
}

[[nodiscard]] constexpr bool
observe_assembly_is_valid(const siqs::SIQSShadowProofObserveRecord& record) noexcept {
    std::size_t selected_and_trimmed = 0;
    std::size_t selected_origins = 0;
    return record.assembly_evidence_supported && record.assembly_fingerprint_supported &&
           record.assembly_pretrim_rows <= record.row_candidate_upper &&
           record.assembly_pretrim_rows <= record.pretrim_row_cap &&
           checked_add(record.assembly_selected_rows, record.assembly_trimmed_rows,
                       selected_and_trimmed) &&
           selected_and_trimmed == record.assembly_pretrim_rows &&
           checked_add(record.assembly_selected_full_rows, record.assembly_selected_cycle_rows,
                       selected_origins) &&
           selected_origins == record.assembly_selected_rows &&
           record.assembly_selected_full_rows <= record.adapter_full_relations &&
           record.assembly_selected_cycle_rows <= record.graph_cycles &&
           record.assembly_selected_rows == SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS &&
           fingerprint_is_nonzero(record.assembly_source_fingerprint_low,
                                  record.assembly_source_fingerprint_high) &&
           fingerprint_is_nonzero(record.assembly_pretrim_fingerprint_low,
                                  record.assembly_pretrim_fingerprint_high) &&
           fingerprint_is_nonzero(record.assembly_selected_fingerprint_low,
                                  record.assembly_selected_fingerprint_high);
}

[[nodiscard]] constexpr bool
observe_matrix_is_valid(const siqs::SIQSShadowProofObserveRecord& record) noexcept {
    if (!record.projected_dense_bytes_supported || !record.matrix_evidence_supported ||
        record.matrix_rows != record.assembly_selected_rows ||
        record.matrix_columns != record.factor_base_columns ||
        record.matrix_rows <= record.matrix_columns ||
        record.minimum_nullity != record.matrix_rows - record.matrix_columns ||
        record.matrix_rows > record.matrix_dense_variable_cap) {
        return false;
    }
    const auto projected =
        siqs::checked_siqs_shadow_dense_matrix_bytes(record.matrix_rows, record.matrix_columns);
    return projected.has_value() && record.projected_dense_bytes == *projected &&
           record.projected_dense_bytes <= record.matrix_dense_bytes_cap;
}

[[nodiscard]] constexpr bool
observe_dependency_is_valid(const siqs::SIQSShadowProofObserveRecord& record) noexcept {
    std::size_t outcomes = 0;
    std::size_t winning_plus_one = 0;
    return record.dependencies_returned == record.matrix_max_dependencies &&
           record.dependencies_returned != 0 && record.dependencies_examined != 0 &&
           record.dependencies_examined <= record.dependencies_returned &&
           record.dependencies_verified == record.dependencies_examined &&
           checked_add(record.no_factor_count, record.factor_found_count, outcomes) &&
           outcomes == record.dependencies_verified && record.factor_found_count == 1 &&
           record.dependency_cap_reached && record.dependency_fingerprint_supported &&
           fingerprint_is_nonzero(record.dependency_fingerprint_low,
                                  record.dependency_fingerprint_high) &&
           !record.first_failed_dependency_supported && record.first_failed_dependency == 0 &&
           record.winning_dependency_supported &&
           checked_add(record.winning_dependency, std::size_t{1}, winning_plus_one) &&
           record.winning_dependency == record.no_factor_count &&
           winning_plus_one == record.dependencies_examined &&
           record.winning_dependency < record.dependencies_returned &&
           record.winning_dependency_size_supported && record.winning_dependency_size != 0 &&
           record.winning_dependency_size <= record.matrix_rows;
}

[[nodiscard]] constexpr bool
observe_memory_is_valid(const siqs::SIQSShadowProofObserveRecord& record,
                        util::ProcessMemoryBackend expected_backend) noexcept {
    if (record.before_memory.backend != expected_backend ||
        record.after_memory.backend != expected_backend) {
        return false;
    }
    const auto before_peak = record.before_memory.lifetime_peak_rss_bytes;
    const auto after_peak = record.after_memory.lifetime_peak_rss_bytes;
    return !before_peak.has_value() || !after_peak.has_value() || *before_peak <= *after_peak;
}

inline void append_field(std::string& output, std::string_view key, std::string_view value) {
    output.push_back(' ');
    output.append(key);
    output.push_back('=');
    output.append(value);
}

inline void append_u64(std::string& output, std::string_view key, uint64_t value) {
    char buffer[std::numeric_limits<uint64_t>::digits10 + 2]{};
    const auto [end, status] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (status != std::errc{}) {
        throw std::bad_alloc();
    }
    append_field(output, key, std::string_view(buffer, static_cast<std::size_t>(end - buffer)));
}

inline void append_digest(std::string& output, std::string_view prefix,
                          siqs::SIQSShadowProofRssCorpusDigest digest) {
    std::string low_key(prefix);
    low_key.append("_low");
    append_u64(output, low_key, digest.low);
    std::string high_key(prefix);
    high_key.append("_high");
    append_u64(output, high_key, digest.high);
}

[[nodiscard]] inline std::string
canonical_projection(const siqs::SIQSShadowProofRssGatePolicy& policy,
                     const siqs::SIQSShadowProofRssCampaignRuntimeFacts& facts,
                     const siqs::SIQSShadowProofRssCampaignSlot& slot,
                     const SIQSShadowProofRssHoldoutProbeDecodedRecord& probe,
                     siqs::SIQSShadowProofRssEvidence evidence,
                     const SIQSShadowProofRssUncommittedArtifactFingerprint& stdout_fingerprint,
                     const SIQSShadowProofRssUncommittedArtifactFingerprint& stderr_fingerprint) {
    std::string output;
    output.reserve(1024);
    output.append(SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_PREFIX);
    append_u64(output, "schema_version", SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_SCHEMA_VERSION);
    append_field(output, "status", "validated");
    append_field(output, "authority", "uncommitted");
    append_digest(output, "policy_binding_digest",
                  siqs::siqs_shadow_proof_rss_policy_binding_digest(policy));
    append_u64(output, "slot_number", slot.slot_number);
    append_u64(output, "fixture_id", slot.fixture_id);
    append_field(output, "mode", siqs::siqs_shadow_proof_rss_sample_mode_name(slot.mode));
    append_u64(output, "ordinal", slot.ordinal);
    append_field(output, "operating_system",
                 siqs::siqs_shadow_proof_rss_operating_system_name(facts.operating_system));
    append_field(output, "architecture",
                 siqs::siqs_shadow_proof_rss_architecture_name(facts.architecture));
    append_field(output, "rss_backend", util::process_memory_backend_name(facts.memory_backend));
    append_u64(output, "resolved_production_sieve_workers",
               static_cast<uint64_t>(facts.resolved_production_sieve_workers));
    append_field(output, "candidate_revision", facts.candidate_revision);
    append_field(output, "fresh_process", "true");
    append_field(output, "completed", "true");
    append_field(output, "factor_identity", "pass");
    append_field(output, "proof_evidence", siqs::siqs_shadow_proof_rss_evidence_name(evidence));
    append_field(output, "matrix_evidence", siqs::siqs_shadow_proof_rss_evidence_name(evidence));
    append_u64(output, "relations_found", probe.relations_found);
    append_u64(output, "polynomials_used", probe.polynomials_used);
    append_u64(output, "absolute_peak_rss_bytes", probe.absolute_peak_rss_bytes);
    append_field(output, "current_rss_supported",
                 probe.after_memory.current_rss_bytes.has_value() ? "true" : "false");
    append_u64(output, "current_rss_bytes", probe.after_memory.current_rss_bytes.value_or(0));
    append_field(output, "peak_growth_supported",
                 probe.peak_growth_bytes.has_value() ? "true" : "false");
    append_u64(output, "peak_growth_bytes", probe.peak_growth_bytes.value_or(0));
    append_u64(output, "wall_ns", probe.factor_wall_ns);
    append_u64(output, "stdout_byte_count", stdout_fingerprint.byte_count);
    append_digest(output, "stdout_digest", stdout_fingerprint.digest);
    append_u64(output, "stderr_byte_count", stderr_fingerprint.byte_count);
    append_digest(output, "stderr_digest", stderr_fingerprint.digest);
    append_field(output, "route", "none");
    append_field(output, "promotion", "false");
    output.push_back('\n');
    return output;
}

} // namespace siqs_shadow_proof_rss_holdout_stream_join_detail

/// Validate two already-collected byte streams against one canonical campaign
/// slot. This pure overload does not prove that the streams came from one
/// process. A future launcher must supply that capability before persistence.
[[nodiscard]] inline SIQSShadowProofRssHoldoutStreamJoinResult
join_siqs_shadow_proof_rss_holdout_streams(
    const siqs::SIQSShadowProofRssGatePolicy* policy,
    const siqs::SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts,
    const siqs::SIQSShadowProofRssCampaignSlot* slot, std::string_view stdout_bytes,
    std::string_view stderr_bytes) noexcept {
    using Error = SIQSShadowProofRssHoldoutStreamJoinError;
    using namespace siqs_shadow_proof_rss_holdout_stream_join_detail;

    if (policy == nullptr || siqs::siqs_shadow_proof_rss_gate_policy_error(policy).has_value()) {
        return failure(Error::policy_invalid);
    }
    if (runtime_facts == nullptr ||
        !siqs::shadow_proof_rss_campaign_journal_detail::runtime_facts_are_valid(*runtime_facts)) {
        return failure(Error::runtime_facts_invalid);
    }
    if (!siqs::shadow_proof_rss_campaign_journal_detail::runtime_facts_match_policy(*runtime_facts,
                                                                                    *policy)) {
        return failure(Error::runtime_facts_mismatch);
    }
    if (!runtime_facts->release_build || !runtime_facts->ndebug) {
        return failure(Error::release_ndebug_required);
    }
    if (slot == nullptr) {
        return failure(Error::slot_invalid);
    }
    const auto plan = siqs::make_siqs_shadow_proof_rss_campaign_plan(policy);
    if (plan.status != siqs::SIQSShadowProofRssCampaignPlanStatus::ready ||
        slot->slot_number == 0 || slot->slot_number > plan.slot_count ||
        *slot != plan.slots[slot->slot_number - 1]) {
        return failure(Error::slot_invalid);
    }

    const auto stdout_result = decode_siqs_shadow_proof_rss_holdout_probe_record(stdout_bytes);
    if (!stdout_result) {
        auto result = failure(Error::stdout_invalid);
        result.stdout_diagnostic = stdout_result.diagnostic();
        return result;
    }
    const auto& probe = *stdout_result.decoded();
    if (probe.fixture_id != slot->fixture_id || gate_mode(probe.mode) != slot->mode ||
        probe.ordinal != slot->ordinal || probe.memory_backend != runtime_facts->memory_backend ||
        probe.relations_found != SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS ||
        probe.resolved_production_sieve_workers !=
            static_cast<uint64_t>(runtime_facts->resolved_production_sieve_workers)) {
        return failure(Error::stdout_binding_mismatch);
    }

    siqs::SIQSShadowProofRssEvidence evidence = siqs::SIQSShadowProofRssEvidence::not_applicable;
    if (slot->mode == siqs::SIQSShadowProofRssSampleMode::off) {
        if (!stderr_bytes.empty()) {
            return failure(Error::stderr_presence_invalid);
        }
    } else {
        if (slot->mode != siqs::SIQSShadowProofRssSampleMode::observe || stderr_bytes.empty()) {
            return failure(Error::stderr_presence_invalid);
        }
        const auto stderr_result = siqs::parse_siqs_shadow_proof_observe_record(stderr_bytes);
        if (!stderr_result) {
            auto result = failure(Error::stderr_invalid);
            result.stderr_diagnostic = stderr_result.diagnostic;
            return result;
        }
        const auto& observe = *stderr_result.record;
        if (!observe.proof_attempted ||
            observe.terminal_status != siqs::SIQSShadowProofTerminalStatus::factor_found ||
            observe.stage != siqs::SIQSShadowProofStage::factor_extraction ||
            observe.fallback_reason != siqs::SIQSShadowProofFallbackReason::none ||
            !observe.factor_found || observe.observe_wall_ns == 0) {
            return failure(Error::observe_terminal_invalid);
        }
        if (!observe_profile_is_valid(observe, slot->fixture_id)) {
            return failure(Error::observe_profile_invalid);
        }
        if (!observe_adapter_is_valid(observe)) {
            return failure(Error::observe_adapter_invalid);
        }
        if (!observe_graph_is_valid(observe)) {
            return failure(Error::observe_graph_invalid);
        }
        if (!observe_assembly_is_valid(observe)) {
            return failure(Error::observe_assembly_invalid);
        }
        if (!observe_matrix_is_valid(observe)) {
            return failure(Error::observe_matrix_invalid);
        }
        if (!observe_dependency_is_valid(observe)) {
            return failure(Error::observe_dependency_invalid);
        }
        if (!observe_memory_is_valid(observe, runtime_facts->memory_backend)) {
            return failure(Error::observe_memory_invalid);
        }
        if (probe.relations_found != static_cast<uint64_t>(observe.matrix_rows) ||
            observe.observe_wall_ns > probe.factor_wall_ns) {
            return failure(Error::cross_stream_mismatch);
        }
        const auto outer_before_peak = probe.before_memory.lifetime_peak_rss_bytes;
        const auto inner_before_peak = observe.before_memory.lifetime_peak_rss_bytes;
        const auto inner_after_peak = observe.after_memory.lifetime_peak_rss_bytes;
        if ((outer_before_peak.has_value() && inner_before_peak.has_value() &&
             *outer_before_peak > *inner_before_peak) ||
            (outer_before_peak.has_value() && inner_after_peak.has_value() &&
             *outer_before_peak > *inner_after_peak) ||
            (inner_after_peak.has_value() && *inner_after_peak > probe.absolute_peak_rss_bytes)) {
            return failure(Error::cross_stream_mismatch);
        }
        evidence = siqs::SIQSShadowProofRssEvidence::pass;
    }

    try {
        const auto stdout_fingerprint =
            fingerprint(siqs::SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes);
        const auto stderr_fingerprint =
            fingerprint(siqs::SIQSShadowProofRssArtifactKind::probe_stderr, stderr_bytes);
        SIQSShadowProofRssUncommittedSampleDraft draft;
        draft.policy_binding_digest = siqs::siqs_shadow_proof_rss_policy_binding_digest(*policy);
        draft.slot_number = slot->slot_number;
        draft.fixture_id = slot->fixture_id;
        draft.mode = slot->mode;
        draft.ordinal = slot->ordinal;
        draft.operating_system = runtime_facts->operating_system;
        draft.architecture = runtime_facts->architecture;
        draft.memory_backend = runtime_facts->memory_backend;
        draft.resolved_production_sieve_workers = runtime_facts->resolved_production_sieve_workers;
        draft.fresh_process = true;
        draft.completed = true;
        draft.factor_identity = siqs::SIQSShadowProofRssFactorIdentity::pass;
        draft.proof_evidence = evidence;
        draft.matrix_evidence = evidence;
        draft.relations_found = probe.relations_found;
        draft.polynomials_used = probe.polynomials_used;
        draft.absolute_peak_rss_bytes = probe.absolute_peak_rss_bytes;
        draft.current_rss_bytes = probe.after_memory.current_rss_bytes;
        draft.peak_growth_bytes = probe.peak_growth_bytes;
        draft.wall_ns = probe.factor_wall_ns;
        draft.stdout_fingerprint = stdout_fingerprint;
        draft.stderr_fingerprint = stderr_fingerprint;
        draft.stdout_bytes.assign(stdout_bytes);
        draft.stderr_bytes.assign(stderr_bytes);
        draft.joined_bytes = canonical_projection(*policy, *runtime_facts, *slot, probe, evidence,
                                                  stdout_fingerprint, stderr_fingerprint);

        SIQSShadowProofRssHoldoutStreamJoinResult result;
        result.draft.emplace(std::move(draft));
        return result;
    } catch (...) {
        return failure(Error::allocation_failure);
    }
}

} // namespace gnfs::siqs::shadow_proof_rss_holdout_detail
