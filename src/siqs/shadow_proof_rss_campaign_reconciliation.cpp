#include "shadow_proof_rss_campaign_journal_store_internal.hpp"
#include "shadow_proof_rss_campaign_reconciliation_internal.hpp"

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

CampaignReconciliationResult reconcile_siqs_shadow_proof_rss_campaign(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept {
    return ReconciliationOrchestrator::reconcile_claims(policy, runtime_facts);
}

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
