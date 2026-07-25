#include "shadow_proof_rss_campaign_journal_store_internal.hpp"
#include "shadow_proof_rss_probe_execution_identity_internal.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string>
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

[[nodiscard]] SIQSShadowProofRssGatePolicy
approved_policy_view(const DeploymentEntry& deployment) noexcept {
    const ApprovedCampaignBinding& approval = deployment.approval;
    return {
        .approved = true,
        .corpus_id = approval.corpus_id,
        .corpus_digest = approval.corpus_digest,
        .operating_system = approval.operating_system,
        .architecture = approval.architecture,
        .memory_backend = approval.memory_backend,
        .resolved_production_sieve_workers = approval.resolved_production_sieve_workers,
        .candidate_revision = approval.candidate_revision,
        .probe_execution_identity = approval.probe_execution_identity,
        .approval_id = approval.approval_id,
        .journal_store =
            {
                .trusted_base_id = deployment.trusted_base_id,
                .store_id = deployment.store_id,
                .relative_locator = deployment.relative_locator,
            },
        .deployment_budget_bytes = approval.deployment_budget_bytes,
        .reserved_headroom_bytes = approval.reserved_headroom_bytes,
    };
}

[[nodiscard]] SIQSShadowProofRssCampaignRuntimeFacts
approved_runtime_facts_view(const DeploymentEntry& deployment) noexcept {
    const ApprovedCampaignBinding& approval = deployment.approval;
    return {
        .operating_system = approval.operating_system,
        .architecture = approval.architecture,
        .memory_backend = approval.memory_backend,
        .resolved_production_sieve_workers = approval.resolved_production_sieve_workers,
        .probe_kind = deployment.probe_kind,
        .candidate_revision = approval.candidate_revision,
        .probe_execution_identity = approval.probe_execution_identity,
        .release_build = approval.release_build,
        .ndebug = approval.ndebug,
    };
}

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

[[nodiscard]] bool
executable_environment_is_canonical(std::span<const std::string> environment) noexcept {
    return shadow_proof_rss_probe_execution_identity_detail::canonical_detail::
        environment_is_canonical(environment);
}

