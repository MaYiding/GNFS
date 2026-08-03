#pragma once

/// @file shadow_proof_prefer.hpp
/// @brief Pure, fail-closed SIQS shadow-proof prefer decision contract.

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/shadow_proof_runner.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace gnfs::siqs {

inline constexpr char SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX[] =
    "GNFS_SIQS_SHADOW_PROOF_PREFER_DECISION_V2";

enum class SIQSShadowProofPreferDecisionKind : uint8_t {
    shadow_candidate,
    legacy_fallback,
};

enum class SIQSShadowProofPreferReason : uint8_t {
    shadow_factor_valid,
    shadow_not_factor,
    shadow_contract_invalid,
    factor_identity_invalid,
    result_metadata_invalid,
    decision_internal_failure,
};

enum class SIQSShadowProofPreferFactorIdentity : uint8_t {
    pass,
    fail,
    not_checked,
};

/// Non-owning view used only during evaluate(). The owning draft never retains
/// these pointers, so tests can synthesize defensive-boundary states without
/// weakening SIQSShadowProofResult's invariant-safe constructors.
struct SIQSShadowProofPreferSourceView final {
    SIQSShadowProofTerminalStatus terminal_status =
        SIQSShadowProofTerminalStatus::internal_invariant_failure;
    SIQSShadowProofStage stage = SIQSShadowProofStage::not_started;
    SIQSShadowProofFallbackReason fallback_reason = SIQSShadowProofFallbackReason::none;
    const SIQSShadowProofEvidence* evidence = nullptr;
    const SIQSPostMergeFactorization* factorization = nullptr;
};

struct SIQSShadowProofPreferAcceptedFactor final {
    SIQSPostMergeFactorization factorization;
    size_t relations_found = 0;
    size_t polynomials_used = 0;
};

/// Owning result of factor/evidence evaluation before the caller samples the
/// SIQS decision wall clock. Only a valid shadow candidate owns factors.
struct SIQSShadowProofPreferDraft final {
    SIQSShadowProofPreferDecisionKind decision = SIQSShadowProofPreferDecisionKind::legacy_fallback;
    SIQSShadowProofPreferReason reason = SIQSShadowProofPreferReason::decision_internal_failure;
    SIQSShadowProofTerminalStatus shadow_terminal =
        SIQSShadowProofTerminalStatus::internal_invariant_failure;
    SIQSShadowProofStage shadow_stage = SIQSShadowProofStage::not_started;
    SIQSShadowProofFallbackReason shadow_fallback = SIQSShadowProofFallbackReason::none;
    bool factorization_present = false;
    SIQSShadowProofPreferFactorIdentity factor_identity =
        SIQSShadowProofPreferFactorIdentity::not_checked;
    std::optional<SIQSShadowProofPreferAcceptedFactor> accepted;
};

struct SIQSShadowProofPreferCandidate final {
    SIQSPostMergeFactorization factorization;
    size_t relations_found = 0;
    size_t polynomials_used = 0;
    uint64_t decision_wall_ns = 0;
    double time_seconds = 0.0;
};

/// Final immutable-by-convention decision consumed by the V2 emitter. The
/// production adapter may follow next_route=shadow_return only after
/// successful emission.
struct SIQSShadowProofPreferDecision final {
    SIQSShadowProofPreferDecisionKind decision = SIQSShadowProofPreferDecisionKind::legacy_fallback;
    SIQSShadowProofPreferReason reason = SIQSShadowProofPreferReason::decision_internal_failure;
    SIQSShadowProofTerminalStatus shadow_terminal =
        SIQSShadowProofTerminalStatus::internal_invariant_failure;
    SIQSShadowProofStage shadow_stage = SIQSShadowProofStage::not_started;
    SIQSShadowProofFallbackReason shadow_fallback = SIQSShadowProofFallbackReason::none;
    bool factorization_present = false;
    SIQSShadowProofPreferFactorIdentity factor_identity =
        SIQSShadowProofPreferFactorIdentity::not_checked;
    std::optional<SIQSShadowProofPreferCandidate> candidate;

    [[nodiscard]] bool is_shadow_candidate() const noexcept {
        return decision == SIQSShadowProofPreferDecisionKind::shadow_candidate &&
               candidate.has_value();
    }
};

[[nodiscard]] inline SIQSShadowProofPreferSourceView
make_siqs_shadow_proof_prefer_source_view(const SIQSShadowProofResult& result) noexcept {
    const auto& factorization = result.factorization();
    return SIQSShadowProofPreferSourceView{
        result.status(),
        result.stage(),
        result.fallback_reason(),
        &result.evidence(),
        factorization ? &*factorization : nullptr,
    };
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_prefer_decision_name(SIQSShadowProofPreferDecisionKind decision) noexcept {
    switch (decision) {
    case SIQSShadowProofPreferDecisionKind::shadow_candidate:
        return "shadow_candidate";
    case SIQSShadowProofPreferDecisionKind::legacy_fallback:
        return "legacy_fallback";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_prefer_reason_name(SIQSShadowProofPreferReason reason) noexcept {
    switch (reason) {
    case SIQSShadowProofPreferReason::shadow_factor_valid:
        return "shadow_factor_valid";
    case SIQSShadowProofPreferReason::shadow_not_factor:
        return "shadow_not_factor";
    case SIQSShadowProofPreferReason::shadow_contract_invalid:
        return "shadow_contract_invalid";
    case SIQSShadowProofPreferReason::factor_identity_invalid:
        return "factor_identity_invalid";
    case SIQSShadowProofPreferReason::result_metadata_invalid:
        return "result_metadata_invalid";
    case SIQSShadowProofPreferReason::decision_internal_failure:
        return "decision_internal_failure";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_prefer_factor_identity_name(
    SIQSShadowProofPreferFactorIdentity identity) noexcept {
    switch (identity) {
    case SIQSShadowProofPreferFactorIdentity::pass:
        return "pass";
    case SIQSShadowProofPreferFactorIdentity::fail:
        return "fail";
    case SIQSShadowProofPreferFactorIdentity::not_checked:
        return "not_checked";
    }
    return "unknown";
}

namespace shadow_proof_prefer_detail {

[[nodiscard]] constexpr bool known_decision(SIQSShadowProofPreferDecisionKind decision) noexcept {
    switch (decision) {
    case SIQSShadowProofPreferDecisionKind::shadow_candidate:
    case SIQSShadowProofPreferDecisionKind::legacy_fallback:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool known_reason(SIQSShadowProofPreferReason reason) noexcept {
    switch (reason) {
    case SIQSShadowProofPreferReason::shadow_factor_valid:
    case SIQSShadowProofPreferReason::shadow_not_factor:
    case SIQSShadowProofPreferReason::shadow_contract_invalid:
    case SIQSShadowProofPreferReason::factor_identity_invalid:
    case SIQSShadowProofPreferReason::result_metadata_invalid:
    case SIQSShadowProofPreferReason::decision_internal_failure:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool
known_factor_identity(SIQSShadowProofPreferFactorIdentity identity) noexcept {
    switch (identity) {
    case SIQSShadowProofPreferFactorIdentity::pass:
    case SIQSShadowProofPreferFactorIdentity::fail:
    case SIQSShadowProofPreferFactorIdentity::not_checked:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool known_terminal(SIQSShadowProofTerminalStatus status) noexcept {
    switch (status) {
    case SIQSShadowProofTerminalStatus::factor_found:
    case SIQSShadowProofTerminalStatus::no_factor:
    case SIQSShadowProofTerminalStatus::bounded_fallback:
    case SIQSShadowProofTerminalStatus::invalid_input:
    case SIQSShadowProofTerminalStatus::stage_failure:
    case SIQSShadowProofTerminalStatus::resource_exhausted:
    case SIQSShadowProofTerminalStatus::exception_failure:
    case SIQSShadowProofTerminalStatus::internal_invariant_failure:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool known_stage(SIQSShadowProofStage stage) noexcept {
    switch (stage) {
    case SIQSShadowProofStage::not_started:
    case SIQSShadowProofStage::input_validation:
    case SIQSShadowProofStage::payload_accounting:
    case SIQSShadowProofStage::adapter_preflight:
    case SIQSShadowProofStage::graph_preflight:
    case SIQSShadowProofStage::assembly:
    case SIQSShadowProofStage::matrix:
    case SIQSShadowProofStage::dependency_verification:
    case SIQSShadowProofStage::factor_extraction:
    case SIQSShadowProofStage::complete:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool known_fallback(SIQSShadowProofFallbackReason fallback) noexcept {
    switch (fallback) {
    case SIQSShadowProofFallbackReason::none:
    case SIQSShadowProofFallbackReason::raw_relation_limit:
    case SIQSShadowProofFallbackReason::raw_payload_limit:
    case SIQSShadowProofFallbackReason::graph_edge_limit:
    case SIQSShadowProofFallbackReason::graph_cycle_limit:
    case SIQSShadowProofFallbackReason::graph_incidence_limit:
    case SIQSShadowProofFallbackReason::row_candidate_limit:
    case SIQSShadowProofFallbackReason::pretrim_row_limit:
    case SIQSShadowProofFallbackReason::insufficient_rows:
    case SIQSShadowProofFallbackReason::matrix_resource_limit:
    case SIQSShadowProofFallbackReason::matrix_backend_unavailable:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr std::string_view
terminal_name(SIQSShadowProofTerminalStatus status) noexcept {
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

[[nodiscard]] constexpr std::string_view stage_name(SIQSShadowProofStage stage) noexcept {
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
fallback_name(SIQSShadowProofFallbackReason fallback) noexcept {
    switch (fallback) {
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

[[nodiscard]] constexpr bool checked_add(size_t lhs, size_t rhs, size_t& result) noexcept {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] inline bool
source_contract_is_consistent(const SIQSShadowProofPreferSourceView& source) noexcept {
    if (!known_terminal(source.terminal_status) || !known_stage(source.stage) ||
        !known_fallback(source.fallback_reason) || source.evidence == nullptr) {
        return false;
    }
    const bool is_factor = source.terminal_status == SIQSShadowProofTerminalStatus::factor_found;
    const bool is_fallback =
        source.terminal_status == SIQSShadowProofTerminalStatus::bounded_fallback;
    if (is_factor != (source.factorization != nullptr) ||
        is_fallback != (source.fallback_reason != SIQSShadowProofFallbackReason::none)) {
        return false;
    }
    if (source.terminal_status == SIQSShadowProofTerminalStatus::no_factor &&
        source.stage != SIQSShadowProofStage::complete) {
        return false;
    }
    if (source.terminal_status == SIQSShadowProofTerminalStatus::invalid_input &&
        source.stage != SIQSShadowProofStage::input_validation) {
        return false;
    }
    return !is_factor || source.stage == SIQSShadowProofStage::factor_extraction;
}

[[nodiscard]] inline bool
proof_evidence_is_consistent(const SIQSShadowProofEvidence& evidence) noexcept {
    if (!evidence.dependency_status ||
        *evidence.dependency_status != SIQSPostMergeDependencyStatus::valid ||
        !evidence.factor_status ||
        *evidence.factor_status != SIQSPostMergeFactorStatus::factor_found ||
        evidence.dependencies_returned == 0 || evidence.dependencies_examined == 0 ||
        evidence.dependencies_examined > evidence.dependencies_returned ||
        evidence.dependencies_verified != evidence.dependencies_examined ||
        evidence.factor_found_count != 1 || evidence.first_failed_dependency.has_value() ||
        !evidence.winning_dependency || !evidence.winning_dependency_size) {
        return false;
    }
    size_t outcomes = 0;
    if (!checked_add(evidence.no_factor_count, evidence.factor_found_count, outcomes) ||
        outcomes != evidence.dependencies_verified ||
        *evidence.winning_dependency >= evidence.dependencies_returned ||
        *evidence.winning_dependency == std::numeric_limits<size_t>::max() ||
        *evidence.winning_dependency + size_t{1} != evidence.dependencies_examined) {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool result_metadata_is_consistent(const SIQSShadowProofEvidence& evidence,
                                                        size_t polynomials_used) noexcept {
    if (!evidence.assembly_status || *evidence.assembly_status != SIQSShadowAssemblyStatus::valid ||
        evidence.assembly_limit_evidence.has_value() || !evidence.matrix_status ||
        *evidence.matrix_status != SIQSShadowMatrixStatus::valid || evidence.matrix_columns == 0 ||
        evidence.matrix_rows <= evidence.matrix_columns ||
        evidence.minimum_nullity != evidence.matrix_rows - evidence.matrix_columns ||
        evidence.assembly.selected_rows != evidence.matrix_rows || polynomials_used == 0 ||
        *evidence.winning_dependency_size == 0 ||
        *evidence.winning_dependency_size > evidence.matrix_rows) {
        return false;
    }
    size_t selected_sources = 0;
    size_t pretrim_rows = 0;
    return checked_add(evidence.assembly.selected_full_rows, evidence.assembly.selected_cycle_rows,
                       selected_sources) &&
           selected_sources == evidence.assembly.selected_rows &&
           checked_add(evidence.assembly.selected_rows, evidence.assembly.trimmed_rows,
                       pretrim_rows) &&
           pretrim_rows == evidence.assembly.pretrim_rows;
}

[[nodiscard]] inline SIQSShadowProofPreferDraft
fallback_draft(const SIQSShadowProofPreferSourceView& source, SIQSShadowProofPreferReason reason,
               SIQSShadowProofPreferFactorIdentity factor_identity =
                   SIQSShadowProofPreferFactorIdentity::not_checked) noexcept {
    return SIQSShadowProofPreferDraft{
        SIQSShadowProofPreferDecisionKind::legacy_fallback,
        reason,
        source.terminal_status,
        source.stage,
        source.fallback_reason,
        source.factorization != nullptr,
        factor_identity,
        std::nullopt,
    };
}

[[nodiscard]] constexpr bool
fallback_state_is_consistent(SIQSShadowProofPreferReason reason,
                             SIQSShadowProofTerminalStatus terminal, SIQSShadowProofStage stage,
                             SIQSShadowProofFallbackReason fallback, bool factorization_present,
                             SIQSShadowProofPreferFactorIdentity factor_identity) noexcept {
    switch (reason) {
    case SIQSShadowProofPreferReason::shadow_not_factor: {
        if (terminal == SIQSShadowProofTerminalStatus::factor_found || factorization_present ||
            factor_identity != SIQSShadowProofPreferFactorIdentity::not_checked) {
            return false;
        }
        const bool bounded_fallback = terminal == SIQSShadowProofTerminalStatus::bounded_fallback;
        if (bounded_fallback != (fallback != SIQSShadowProofFallbackReason::none)) {
            return false;
        }
        if (terminal == SIQSShadowProofTerminalStatus::no_factor &&
            stage != SIQSShadowProofStage::complete) {
            return false;
        }
        return terminal != SIQSShadowProofTerminalStatus::invalid_input ||
               stage == SIQSShadowProofStage::input_validation;
    }
    case SIQSShadowProofPreferReason::shadow_contract_invalid:
    case SIQSShadowProofPreferReason::decision_internal_failure:
        return factor_identity == SIQSShadowProofPreferFactorIdentity::not_checked;
    case SIQSShadowProofPreferReason::factor_identity_invalid:
        return terminal == SIQSShadowProofTerminalStatus::factor_found &&
               stage == SIQSShadowProofStage::factor_extraction &&
               fallback == SIQSShadowProofFallbackReason::none && factorization_present &&
               factor_identity == SIQSShadowProofPreferFactorIdentity::fail;
    case SIQSShadowProofPreferReason::result_metadata_invalid:
        return terminal == SIQSShadowProofTerminalStatus::factor_found &&
               stage == SIQSShadowProofStage::factor_extraction &&
               fallback == SIQSShadowProofFallbackReason::none && factorization_present &&
               (factor_identity == SIQSShadowProofPreferFactorIdentity::not_checked ||
                factor_identity == SIQSShadowProofPreferFactorIdentity::pass);
    case SIQSShadowProofPreferReason::shadow_factor_valid:
        return false;
    }
    return false;
}

[[nodiscard]] inline bool draft_is_consistent(const SIQSShadowProofPreferDraft& draft) noexcept {
    if (!known_decision(draft.decision) || !known_reason(draft.reason) ||
        !known_terminal(draft.shadow_terminal) || !known_stage(draft.shadow_stage) ||
        !known_fallback(draft.shadow_fallback) || !known_factor_identity(draft.factor_identity)) {
        return false;
    }
    if (draft.decision == SIQSShadowProofPreferDecisionKind::shadow_candidate) {
        return draft.reason == SIQSShadowProofPreferReason::shadow_factor_valid &&
               draft.shadow_terminal == SIQSShadowProofTerminalStatus::factor_found &&
               draft.shadow_stage == SIQSShadowProofStage::factor_extraction &&
               draft.shadow_fallback == SIQSShadowProofFallbackReason::none &&
               draft.factorization_present &&
               draft.factor_identity == SIQSShadowProofPreferFactorIdentity::pass &&
               draft.accepted.has_value() && draft.accepted->relations_found > 0 &&
               draft.accepted->polynomials_used > 0;
    }
    return draft.decision == SIQSShadowProofPreferDecisionKind::legacy_fallback &&
           !draft.accepted.has_value() &&
           fallback_state_is_consistent(draft.reason, draft.shadow_terminal, draft.shadow_stage,
                                        draft.shadow_fallback, draft.factorization_present,
                                        draft.factor_identity);
}

[[nodiscard]] inline bool decision_is_consistent(const SIQSShadowProofPreferDecision& decision,
                                                 const core::Integer& original_n) {
    if (!known_decision(decision.decision) || !known_reason(decision.reason) ||
        !known_terminal(decision.shadow_terminal) || !known_stage(decision.shadow_stage) ||
        !known_fallback(decision.shadow_fallback) ||
        !known_factor_identity(decision.factor_identity)) {
        return false;
    }
    if (decision.decision == SIQSShadowProofPreferDecisionKind::shadow_candidate) {
        if (decision.reason != SIQSShadowProofPreferReason::shadow_factor_valid ||
            decision.shadow_terminal != SIQSShadowProofTerminalStatus::factor_found ||
            decision.shadow_stage != SIQSShadowProofStage::factor_extraction ||
            decision.shadow_fallback != SIQSShadowProofFallbackReason::none ||
            !decision.factorization_present ||
            decision.factor_identity != SIQSShadowProofPreferFactorIdentity::pass ||
            !decision.candidate || decision.candidate->relations_found == 0 ||
            decision.candidate->polynomials_used == 0 ||
            decision.candidate->decision_wall_ns == 0 ||
            !std::isfinite(decision.candidate->time_seconds) ||
            decision.candidate->time_seconds <= 0.0) {
            return false;
        }
        const auto& factors = decision.candidate->factorization;
        if (!original_n.is_positive() || original_n.is_one() || !factors.factor.is_positive() ||
            factors.factor.is_one() || !factors.cofactor.is_positive() ||
            factors.cofactor.is_one() || factors.factor > factors.cofactor ||
            factors.factor >= original_n || factors.cofactor >= original_n ||
            factors.factor * factors.cofactor != original_n) {
            return false;
        }
        const double expected_seconds =
            static_cast<double>(decision.candidate->decision_wall_ns) / 1'000'000'000.0;
        return decision.candidate->time_seconds == expected_seconds;
    }
    return decision.decision == SIQSShadowProofPreferDecisionKind::legacy_fallback &&
           !decision.candidate.has_value() &&
           fallback_state_is_consistent(decision.reason, decision.shadow_terminal,
                                        decision.shadow_stage, decision.shadow_fallback,
                                        decision.factorization_present, decision.factor_identity);
}

[[nodiscard]] constexpr const char* bool_name(bool value) noexcept {
    return value ? "true" : "false";
}

} // namespace shadow_proof_prefer_detail

/// Defensively evaluate an untrusted typed proof view. No pointer from source
/// escapes; a candidate owns copied GMP factors. All exceptions become legacy.
[[nodiscard]] inline SIQSShadowProofPreferDraft
evaluate_siqs_shadow_proof_prefer(const SIQSShadowProofPreferSourceView& source,
                                  const core::Integer& original_n,
                                  size_t polynomials_used) noexcept {
    using namespace shadow_proof_prefer_detail;
    try {
        if (!source_contract_is_consistent(source)) {
            return fallback_draft(source, SIQSShadowProofPreferReason::shadow_contract_invalid);
        }
        if (source.terminal_status != SIQSShadowProofTerminalStatus::factor_found) {
            return fallback_draft(source, SIQSShadowProofPreferReason::shadow_not_factor);
        }
        if (!proof_evidence_is_consistent(*source.evidence)) {
            return fallback_draft(source, SIQSShadowProofPreferReason::shadow_contract_invalid);
        }
        if (!result_metadata_is_consistent(*source.evidence, polynomials_used)) {
            return fallback_draft(source, SIQSShadowProofPreferReason::result_metadata_invalid);
        }

        const SIQSPostMergeFactorization& factors = *source.factorization;
        if (!original_n.is_positive() || original_n.is_one() || !factors.factor.is_positive() ||
            factors.factor.is_one() || !factors.cofactor.is_positive() ||
            factors.cofactor.is_one() || factors.factor > factors.cofactor ||
            factors.factor >= original_n || factors.cofactor >= original_n ||
            factors.factor * factors.cofactor != original_n) {
            return fallback_draft(source, SIQSShadowProofPreferReason::factor_identity_invalid,
                                  SIQSShadowProofPreferFactorIdentity::fail);
        }

        SIQSShadowProofPreferAcceptedFactor accepted{
            SIQSPostMergeFactorization{factors.factor, factors.cofactor},
            source.evidence->matrix_rows,
            polynomials_used,
        };
        return SIQSShadowProofPreferDraft{
            SIQSShadowProofPreferDecisionKind::shadow_candidate,
            SIQSShadowProofPreferReason::shadow_factor_valid,
            source.terminal_status,
            source.stage,
            source.fallback_reason,
            true,
            SIQSShadowProofPreferFactorIdentity::pass,
            std::move(accepted),
        };
    } catch (const std::bad_alloc&) {
        return shadow_proof_prefer_detail::fallback_draft(
            source, SIQSShadowProofPreferReason::decision_internal_failure);
    } catch (...) {
        return shadow_proof_prefer_detail::fallback_draft(
            source, SIQSShadowProofPreferReason::decision_internal_failure);
    }
}

[[nodiscard]] inline SIQSShadowProofPreferDraft
evaluate_siqs_shadow_proof_prefer(const SIQSShadowProofResult& result,
                                  const core::Integer& original_n,
                                  size_t polynomials_used) noexcept {
    return evaluate_siqs_shadow_proof_prefer(make_siqs_shadow_proof_prefer_source_view(result),
                                             original_n, polynomials_used);
}

/// Attach the caller's post-evaluation, pre-emission wall sample. A zero sample
/// invalidates only the candidate metadata and remains a legacy fallback.
[[nodiscard]] inline SIQSShadowProofPreferDecision
finalize_siqs_shadow_proof_prefer(SIQSShadowProofPreferDraft&& draft,
                                  uint64_t decision_wall_ns) noexcept {
    using namespace shadow_proof_prefer_detail;
    if (!draft_is_consistent(draft)) {
        return SIQSShadowProofPreferDecision{
            SIQSShadowProofPreferDecisionKind::legacy_fallback,
            SIQSShadowProofPreferReason::decision_internal_failure,
            draft.shadow_terminal,
            draft.shadow_stage,
            draft.shadow_fallback,
            draft.factorization_present,
            SIQSShadowProofPreferFactorIdentity::not_checked,
            std::nullopt,
        };
    }
    if (draft.decision == SIQSShadowProofPreferDecisionKind::legacy_fallback) {
        return SIQSShadowProofPreferDecision{
            draft.decision,        draft.reason,          draft.shadow_terminal,
            draft.shadow_stage,    draft.shadow_fallback, draft.factorization_present,
            draft.factor_identity, std::nullopt,
        };
    }
    if (decision_wall_ns == 0) {
        return SIQSShadowProofPreferDecision{
            SIQSShadowProofPreferDecisionKind::legacy_fallback,
            SIQSShadowProofPreferReason::result_metadata_invalid,
            draft.shadow_terminal,
            draft.shadow_stage,
            draft.shadow_fallback,
            draft.factorization_present,
            draft.factor_identity,
            std::nullopt,
        };
    }

    const double time_seconds = static_cast<double>(decision_wall_ns) / 1'000'000'000.0;
    if (!std::isfinite(time_seconds) || time_seconds <= 0.0) {
        return SIQSShadowProofPreferDecision{
            SIQSShadowProofPreferDecisionKind::legacy_fallback,
            SIQSShadowProofPreferReason::result_metadata_invalid,
            draft.shadow_terminal,
            draft.shadow_stage,
            draft.shadow_fallback,
            draft.factorization_present,
            draft.factor_identity,
            std::nullopt,
        };
    }

    auto accepted = std::move(*draft.accepted);
    return SIQSShadowProofPreferDecision{
        SIQSShadowProofPreferDecisionKind::shadow_candidate,
        SIQSShadowProofPreferReason::shadow_factor_valid,
        draft.shadow_terminal,
        draft.shadow_stage,
        draft.shadow_fallback,
        true,
        SIQSShadowProofPreferFactorIdentity::pass,
        SIQSShadowProofPreferCandidate{
            std::move(accepted.factorization),
            accepted.relations_found,
            accepted.polynomials_used,
            decision_wall_ns,
            time_seconds,
        },
    };
}

/// Emit one closed V2 pre-route record. The record states next_route rather
/// than claiming the route occurred. A caller may commit that route only after
/// this function returns true.
[[nodiscard]] inline bool
emit_siqs_shadow_proof_prefer_decision(std::FILE* output, const core::Integer& original_n,
                                       const SIQSShadowProofPreferDecision& decision) noexcept {
    if (output == nullptr) {
        return false;
    }
    try {
        if (!shadow_proof_prefer_detail::decision_is_consistent(decision, original_n) ||
            std::ferror(output) != 0) {
            return false;
        }

        const bool candidate = decision.is_shadow_candidate();
        const std::string input_n = original_n.to_string();
        std::string factor = "0";
        std::string cofactor = "0";
        size_t relations_found = 0;
        size_t polynomials_used = 0;
        uint64_t decision_wall_ns = 0;
        if (candidate) {
            factor = decision.candidate->factorization.factor.to_string();
            cofactor = decision.candidate->factorization.cofactor.to_string();
            relations_found = decision.candidate->relations_found;
            polynomials_used = decision.candidate->polynomials_used;
            decision_wall_ns = decision.candidate->decision_wall_ns;
        }

        const std::string_view decision_name =
            siqs_shadow_proof_prefer_decision_name(decision.decision);
        const std::string_view reason_name = siqs_shadow_proof_prefer_reason_name(decision.reason);
        const std::string_view terminal_name =
            shadow_proof_prefer_detail::terminal_name(decision.shadow_terminal);
        const std::string_view stage_name =
            shadow_proof_prefer_detail::stage_name(decision.shadow_stage);
        const std::string_view fallback_name =
            shadow_proof_prefer_detail::fallback_name(decision.shadow_fallback);
        const std::string_view factor_identity =
            siqs_shadow_proof_prefer_factor_identity_name(decision.factor_identity);

        const int emitted = std::fprintf(
            output,
            "%s schema_version=2 status=valid mode=prefer decision=%.*s reason=%.*s"
            " next_route=%s shadow_terminal=%.*s shadow_stage=%.*s shadow_fallback=%.*s"
            " factorization_present=%s input_n=%s factor=%s cofactor=%s"
            " factor_identity=%.*s result_present=%s relations_found=%zu"
            " relations_source=%s polynomials_used=%zu polynomials_source=%s"
            " decision_wall_ns_supported=%s decision_wall_ns=%llu time_scope=%s"
            " emit_phase=before_route promotion=false\n",
            SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX, static_cast<int>(decision_name.size()),
            decision_name.data(), static_cast<int>(reason_name.size()), reason_name.data(),
            candidate ? "shadow_return" : "legacy_continue", static_cast<int>(terminal_name.size()),
            terminal_name.data(), static_cast<int>(stage_name.size()), stage_name.data(),
            static_cast<int>(fallback_name.size()), fallback_name.data(),
            shadow_proof_prefer_detail::bool_name(decision.factorization_present), input_n.c_str(),
            factor.c_str(), cofactor.c_str(), static_cast<int>(factor_identity.size()),
            factor_identity.data(), shadow_proof_prefer_detail::bool_name(candidate),
            relations_found, candidate ? "shadow_selected_rows" : "none", polynomials_used,
            candidate ? "production_sieve_counter" : "none",
            shadow_proof_prefer_detail::bool_name(candidate),
            static_cast<unsigned long long>(decision_wall_ns),
            candidate ? "siqs_timer_to_pre_emit_decision" : "unavailable");
        const int flush_status = std::fflush(output);
        const int stream_error = std::ferror(output);
        return emitted >= 0 && flush_status == 0 && stream_error == 0;
    } catch (...) {
        return false;
    }
}

} // namespace gnfs::siqs
