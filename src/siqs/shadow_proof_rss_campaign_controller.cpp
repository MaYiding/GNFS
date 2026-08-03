#include "shadow_proof_rss_campaign_controller_internal.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

SerialCampaignResult
run_serial_campaign_to_terminal(SIQSShadowProofRssCampaignJournalSession session) noexcept {
    static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_MAX_CONCURRENCY == 1);

    SerialCampaignResult result(session.view());
    if (!session.active()) {
        result.failure_ = SerialCampaignFailure::inactive_session;
        return result;
    }
    if (!serial_campaign_detail::fresh_view_is_valid(result.initial_view_)) {
        result.failure_ = SerialCampaignFailure::initial_state_invalid;
        return result;
    }

    std::optional<SIQSShadowProofRssCampaignJournalSession> current_session(std::move(session));
    for (std::uint32_t attempt = 0; attempt < SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT;
         ++attempt) {
        const SIQSShadowProofRssCampaignJournalSessionView before = current_session->view();
        const bool before_valid = attempt == 0
                                      ? serial_campaign_detail::fresh_view_is_valid(before)
                                      : serial_campaign_detail::continuation_view_is_valid(before);
        if (!before_valid || before.committed_slot_count != attempt ||
            before.next_slot_number != attempt + 1) {
            result.outcome_ = SerialCampaignOutcome::reconcile_required;
            result.failure_ = SerialCampaignFailure::progress_violation;
            return result;
        }
        result.attempted_slot_number_ = before.next_slot_number;

        auto begin = std::move(*current_session).begin_next_slot();
        current_session.reset();
        if (!begin) {
            result.outcome_ = SerialCampaignOutcome::reconcile_required;
            result.failure_ = SerialCampaignFailure::begin_failed;
            result.begin_diagnostic_ = begin.diagnostic();
            return result;
        }

        auto active_slot = std::move(begin).take_active_slot();
        if (!active_slot.has_value()) {
            result.outcome_ = SerialCampaignOutcome::reconcile_required;
            result.failure_ = SerialCampaignFailure::active_slot_invalid;
            return result;
        }
        const bool active_valid =
            active_slot->active() && active_slot->slot_number() == before.next_slot_number &&
            serial_campaign_detail::pending_slot_view_is_valid(before, active_slot->view());
        if (!active_valid) {
            result.failure_ = SerialCampaignFailure::active_slot_invalid;
            if (!active_slot->active()) {
                result.outcome_ = SerialCampaignOutcome::reconcile_required;
                return result;
            }
            auto taint = std::move(*active_slot).taint();
            active_slot.reset();
            result.direct_taint_diagnostic_ = taint.diagnostic();
            if (!taint) {
                result.outcome_ = SerialCampaignOutcome::reconcile_required;
                return result;
            }
            if (serial_campaign_detail::explicit_taint_view_is_valid(taint.view())) {
                result.outcome_ = SerialCampaignOutcome::durably_tainted;
                result.terminal_view_ = taint.view();
            } else {
                result.outcome_ = SerialCampaignOutcome::reconcile_required;
            }
            return result;
        }

        // From a durable start, the next authority-bearing operation is always
        // the same-child runner. No callback, cancellation check, allocation,
        // or caller-controlled branch is permitted in this gap.
        auto slot_result = SlotRunnerFactory::run(std::move(*active_slot));
        active_slot.reset();
        result.slot_diagnostic_ = slot_result.diagnostic();
        if (!slot_result) {
            result.failure_ = SerialCampaignFailure::slot_failed;
            if (result.slot_diagnostic_.error == SlotRunnerError::commit_outcome_uncertain ||
                !result.slot_diagnostic_.taint_durable) {
                result.outcome_ = SerialCampaignOutcome::reconcile_required;
                return result;
            }
            if (serial_campaign_detail::explicit_taint_view_is_valid(slot_result.view())) {
                result.outcome_ = SerialCampaignOutcome::durably_tainted;
                result.terminal_view_ = slot_result.view();
            } else {
                result.outcome_ = SerialCampaignOutcome::reconcile_required;
            }
            return result;
        }

        auto continuation = std::move(slot_result).take_session();
        if (!continuation.has_value() || !continuation->active() ||
            !serial_campaign_detail::transition_is_valid(before, continuation->view())) {
            result.outcome_ = SerialCampaignOutcome::reconcile_required;
            result.failure_ = SerialCampaignFailure::progress_violation;
            return result;
        }

        ++result.committed_slots_in_run_;
        if (continuation->view().status == SIQSShadowProofRssJournalStatus::complete) {
            result.outcome_ =
                continuation->view().reason == SIQSShadowProofRssJournalReason::complete
                    ? SerialCampaignOutcome::production_complete_gate_required
                    : SerialCampaignOutcome::synthetic_complete;
            result.failure_ = SerialCampaignFailure::none;
            result.terminal_view_ = continuation->view();
            return result;
        }

        // `current_session` is disengaged here. Emplace move-constructs the
        // next lease and can never overwrite a live authority token.
        current_session.emplace(std::move(*continuation));
        continuation.reset();
    }

    result.outcome_ = SerialCampaignOutcome::reconcile_required;
    result.failure_ = SerialCampaignFailure::progress_violation;
    return result;
}

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
