#pragma once

/// @file shadow_proof_rss_campaign_journal_store.hpp
/// @brief Leased native-store boundary for an approved SIQS RSS campaign.

#include <gnfs/siqs/shadow_proof_rss_campaign_artifact_layout.hpp>
#include <gnfs/siqs/shadow_proof_rss_campaign_journal_layout.hpp>
#include <gnfs/util/durable_immutable_file.hpp>

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
    executable_authentication_failed,
    base_open_failed,
    base_invalid,
    root_open_failed,
    root_invalid,
    artifact_root_open_failed,
    artifact_root_invalid,
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
    artifact_layout_invalid,
    artifact_consistency_invalid,
    replay_rejected,
    session_inactive,
    session_action_invalid,
    journal_encode_failed,
    publication_conflict,
    publication_failed,
    receipt_rejected,
    commit_outcome_uncertain,
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
    case SIQSShadowProofRssCampaignJournalStoreError::executable_authentication_failed:
        return "executable_authentication_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::base_open_failed:
        return "base_open_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::base_invalid:
        return "base_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::root_open_failed:
        return "root_open_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::root_invalid:
        return "root_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::artifact_root_open_failed:
        return "artifact_root_open_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::artifact_root_invalid:
        return "artifact_root_invalid";
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
    case SIQSShadowProofRssCampaignJournalStoreError::artifact_layout_invalid:
        return "artifact_layout_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::artifact_consistency_invalid:
        return "artifact_consistency_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::replay_rejected:
        return "replay_rejected";
    case SIQSShadowProofRssCampaignJournalStoreError::session_inactive:
        return "session_inactive";
    case SIQSShadowProofRssCampaignJournalStoreError::session_action_invalid:
        return "session_action_invalid";
    case SIQSShadowProofRssCampaignJournalStoreError::journal_encode_failed:
        return "journal_encode_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::publication_conflict:
        return "publication_conflict";
    case SIQSShadowProofRssCampaignJournalStoreError::publication_failed:
        return "publication_failed";
    case SIQSShadowProofRssCampaignJournalStoreError::receipt_rejected:
        return "receipt_rejected";
    case SIQSShadowProofRssCampaignJournalStoreError::commit_outcome_uncertain:
        return "commit_outcome_uncertain";
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
    probe_executable,
    trusted_base,
    store_root,
    artifact_root,
    artifact,
    session_lock,
    directory,
    journal_header,
    journal_record,
    terminal_gate_record,
};

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_campaign_journal_store_object_name(
    SIQSShadowProofRssCampaignJournalStoreObject object) noexcept {
    switch (object) {
    case SIQSShadowProofRssCampaignJournalStoreObject::none:
        return "none";
    case SIQSShadowProofRssCampaignJournalStoreObject::deployment_registry:
        return "deployment_registry";
    case SIQSShadowProofRssCampaignJournalStoreObject::probe_executable:
        return "probe_executable";
    case SIQSShadowProofRssCampaignJournalStoreObject::trusted_base:
        return "trusted_base";
    case SIQSShadowProofRssCampaignJournalStoreObject::store_root:
        return "store_root";
    case SIQSShadowProofRssCampaignJournalStoreObject::artifact_root:
        return "artifact_root";
    case SIQSShadowProofRssCampaignJournalStoreObject::artifact:
        return "artifact";
    case SIQSShadowProofRssCampaignJournalStoreObject::session_lock:
        return "session_lock";
    case SIQSShadowProofRssCampaignJournalStoreObject::directory:
        return "directory";
    case SIQSShadowProofRssCampaignJournalStoreObject::journal_header:
        return "journal_header";
    case SIQSShadowProofRssCampaignJournalStoreObject::journal_record:
        return "journal_record";
    case SIQSShadowProofRssCampaignJournalStoreObject::terminal_gate_record:
        return "terminal_gate_record";
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
    SIQSShadowProofRssCampaignArtifactLayoutDiagnostic artifact_layout;
    SIQSShadowProofRssCampaignArtifactConsistencyDiagnostic artifact_consistency;
    std::optional<SIQSShadowProofRssJournalReason> journal_reason;
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
    uint32_t artifact_slot_number = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT;
    std::optional<SIQSShadowProofRssArtifactKind> artifact_kind;
    std::optional<util::durable_immutable_file::PublishStatus> publication_status;
    uint64_t publication_bytes_written = 0;
    std::optional<SIQSShadowProofRssCampaignJournalStoreObject> last_durable_publication_object;
    uint32_t last_durable_publication_record_sequence =
        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
    uint32_t last_durable_artifact_slot_number = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT;
    std::optional<SIQSShadowProofRssArtifactKind> last_durable_artifact_kind;
    uint64_t last_durable_publication_bytes_written = 0;
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
class SlotRunnerFactory;
} // namespace shadow_proof_rss_campaign_journal_store_detail

class SIQSShadowProofRssCampaignJournalSession;
class SIQSShadowProofRssCampaignJournalTaintResult;

/// Move-only in-flight slot authority. It owns both the native session lease
/// and the launch permit, so neither capability can escape the transaction.
/// The current storage slice intentionally exposes no launch operation.
class SIQSShadowProofRssCampaignJournalActiveSlot final {
public:
    SIQSShadowProofRssCampaignJournalActiveSlot() = delete;
    ~SIQSShadowProofRssCampaignJournalActiveSlot();

    SIQSShadowProofRssCampaignJournalActiveSlot(
        SIQSShadowProofRssCampaignJournalActiveSlot&&) noexcept;
    SIQSShadowProofRssCampaignJournalActiveSlot&
    operator=(SIQSShadowProofRssCampaignJournalActiveSlot&&) = delete;

    SIQSShadowProofRssCampaignJournalActiveSlot(
        const SIQSShadowProofRssCampaignJournalActiveSlot&) = delete;
    SIQSShadowProofRssCampaignJournalActiveSlot&
    operator=(const SIQSShadowProofRssCampaignJournalActiveSlot&) = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] uint32_t slot_number() const noexcept;
    [[nodiscard]] SIQSShadowProofRssCampaignJournalSessionView view() const noexcept;

    /// Consume this in-flight slot and durably append its exact prepared taint
    /// record. Destruction alone only releases the lease; it never claims that
    /// the journal reached a durable terminal state.
    [[nodiscard]] SIQSShadowProofRssCampaignJournalTaintResult taint() && noexcept;

private:
    SIQSShadowProofRssCampaignJournalActiveSlot(
        std::unique_ptr<shadow_proof_rss_campaign_journal_store_detail::SessionCore>,
        SIQSShadowProofRssLaunchPermit&&) noexcept;

    std::unique_ptr<shadow_proof_rss_campaign_journal_store_detail::SessionCore> core_;
    std::optional<SIQSShadowProofRssLaunchPermit> permit_;

    friend class SIQSShadowProofRssCampaignJournalSession;
    friend class shadow_proof_rss_campaign_journal_store_detail::SessionFactory;
    friend class shadow_proof_rss_campaign_journal_store_detail::SlotRunnerFactory;
};

