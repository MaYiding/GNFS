#pragma once

// Source-private serial controller for an already-open campaign session. It
// owns no policy, registry, gate, or recovery authority.

#include "shadow_proof_rss_campaign_slot_runner_internal.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

enum class SerialCampaignOutcome : std::uint8_t {
    production_complete_gate_required,
    synthetic_complete,
    durably_tainted,
    stopped,
    reconcile_required,
};

[[nodiscard]] constexpr std::string_view
serial_campaign_outcome_name(SerialCampaignOutcome outcome) noexcept {
    switch (outcome) {
    case SerialCampaignOutcome::production_complete_gate_required:
        return "production_complete_gate_required";
    case SerialCampaignOutcome::synthetic_complete:
        return "synthetic_complete";
    case SerialCampaignOutcome::durably_tainted:
        return "durably_tainted";
    case SerialCampaignOutcome::stopped:
        return "stopped";
    case SerialCampaignOutcome::reconcile_required:
        return "reconcile_required";
    }
    return "unknown";
}

enum class SerialCampaignFailure : std::uint8_t {
    none,
    inactive_session,
    initial_state_invalid,
    begin_failed,
    active_slot_invalid,
    slot_failed,
    progress_violation,
};

[[nodiscard]] constexpr std::string_view
serial_campaign_failure_name(SerialCampaignFailure failure) noexcept {
    switch (failure) {
    case SerialCampaignFailure::none:
        return "none";
    case SerialCampaignFailure::inactive_session:
        return "inactive_session";
    case SerialCampaignFailure::initial_state_invalid:
        return "initial_state_invalid";
    case SerialCampaignFailure::begin_failed:
        return "begin_failed";
    case SerialCampaignFailure::active_slot_invalid:
        return "active_slot_invalid";
    case SerialCampaignFailure::slot_failed:
        return "slot_failed";
    case SerialCampaignFailure::progress_violation:
        return "progress_violation";
    }
    return "unknown";
}

/// Read-only data outcome. A terminal view exists only for an exact durable
/// production-complete, synthetic-complete, or explicit-taint state.
/// `reconcile_required` deliberately withholds any potentially stale
/// post-action view. This type grants neither gate nor continuation authority
/// and intentionally has no default construction or boolean conversion.
class SerialCampaignResult final {
public:
    SerialCampaignResult() = delete;
    SerialCampaignResult(const SerialCampaignResult&) = default;
    SerialCampaignResult(SerialCampaignResult&&) noexcept = default;
    SerialCampaignResult& operator=(const SerialCampaignResult&) = default;
    SerialCampaignResult& operator=(SerialCampaignResult&&) noexcept = default;

    [[nodiscard]] SerialCampaignOutcome outcome() const noexcept {
        return outcome_;
    }

    [[nodiscard]] SerialCampaignFailure failure() const noexcept {
        return failure_;
    }

    [[nodiscard]] const SIQSShadowProofRssCampaignJournalSessionView&
    initial_view() const noexcept {
        return initial_view_;
    }

    [[nodiscard]] const std::optional<SIQSShadowProofRssCampaignJournalSessionView>&
    terminal_view() const noexcept {
        return terminal_view_;
    }

    [[nodiscard]] std::uint32_t attempted_slot_number() const noexcept {
        return attempted_slot_number_;
    }

    [[nodiscard]] std::uint32_t committed_slots_in_run() const noexcept {
        return committed_slots_in_run_;
    }

    [[nodiscard]] const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
    begin_diagnostic() const noexcept {
        return begin_diagnostic_;
    }

    [[nodiscard]] const SlotRunnerDiagnostic& slot_diagnostic() const noexcept {
        return slot_diagnostic_;
    }

    [[nodiscard]] const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
    direct_taint_diagnostic() const noexcept {
        return direct_taint_diagnostic_;
    }

    [[nodiscard]] bool terminal_durable() const noexcept {
        return outcome_ == SerialCampaignOutcome::production_complete_gate_required ||
               outcome_ == SerialCampaignOutcome::synthetic_complete ||
               outcome_ == SerialCampaignOutcome::durably_tainted;
    }

private:
    explicit SerialCampaignResult(
        SIQSShadowProofRssCampaignJournalSessionView initial_view) noexcept
        : initial_view_(initial_view) {}

    SerialCampaignOutcome outcome_ = SerialCampaignOutcome::stopped;
    SerialCampaignFailure failure_ = SerialCampaignFailure::initial_state_invalid;
    SIQSShadowProofRssCampaignJournalSessionView initial_view_;
    std::optional<SIQSShadowProofRssCampaignJournalSessionView> terminal_view_;
    std::uint32_t attempted_slot_number_ = 0;
    std::uint32_t committed_slots_in_run_ = 0;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic begin_diagnostic_;
    SlotRunnerDiagnostic slot_diagnostic_;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic direct_taint_diagnostic_;

