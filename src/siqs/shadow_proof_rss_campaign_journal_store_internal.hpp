#pragma once

#include "shadow_proof_rss_campaign_reconciliation_internal.hpp"
#include "shadow_proof_rss_probe_execution_identity_internal.hpp"

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_store.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

/// Private store-level durability seam. It delegates to the production native
/// publisher by default; native-store tests may inject failures without gaining
/// receipt or launch authority.
class PublicationOps {
public:
    PublicationOps() = default;
    virtual ~PublicationOps() = default;

    PublicationOps(const PublicationOps&) = delete;
    PublicationOps& operator=(const PublicationOps&) = delete;
    PublicationOps(PublicationOps&&) = delete;
    PublicationOps& operator=(PublicationOps&&) = delete;

    [[nodiscard]] virtual util::durable_immutable_file::PublishResult
    publish_at(util::durable_immutable_file::NativeHandle parent_handle,
               const std::filesystem::path& leaf, std::span<const std::byte> bytes) noexcept = 0;

    [[nodiscard]] virtual util::durable_immutable_file::PublishResult
    confirm_durable_at(util::durable_immutable_file::NativeHandle parent_handle,
                       const std::filesystem::path& leaf) noexcept = 0;
};

/// Complete deployment-owned approval and runtime contract for one campaign
/// namespace. Public policy/runtime values are untrusted claims: the store
/// selects a unique private deployment row, requires an exact field-by-field
/// match, and then constructs the authority-bearing session from this owning
/// snapshot instead of retaining caller-provided views.
struct ApprovedCampaignBinding final {
    std::string corpus_id;
    SIQSShadowProofRssCorpusDigest corpus_digest;
    SIQSShadowProofRssOperatingSystem operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    SIQSShadowProofRssArchitecture architecture = SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t resolved_production_sieve_workers = 0;
    std::string candidate_revision;
    std::string approval_id;
    SIQSShadowProofRssProbeExecutionIdentity probe_execution_identity;
    std::optional<uint64_t> deployment_budget_bytes;
    std::optional<uint64_t> reserved_headroom_bytes;
    bool release_build = false;
    bool ndebug = false;
};

/// Private executable row selected before a slot can launch. The complete
/// child environment and timeout are deployment-owned; callers cannot amend
/// either through the runner API.
struct ProbeExecutableBinding final {
    std::filesystem::path executable;
    std::string candidate_revision;
    SIQSShadowProofRssProbeKind probe_kind = SIQSShadowProofRssProbeKind::unknown;
    shadow_proof_rss_probe_execution_identity_detail::ProbeExecutableLaunchProfile launch_profile =
        shadow_proof_rss_probe_execution_identity_detail::ProbeExecutableLaunchProfile::unknown;
    std::vector<std::string> environment;
    std::chrono::milliseconds timeout{35000};
    uint64_t expected_owner = 0;
    SIQSShadowProofRssProbeExecutionIdentity probe_execution_identity;
};

/// Private deployment table row. Production builds keep the table empty.
/// Tests may supply owning rows through SessionFactory without adding a public
/// path, descriptor, resolver, or registry installation API.
struct DeploymentEntry final {
    SIQSShadowProofRssCorpusDigest trusted_base_id;
    SIQSShadowProofRssCorpusDigest store_id;
    std::string relative_locator;
    std::filesystem::path trusted_base_path;
    uint64_t expected_owner = 0;
    SIQSShadowProofRssProbeKind probe_kind = SIQSShadowProofRssProbeKind::unknown;
    ApprovedCampaignBinding approval;
    PublicationOps* publication_ops = nullptr;
    std::optional<ProbeExecutableBinding> holdout_probe;
};

struct PlatformReconciliationOpenResult;

/// Move-only, opaque proof that one deployment-owned row has already passed
/// closed-registry selection and exact claim validation. It deliberately has
/// no public constructor or accessor, so a path, deployment row, publication
/// seam, or approved view cannot be recovered from it by an ordinary caller.
class ApprovedReconciliationBinding final {
public:
    ApprovedReconciliationBinding(const ApprovedReconciliationBinding&) = delete;
    ApprovedReconciliationBinding& operator=(const ApprovedReconciliationBinding&) = delete;
    ApprovedReconciliationBinding(ApprovedReconciliationBinding&& other) noexcept
        : deployment_(std::move(other.deployment_)),
          expected_plan_digest_(other.expected_plan_digest_) {
        bind_approved_views();
    }
    ApprovedReconciliationBinding& operator=(ApprovedReconciliationBinding&&) = delete;
    ~ApprovedReconciliationBinding() = default;

private:
    ApprovedReconciliationBinding() = default;

