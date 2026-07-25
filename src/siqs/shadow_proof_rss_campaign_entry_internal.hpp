#pragma once

// Source-private, default-closed production composition. This file is not an
// installed API: approving a policy and provisioning the production registry
// remain separate deployment milestones.

#include "shadow_proof_rss_campaign_controller_internal.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

enum class ProductionCampaignEntryOutcome : std::uint8_t {
    preflight_rejected,
    production_classification_rejected,
    open_rejected,
    production_complete_gate_required,
    durably_tainted,
    reconcile_required,
};

[[nodiscard]] constexpr std::string_view
production_campaign_entry_outcome_name(ProductionCampaignEntryOutcome outcome) noexcept {
    switch (outcome) {
    case ProductionCampaignEntryOutcome::preflight_rejected:
        return "preflight_rejected";
    case ProductionCampaignEntryOutcome::production_classification_rejected:
        return "production_classification_rejected";
    case ProductionCampaignEntryOutcome::open_rejected:
        return "open_rejected";
    case ProductionCampaignEntryOutcome::production_complete_gate_required:
        return "production_complete_gate_required";
    case ProductionCampaignEntryOutcome::durably_tainted:
        return "durably_tainted";
    case ProductionCampaignEntryOutcome::reconcile_required:
        return "reconcile_required";
    }
    return "unknown";
}

namespace production_campaign_entry_detail {

[[nodiscard]] constexpr ProductionCampaignEntryOutcome
project_serial_outcome(SerialCampaignOutcome outcome) noexcept {
    switch (outcome) {
    case SerialCampaignOutcome::production_complete_gate_required:
        return ProductionCampaignEntryOutcome::production_complete_gate_required;
    case SerialCampaignOutcome::durably_tainted:
        return ProductionCampaignEntryOutcome::durably_tainted;
    case SerialCampaignOutcome::synthetic_complete:
    case SerialCampaignOutcome::stopped:
    case SerialCampaignOutcome::reconcile_required:
        return ProductionCampaignEntryOutcome::reconcile_required;
    }
    return ProductionCampaignEntryOutcome::reconcile_required;
}

} // namespace production_campaign_entry_detail

/// Copyable, authority-free projection of the blocked entry or consumed
/// controller result. Reject outcomes never carry a controller result.
/// Controller outcomes never carry a store error. This type intentionally has
/// no boolean conversion, gate operation, retry, reopen, or session accessor.
class ProductionCampaignEntryResult final {
public:
    ProductionCampaignEntryResult() = delete;
    ProductionCampaignEntryResult(const ProductionCampaignEntryResult&) = default;
    ProductionCampaignEntryResult(ProductionCampaignEntryResult&&) noexcept = default;
    ProductionCampaignEntryResult& operator=(const ProductionCampaignEntryResult&) = default;
    ProductionCampaignEntryResult& operator=(ProductionCampaignEntryResult&&) noexcept = default;

    [[nodiscard]] ProductionCampaignEntryOutcome outcome() const noexcept {
        return outcome_;
    }

    [[nodiscard]] const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
    store_diagnostic() const noexcept {
        return store_diagnostic_;
    }

    [[nodiscard]] const std::optional<SerialCampaignResult>& controller_result() const noexcept {
        return controller_result_;
    }

private:
    ProductionCampaignEntryResult(
        ProductionCampaignEntryOutcome outcome,
        SIQSShadowProofRssCampaignJournalStoreDiagnostic store_diagnostic) noexcept
        : outcome_(outcome), store_diagnostic_(std::move(store_diagnostic)) {}

    ProductionCampaignEntryResult(ProductionCampaignEntryOutcome outcome,
                                  SerialCampaignResult controller_result) noexcept
        : outcome_(outcome), controller_result_(std::move(controller_result)) {}

    ProductionCampaignEntryOutcome outcome_;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic store_diagnostic_;
    std::optional<SerialCampaignResult> controller_result_;

    friend ProductionCampaignEntryResult run_siqs_shadow_proof_rss_production_campaign(
        const SIQSShadowProofRssGatePolicy*,
        const SIQSShadowProofRssCampaignRuntimeFacts*) noexcept;
};

/// Perform the pure absent-journal preflight, reject non-production claims,
/// call the public deployment-registry open exactly once, and immediately
/// consume any returned session in the fresh-only serial controller. The
/// function has no injected registry, path, callback, session, recovery, or
/// gate authority.
[[nodiscard]] ProductionCampaignEntryResult run_siqs_shadow_proof_rss_production_campaign(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept;

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
