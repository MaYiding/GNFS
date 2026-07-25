#include "siqs_shadow_proof_rss_terminal_gate_test_peer.hpp"

#include <new>
#include <utility>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {
namespace {

using StoreError = SIQSShadowProofRssCampaignJournalStoreError;
using StoreObject = SIQSShadowProofRssCampaignJournalStoreObject;

[[nodiscard]] bool policy_claim_matches(const SIQSShadowProofRssGatePolicy& claim,
                                        const SIQSShadowProofRssGatePolicy& approved) noexcept {
    return claim.approved == approved.approved && claim.corpus_id == approved.corpus_id &&
           claim.corpus_digest == approved.corpus_digest &&
           claim.operating_system == approved.operating_system &&
           claim.architecture == approved.architecture &&
           claim.memory_backend == approved.memory_backend &&
           claim.resolved_production_sieve_workers == approved.resolved_production_sieve_workers &&
           claim.candidate_revision == approved.candidate_revision &&
           claim.probe_execution_identity == approved.probe_execution_identity &&
           claim.approval_id == approved.approval_id &&
           claim.journal_store == approved.journal_store &&
           claim.deployment_budget_bytes == approved.deployment_budget_bytes &&
           claim.reserved_headroom_bytes == approved.reserved_headroom_bytes;
}

[[nodiscard]] bool
runtime_claim_matches(const SIQSShadowProofRssCampaignRuntimeFacts& claim,
                      const SIQSShadowProofRssCampaignRuntimeFacts& approved) noexcept {
    return claim.operating_system == approved.operating_system &&
           claim.architecture == approved.architecture &&
           claim.memory_backend == approved.memory_backend &&
           claim.resolved_production_sieve_workers == approved.resolved_production_sieve_workers &&
           claim.probe_kind == approved.probe_kind &&
           claim.candidate_revision == approved.candidate_revision &&
           claim.probe_execution_identity == approved.probe_execution_identity &&
           claim.release_build == approved.release_build && claim.ndebug == approved.ndebug;
}

[[nodiscard]] SIQSShadowProofRssCampaignJournalStoreDiagnostic
make_diagnostic(StoreError error, StoreObject object = StoreObject::none) noexcept {
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
    diagnostic.error = error;
    diagnostic.object = object;
    return diagnostic;
}

} // namespace

TerminalGateTransactionResult
TerminalGateTestPeer::evaluate(const SIQSShadowProofRssGatePolicy* policy,
                               const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts,
                               DeploymentEntry deployment) noexcept {
    try {
        const auto claims_preflight = resume_siqs_shadow_proof_rss_campaign_journal(
            policy, runtime_facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {});
        if (!absent_journal_preflight_is_ready(claims_preflight)) {
            auto diagnostic = make_diagnostic(StoreError::preflight_rejected);
            diagnostic.journal_reason = claims_preflight.reason;
            return {TerminalGateTransactionOutcome::admission_rejected, std::move(diagnostic)};
        }

        ApprovedTerminalGateBinding binding;
        binding.deployment_ = std::move(deployment);
        binding.bind_approved_views();
        const auto approved_preflight = resume_siqs_shadow_proof_rss_campaign_journal(
            &binding.approved_policy_, &binding.approved_runtime_facts_,
            SIQSShadowProofRssJournalPresence::absent, nullptr, {});
        if (!absent_journal_preflight_is_ready(approved_preflight) ||
            approved_preflight.plan_digest == SIQSShadowProofRssCorpusDigest{} ||
            approved_preflight.plan_digest != claims_preflight.plan_digest || policy == nullptr ||
            runtime_facts == nullptr ||
            binding.approved_runtime_facts_.probe_kind !=
                SIQSShadowProofRssProbeKind::production_holdout ||
            !policy_claim_matches(*policy, binding.approved_policy_) ||
            !runtime_claim_matches(*runtime_facts, binding.approved_runtime_facts_)) {
            return {
                TerminalGateTransactionOutcome::admission_rejected,
                make_diagnostic(StoreError::registry_binding_mismatch,
                                StoreObject::deployment_registry),
            };
        }

        binding.expected_plan_digest_ = approved_preflight.plan_digest;
        return TerminalGateOrchestrator::evaluate_approved(std::move(binding));
    } catch (const std::bad_alloc&) {
        return {TerminalGateTransactionOutcome::admission_rejected,
                make_diagnostic(StoreError::resource_exhausted)};
    } catch (...) {
        return {TerminalGateTransactionOutcome::admission_rejected,
                make_diagnostic(StoreError::unexpected_failure)};
    }
}

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
