#pragma once

/// @file shadow_proof_rss_campaign.hpp
/// @brief Pure deterministic campaign planning for approved SIQS RSS policies.

#include <gnfs/siqs/shadow_proof_rss_gate.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gnfs::siqs {

/// Campaign execution is deliberately sequential. This is a contract constant,
/// not a runtime setting, so consumers cannot make RSS samples overlap.
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_MAX_CONCURRENCY = 1;

enum class SIQSShadowProofRssCampaignPlanStatus : uint8_t {
    blocked,
    invalid,
    ready,
};

enum class SIQSShadowProofRssCampaignPlanReason : uint8_t {
    policy_missing,
    policy_not_approved,
    policy_budget_missing,
    policy_headroom_missing,
    policy_budget_not_above_headroom,
    policy_binding_invalid,
    ready,
};

/// One immutable-description campaign slot. String views retain the same
/// non-owning lifetime contract as SIQSShadowProofRssGatePolicy.
struct SIQSShadowProofRssCampaignSlot final {
    uint32_t slot_number = 0;
    uint32_t fixture_id = 0;
    SIQSShadowProofRssSampleMode mode = SIQSShadowProofRssSampleMode::unknown;
    uint32_t ordinal = 0;

    SIQSShadowProofRssCorpusDigest policy_binding_digest;
    bool policy_approved = false;
    std::string_view corpus_id;
    SIQSShadowProofRssCorpusDigest corpus_digest;
    SIQSShadowProofRssOperatingSystem operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    SIQSShadowProofRssArchitecture architecture = SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t resolved_production_sieve_workers = 0;
    std::string_view candidate_revision;
    SIQSShadowProofRssProbeExecutionIdentity probe_execution_identity;
    std::string_view approval_id;
    SIQSShadowProofRssJournalStoreBinding journal_store;
    std::optional<uint64_t> deployment_budget_bytes;
    std::optional<uint64_t> reserved_headroom_bytes;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCampaignSlot&,
               const SIQSShadowProofRssCampaignSlot&) noexcept = default;
};

struct SIQSShadowProofRssCampaignPlan final {
    static constexpr std::size_t max_concurrency = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_MAX_CONCURRENCY;

    SIQSShadowProofRssCampaignPlanStatus status = SIQSShadowProofRssCampaignPlanStatus::invalid;
    SIQSShadowProofRssCampaignPlanReason reason =
        SIQSShadowProofRssCampaignPlanReason::policy_binding_invalid;
    std::size_t slot_count = 0;
    std::array<SIQSShadowProofRssCampaignSlot, SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT>
        slots{};

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCampaignPlan&,
               const SIQSShadowProofRssCampaignPlan&) noexcept = default;
};

