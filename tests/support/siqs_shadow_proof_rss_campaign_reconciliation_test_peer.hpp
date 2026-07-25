#pragma once

#include "shadow_proof_rss_campaign_journal_store_internal.hpp"

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

/// Test-executable-only bridge for exercising native reconciliation against a
/// fixture deployment. Production core code exposes only the claims-only
/// orchestrator and cannot accept a registry, path, or PublicationOps seam.
class ReconciliationTestPeer final {
public:
    [[nodiscard]] static CampaignReconciliationResult
    reconcile(const SIQSShadowProofRssGatePolicy* policy,
              const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts,
              DeploymentEntry deployment) noexcept;
};

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
