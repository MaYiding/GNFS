#pragma once

// Source-private, authority-free reconciliation boundary for one approved SIQS
// RSS campaign namespace. This file is not installed as public API.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_store.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

class ApprovedReconciliationBinding;
class ReconciliationOrchestrator;
class ReconciliationResultProjector;
class ReconciliationTestPeer;

enum class CampaignReconciliationOutcome : std::uint8_t {
    admission_rejected,
    no_nonfresh_state,
    stable_prefix_confirmed,
    dangling_start_durably_tainted,
    terminal_confirmed,
    reconcile_required,
};

[[nodiscard]] constexpr std::string_view
campaign_reconciliation_outcome_name(CampaignReconciliationOutcome outcome) noexcept {
    switch (outcome) {
    case CampaignReconciliationOutcome::admission_rejected:
        return "admission_rejected";
    case CampaignReconciliationOutcome::no_nonfresh_state:
        return "no_nonfresh_state";
    case CampaignReconciliationOutcome::stable_prefix_confirmed:
        return "stable_prefix_confirmed";
    case CampaignReconciliationOutcome::dangling_start_durably_tainted:
        return "dangling_start_durably_tainted";
    case CampaignReconciliationOutcome::terminal_confirmed:
        return "terminal_confirmed";
    case CampaignReconciliationOutcome::reconcile_required:
        return "reconcile_required";
    }
    return "unknown";
}

/// Copyable data projection of one exactly confirmed durable state. It omits
/// journal action and next-slot fields so it cannot be used as launch, resume,
/// retry, or gate authority.
struct CampaignReconciliationObservation final {
    SIQSShadowProofRssJournalStatus status = SIQSShadowProofRssJournalStatus::invalid;
    SIQSShadowProofRssJournalReason reason = SIQSShadowProofRssJournalReason::record_invalid;
    std::uint32_t committed_slot_count = 0;
    SIQSShadowProofRssCorpusDigest plan_digest;

    [[nodiscard]] friend constexpr bool
    operator==(const CampaignReconciliationObservation&,
               const CampaignReconciliationObservation&) noexcept = default;
};

/// Authority-free reconciliation result. Failed or uncertain outcomes never
/// carry an observation. The type deliberately exposes no boolean conversion,
/// session, controller, gate, retry, reopen, callback, or next-slot accessor.
class CampaignReconciliationResult final {
public:
    CampaignReconciliationResult() = delete;
    CampaignReconciliationResult(const CampaignReconciliationResult&) = default;
    CampaignReconciliationResult(CampaignReconciliationResult&&) noexcept = default;
    CampaignReconciliationResult& operator=(const CampaignReconciliationResult&) = default;
    CampaignReconciliationResult& operator=(CampaignReconciliationResult&&) noexcept = default;

    [[nodiscard]] CampaignReconciliationOutcome outcome() const noexcept {
        return outcome_;
    }

    [[nodiscard]] const std::optional<CampaignReconciliationObservation>&
    confirmed_observation() const noexcept {
        return confirmed_observation_;
    }

    [[nodiscard]] const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
    store_diagnostic() const noexcept {
        return store_diagnostic_;
    }

private:
    CampaignReconciliationResult(
        CampaignReconciliationOutcome outcome,
        SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic) noexcept
        : outcome_(outcome), store_diagnostic_(std::move(diagnostic)) {}

    CampaignReconciliationResult(CampaignReconciliationOutcome outcome,
                                 CampaignReconciliationObservation observation) noexcept
        : outcome_(outcome), confirmed_observation_(observation) {}

    CampaignReconciliationOutcome outcome_;
    std::optional<CampaignReconciliationObservation> confirmed_observation_;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic store_diagnostic_;

    friend class ReconciliationOrchestrator;
    friend class ReconciliationResultProjector;
    friend class ReconciliationTestPeer;
};

struct CoreReconciliationResult final {
    CampaignReconciliationOutcome outcome = CampaignReconciliationOutcome::reconcile_required;
    std::optional<CampaignReconciliationObservation> confirmed_observation;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
};

/// Pure fail-closed projection from a platform core result into the
/// authority-free caller result. The expected digest is supplied by the
/// approved binding, not by the platform core.
class ReconciliationResultProjector final {
public:
    [[nodiscard]] static CampaignReconciliationResult
    project(CoreReconciliationResult core_result,
            SIQSShadowProofRssCorpusDigest expected_plan_digest) noexcept;
};

/// Claims-only production orchestration plus an opaque-binding continuation.
/// The latter accepts no registry, path, deployment row, or publication seam;
/// only the closed selector and the test-only peer can construct its argument.
class ReconciliationOrchestrator final {
public:
    [[nodiscard]] static CampaignReconciliationResult
    reconcile_claims(const SIQSShadowProofRssGatePolicy* policy,
                     const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept;

    [[nodiscard]] static CampaignReconciliationResult
    reconcile_approved(ApprovedReconciliationBinding binding) noexcept;
};

/// Select the deployment-owned production namespace from the closed registry,
/// acquire a fresh native observation lease, and reconcile only what the disk
/// proves. Callers can supply policy/runtime claims but no registry, path,
/// session, callback, controller result, or recovery action.
[[nodiscard]] CampaignReconciliationResult reconcile_siqs_shadow_proof_rss_campaign(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept;

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
