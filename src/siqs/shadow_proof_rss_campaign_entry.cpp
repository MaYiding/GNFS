#include "shadow_proof_rss_campaign_entry_internal.hpp"
#include "shadow_proof_rss_campaign_journal_store_internal.hpp"

#include <utility>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

ProductionCampaignEntryResult run_siqs_shadow_proof_rss_production_campaign(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept {
    const auto preflight = resume_siqs_shadow_proof_rss_campaign_journal(
        policy, runtime_facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {});
    if (!absent_journal_preflight_is_ready(preflight)) {
        SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
        diagnostic.error = SIQSShadowProofRssCampaignJournalStoreError::preflight_rejected;
        diagnostic.journal_reason = preflight.reason;
        return {ProductionCampaignEntryOutcome::preflight_rejected, std::move(diagnostic)};
    }

    // Synthetic execution remains available only through private transaction
    // tests. It cannot reach the production registry or controller here.
    if (runtime_facts->probe_kind != SIQSShadowProofRssProbeKind::production_holdout) {
        return {ProductionCampaignEntryOutcome::production_classification_rejected,
                SIQSShadowProofRssCampaignJournalStoreDiagnostic{}};
    }

    // The public open is the sole production admission point. In the default
    // build its closed registry rejects before platform or filesystem access.
    auto opened = open_siqs_shadow_proof_rss_campaign_journal_session(policy, runtime_facts);
    if (!opened) {
        return {ProductionCampaignEntryOutcome::open_rejected, opened.diagnostic()};
    }

    auto session = std::move(opened).take_session();
    if (!session.has_value() || !session->active()) {
        SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
        diagnostic.error = SIQSShadowProofRssCampaignJournalStoreError::unexpected_failure;
        return {ProductionCampaignEntryOutcome::open_rejected, std::move(diagnostic)};
    }

    auto controller_result = run_serial_campaign_to_terminal(std::move(*session));
    session.reset();
    const auto outcome =
        production_campaign_entry_detail::project_serial_outcome(controller_result.outcome());
    return {outcome, std::move(controller_result)};
}

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