class SIQSShadowProofRssCampaignJournalBeginSlotResult final {
public:
    SIQSShadowProofRssCampaignJournalBeginSlotResult() = delete;
    ~SIQSShadowProofRssCampaignJournalBeginSlotResult();

    SIQSShadowProofRssCampaignJournalBeginSlotResult(
        SIQSShadowProofRssCampaignJournalBeginSlotResult&&) noexcept;
    SIQSShadowProofRssCampaignJournalBeginSlotResult&
    operator=(SIQSShadowProofRssCampaignJournalBeginSlotResult&&) = delete;

    SIQSShadowProofRssCampaignJournalBeginSlotResult(
        const SIQSShadowProofRssCampaignJournalBeginSlotResult&) = delete;
    SIQSShadowProofRssCampaignJournalBeginSlotResult&
    operator=(const SIQSShadowProofRssCampaignJournalBeginSlotResult&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
    diagnostic() const noexcept;
    [[nodiscard]] std::optional<SIQSShadowProofRssCampaignJournalActiveSlot>
    take_active_slot() && noexcept;

private:
    explicit SIQSShadowProofRssCampaignJournalBeginSlotResult(
        SIQSShadowProofRssCampaignJournalStoreDiagnostic) noexcept;
    explicit SIQSShadowProofRssCampaignJournalBeginSlotResult(
        SIQSShadowProofRssCampaignJournalActiveSlot&&) noexcept;

    std::optional<SIQSShadowProofRssCampaignJournalActiveSlot> active_slot_;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic_;

    friend class SIQSShadowProofRssCampaignJournalSession;
};

/// Move-only lifetime token for the native root and its cross-process lease.
class SIQSShadowProofRssCampaignJournalSession final {
public:
    SIQSShadowProofRssCampaignJournalSession() = delete;
    ~SIQSShadowProofRssCampaignJournalSession();