    void bind_approved_views() noexcept {
        approved_policy_.approved = true;
        approved_policy_.corpus_id = deployment_.approval.corpus_id;
        approved_policy_.corpus_digest = deployment_.approval.corpus_digest;
        approved_policy_.operating_system = deployment_.approval.operating_system;
        approved_policy_.architecture = deployment_.approval.architecture;
        approved_policy_.memory_backend = deployment_.approval.memory_backend;
        approved_policy_.resolved_production_sieve_workers =
            deployment_.approval.resolved_production_sieve_workers;
        approved_policy_.candidate_revision = deployment_.approval.candidate_revision;
        approved_policy_.probe_execution_identity = deployment_.approval.probe_execution_identity;
        approved_policy_.approval_id = deployment_.approval.approval_id;
        approved_policy_.journal_store = {
            deployment_.trusted_base_id,
            deployment_.store_id,
            deployment_.relative_locator,
        };
        approved_policy_.deployment_budget_bytes = deployment_.approval.deployment_budget_bytes;
        approved_policy_.reserved_headroom_bytes = deployment_.approval.reserved_headroom_bytes;

        approved_runtime_facts_.operating_system = deployment_.approval.operating_system;
        approved_runtime_facts_.architecture = deployment_.approval.architecture;
        approved_runtime_facts_.memory_backend = deployment_.approval.memory_backend;
        approved_runtime_facts_.resolved_production_sieve_workers =
            deployment_.approval.resolved_production_sieve_workers;
        approved_runtime_facts_.probe_kind = deployment_.probe_kind;
        approved_runtime_facts_.candidate_revision = deployment_.approval.candidate_revision;
        approved_runtime_facts_.probe_execution_identity =
            deployment_.approval.probe_execution_identity;
        approved_runtime_facts_.release_build = deployment_.approval.release_build;
        approved_runtime_facts_.ndebug = deployment_.approval.ndebug;
    }

    DeploymentEntry deployment_;
    SIQSShadowProofRssGatePolicy approved_policy_;
    SIQSShadowProofRssCampaignRuntimeFacts approved_runtime_facts_;
    SIQSShadowProofRssCorpusDigest expected_plan_digest_;

    friend class ReconciliationOrchestrator;
    friend class ReconciliationTestPeer;
    friend PlatformReconciliationOpenResult
    open_siqs_shadow_proof_rss_campaign_journal_platform_reconciliation(
        ApprovedReconciliationBinding binding) noexcept;
};

struct SessionBeginSlotResult final {
    std::optional<SIQSShadowProofRssLaunchPermit> permit;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return permit.has_value() && permit->active() &&
               diagnostic.error == SIQSShadowProofRssCampaignJournalStoreError::none;
    }
};

struct SessionTaintResult final {
    SIQSShadowProofRssCampaignJournalSessionView view;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return view.status == SIQSShadowProofRssJournalStatus::tainted &&
               view.reason == SIQSShadowProofRssJournalReason::explicitly_tainted &&
               view.action == SIQSShadowProofRssJournalAction::none &&
               diagnostic.error == SIQSShadowProofRssCampaignJournalStoreError::none;
    }
};

struct SessionArtifactBatchResult final {
    std::array<SIQSShadowProofRssArtifactSeal, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACTS_PER_SLOT>
        seals{};
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return diagnostic.error == SIQSShadowProofRssCampaignJournalStoreError::none &&
               seals[0].committed && seals[1].committed && seals[2].committed;
    }
};

/// Private synchronous projection produced only after one bounded child has
/// exited successfully, both streams reached EOF, cleanup completed, and the
/// strict stream join accepted the exact bytes. Byte streams are owned; the
/// canonical slot retains policy-backed views while the session core remains
/// alive. Ordinary data-only transport results cannot construct its receipt.
struct SameChildExecutionEvidence final {
    SIQSShadowProofRssCampaignJournalRecord durable_start_record;
    SIQSShadowProofRssCorpusDigest policy_binding_digest;
    SIQSShadowProofRssCampaignSlot slot;
    SIQSShadowProofRssOperatingSystem operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    SIQSShadowProofRssArchitecture architecture = SIQSShadowProofRssArchitecture::unknown;
    util::ProcessMemoryBackend memory_backend = util::ProcessMemoryBackend::Unsupported;
    std::size_t resolved_production_sieve_workers = 0;
    SIQSShadowProofRssProbeKind deployment_probe_kind = SIQSShadowProofRssProbeKind::unknown;
    SIQSShadowProofRssProbeExecutionIdentity probe_execution_identity;
    bool same_object_authenticated = false;
    bool fresh_process = false;
    bool completed = false;
    SIQSShadowProofRssFactorIdentity factor_identity = SIQSShadowProofRssFactorIdentity::unknown;
    SIQSShadowProofRssEvidence proof_evidence = SIQSShadowProofRssEvidence::unknown;
    SIQSShadowProofRssEvidence matrix_evidence = SIQSShadowProofRssEvidence::unknown;
    uint64_t relations_found = 0;
    uint64_t polynomials_used = 0;
    uint64_t absolute_peak_rss_bytes = 0;
    std::optional<uint64_t> current_rss_bytes;
    std::optional<uint64_t> peak_growth_bytes;
    uint64_t wall_ns = 0;
    std::string stdout_bytes;
    std::string stderr_bytes;
    std::string joined_bytes;
};

