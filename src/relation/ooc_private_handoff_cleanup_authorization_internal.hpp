#pragma once

// Source-private capability boundary for converting one adopted generic
// handoff into externally authorized cleanup. This file is not installed as
// public API and deliberately provides no production mint function.

#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/sha256.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_resume_detail {

class DistributedSieveExternalCleanupAuthorizationState;
[[nodiscard]] bool distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
    const DistributedSieveExternalCleanupAuthorizationState& state) noexcept;
void distributed_sieve_external_cleanup_authorization_state_release_receipt_claim(
    const DistributedSieveExternalCleanupAuthorizationState& state) noexcept;

} // namespace gnfs::sieve::distributed_sieve_resume_detail

namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail {

class DistributedSieveWorkerCleanupReceiptMintAuthorityV1;

} // namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail

namespace gnfs::relation {
class OOCRelationReader;
}

namespace gnfs::relation::ooc_cleanup_detail {

class OOCPrivateHandoffCleanupAuthorizationTestAuthorityV2;
class OOCPrivateHandoffCleanupIntentConversionExecutorV2;
class OOCPrivateHandoffCleanupIntentReconciliationExecutorV2;
class OOCPrivateHandoffCleanupIntentPublicationTestKeyV2;
class OOCPrivateLeaseRecoveryBorrowedBaseLockV1;
class OOCPrivateHandoffCleanupResumeExecutorV2;
class OOCPrivateHandoffCleanupResumeTestKeyV2;
class OOCPrivateHandoffReadOnlyReleaseExecutorV1;
struct OOCPrivateHandoffCleanupIntentPublicationResultV2;
struct OOCPrivateHandoffCleanupIntentPublicationTestHooksV2;

/// Data-only trusted-test liveness anchor. It carries no production state,
/// mint, cleanup, or claim-release authority; the optional counter observes
/// only the receipt's fixed test-only destructor callback.
struct OOCPrivateHandoffCleanupAuthorizationTestLivenessV2 final {
    std::uint64_t creator_process_id = 0;
    std::shared_ptr<std::atomic_bool> live;
    std::shared_ptr<std::atomic<std::size_t>> release_count;
};

/// Authority-free projection of the exact application and relation bindings
/// that a future wave-store mint must prove. Constructing or copying this value
/// does not grant cleanup, publication, adoption, or namespace authority.
struct OOCPrivateHandoffCleanupAuthorizationBinding final {
    std::filesystem::path base_path;
    util::Sha256Digest external_authorization_digest;
    util::Sha256Digest generic_handoff_self_digest;
    std::array<std::uint64_t, 2> lease_id{};
    util::durable_immutable_record::NativeIdentity parent_directory_identity;
    util::durable_immutable_record::NativeIdentity lock_identity;
    util::durable_immutable_record::NativeIdentity directory_identity;
    util::durable_immutable_record::NativeIdentity owner_marker_identity;
    util::durable_immutable_record::NativeIdentity owned_marker_identity;
    OOCPrivateHandoffPairDescriptorV1 pair;
    OOCPrivateHandoffArtifactBindingV1 handoff;
    OOCPrivateHandoffArtifactBindingV1 index;
    OOCPrivateHandoffArtifactBindingV1 data;

    [[nodiscard]] friend bool
    operator==(const OOCPrivateHandoffCleanupAuthorizationBinding&,
               const OOCPrivateHandoffCleanupAuthorizationBinding&) = default;
};

/// Complete structural state of one exact authorized-cleanup V2 prefix.
///
/// Every enumerator closes the presence/absence of every reserved marker,
/// handoff, live artifact, and quarantined artifact covered by the witness.
enum class OOCPrivateHandoffCleanupPrefixStateV2 : std::uint8_t {
    LiveUnconverted,
    IntentPendingOnly,
    IntentCanonicalAndPending,
    IntentCanonicalWithHandoff,
    IntentCanonicalWithLivePair,
    IntentCanonicalWithQuarantinedIndex,
    IntentCanonicalWithQuarantinedPair,
    IntentCanonicalWithStagedPending,
    IntentCanonicalWithStagedAndPending,
    IntentCanonicalWithStagedPair,
    IntentCanonicalWithStagedIndex,
    IntentCanonicalWithStagedOnly,
    StagedWithOwner,
    StagedOnly,
    EmptyPrivateDirectory,
    OwnedOnly,
    Absent,
    Count,
};

/// Exact immutable bytes and native snapshot of one validated control leaf.
struct OOCPrivateHandoffCleanupPrefixLeafWitnessV2 final {
    std::vector<std::byte> bytes;
    util::durable_immutable_record::RecordSnapshot snapshot;

