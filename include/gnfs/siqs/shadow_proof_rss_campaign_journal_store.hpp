#pragma once

/// @file shadow_proof_rss_campaign_journal_store.hpp
/// @brief Leased native-store boundary for an approved SIQS RSS campaign.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_layout.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace gnfs::siqs {

enum class SIQSShadowProofRssCampaignJournalStoreError : uint8_t {
    none,
    preflight_rejected,
    platform_unavailable,
    binding_not_registered,
    binding_ambiguous,
    registry_binding_mismatch,
    base_open_failed,
    base_invalid,
    root_open_failed,
    root_invalid,
    lock_open_failed,
    lock_invalid,
    lock_busy,
    lock_failed,
    directory_open_failed,
    directory_read_failed,
    entry_metadata_failed,
    entry_trust_invalid,
    entry_open_failed,
    entry_read_failed,
    entry_identity_mismatch,
    entry_changed_during_read,
    snapshot_changed,
    layout_invalid,
    replay_rejected,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_campaign_journal_store_error_name(
    SIQSShadowProofRssCampaignJournalStoreError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssCampaignJournalStoreError::none:
        return "none";
    case SIQSShadowProofRssCampaignJournalStoreError::preflight_rejected:
        return "preflight_rejected";
    case SIQSShadowProofRssCampaignJournalStoreError::platform_unavailable:
        return "platform_unavailable";
    case SIQSShadowProofRssCampaignJournalStoreError::binding_not_registered:
        return "binding_not_registered";
    case SIQSShadowProofRssCampaignJournalStoreError::binding_ambiguous:
        return "binding_ambiguous";
    case SIQSShadowProofRssCampaignJournalStoreError::registry_binding_mismatch:
        return "registry_binding_mismatch";
    case SIQSShadowProofRssCampaignJournalStoreError::base_open_failed:
        return "base_open_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::base_invalid:
        return "base_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::root_open_failed:
        return "root_open_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::root_invalid:
        return "root_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::lock_open_failed:
        return "lock_open_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::lock_invalid:
        return "lock_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::lock_busy:
        return "lock_busy";
    case SIQSShadowProofRssCampaignJournalStoreError::lock_failed:
        return "lock_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::directory_open_failed:
        return "directory_open_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::directory_read_failed:
        return "directory_read_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::entry_metadata_failed:
        return "entry_metadata_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::entry_trust_invalid:
        return "entry_trust_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::entry_open_failed:
        return "entry_open_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::entry_read_failed:
        return "entry_read_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::entry_identity_mismatch:
        return "entry_identity_mismatch";
    case SIQSShadowProofRssCampaignJournalStoreError::entry_changed_during_read:
        return "entry_changed_during_read";
    case SIQSShadowProofRssCampaignJournalStoreError::snapshot_changed:
        return "snapshot_changed";
    case SIQSShadowProofRssCampaignJournalStoreError::layout_invalid:
        return "layout_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::replay_rejected:
        return "replay_rejected";
    case SIQSShadowProofRssCampaignJournalStoreError::resource_exhausted:
        return "resource_exhausted";
    case SIQSShadowProofRssCampaignJournalStoreError::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

enum class SIQSShadowProofRssCampaignJournalStoreObject : uint8_t {
    none,
    deployment_registry,
    trusted_base,
    store_root,
    session_lock,
    directory,
    journal_header,
    journal_record,
};

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_campaign_journal_store_object_name(
    SIQSShadowProofRssCampaignJournalStoreObject object) noexcept {
    switch (object) {
    case SIQSShadowProofRssCampaignJournalStoreObject::none:
        return "none";
    case SIQSShadowProofRssCampaignJournalStoreObject::deployment_registry:
        return "deployment_registry";
    case SIQSShadowProofRssCampaignJournalStoreObject::trusted_base:
        return "trusted_base";
    case SIQSShadowProofRssCampaignJournalStoreObject::store_root:
        return "store_root";
    case SIQSShadowProofRssCampaignJournalStoreObject::session_lock:
        return "session_lock";
    case SIQSShadowProofRssCampaignJournalStoreObject::directory:
        return "directory";
    case SIQSShadowProofRssCampaignJournalStoreObject::journal_header:
        return "journal_header";
    case SIQSShadowProofRssCampaignJournalStoreObject::journal_record:
        return "journal_record";
    }
    return "unknown";
}

struct SIQSShadowProofRssCampaignJournalStoreDiagnostic final {
    SIQSShadowProofRssCampaignJournalStoreError error =
        SIQSShadowProofRssCampaignJournalStoreError::none;
    SIQSShadowProofRssCampaignJournalStoreObject object =
        SIQSShadowProofRssCampaignJournalStoreObject::none;
    std::error_code native_error;
    SIQSShadowProofRssCampaignJournalLayoutDiagnostic layout;
    std::optional<SIQSShadowProofRssJournalReason> journal_reason;
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
};

/// Authority-free projection of the replay state held by one active lease.
struct SIQSShadowProofRssCampaignJournalSessionView final {
    SIQSShadowProofRssJournalStatus status = SIQSShadowProofRssJournalStatus::invalid;
    SIQSShadowProofRssJournalReason reason = SIQSShadowProofRssJournalReason::record_invalid;
    SIQSShadowProofRssJournalAction action = SIQSShadowProofRssJournalAction::none;
    uint32_t committed_slot_count = 0;
    uint32_t next_slot_number = 0;
    SIQSShadowProofRssCorpusDigest plan_digest;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssCampaignJournalSessionView&,
               const SIQSShadowProofRssCampaignJournalSessionView&) noexcept = default;
};

namespace shadow_proof_rss_campaign_journal_store_detail {
class SessionCore;
class SessionFactory;
} // namespace shadow_proof_rss_campaign_journal_store_detail

/// Move-only lifetime token for the native root and its cross-process lease.
class SIQSShadowProofRssCampaignJournalSession final {
public:
    SIQSShadowProofRssCampaignJournalSession() = delete;
    ~SIQSShadowProofRssCampaignJournalSession();

