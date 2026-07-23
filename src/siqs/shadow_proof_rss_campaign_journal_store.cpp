#include "shadow_proof_rss_campaign_journal_store_internal.hpp"

#include <cstddef>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace gnfs::siqs {

namespace detail = shadow_proof_rss_campaign_journal_store_detail;

SIQSShadowProofRssCampaignJournalActiveSlot::SIQSShadowProofRssCampaignJournalActiveSlot(
    std::unique_ptr<detail::SessionCore> core, SIQSShadowProofRssLaunchPermit&& permit) noexcept
    : core_(std::move(core)), permit_(std::move(permit)) {}

SIQSShadowProofRssCampaignJournalActiveSlot::~SIQSShadowProofRssCampaignJournalActiveSlot() =
    default;

SIQSShadowProofRssCampaignJournalActiveSlot::SIQSShadowProofRssCampaignJournalActiveSlot(
    SIQSShadowProofRssCampaignJournalActiveSlot&&) noexcept = default;

SIQSShadowProofRssCampaignJournalActiveSlot& SIQSShadowProofRssCampaignJournalActiveSlot::operator=(
    SIQSShadowProofRssCampaignJournalActiveSlot&&) noexcept = default;

bool SIQSShadowProofRssCampaignJournalActiveSlot::active() const noexcept {
    return core_ != nullptr && permit_.has_value() && permit_->active();
}

uint32_t SIQSShadowProofRssCampaignJournalActiveSlot::slot_number() const noexcept {
    return active() ? permit_->durable_start_record().slot_number : 0;
}

SIQSShadowProofRssCampaignJournalSessionView
SIQSShadowProofRssCampaignJournalActiveSlot::view() const noexcept {
    return core_ != nullptr ? core_->view() : SIQSShadowProofRssCampaignJournalSessionView{};
}

SIQSShadowProofRssCampaignJournalTaintResult
SIQSShadowProofRssCampaignJournalActiveSlot::taint() && noexcept {
    if (!active()) {
        SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
        diagnostic.error = SIQSShadowProofRssCampaignJournalStoreError::session_inactive;
        return SIQSShadowProofRssCampaignJournalTaintResult({}, std::move(diagnostic));
    }

    permit_.reset();
    auto core = std::move(core_);
    auto result = core->append_pending_taint();
    return SIQSShadowProofRssCampaignJournalTaintResult(result.view, std::move(result.diagnostic));
}

SIQSShadowProofRssCampaignJournalBeginSlotResult::SIQSShadowProofRssCampaignJournalBeginSlotResult(
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic) noexcept
    : diagnostic_(std::move(diagnostic)) {}

SIQSShadowProofRssCampaignJournalBeginSlotResult::SIQSShadowProofRssCampaignJournalBeginSlotResult(
    SIQSShadowProofRssCampaignJournalActiveSlot&& active_slot) noexcept
    : active_slot_(std::move(active_slot)) {}

SIQSShadowProofRssCampaignJournalBeginSlotResult::
    ~SIQSShadowProofRssCampaignJournalBeginSlotResult() = default;

SIQSShadowProofRssCampaignJournalBeginSlotResult::SIQSShadowProofRssCampaignJournalBeginSlotResult(
    SIQSShadowProofRssCampaignJournalBeginSlotResult&& other) noexcept
    : active_slot_(std::move(other.active_slot_)), diagnostic_(std::move(other.diagnostic_)) {
    other.active_slot_.reset();
}

SIQSShadowProofRssCampaignJournalBeginSlotResult&
SIQSShadowProofRssCampaignJournalBeginSlotResult::operator=(
    SIQSShadowProofRssCampaignJournalBeginSlotResult&& other) noexcept {
    if (this != &other) {
        active_slot_ = std::move(other.active_slot_);
        diagnostic_ = std::move(other.diagnostic_);
        other.active_slot_.reset();
    }
    return *this;
}

SIQSShadowProofRssCampaignJournalBeginSlotResult::operator bool() const noexcept {
    return active_slot_.has_value() && active_slot_->active() &&
           diagnostic_.error == SIQSShadowProofRssCampaignJournalStoreError::none;
}

const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
SIQSShadowProofRssCampaignJournalBeginSlotResult::diagnostic() const noexcept {
    return diagnostic_;
}

