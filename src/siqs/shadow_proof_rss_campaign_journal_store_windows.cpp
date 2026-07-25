#include "shadow_proof_rss_campaign_journal_store_internal.hpp"

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

PlatformSessionOpenResult open_siqs_shadow_proof_rss_campaign_journal_platform_session(
    const SIQSShadowProofRssGatePolicy& policy,
    const SIQSShadowProofRssCampaignRuntimeFacts& runtime_facts,
    const DeploymentEntry& deployment) noexcept {
    (void)policy;
    (void)runtime_facts;
    (void)deployment;

    PlatformSessionOpenResult result;
    result.diagnostic.error = SIQSShadowProofRssCampaignJournalStoreError::platform_unavailable;
    return result;
}

PlatformReconciliationOpenResult
open_siqs_shadow_proof_rss_campaign_journal_platform_reconciliation(
    ApprovedReconciliationBinding binding) noexcept {
    (void)binding;

    PlatformReconciliationOpenResult result;
    result.diagnostic.error = SIQSShadowProofRssCampaignJournalStoreError::platform_unavailable;
    return result;
}

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
