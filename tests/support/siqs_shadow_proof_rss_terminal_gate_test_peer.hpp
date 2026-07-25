#pragma once

#include "shadow_proof_rss_campaign_journal_store_internal.hpp"

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

/// Test-executable-only bridge for exercising the closed terminal-gate
/// transaction against one owning fixture row. Production core code exposes
/// only the claims-only, default-empty-registry entry.
class TerminalGateTestPeer final {
public:
    [[nodiscard]] static TerminalGateTransactionResult
    evaluate(const SIQSShadowProofRssGatePolicy* policy,
             const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts,
             DeploymentEntry deployment) noexcept;
};

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