std::optional<SIQSShadowProofRssCampaignJournalActiveSlot>
SIQSShadowProofRssCampaignJournalBeginSlotResult::take_active_slot() && noexcept {
    auto active_slot = std::move(active_slot_);
    active_slot_.reset();
    return active_slot;
}

SIQSShadowProofRssCampaignJournalSession::SIQSShadowProofRssCampaignJournalSession(
    std::unique_ptr<detail::SessionCore> core) noexcept
    : core_(std::move(core)) {}

SIQSShadowProofRssCampaignJournalSession::~SIQSShadowProofRssCampaignJournalSession() = default;

SIQSShadowProofRssCampaignJournalSession::SIQSShadowProofRssCampaignJournalSession(
    SIQSShadowProofRssCampaignJournalSession&&) noexcept = default;

SIQSShadowProofRssCampaignJournalSession& SIQSShadowProofRssCampaignJournalSession::operator=(
    SIQSShadowProofRssCampaignJournalSession&&) noexcept = default;

bool SIQSShadowProofRssCampaignJournalSession::active() const noexcept {
    return core_ != nullptr;
}

SIQSShadowProofRssCampaignJournalSessionView
SIQSShadowProofRssCampaignJournalSession::view() const noexcept {
    return core_ != nullptr ? core_->view() : SIQSShadowProofRssCampaignJournalSessionView{};
}

SIQSShadowProofRssCampaignJournalBeginSlotResult
SIQSShadowProofRssCampaignJournalSession::begin_next_slot() && noexcept {
    if (core_ == nullptr) {
        SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
        diagnostic.error = SIQSShadowProofRssCampaignJournalStoreError::session_inactive;
        return SIQSShadowProofRssCampaignJournalBeginSlotResult(std::move(diagnostic));
    }

    auto core = std::move(core_);
    detail::SessionBeginSlotResult result = core->begin_next_slot();
    if (!result) {
        if (result.diagnostic.error == SIQSShadowProofRssCampaignJournalStoreError::none) {
            result.diagnostic.error = SIQSShadowProofRssCampaignJournalStoreError::receipt_rejected;
        }
        return SIQSShadowProofRssCampaignJournalBeginSlotResult(std::move(result.diagnostic));
    }

    auto permit = std::move(*result.permit);
    result.permit.reset();
    return SIQSShadowProofRssCampaignJournalBeginSlotResult(
        SIQSShadowProofRssCampaignJournalActiveSlot(std::move(core), std::move(permit)));
}

SIQSShadowProofRssCampaignJournalTaintResult
SIQSShadowProofRssCampaignJournalSession::append_pending_taint() && noexcept {
    if (core_ == nullptr) {
        SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
        diagnostic.error = SIQSShadowProofRssCampaignJournalStoreError::session_inactive;
        return SIQSShadowProofRssCampaignJournalTaintResult({}, std::move(diagnostic));
    }

    auto core = std::move(core_);
    auto result = core->append_pending_taint();
    return SIQSShadowProofRssCampaignJournalTaintResult(result.view, std::move(result.diagnostic));
}

SIQSShadowProofRssCampaignJournalTaintResult::SIQSShadowProofRssCampaignJournalTaintResult(
    SIQSShadowProofRssCampaignJournalSessionView view,
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic) noexcept
    : view_(view), diagnostic_(std::move(diagnostic)) {}

SIQSShadowProofRssCampaignJournalTaintResult::~SIQSShadowProofRssCampaignJournalTaintResult() =
    default;

SIQSShadowProofRssCampaignJournalTaintResult::SIQSShadowProofRssCampaignJournalTaintResult(
    SIQSShadowProofRssCampaignJournalTaintResult&&) noexcept = default;

SIQSShadowProofRssCampaignJournalTaintResult&
SIQSShadowProofRssCampaignJournalTaintResult::operator=(
    SIQSShadowProofRssCampaignJournalTaintResult&&) noexcept = default;

SIQSShadowProofRssCampaignJournalTaintResult::operator bool() const noexcept {
    return view_.status == SIQSShadowProofRssJournalStatus::tainted &&
           view_.reason == SIQSShadowProofRssJournalReason::explicitly_tainted &&
           view_.action == SIQSShadowProofRssJournalAction::none &&
           diagnostic_.error == SIQSShadowProofRssCampaignJournalStoreError::none;
}