[[nodiscard]] bool deployment_contract_is_well_formed(const DeploymentEntry& deployment) noexcept {
    constexpr auto max_probe_timeout = std::chrono::seconds(60);
    const bool known_probe_kind =
        deployment.probe_kind == SIQSShadowProofRssProbeKind::synthetic_test ||
        deployment.probe_kind == SIQSShadowProofRssProbeKind::production_holdout;
    if (!known_probe_kind || deployment.trusted_base_path.empty() ||
        !siqs_shadow_proof_rss_probe_execution_identity_is_valid(
            deployment.approval.probe_execution_identity)) {
        return false;
    }
    if (deployment.probe_kind == SIQSShadowProofRssProbeKind::production_holdout &&
        (!deployment.holdout_probe.has_value() || deployment.publication_ops != nullptr)) {
        return false;
    }
    if (!deployment.holdout_probe.has_value()) {
        return true;
    }
    const ProbeExecutableBinding& executable = *deployment.holdout_probe;
    const auto& native_executable = executable.executable.native();
    if (executable.executable.empty() || !executable.executable.is_absolute() ||
        native_executable.find('\0') != std::string::npos ||
        executable.candidate_revision != deployment.approval.candidate_revision ||
        executable.probe_kind != deployment.probe_kind ||
        executable.expected_owner != deployment.expected_owner ||
        executable.timeout <= std::chrono::milliseconds::zero() ||
        executable.timeout > max_probe_timeout ||
        !executable_environment_is_canonical(executable.environment) ||
        !siqs_shadow_proof_rss_probe_execution_identity_is_valid(
            executable.probe_execution_identity) ||
        executable.probe_execution_identity != deployment.approval.probe_execution_identity) {
        return false;
    }

    using shadow_proof_rss_probe_execution_identity_detail::
        make_siqs_shadow_proof_rss_probe_execution_identity;
    using shadow_proof_rss_probe_execution_identity_detail::ProbeExecutionContractInput;
    const auto canonical_identity =
        make_siqs_shadow_proof_rss_probe_execution_identity(ProbeExecutionContractInput{
            .executable_sha256 = executable.probe_execution_identity.executable_sha256,
            .probe_kind = deployment.probe_kind,
            .launch_profile = executable.launch_profile,
            .candidate_revision = deployment.approval.candidate_revision,
            .operating_system = deployment.approval.operating_system,
            .architecture = deployment.approval.architecture,
            .memory_backend = deployment.approval.memory_backend,
            .resolved_production_sieve_workers =
                deployment.approval.resolved_production_sieve_workers,
            .release_build = deployment.approval.release_build,
            .ndebug = deployment.approval.ndebug,
            .environment = executable.environment,
            .timeout_ms = static_cast<std::uint64_t>(executable.timeout.count()),
            .expected_owner = executable.expected_owner,
        });
    return canonical_identity.has_value() &&
           *canonical_identity == executable.probe_execution_identity;
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

[[nodiscard]] bool
production_launch_profile_is_supported_on_host(const DeploymentEntry& deployment) noexcept {
    if (deployment.probe_kind != SIQSShadowProofRssProbeKind::production_holdout) {
        return true;
    }
    if (!deployment.holdout_probe.has_value()) {
        return false;
    }

#if defined(__linux__)
    using LaunchProfile =
        shadow_proof_rss_probe_execution_identity_detail::ProbeExecutableLaunchProfile;
    return deployment.approval.operating_system == SIQSShadowProofRssOperatingSystem::linux &&
           deployment.holdout_probe->launch_profile ==
               LaunchProfile::linux_sealed_memfd_execveat_v1;
#else
    // Darwin's hardened launch profile remains contract-only until the
    // same-object authenticator is implemented. Windows and unknown hosts are
    // likewise explicitly unavailable.
    return false;
#endif
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
        if (selected == nullptr || !deployment_contract_is_well_formed(*selected)) {
            return SIQSShadowProofRssCampaignJournalStoreOpenResult(make_common_diagnostic(
                SIQSShadowProofRssCampaignJournalStoreError::registry_binding_mismatch,
                SIQSShadowProofRssCampaignJournalStoreObject::deployment_registry));
        }

        const SIQSShadowProofRssGatePolicy approved_policy = approved_policy_view(*selected);
        const SIQSShadowProofRssCampaignRuntimeFacts approved_runtime_facts =
            approved_runtime_facts_view(*selected);
        const auto approved_preflight = resume_siqs_shadow_proof_rss_campaign_journal(
            &approved_policy, &approved_runtime_facts, SIQSShadowProofRssJournalPresence::absent,
            nullptr, {});
        if (!preflight_is_ready(approved_preflight) ||
            !policy_claim_matches(*policy, approved_policy) ||
            !runtime_claim_matches(*runtime_facts, approved_runtime_facts)) {
            return SIQSShadowProofRssCampaignJournalStoreOpenResult(make_common_diagnostic(
                SIQSShadowProofRssCampaignJournalStoreError::registry_binding_mismatch,
                SIQSShadowProofRssCampaignJournalStoreObject::deployment_registry));
        }

        // No production slot may publish a durable start on a platform whose
        // exact host/profile same-object authenticator is not available.
        if (!production_launch_profile_is_supported_on_host(*selected)) {
            return SIQSShadowProofRssCampaignJournalStoreOpenResult(make_common_diagnostic(
                SIQSShadowProofRssCampaignJournalStoreError::platform_unavailable,
                SIQSShadowProofRssCampaignJournalStoreObject::probe_executable));
        }

        PlatformOpenResult platform = open_siqs_shadow_proof_rss_campaign_journal_platform_session(
            approved_policy, approved_runtime_facts, *selected);
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
