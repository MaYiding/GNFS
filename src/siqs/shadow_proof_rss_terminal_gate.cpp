#include "shadow_proof_rss_campaign_journal_store_internal.hpp"
#include "shadow_proof_rss_terminal_gate_internal.hpp"

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

TerminalGateTransactionResult evaluate_siqs_shadow_proof_rss_terminal_gate(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept {
    return TerminalGateOrchestrator::evaluate_claims(policy, runtime_facts);
}

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
