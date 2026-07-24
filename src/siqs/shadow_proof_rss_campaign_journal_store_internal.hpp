#pragma once

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
    std::vector<std::string> environment;
    std::chrono::milliseconds timeout{35000};
    uint64_t expected_owner = 0;
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

    SameChildExecutionReceipt& operator=(SameChildExecutionReceipt&& other) noexcept {
        if (this != &other) {
            evidence_ = std::move(other.evidence_);
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

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

struct PlatformOpenResult final {
    std::unique_ptr<SessionCore> core;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;

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

[[nodiscard]] PlatformOpenResult open_siqs_shadow_proof_rss_campaign_journal_platform_session(
    const SIQSShadowProofRssGatePolicy& policy,
    const SIQSShadowProofRssCampaignRuntimeFacts& runtime_facts,
    const DeploymentEntry& deployment) noexcept;

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
