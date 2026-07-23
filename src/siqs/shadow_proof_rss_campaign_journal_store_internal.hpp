#pragma once

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_store.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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

/// Private deployment table row. Production builds keep the table empty.
/// Tests may supply owning rows through SessionFactory without adding a public
/// path, descriptor, resolver, or registry installation API.
struct DeploymentEntry final {
    SIQSShadowProofRssCorpusDigest trusted_base_id;
    SIQSShadowProofRssCorpusDigest store_id;
    std::string relative_locator;
    std::filesystem::path trusted_base_path;
    uint64_t expected_owner = 0;
    PublicationOps* publication_ops = nullptr;
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
    [[nodiscard]] virtual SessionArtifactBatchResult
    publish_artifact_batch(const SIQSShadowProofRssCampaignJournalRecord& durable_start_record,
                           std::string_view stdout_bytes, std::string_view stderr_bytes,
                           std::string_view joined_bytes) noexcept = 0;

protected:
    [[nodiscard]] static constexpr SIQSShadowProofRssDurableRecordReceipt
    issue_durable_record_receipt(const SIQSShadowProofRssCampaignJournalRecord& record) noexcept {
        return SIQSShadowProofRssDurableRecordReceipt(record);
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