SIQSShadowProofRssCampaignJournalSessionView
SIQSShadowProofRssCampaignJournalTaintResult::view() const noexcept {
    return view_;
}

const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
SIQSShadowProofRssCampaignJournalTaintResult::diagnostic() const noexcept {
    return diagnostic_;
}

SIQSShadowProofRssCampaignJournalStoreOpenResult::SIQSShadowProofRssCampaignJournalStoreOpenResult(
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic) noexcept
    : diagnostic_(std::move(diagnostic)) {}

SIQSShadowProofRssCampaignJournalStoreOpenResult::SIQSShadowProofRssCampaignJournalStoreOpenResult(
    SIQSShadowProofRssCampaignJournalSession&& session) noexcept
    : session_(std::move(session)) {}

SIQSShadowProofRssCampaignJournalStoreOpenResult::
    ~SIQSShadowProofRssCampaignJournalStoreOpenResult() = default;

SIQSShadowProofRssCampaignJournalStoreOpenResult::SIQSShadowProofRssCampaignJournalStoreOpenResult(
    SIQSShadowProofRssCampaignJournalStoreOpenResult&& other) noexcept
    : session_(std::move(other.session_)), diagnostic_(std::move(other.diagnostic_)) {
    // A moved-from result must not retain an engaged, inactive capability
    // wrapper. The destination owns the only observable session token.
    other.session_.reset();
}

SIQSShadowProofRssCampaignJournalStoreOpenResult&
SIQSShadowProofRssCampaignJournalStoreOpenResult::operator=(
    SIQSShadowProofRssCampaignJournalStoreOpenResult&& other) noexcept {
    if (this != &other) {
        session_ = std::move(other.session_);
        diagnostic_ = std::move(other.diagnostic_);
        other.session_.reset();
    }
    return *this;
}

SIQSShadowProofRssCampaignJournalStoreOpenResult::operator bool() const noexcept {
    return session_.has_value() && session_->active() &&
           diagnostic_.error == SIQSShadowProofRssCampaignJournalStoreError::none;
}

const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
SIQSShadowProofRssCampaignJournalStoreOpenResult::diagnostic() const noexcept {
    return diagnostic_;
}

std::optional<SIQSShadowProofRssCampaignJournalSession>
SIQSShadowProofRssCampaignJournalStoreOpenResult::take_session() && noexcept {
    auto session = std::move(session_);
    session_.reset();
    return session;
}