    SIQSShadowProofRssCampaignJournalSession(SIQSShadowProofRssCampaignJournalSession&&) noexcept;
    SIQSShadowProofRssCampaignJournalSession&
    operator=(SIQSShadowProofRssCampaignJournalSession&&) = delete;

    SIQSShadowProofRssCampaignJournalSession(const SIQSShadowProofRssCampaignJournalSession&) =
        delete;
    SIQSShadowProofRssCampaignJournalSession&
    operator=(const SIQSShadowProofRssCampaignJournalSession&) = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] SIQSShadowProofRssCampaignJournalSessionView view() const noexcept;

    /// Consume this session, durably publish any pending header and the exact
    /// next slot-start record, then return one lease-bound active slot. The
    /// raw receipt and launch permit never cross the public store boundary.
    [[nodiscard]] SIQSShadowProofRssCampaignJournalBeginSlotResult begin_next_slot() && noexcept;

    /// Consume a reopened dangling-start session and durably append the exact
    /// taint record prepared by replay. No retry or launch authority is issued.
    [[nodiscard]] SIQSShadowProofRssCampaignJournalTaintResult append_pending_taint() && noexcept;

private:
    explicit SIQSShadowProofRssCampaignJournalSession(
        std::unique_ptr<shadow_proof_rss_campaign_journal_store_detail::SessionCore>) noexcept;

    std::unique_ptr<shadow_proof_rss_campaign_journal_store_detail::SessionCore> core_;

    friend class shadow_proof_rss_campaign_journal_store_detail::SessionFactory;
    friend class shadow_proof_rss_campaign_journal_store_detail::SlotRunnerFactory;
};

/// Move-only terminal result for a durable campaign-taint transition.
class SIQSShadowProofRssCampaignJournalTaintResult final {
public:
    SIQSShadowProofRssCampaignJournalTaintResult() = delete;
    ~SIQSShadowProofRssCampaignJournalTaintResult();

    SIQSShadowProofRssCampaignJournalTaintResult(
        SIQSShadowProofRssCampaignJournalTaintResult&&) noexcept;
    SIQSShadowProofRssCampaignJournalTaintResult&
    operator=(SIQSShadowProofRssCampaignJournalTaintResult&&) noexcept;

    SIQSShadowProofRssCampaignJournalTaintResult(
        const SIQSShadowProofRssCampaignJournalTaintResult&) = delete;
    SIQSShadowProofRssCampaignJournalTaintResult&
    operator=(const SIQSShadowProofRssCampaignJournalTaintResult&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] SIQSShadowProofRssCampaignJournalSessionView view() const noexcept;
    [[nodiscard]] const SIQSShadowProofRssCampaignJournalStoreDiagnostic&
    diagnostic() const noexcept;

private:
    SIQSShadowProofRssCampaignJournalTaintResult(
        SIQSShadowProofRssCampaignJournalSessionView,
        SIQSShadowProofRssCampaignJournalStoreDiagnostic) noexcept;

    SIQSShadowProofRssCampaignJournalSessionView view_;
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic_;

    friend class SIQSShadowProofRssCampaignJournalActiveSlot;
    friend class SIQSShadowProofRssCampaignJournalSession;
};

class SIQSShadowProofRssCampaignJournalStoreOpenResult final {
public:
    SIQSShadowProofRssCampaignJournalStoreOpenResult() = delete;
    ~SIQSShadowProofRssCampaignJournalStoreOpenResult();

    SIQSShadowProofRssCampaignJournalStoreOpenResult(
        SIQSShadowProofRssCampaignJournalStoreOpenResult&&) noexcept;
    SIQSShadowProofRssCampaignJournalStoreOpenResult&
    operator=(SIQSShadowProofRssCampaignJournalStoreOpenResult&&) = delete;

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

/// Select the one deployment-registered store named by the caller's policy
/// claim, require exact policy and runtime agreement with that private row,
/// and construct the session from row-owned values. No caller path, descriptor,
/// resolver, snapshot, or publication result crosses this boundary.
[[nodiscard]] SIQSShadowProofRssCampaignJournalStoreOpenResult
open_siqs_shadow_proof_rss_campaign_journal_session(
    const SIQSShadowProofRssGatePolicy* policy,
    const SIQSShadowProofRssCampaignRuntimeFacts* runtime_facts) noexcept;

} // namespace gnfs::siqs