    SIQSShadowProofRssCampaignJournalSession(SIQSShadowProofRssCampaignJournalSession&&) noexcept;
    SIQSShadowProofRssCampaignJournalSession&
    operator=(SIQSShadowProofRssCampaignJournalSession&&) noexcept;

    SIQSShadowProofRssCampaignJournalSession(const SIQSShadowProofRssCampaignJournalSession&) =
        delete;
    SIQSShadowProofRssCampaignJournalSession&
    operator=(const SIQSShadowProofRssCampaignJournalSession&) = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] SIQSShadowProofRssCampaignJournalSessionView view() const noexcept;

private:
    explicit SIQSShadowProofRssCampaignJournalSession(
        std::unique_ptr<shadow_proof_rss_campaign_journal_store_detail::SessionCore>) noexcept;

    std::unique_ptr<shadow_proof_rss_campaign_journal_store_detail::SessionCore> core_;

    friend class shadow_proof_rss_campaign_journal_store_detail::SessionFactory;
};

class SIQSShadowProofRssCampaignJournalStoreOpenResult final {
public:
    SIQSShadowProofRssCampaignJournalStoreOpenResult() = delete;
    ~SIQSShadowProofRssCampaignJournalStoreOpenResult();

    SIQSShadowProofRssCampaignJournalStoreOpenResult(
        SIQSShadowProofRssCampaignJournalStoreOpenResult&&) noexcept;
    SIQSShadowProofRssCampaignJournalStoreOpenResult&
    operator=(SIQSShadowProofRssCampaignJournalStoreOpenResult&&) noexcept;

    SIQSShadowProofRssCampaignJournalStoreOpenResult(
        const SIQSShadowProofRssCampaignJournalStoreOpenResult&) = delete;
    SIQSShadowProofRssCampaignJournalStoreOpenResult&
    operator=(const SIQSShadowProofRssCampaignJournalStoreOpenResult&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
    diagnostic() const noexcept;
    [[nodiscard]] std::optional<SIQSShadowProofRssCampaignJournalSession>
    take_session() && noexcept;

private:
    explicit SIQSShadowProofRssCampaignJournalStoreOpenResult(
        SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic) noexcept;
    explicit SIQSShadowProofRssCampaignJournalStoreOpenResult(
        SIQSShadowProofRssCampaignJournalSession&& session) noexcept;

    std::optional<SIQSShadowProofRssCampaignJournalSession> session_;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic_;

    friend class shadow_proof_rss_campaign_journal_store_detail::SessionFactory;
};

/// Open the one deployment-registered store named by `policy`. No caller path,
/// descriptor, resolver, snapshot, or publication result crosses this boundary.
[[nodiscard]] SIQSShadowProofRssCampaignJournalStoreOpenResult
open_siqs_shadow_proof_rss_campaign_journal_session(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept;

} // namespace gnfs::siqs