    [[nodiscard]] friend bool
    operator==(const OOCPrivateHandoffCleanupPrefixLeafWitnessV2&,
               const OOCPrivateHandoffCleanupPrefixLeafWitnessV2&) = default;
};

/// Authority-free, comparable observation of one exact V2 cleanup prefix.
///
/// This is pure data: it owns no descriptor, handle, receipt, action claim, or
/// callable path operation. `Absent` is emitted only after the strict
/// classifier proves the private directory and OWNED leaf absent while the
/// exact named parent and non-creating BaseLock binding remain stable. The
/// retained binding carries the historical deleted generations needed for a
/// later independent comparison.
struct OOCPrivateHandoffCleanupPrefixWitnessV2 final {
    OOCPrivateHandoffCleanupPrefixStateV2 state = OOCPrivateHandoffCleanupPrefixStateV2::Count;
    OOCPrivateHandoffCleanupAuthorizationBinding binding;
    util::durable_immutable_record::NativeIdentity parent_directory_identity;
    util::durable_immutable_record::NativeIdentity base_lock_identity;
    std::optional<util::durable_immutable_record::NativeIdentity> private_directory_identity;
    std::optional<OOCPrivateHandoffCleanupPrefixLeafWitnessV2> intent;
    std::optional<OOCPrivateHandoffCleanupPrefixLeafWitnessV2> intent_pending;
    std::optional<OOCPrivateHandoffCleanupPrefixLeafWitnessV2> staged;
    std::optional<OOCPrivateHandoffCleanupPrefixLeafWitnessV2> staged_pending;
    std::optional<OOCPrivateHandoffCleanupPrefixLeafWitnessV2> handoff;
    std::optional<OOCPrivateHandoffCleanupPrefixLeafWitnessV2> owner;
    std::optional<OOCPrivateHandoffCleanupPrefixLeafWitnessV2> owned;
    std::optional<util::durable_immutable_record::RecordSnapshot> index;
    std::optional<util::durable_immutable_record::RecordSnapshot> data;
    std::optional<util::durable_immutable_record::RecordSnapshot> quarantine_index;
    std::optional<util::durable_immutable_record::RecordSnapshot> quarantine_data;

    [[nodiscard]] friend bool operator==(const OOCPrivateHandoffCleanupPrefixWitnessV2&,
                                         const OOCPrivateHandoffCleanupPrefixWitnessV2&) = default;
};

/// Non-throwing result for an exact, read-only cleanup-prefix observation.
struct OOCPrivateHandoffCleanupPrefixObservationResultV2 final {
    OOCCleanupResult result;
    std::optional<OOCPrivateHandoffCleanupPrefixWitnessV2> witness;

    [[nodiscard]] bool observed() const noexcept {
        return result.status == OOCCleanupStatus::Completed && witness.has_value();
    }