class SameChildExecutionReceipt final {
public:
    SameChildExecutionReceipt() = delete;
    SameChildExecutionReceipt(const SameChildExecutionReceipt&) = delete;
    SameChildExecutionReceipt& operator=(const SameChildExecutionReceipt&) = delete;

    SameChildExecutionReceipt(SameChildExecutionReceipt&& other) noexcept
        : evidence_(std::move(other.evidence_)), active_(std::exchange(other.active_, false)) {}

    SameChildExecutionReceipt& operator=(SameChildExecutionReceipt&&) = delete;

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

private:
    explicit SameChildExecutionReceipt(SameChildExecutionEvidence evidence) noexcept
        : evidence_(std::move(evidence)), active_(true) {}

    SameChildExecutionEvidence evidence_;
    bool active_ = false;

    friend class SessionCore;
    friend class SlotRunnerFactory;
};

enum class SessionCommitStatus : uint8_t {
    committed,
    taint_allowed,
    outcome_uncertain,
};

struct SessionCommitResult final {
    SessionCommitStatus status = SessionCommitStatus::outcome_uncertain;
    SIQSShadowProofRssCampaignJournalSessionView view;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == SessionCommitStatus::committed &&
               diagnostic.error == SIQSShadowProofRssCampaignJournalStoreError::none;
    }
};

struct SessionSlotRunContext final {
    const SIQSShadowProofRssGatePolicy* policy = nullptr;
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts = nullptr;
    const ProbeExecutableBinding* executable = nullptr;
    SIQSShadowProofRssProbeKind deployment_probe_kind = SIQSShadowProofRssProbeKind::unknown;
    SIQSShadowProofRssCampaignSlot slot;
};

struct SessionPrepareRunResult final {
    std::optional<SessionSlotRunContext> context;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return context.has_value() && context->policy != nullptr &&
               context->runtime_facts != nullptr && context->executable != nullptr &&
               context->deployment_probe_kind != SIQSShadowProofRssProbeKind::unknown &&
               diagnostic.error == SIQSShadowProofRssCampaignJournalStoreError::none;
    }
};

class ReconciliationCore {
public:
    ReconciliationCore() = default;
    virtual ~ReconciliationCore() = default;

    ReconciliationCore(const ReconciliationCore&) = delete;
    ReconciliationCore& operator=(const ReconciliationCore&) = delete;
    ReconciliationCore(ReconciliationCore&&) = delete;
    ReconciliationCore& operator=(ReconciliationCore&&) = delete;

    [[nodiscard]] virtual CoreReconciliationResult reconcile() noexcept = 0;
};

class SessionCore {
public:
    SessionCore() = default;
    virtual ~SessionCore() = default;

    SessionCore(const SessionCore&) = delete;
    SessionCore& operator=(const SessionCore&) = delete;
    SessionCore(SessionCore&&) = delete;
    SessionCore& operator=(SessionCore&&) = delete;

    [[nodiscard]] virtual SIQSShadowProofRssCampaignJournalSessionView view() const noexcept = 0;
    [[nodiscard]] virtual SessionBeginSlotResult begin_next_slot() noexcept = 0;
    [[nodiscard]] virtual SessionTaintResult append_pending_taint() noexcept = 0;
    [[nodiscard]] virtual SessionPrepareRunResult prepare_pending_slot_run(
        const SIQSShadowProofRssCampaignJournalRecord& durable_start_record) noexcept = 0;
    [[nodiscard]] virtual SessionArtifactBatchResult
    publish_artifact_batch(const SIQSShadowProofRssCampaignJournalRecord& durable_start_record,
                           std::string_view stdout_bytes, std::string_view stderr_bytes,
                           std::string_view joined_bytes) noexcept = 0;
    [[nodiscard]] virtual SessionCommitResult
    commit_same_child_execution(SIQSShadowProofRssLaunchPermit&& permit,
                                SameChildExecutionReceipt&& receipt) noexcept = 0;

protected:
    [[nodiscard]] static constexpr SIQSShadowProofRssDurableRecordReceipt
    issue_durable_record_receipt(const SIQSShadowProofRssCampaignJournalRecord& record) noexcept {
        return SIQSShadowProofRssDurableRecordReceipt(record);
    }