namespace shadow_proof_rss_campaign_journal_store_detail {
namespace {

[[nodiscard]] SIQSShadowProofRssCampaignJournalStoreDiagnostic
make_common_diagnostic(SIQSShadowProofRssCampaignJournalStoreError error,
                       SIQSShadowProofRssCampaignJournalStoreObject object =
                           SIQSShadowProofRssCampaignJournalStoreObject::none) noexcept {
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
    diagnostic.error = error;
    diagnostic.object = object;
    return diagnostic;
}

[[nodiscard]] bool authority_locator_match(const SIQSShadowProofRssJournalStoreBinding& binding,
                                           const DeploymentEntry& deployment) noexcept {
    return deployment.trusted_base_id == binding.trusted_base_id &&
           deployment.relative_locator == binding.relative_locator;
}

[[nodiscard]] bool
preflight_is_ready(const SIQSShadowProofRssCampaignJournalResume& preflight) noexcept {
    return preflight.status == SIQSShadowProofRssJournalStatus::ready &&
           preflight.reason == SIQSShadowProofRssJournalReason::ready &&
           preflight.action == SIQSShadowProofRssJournalAction::create_header &&
           preflight.committed_slot_count == 0 && preflight.next_slot_number == 1 &&
           preflight.header_to_create.has_value() && !preflight.prepared_slot_start.has_value() &&
           !preflight.taint_to_append.has_value();
}

} // namespace

SIQSShadowProofRssCampaignJournalStoreOpenResult
SessionFactory::open_with_deployments(const SIQSShadowProofRssGatePolicy* policy,
                                      const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts,
                                      std::span<const DeploymentEntry> deployments) noexcept {
    try {
        // This pure preflight must remain before registry lookup or any
        // platform call, so rejected policy/facts never touch the filesystem.
        const auto preflight = resume_siqs_shadow_proof_rss_campaign_journal(
            policy, runtime_facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {});
        if (!preflight_is_ready(preflight)) {
            auto diagnostic = make_common_diagnostic(
                SIQSShadowProofRssCampaignJournalStoreError::preflight_rejected);
            diagnostic.journal_reason = preflight.reason;
            return SIQSShadowProofRssCampaignJournalStoreOpenResult(std::move(diagnostic));
        }

        const DeploymentEntry* selected = nullptr;
        std::size_t authority_match_count = 0;
        for (const DeploymentEntry& deployment : deployments) {
            if (authority_locator_match(policy->journal_store, deployment)) {
                ++authority_match_count;
                if (selected == nullptr) {
                    selected = &deployment;
                }
            }
        }

        if (authority_match_count == 0) {
            return SIQSShadowProofRssCampaignJournalStoreOpenResult(make_common_diagnostic(
                SIQSShadowProofRssCampaignJournalStoreError::binding_not_registered,
                SIQSShadowProofRssCampaignJournalStoreObject::deployment_registry));
        }
        if (authority_match_count != 1) {
            return SIQSShadowProofRssCampaignJournalStoreOpenResult(make_common_diagnostic(
                SIQSShadowProofRssCampaignJournalStoreError::binding_ambiguous,
                SIQSShadowProofRssCampaignJournalStoreObject::deployment_registry));
        }
        if (selected == nullptr || selected->store_id != policy->journal_store.store_id ||
            selected->trusted_base_path.empty()) {
            return SIQSShadowProofRssCampaignJournalStoreOpenResult(make_common_diagnostic(
                SIQSShadowProofRssCampaignJournalStoreError::registry_binding_mismatch,
                SIQSShadowProofRssCampaignJournalStoreObject::deployment_registry));
        }

        PlatformOpenResult platform = open_siqs_shadow_proof_rss_campaign_journal_platform_session(
            *policy, *runtime_facts, *selected);
        if (!platform) {
            if (platform.diagnostic.error == SIQSShadowProofRssCampaignJournalStoreError::none) {
                platform.diagnostic = make_common_diagnostic(
                    SIQSShadowProofRssCampaignJournalStoreError::unexpected_failure);
            }
            return SIQSShadowProofRssCampaignJournalStoreOpenResult(std::move(platform.diagnostic));
        }
        return SIQSShadowProofRssCampaignJournalStoreOpenResult(
            SIQSShadowProofRssCampaignJournalSession(std::move(platform.core)));
    } catch (const std::bad_alloc&) {
        return SIQSShadowProofRssCampaignJournalStoreOpenResult(make_common_diagnostic(
            SIQSShadowProofRssCampaignJournalStoreError::resource_exhausted));
    } catch (...) {
        return SIQSShadowProofRssCampaignJournalStoreOpenResult(make_common_diagnostic(
            SIQSShadowProofRssCampaignJournalStoreError::unexpected_failure));
    }
}

SessionArtifactBatchResult
SessionFactory::publish_artifact_batch(SIQSShadowProofRssCampaignJournalActiveSlot& active_slot,
                                       std::string_view stdout_bytes, std::string_view stderr_bytes,
                                       std::string_view joined_bytes) noexcept {
    if (!active_slot.active() || active_slot.core_ == nullptr || !active_slot.permit_.has_value()) {
        SessionArtifactBatchResult result;
        result.diagnostic.error = SIQSShadowProofRssCampaignJournalStoreError::session_inactive;
        return result;
    }
    return active_slot.core_->publish_artifact_batch(active_slot.permit_->durable_start_record(),
                                                     stdout_bytes, stderr_bytes, joined_bytes);
}

} // namespace shadow_proof_rss_campaign_journal_store_detail

SIQSShadowProofRssCampaignJournalStoreOpenResult
open_siqs_shadow_proof_rss_campaign_journal_session(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept {
    // Production provisioning is deliberately closed in the default build.
    // Deployment packaging may replace this private table; callers cannot
    // populate it at runtime through the public API.
    static constexpr std::span<const detail::DeploymentEntry> production_deployments{};
    return detail::SessionFactory::open_with_deployments(policy, runtime_facts,
                                                         production_deployments);
}

} // namespace gnfs::siqs