    [[nodiscard]] friend bool
    operator==(const OOCPrivateHandoffCleanupPrefixObservationResultV2& lhs,
               const OOCPrivateHandoffCleanupPrefixObservationResultV2& rhs) noexcept {
        return lhs.result.status == rhs.result.status && lhs.result.stage == rhs.result.stage &&
               lhs.result.native_error == rhs.result.native_error && lhs.witness == rhs.witness;
    }
};

/// Observe one exact V2 prefix under a new non-creating BaseLock epoch. This
/// function performs no create, rename, delete, durability sync, or authority
/// mint and never turns the returned data into cleanup authority.
[[nodiscard]] OOCPrivateHandoffCleanupPrefixObservationResultV2
observe_authorized_private_handoff_cleanup_prefix_v2(
    const OOCPrivateHandoffCleanupAuthorizationBinding& binding) noexcept;

/// Observe under the caller's already-held BaseLock open-file description.
/// The one-shot carrier is duplicated and adopted close-only: no competing
/// open/flock is issued and returning the witness cannot release the caller's
/// remaining descriptor or lock epoch.
[[nodiscard]] OOCPrivateHandoffCleanupPrefixObservationResultV2
observe_authorized_private_handoff_cleanup_prefix_v2(
    const OOCPrivateHandoffCleanupAuthorizationBinding& binding,
    OOCPrivateLeaseRecoveryBorrowedBaseLockV1&& borrowed) noexcept;

/// Unforgeable constructor token. Only the source-private worker-cleanup mint
/// authority may create one after it holds the root claim and confirms the
/// canonical external authorization record.
class OOCPrivateHandoffCleanupAuthorizationMintKey final {
public:
    OOCPrivateHandoffCleanupAuthorizationMintKey(
        const OOCPrivateHandoffCleanupAuthorizationMintKey&) = delete;
    OOCPrivateHandoffCleanupAuthorizationMintKey&
    operator=(const OOCPrivateHandoffCleanupAuthorizationMintKey&) = delete;
    OOCPrivateHandoffCleanupAuthorizationMintKey(OOCPrivateHandoffCleanupAuthorizationMintKey&&) =
        delete;
    OOCPrivateHandoffCleanupAuthorizationMintKey&
    operator=(OOCPrivateHandoffCleanupAuthorizationMintKey&&) = delete;
    ~OOCPrivateHandoffCleanupAuthorizationMintKey() = default;

private:
    OOCPrivateHandoffCleanupAuthorizationMintKey() noexcept = default;

    friend class gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail::
        DistributedSieveWorkerCleanupReceiptMintAuthorityV1;
};

/// Current-process application authority for one exact generic handoff.
///
/// The opaque lifetime must retain the claimed cleanup root and canonical
/// external-record binding. Exactly one live receipt owns the type-erased
/// release callback; moves transfer that responsibility and destruction
/// releases it. The value remains unusable without a separately acquired
/// matching adoption receipt. Neither this type nor its pure binding exposes a
/// public factory, path accessor, native handle, or namespace operation.
class OOCPrivateHandoffCleanupAuthorizationReceipt final {
public:
    OOCPrivateHandoffCleanupAuthorizationReceipt() = delete;
    OOCPrivateHandoffCleanupAuthorizationReceipt(
        const OOCPrivateHandoffCleanupAuthorizationReceipt&) = delete;
    OOCPrivateHandoffCleanupAuthorizationReceipt&
    operator=(const OOCPrivateHandoffCleanupAuthorizationReceipt&) = delete;

    OOCPrivateHandoffCleanupAuthorizationReceipt(
        OOCPrivateHandoffCleanupAuthorizationReceipt&& other) noexcept
        : binding_(std::move(other.binding_)), live_authority_(std::move(other.live_authority_)),
          validate_live_authority_(std::exchange(other.validate_live_authority_, nullptr)),
          release_live_authority_(std::exchange(other.release_live_authority_, nullptr)),
          spent_(std::exchange(other.spent_, true)) {}

    OOCPrivateHandoffCleanupAuthorizationReceipt&
    operator=(OOCPrivateHandoffCleanupAuthorizationReceipt&&) = delete;
    ~OOCPrivateHandoffCleanupAuthorizationReceipt() noexcept {
        release_live_authority();
    }

    [[nodiscard]] bool spent() const noexcept {
        return spent_ || !live_authority_valid();
    }

private:
    using ValidateLiveAuthority = bool (*)(const void* authority) noexcept;
    using ReleaseLiveAuthority = void (*)(const void* authority) noexcept;

