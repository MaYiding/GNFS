#pragma once

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_store.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {

/// Private deployment table row. Production builds keep the table empty.
/// Tests may supply owning rows through SessionFactory without adding a public
/// path, descriptor, resolver, or registry installation API.
struct DeploymentEntry final {
    SIQSShadowProofRssCorpusDigest trusted_base_id;
    SIQSShadowProofRssCorpusDigest store_id;
    std::string relative_locator;
    std::filesystem::path trusted_base_path;
    uint64_t expected_owner = 0;
};

struct SessionBeginSlotResult final {
    std::optional<SIQSShadowProofRssLaunchPermit> permit;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return permit.has_value() && permit->active() &&
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
};

[[nodiscard]] PlatformOpenResult open_siqs_shadow_proof_rss_campaign_journal_platform_session(
    const SIQSShadowProofRssGatePolicy& policy,
    const SIQSShadowProofRssCampaignRuntimeFacts& runtime_facts,
    const DeploymentEntry& deployment) noexcept;

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