    friend SerialCampaignResult
        run_serial_campaign_to_terminal(SIQSShadowProofRssCampaignJournalSession) noexcept;
};

namespace serial_campaign_detail {

[[nodiscard]] constexpr bool plan_digest_is_valid(SIQSShadowProofRssCorpusDigest digest) noexcept {
    return digest.low != 0 || digest.high != 0;
}

[[nodiscard]] constexpr bool
fresh_view_is_valid(const SIQSShadowProofRssCampaignJournalSessionView& view) noexcept {
    return view.status == SIQSShadowProofRssJournalStatus::ready &&
           view.reason == SIQSShadowProofRssJournalReason::ready &&
           view.action == SIQSShadowProofRssJournalAction::create_header &&
           view.committed_slot_count == 0 && view.next_slot_number == 1 &&
           plan_digest_is_valid(view.plan_digest);
}

[[nodiscard]] constexpr bool
continuation_view_is_valid(const SIQSShadowProofRssCampaignJournalSessionView& view) noexcept {
    return view.status == SIQSShadowProofRssJournalStatus::ready &&
           view.reason == SIQSShadowProofRssJournalReason::ready &&
           view.action == SIQSShadowProofRssJournalAction::append_slot_start &&
           view.committed_slot_count > 0 &&
           view.committed_slot_count < SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT &&
           view.next_slot_number == view.committed_slot_count + 1 &&
           plan_digest_is_valid(view.plan_digest);
}

[[nodiscard]] constexpr bool
terminal_view_is_valid(const SIQSShadowProofRssCampaignJournalSessionView& view) noexcept {
    const bool production_terminal = view.reason == SIQSShadowProofRssJournalReason::complete &&
                                     view.action == SIQSShadowProofRssJournalAction::evaluate_gate;
    const bool synthetic_terminal =
        view.reason == SIQSShadowProofRssJournalReason::synthetic_complete &&
        view.action == SIQSShadowProofRssJournalAction::none;
    return view.status == SIQSShadowProofRssJournalStatus::complete &&
           (production_terminal || synthetic_terminal) &&
           view.committed_slot_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT &&
           view.next_slot_number == 0 && plan_digest_is_valid(view.plan_digest);
}

[[nodiscard]] constexpr bool
explicit_taint_view_is_valid(const SIQSShadowProofRssCampaignJournalSessionView& view) noexcept {
    return view.status == SIQSShadowProofRssJournalStatus::tainted &&
           view.reason == SIQSShadowProofRssJournalReason::explicitly_tainted &&
           view.action == SIQSShadowProofRssJournalAction::none &&
           view.committed_slot_count < SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT &&
           view.next_slot_number == view.committed_slot_count + 1 &&
           plan_digest_is_valid(view.plan_digest);
}

[[nodiscard]] constexpr bool
pending_slot_view_is_valid(const SIQSShadowProofRssCampaignJournalSessionView& before,
                           const SIQSShadowProofRssCampaignJournalSessionView& pending) noexcept {
    return pending.status == SIQSShadowProofRssJournalStatus::tainted &&
           pending.reason == SIQSShadowProofRssJournalReason::dangling_slot_start &&
           pending.action == SIQSShadowProofRssJournalAction::append_taint &&
           pending.committed_slot_count == before.committed_slot_count &&
           pending.next_slot_number == before.next_slot_number &&
           pending.plan_digest == before.plan_digest;
}

[[nodiscard]] constexpr bool
transition_is_valid(const SIQSShadowProofRssCampaignJournalSessionView& before,
                    const SIQSShadowProofRssCampaignJournalSessionView& after) noexcept {
    const bool before_valid = fresh_view_is_valid(before) || continuation_view_is_valid(before);
    if (!before_valid || before.plan_digest != after.plan_digest ||
        after.committed_slot_count != before.committed_slot_count + 1) {
        return false;
    }
    if (after.committed_slot_count < SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT) {
        return continuation_view_is_valid(after) &&
               after.next_slot_number == before.next_slot_number + 1;
    }
    return after.committed_slot_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT &&
           terminal_view_is_valid(after);
}

} // namespace serial_campaign_detail

/// Consume one fresh lease and run all canonical slots serially until complete
/// or the first failure boundary. Partial prefixes are rejected rather than
/// resumed. There is no callback, retry, reopen, cancellation, policy input,
/// or gate evaluation. A successful begin is handed immediately to the
/// same-child runner, and no authority is returned.
[[nodiscard]] SerialCampaignResult
run_serial_campaign_to_terminal(SIQSShadowProofRssCampaignJournalSession session) noexcept;

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