    OOCPrivateHandoffCleanupAuthorizationReceipt(
        OOCPrivateHandoffCleanupAuthorizationMintKey&&,
        OOCPrivateHandoffCleanupAuthorizationBinding binding,
        std::shared_ptr<const gnfs::sieve::distributed_sieve_resume_detail::
                            DistributedSieveExternalCleanupAuthorizationState>
            live_wave_authority) noexcept
        : binding_(std::move(binding)), live_authority_(std::move(live_wave_authority)),
          validate_live_authority_([](const void* authority) noexcept {
              if (authority == nullptr) {
                  return false;
              }
              return gnfs::sieve::distributed_sieve_resume_detail::
                  distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
                      *static_cast<const gnfs::sieve::distributed_sieve_resume_detail::
                                       DistributedSieveExternalCleanupAuthorizationState*>(
                          authority));
          }),
          release_live_authority_([](const void* authority) noexcept {
              if (authority == nullptr) {
                  return;
              }
              gnfs::sieve::distributed_sieve_resume_detail::
                  distributed_sieve_external_cleanup_authorization_state_release_receipt_claim(
                      *static_cast<const gnfs::sieve::distributed_sieve_resume_detail::
                                       DistributedSieveExternalCleanupAuthorizationState*>(
                          authority));
          }) {}

    OOCPrivateHandoffCleanupAuthorizationReceipt(
        OOCPrivateHandoffCleanupAuthorizationBinding binding,
        std::shared_ptr<const OOCPrivateHandoffCleanupAuthorizationTestLivenessV2>
            live_authority) noexcept
        : binding_(std::move(binding)), live_authority_(std::move(live_authority)),
          validate_live_authority_([](const void* authority) noexcept {
              const auto* state =
                  static_cast<const OOCPrivateHandoffCleanupAuthorizationTestLivenessV2*>(
                      authority);
              return state != nullptr && state->creator_process_id != 0 && state->live &&
                     state->creator_process_id ==
                         static_cast<std::uint64_t>(gnfs::util::process_id()) &&
                     state->live->load(std::memory_order_acquire);
          }),
          release_live_authority_([](const void* authority) noexcept {
              const auto* state =
                  static_cast<const OOCPrivateHandoffCleanupAuthorizationTestLivenessV2*>(
                      authority);
              if (state == nullptr) {
                  return;
              }
              if (state->live) {
                  state->live->store(false, std::memory_order_release);
              }
              if (state->release_count) {
                  state->release_count->fetch_add(1, std::memory_order_acq_rel);
              }
          }) {}

    void commit_spend() noexcept {
        spent_ = true;
    }

    [[nodiscard]] bool live_authority_valid() const noexcept {
        return live_authority_ && validate_live_authority_ != nullptr &&
               validate_live_authority_(live_authority_.get());
    }

    void release_live_authority() noexcept {
        const auto release = std::exchange(release_live_authority_, nullptr);
        if (release != nullptr && live_authority_) {
            release(live_authority_.get());
        }
    }

    OOCPrivateHandoffCleanupAuthorizationBinding binding_;
    std::shared_ptr<const void> live_authority_;
    ValidateLiveAuthority validate_live_authority_ = nullptr;
    ReleaseLiveAuthority release_live_authority_ = nullptr;
    bool spent_ = false;

    friend class gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail::
        DistributedSieveWorkerCleanupReceiptMintAuthorityV1;
    friend class OOCPrivateHandoffCleanupAuthorizationTestAuthorityV2;
    friend class OOCPrivateHandoffCleanupIntentConversionExecutorV2;
    friend class OOCPrivateHandoffCleanupIntentReconciliationExecutorV2;
    friend class OOCPrivateHandoffCleanupResumeExecutorV2;
};

/// Unforgeable token preventing production/internal callers from arming the
/// partial-publication fault seam.
class OOCPrivateHandoffCleanupIntentPublicationTestKeyV2 final {
public:
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2(
        const OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&) = delete;
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&
    operator=(const OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&) = delete;
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2(
        OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&&) = delete;
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&
    operator=(OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&&) = delete;

private:
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2() noexcept = default;

