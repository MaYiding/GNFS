#pragma once

#include "../util/bounded_child_process_internal.hpp"
#include "shadow_proof_rss_campaign_journal_store_internal.hpp"

#include <gnfs/util/bounded_child_process.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

enum class SlotRunnerError : uint8_t {
    none,
    platform_unavailable,
    session_inactive,
    deployment_unavailable,
    deployment_invalid,
    executable_authentication_failed,
    transport_failed,
    stream_join_failed,
    artifact_publication_failed,
    commit_failed,
    commit_outcome_uncertain,
    taint_failed,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view slot_runner_error_name(SlotRunnerError error) noexcept {
    switch (error) {
    case SlotRunnerError::none:
        return "none";
    case SlotRunnerError::platform_unavailable:
        return "platform_unavailable";
    case SlotRunnerError::session_inactive:
        return "session_inactive";
    case SlotRunnerError::deployment_unavailable:
        return "deployment_unavailable";
    case SlotRunnerError::deployment_invalid:
        return "deployment_invalid";
    case SlotRunnerError::executable_authentication_failed:
        return "executable_authentication_failed";
    case SlotRunnerError::transport_failed:
        return "transport_failed";
    case SlotRunnerError::stream_join_failed:
        return "stream_join_failed";
    case SlotRunnerError::artifact_publication_failed:
        return "artifact_publication_failed";
    case SlotRunnerError::commit_failed:
        return "commit_failed";
    case SlotRunnerError::commit_outcome_uncertain:
        return "commit_outcome_uncertain";
    case SlotRunnerError::taint_failed:
        return "taint_failed";
    case SlotRunnerError::resource_exhausted:
        return "resource_exhausted";
    case SlotRunnerError::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct SlotRunnerDiagnostic final {
    SlotRunnerError error = SlotRunnerError::none;
    util::BoundedChildProcessError transport_error = util::BoundedChildProcessError::none;
    util::ExecutableImageAuthenticationDiagnostic authentication;
    uint8_t stream_join_error = 0;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic store_diagnostic;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic taint_diagnostic;
    util::BoundedChildTermination termination;
    std::size_t stdout_byte_count = 0;
    std::size_t stderr_byte_count = 0;
    bool child_started = false;
    bool stdout_eof = false;
    bool stderr_eof = false;
    bool cleanup_complete = false;
    bool taint_attempted = false;
    bool taint_durable = false;
};

class SlotRunnerResult final {
public:
    SlotRunnerResult() = delete;
    ~SlotRunnerResult();

    SlotRunnerResult(SlotRunnerResult&&) noexcept;
    SlotRunnerResult& operator=(SlotRunnerResult&&) noexcept;

    SlotRunnerResult(const SlotRunnerResult&) = delete;
    SlotRunnerResult& operator=(const SlotRunnerResult&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const SlotRunnerDiagnostic& diagnostic() const noexcept;
    [[nodiscard]] SIQSShadowProofRssCampaignJournalSessionView view() const noexcept;
    [[nodiscard]] std::optional<SIQSShadowProofRssCampaignJournalSession>
    take_session() && noexcept;

private:
    explicit SlotRunnerResult(SlotRunnerDiagnostic diagnostic,
                              SIQSShadowProofRssCampaignJournalSessionView view = {}) noexcept;
    explicit SlotRunnerResult(SIQSShadowProofRssCampaignJournalSession&& session) noexcept;

    std::optional<SIQSShadowProofRssCampaignJournalSession> session_;
    SIQSShadowProofRssCampaignJournalSessionView view_;
    SlotRunnerDiagnostic diagnostic_;

    friend class SlotRunnerFactory;
};

/// Private same-child transaction. The only runtime input is the lease-bound
/// active slot; executable, argv, complete environment, deadline, and capture
/// limits are derived from its private deployment and canonical campaign slot.
class SlotRunnerFactory final {
public:
    SlotRunnerFactory() = delete;

    [[nodiscard]] static SlotRunnerResult
    run(SIQSShadowProofRssCampaignJournalActiveSlot&& active_slot) noexcept;

private:
    [[nodiscard]] static SlotRunnerResult
    finish_with_taint(SIQSShadowProofRssCampaignJournalActiveSlot&& active_slot,
                      SlotRunnerDiagnostic diagnostic) noexcept;
};

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
