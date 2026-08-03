#pragma once

// Source-private, authority-free result boundary for one durable terminal
// gate transaction. This file is not installed as public API.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_store.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

class ApprovedTerminalGateBinding;
class TerminalGateOrchestrator;
class TerminalGateResultProjector;
class TerminalGateTestPeer;

enum class TerminalGateTransactionOutcome : std::uint8_t {
    admission_rejected,
    gate_not_ready,
    durable_outcome_confirmed,
    outcome_uncertain,
    reconcile_required,
};

[[nodiscard]] constexpr std::string_view
terminal_gate_transaction_outcome_name(TerminalGateTransactionOutcome outcome) noexcept {
    switch (outcome) {
    case TerminalGateTransactionOutcome::admission_rejected:
        return "admission_rejected";
    case TerminalGateTransactionOutcome::gate_not_ready:
        return "gate_not_ready";
    case TerminalGateTransactionOutcome::durable_outcome_confirmed:
        return "durable_outcome_confirmed";
    case TerminalGateTransactionOutcome::outcome_uncertain:
        return "outcome_uncertain";
    case TerminalGateTransactionOutcome::reconcile_required:
        return "reconcile_required";
    }
    return "unknown";
}

/// Copyable data emitted only after the exact terminal record was confirmed
/// durable under the same native lease that validated the complete journal.
/// It is evidence for manual review, never promotion or retry authority.
struct TerminalGateObservation final {
    SIQSShadowProofRssGateOutcome gate_outcome;
    SIQSShadowProofRssCorpusDigest plan_digest;
    SIQSShadowProofRssCorpusDigest terminal_journal_record_digest;
    SIQSShadowProofRssCorpusDigest terminal_gate_record_digest;

    [[nodiscard]] friend constexpr bool
    operator==(const TerminalGateObservation&, const TerminalGateObservation&) noexcept = default;
};

/// Authority-free terminal transaction result. Failed and uncertain outcomes
/// never carry an observation. The type deliberately exposes no samples,
/// policy, deployment row, path, session, retry, reopen, callback, gate
/// operation, routing, or promotion capability.
class TerminalGateTransactionResult final {
public:
    TerminalGateTransactionResult() = delete;
    TerminalGateTransactionResult(const TerminalGateTransactionResult&) = default;
    TerminalGateTransactionResult(TerminalGateTransactionResult&&) noexcept = default;
    TerminalGateTransactionResult& operator=(const TerminalGateTransactionResult&) = default;
    TerminalGateTransactionResult& operator=(TerminalGateTransactionResult&&) noexcept = default;

    [[nodiscard]] TerminalGateTransactionOutcome outcome() const noexcept {
        return outcome_;
    }

    [[nodiscard]] const std::optional<TerminalGateObservation>&
    confirmed_observation() const noexcept {
        return confirmed_observation_;
    }

    [[nodiscard]] const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
    store_diagnostic() const noexcept {
        return store_diagnostic_;
    }

private:
    TerminalGateTransactionResult(
        TerminalGateTransactionOutcome outcome,
        SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic) noexcept
        : outcome_(outcome), store_diagnostic_(std::move(diagnostic)) {}

    TerminalGateTransactionResult(TerminalGateTransactionOutcome outcome,
                                  TerminalGateObservation observation) noexcept
        : outcome_(outcome), confirmed_observation_(observation) {}

    TerminalGateTransactionOutcome outcome_;
    std::optional<TerminalGateObservation> confirmed_observation_;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic store_diagnostic_;

    friend class TerminalGateOrchestrator;
    friend class TerminalGateResultProjector;
    friend class TerminalGateTestPeer;
};

struct CoreTerminalGateResult final {
    TerminalGateTransactionOutcome outcome = TerminalGateTransactionOutcome::reconcile_required;
    std::optional<TerminalGateObservation> confirmed_observation;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
};

/// Pure fail-closed projection from a platform core result. Every expected
/// binding is supplied by the approved deployment selector, never the platform
/// core or the caller's claims.
class TerminalGateResultProjector final {
public:
    [[nodiscard]] static TerminalGateTransactionResult
    project(CoreTerminalGateResult core_result, SIQSShadowProofRssCorpusDigest expected_plan_digest,
            SIQSShadowProofRssCorpusDigest expected_policy_binding_digest,
            SIQSShadowProofRssProbeExecutionIdentity expected_probe_execution_identity,
            std::uint64_t expected_rss_limit_bytes) noexcept;
};

class TerminalGateOrchestrator final {
public:
    [[nodiscard]] static TerminalGateTransactionResult
    evaluate_claims(const SIQSShadowProofRssGatePolicy* policy,
                    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept;

    [[nodiscard]] static TerminalGateTransactionResult
    evaluate_approved(ApprovedTerminalGateBinding binding) noexcept;
};

/// Select the deployment-owned production namespace, acquire a fresh native
/// lease, confirm the complete journal and all artifacts, evaluate the gate,
/// and durably commit or confirm one immutable terminal record. The default
/// production registry remains empty.
[[nodiscard]] TerminalGateTransactionResult evaluate_siqs_shadow_proof_rss_terminal_gate(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept;

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