    friend class OOCPrivateHandoffCleanupAuthorizationTestAuthorityV2;
};

/// Unforgeable token preventing production/internal callers from arming the
/// destructive T2b crash seam.
class OOCPrivateHandoffCleanupResumeTestKeyV2 final {
public:
    OOCPrivateHandoffCleanupResumeTestKeyV2(const OOCPrivateHandoffCleanupResumeTestKeyV2&) =
        delete;
    OOCPrivateHandoffCleanupResumeTestKeyV2&
    operator=(const OOCPrivateHandoffCleanupResumeTestKeyV2&) = delete;
    OOCPrivateHandoffCleanupResumeTestKeyV2(OOCPrivateHandoffCleanupResumeTestKeyV2&&) = delete;
    OOCPrivateHandoffCleanupResumeTestKeyV2&
    operator=(OOCPrivateHandoffCleanupResumeTestKeyV2&&) = delete;

private:
    OOCPrivateHandoffCleanupResumeTestKeyV2() noexcept = default;

    friend class OOCPrivateHandoffCleanupAuthorizationTestAuthorityV2;
};

enum class OOCPrivateHandoffCleanupIntentPublicationFaultPointV2 : std::uint8_t {
    ReaderViewsClosed,
    BindingRevalidated,
    IntentPendingDurable,
    IntentCanonicalPromoted,
    IntentCanonicalDurable,
    CanonicalBindingRevalidated,
    Count,
};