    [[nodiscard]] static std::optional<SameChildExecutionEvidence>
    consume_same_child_execution_receipt(SameChildExecutionReceipt&& receipt) noexcept {
        if (!receipt.active_) {
            return std::nullopt;
        }
        receipt.active_ = false;
        return std::move(receipt.evidence_);
    }

private:
    friend class PosixCampaignEngine;
};

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalSessionView
make_session_view(const SIQSShadowProofRssCampaignJournalResume& resume) noexcept {
    return {
        .status = resume.status,
        .reason = resume.reason,
        .action = resume.action,
        .committed_slot_count = resume.committed_slot_count,
        .next_slot_number = resume.next_slot_number,
        .plan_digest = resume.plan_digest,
    };
}

/// Shared exact predicate for the pure absent-journal preflight. Production
/// composition may classify the probe only after this predicate succeeds; the
/// native store uses the same predicate before registry lookup or filesystem
/// access.
[[nodiscard]] constexpr bool absent_journal_preflight_is_ready(
    const SIQSShadowProofRssCampaignJournalResume& preflight) noexcept {
    return preflight.status == SIQSShadowProofRssJournalStatus::ready &&
           preflight.reason == SIQSShadowProofRssJournalReason::ready &&
           preflight.action == SIQSShadowProofRssJournalAction::create_header &&
           preflight.committed_slot_count == 0 && preflight.next_slot_number == 1 &&
           preflight.header_to_create.has_value() && !preflight.prepared_slot_start.has_value() &&
           !preflight.taint_to_append.has_value();
}

struct PlatformSessionOpenResult final {
    PlatformSessionOpenResult() = default;
    PlatformSessionOpenResult(
        std::unique_ptr<SessionCore> selected_core,
        SIQSShadowProofRssCampaignJournalStoreDiagnostic selected_diagnostic) noexcept
        : core(std::move(selected_core)), diagnostic(std::move(selected_diagnostic)) {}
    PlatformSessionOpenResult(const PlatformSessionOpenResult&) = delete;
    PlatformSessionOpenResult& operator=(const PlatformSessionOpenResult&) = delete;
    PlatformSessionOpenResult(PlatformSessionOpenResult&&) noexcept = default;
    PlatformSessionOpenResult& operator=(PlatformSessionOpenResult&&) = delete;

    std::unique_ptr<SessionCore> core;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return core != nullptr &&
               diagnostic.error == SIQSShadowProofRssCampaignJournalStoreError::none;
    }
};

struct PlatformReconciliationOpenResult final {
    PlatformReconciliationOpenResult() = default;
    PlatformReconciliationOpenResult(
        std::unique_ptr<ReconciliationCore> selected_core,
        SIQSShadowProofRssCampaignJournalStoreDiagnostic selected_diagnostic) noexcept
        : core(std::move(selected_core)), diagnostic(std::move(selected_diagnostic)) {}
    PlatformReconciliationOpenResult(const PlatformReconciliationOpenResult&) = delete;
    PlatformReconciliationOpenResult& operator=(const PlatformReconciliationOpenResult&) = delete;
    PlatformReconciliationOpenResult(PlatformReconciliationOpenResult&&) noexcept = default;
    PlatformReconciliationOpenResult& operator=(PlatformReconciliationOpenResult&&) = delete;

    std::unique_ptr<ReconciliationCore> core;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
    bool no_persistent_state = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return core != nullptr &&
               diagnostic.error == SIQSShadowProofRssCampaignJournalStoreError::none;
    }
};

class SessionFactory final {
public:
    [[nodiscard]] static SIQSShadowProofRssCampaignJournalStoreOpenResult
    open_with_deployments(const SIQSShadowProofRssGatePolicy* policy,
                          const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts,
                          std::span<const DeploymentEntry> deployments) noexcept;

    /// Private integration seam used by native-store tests. It can persist an
    /// artifact batch but cannot issue a commit payload, receipt, or launch
    /// authority.
    [[nodiscard]] static SessionArtifactBatchResult
    publish_artifact_batch(SIQSShadowProofRssCampaignJournalActiveSlot& active_slot,
                           std::string_view stdout_bytes, std::string_view stderr_bytes,
                           std::string_view joined_bytes) noexcept;
};

[[nodiscard]] PlatformSessionOpenResult
open_siqs_shadow_proof_rss_campaign_journal_platform_session(
    const SIQSShadowProofRssGatePolicy& policy,
    const SIQSShadowProofRssCampaignRuntimeFacts& runtime_facts,
    const DeploymentEntry& deployment) noexcept;

[[nodiscard]] PlatformReconciliationOpenResult
open_siqs_shadow_proof_rss_campaign_journal_platform_reconciliation(
    ApprovedReconciliationBinding binding) noexcept;

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