namespace shadow_proof_rss_campaign_detail {

[[nodiscard]] constexpr SIQSShadowProofRssCampaignPlan
empty_campaign_plan(SIQSShadowProofRssCampaignPlanStatus status,
                    SIQSShadowProofRssCampaignPlanReason reason) noexcept {
    SIQSShadowProofRssCampaignPlan plan;
    plan.status = status;
    plan.reason = reason;
    return plan;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignSlot
campaign_slot(const SIQSShadowProofRssGatePolicy& policy,
              SIQSShadowProofRssCorpusDigest policy_digest, uint32_t slot_number,
              uint32_t fixture_id, SIQSShadowProofRssSampleMode mode, uint32_t ordinal) noexcept {
    SIQSShadowProofRssCampaignSlot slot;
    slot.slot_number = slot_number;
    slot.fixture_id = fixture_id;
    slot.mode = mode;
    slot.ordinal = ordinal;
    slot.policy_binding_digest = policy_digest;
    slot.policy_approved = policy.approved;
    slot.corpus_id = policy.corpus_id;
    slot.corpus_digest = policy.corpus_digest;
    slot.operating_system = policy.operating_system;
    slot.architecture = policy.architecture;
    slot.memory_backend = policy.memory_backend;
    slot.resolved_production_sieve_workers = policy.resolved_production_sieve_workers;
    slot.candidate_revision = policy.candidate_revision;
    slot.probe_execution_identity = policy.probe_execution_identity;
    slot.approval_id = policy.approval_id;
    slot.journal_store = policy.journal_store;
    slot.deployment_budget_bytes = policy.deployment_budget_bytes;
    slot.reserved_headroom_bytes = policy.reserved_headroom_bytes;
    return slot;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignPlan
rejected_campaign_plan(SIQSShadowProofRssGateReason policy_error) noexcept {
    switch (policy_error) {
    case SIQSShadowProofRssGateReason::policy_missing:
        return empty_campaign_plan(SIQSShadowProofRssCampaignPlanStatus::blocked,
                                   SIQSShadowProofRssCampaignPlanReason::policy_missing);
    case SIQSShadowProofRssGateReason::policy_not_approved:
        return empty_campaign_plan(SIQSShadowProofRssCampaignPlanStatus::blocked,
                                   SIQSShadowProofRssCampaignPlanReason::policy_not_approved);
    case SIQSShadowProofRssGateReason::policy_budget_missing:
        return empty_campaign_plan(SIQSShadowProofRssCampaignPlanStatus::blocked,
                                   SIQSShadowProofRssCampaignPlanReason::policy_budget_missing);
    case SIQSShadowProofRssGateReason::policy_headroom_missing:
        return empty_campaign_plan(SIQSShadowProofRssCampaignPlanStatus::blocked,
                                   SIQSShadowProofRssCampaignPlanReason::policy_headroom_missing);
    case SIQSShadowProofRssGateReason::policy_budget_not_above_headroom:
        return empty_campaign_plan(
            SIQSShadowProofRssCampaignPlanStatus::invalid,
            SIQSShadowProofRssCampaignPlanReason::policy_budget_not_above_headroom);
    case SIQSShadowProofRssGateReason::policy_binding_invalid:
        return empty_campaign_plan(SIQSShadowProofRssCampaignPlanStatus::invalid,
                                   SIQSShadowProofRssCampaignPlanReason::policy_binding_invalid);
    default:
        return empty_campaign_plan(SIQSShadowProofRssCampaignPlanStatus::invalid,
                                   SIQSShadowProofRssCampaignPlanReason::policy_binding_invalid);
    }
}

} // namespace shadow_proof_rss_campaign_detail

/// Produce the canonical fixture-major campaign from one already-typed policy.
/// The function does no I/O and does not inspect host, environment, fixtures,
/// processes, or command-line arguments. Rejected policies always yield zero
/// slots. A ready policy yields exactly 80 slots: off 1..3 then observe 1..7
/// for each fixture 1..8.
[[nodiscard]] constexpr SIQSShadowProofRssCampaignPlan
make_siqs_shadow_proof_rss_campaign_plan(const SIQSShadowProofRssGatePolicy* policy) noexcept {
    using shadow_proof_rss_campaign_detail::campaign_slot;
    using shadow_proof_rss_campaign_detail::rejected_campaign_plan;

    if (const auto policy_error = siqs_shadow_proof_rss_gate_policy_error(policy)) {
        return rejected_campaign_plan(*policy_error);
    }

    SIQSShadowProofRssCampaignPlan plan;
    plan.status = SIQSShadowProofRssCampaignPlanStatus::ready;
    plan.reason = SIQSShadowProofRssCampaignPlanReason::ready;
    const SIQSShadowProofRssCorpusDigest policy_digest =
        siqs_shadow_proof_rss_policy_binding_digest(*policy);

    for (uint32_t fixture_id = 1; fixture_id <= SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT;
         ++fixture_id) {
        for (uint32_t ordinal = 1; ordinal <= SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS;
             ++ordinal) {
            const uint32_t slot_number = static_cast<uint32_t>(plan.slot_count + 1);
            plan.slots[plan.slot_count++] =
                campaign_slot(*policy, policy_digest, slot_number, fixture_id,
                              SIQSShadowProofRssSampleMode::off, ordinal);
        }
        for (uint32_t ordinal = 1; ordinal <= SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
             ++ordinal) {
            const uint32_t slot_number = static_cast<uint32_t>(plan.slot_count + 1);
            plan.slots[plan.slot_count++] =
                campaign_slot(*policy, policy_digest, slot_number, fixture_id,
                              SIQSShadowProofRssSampleMode::observe, ordinal);
        }
    }
    return plan;
}

} // namespace gnfs::siqs