struct OOCPrivateHandoffCleanupIntentPublicationTestHooksV2 final {
    using StopAfter = bool (*)(OOCPrivateHandoffCleanupIntentPublicationFaultPointV2 point,
                               void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

enum class OOCPrivateHandoffCleanupIntentPublicationDispositionV2 : std::uint8_t {
    Failed,
    CapabilitiesRetained,
    CanonicalReconciliationRequired,
    IntentPublished,
};

/// Durable evidence for the T2a authority-conversion boundary. This proves
/// only canonical intent publication. It deliberately does not claim artifact,
/// handoff, lease, or namespace cleanup completion.
struct OOCPrivateHandoffCleanupIntentPublicationEvidenceV2 final {
    util::Sha256Digest external_authorization_digest;
    util::durable_immutable_record::RecordSnapshot intent_snapshot;
    util::durable_immutable_record::NativeIdentity parent_directory_identity;
};

struct OOCPrivateHandoffCleanupIntentPublicationResultV2 final {
    OOCCleanupResult result;
    OOCPrivateHandoffCleanupIntentPublicationDispositionV2 disposition =
        OOCPrivateHandoffCleanupIntentPublicationDispositionV2::Failed;
    std::optional<OOCPrivateHandoffCleanupIntentPublicationEvidenceV2> evidence;

    [[nodiscard]] bool intent_published() const noexcept {
        return disposition ==
                   OOCPrivateHandoffCleanupIntentPublicationDispositionV2::IntentPublished &&
               evidence.has_value();
    }

    [[nodiscard]] bool capabilities_retained() const noexcept {
        return disposition ==
               OOCPrivateHandoffCleanupIntentPublicationDispositionV2::CapabilitiesRetained;
    }

    [[nodiscard]] bool canonical_reconciliation_required() const noexcept {
        return disposition == OOCPrivateHandoffCleanupIntentPublicationDispositionV2::
                                  CanonicalReconciliationRequired;
    }

    [[nodiscard]] bool capabilities_spent() const noexcept {
        return intent_published() || canonical_reconciliation_required();
    }
};

/// Convert the exact same-handle reader and one matching application receipt
/// into canonical V2 cleanup intent. Once canonical promotion may be visible,
/// both live capabilities are spent and any uncertain durability prefix is
/// reconciliation-only. This T2a seam performs no artifact or lease deletion.
[[nodiscard]] OOCPrivateHandoffCleanupIntentPublicationResultV2
convert_authorized_private_handoff_to_cleanup_intent_v2(
    OOCPrivateHandoffReader&& reader,
    OOCPrivateHandoffCleanupAuthorizationReceipt&& authorization) noexcept;

/// Same conversion with deterministic partial-publication stops. Only the
/// exact trusted-test friend can construct the first argument.
[[nodiscard]] OOCPrivateHandoffCleanupIntentPublicationResultV2
convert_authorized_private_handoff_to_cleanup_intent_v2_for_trusted_test(
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&&, OOCPrivateHandoffReader&& reader,
    OOCPrivateHandoffCleanupAuthorizationReceipt&& authorization,
    OOCPrivateHandoffCleanupIntentPublicationTestHooksV2 hooks) noexcept;

enum class OOCPrivateHandoffCleanupIntentReconciliationFaultPointV2 : std::uint8_t {
    PendingReconfirmedDurable,
    CanonicalPromotionReady,
    CanonicalPromoted,
    CanonicalDurable,
    CanonicalBindingRevalidated,
    Count,
};

struct OOCPrivateHandoffCleanupIntentReconciliationTestHooksV2 final {
    using StopAfter = bool (*)(OOCPrivateHandoffCleanupIntentReconciliationFaultPointV2 point,
                               void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

enum class OOCPrivateHandoffCleanupIntentReconciliationDispositionV2 : std::uint8_t {
    Failed,
    AuthorizationRetained,
    ReconciliationRequired,
    IntentCanonical,
};

struct OOCPrivateHandoffCleanupIntentReconciliationResultV2 final {
    OOCCleanupResult result;
    OOCPrivateHandoffCleanupIntentReconciliationDispositionV2 disposition =
        OOCPrivateHandoffCleanupIntentReconciliationDispositionV2::Failed;
    std::optional<OOCPrivateHandoffCleanupIntentPublicationEvidenceV2> evidence;

    [[nodiscard]] bool intent_canonical() const noexcept {
        return disposition ==
                   OOCPrivateHandoffCleanupIntentReconciliationDispositionV2::IntentCanonical &&
               evidence.has_value();
    }

    [[nodiscard]] bool authorization_retained() const noexcept {
        return disposition ==
               OOCPrivateHandoffCleanupIntentReconciliationDispositionV2::AuthorizationRetained;
    }

    [[nodiscard]] bool reconciliation_required() const noexcept {
        return disposition ==
               OOCPrivateHandoffCleanupIntentReconciliationDispositionV2::ReconciliationRequired;
    }
};

/// Recovery-only T2a normalization. This opens one new non-creating BaseLock
/// epoch and may only promote the exact already-existing pending V2 intent to
/// its canonical name. It accepts no path or payload, never creates pending,
/// and performs no artifact, marker, lease, or directory deletion.
[[nodiscard]] OOCPrivateHandoffCleanupIntentReconciliationResultV2
reconcile_authorized_private_handoff_cleanup_intent_v2(
    OOCPrivateHandoffCleanupAuthorizationReceipt&& authorization) noexcept;

/// Same recovery-only transition with deterministic crash boundaries. The
/// ordinary intent-publication test key also gates this closely related seam.
[[nodiscard]] OOCPrivateHandoffCleanupIntentReconciliationResultV2
reconcile_authorized_private_handoff_cleanup_intent_v2_for_trusted_test(
    OOCPrivateHandoffCleanupIntentPublicationTestKeyV2&&,
    OOCPrivateHandoffCleanupAuthorizationReceipt&& authorization,
    OOCPrivateHandoffCleanupIntentReconciliationTestHooksV2 hooks) noexcept;

/// Source-private authority erasure for a same-handle adopted reader. The
/// returned reader owns only the already-open read handles; all adoption,
/// action-claim, directory, parent, and BaseLock authority is released.
[[nodiscard]] OOCRelationReader
take_read_only_reader_and_release_adoption_authority(OOCPrivateHandoffReader&& reader);

enum class OOCPrivateHandoffCleanupResumeFaultPointV2 : std::uint8_t {
    RecoveryPermitAcquired,
    IntentCanonicalReconfirmedDurable,
    IntentDuplicatePendingRemovedDurable,
    HandoffRemovedDurable,
    IndexQuarantinedDurable,
    PairQuarantinedDurable,
    StagedPendingDurable,
    StagedCanonicalPromoted,
    StagedCanonicalDurable,
    StagedDuplicatePendingRemovedDurable,
    DataRemovedDurable,
    IndexRemovedDurable,
    IntentRemovedDurable,
    OwnerRemovedDurable,
    StagedRemovedDurable,
    PrivateDirectoryRemovedDurable,
    OwnedRemovedDurable,
    ParentAbsenceDurable,
    AbsenceEvidenceReady,
    Count,
};

struct OOCPrivateHandoffCleanupResumeTestHooksV2 final {
    using StopAfter = bool (*)(OOCPrivateHandoffCleanupResumeFaultPointV2 point,
                               void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

enum class OOCPrivateHandoffCleanupResumeDispositionV2 : std::uint8_t {
    Failed,
    AuthorizationRetained,
    IntentConversionRequired,
    ReconciliationRequired,
    NamespaceAbsent,
};

/// Parent-durable proof returned only after the exact authorized private
/// namespace is absent. A process that observed the canonical intent retains
/// its exact snapshot. Cold staged-only or markerless recovery cannot recreate
/// the deleted inode and deliberately returns `cleanup_intent_snapshot == nullopt`.
struct OOCPrivateHandoffCleanupAbsenceEvidenceV2 final {
    util::Sha256Digest base_path_digest;
    util::Sha256Digest external_authorization_digest;
    std::array<std::uint64_t, 2> lease_id{};
    std::optional<util::durable_immutable_record::RecordSnapshot> cleanup_intent_snapshot;
    util::durable_immutable_record::NativeIdentity parent_directory_identity;
    bool parent_directory_durability_confirmed = false;
    bool expected_namespace_absent = false;
};

struct OOCPrivateHandoffCleanupResumeResultV2 final {
    OOCCleanupResult result;
    OOCPrivateHandoffCleanupResumeDispositionV2 disposition =
        OOCPrivateHandoffCleanupResumeDispositionV2::Failed;
    std::optional<OOCPrivateHandoffCleanupAbsenceEvidenceV2> evidence;

    [[nodiscard]] bool namespace_absent() const noexcept {
        return disposition == OOCPrivateHandoffCleanupResumeDispositionV2::NamespaceAbsent &&
               evidence.has_value() && evidence->parent_directory_durability_confirmed &&
               evidence->expected_namespace_absent;
    }

    [[nodiscard]] bool authorization_retained() const noexcept {
        return disposition == OOCPrivateHandoffCleanupResumeDispositionV2::AuthorizationRetained ||
               disposition == OOCPrivateHandoffCleanupResumeDispositionV2::IntentConversionRequired;
    }

    [[nodiscard]] bool reconciliation_required() const noexcept {
        return disposition == OOCPrivateHandoffCleanupResumeDispositionV2::ReconciliationRequired;
    }
};

/// Cold-resume one exact V2 cleanup generation. The executor opens one fresh,
/// independent non-creating BaseLock and retains it for the complete call; it
/// never accepts a borrowed/duplicated lock wrapper or reopens the lock. A
/// pending-only intent remains a T2a conversion prefix and grants no deletion
/// authority. Positive filesystem mutation is currently macOS-only.
[[nodiscard]] OOCPrivateHandoffCleanupResumeResultV2 resume_authorized_private_handoff_cleanup_v2(
    OOCPrivateHandoffCleanupAuthorizationReceipt&& authorization) noexcept;

/// Same cold resume with deterministic crash boundaries. Only the trusted-test
/// friend can construct the first argument.
[[nodiscard]] OOCPrivateHandoffCleanupResumeResultV2
resume_authorized_private_handoff_cleanup_v2_for_trusted_test(
    OOCPrivateHandoffCleanupResumeTestKeyV2&&,
    OOCPrivateHandoffCleanupAuthorizationReceipt&& authorization,
    OOCPrivateHandoffCleanupResumeTestHooksV2 hooks) noexcept;

} // namespace gnfs::relation::ooc_cleanup_detail
