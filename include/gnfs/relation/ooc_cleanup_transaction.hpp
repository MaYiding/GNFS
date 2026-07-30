#pragma once

#include "gnfs/relation/ooc_durable_handoff.hpp"
#include "gnfs/relation/ooc_relation_format.hpp"
#include "gnfs/util/durable_immutable_file.hpp"
#include "gnfs/util/durable_immutable_record.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/sha256.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace gnfs::sieve::distributed_sieve_worker_entry_detail::
    distributed_sieve_worker_writer_detail {
class OOCInheritedP8WriterMintV1;
}

namespace gnfs::relation {

class OOCRelationWriter;
class OOCPrivateHandoffReader;
class OOCCleanupTransaction;
struct OOCCleanupPaths;
namespace ooc_cleanup_detail {
class AdoptionParentDirectoryHandle;
class BaseLock;
class OOCPrivateHandoffAdoptionBuilderV1;
class OOCPrivateHandoffBorrowedBaseLockV1;
class PrivateCleanupActionPermit;
class PrivateCleanupMutationGate;
class PrivateDirectoryHandle;
class PathPrivateLeaseReservationTarget;
enum class PrivateCleanupMutationBoundary : std::uint8_t {
    Generic,
    PendingPreparation,
    PendingRemoval,
    CanonicalRename,
    PublicationComplete,
};
void authorize_private_cleanup_mutation(
    PrivateCleanupMutationGate& gate, const OOCCleanupPaths& paths, const BaseLock& lock,
    PrivateCleanupMutationBoundary boundary = PrivateCleanupMutationBoundary::Generic);
} // namespace ooc_cleanup_detail

/// Last durable cleanup boundary reached by an OOC cleanup transaction.
enum class OOCCleanupStage : std::uint8_t {
    None,
    IntentDurable,
    IndexQuarantined,
    PairQuarantined,
    DeleteAuthorized,
    DataRemoved,
    IndexRemoved,
    IntentRemoved,
    Completed,
};

enum class OOCCleanupStatus : std::uint8_t {
    Completed,
    NoTransaction,
    Interrupted,
    InvalidRequest,
    SourcePairInvalid,
    IntentCorrupt,
    IntentConflict,
    ForeignReplacementPreserved,
    NamespaceConflict,
    RecoveryRequired,
    HandoffPresent,
    Busy,
    IoFailure,
    DurabilityFailure,
    PlatformUnsupported,
    UnexpectedFailure,
};

struct OOCCleanupResult final {
    OOCCleanupStatus status = OOCCleanupStatus::UnexpectedFailure;
    OOCCleanupStage stage = OOCCleanupStage::None;
    std::error_code native_error;

    [[nodiscard]] bool completed() const noexcept {
        return status == OOCCleanupStatus::Completed;
    }

    /// No further canonical cleanup transaction is currently actionable.
    /// This does not prove the pair namespace is empty: NoTransaction is also
    /// returned when unrelated live artifacts exist without an intent.
    [[nodiscard]] bool transaction_terminal() const noexcept {
        return status == OOCCleanupStatus::Completed || status == OOCCleanupStatus::NoTransaction;
    }

    [[nodiscard]] bool retryable() const noexcept {
        return status == OOCCleanupStatus::Interrupted || status == OOCCleanupStatus::Busy ||
               status == OOCCleanupStatus::RecoveryRequired ||
               status == OOCCleanupStatus::IoFailure ||
               status == OOCCleanupStatus::DurabilityFailure;
    }
};

struct OOCExactCleanupExpectation final {
    std::uint64_t index_magic = 0;
    std::uint64_t persisted_count = 0;
    std::uint64_t index_size = 0;
    std::uint64_t data_size = 0;

    friend bool operator==(const OOCExactCleanupExpectation&,
                           const OOCExactCleanupExpectation&) = default;
};

/// Move-only authority to start cleanup for one pair created by a trusted
/// owner. The stable native identities are captured at ownership acquisition;
/// store_id correlates the two V3 headers but never grants authority alone.
///
/// No public path/store-id factory exists. Only a fresh writer issues this
/// receipt while it still owns the exact O_EXCL-created pair. Structural
/// recovery descriptors and sequence digests never recreate it. Once a
/// canonical durable intent exists, begin_or_resume() consumes the receipt and
/// later processes resume from that intent alone.
class OOCCleanupOwnershipReceipt final {
public:
    OOCCleanupOwnershipReceipt(const OOCCleanupOwnershipReceipt&) = delete;
    OOCCleanupOwnershipReceipt& operator=(const OOCCleanupOwnershipReceipt&) = delete;

    OOCCleanupOwnershipReceipt(OOCCleanupOwnershipReceipt&& other) noexcept
        : base_path_(std::move(other.base_path_)), store_id_(other.store_id_),
          index_identity_(other.index_identity_), data_identity_(other.data_identity_),
          spent_(other.spent_) {
        other.store_id_ = 0;
        other.index_identity_ = {};
        other.data_identity_ = {};
        other.spent_ = true;
    }

    OOCCleanupOwnershipReceipt& operator=(OOCCleanupOwnershipReceipt&&) = delete;

    [[nodiscard]] bool spent() const noexcept {
        return spent_;
    }

private:
    struct NativeIdentity final {
        std::uint64_t first = 0;
        std::uint64_t second = 0;
        std::uint64_t third = 0;

        friend bool operator==(const NativeIdentity&, const NativeIdentity&) = default;
    };

    OOCCleanupOwnershipReceipt(std::filesystem::path base_path, std::uint64_t store_id,
                               NativeIdentity index_identity, NativeIdentity data_identity) noexcept
        : base_path_(std::move(base_path)), store_id_(store_id), index_identity_(index_identity),
          data_identity_(data_identity) {}

    std::filesystem::path base_path_;
    std::uint64_t store_id_ = 0;
    NativeIdentity index_identity_;
    NativeIdentity data_identity_;
    bool spent_ = false;

    friend class OOCRelationWriter;
    friend class OOCCleanupTransaction;
};

/// Move-only authority to remove one RelationSink lease directory created
/// under the matching persistent external lock. The receipt alone never
/// authorizes pair deletion. Before activation, its exact durable
/// RESERVED/OWNED chain may roll back only the fresh pair inside that owned
/// directory; after RESERVED is consumed, pair cleanup again requires the
/// separate fresh-writer receipt above.
class OOCPrivateLeaseOwnershipReceipt final {
public:
    OOCPrivateLeaseOwnershipReceipt(const OOCPrivateLeaseOwnershipReceipt&) = delete;
    OOCPrivateLeaseOwnershipReceipt& operator=(const OOCPrivateLeaseOwnershipReceipt&) = delete;

    OOCPrivateLeaseOwnershipReceipt(OOCPrivateLeaseOwnershipReceipt&& other) noexcept
        : base_path_(std::move(other.base_path_)),
          private_directory_(std::move(other.private_directory_)),
          lock_path_(std::move(other.lock_path_)), directory_identity_(other.directory_identity_),
          lease_id_(other.lease_id_), owner_identity_(other.owner_identity_),
          owned_identity_(other.owned_identity_), live_lock_(std::move(other.live_lock_)),
          owner_process_id_(other.owner_process_id_), active_(other.active_), spent_(other.spent_) {
        other.base_path_.clear();
        other.private_directory_.clear();
        other.lock_path_.clear();
        other.directory_identity_ = {};
        other.lease_id_ = {};
        other.owner_identity_ = {};
        other.owned_identity_ = {};
        other.owner_process_id_ = 0;
        other.active_ = false;
        other.spent_ = true;
    }

    OOCPrivateLeaseOwnershipReceipt& operator=(OOCPrivateLeaseOwnershipReceipt&&) = delete;

    [[nodiscard]] bool spent() const noexcept {
        return spent_;
    }

    [[nodiscard]] const std::filesystem::path& base_path() const noexcept {
        return base_path_;
    }

    [[nodiscard]] const std::filesystem::path& private_directory() const noexcept {
        return private_directory_;
    }

private:
    OOCPrivateLeaseOwnershipReceipt(std::filesystem::path base_path,
                                    std::filesystem::path private_directory,
                                    std::filesystem::path lock_path,
                                    std::array<std::uint64_t, 3> directory_identity,
                                    std::array<std::uint64_t, 2> lease_id,
                                    std::array<std::uint64_t, 3> owner_identity,
                                    std::array<std::uint64_t, 3> owned_identity,
                                    std::shared_ptr<ooc_cleanup_detail::BaseLock> live_lock,
                                    std::uint64_t owner_process_id) noexcept
        : base_path_(std::move(base_path)), private_directory_(std::move(private_directory)),
          lock_path_(std::move(lock_path)), directory_identity_(directory_identity),
          lease_id_(lease_id), owner_identity_(owner_identity), owned_identity_(owned_identity),
          live_lock_(std::move(live_lock)), owner_process_id_(owner_process_id) {}

    std::filesystem::path base_path_;
    std::filesystem::path private_directory_;
    std::filesystem::path lock_path_;
    std::array<std::uint64_t, 3> directory_identity_{};
    std::array<std::uint64_t, 2> lease_id_{};
    std::array<std::uint64_t, 3> owner_identity_{};
    std::array<std::uint64_t, 3> owned_identity_{};
    std::shared_ptr<ooc_cleanup_detail::BaseLock> live_lock_;
    std::uint64_t owner_process_id_ = 0;
    bool active_ = false;
    bool spent_ = false;

    friend class OOCRelationWriter;
    friend class OOCCleanupTransaction;
    friend class ::gnfs::sieve::distributed_sieve_worker_entry_detail::
        distributed_sieve_worker_writer_detail::OOCInheritedP8WriterMintV1;
};

struct OOCPrivateLeaseReservation final {
    OOCCleanupResult result;
    std::optional<OOCPrivateLeaseOwnershipReceipt> ownership;

    [[nodiscard]] bool completed() const noexcept {
        return result.completed() && ownership.has_value() && !ownership->spent();
    }
};

/// Optional narrowing expectation for resume(). It cannot authorize a new
/// intent; begin_or_resume() derives path and store identity from a receipt.
struct OOCCleanupRequest final {
    std::filesystem::path base_path;
    std::uint64_t store_id = 0;
    std::optional<OOCExactCleanupExpectation> exact;
};

/// Trusted test-only interruption boundaries. Durable points are emitted only
/// after the corresponding namespace mutation and parent-directory sync.
enum class OOCCleanupFaultPoint : std::uint8_t {
    IntentDurable,
    FirstRenameDurable,
    SecondRenameDurable,
    DeleteAuthorizedDurable,
    FirstUnlinkDurable,
    SecondUnlinkDurable,
    IntentRemovedDurable,
    /// RunLegacyCleanup has retained its action permit, but has not observed C1
    /// or mutated the cleanup transaction namespace.
    LegacyCleanupPermitAcquired,
    /// Cleanup-handoff publication has retained its post-finalize action permit,
    /// but has not consumed C1 or mutated the cleanup transaction namespace.
    PrivateLeaseCleanupHandoffPermitAcquired,
};

/// Trusted test-only boundaries for the no-authority pending publication
/// protocol. The existing OOCCleanupFaultPoint values remain the authoritative
/// post-canonical boundaries.
enum class OOCCleanupPublishFaultPoint : std::uint8_t {
    IntentPendingDurable,
    StagedPendingDurable,
};

/// Trusted test-only failures injected immediately before a namespace
/// operation or its following durability barrier.
enum class OOCCleanupTestOperation : std::uint8_t {
    IndexRename,
    IndexRenameParentSync,
    DataRename,
    DataRenameParentSync,
    DataUnlink,
    DataUnlinkParentSync,
    IndexUnlink,
    IndexUnlinkParentSync,
    IntentUnlink,
    IntentUnlinkParentSync,
    StagedUnlink,
    StagedUnlinkParentSync,
    MarkerRename,
    MarkerPendingUnlink,
    MarkerPendingUnlinkParentSync,
    MarkerRenameAuthorized,
};

struct OOCCleanupTestHooks final {
    using StopAfter = bool (*)(OOCCleanupFaultPoint point, void* context) noexcept;
    using StopAfterPublish = bool (*)(OOCCleanupPublishFaultPoint point, void* context) noexcept;
    using FailBeforeOperation = bool (*)(OOCCleanupTestOperation operation, void* context) noexcept;

    StopAfter stop_after = nullptr;
    StopAfterPublish stop_after_publish = nullptr;
    FailBeforeOperation fail_before_operation = nullptr;
    void* context = nullptr;
};

/// Trusted test-only boundaries for the private lease protocol. Values with a
/// `Durable` suffix are emitted only after the corresponding namespace
/// mutation and durability barrier. Fresh-writer reservation values cover
/// volatile process-crash windows before lease activation.
enum class OOCPrivateLeaseFaultPoint : std::uint8_t {
    ReservedPendingDurable,
    ReservedDurable,
    StagingDirectoryDurable,
    OwnerPendingDurable,
    OwnerDurable,
    OwnedPendingDurable,
    OwnedDurable,
    FinalRenameDurable,
    FreshIndexReserved,
    FreshDataReserved,
    /// The private writer permit and both held-name identities have passed the
    /// final pre-write authorization gate, but no buffered I/O handle has been
    /// attached yet.
    FreshHeaderWriteAuthorized,
    /// Both buffered I/O files are attached to duplicates of the held O_EXCL
    /// handles, but no protocol header byte has been written yet.
    FreshStreamsAttached,
    FreshHeadersValidated,
    FreshPairOwnershipCaptured,
    PreactiveDirectoryQuarantinedDurable,
    PreactiveDataRemovedDurable,
    PreactiveIndexRemovedDurable,
    ReservedRemovedDurable,
    OwnerRemovedDurable,
    FinalDirectoryRemovedDurable,
    OwnedRemovedDurable,
    /// Permit is fully acquired and retained, but no recovery mutation has
    /// started. A true test callback interrupts without consuming namespace
    /// state; a false callback may exercise final witness revalidation.
    RecoveryPermitAcquired,
    /// A removal permit is fully acquired and retained, but no handoff
    /// reconciliation or lease mutation has started.
    RemovalPermitAcquired,
    /// Recovery has converged, and a separate reservation permit now retains
    /// the exact empty namespace before RESERVED publication starts.
    ReservationPermitAcquired,
    /// A fresh-writer permit retains the exact preactive lease and empty pair
    /// before the first O_EXCL reservation.
    FreshWriterPermitAcquired,
    /// A distinct activation permit retains the exact lease generation and
    /// fresh pair before RESERVED convergence or removal.
    ActivationPermitAcquired,
};

struct OOCPrivateLeaseTestHooks final {
    using StopAfter = bool (*)(OOCPrivateLeaseFaultPoint point, void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

/// Crash boundaries for generic no-delete handoff publication. CanonicalPromoted
/// is intentionally before the following parent-directory durability barrier.
enum class OOCPrivateHandoffFaultPoint : std::uint8_t {
    PendingDurable,
    CanonicalPromoted,
    CanonicalDurable,
    ReservedRevokedDurable,
};

struct OOCPrivateHandoffTestHooks final {
    using StopAfter = bool (*)(OOCPrivateHandoffFaultPoint point, void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

enum class OOCPrivateHandoffState : std::uint8_t {
    None,
    PendingOnly,
    Canonical,
    TaintedPreserved,
};

/// Read-only observation of a generic private handoff. Neither this result nor
/// its record/identity values authorize deletion, cleanup-intent publication,
/// namespace mutation, or reconstruction of an ownership receipt.
struct OOCPrivateHandoffInspectResult final {
    OOCCleanupResult result;
    OOCPrivateHandoffState state = OOCPrivateHandoffState::TaintedPreserved;
    std::optional<OOCPrivateHandoffRecordV1> record;
    std::optional<util::durable_immutable_record::NativeIdentity> identity;

    [[nodiscard]] bool canonical() const noexcept {
        return result.status == OOCCleanupStatus::HandoffPresent &&
               state == OOCPrivateHandoffState::Canonical && record.has_value() &&
               identity.has_value();
    }
};

using OOCPrivateHandoffPublishResult = OOCPrivateHandoffInspectResult;

/// Trusted test-only observation boundaries for exact private-handoff
/// adoption. Each boundary runs while the canonical BaseLock remains held.
enum class OOCPrivateHandoffAdoptionFaultPoint : std::uint8_t {
    CanonicalClassified,
    IndexInitialValidationComplete,
    IndexOpened,
    DataInitialValidationComplete,
    DataOpened,
    BeforeFinalRevalidation,
    BeforeReceiptCommitRevalidation,
};

struct OOCPrivateHandoffAdoptionTestHooks final {
    using StopAfter = bool (*)(OOCPrivateHandoffAdoptionFaultPoint point, void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

/// Current-process read authority for one exact canonical private handoff.
///
/// The receipt owns two exact read-only native files, the held parent and
/// private-directory bindings, and the matching persistent BaseLock. It grants
/// no cleanup, intent-publication, lease activation, or namespace-mutation
/// authority. Destruction closes the files and directories before releasing
/// the lock.
class OOCPrivateHandoffAdoptionReceipt final {
public:
    OOCPrivateHandoffAdoptionReceipt(const OOCPrivateHandoffAdoptionReceipt&) = delete;
    OOCPrivateHandoffAdoptionReceipt& operator=(const OOCPrivateHandoffAdoptionReceipt&) = delete;

    OOCPrivateHandoffAdoptionReceipt(OOCPrivateHandoffAdoptionReceipt&& other) noexcept
        : base_path_(std::move(other.base_path_)),
          private_directory_(std::move(other.private_directory_)),
          lock_path_(std::move(other.lock_path_)), record_(std::move(other.record_)),
          handoff_snapshot_(other.handoff_snapshot_),
          pending_handoff_snapshot_(other.pending_handoff_snapshot_),
          live_lock_(std::move(other.live_lock_)),
          parent_directory_(std::move(other.parent_directory_)),
          private_directory_handle_(std::move(other.private_directory_handle_)),
          index_(std::move(other.index_)), data_(std::move(other.data_)),
          adopter_process_id_(other.adopter_process_id_),
          spent_(std::exchange(other.spent_, true)) {
        other.base_path_.clear();
        other.private_directory_.clear();
        other.lock_path_.clear();
        other.handoff_snapshot_ = {};
        other.pending_handoff_snapshot_.reset();
        other.adopter_process_id_ = 0;
    }

    OOCPrivateHandoffAdoptionReceipt& operator=(OOCPrivateHandoffAdoptionReceipt&&) = delete;

    [[nodiscard]] bool spent() const noexcept {
        return spent_ || !live_lock_ || !parent_directory_ || !private_directory_handle_ ||
               !index_.file.valid() || !data_.file.valid() || !owned_by_current_process();
    }

    [[nodiscard]] const OOCPrivateHandoffRecordV1& record() const noexcept {
        return record_;
    }

    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    handoff_snapshot() const noexcept {
        return handoff_snapshot_;
    }

    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    index_snapshot() const noexcept {
        return index_.snapshot;
    }

    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    data_snapshot() const noexcept {
        return data_.snapshot;
    }

private:
    [[nodiscard]] bool owned_by_current_process() const noexcept {
        return adopter_process_id_ != 0 &&
               adopter_process_id_ == static_cast<std::uint64_t>(gnfs::util::process_id());
    }

    OOCPrivateHandoffAdoptionReceipt(
        std::filesystem::path base_path, std::filesystem::path private_directory,
        std::filesystem::path lock_path, OOCPrivateHandoffRecordV1 record,
        util::durable_immutable_record::RecordSnapshot handoff_snapshot,
        std::optional<util::durable_immutable_record::RecordSnapshot> pending_handoff_snapshot,
        util::durable_immutable_record::OpenedOwnedFile&& index,
        util::durable_immutable_record::OpenedOwnedFile&& data,
        std::shared_ptr<ooc_cleanup_detail::BaseLock> live_lock,
        std::shared_ptr<ooc_cleanup_detail::AdoptionParentDirectoryHandle> parent_directory,
        std::shared_ptr<ooc_cleanup_detail::PrivateDirectoryHandle> private_directory_handle,
        std::uint64_t adopter_process_id) noexcept
        : base_path_(std::move(base_path)), private_directory_(std::move(private_directory)),
          lock_path_(std::move(lock_path)), record_(std::move(record)),
          handoff_snapshot_(handoff_snapshot), pending_handoff_snapshot_(pending_handoff_snapshot),
          live_lock_(std::move(live_lock)), parent_directory_(std::move(parent_directory)),
          private_directory_handle_(std::move(private_directory_handle)), index_(std::move(index)),
          data_(std::move(data)), adopter_process_id_(adopter_process_id) {}

    std::filesystem::path base_path_;
    std::filesystem::path private_directory_;
    std::filesystem::path lock_path_;
    OOCPrivateHandoffRecordV1 record_;
    util::durable_immutable_record::RecordSnapshot handoff_snapshot_;
    std::optional<util::durable_immutable_record::RecordSnapshot> pending_handoff_snapshot_;
    std::shared_ptr<ooc_cleanup_detail::BaseLock> live_lock_;
    std::shared_ptr<ooc_cleanup_detail::AdoptionParentDirectoryHandle> parent_directory_;
    std::shared_ptr<ooc_cleanup_detail::PrivateDirectoryHandle> private_directory_handle_;
    util::durable_immutable_record::OpenedOwnedFile index_;
    util::durable_immutable_record::OpenedOwnedFile data_;
    std::uint64_t adopter_process_id_ = 0;
    bool spent_ = false;

    friend class OOCCleanupTransaction;
    friend class ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1;
    friend class OOCPrivateHandoffReader;
};

struct OOCPrivateHandoffAdoptionResult final {
    OOCCleanupResult result;
    OOCPrivateHandoffState state = OOCPrivateHandoffState::TaintedPreserved;
    std::optional<OOCPrivateHandoffAdoptionReceipt> adoption;

    [[nodiscard]] bool adopted() const noexcept {
        return result.status == OOCCleanupStatus::HandoffPresent &&
               state == OOCPrivateHandoffState::Canonical && adoption.has_value() &&
               !adoption->spent();
    }
};

struct OOCCleanupPaths final {
    std::filesystem::path base_path;
    std::filesystem::path private_directory;
    std::filesystem::path index_path;
    std::filesystem::path data_path;
    std::filesystem::path intent_path;
    std::filesystem::path intent_pending_path;
    std::filesystem::path staged_path;
    std::filesystem::path staged_pending_path;
    std::filesystem::path lock_path;
    std::filesystem::path lease_reserved_path;
    std::filesystem::path lease_reserved_pending_path;
    std::filesystem::path lease_owned_path;
    std::filesystem::path lease_owned_pending_path;
    std::filesystem::path private_handoff_path;
    std::filesystem::path private_handoff_pending_path;
    std::filesystem::path private_handoff_rollback_path;
    std::filesystem::path quarantine_index_path;
    std::filesystem::path quarantine_data_path;
};

namespace ooc_cleanup_detail {

[[nodiscard]] OOCPrivateHandoffInspectResult
classify_private_handoff_locked(const OOCCleanupPaths& paths, const BaseLock& lock);
[[nodiscard]] std::optional<OOCCleanupResult>
preflight_private_cleanup_union_for_transaction_locked(const OOCCleanupPaths& paths,
                                                       const BaseLock& lock,
                                                       bool publish_intent_only);
[[nodiscard]] OOCCleanupResult
recover_private_lease_locked(const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock,
                             const OOCPrivateLeaseTestHooks& hooks = {});
[[nodiscard]] inline std::optional<std::array<std::uint64_t, 3>>
inspect_directory_identity_locked(const std::filesystem::path& directory_path);
inline void inspect_private_lease_transaction_entries(const std::filesystem::path& directory_path,
                                                      const OOCCleanupPaths& paths);

inline constexpr std::uint64_t INTENT_MAGIC = 0x474E46534F434954ULL; // 'GNFSOCIT'
inline constexpr std::uint64_t STAGED_MAGIC = 0x474E46534F435354ULL; // 'GNFSOCST'
inline constexpr std::uint64_t INTENT_VERSION = 1;
inline constexpr std::size_t MARKER_FIELD_COUNT = 20;
inline constexpr std::size_t MARKER_PAYLOAD_BYTES = MARKER_FIELD_COUNT * sizeof(std::uint64_t);
inline constexpr std::size_t MARKER_BYTES = MARKER_PAYLOAD_BYTES + util::SHA256_DIGEST_BYTES;
inline constexpr std::uint64_t PRIVATE_LEASE_MAGIC = 0x474E46534C534531ULL; // 'GNFSLSE1'
inline constexpr std::uint64_t PRIVATE_LEASE_VERSION = 1;
inline constexpr std::size_t PRIVATE_LEASE_FIELD_COUNT = 27;
inline constexpr std::size_t PRIVATE_LEASE_PAYLOAD_BYTES =
    PRIVATE_LEASE_FIELD_COUNT * sizeof(std::uint64_t);
inline constexpr std::size_t PRIVATE_LEASE_MARKER_BYTES =
    PRIVATE_LEASE_PAYLOAD_BYTES + util::SHA256_DIGEST_BYTES;

#ifdef _WIN32
inline constexpr std::uint64_t PLATFORM_ID = 2;
#else
inline constexpr std::uint64_t PLATFORM_ID = 1;
#endif

struct Failure final {
    OOCCleanupStatus status;
    OOCCleanupStage stage;
    std::error_code error;
};

[[noreturn]] inline void fail(OOCCleanupStatus status, OOCCleanupStage stage,
                              std::error_code error = {}) {
    throw Failure{status, stage, error};
}

[[nodiscard]] inline std::error_code invalid_argument_error() noexcept {
    return std::make_error_code(std::errc::invalid_argument);
}

[[nodiscard]] inline std::error_code protocol_error() noexcept {
    return std::make_error_code(std::errc::protocol_error);
}

[[nodiscard]] inline bool path_contains_nul(const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return native.find(std::filesystem::path::value_type{}) !=
           std::filesystem::path::string_type::npos;
}

[[nodiscard]] inline bool
path_leaf_uses_reserved_cleanup_suffix(const std::filesystem::path& leaf) noexcept {
    using Character = std::filesystem::path::value_type;
    const auto& native = leaf.native();
    const std::array reserved_suffixes{
        std::filesystem::path(".gnfs-ooc-cleanup-v1").native(),
        std::filesystem::path(".gnfs-ooc-private-handoff-v1.rollback").native(),
    };
    const auto fold_ascii = [](Character value) noexcept {
        const Character upper_a = static_cast<Character>('A');
        const Character upper_z = static_cast<Character>('Z');
        const Character case_delta = static_cast<Character>('a' - 'A');
        return value >= upper_a && value <= upper_z ? static_cast<Character>(value + case_delta)
                                                    : value;
    };
    for (const auto& reserved : reserved_suffixes) {
        if (native.size() < reserved.size()) {
            continue;
        }
        const std::size_t offset = native.size() - reserved.size();
        bool matches = true;
        for (std::size_t index = 0; index < reserved.size(); ++index) {
            matches = matches && fold_ascii(native[offset + index]) == fold_ascii(reserved[index]);
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool path_leaf_equals_ascii(const std::filesystem::path& leaf,
                                                 const char* expected) noexcept {
    using Character = std::filesystem::path::value_type;
    const auto& native = leaf.native();
    const auto expected_native = std::filesystem::path(expected).native();
    if (native.size() != expected_native.size()) {
        return false;
    }
    const auto fold_ascii = [](Character value) noexcept {
        const Character upper_a = static_cast<Character>('A');
        const Character upper_z = static_cast<Character>('Z');
        const Character case_delta = static_cast<Character>('a' - 'A');
        return value >= upper_a && value <= upper_z ? static_cast<Character>(value + case_delta)
                                                    : value;
    };
    for (std::size_t index = 0; index < native.size(); ++index) {
        if (fold_ascii(native[index]) != fold_ascii(expected_native[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool path_leaf_equals(const std::filesystem::path& left,
                                           const std::filesystem::path& right) noexcept {
    using Character = std::filesystem::path::value_type;
    const auto& left_native = left.native();
    const auto& right_native = right.native();
    if (left_native.size() != right_native.size()) {
        return false;
    }
    const auto fold_ascii = [](Character value) noexcept {
        const Character upper_a = static_cast<Character>('A');
        const Character upper_z = static_cast<Character>('Z');
        const Character case_delta = static_cast<Character>('a' - 'A');
        return value >= upper_a && value <= upper_z ? static_cast<Character>(value + case_delta)
                                                    : value;
    };
    for (std::size_t index = 0; index < left_native.size(); ++index) {
        if (fold_ascii(left_native[index]) != fold_ascii(right_native[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool path_leaf_ends_with_ascii(const std::filesystem::path& leaf,
                                                    const char* suffix) noexcept {
    using Character = std::filesystem::path::value_type;
    const auto& native = leaf.native();
    const auto suffix_native = std::filesystem::path(suffix).native();
    if (native.size() < suffix_native.size()) {
        return false;
    }
    const auto fold_ascii = [](Character value) noexcept {
        const Character upper_a = static_cast<Character>('A');
        const Character upper_z = static_cast<Character>('Z');
        const Character case_delta = static_cast<Character>('a' - 'A');
        return value >= upper_a && value <= upper_z ? static_cast<Character>(value + case_delta)
                                                    : value;
    };
    const std::size_t offset = native.size() - suffix_native.size();
    for (std::size_t index = 0; index < suffix_native.size(); ++index) {
        if (fold_ascii(native[offset + index]) != fold_ascii(suffix_native[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::filesystem::path append_leaf_suffix(const std::filesystem::path& parent,
                                                              const std::filesystem::path& leaf,
                                                              const char* suffix) {
    auto name = leaf.native();
    name.append(std::filesystem::path(suffix).native());
    return parent / std::filesystem::path(std::move(name));
}

[[nodiscard]] inline OOCCleanupPaths freeze_paths(const std::filesystem::path& requested) {
    if (requested.empty() || path_contains_nul(requested)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    std::error_code error;
    auto absolute = std::filesystem::absolute(requested, error);
    if (error) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, error);
    }
    absolute = absolute.lexically_normal();
    const auto leaf = absolute.filename();
    if (absolute.empty() || absolute == absolute.root_path() || leaf.empty() || leaf == "." ||
        leaf == ".." || path_contains_nul(leaf) || path_leaf_uses_reserved_cleanup_suffix(leaf)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    const auto requested_parent = absolute.parent_path();
    const auto requested_parent_leaf = requested_parent.filename();
    const bool private_sink_layout =
        path_leaf_equals_ascii(leaf, "corpus") &&
        path_leaf_ends_with_ascii(requested_parent_leaf, ".gnfs-sink-lease");
    const auto parent_to_freeze =
        private_sink_layout ? requested_parent.parent_path() : requested_parent;
    auto frozen_parent = std::filesystem::weakly_canonical(parent_to_freeze, error);
    if (error || frozen_parent.empty() || !frozen_parent.is_absolute()) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
             error ? error : invalid_argument_error());
    }
    frozen_parent = frozen_parent.lexically_normal();

    std::filesystem::path artifact_parent = frozen_parent;
    std::filesystem::path private_directory;
    std::filesystem::path lock_parent = frozen_parent;
    std::filesystem::path lock_leaf = leaf;
    if (private_sink_layout) {
        if (requested_parent_leaf.empty() || requested_parent_leaf == "." ||
            requested_parent_leaf == ".." || path_contains_nul(requested_parent_leaf) ||
            path_leaf_uses_reserved_cleanup_suffix(requested_parent_leaf)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        artifact_parent = frozen_parent / requested_parent_leaf;
        private_directory = artifact_parent;
        lock_leaf = requested_parent_leaf;
    }

    const auto base = artifact_parent / leaf;
    return OOCCleanupPaths{
        .base_path = base,
        .private_directory = private_directory,
        .index_path = append_leaf_suffix(artifact_parent, leaf, ".relidx"),
        .data_path = append_leaf_suffix(artifact_parent, leaf, ".reldata"),
        .intent_path = append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.intent"),
        .intent_pending_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.intent.pending"),
        .staged_path = append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.staged"),
        .staged_pending_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.staged.pending"),
        .lock_path = append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-ooc-cleanup-v1.lock"),
        .lease_reserved_path =
            append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-private-lease-v1.reserved"),
        .lease_reserved_pending_path =
            append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-private-lease-v1.reserved.pending"),
        .lease_owned_path =
            append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-private-lease-v1.owned"),
        .lease_owned_pending_path =
            append_leaf_suffix(lock_parent, lock_leaf, ".gnfs-private-lease-v1.owned.pending"),
        .private_handoff_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-private-handoff-v1"),
        .private_handoff_pending_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-private-handoff-v1.pending"),
        .private_handoff_rollback_path =
            private_directory.empty()
                ? std::filesystem::path{}
                : append_leaf_suffix(private_directory.parent_path(), private_directory.filename(),
                                     ".gnfs-ooc-private-handoff-v1.rollback"),
        .quarantine_index_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.relidx"),
        .quarantine_data_path =
            append_leaf_suffix(artifact_parent, leaf, ".gnfs-ooc-cleanup-v1.reldata"),
    };
}

[[nodiscard]] inline std::array<std::uint64_t, 2> allocate_private_lease_id() noexcept {
    static std::atomic<std::uint64_t> sequence{1};
    auto mix = [](std::uint64_t value) noexcept {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    };

    std::uint64_t seed = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
#ifdef _WIN32
    seed ^= static_cast<std::uint64_t>(::GetCurrentProcessId()) << 32U;
#else
    seed ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(::getpid())) << 32U;
#endif
    seed ^= sequence.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t entropy = 0;
    try {
        std::random_device random;
        seed ^= static_cast<std::uint64_t>(random()) << 32U;
        seed ^= static_cast<std::uint64_t>(random());
        entropy ^= static_cast<std::uint64_t>(random()) << 32U;
        entropy ^= static_cast<std::uint64_t>(random());
    } catch (...) {
        entropy = seed ^ 0xD1B54A32D192ED03ULL;
    }
    std::array<std::uint64_t, 2> lease_id{
        mix(seed),
        mix(entropy ^ seed ^ 0x94D049BB133111EBULL),
    };
    if (lease_id[0] == 0 && lease_id[1] == 0) {
        lease_id[1] = 1;
    }
    return lease_id;
}

[[nodiscard]] inline std::string
private_lease_id_hex(const std::array<std::uint64_t, 2>& lease_id) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded(32, '0');
    for (std::size_t word = 0; word < lease_id.size(); ++word) {
        for (std::size_t digit = 0; digit < 16; ++digit) {
            const auto shift = static_cast<unsigned>((15 - digit) * 4);
            encoded[word * 16 + digit] =
                digits[static_cast<std::size_t>((lease_id[word] >> shift) & 0x0fU)];
        }
    }
    return encoded;
}

[[nodiscard]] inline std::filesystem::path
private_lease_staging_path(const OOCCleanupPaths& paths,
                           const std::array<std::uint64_t, 2>& lease_id) {
    auto leaf = paths.private_directory.filename().native();
    leaf.append(std::filesystem::path(".gnfs-private-lease-v1.stage-").native());
    leaf.append(std::filesystem::path(private_lease_id_hex(lease_id)).native());
    return paths.private_directory.parent_path() / std::filesystem::path(std::move(leaf));
}

[[nodiscard]] inline std::filesystem::path
private_lease_owner_path(const std::filesystem::path& directory) {
    return directory / ".gnfs-private-lease-v1.owner";
}

[[nodiscard]] inline std::filesystem::path
private_lease_owner_pending_path(const std::filesystem::path& directory) {
    return directory / ".gnfs-private-lease-v1.owner.pending";
}

struct FileIdentity final {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t third = 0;
    std::uint64_t size = 0;

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

void record_private_cleanup_pending_successor(PrivateCleanupMutationGate& gate,
                                              const OOCCleanupPaths& paths, const BaseLock& lock,
                                              const std::filesystem::path& pending_path,
                                              const FileIdentity& identity);
void commit_private_cleanup_canonical(PrivateCleanupMutationGate& gate,
                                      const OOCCleanupPaths& paths, const BaseLock& lock,
                                      const FileIdentity& durable_identity,
                                      bool renamed_from_pending, bool pending_must_remain);

struct OwnershipProof final {
    std::filesystem::path base_path;
    std::uint64_t store_id = 0;
    std::array<std::uint64_t, 3> index_identity{};
    std::array<std::uint64_t, 3> data_identity{};
};

[[nodiscard]] inline std::array<std::uint64_t, 3>
stable_identity(const FileIdentity& identity) noexcept {
    return {identity.first, identity.second, identity.third};
}

enum class InspectKind : std::uint8_t {
    Missing,
    Present,
    Rejected,
    Error,
};

struct InspectResult final {
    InspectKind kind = InspectKind::Error;
    FileIdentity identity;
    std::vector<std::byte> bytes;
    std::error_code error;
};

#ifdef _WIN32

[[nodiscard]] inline std::error_code windows_error(DWORD value) noexcept {
    return {static_cast<int>(value), std::system_category()};
}

[[nodiscard]] inline std::optional<FileIdentity>
windows_identity(HANDLE handle, const BY_HANDLE_FILE_INFORMATION& info) noexcept {
    FILE_ID_INFO file_id{};
    if (!::GetFileInformationByHandleEx(handle, FileIdInfo, &file_id, sizeof(file_id))) {
        return std::nullopt;
    }

    const auto size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) |
                      static_cast<std::uint64_t>(info.nFileSizeLow);
    std::uint64_t file_id_low = 0;
    std::uint64_t file_id_high = 0;
    static_assert(sizeof(file_id.FileId.Identifier) == sizeof(file_id_low) + sizeof(file_id_high));
    std::memcpy(&file_id_low, file_id.FileId.Identifier, sizeof(file_id_low));
    std::memcpy(&file_id_high, file_id.FileId.Identifier + sizeof(file_id_low),
                sizeof(file_id_high));
    return FileIdentity{
        .first = static_cast<std::uint64_t>(file_id.VolumeSerialNumber),
        .second = file_id_low,
        .third = file_id_high,
        .size = size,
    };
}

[[nodiscard]] inline bool
windows_regular_single_link(const BY_HANDLE_FILE_INFORMATION& info) noexcept {
    return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && info.nNumberOfLinks == 1;
}

[[nodiscard]] inline InspectResult inspect_file(const std::filesystem::path& path,
                                                std::size_t bytes_to_read,
                                                bool require_exact_size) {
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return InspectResult{
                .kind = InspectKind::Missing,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(code),
        };
    }

    BY_HANDLE_FILE_INFORMATION before{};
    if (!::GetFileInformationByHandle(handle, &before)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(code),
        };
    }
    const auto before_identity = windows_identity(handle, before);
    if (!before_identity || !windows_regular_single_link(before) ||
        before_identity->size < bytes_to_read ||
        (require_exact_size && before_identity->size != bytes_to_read)) {
        (void)::CloseHandle(handle);
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }

    std::vector<std::byte> bytes(bytes_to_read);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset,
                       static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (!::ReadFile(handle, bytes.data() + offset, request, &read, nullptr)) {
            const DWORD code = ::GetLastError();
            (void)::CloseHandle(handle);
            return InspectResult{
                .kind = InspectKind::Error,
                .identity = {},
                .bytes = {},
                .error = windows_error(code),
            };
        }
        if (read == 0) {
            (void)::CloseHandle(handle);
            return InspectResult{
                .kind = InspectKind::Rejected,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        offset += static_cast<std::size_t>(read);
    }

    BY_HANDLE_FILE_INFORMATION after{};
    if (!::GetFileInformationByHandle(handle, &after)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(code),
        };
    }
    const auto after_identity = windows_identity(handle, after);
    if (!after_identity) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(code),
        };
    }
    if (!::CloseHandle(handle)) {
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = windows_error(::GetLastError()),
        };
    }
    if (!windows_regular_single_link(after) || *after_identity != *before_identity) {
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }
    return InspectResult{
        .kind = InspectKind::Present,
        .identity = *before_identity,
        .bytes = std::move(bytes),
        .error = {},
    };
}

#else

[[nodiscard]] inline std::error_code posix_error(int value) noexcept {
    return {value, std::generic_category()};
}

[[nodiscard]] inline FileIdentity posix_identity(const struct stat& info) noexcept {
    return FileIdentity{
        .first = static_cast<std::uint64_t>(info.st_dev),
        .second = static_cast<std::uint64_t>(info.st_ino),
        .third = 0,
        .size = static_cast<std::uint64_t>(info.st_size),
    };
}

[[nodiscard]] inline bool posix_regular_single_link(const struct stat& info) noexcept {
    return S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size >= 0;
}

[[nodiscard]] inline InspectResult inspect_file(const std::filesystem::path& path,
                                                std::size_t bytes_to_read,
                                                bool require_exact_size) {
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return InspectResult{
                .kind = InspectKind::Missing,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        if (saved_errno == ELOOP) {
            return InspectResult{
                .kind = InspectKind::Rejected,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = posix_error(saved_errno),
        };
    }

    struct stat before{};
    if (::fstat(descriptor, &before) != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = posix_error(saved_errno),
        };
    }
    const FileIdentity before_identity = posix_identity(before);
    if (!posix_regular_single_link(before) || before_identity.size < bytes_to_read ||
        (require_exact_size && before_identity.size != bytes_to_read)) {
        (void)::close(descriptor);
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }

    std::vector<std::byte> bytes(bytes_to_read);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::pread(descriptor, bytes.data() + offset, bytes.size() - offset,
                                      static_cast<off_t>(offset));
        if (count < 0) {
            const int saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            (void)::close(descriptor);
            return InspectResult{
                .kind = InspectKind::Error,
                .identity = {},
                .bytes = {},
                .error = posix_error(saved_errno),
            };
        }
        if (count == 0) {
            (void)::close(descriptor);
            return InspectResult{
                .kind = InspectKind::Rejected,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }
        offset += static_cast<std::size_t>(count);
    }

    struct stat after{};
    if (::fstat(descriptor, &after) != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = posix_error(saved_errno),
        };
    }
    if (::close(descriptor) != 0) {
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = posix_error(errno),
        };
    }
    if (!posix_regular_single_link(after) || posix_identity(after) != before_identity) {
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }
    return InspectResult{
        .kind = InspectKind::Present,
        .identity = before_identity,
        .bytes = std::move(bytes),
        .error = {},
    };
}

#endif

[[nodiscard]] inline std::uint64_t read_native_u64(std::span<const std::byte> bytes,
                                                   std::size_t offset) {
    if (offset > bytes.size() || sizeof(std::uint64_t) > bytes.size() - offset) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

struct ArtifactFingerprint final {
    FileIdentity identity;
    std::uint64_t header_magic = 0;
    std::uint64_t header_version = 0;
    std::uint64_t header_store_id = 0;
    std::uint64_t header_count = 0;

    friend bool operator==(const ArtifactFingerprint&, const ArtifactFingerprint&) = default;
};

enum class ArtifactKind : std::uint8_t {
    Index,
    Data,
};

[[nodiscard]] inline std::size_t artifact_header_bytes(ArtifactKind kind) noexcept {
    return kind == ArtifactKind::Index ? OOCRelationStoreFormat::INDEX_HEADER_BYTES
                                       : OOCRelationStoreFormat::DATA_HEADER_BYTES;
}

[[nodiscard]] inline std::optional<ArtifactFingerprint>
artifact_fingerprint(const InspectResult& inspected, ArtifactKind kind,
                     std::uint64_t expected_store_id) {
    if (inspected.kind != InspectKind::Present ||
        inspected.bytes.size() < OOCRelationStoreFormat::DATA_HEADER_BYTES) {
        return std::nullopt;
    }

    const auto magic = read_native_u64(inspected.bytes, 0);
    const auto version = read_native_u64(inspected.bytes, 8);
    const auto store_id = read_native_u64(inspected.bytes, 16);
    const bool valid_magic = kind == ArtifactKind::Index
                                 ? magic == OOCRelationStoreFormat::MAGIC_V3_FINAL ||
                                       magic == OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE
                                 : magic == OOCRelationStoreFormat::MAGIC_V3_DATA;
    const std::uint64_t minimum_size = kind == ArtifactKind::Index
                                           ? OOCRelationStoreFormat::INDEX_HEADER_BYTES
                                           : OOCRelationStoreFormat::DATA_HEADER_BYTES;
    if (!valid_magic || version != OOCRelationStoreFormat::FORMAT_VERSION_V3 || store_id == 0 ||
        store_id != expected_store_id || inspected.identity.size < minimum_size) {
        return std::nullopt;
    }

    return ArtifactFingerprint{
        .identity = inspected.identity,
        .header_magic = magic,
        .header_version = version,
        .header_store_id = store_id,
        .header_count =
            kind == ArtifactKind::Index
                ? read_native_u64(inspected.bytes, OOCRelationStoreFormat::INDEX_COUNT_OFFSET)
                : 0,
    };
}

struct IntentRecord final {
    std::uint64_t platform_id = PLATFORM_ID;
    std::uint64_t store_id = 0;
    ArtifactFingerprint index;
    ArtifactFingerprint data;

    friend bool operator==(const IntentRecord&, const IntentRecord&) = default;
};

enum class PrivateLeasePhase : std::uint64_t {
    Reserved = 1,
    Owner = 2,
    Owned = 3,
};

enum class PrivateLeaseCapability : std::uint64_t {
    RollbackStagingOnly = 1,
    RemoveOwnerAndEmptyLease = 2,
    RollbackPreactivePairAndLease = 3,
};

struct PrivateLeaseRecord final {
    std::uint64_t platform_id = PLATFORM_ID;
    PrivateLeasePhase phase = PrivateLeasePhase::Reserved;
    PrivateLeaseCapability capability = PrivateLeaseCapability::RollbackStagingOnly;
    std::array<std::uint64_t, 2> lease_id{};
    std::array<std::uint64_t, 4> base_path_digest{};
    std::array<std::uint64_t, 3> parent_identity{};
    std::array<std::uint64_t, 3> lock_identity{};
    std::array<std::uint64_t, 3> directory_identity{};
    std::array<std::uint64_t, 4> reserved_digest{};
    std::array<std::uint64_t, 3> owner_identity{};

    friend bool operator==(const PrivateLeaseRecord&, const PrivateLeaseRecord&) = default;
};

inline void append_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] inline std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t& cursor);

template <std::size_t Size>
inline void append_u64_array(std::vector<std::byte>& bytes,
                             const std::array<std::uint64_t, Size>& values) {
    for (const auto value : values) {
        append_u64(bytes, value);
    }
}

template <std::size_t Size>
[[nodiscard]] inline std::array<std::uint64_t, Size>
read_u64_array(std::span<const std::byte> bytes, std::size_t& cursor) {
    std::array<std::uint64_t, Size> values{};
    for (auto& value : values) {
        value = read_u64(bytes, cursor);
    }
    return values;
}

[[nodiscard]] inline std::array<std::uint64_t, 4>
digest_words(const util::Sha256Digest& digest) noexcept {
    std::array<std::uint64_t, 4> words{};
    for (std::size_t word = 0; word < words.size(); ++word) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            const auto index = word * sizeof(std::uint64_t) + shift / 8;
            words[word] |=
                static_cast<std::uint64_t>(std::to_integer<unsigned char>(digest.bytes[index]))
                << shift;
        }
    }
    return words;
}

[[nodiscard]] inline std::array<std::uint64_t, 4> digest_words(std::span<const std::byte> bytes) {
    const auto digest = util::sha256(bytes);
    if (!digest) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    return digest_words(*digest);
}

[[nodiscard]] inline std::array<std::uint64_t, 4>
frozen_path_digest(const std::filesystem::path& path) {
    const auto& native = path.native();
    const auto characters =
        std::span<const std::filesystem::path::value_type>(native.data(), native.size());
    return digest_words(std::as_bytes(characters));
}

[[nodiscard]] inline bool all_zero(std::span<const std::uint64_t> values) noexcept {
    return std::all_of(values.begin(), values.end(),
                       [](std::uint64_t value) noexcept { return value == 0; });
}

[[nodiscard]] inline bool
private_lease_record_shape_valid(const PrivateLeaseRecord& record) noexcept {
    if (record.platform_id != PLATFORM_ID || all_zero(record.lease_id) ||
        all_zero(record.base_path_digest) || all_zero(record.parent_identity) ||
        all_zero(record.lock_identity)) {
        return false;
    }
    switch (record.phase) {
    case PrivateLeasePhase::Reserved:
        return record.capability == PrivateLeaseCapability::RollbackStagingOnly &&
               all_zero(record.directory_identity) && all_zero(record.reserved_digest) &&
               all_zero(record.owner_identity);
    case PrivateLeasePhase::Owner:
        return record.capability == PrivateLeaseCapability::RemoveOwnerAndEmptyLease &&
               !all_zero(record.directory_identity) && !all_zero(record.reserved_digest) &&
               all_zero(record.owner_identity);
    case PrivateLeasePhase::Owned:
        return (record.capability == PrivateLeaseCapability::RemoveOwnerAndEmptyLease ||
                record.capability == PrivateLeaseCapability::RollbackPreactivePairAndLease) &&
               !all_zero(record.directory_identity) && !all_zero(record.reserved_digest) &&
               !all_zero(record.owner_identity);
    }
    return false;
}

[[nodiscard]] inline std::vector<std::byte>
serialize_private_lease_marker(const PrivateLeaseRecord& record) {
    if (!private_lease_record_shape_valid(record)) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    std::vector<std::byte> bytes;
    bytes.reserve(PRIVATE_LEASE_MARKER_BYTES);
    append_u64(bytes, PRIVATE_LEASE_MAGIC);
    append_u64(bytes, PRIVATE_LEASE_VERSION);
    append_u64(bytes, record.platform_id);
    append_u64(bytes, static_cast<std::uint64_t>(record.phase));
    append_u64(bytes, static_cast<std::uint64_t>(record.capability));
    append_u64_array(bytes, record.lease_id);
    append_u64_array(bytes, record.base_path_digest);
    append_u64_array(bytes, record.parent_identity);
    append_u64_array(bytes, record.lock_identity);
    append_u64_array(bytes, record.directory_identity);
    append_u64_array(bytes, record.reserved_digest);
    append_u64_array(bytes, record.owner_identity);
    if (bytes.size() != PRIVATE_LEASE_PAYLOAD_BYTES) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    const auto digest = util::sha256(bytes);
    if (!digest) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    bytes.insert(bytes.end(), digest->bytes.begin(), digest->bytes.end());
    return bytes;
}

[[nodiscard]] inline PrivateLeaseRecord
parse_private_lease_marker(std::span<const std::byte> bytes) {
    if (bytes.size() != PRIVATE_LEASE_MARKER_BYTES) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    const auto digest = util::sha256(bytes.first(PRIVATE_LEASE_PAYLOAD_BYTES));
    if (!digest ||
        !std::equal(digest->bytes.begin(), digest->bytes.end(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(PRIVATE_LEASE_PAYLOAD_BYTES))) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    std::size_t cursor = 0;
    if (read_u64(bytes, cursor) != PRIVATE_LEASE_MAGIC ||
        read_u64(bytes, cursor) != PRIVATE_LEASE_VERSION) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    PrivateLeaseRecord record;
    record.platform_id = read_u64(bytes, cursor);
    record.phase = static_cast<PrivateLeasePhase>(read_u64(bytes, cursor));
    record.capability = static_cast<PrivateLeaseCapability>(read_u64(bytes, cursor));
    record.lease_id = read_u64_array<2>(bytes, cursor);
    record.base_path_digest = read_u64_array<4>(bytes, cursor);
    record.parent_identity = read_u64_array<3>(bytes, cursor);
    record.lock_identity = read_u64_array<3>(bytes, cursor);
    record.directory_identity = read_u64_array<3>(bytes, cursor);
    record.reserved_digest = read_u64_array<4>(bytes, cursor);
    record.owner_identity = read_u64_array<3>(bytes, cursor);
    if (cursor != PRIVATE_LEASE_PAYLOAD_BYTES || !private_lease_record_shape_valid(record)) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    return record;
}

[[nodiscard]] inline std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t& cursor) {
    if (cursor > bytes.size() || sizeof(std::uint64_t) > bytes.size() - cursor) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[cursor++]))
                 << shift;
    }
    return value;
}

inline void append_fingerprint(std::vector<std::byte>& bytes,
                               const ArtifactFingerprint& fingerprint) {
    append_u64(bytes, fingerprint.identity.first);
    append_u64(bytes, fingerprint.identity.second);
    append_u64(bytes, fingerprint.identity.third);
    append_u64(bytes, fingerprint.identity.size);
    append_u64(bytes, fingerprint.header_magic);
    append_u64(bytes, fingerprint.header_version);
    append_u64(bytes, fingerprint.header_store_id);
    append_u64(bytes, fingerprint.header_count);
}

[[nodiscard]] inline ArtifactFingerprint read_fingerprint(std::span<const std::byte> bytes,
                                                          std::size_t& cursor) {
    ArtifactFingerprint fingerprint;
    fingerprint.identity.first = read_u64(bytes, cursor);
    fingerprint.identity.second = read_u64(bytes, cursor);
    fingerprint.identity.third = read_u64(bytes, cursor);
    fingerprint.identity.size = read_u64(bytes, cursor);
    fingerprint.header_magic = read_u64(bytes, cursor);
    fingerprint.header_version = read_u64(bytes, cursor);
    fingerprint.header_store_id = read_u64(bytes, cursor);
    fingerprint.header_count = read_u64(bytes, cursor);
    return fingerprint;
}

[[nodiscard]] inline std::vector<std::byte> serialize_marker(const IntentRecord& intent,
                                                             std::uint64_t marker_magic) {
    std::vector<std::byte> bytes;
    bytes.reserve(MARKER_BYTES);
    append_u64(bytes, marker_magic);
    append_u64(bytes, INTENT_VERSION);
    append_u64(bytes, intent.platform_id);
    append_u64(bytes, intent.store_id);
    append_fingerprint(bytes, intent.index);
    append_fingerprint(bytes, intent.data);
    if (bytes.size() != MARKER_PAYLOAD_BYTES) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    const auto digest = util::sha256(bytes);
    if (!digest) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    bytes.insert(bytes.end(), digest->bytes.begin(), digest->bytes.end());
    return bytes;
}

[[nodiscard]] inline IntentRecord parse_marker(std::span<const std::byte> bytes,
                                               std::uint64_t expected_magic) {
    if (bytes.size() != MARKER_BYTES) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    const auto digest = util::sha256(bytes.first(MARKER_PAYLOAD_BYTES));
    if (!digest || !std::equal(digest->bytes.begin(), digest->bytes.end(),
                               bytes.begin() + static_cast<std::ptrdiff_t>(MARKER_PAYLOAD_BYTES))) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }

    std::size_t cursor = 0;
    if (read_u64(bytes, cursor) != expected_magic || read_u64(bytes, cursor) != INTENT_VERSION) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }

    IntentRecord intent;
    intent.platform_id = read_u64(bytes, cursor);
    intent.store_id = read_u64(bytes, cursor);
    intent.index = read_fingerprint(bytes, cursor);
    intent.data = read_fingerprint(bytes, cursor);
    if (cursor != MARKER_PAYLOAD_BYTES || intent.platform_id != PLATFORM_ID ||
        intent.store_id == 0 || intent.index.header_store_id != intent.store_id ||
        intent.data.header_store_id != intent.store_id ||
        intent.index.header_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
        intent.data.header_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
        (intent.index.header_magic != OOCRelationStoreFormat::MAGIC_V3_FINAL &&
         intent.index.header_magic != OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE) ||
        intent.data.header_magic != OOCRelationStoreFormat::MAGIC_V3_DATA ||
        intent.data.header_count != 0 ||
        intent.index.identity.size < OOCRelationStoreFormat::INDEX_HEADER_BYTES ||
        intent.data.identity.size < OOCRelationStoreFormat::DATA_HEADER_BYTES ||
        intent.index.identity == intent.data.identity) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (intent.index.header_magic == OOCRelationStoreFormat::MAGIC_V3_FINAL) {
        constexpr std::uint64_t offset_bytes = sizeof(std::uint64_t);
        if (intent.index.header_count >
            (std::numeric_limits<std::uint64_t>::max() -
             OOCRelationStoreFormat::INDEX_HEADER_BYTES - offset_bytes) /
                offset_bytes) {
            fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
        }
        const std::uint64_t expected_index_size = OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                                                  (intent.index.header_count + 1) * offset_bytes;
        if (intent.index.identity.size != expected_index_size) {
            fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
        }
    }
    return intent;
}

template <typename OnDurabilityBarrier>
inline void sync_parent_directory_impl(const std::filesystem::path& parent, OOCCleanupStage stage,
                                       OnDurabilityBarrier&& on_durability_barrier) {
    static_assert(std::is_nothrow_invocable_v<OnDurabilityBarrier&>);
#ifdef _WIN32
    const HANDLE directory = ::CreateFileW(
        parent.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!::GetFileInformationByHandle(directory, &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(directory);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }
    if (!::FlushFileBuffers(directory)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(directory);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }
    on_durability_barrier();
    if (!::CloseHandle(directory)) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
#else
    int descriptor = -1;
    do {
        descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
    int result = -1;
    do {
#if defined(__APPLE__)
        result = ::fcntl(descriptor, F_FULLFSYNC);
#else
        result = ::fsync(descriptor);
#endif
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(saved_errno));
    }
    on_durability_barrier();
    if (::close(descriptor) != 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
#endif
}

inline void sync_parent_directory(const std::filesystem::path& parent, OOCCleanupStage stage) {
    sync_parent_directory_impl(parent, stage, []() noexcept {});
}

class BaseLock final {
public:
    BaseLock(const BaseLock&) = delete;
    BaseLock& operator=(const BaseLock&) = delete;
    BaseLock(BaseLock&&) = delete;
    BaseLock& operator=(BaseLock&&) = delete;

    explicit BaseLock(const std::filesystem::path& path, bool allow_create = true) : path_(path) {
#ifdef _WIN32
        ::SetLastError(ERROR_SUCCESS);
        handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                allow_create ? OPEN_ALWAYS : OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                    FILE_FLAG_WRITE_THROUGH,
                                nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
                fail(OOCCleanupStatus::Busy, OOCCleanupStage::None, windows_error(code));
            }
            if (!allow_create && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                     windows_error(code));
            }
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
        }
        const bool created = allow_create && ::GetLastError() != ERROR_ALREADY_EXISTS;
        BY_HANDLE_FILE_INFORMATION information{};
        std::optional<FileIdentity> held_identity;
        if (!::GetFileInformationByHandle(handle_, &information) ||
            !windows_regular_single_link(information) ||
            !(held_identity = windows_identity(handle_, information))) {
            const DWORD code =
                ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
            release_noexcept();
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, windows_error(code));
        }
        identity_ = stable_identity(*held_identity);
        if (created) {
            if (!::FlushFileBuffers(handle_)) {
                const DWORD code = ::GetLastError();
                release_noexcept();
                fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None,
                     windows_error(code));
            }
            try {
                sync_parent_directory(path.parent_path(), OOCCleanupStage::None);
            } catch (...) {
                release_noexcept();
                throw;
            }
            if (!::FlushFileBuffers(handle_)) {
                const DWORD code = ::GetLastError();
                release_noexcept();
                fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None,
                     windows_error(code));
            }
        }
#else
        bool created = false;
        if (allow_create) {
            do {
                descriptor_ =
                    ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            } while (descriptor_ < 0 && errno == EINTR);
            if (descriptor_ >= 0) {
                created = true;
            } else if (errno == EEXIST) {
                do {
                    descriptor_ = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
                } while (descriptor_ < 0 && errno == EINTR);
            }
        } else {
            do {
                descriptor_ = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
            } while (descriptor_ < 0 && errno == EINTR);
        }
        if (descriptor_ < 0) {
            const int saved_errno = errno;
            if (!allow_create && saved_errno == ENOENT) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                     posix_error(saved_errno));
            }
            fail(saved_errno == ELOOP ? OOCCleanupStatus::NamespaceConflict
                                      : OOCCleanupStatus::IoFailure,
                 OOCCleanupStage::None, posix_error(saved_errno));
        }

        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor_, &held) != 0 || ::lstat(path.c_str(), &named) != 0 ||
            !posix_regular_single_link(held) || !S_ISREG(named.st_mode) || named.st_nlink != 1 ||
            held.st_dev != named.st_dev || held.st_ino != named.st_ino) {
            const int saved_errno = errno == 0 ? EACCES : errno;
            release_noexcept();
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(saved_errno));
        }
        identity_ = stable_identity(posix_identity(held));
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int saved_errno = errno;
            release_noexcept();
            if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
                fail(OOCCleanupStatus::Busy, OOCCleanupStage::None, posix_error(saved_errno));
            }
            if (saved_errno == ENOTSUP || saved_errno == EOPNOTSUPP) {
                fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
                     posix_error(saved_errno));
            }
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(saved_errno));
        }
        struct stat locked_name{};
        if (::lstat(path.c_str(), &locked_name) != 0 || locked_name.st_dev != held.st_dev ||
            locked_name.st_ino != held.st_ino || !S_ISREG(locked_name.st_mode) ||
            locked_name.st_nlink != 1) {
            const int saved_errno = errno == 0 ? EACCES : errno;
            release_noexcept();
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(saved_errno));
        }
        if (created) {
            const auto sync_held_lock = [&]() noexcept {
                int result = -1;
                do {
#if defined(__APPLE__)
                    result = ::fcntl(descriptor_, F_FULLFSYNC);
#else
                    result = ::fsync(descriptor_);
#endif
                } while (result != 0 && errno == EINTR);
                return result;
            };
            if (sync_held_lock() != 0) {
                const int saved_errno = errno;
                release_noexcept();
                fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None,
                     posix_error(saved_errno));
            }
            try {
                sync_parent_directory(path.parent_path(), OOCCleanupStage::None);
            } catch (...) {
                release_noexcept();
                throw;
            }
            if (sync_held_lock() != 0) {
                const int saved_errno = errno;
                release_noexcept();
                fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None,
                     posix_error(saved_errno));
            }
        }
#endif
    }

    [[nodiscard]] const std::array<std::uint64_t, 3>& identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] bool matches(const std::filesystem::path& path) const noexcept {
        return path_ == path;
    }

    /// Non-throwing form used only inside noexcept publication callbacks.
    [[nodiscard]] bool stable_noexcept() const noexcept {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION information{};
        const auto held_identity = ::GetFileInformationByHandle(handle_, &information) != FALSE
                                       ? windows_identity(handle_, information)
                                       : std::nullopt;
        const DWORD attributes = ::GetFileAttributesW(path_.c_str());
        return held_identity && windows_regular_single_link(information) &&
               stable_identity(*held_identity) == identity_ &&
               attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
#else
        struct stat held{};
        struct stat named{};
        struct stat parent{};
        int held_result = -1;
        do {
            held_result = ::fstat(descriptor_, &held);
        } while (held_result != 0 && errno == EINTR);
        int named_result = -1;
        int parent_result = 0;
        if (named_parent_descriptor_ >= 0) {
            do {
                parent_result = ::fstat(named_parent_descriptor_, &parent);
            } while (parent_result != 0 && errno == EINTR);
            do {
                named_result = ::fstatat(named_parent_descriptor_, named_leaf_.c_str(), &named,
                                         AT_SYMLINK_NOFOLLOW);
            } while (named_result != 0 && errno == EINTR);
        } else {
            do {
                named_result = ::lstat(path_.c_str(), &named);
            } while (named_result != 0 && errno == EINTR);
        }
        const std::array<std::uint64_t, 3> parent_identity{
            static_cast<std::uint64_t>(parent.st_dev),
            static_cast<std::uint64_t>(parent.st_ino),
            0,
        };
        return held_result == 0 && named_result == 0 && parent_result == 0 &&
               (named_parent_descriptor_ < 0 ||
                (S_ISDIR(parent.st_mode) && parent_identity == named_parent_identity_)) &&
               posix_regular_single_link(held) && posix_regular_single_link(named) &&
               stable_identity(posix_identity(held)) == identity_ && held.st_dev == named.st_dev &&
               held.st_ino == named.st_ino;
#endif
    }

    /// Revalidate that this held lock is still the single-link regular object
    /// named by the frozen lock path.
    ///
    /// POSIX advisory locks follow the open file description, so another
    /// process can rename the locked inode and create a second lock leaf at the
    /// original path. Every generic handoff authority boundary must reject that
    /// split namespace. Windows opens the lock without delete sharing, which
    /// prevents replacement while held; the repeated handle and path checks
    /// preserve the same fail-closed contract.
    void require_stable() const {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION information{};
        const bool handle_inspected = ::GetFileInformationByHandle(handle_, &information) != FALSE;
        const DWORD handle_error = handle_inspected ? ERROR_SUCCESS : ::GetLastError();
        const auto held_identity =
            handle_inspected ? windows_identity(handle_, information) : std::nullopt;
        const DWORD attributes = ::GetFileAttributesW(path_.c_str());
        const DWORD path_error =
            attributes == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : ERROR_SUCCESS;
        if (!held_identity || !windows_regular_single_link(information) ||
            stable_identity(*held_identity) != identity_ || attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            const DWORD code = handle_error != ERROR_SUCCESS ? handle_error
                               : path_error != ERROR_SUCCESS ? path_error
                                                             : ERROR_ACCESS_DENIED;
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, windows_error(code));
        }
#else
        struct stat held{};
        struct stat named{};
        struct stat parent{};
        int held_result = -1;
        do {
            held_result = ::fstat(descriptor_, &held);
        } while (held_result != 0 && errno == EINTR);
        if (held_result != 0) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
        }

        int named_result = -1;
        int parent_result = 0;
        if (named_parent_descriptor_ >= 0) {
            do {
                parent_result = ::fstat(named_parent_descriptor_, &parent);
            } while (parent_result != 0 && errno == EINTR);
            do {
                named_result = ::fstatat(named_parent_descriptor_, named_leaf_.c_str(), &named,
                                         AT_SYMLINK_NOFOLLOW);
            } while (named_result != 0 && errno == EINTR);
        } else {
            do {
                named_result = ::lstat(path_.c_str(), &named);
            } while (named_result != 0 && errno == EINTR);
        }
        const std::array<std::uint64_t, 3> parent_identity{
            static_cast<std::uint64_t>(parent.st_dev),
            static_cast<std::uint64_t>(parent.st_ino),
            0,
        };
        if (named_result != 0 || parent_result != 0 ||
            (named_parent_descriptor_ >= 0 &&
             (!S_ISDIR(parent.st_mode) || parent_identity != named_parent_identity_)) ||
            !posix_regular_single_link(held) || !posix_regular_single_link(named) ||
            stable_identity(posix_identity(held)) != identity_ || held.st_dev != named.st_dev ||
            held.st_ino != named.st_ino) {
            const int saved_errno = named_result == 0 && parent_result == 0 ? EACCES : errno;
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(saved_errno));
        }
#endif
    }

    ~BaseLock() {
        release_noexcept();
    }

private:
    struct AdoptInheritedOpenFileDescription final {};

    BaseLock(std::filesystem::path path, int descriptor, int named_parent_descriptor,
             std::string named_leaf, const std::array<std::uint64_t, 3>& expected_parent_identity,
             const std::array<std::uint64_t, 3>& expected_lock_identity,
             AdoptInheritedOpenFileDescription)
        : path_(std::move(path)), named_parent_descriptor_(named_parent_descriptor),
          named_leaf_(std::move(named_leaf)), named_parent_identity_(expected_parent_identity) {
#ifdef _WIN32
        (void)descriptor;
        (void)expected_lock_identity;
        fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
             std::make_error_code(std::errc::operation_not_supported));
#else
        if (descriptor < 0 || named_parent_descriptor_ < 0 || named_leaf_.empty() ||
            all_zero(expected_parent_identity) || all_zero(expected_lock_identity)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }

        const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
        const int status_flags = ::fcntl(descriptor, F_GETFL);
        if (descriptor_flags < 0 || status_flags < 0 || (descriptor_flags & FD_CLOEXEC) == 0 ||
            (status_flags & O_ACCMODE) != O_RDWR) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(errno == 0 ? EACCES : errno));
        }

        const auto identity_for = [](const struct stat& metadata) noexcept {
            return std::array<std::uint64_t, 3>{
                static_cast<std::uint64_t>(metadata.st_dev),
                static_cast<std::uint64_t>(metadata.st_ino),
                0,
            };
        };
        const auto lock_policy = [&](const struct stat& metadata) noexcept {
            return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 &&
                   (metadata.st_mode & static_cast<mode_t>(07777)) == 0600 &&
                   metadata.st_uid == ::geteuid();
        };
        const auto parent_policy = [&](const struct stat& metadata) noexcept {
            return S_ISDIR(metadata.st_mode) &&
                   (metadata.st_mode & static_cast<mode_t>(07777)) == 0700 &&
                   metadata.st_uid == ::geteuid();
        };

        struct stat parent_before{};
        struct stat held_before{};
        struct stat named_before{};
        if (::fstat(named_parent_descriptor_, &parent_before) != 0 ||
            ::fstat(descriptor, &held_before) != 0 ||
            ::fstatat(named_parent_descriptor_, named_leaf_.c_str(), &named_before,
                      AT_SYMLINK_NOFOLLOW) != 0) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
        }
        if (!parent_policy(parent_before) || !lock_policy(held_before) ||
            !lock_policy(named_before) || identity_for(parent_before) != expected_parent_identity ||
            identity_for(held_before) != expected_lock_identity ||
            identity_for(named_before) != expected_lock_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, posix_error(EACCES));
        }

        int contender = -1;
        do {
            contender = ::openat(named_parent_descriptor_, named_leaf_.c_str(),
                                 O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        } while (contender < 0 && errno == EINTR);
        if (contender < 0) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
        }
        struct stat contender_metadata{};
        if (::fstat(contender, &contender_metadata) != 0 || !lock_policy(contender_metadata) ||
            identity_for(contender_metadata) != expected_lock_identity) {
            const int saved_errno = errno == 0 ? EACCES : errno;
            (void)::close(contender);
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(saved_errno));
        }
        int contender_result = -1;
        do {
            contender_result = ::flock(contender, LOCK_EX | LOCK_NB);
        } while (contender_result != 0 && errno == EINTR);
        if (contender_result == 0) {
            (void)::close(contender);
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, posix_error(EACCES));
        }
        const int contender_error = errno;
        (void)::close(contender);
        if (contender_error != EWOULDBLOCK && contender_error != EAGAIN) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(contender_error));
        }

        int retained_result = -1;
        do {
            retained_result = ::flock(descriptor, LOCK_EX | LOCK_NB);
        } while (retained_result != 0 && errno == EINTR);
        if (retained_result != 0) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, posix_error(errno));
        }

        struct stat parent_after{};
        struct stat held_after{};
        struct stat named_after{};
        if (::fstat(named_parent_descriptor_, &parent_after) != 0 ||
            ::fstat(descriptor, &held_after) != 0 ||
            ::fstatat(named_parent_descriptor_, named_leaf_.c_str(), &named_after,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !parent_policy(parent_after) || !lock_policy(held_after) || !lock_policy(named_after) ||
            identity_for(parent_after) != expected_parent_identity ||
            identity_for(held_after) != expected_lock_identity ||
            identity_for(named_after) != expected_lock_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(errno == 0 ? EACCES : errno));
        }

        identity_ = expected_lock_identity;
        descriptor_ = descriptor;
#endif
    }

    friend bool try_claim_private_cleanup_action(BaseLock& lock) noexcept;
    friend void release_private_cleanup_action(BaseLock& lock) noexcept;
    friend class OOCPrivateHandoffBorrowedBaseLockV1;
    friend class ::gnfs::sieve::distributed_sieve_worker_entry_detail::
        distributed_sieve_worker_writer_detail::OOCInheritedP8WriterMintV1;

    void release_noexcept() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            // close() releases an ordinary in-process flock when this is the
            // last reference. After fork(), however, the child owns a
            // duplicate descriptor for the same open-file description.
            // Explicit LOCK_UN in either process would unlock both copies and
            // let recovery race a still-running worker; close-only keeps the
            // lock until the final inherited descriptor is gone.
            (void)::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
    std::filesystem::path path_;
    std::array<std::uint64_t, 3> identity_{};
    int named_parent_descriptor_ = -1;
    std::string named_leaf_;
    std::array<std::uint64_t, 3> named_parent_identity_{};
    std::atomic_bool private_cleanup_action_claimed_{false};
};

/// Execute one lock-protected phase only while the held lock remains the
/// canonical namespace leaf. The second check covers test hooks and native
/// operations that may return normally after replacing the named lock.
template <typename Operation>
decltype(auto) invoke_with_stable_base_lock(const BaseLock& lock, Operation&& operation) {
    lock.require_stable();
    const auto invoke_once = [&]() -> decltype(auto) {
        try {
            return std::forward<Operation>(operation)();
        } catch (...) {
            lock.require_stable();
            throw;
        }
    };
    if constexpr (std::is_void_v<std::invoke_result_t<Operation&>>) {
        invoke_once();
        lock.require_stable();
    } else {
        decltype(auto) result = invoke_once();
        lock.require_stable();
        return result;
    }
}

inline void confirm_file_durable(const std::filesystem::path& path, const FileIdentity& expected,
                                 const std::filesystem::path& parent, OOCCleanupStage stage) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool inspected = ::GetFileInformationByHandle(handle, &information) != FALSE;
    const auto identity = inspected ? windows_identity(handle, information) : std::nullopt;
    if (!inspected || !identity || !windows_regular_single_link(information) ||
        *identity != expected) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IntentCorrupt, stage, windows_error(code));
    }
    if (!::FlushFileBuffers(handle)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }
    try {
        sync_parent_directory(parent, stage);
    } catch (...) {
        (void)::CloseHandle(handle);
        throw;
    }
    if (!::FlushFileBuffers(handle)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }
    if (!::CloseHandle(handle)) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
    struct stat information{};
    if (::fstat(descriptor, &information) != 0 || !posix_regular_single_link(information) ||
        posix_identity(information) != expected) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::IntentCorrupt, stage, posix_error(saved_errno));
    }
    const auto sync_held_file = [&]() noexcept {
        int result = -1;
        do {
#if defined(__APPLE__)
            result = ::fcntl(descriptor, F_FULLFSYNC);
#else
            result = ::fsync(descriptor);
#endif
        } while (result != 0 && errno == EINTR);
        return result;
    };
    if (sync_held_file() != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(saved_errno));
    }
    try {
        sync_parent_directory(parent, stage);
    } catch (...) {
        (void)::close(descriptor);
        throw;
    }
    if (sync_held_file() != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(saved_errno));
    }
    if (::close(descriptor) != 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
#endif
}

enum class RenameResult : std::uint8_t {
    Succeeded,
    DestinationExists,
    Unsupported,
    Failed,
};

struct RenameOutcome final {
    RenameResult result = RenameResult::Failed;
    std::error_code error;
};

[[nodiscard]] inline RenameOutcome
rename_no_replace(const std::filesystem::path& source,
                  const std::filesystem::path& destination) noexcept {
#ifdef _WIN32
    if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return RenameOutcome{.result = RenameResult::Succeeded, .error = {}};
    }
    const DWORD code = ::GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
        return RenameOutcome{
            .result = RenameResult::DestinationExists,
            .error = windows_error(code),
        };
    }
    return RenameOutcome{.result = RenameResult::Failed, .error = windows_error(code)};
#elif defined(__APPLE__)
    if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0) {
        return RenameOutcome{.result = RenameResult::Succeeded, .error = {}};
    }
    const int saved_errno = errno;
    if (saved_errno == EEXIST) {
        return RenameOutcome{
            .result = RenameResult::DestinationExists,
            .error = posix_error(saved_errno),
        };
    }
    return RenameOutcome{.result = RenameResult::Failed, .error = posix_error(saved_errno)};
#elif defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int rename_noreplace = 1U;
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                  rename_noreplace) == 0) {
        return RenameOutcome{.result = RenameResult::Succeeded, .error = {}};
    }
    const int saved_errno = errno;
    if (saved_errno == EEXIST) {
        return RenameOutcome{
            .result = RenameResult::DestinationExists,
            .error = posix_error(saved_errno),
        };
    }
    if (saved_errno == ENOSYS || saved_errno == EINVAL || saved_errno == EOPNOTSUPP) {
        return RenameOutcome{.result = RenameResult::Unsupported,
                             .error = posix_error(saved_errno)};
    }
    return RenameOutcome{.result = RenameResult::Failed, .error = posix_error(saved_errno)};
#else
    (void)source;
    (void)destination;
    return RenameOutcome{
        .result = RenameResult::Unsupported,
        .error = std::make_error_code(std::errc::operation_not_supported),
    };
#endif
}

inline void remove_file(const std::filesystem::path& path, OOCCleanupStage stage) {
#ifdef _WIN32
    if (!::DeleteFileW(path.c_str())) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return;
        }
        fail(OOCCleanupStatus::IoFailure, stage, windows_error(code));
    }
#else
    if (::unlink(path.c_str()) != 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return;
        }
        fail(OOCCleanupStatus::IoFailure, stage, posix_error(saved_errno));
    }
#endif
}

[[nodiscard]] inline InspectResult inspect_pending_file(const std::filesystem::path& path,
                                                        std::size_t maximum_bytes = MARKER_BYTES) {
    const auto metadata = inspect_file(path, 0, false);
    if (metadata.kind != InspectKind::Present) {
        return metadata;
    }
    if (metadata.identity.size > maximum_bytes) {
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }

    auto inspected = inspect_file(path, static_cast<std::size_t>(metadata.identity.size), true);
    if (inspected.kind == InspectKind::Present && inspected.identity != metadata.identity) {
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    }
    return inspected;
}

[[nodiscard]] inline bool marker_bytes_equal(std::span<const std::byte> actual,
                                             std::span<const std::byte> expected) noexcept {
    return actual.size() == expected.size() &&
           std::equal(actual.begin(), actual.end(), expected.begin());
}

[[nodiscard]] inline std::optional<IntentRecord> try_parse_marker(std::span<const std::byte> bytes,
                                                                  std::uint64_t expected_magic) {
    try {
        return parse_marker(bytes, expected_magic);
    } catch (const Failure& failure) {
        if (failure.status != OOCCleanupStatus::IntentCorrupt) {
            throw;
        }
        return std::nullopt;
    }
}

[[nodiscard]] inline bool same_native_file(const FileIdentity& actual,
                                           const FileIdentity& expected) noexcept {
    return actual.first == expected.first && actual.second == expected.second &&
           actual.third == expected.third;
}

inline void rewrite_pending_durable(const std::filesystem::path& path,
                                    const FileIdentity& expected_identity,
                                    std::span<const std::byte> bytes, OOCCleanupStage stage) {
#ifdef _WIN32
    const HANDLE handle =
        ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail(OOCCleanupStatus::IoFailure, stage, windows_error(::GetLastError()));
    }

    BY_HANDLE_FILE_INFORMATION before{};
    if (!::GetFileInformationByHandle(handle, &before) || !windows_regular_single_link(before)) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, windows_error(code));
    }
    const auto before_identity = windows_identity(handle, before);
    if (!before_identity || !same_native_file(*before_identity, expected_identity)) {
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    LARGE_INTEGER start{};
    if (!::SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, stage, windows_error(code));
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            (std::min)(bytes.size() - offset,
                       static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!::WriteFile(handle, bytes.data() + offset, requested, &written, nullptr)) {
            const DWORD code = ::GetLastError();
            (void)::CloseHandle(handle);
            fail(OOCCleanupStatus::IoFailure, stage, windows_error(code));
        }
        if (written == 0) {
            (void)::CloseHandle(handle);
            fail(OOCCleanupStatus::IoFailure, stage, std::make_error_code(std::errc::io_error));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (!::SetEndOfFile(handle)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, stage, windows_error(code));
    }
    if (!::FlushFileBuffers(handle)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(code));
    }

    BY_HANDLE_FILE_INFORMATION after{};
    if (!::GetFileInformationByHandle(handle, &after) || !windows_regular_single_link(after)) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, windows_error(code));
    }
    const auto after_identity = windows_identity(handle, after);
    if (!after_identity || !same_native_file(*after_identity, expected_identity) ||
        after_identity->size != bytes.size()) {
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (!::CloseHandle(handle)) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, windows_error(::GetLastError()));
    }
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        const int saved_errno = errno;
        fail(saved_errno == ELOOP ? OOCCleanupStatus::ForeignReplacementPreserved
                                  : OOCCleanupStatus::IoFailure,
             stage, posix_error(saved_errno));
    }

    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !posix_regular_single_link(before)) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, posix_error(saved_errno));
    }
    if (!same_native_file(posix_identity(before), expected_identity)) {
        (void)::close(descriptor);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::pwrite(descriptor, bytes.data() + offset, bytes.size() - offset,
                                         static_cast<off_t>(offset));
        if (written < 0) {
            const int saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            (void)::close(descriptor);
            fail(OOCCleanupStatus::IoFailure, stage, posix_error(saved_errno));
        }
        if (written == 0) {
            (void)::close(descriptor);
            fail(OOCCleanupStatus::IoFailure, stage, std::make_error_code(std::errc::io_error));
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::ftruncate(descriptor, static_cast<off_t>(bytes.size())) != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::IoFailure, stage, posix_error(saved_errno));
    }

#if defined(__APPLE__)
    int sync_result = -1;
    do {
        sync_result = ::fcntl(descriptor, F_FULLFSYNC);
    } while (sync_result != 0 && errno == EINTR);
#else
    int sync_result = -1;
    do {
        sync_result = ::fsync(descriptor);
    } while (sync_result != 0 && errno == EINTR);
#endif
    if (sync_result != 0) {
        const int saved_errno = errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(saved_errno));
    }

    struct stat after{};
    if (::fstat(descriptor, &after) != 0 || !posix_regular_single_link(after)) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, posix_error(saved_errno));
    }
    const auto after_identity = posix_identity(after);
    if (!same_native_file(after_identity, expected_identity) ||
        after_identity.size != bytes.size()) {
        (void)::close(descriptor);
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (::close(descriptor) != 0) {
        fail(OOCCleanupStatus::DurabilityFailure, stage, posix_error(errno));
    }
#endif

    sync_parent_directory(path.parent_path(), stage);
    const auto persisted = inspect_file(path, bytes.size(), true);
    if (persisted.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, persisted.error);
    }
    if (persisted.kind != InspectKind::Present || !marker_bytes_equal(persisted.bytes, bytes)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
}

[[nodiscard]] inline IntentRecord load_marker(const std::filesystem::path& path,
                                              std::uint64_t expected_magic,
                                              FileIdentity* identity = nullptr) {
    auto inspected = inspect_file(path, MARKER_BYTES, true);
    if (inspected.kind == InspectKind::Missing) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None,
             std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (inspected.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (identity != nullptr) {
        *identity = inspected.identity;
    }
    return parse_marker(inspected.bytes, expected_magic);
}

inline FileIdentity confirm_existing_marker(const std::filesystem::path& path,
                                            const IntentRecord& expected,
                                            std::uint64_t expected_magic, OOCCleanupStage stage) {
    FileIdentity before_identity;
    const IntentRecord before = load_marker(path, expected_magic, &before_identity);
    if (before != expected) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    confirm_file_durable(path, before_identity, path.parent_path(), stage);
    FileIdentity after_identity;
    const IntentRecord after = load_marker(path, expected_magic, &after_identity);
    if (after != expected || after_identity != before_identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    return after_identity;
}

inline void revalidate_marker(const std::filesystem::path& path, const IntentRecord& expected,
                              std::uint64_t expected_magic, OOCCleanupStage stage) {
    const IntentRecord current = load_marker(path, expected_magic);
    if (current != expected) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
}

[[nodiscard]] inline IntentRecord load_intent(const OOCCleanupPaths& paths,
                                              FileIdentity* identity = nullptr) {
    return load_marker(paths.intent_path, INTENT_MAGIC, identity);
}

[[nodiscard]] inline IntentRecord load_staged(const OOCCleanupPaths& paths,
                                              FileIdentity* identity = nullptr) {
    return load_marker(paths.staged_path, STAGED_MAGIC, identity);
}

inline void confirm_existing_intent(const OOCCleanupPaths& paths, const IntentRecord& expected,
                                    OOCCleanupStage stage) {
    confirm_existing_marker(paths.intent_path, expected, INTENT_MAGIC, stage);
}

inline void confirm_existing_staged(const OOCCleanupPaths& paths, const IntentRecord& expected,
                                    OOCCleanupStage stage) {
    confirm_existing_marker(paths.staged_path, expected, STAGED_MAGIC, stage);
}

inline void revalidate_intent(const OOCCleanupPaths& paths, const IntentRecord& expected,
                              OOCCleanupStage stage) {
    revalidate_marker(paths.intent_path, expected, INTENT_MAGIC, stage);
}

inline void revalidate_staged(const OOCCleanupPaths& paths, const IntentRecord& expected,
                              OOCCleanupStage stage) {
    revalidate_marker(paths.staged_path, expected, STAGED_MAGIC, stage);
}

enum class ArtifactLocation : std::uint8_t {
    Missing,
    Live,
    Quarantine,
};

[[nodiscard]] inline ArtifactLocation locate_artifact(const std::filesystem::path& live_path,
                                                      const std::filesystem::path& quarantine_path,
                                                      ArtifactKind kind,
                                                      const ArtifactFingerprint& expected,
                                                      OOCCleanupStage stage) {
    auto live = inspect_file(live_path, artifact_header_bytes(kind), false);
    auto quarantine = inspect_file(quarantine_path, artifact_header_bytes(kind), false);

    if (live.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, live.error);
    }
    if (quarantine.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, quarantine.error);
    }
    if (live.kind == InspectKind::Rejected || quarantine.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (live.kind == InspectKind::Present && quarantine.kind == InspectKind::Present) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage,
             std::make_error_code(std::errc::file_exists));
    }
    if (live.kind == InspectKind::Present) {
        const auto actual = artifact_fingerprint(live, kind, expected.header_store_id);
        if (!actual || *actual != expected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
        }
        return ArtifactLocation::Live;
    }
    if (quarantine.kind == InspectKind::Present) {
        const auto actual = artifact_fingerprint(quarantine, kind, expected.header_store_id);
        if (!actual || *actual != expected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
        }
        return ArtifactLocation::Quarantine;
    }
    return ArtifactLocation::Missing;
}

struct NamespaceState final {
    ArtifactLocation index = ArtifactLocation::Missing;
    ArtifactLocation data = ArtifactLocation::Missing;
};

[[nodiscard]] inline NamespaceState audit_namespace(const OOCCleanupPaths& paths,
                                                    const IntentRecord& intent,
                                                    OOCCleanupStage stage, bool allow_missing) {
    revalidate_intent(paths, intent, stage);
    const NamespaceState state{
        .index = locate_artifact(paths.index_path, paths.quarantine_index_path, ArtifactKind::Index,
                                 intent.index, stage),
        .data = locate_artifact(paths.data_path, paths.quarantine_data_path, ArtifactKind::Data,
                                intent.data, stage),
    };
    if (!allow_missing &&
        (state.index == ArtifactLocation::Missing || state.data == ArtifactLocation::Missing)) {
        fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
    }
    return state;
}

[[nodiscard]] inline bool should_interrupt(const OOCCleanupTestHooks& hooks,
                                           OOCCleanupFaultPoint point) noexcept {
    return hooks.stop_after != nullptr && hooks.stop_after(point, hooks.context);
}

[[nodiscard]] inline bool should_interrupt_publish(const OOCCleanupTestHooks& hooks,
                                                   OOCCleanupPublishFaultPoint point) noexcept {
    return hooks.stop_after_publish != nullptr && hooks.stop_after_publish(point, hooks.context);
}

[[nodiscard]] inline bool should_fail_operation(const OOCCleanupTestHooks& hooks,
                                                OOCCleanupTestOperation operation) noexcept {
    return hooks.fail_before_operation != nullptr &&
           hooks.fail_before_operation(operation, hooks.context);
}

inline void inject_operation_failure(const OOCCleanupTestHooks& hooks,
                                     OOCCleanupTestOperation operation, OOCCleanupStatus status,
                                     OOCCleanupStage stage) {
    if (should_fail_operation(hooks, operation)) {
        fail(status, stage, std::make_error_code(std::errc::io_error));
    }
}

[[nodiscard]] inline OOCCleanupResult interrupted(OOCCleanupStage stage) noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Interrupted,
        .stage = stage,
        .native_error = {},
    };
}

inline void quarantine_one(const std::filesystem::path& live,
                           const std::filesystem::path& quarantine, OOCCleanupStage prior_stage) {
    const RenameOutcome renamed = rename_no_replace(live, quarantine);
    switch (renamed.result) {
    case RenameResult::Succeeded:
        return;
    case RenameResult::DestinationExists:
        fail(OOCCleanupStatus::NamespaceConflict, prior_stage, renamed.error);
    case RenameResult::Unsupported:
        fail(OOCCleanupStatus::PlatformUnsupported, prior_stage, renamed.error);
    case RenameResult::Failed:
        fail(OOCCleanupStatus::IoFailure, prior_stage, renamed.error);
    }
    fail(OOCCleanupStatus::UnexpectedFailure, prior_stage, protocol_error());
}

[[nodiscard]] inline bool path_exists_or_rejected(const std::filesystem::path& path,
                                                  OOCCleanupStage stage) {
    const auto inspected = inspect_file(path, 0, false);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, inspected.error);
    }
    return inspected.kind != InspectKind::Missing;
}

[[nodiscard]] inline IntentRecord capture_source_pair(const OOCCleanupPaths& paths,
                                                      std::uint64_t store_id) {
    auto index = inspect_file(paths.index_path, OOCRelationStoreFormat::INDEX_HEADER_BYTES, false);
    auto data = inspect_file(paths.data_path, OOCRelationStoreFormat::DATA_HEADER_BYTES, false);
    if (index.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, index.error);
    }
    if (data.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, data.error);
    }
    if (index.kind != InspectKind::Present || data.kind != InspectKind::Present) {
        fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
    }
    const auto index_fingerprint = artifact_fingerprint(index, ArtifactKind::Index, store_id);
    const auto data_fingerprint = artifact_fingerprint(data, ArtifactKind::Data, store_id);
    if (!index_fingerprint || !data_fingerprint ||
        index_fingerprint->identity == data_fingerprint->identity) {
        fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
    }
    return IntentRecord{
        .platform_id = PLATFORM_ID,
        .store_id = store_id,
        .index = *index_fingerprint,
        .data = *data_fingerprint,
    };
}

inline void require_source_pair_unchanged(const OOCCleanupPaths& paths,
                                          const IntentRecord& intent) {
    const auto current = capture_source_pair(paths, intent.store_id);
    if (current != intent) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

[[nodiscard]] inline bool
expectation_is_well_formed(const OOCExactCleanupExpectation& exact) noexcept {
    if ((exact.index_magic != OOCRelationStoreFormat::MAGIC_V3_FINAL &&
         exact.index_magic != OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE) ||
        exact.index_size < OOCRelationStoreFormat::INDEX_HEADER_BYTES ||
        exact.data_size < OOCRelationStoreFormat::DATA_HEADER_BYTES) {
        return false;
    }
    if (exact.index_magic == OOCRelationStoreFormat::MAGIC_V3_FINAL) {
        constexpr std::uint64_t offset_bytes = sizeof(std::uint64_t);
        if (exact.persisted_count > (std::numeric_limits<std::uint64_t>::max() -
                                     OOCRelationStoreFormat::INDEX_HEADER_BYTES - offset_bytes) /
                                        offset_bytes) {
            return false;
        }
        return exact.index_size == OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                                       (exact.persisted_count + 1) * offset_bytes;
    }
    return true;
}

[[nodiscard]] inline bool request_matches_intent(const OOCCleanupRequest& request,
                                                 const IntentRecord& intent) noexcept {
    if (request.store_id == 0 || request.store_id != intent.store_id) {
        return false;
    }
    if (!request.exact) {
        return intent.index.header_magic == OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE;
    }
    return expectation_is_well_formed(*request.exact) &&
           request.exact->index_magic == intent.index.header_magic &&
           request.exact->persisted_count == intent.index.header_count &&
           request.exact->index_size == intent.index.identity.size &&
           request.exact->data_size == intent.data.identity.size;
}

[[nodiscard]] inline bool ownership_proof_matches(const OwnershipProof& proof,
                                                  const OOCCleanupPaths& paths,
                                                  const IntentRecord& intent) noexcept {
    return proof.base_path == paths.base_path && proof.store_id == intent.store_id &&
           proof.index_identity == stable_identity(intent.index.identity) &&
           proof.data_identity == stable_identity(intent.data.identity);
}

[[nodiscard]] inline OOCCleanupStatus
publish_failure_status(util::durable_immutable_file::PublishStatus status) noexcept {
    using PublishStatus = util::durable_immutable_file::PublishStatus;
    switch (status) {
    case PublishStatus::invalid_path:
        return OOCCleanupStatus::InvalidRequest;
    case PublishStatus::file_sync_failed:
    case PublishStatus::parent_directory_sync_failed:
    case PublishStatus::parent_directory_close_failed:
        return OOCCleanupStatus::DurabilityFailure;
    case PublishStatus::already_exists:
        return OOCCleanupStatus::IntentConflict;
    case PublishStatus::input_too_large:
    case PublishStatus::file_ops_contract_violation:
        return OOCCleanupStatus::UnexpectedFailure;
    case PublishStatus::parent_directory_open_failed:
    case PublishStatus::open_failed:
    case PublishStatus::write_failed:
    case PublishStatus::zero_write_progress:
    case PublishStatus::close_failed:
        return OOCCleanupStatus::IoFailure;
    case PublishStatus::durable:
        return OOCCleanupStatus::Completed;
    case PublishStatus::unexpected_failure:
        return OOCCleanupStatus::UnexpectedFailure;
    }
    return OOCCleanupStatus::UnexpectedFailure;
}

[[nodiscard]] inline bool should_interrupt_private_lease(const OOCPrivateLeaseTestHooks& hooks,
                                                         OOCPrivateLeaseFaultPoint point) noexcept {
    return hooks.stop_after != nullptr && hooks.stop_after(point, hooks.context);
}

[[nodiscard]] inline InspectResult inspect_private_lease_marker(const std::filesystem::path& path) {
    return inspect_file(path, PRIVATE_LEASE_MARKER_BYTES, true);
}

[[nodiscard]] inline PrivateLeaseRecord
load_private_lease_marker(const std::filesystem::path& path, FileIdentity* identity = nullptr) {
    const auto inspected = inspect_private_lease_marker(path);
    if (inspected.kind == InspectKind::Missing) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None,
             std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (inspected.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (identity != nullptr) {
        *identity = inspected.identity;
    }
    return parse_private_lease_marker(inspected.bytes);
}

inline void
validate_private_lease_record_context(const PrivateLeaseRecord& record,
                                      const OOCCleanupPaths& paths,
                                      const std::array<std::uint64_t, 3>& parent_identity,
                                      const std::array<std::uint64_t, 3>& lock_identity) {
    if (record.base_path_digest != frozen_path_digest(paths.base_path) ||
        record.parent_identity != parent_identity || record.lock_identity != lock_identity) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
}

inline void confirm_private_lease_marker(
    const std::filesystem::path& path, const PrivateLeaseRecord& expected,
    std::optional<std::array<std::uint64_t, 3>> expected_identity = std::nullopt) {
    FileIdentity identity;
    const auto before = load_private_lease_marker(path, &identity);
    if (before != expected ||
        (expected_identity && stable_identity(identity) != *expected_identity)) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_file_durable(path, identity, path.parent_path(), OOCCleanupStage::None);
    FileIdentity after_identity;
    const auto after = load_private_lease_marker(path, &after_identity);
    if (after != expected || !same_native_file(after_identity, identity)) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
}

struct PrivateLeasePublication final {
    std::array<std::uint64_t, 3> identity{};
    bool interrupted = false;
};

enum class PrivateLeaseMarkerPublicationPoint : std::uint8_t {
    BeforePendingPreparation,
    PendingDurable,
    BeforeCanonicalRename,
    CanonicalDurable,
    BeforeDuplicatePendingRemoval,
    Complete,
};

/// Source-owned phase callback used only when a retained action permit drives
/// lease-marker publication. Recording happens before user hooks so a hook
/// cannot replace an inode and teach the action a forged successor.
struct PrivateLeaseMarkerPublicationGuard final {
    using Transition = void (*)(PrivateLeaseMarkerPublicationPoint point,
                                const std::filesystem::path& canonical_path,
                                const std::filesystem::path& pending_path,
                                const PrivateLeaseRecord& record, const FileIdentity* identity,
                                void* context);

    Transition transition = nullptr;
    void* context = nullptr;
};

inline void transition_private_lease_marker_guard(const PrivateLeaseMarkerPublicationGuard* guard,
                                                  PrivateLeaseMarkerPublicationPoint point,
                                                  const std::filesystem::path& canonical_path,
                                                  const std::filesystem::path& pending_path,
                                                  const PrivateLeaseRecord& record,
                                                  const FileIdentity* identity = nullptr) {
    if (guard == nullptr) {
        return;
    }
    if (guard->transition == nullptr || guard->context == nullptr) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    guard->transition(point, canonical_path, pending_path, record, identity, guard->context);
}

[[nodiscard]] inline PrivateLeasePublication publish_private_lease_marker_durable(
    const std::filesystem::path& canonical_path, const std::filesystem::path& pending_path,
    const PrivateLeaseRecord& record, const BaseLock& lock, const OOCPrivateLeaseTestHooks& hooks,
    OOCPrivateLeaseFaultPoint pending_fault_point,
    const PrivateLeaseMarkerPublicationGuard* guard = nullptr) {
    if (guard != nullptr && (guard->transition == nullptr || guard->context == nullptr)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    const auto expected = serialize_private_lease_marker(record);
    auto canonical = inspect_private_lease_marker(canonical_path);
    if (canonical.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, canonical.error);
    }
    if (canonical.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (canonical.kind == InspectKind::Present) {
        if (guard != nullptr) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        if (!marker_bytes_equal(canonical.bytes, expected)) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        invoke_with_stable_base_lock(lock,
                                     [&] { confirm_private_lease_marker(canonical_path, record); });
        transition_private_lease_marker_guard(
            guard, PrivateLeaseMarkerPublicationPoint::CanonicalDurable, canonical_path,
            pending_path, record, &canonical.identity);
        const auto pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
        if (pending.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
        }
        if (pending.kind == InspectKind::Rejected ||
            (pending.kind == InspectKind::Present &&
             !marker_bytes_equal(pending.bytes, expected))) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        if (pending.kind == InspectKind::Present) {
            transition_private_lease_marker_guard(
                guard, PrivateLeaseMarkerPublicationPoint::BeforeDuplicatePendingRemoval,
                canonical_path, pending_path, record, &pending.identity);
            invoke_with_stable_base_lock(lock,
                                         [&] { remove_file(pending_path, OOCCleanupStage::None); });
            invoke_with_stable_base_lock(lock, [&] {
                sync_parent_directory(pending_path.parent_path(), OOCCleanupStage::None);
            });
            invoke_with_stable_base_lock(
                lock, [&] { confirm_private_lease_marker(canonical_path, record); });
        }
        transition_private_lease_marker_guard(guard, PrivateLeaseMarkerPublicationPoint::Complete,
                                              canonical_path, pending_path, record,
                                              &canonical.identity);
        return PrivateLeasePublication{
            .identity = stable_identity(canonical.identity),
            .interrupted = false,
        };
    }

    auto pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    if (guard != nullptr && pending.kind == InspectKind::Present) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    bool newly_durable = false;
    if (pending.kind == InspectKind::Missing) {
        transition_private_lease_marker_guard(
            guard, PrivateLeaseMarkerPublicationPoint::BeforePendingPreparation, canonical_path,
            pending_path, record);
        const auto published = invoke_with_stable_base_lock(
            lock, [&] { return util::durable_immutable_file::publish(pending_path, expected); });
        if (!published.is_durable()) {
            if (guard != nullptr) {
                fail(publish_failure_status(published.status()), OOCCleanupStage::None,
                     published.native_error());
            }
            pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
            if (pending.kind != InspectKind::Present ||
                !marker_bytes_equal(pending.bytes, expected)) {
                fail(publish_failure_status(published.status()), OOCCleanupStage::None,
                     published.native_error());
            }
        }
        newly_durable = true;
    } else if (!marker_bytes_equal(pending.bytes, expected)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    FileIdentity pending_identity;
    const auto ready = load_private_lease_marker(pending_path, &pending_identity);
    if (ready != record) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    invoke_with_stable_base_lock(lock, [&] {
        confirm_private_lease_marker(pending_path, record, stable_identity(pending_identity));
    });
    transition_private_lease_marker_guard(guard, PrivateLeaseMarkerPublicationPoint::PendingDurable,
                                          canonical_path, pending_path, record, &pending_identity);
    const bool stop_after_pending =
        newly_durable && invoke_with_stable_base_lock(lock, [&] {
            return should_interrupt_private_lease(hooks, pending_fault_point);
        });
    if (stop_after_pending) {
        return PrivateLeasePublication{
            .identity = stable_identity(pending_identity),
            .interrupted = true,
        };
    }

    transition_private_lease_marker_guard(guard,
                                          PrivateLeaseMarkerPublicationPoint::BeforeCanonicalRename,
                                          canonical_path, pending_path, record, &pending_identity);
    const auto renamed = invoke_with_stable_base_lock(
        lock, [&] { return rename_no_replace(pending_path, canonical_path); });
    switch (renamed.result) {
    case RenameResult::Succeeded:
        invoke_with_stable_base_lock(lock, [&] {
            sync_parent_directory(canonical_path.parent_path(), OOCCleanupStage::None);
        });
        break;
    case RenameResult::DestinationExists:
        if (guard != nullptr) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 renamed.error ? renamed.error : protocol_error());
        }
        invoke_with_stable_base_lock(lock,
                                     [&] { confirm_private_lease_marker(canonical_path, record); });
        break;
    case RenameResult::Unsupported:
        fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None, renamed.error);
    case RenameResult::Failed:
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, renamed.error);
    }

    FileIdentity canonical_identity;
    const auto persisted = load_private_lease_marker(canonical_path, &canonical_identity);
    if (persisted != record || !same_native_file(canonical_identity, pending_identity)) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    invoke_with_stable_base_lock(lock, [&] {
        confirm_private_lease_marker(canonical_path, record, stable_identity(canonical_identity));
    });
    transition_private_lease_marker_guard(
        guard, PrivateLeaseMarkerPublicationPoint::CanonicalDurable, canonical_path, pending_path,
        record, &canonical_identity);
    transition_private_lease_marker_guard(guard, PrivateLeaseMarkerPublicationPoint::Complete,
                                          canonical_path, pending_path, record,
                                          &canonical_identity);
    return PrivateLeasePublication{
        .identity = stable_identity(canonical_identity),
        .interrupted = false,
    };
}

template <typename OnDurableRemoval>
inline void remove_private_lease_marker_durable_impl(
    const std::filesystem::path& path, const PrivateLeaseRecord& expected,
    std::optional<std::array<std::uint64_t, 3>> expected_identity,
    OnDurableRemoval&& on_durable_removal) {
    static_assert(std::is_nothrow_invocable_v<OnDurableRemoval&>);
    const auto inspected = inspect_private_lease_marker(path);
    if (inspected.kind == InspectKind::Missing) {
        sync_parent_directory_impl(path.parent_path(), OOCCleanupStage::None, on_durable_removal);
        return;
    }
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind == InspectKind::Rejected ||
        parse_private_lease_marker(inspected.bytes) != expected ||
        (expected_identity && stable_identity(inspected.identity) != *expected_identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    remove_file(path, OOCCleanupStage::None);
    sync_parent_directory_impl(path.parent_path(), OOCCleanupStage::None, on_durable_removal);
    // The capability transition occurs at the durability barrier, before
    // directory-handle close and absence verification. A close, verification,
    // or BaseLock post-check failure must still observe the committed phase.
    const auto absent = inspect_file(path, 0, false);
    if (absent.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, absent.error);
    }
    if (absent.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

inline void remove_private_lease_marker_durable(
    const std::filesystem::path& path, const PrivateLeaseRecord& expected,
    std::optional<std::array<std::uint64_t, 3>> expected_identity = std::nullopt) {
    remove_private_lease_marker_durable_impl(path, expected, expected_identity, []() noexcept {});
}

[[nodiscard]] inline bool marker_is_valid(std::span<const std::byte> bytes,
                                          std::uint64_t marker_magic) {
    return try_parse_marker(bytes, marker_magic).has_value();
}

[[nodiscard]] inline bool
pending_is_recognizable_owned(std::span<const std::byte> pending,
                              std::span<const std::byte> expected) noexcept {
    constexpr std::size_t ownership_prefix_bytes = 4 * sizeof(std::uint64_t);
    return pending.size() >= ownership_prefix_bytes && pending.size() <= expected.size() &&
           std::equal(pending.begin(),
                      pending.begin() + static_cast<std::ptrdiff_t>(ownership_prefix_bytes),
                      expected.begin());
}

struct PendingDurableResult final {
    FileIdentity identity;
    bool newly_durable = false;
};

[[nodiscard]] inline PendingDurableResult
confirm_pending_durable(const std::filesystem::path& pending_path, const IntentRecord& intent,
                        std::uint64_t marker_magic, OOCCleanupStage stage, bool newly_durable) {
    FileIdentity identity;
    const IntentRecord persisted = load_marker(pending_path, marker_magic, &identity);
    if (persisted != intent) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    confirm_existing_marker(pending_path, intent, marker_magic, stage);
    return PendingDurableResult{
        .identity = identity,
        .newly_durable = newly_durable,
    };
}

[[nodiscard]] inline PendingDurableResult
ensure_pending_durable(const OOCCleanupPaths& paths, const std::filesystem::path& pending_path,
                       const IntentRecord& intent, std::uint64_t marker_magic,
                       OOCCleanupStage stage, const BaseLock& lock,
                       PrivateCleanupMutationGate* mutation_gate = nullptr) {
    const auto expected = serialize_marker(intent, marker_magic);
    auto pending = inspect_pending_file(pending_path);
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, pending.error);
    }
    if (pending.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    if (pending.kind == InspectKind::Missing) {
        if (mutation_gate != nullptr) {
            authorize_private_cleanup_mutation(*mutation_gate, paths, lock,
                                               PrivateCleanupMutationBoundary::PendingPreparation);
        }
        const auto published = util::durable_immutable_file::publish(pending_path, expected);
        if (!published.is_durable()) {
            pending = inspect_pending_file(pending_path);
            if (pending.kind != InspectKind::Present ||
                !marker_bytes_equal(pending.bytes, expected)) {
                fail(publish_failure_status(published.status()), stage, published.native_error());
            }
        }
        return confirm_pending_durable(pending_path, intent, marker_magic, stage, true);
    }

    if (marker_bytes_equal(pending.bytes, expected)) {
        return confirm_pending_durable(pending_path, intent, marker_magic, stage, false);
    }

    // A complete, valid marker for another transaction is foreign even when it
    // happens to share the same reserved pending leaf.
    if (pending.bytes.size() == MARKER_BYTES && (marker_is_valid(pending.bytes, INTENT_MAGIC) ||
                                                 marker_is_valid(pending.bytes, STAGED_MAGIC))) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    // The trusted-parent contract makes a regular single-link malformed
    // pending leaf an abandoned no-authority write. Rewrite that same inode;
    // never unlink it, and never follow or rewrite a hardlink/reparse point.
    if (mutation_gate != nullptr) {
        authorize_private_cleanup_mutation(*mutation_gate, paths, lock,
                                           PrivateCleanupMutationBoundary::PendingPreparation);
    }
    rewrite_pending_durable(pending_path, pending.identity, expected, stage);
    return confirm_pending_durable(pending_path, intent, marker_magic, stage, true);
}

inline void reclaim_pending_for_canonical(const OOCCleanupPaths& paths,
                                          const std::filesystem::path& canonical_path,
                                          const std::filesystem::path& pending_path,
                                          const IntentRecord& intent, std::uint64_t marker_magic,
                                          OOCCleanupStage stage, const BaseLock& lock,
                                          PrivateCleanupMutationGate* mutation_gate = nullptr,
                                          const OOCCleanupTestHooks* hooks = nullptr) {
    const auto expected = serialize_marker(intent, marker_magic);
    auto pending = inspect_pending_file(pending_path);
    if (pending.kind == InspectKind::Missing) {
        return;
    }
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, pending.error);
    }
    if (pending.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    if (pending.bytes.size() == MARKER_BYTES && !marker_bytes_equal(pending.bytes, expected) &&
        (marker_is_valid(pending.bytes, INTENT_MAGIC) ||
         marker_is_valid(pending.bytes, STAGED_MAGIC))) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (!marker_bytes_equal(pending.bytes, expected) &&
        !pending_is_recognizable_owned(pending.bytes, expected)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    revalidate_marker(canonical_path, intent, marker_magic, stage);
    const auto rechecked = inspect_pending_file(pending_path);
    if (rechecked.kind != InspectKind::Present || rechecked.identity != pending.identity ||
        !marker_bytes_equal(rechecked.bytes, pending.bytes)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (hooks != nullptr) {
        inject_operation_failure(*hooks, OOCCleanupTestOperation::MarkerPendingUnlink,
                                 OOCCleanupStatus::IoFailure, stage);
    }
    if (mutation_gate != nullptr) {
        authorize_private_cleanup_mutation(*mutation_gate, paths, lock,
                                           PrivateCleanupMutationBoundary::PendingRemoval);
    }
    const auto final_pending = inspect_pending_file(pending_path);
    if (final_pending.kind != InspectKind::Present || final_pending.identity != pending.identity ||
        !marker_bytes_equal(final_pending.bytes, pending.bytes)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    remove_file(pending_path, stage);
    if (hooks != nullptr) {
        inject_operation_failure(*hooks, OOCCleanupTestOperation::MarkerPendingUnlinkParentSync,
                                 OOCCleanupStatus::DurabilityFailure, stage);
    }
    sync_parent_directory(pending_path.parent_path(), stage);
    revalidate_marker(canonical_path, intent, marker_magic, stage);
}

[[nodiscard]] inline bool publish_marker_durable(
    const OOCCleanupPaths& paths, const std::filesystem::path& canonical_path,
    const std::filesystem::path& pending_path, const IntentRecord& intent,
    std::uint64_t marker_magic, OOCCleanupStage stage, const BaseLock& lock,
    const OOCCleanupTestHooks& hooks, OOCCleanupPublishFaultPoint pending_fault_point,
    PrivateCleanupMutationGate* mutation_gate = nullptr, bool* consume_receipt = nullptr) {
    const auto expected = serialize_marker(intent, marker_magic);
    const auto commit_publication_canonical = [&](const FileIdentity& durable_identity,
                                                  bool renamed_from_pending,
                                                  bool pending_must_remain) {
        if (mutation_gate == nullptr || consume_receipt == nullptr) {
            return;
        }
        if (marker_magic != INTENT_MAGIC) {
            fail(OOCCleanupStatus::InvalidRequest, stage, invalid_argument_error());
        }
        // Set the capability bit before the phase bridge performs any
        // successor reads. Once this exact canonical proof is durable,
        // every later failure must leave the escrowed receipt consumed.
        *consume_receipt = true;
        commit_private_cleanup_canonical(*mutation_gate, paths, lock, durable_identity,
                                         renamed_from_pending, pending_must_remain);
    };
    const auto canonical = inspect_file(canonical_path, MARKER_BYTES, true);
    if (canonical.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, canonical.error);
    }
    if (canonical.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, stage, protocol_error());
    }
    if (canonical.kind == InspectKind::Present) {
        if (!marker_bytes_equal(canonical.bytes, expected)) {
            fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
        }
        const auto canonical_identity = invoke_with_stable_base_lock(lock, [&] {
            return confirm_existing_marker(canonical_path, intent, marker_magic, stage);
        });
        commit_publication_canonical(canonical_identity, false, false);
        invoke_with_stable_base_lock(lock, [&] {
            reclaim_pending_for_canonical(paths, canonical_path, pending_path, intent, marker_magic,
                                          stage, lock, mutation_gate, &hooks);
        });
        return false;
    }

    const auto pending = invoke_with_stable_base_lock(lock, [&] {
        return ensure_pending_durable(paths, pending_path, intent, marker_magic, stage, lock,
                                      mutation_gate);
    });
    if (mutation_gate != nullptr) {
        record_private_cleanup_pending_successor(*mutation_gate, paths, lock, pending_path,
                                                 pending.identity);
    }
    const bool stop_after_pending =
        pending.newly_durable && invoke_with_stable_base_lock(lock, [&] {
            return should_interrupt_publish(hooks, pending_fault_point);
        });
    if (stop_after_pending) {
        return true;
    }

    FileIdentity before_rename;
    const IntentRecord ready = load_marker(pending_path, marker_magic, &before_rename);
    if (ready != intent || before_rename != pending.identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    invoke_with_stable_base_lock(lock, [&] {
        inject_operation_failure(hooks, OOCCleanupTestOperation::MarkerRename,
                                 OOCCleanupStatus::IoFailure, stage);
    });
    if (mutation_gate != nullptr) {
        authorize_private_cleanup_mutation(*mutation_gate, paths, lock,
                                           PrivateCleanupMutationBoundary::CanonicalRename);
    }
    invoke_with_stable_base_lock(lock, [&] {
        inject_operation_failure(hooks, OOCCleanupTestOperation::MarkerRenameAuthorized,
                                 OOCCleanupStatus::IoFailure, stage);
    });
    const RenameOutcome renamed = invoke_with_stable_base_lock(
        lock, [&] { return rename_no_replace(pending_path, canonical_path); });
    switch (renamed.result) {
    case RenameResult::Succeeded:
        invoke_with_stable_base_lock(
            lock, [&] { sync_parent_directory(canonical_path.parent_path(), stage); });
        break;
    case RenameResult::DestinationExists: {
        const auto canonical_identity = invoke_with_stable_base_lock(lock, [&] {
            return confirm_existing_marker(canonical_path, intent, marker_magic, stage);
        });
        commit_publication_canonical(canonical_identity, false, true);
    }
        invoke_with_stable_base_lock(lock, [&] {
            reclaim_pending_for_canonical(paths, canonical_path, pending_path, intent, marker_magic,
                                          stage, lock, mutation_gate, &hooks);
        });
        return false;
    case RenameResult::Unsupported:
        fail(OOCCleanupStatus::PlatformUnsupported, stage, renamed.error);
    case RenameResult::Failed:
        fail(OOCCleanupStatus::IoFailure, stage, renamed.error);
    }

    FileIdentity canonical_identity;
    const IntentRecord persisted = load_marker(canonical_path, marker_magic, &canonical_identity);
    if (persisted != intent || !same_native_file(canonical_identity, before_rename)) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    const auto confirmed_identity = invoke_with_stable_base_lock(
        lock, [&] { return confirm_existing_marker(canonical_path, intent, marker_magic, stage); });
    if (!same_native_file(confirmed_identity, before_rename)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    commit_publication_canonical(confirmed_identity, true, false);
    return false;
}

[[nodiscard]] inline OOCCleanupResult
finish_staged_only_tail(const OOCCleanupPaths& paths, const IntentRecord& staged,
                        const BaseLock& lock, const OOCCleanupTestHooks& hooks,
                        PrivateCleanupMutationGate* mutation_gate = nullptr) {
    constexpr OOCCleanupStage stage = OOCCleanupStage::IntentRemoved;
    revalidate_staged(paths, staged, stage);
    invoke_with_stable_base_lock(lock, [&] {
        reclaim_pending_for_canonical(paths, paths.staged_path, paths.staged_pending_path, staged,
                                      STAGED_MAGIC, stage, lock, mutation_gate, &hooks);
    });
    // Intent has already been durably consumed, so this marker has no delete
    // authority. Original live names may now belong to a newer store and are
    // deliberately ignored; only an unfinished quarantine tail blocks cleanup.
    if (path_exists_or_rejected(paths.quarantine_index_path, stage) ||
        path_exists_or_rejected(paths.quarantine_data_path, stage)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    invoke_with_stable_base_lock(lock, [&] {
        inject_operation_failure(hooks, OOCCleanupTestOperation::StagedUnlink,
                                 OOCCleanupStatus::IoFailure, stage);
    });
    if (mutation_gate != nullptr) {
        authorize_private_cleanup_mutation(*mutation_gate, paths, lock);
    }
    invoke_with_stable_base_lock(lock, [&] { remove_file(paths.staged_path, stage); });
    invoke_with_stable_base_lock(lock, [&] {
        inject_operation_failure(hooks, OOCCleanupTestOperation::StagedUnlinkParentSync,
                                 OOCCleanupStatus::DurabilityFailure, stage);
    });
    invoke_with_stable_base_lock(
        lock, [&] { sync_parent_directory(paths.staged_path.parent_path(), stage); });
    const auto final_staged = inspect_file(paths.staged_path, 0, false);
    if (final_staged.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, final_staged.error);
    }
    if (final_staged.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    lock.require_stable();
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Completed,
        .stage = OOCCleanupStage::Completed,
        .native_error = {},
    };
}

[[nodiscard]] inline OOCCleanupResult
advance_transaction(const OOCCleanupPaths& paths, const IntentRecord& intent, bool staged_exists,
                    const BaseLock& lock, const OOCCleanupTestHooks& hooks,
                    PrivateCleanupMutationGate* mutation_gate = nullptr) {
    OOCCleanupStage stage = OOCCleanupStage::IntentDurable;

    // Confirm any namespace state recovered after a prior uncertain barrier
    // before allowing the next transition to consume it.
    invoke_with_stable_base_lock(
        lock, [&] { sync_parent_directory(paths.intent_path.parent_path(), stage); });

    if (!staged_exists) {
        // Before delete authorization exists, every owned leaf must remain
        // visible at exactly one of its live/quarantine names.
        auto state = audit_namespace(paths, intent, stage, false);
        const bool valid_pre_staged_state =
            (state.index == ArtifactLocation::Live && state.data == ArtifactLocation::Live) ||
            (state.index == ArtifactLocation::Quarantine && state.data == ArtifactLocation::Live) ||
            (state.index == ArtifactLocation::Quarantine &&
             state.data == ArtifactLocation::Quarantine);
        if (!valid_pre_staged_state) {
            fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
        }
        if (state.index == ArtifactLocation::Live) {
            invoke_with_stable_base_lock(lock, [&] {
                inject_operation_failure(hooks, OOCCleanupTestOperation::IndexRename,
                                         OOCCleanupStatus::IoFailure, stage);
            });
            if (mutation_gate != nullptr) {
                authorize_private_cleanup_mutation(*mutation_gate, paths, lock);
            }
            invoke_with_stable_base_lock(lock, [&] {
                quarantine_one(paths.index_path, paths.quarantine_index_path, stage);
            });
            invoke_with_stable_base_lock(lock, [&] {
                inject_operation_failure(hooks, OOCCleanupTestOperation::IndexRenameParentSync,
                                         OOCCleanupStatus::DurabilityFailure, stage);
            });
            invoke_with_stable_base_lock(
                lock, [&] { sync_parent_directory(paths.intent_path.parent_path(), stage); });
            state = audit_namespace(paths, intent, stage, false);
            if (state.index != ArtifactLocation::Quarantine) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
            }
            stage = OOCCleanupStage::IndexQuarantined;
            if (invoke_with_stable_base_lock(lock, [&] {
                    return should_interrupt(hooks, OOCCleanupFaultPoint::FirstRenameDurable);
                })) {
                return interrupted(stage);
            }
        } else if (state.index != ArtifactLocation::Quarantine) {
            fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
        } else {
            stage = OOCCleanupStage::IndexQuarantined;
        }

        state = audit_namespace(paths, intent, stage, false);
        if (state.data == ArtifactLocation::Live) {
            invoke_with_stable_base_lock(lock, [&] {
                inject_operation_failure(hooks, OOCCleanupTestOperation::DataRename,
                                         OOCCleanupStatus::IoFailure, stage);
            });
            if (mutation_gate != nullptr) {
                authorize_private_cleanup_mutation(*mutation_gate, paths, lock);
            }
            invoke_with_stable_base_lock(
                lock, [&] { quarantine_one(paths.data_path, paths.quarantine_data_path, stage); });
            invoke_with_stable_base_lock(lock, [&] {
                inject_operation_failure(hooks, OOCCleanupTestOperation::DataRenameParentSync,
                                         OOCCleanupStatus::DurabilityFailure, stage);
            });
            invoke_with_stable_base_lock(
                lock, [&] { sync_parent_directory(paths.intent_path.parent_path(), stage); });
            state = audit_namespace(paths, intent, stage, false);
            if (state.index != ArtifactLocation::Quarantine ||
                state.data != ArtifactLocation::Quarantine) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
            }
            stage = OOCCleanupStage::PairQuarantined;
            if (invoke_with_stable_base_lock(lock, [&] {
                    return should_interrupt(hooks, OOCCleanupFaultPoint::SecondRenameDurable);
                })) {
                return interrupted(stage);
            }
        } else if (state.data != ArtifactLocation::Quarantine) {
            fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
        } else {
            stage = OOCCleanupStage::PairQuarantined;
        }

        state = audit_namespace(paths, intent, stage, false);
        if (state.index != ArtifactLocation::Quarantine ||
            state.data != ArtifactLocation::Quarantine) {
            fail(OOCCleanupStatus::NamespaceConflict, stage, protocol_error());
        }
        if (publish_marker_durable(
                paths, paths.staged_path, paths.staged_pending_path, intent, STAGED_MAGIC, stage,
                lock, hooks, OOCCleanupPublishFaultPoint::StagedPendingDurable, mutation_gate)) {
            return interrupted(stage);
        }
        confirm_existing_staged(paths, intent, stage);
        stage = OOCCleanupStage::DeleteAuthorized;
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt(hooks, OOCCleanupFaultPoint::DeleteAuthorizedDurable);
            })) {
            return interrupted(stage);
        }
    } else {
        invoke_with_stable_base_lock(lock, [&] {
            confirm_existing_staged(paths, intent, OOCCleanupStage::DeleteAuthorized);
        });
        invoke_with_stable_base_lock(lock, [&] {
            reclaim_pending_for_canonical(paths, paths.staged_path, paths.staged_pending_path,
                                          intent, STAGED_MAGIC, OOCCleanupStage::DeleteAuthorized,
                                          lock, mutation_gate, &hooks);
        });
        stage = OOCCleanupStage::DeleteAuthorized;
    }

    revalidate_staged(paths, intent, stage);
    auto state = audit_namespace(paths, intent, stage, true);
    if (state.index == ArtifactLocation::Live || state.data == ArtifactLocation::Live) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    if (state.data == ArtifactLocation::Quarantine) {
        revalidate_staged(paths, intent, stage);
        invoke_with_stable_base_lock(lock, [&] {
            inject_operation_failure(hooks, OOCCleanupTestOperation::DataUnlink,
                                     OOCCleanupStatus::IoFailure, stage);
        });
        if (mutation_gate != nullptr) {
            authorize_private_cleanup_mutation(*mutation_gate, paths, lock);
        }
        invoke_with_stable_base_lock(lock, [&] { remove_file(paths.quarantine_data_path, stage); });
        invoke_with_stable_base_lock(lock, [&] {
            inject_operation_failure(hooks, OOCCleanupTestOperation::DataUnlinkParentSync,
                                     OOCCleanupStatus::DurabilityFailure, stage);
        });
        invoke_with_stable_base_lock(
            lock, [&] { sync_parent_directory(paths.intent_path.parent_path(), stage); });
        state = audit_namespace(paths, intent, stage, true);
        revalidate_staged(paths, intent, stage);
        if (state.data != ArtifactLocation::Missing) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
        }
        stage = OOCCleanupStage::DataRemoved;
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt(hooks, OOCCleanupFaultPoint::FirstUnlinkDurable);
            })) {
            return interrupted(stage);
        }
    } else {
        stage = OOCCleanupStage::DataRemoved;
    }

    state = audit_namespace(paths, intent, stage, true);
    revalidate_staged(paths, intent, stage);
    if (state.index == ArtifactLocation::Live || state.data == ArtifactLocation::Live) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }
    if (state.index == ArtifactLocation::Quarantine) {
        invoke_with_stable_base_lock(lock, [&] {
            inject_operation_failure(hooks, OOCCleanupTestOperation::IndexUnlink,
                                     OOCCleanupStatus::IoFailure, stage);
        });
        if (mutation_gate != nullptr) {
            authorize_private_cleanup_mutation(*mutation_gate, paths, lock);
        }
        invoke_with_stable_base_lock(lock,
                                     [&] { remove_file(paths.quarantine_index_path, stage); });
        invoke_with_stable_base_lock(lock, [&] {
            inject_operation_failure(hooks, OOCCleanupTestOperation::IndexUnlinkParentSync,
                                     OOCCleanupStatus::DurabilityFailure, stage);
        });
        invoke_with_stable_base_lock(
            lock, [&] { sync_parent_directory(paths.intent_path.parent_path(), stage); });
        state = audit_namespace(paths, intent, stage, true);
        revalidate_staged(paths, intent, stage);
        if (state.index != ArtifactLocation::Missing || state.data != ArtifactLocation::Missing) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
        }
        stage = OOCCleanupStage::IndexRemoved;
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt(hooks, OOCCleanupFaultPoint::SecondUnlinkDurable);
            })) {
            return interrupted(stage);
        }
    } else {
        stage = OOCCleanupStage::IndexRemoved;
    }

    state = audit_namespace(paths, intent, stage, true);
    revalidate_staged(paths, intent, stage);
    if (state.index != ArtifactLocation::Missing || state.data != ArtifactLocation::Missing) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, stage, protocol_error());
    }

    // Consuming intent closes delete authority. If the process exits after this
    // barrier, staged-only recovery may remove only the staged marker.
    invoke_with_stable_base_lock(lock, [&] {
        inject_operation_failure(hooks, OOCCleanupTestOperation::IntentUnlink,
                                 OOCCleanupStatus::IoFailure, stage);
    });
    if (mutation_gate != nullptr) {
        authorize_private_cleanup_mutation(*mutation_gate, paths, lock);
    }
    invoke_with_stable_base_lock(lock, [&] { remove_file(paths.intent_path, stage); });
    invoke_with_stable_base_lock(lock, [&] {
        inject_operation_failure(hooks, OOCCleanupTestOperation::IntentUnlinkParentSync,
                                 OOCCleanupStatus::DurabilityFailure, stage);
    });
    invoke_with_stable_base_lock(
        lock, [&] { sync_parent_directory(paths.intent_path.parent_path(), stage); });
    const auto final_intent = inspect_file(paths.intent_path, 0, false);
    if (final_intent.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, stage, final_intent.error);
    }
    if (final_intent.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::IntentConflict, stage, protocol_error());
    }
    stage = OOCCleanupStage::IntentRemoved;
    if (invoke_with_stable_base_lock(lock, [&] {
            return should_interrupt(hooks, OOCCleanupFaultPoint::IntentRemovedDurable);
        })) {
        return interrupted(stage);
    }
    return finish_staged_only_tail(paths, intent, lock, hooks, mutation_gate);
}

[[nodiscard]] inline OOCCleanupResult
run_transaction_locked(const OOCCleanupPaths& paths, const BaseLock& held_lock,
                       const OOCCleanupRequest* request, bool allow_begin,
                       const OwnershipProof* ownership_proof, bool* consume_receipt,
                       const OOCCleanupTestHooks& hooks, bool publish_intent_only = false,
                       PrivateCleanupMutationGate* mutation_gate = nullptr) {
    if (!held_lock.matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    held_lock.require_stable();
    if ((allow_begin &&
         (request == nullptr || ownership_proof == nullptr || consume_receipt == nullptr)) ||
        (!allow_begin && ownership_proof != nullptr)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    if (mutation_gate == nullptr) {
        if (const auto blocked = preflight_private_cleanup_union_for_transaction_locked(
                paths, held_lock, publish_intent_only)) {
            return *blocked;
        }
    }
    if (!paths.private_directory.empty() &&
        inspect_directory_identity_locked(paths.private_directory)) {
        inspect_private_lease_transaction_entries(paths.private_directory, paths);
    }

    auto intent_inspection = inspect_file(paths.intent_path, MARKER_BYTES, true);
    if (intent_inspection.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, intent_inspection.error);
    }
    if (intent_inspection.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    auto staged_inspection = inspect_file(paths.staged_path, MARKER_BYTES, true);
    if (staged_inspection.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, staged_inspection.error);
    }
    if (staged_inspection.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }

    IntentRecord intent;
    if (intent_inspection.kind == InspectKind::Missing) {
        if (staged_inspection.kind == InspectKind::Present) {
            const IntentRecord staged = parse_marker(staged_inspection.bytes, STAGED_MAGIC);
            const bool tail_matches_request =
                request != nullptr && request_matches_intent(*request, staged);
            confirm_existing_staged(paths, staged, OOCCleanupStage::IntentRemoved);
            const auto tail_result =
                finish_staged_only_tail(paths, staged, held_lock, hooks, mutation_gate);
            if (!allow_begin || request == nullptr) {
                if (request != nullptr && !tail_matches_request) {
                    held_lock.require_stable();
                    return OOCCleanupResult{
                        .status = OOCCleanupStatus::NoTransaction,
                        .stage = OOCCleanupStage::None,
                        .native_error = {},
                    };
                }
                return tail_result;
            }
            staged_inspection = InspectResult{
                .kind = InspectKind::Missing,
                .identity = {},
                .bytes = {},
                .error = {},
            };
        }

        const bool quarantine_exists =
            path_exists_or_rejected(paths.quarantine_index_path, OOCCleanupStage::None) ||
            path_exists_or_rejected(paths.quarantine_data_path, OOCCleanupStage::None);
        if (quarantine_exists) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        const bool intent_pending_exists =
            path_exists_or_rejected(paths.intent_pending_path, OOCCleanupStage::None);
        if (!allow_begin) {
            held_lock.require_stable();
            return OOCCleanupResult{
                .status = OOCCleanupStatus::NoTransaction,
                .stage = OOCCleanupStage::None,
                .native_error = {},
            };
        }
        if (request == nullptr || request->store_id == 0 ||
            (request->exact && !expectation_is_well_formed(*request->exact))) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }

        const bool index_exists = path_exists_or_rejected(paths.index_path, OOCCleanupStage::None);
        const bool data_exists = path_exists_or_rejected(paths.data_path, OOCCleanupStage::None);
        if (!index_exists && !data_exists) {
            if (intent_pending_exists) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            held_lock.require_stable();
            if (mutation_gate != nullptr) {
                if (publish_intent_only) {
                    authorize_private_cleanup_mutation(
                        *mutation_gate, paths, held_lock,
                        PrivateCleanupMutationBoundary::PendingPreparation);
                    fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None,
                         protocol_error());
                }
                authorize_private_cleanup_mutation(*mutation_gate, paths, held_lock);
            }
            *consume_receipt = true;
            return OOCCleanupResult{
                .status = OOCCleanupStatus::Completed,
                .stage = OOCCleanupStage::Completed,
                .native_error = {},
            };
        }

        intent = capture_source_pair(paths, request->store_id);
        if (!request_matches_intent(*request, intent)) {
            fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
        }
        if (!ownership_proof_matches(*ownership_proof, paths, intent)) {
            fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
        }
        require_source_pair_unchanged(paths, intent);
        if (publish_marker_durable(paths, paths.intent_path, paths.intent_pending_path, intent,
                                   INTENT_MAGIC, OOCCleanupStage::None, held_lock, hooks,
                                   OOCCleanupPublishFaultPoint::IntentPendingDurable, mutation_gate,
                                   publish_intent_only ? consume_receipt : nullptr)) {
            return interrupted(OOCCleanupStage::None);
        }
        held_lock.require_stable();
        if (!publish_intent_only || mutation_gate == nullptr) {
            *consume_receipt = true;
        }
        if (invoke_with_stable_base_lock(held_lock, [&] {
                return should_interrupt(hooks, OOCCleanupFaultPoint::IntentDurable);
            })) {
            return interrupted(OOCCleanupStage::IntentDurable);
        }
    } else {
        intent = parse_marker(intent_inspection.bytes, INTENT_MAGIC);
        if (request != nullptr && !request_matches_intent(*request, intent)) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        if (ownership_proof != nullptr &&
            !ownership_proof_matches(*ownership_proof, paths, intent)) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        const auto canonical_identity = invoke_with_stable_base_lock(held_lock, [&] {
            return confirm_existing_marker(paths.intent_path, intent, INTENT_MAGIC,
                                           OOCCleanupStage::IntentDurable);
        });
        if (mutation_gate != nullptr && publish_intent_only) {
            if (consume_receipt == nullptr) {
                fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                     invalid_argument_error());
            }
            *consume_receipt = true;
            commit_private_cleanup_canonical(*mutation_gate, paths, held_lock, canonical_identity,
                                             false, false);
        }
        invoke_with_stable_base_lock(held_lock, [&] {
            reclaim_pending_for_canonical(paths, paths.intent_path, paths.intent_pending_path,
                                          intent, INTENT_MAGIC, OOCCleanupStage::IntentDurable,
                                          held_lock, mutation_gate, &hooks);
        });
        if (consume_receipt != nullptr && (!publish_intent_only || mutation_gate == nullptr)) {
            held_lock.require_stable();
            *consume_receipt = true;
        }
    }

    if (publish_intent_only) {
        if (!allow_begin || staged_inspection.kind != InspectKind::Missing) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::IntentDurable,
                 protocol_error());
        }
        const auto state = audit_namespace(paths, intent, OOCCleanupStage::IntentDurable, false);
        if (state.index != ArtifactLocation::Live || state.data != ArtifactLocation::Live) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::IntentDurable,
                 protocol_error());
        }
        held_lock.require_stable();
        if (mutation_gate != nullptr) {
            authorize_private_cleanup_mutation(*mutation_gate, paths, held_lock,
                                               PrivateCleanupMutationBoundary::PublicationComplete);
        }
        return OOCCleanupResult{
            .status = OOCCleanupStatus::Completed,
            .stage = OOCCleanupStage::IntentDurable,
            .native_error = {},
        };
    }

    bool staged_exists = staged_inspection.kind == InspectKind::Present;
    if (staged_exists) {
        const IntentRecord staged = parse_marker(staged_inspection.bytes, STAGED_MAGIC);
        if (staged != intent) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::DeleteAuthorized,
                 protocol_error());
        }
        invoke_with_stable_base_lock(held_lock, [&] {
            confirm_existing_staged(paths, intent, OOCCleanupStage::DeleteAuthorized);
        });
    }
    return advance_transaction(paths, intent, staged_exists, held_lock, hooks, mutation_gate);
}

/// Run the public legacy cleanup path. The out-of-line implementation retains
/// a source-private RunLegacyCleanup permit through the complete executor.
[[nodiscard]] OOCCleanupResult run_transaction(const std::filesystem::path& requested_base,
                                               const OOCCleanupRequest* request, bool allow_begin,
                                               const OwnershipProof* ownership_proof,
                                               bool* consume_receipt,
                                               const OOCCleanupTestHooks& hooks);

/// Require the complete pair namespace to be empty while the caller holds the
/// matching BaseLock. The persistent regular lock leaf itself is intentionally
/// outside this set.
inline void require_pair_namespace_reusable_locked(const OOCCleanupPaths& paths) {
    const std::array<const std::filesystem::path*, 11> leaves{
        &paths.index_path,
        &paths.data_path,
        &paths.intent_path,
        &paths.intent_pending_path,
        &paths.staged_path,
        &paths.staged_pending_path,
        &paths.private_handoff_path,
        &paths.private_handoff_pending_path,
        &paths.private_handoff_rollback_path,
        &paths.quarantine_index_path,
        &paths.quarantine_data_path,
    };
    for (const auto* leaf : leaves) {
        if (leaf->empty()) {
            continue;
        }
        const auto inspected = inspect_file(*leaf, 0, false);
        if (inspected.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
        }
        if (inspected.kind != InspectKind::Missing) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
    }
}

[[nodiscard]] inline std::array<std::uint64_t, 3>
capture_directory_identity_locked(const std::filesystem::path& directory_path) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        directory_path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(::GetLastError()));
    }

    BY_HANDLE_FILE_INFORMATION information{};
    const bool inspected = ::GetFileInformationByHandle(handle, &information) != FALSE;
    const auto identity = inspected ? windows_identity(handle, information) : std::nullopt;
    if (!inspected || !identity || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const DWORD code =
            ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, windows_error(code));
    }
    if (!::CloseHandle(handle)) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(::GetLastError()));
    }
    return stable_identity(*identity);
#else
    int descriptor = -1;
    do {
        descriptor =
            ::open(directory_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        fail(errno == ELOOP ? OOCCleanupStatus::NamespaceConflict : OOCCleanupStatus::IoFailure,
             OOCCleanupStage::None, posix_error(errno));
    }

    struct stat held{};
    struct stat named{};
    if (::fstat(descriptor, &held) != 0 || ::lstat(directory_path.c_str(), &named) != 0 ||
        !S_ISDIR(held.st_mode) || !S_ISDIR(named.st_mode) || held.st_dev != named.st_dev ||
        held.st_ino != named.st_ino) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(descriptor);
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, posix_error(saved_errno));
    }
    const auto identity = stable_identity(posix_identity(held));
    if (::close(descriptor) != 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }
    return identity;
#endif
}

[[nodiscard]] inline std::optional<std::array<std::uint64_t, 3>>
inspect_directory_identity_locked(const std::filesystem::path& directory_path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(directory_path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return std::nullopt;
        }
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    if (!std::filesystem::is_directory(status)) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    return capture_directory_identity_locked(directory_path);
}

#ifndef _WIN32
/// Held no-follow handle for the private lease directory. Generic handoff
/// classification first uses this handle for relative existence only, so
/// platforms without the production ACL adapter can still preserve legacy V1
/// behavior when both generic leaves are absent. Once either leaf exists, the
/// stricter private-directory policy is mandatory before any record bytes are
/// observed.
class PrivateDirectoryHandle final {
public:
    PrivateDirectoryHandle(const PrivateDirectoryHandle&) = delete;
    PrivateDirectoryHandle& operator=(const PrivateDirectoryHandle&) = delete;
    PrivateDirectoryHandle(PrivateDirectoryHandle&&) = delete;
    PrivateDirectoryHandle& operator=(PrivateDirectoryHandle&&) = delete;

    explicit PrivateDirectoryHandle(const std::filesystem::path& path) : path_(path) {
        do {
            descriptor_ = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } while (descriptor_ < 0 && errno == EINTR);
        if (descriptor_ < 0) {
            const int saved_errno = errno;
            fail(saved_errno == ELOOP || saved_errno == ENOTDIR
                     ? OOCCleanupStatus::NamespaceConflict
                     : OOCCleanupStatus::IoFailure,
                 OOCCleanupStage::None, posix_error(saved_errno));
        }

        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor_, &held) != 0 || ::lstat(path.c_str(), &named) != 0 ||
            !S_ISDIR(held.st_mode) || !S_ISDIR(named.st_mode) || held.st_dev != named.st_dev ||
            held.st_ino != named.st_ino) {
            const int saved_errno = errno == 0 ? EACCES : errno;
            (void)::close(descriptor_);
            descriptor_ = -1;
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(saved_errno));
        }
        identity_ = {
            static_cast<std::uint64_t>(held.st_dev),
            static_cast<std::uint64_t>(held.st_ino),
            0,
        };
    }

    ~PrivateDirectoryHandle() {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
    }

    [[nodiscard]] util::durable_immutable_record::NativeHandle native_handle() const noexcept {
        return static_cast<util::durable_immutable_record::NativeHandle>(descriptor_);
    }

    [[nodiscard]] const std::array<std::uint64_t, 3>& identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] bool leaf_exists(const std::filesystem::path& leaf) const {
        if (leaf.empty() || leaf.has_parent_path()) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        struct stat metadata{};
        int result = -1;
        do {
            result = ::fstatat(descriptor_, leaf.c_str(), &metadata, AT_SYMLINK_NOFOLLOW);
        } while (result != 0 && errno == EINTR);
        if (result == 0) {
            return true;
        }
        if (errno == ENOENT) {
            return false;
        }
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }

    void require_private_policy() const {
        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor_, &held) != 0 || ::lstat(path_.c_str(), &named) != 0) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
        }
        constexpr mode_t private_mode = S_IRWXU;
        if (!S_ISDIR(held.st_mode) || !S_ISDIR(named.st_mode) || held.st_dev != named.st_dev ||
            held.st_ino != named.st_ino ||
            static_cast<std::uint64_t>(held.st_uid) != static_cast<std::uint64_t>(::geteuid()) ||
            (held.st_mode & static_cast<mode_t>(07777)) != private_mode ||
            identity_ != std::array<std::uint64_t, 3>{
                             static_cast<std::uint64_t>(held.st_dev),
                             static_cast<std::uint64_t>(held.st_ino),
                             0,
                         }) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
    }

    void require_stable() const {
        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor_, &held) != 0 || ::lstat(path_.c_str(), &named) != 0) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
        }
        if (!S_ISDIR(held.st_mode) || !S_ISDIR(named.st_mode) || held.st_dev != named.st_dev ||
            held.st_ino != named.st_ino ||
            identity_ != std::array<std::uint64_t, 3>{
                             static_cast<std::uint64_t>(held.st_dev),
                             static_cast<std::uint64_t>(held.st_ino),
                             0,
                         }) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
    }

    /// Rebind the same held directory handle after one verified no-replace
    /// rename. The old name must already be absent, and the new name must name
    /// the exact held inode before the handle accepts the successor path.
    void rebind_after_rename(const std::filesystem::path& old_path,
                             const std::filesystem::path& new_path) {
        if (path_ != old_path) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        struct stat held{};
        struct stat old_named{};
        struct stat new_named{};
        int old_result = -1;
        do {
            old_result = ::lstat(old_path.c_str(), &old_named);
        } while (old_result != 0 && errno == EINTR);
        const int old_error = old_result == 0 ? 0 : errno;
        if (::fstat(descriptor_, &held) != 0 || old_result == 0 || old_error != ENOENT ||
            ::lstat(new_path.c_str(), &new_named) != 0 || !S_ISDIR(held.st_mode) ||
            !S_ISDIR(new_named.st_mode) || held.st_dev != new_named.st_dev ||
            held.st_ino != new_named.st_ino ||
            identity_ != std::array<std::uint64_t, 3>{
                             static_cast<std::uint64_t>(held.st_dev),
                             static_cast<std::uint64_t>(held.st_ino),
                             0,
                         }) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        path_ = new_path;
        require_stable();
    }

private:
    int descriptor_ = -1;
    std::filesystem::path path_;
    std::array<std::uint64_t, 3> identity_{};
};
#endif

struct PrivateLeaseDirectoryEntries final {
    bool owner = false;
    bool owner_pending = false;
};

struct PrivateLeasePreactiveEntries final {
    bool owner = false;
    bool index = false;
    bool data = false;
};

[[nodiscard]] inline PrivateLeaseDirectoryEntries
inspect_private_lease_control_entries(const std::filesystem::path& directory_path) {
    PrivateLeaseDirectoryEntries entries;
    std::error_code error;
    std::filesystem::directory_iterator cursor(directory_path, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    for (const auto& entry : cursor) {
        const auto leaf = entry.path().filename();
        if (path_leaf_equals_ascii(leaf, ".gnfs-private-lease-v1.owner")) {
            if (entries.owner) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.owner = true;
            continue;
        }
        if (path_leaf_equals_ascii(leaf, ".gnfs-private-lease-v1.owner.pending")) {
            if (entries.owner_pending) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.owner_pending = true;
            continue;
        }
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    return entries;
}

[[nodiscard]] inline PrivateLeasePreactiveEntries
inspect_private_lease_preactive_entries(const std::filesystem::path& directory_path,
                                        const OOCCleanupPaths& paths) {
    PrivateLeasePreactiveEntries entries;
    std::error_code error;
    std::filesystem::directory_iterator cursor(directory_path, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    for (const auto& entry : cursor) {
        const auto leaf = entry.path().filename();
        if (path_leaf_equals_ascii(leaf, ".gnfs-private-lease-v1.owner")) {
            if (entries.owner) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.owner = true;
            continue;
        }
        if (path_leaf_equals(leaf, paths.index_path.filename())) {
            if (entries.index) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.index = true;
            continue;
        }
        if (path_leaf_equals(leaf, paths.data_path.filename())) {
            if (entries.data) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.data = true;
            continue;
        }
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    return entries;
}

/// Reject foreign children before a canonical pair transaction performs its
/// first rename. The transaction itself validates the type, identity, and
/// contents of every allowed protocol leaf; this directory-wide pass closes
/// the earlier gap where an unknown sibling was discovered only after the
/// owned pair had already been deleted.
inline void inspect_private_lease_transaction_entries(const std::filesystem::path& directory_path,
                                                      const OOCCleanupPaths& paths) {
    std::array<bool, 9> seen{};
    const std::array<const std::filesystem::path*, 8> pair_leaves{
        &paths.index_path,
        &paths.data_path,
        &paths.intent_path,
        &paths.intent_pending_path,
        &paths.staged_path,
        &paths.staged_pending_path,
        &paths.quarantine_index_path,
        &paths.quarantine_data_path,
    };

    std::error_code error;
    std::filesystem::directory_iterator cursor(directory_path, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    for (const auto& entry : cursor) {
        const auto leaf = entry.path().filename();
        size_t slot = seen.size();
        if (path_leaf_equals_ascii(leaf, ".gnfs-private-lease-v1.owner")) {
            slot = 0;
        } else {
            for (size_t index = 0; index < pair_leaves.size(); ++index) {
                if (path_leaf_equals(leaf, pair_leaves[index]->filename())) {
                    slot = index + 1;
                    break;
                }
            }
        }
        if (slot >= seen.size() || seen[slot]) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        seen[slot] = true;
    }
}

inline void create_directory_durable_locked(const std::filesystem::path& directory_path) {
    if (inspect_directory_identity_locked(directory_path)) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
#ifdef _WIN32
    std::error_code error;
    const bool created = std::filesystem::create_directory(directory_path, error);
    if (error || !created) {
        fail(error == std::errc::file_exists || !created ? OOCCleanupStatus::NamespaceConflict
                                                         : OOCCleanupStatus::IoFailure,
             OOCCleanupStage::None, error ? error : protocol_error());
    }
#else
    int result = -1;
    do {
        result = ::mkdir(directory_path.c_str(), 0700);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const int saved_errno = errno;
        fail(saved_errno == EEXIST ? OOCCleanupStatus::NamespaceConflict
                                   : OOCCleanupStatus::IoFailure,
             OOCCleanupStage::None, posix_error(saved_errno));
    }
#endif
    sync_parent_directory(directory_path.parent_path(), OOCCleanupStage::None);
    if (!inspect_directory_identity_locked(directory_path)) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
}

inline void
remove_empty_directory_durable_locked(const std::filesystem::path& directory_path,
                                      const std::array<std::uint64_t, 3>& expected_identity) {
    const auto current = inspect_directory_identity_locked(directory_path);
    if (!current) {
        sync_parent_directory(directory_path.parent_path(), OOCCleanupStage::None);
        return;
    }
    if (*current != expected_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    std::error_code error;
    std::filesystem::directory_iterator cursor(directory_path, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    if (cursor != std::filesystem::directory_iterator{}) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    const bool removed = std::filesystem::remove(directory_path, error);
    if (error) {
        fail(error == std::errc::directory_not_empty ? OOCCleanupStatus::NamespaceConflict
                                                     : OOCCleanupStatus::IoFailure,
             OOCCleanupStage::None, error);
    }
    if (!removed) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    sync_parent_directory(directory_path.parent_path(), OOCCleanupStage::None);
    if (inspect_directory_identity_locked(directory_path)) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
}

[[nodiscard]] inline PrivateLeaseRecord
make_private_lease_reserved_record(const OOCCleanupPaths& paths,
                                   const std::array<std::uint64_t, 2>& lease_id,
                                   const std::array<std::uint64_t, 3>& parent_identity,
                                   const std::array<std::uint64_t, 3>& lock_identity) {
    return PrivateLeaseRecord{
        .platform_id = PLATFORM_ID,
        .phase = PrivateLeasePhase::Reserved,
        .capability = PrivateLeaseCapability::RollbackStagingOnly,
        .lease_id = lease_id,
        .base_path_digest = frozen_path_digest(paths.base_path),
        .parent_identity = parent_identity,
        .lock_identity = lock_identity,
    };
}

[[nodiscard]] inline PrivateLeaseRecord
make_private_lease_owner_record(const PrivateLeaseRecord& reserved,
                                const std::array<std::uint64_t, 3>& directory_identity) {
    auto owner = reserved;
    owner.phase = PrivateLeasePhase::Owner;
    owner.capability = PrivateLeaseCapability::RemoveOwnerAndEmptyLease;
    owner.directory_identity = directory_identity;
    owner.reserved_digest = digest_words(serialize_private_lease_marker(reserved));
    return owner;
}

[[nodiscard]] inline PrivateLeaseRecord
make_private_lease_owned_record(const PrivateLeaseRecord& owner,
                                const std::array<std::uint64_t, 3>& owner_identity) {
    auto owned = owner;
    owned.phase = PrivateLeasePhase::Owned;
    owned.capability = PrivateLeaseCapability::RollbackPreactivePairAndLease;
    owned.owner_identity = owner_identity;
    return owned;
}

inline void validate_private_lease_record_chain(const PrivateLeaseRecord& reserved,
                                                const PrivateLeaseRecord& owned) {
    if (reserved.phase != PrivateLeasePhase::Reserved || owned.phase != PrivateLeasePhase::Owned ||
        owned.lease_id != reserved.lease_id ||
        owned.base_path_digest != reserved.base_path_digest ||
        owned.parent_identity != reserved.parent_identity ||
        owned.lock_identity != reserved.lock_identity ||
        owned.reserved_digest != digest_words(serialize_private_lease_marker(reserved))) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
}

[[nodiscard]] inline PrivateLeaseRecord owner_record_for(const PrivateLeaseRecord& owned) {
    auto owner = owned;
    owner.phase = PrivateLeasePhase::Owner;
    owner.capability = PrivateLeaseCapability::RemoveOwnerAndEmptyLease;
    owner.owner_identity = {};
    return owner;
}

inline void remove_owner_marker_durable_locked(const std::filesystem::path& directory_path,
                                               const PrivateLeaseRecord& owned,
                                               bool allow_missing) {
    const auto owner_path = private_lease_owner_path(directory_path);
    const auto expected_owner = owner_record_for(owned);
    const auto owner = inspect_private_lease_marker(owner_path);
    if (owner.kind == InspectKind::Missing) {
        if (!allow_missing) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
    } else {
        if (owner.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, owner.error);
        }
        if (owner.kind == InspectKind::Rejected ||
            parse_private_lease_marker(owner.bytes) != expected_owner ||
            stable_identity(owner.identity) != owned.owner_identity) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        remove_file(owner_path, OOCCleanupStage::None);
        sync_parent_directory(directory_path, OOCCleanupStage::None);
    }
    const auto pending_path = private_lease_owner_pending_path(directory_path);
    const auto pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Rejected ||
        (pending.kind == InspectKind::Present &&
         !marker_bytes_equal(pending.bytes, serialize_private_lease_marker(expected_owner)))) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    if (pending.kind == InspectKind::Present) {
        remove_file(pending_path, OOCCleanupStage::None);
        sync_parent_directory(directory_path, OOCCleanupStage::None);
    }
}

struct LoadedPrivateLeaseMarker final {
    PrivateLeaseRecord record;
    std::array<std::uint64_t, 3> identity{};

    friend bool operator==(const LoadedPrivateLeaseMarker&,
                           const LoadedPrivateLeaseMarker&) = default;
};

[[nodiscard]] inline std::optional<LoadedPrivateLeaseMarker>
load_optional_private_lease_marker(const std::filesystem::path& path) {
    const auto inspected = inspect_private_lease_marker(path);
    if (inspected.kind == InspectKind::Missing) {
        return std::nullopt;
    }
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    return LoadedPrivateLeaseMarker{
        .record = parse_private_lease_marker(inspected.bytes),
        .identity = stable_identity(inspected.identity),
    };
}

inline void remove_matching_private_lease_pending(const std::filesystem::path& pending_path,
                                                  const PrivateLeaseRecord& expected) {
    const auto pending = inspect_pending_file(pending_path, PRIVATE_LEASE_MARKER_BYTES);
    if (pending.kind == InspectKind::Missing) {
        return;
    }
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Rejected ||
        !marker_bytes_equal(pending.bytes, serialize_private_lease_marker(expected))) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    remove_file(pending_path, OOCCleanupStage::None);
    sync_parent_directory(pending_path.parent_path(), OOCCleanupStage::None);
}

[[nodiscard]] inline OOCCleanupResult private_lease_completed() noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Completed,
        .stage = OOCCleanupStage::Completed,
        .native_error = {},
    };
}

[[nodiscard]] inline OOCCleanupResult private_lease_no_transaction() noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::NoTransaction,
        .stage = OOCCleanupStage::None,
        .native_error = {},
    };
}

[[nodiscard]] inline OOCCleanupResult private_lease_interrupted() noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Interrupted,
        .stage = OOCCleanupStage::None,
        .native_error = {},
    };
}

inline void validate_private_lease_owner_at(const std::filesystem::path& directory_path,
                                            const PrivateLeaseRecord& owned) {
    if (capture_directory_identity_locked(directory_path) != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(private_lease_owner_path(directory_path), owner_record_for(owned),
                                 owned.owner_identity);
}

[[nodiscard]] inline util::durable_immutable_record::NativeIdentity
handoff_native_identity(const std::array<std::uint64_t, 3>& identity) noexcept {
    return {
        .first = identity[0],
        .second = identity[1],
        .third = identity[2],
    };
}

[[nodiscard]] inline util::durable_immutable_record::NativeIdentity
handoff_native_identity(const FileIdentity& identity) noexcept {
    return {
        .first = identity.first,
        .second = identity.second,
        .third = identity.third,
    };
}

[[nodiscard]] inline OOCPrivateHandoffInspectResult handoff_none() noexcept {
    return {
        .result =
            {
                .status = OOCCleanupStatus::NoTransaction,
                .stage = OOCCleanupStage::None,
                .native_error = {},
            },
        .state = OOCPrivateHandoffState::None,
        .record = std::nullopt,
        .identity = std::nullopt,
    };
}

[[nodiscard]] inline OOCPrivateHandoffInspectResult
handoff_failure(OOCCleanupStatus status, OOCPrivateHandoffState state,
                std::error_code error = protocol_error()) noexcept {
    return {
        .result =
            {
                .status = status,
                .stage = OOCCleanupStage::None,
                .native_error = error,
            },
        .state = state,
        .record = std::nullopt,
        .identity = std::nullopt,
    };
}

[[nodiscard]] inline OOCPrivateHandoffInspectResult
handoff_pending(const OOCPrivateHandoffRecordV1& record,
                const util::durable_immutable_record::RecordSnapshot& snapshot) {
    return {
        .result =
            {
                .status = OOCCleanupStatus::RecoveryRequired,
                .stage = OOCCleanupStage::None,
                .native_error = protocol_error(),
            },
        .state = OOCPrivateHandoffState::PendingOnly,
        .record = record,
        .identity = snapshot.identity,
    };
}

[[nodiscard]] inline OOCPrivateHandoffInspectResult
handoff_present(const OOCPrivateHandoffRecordV1& record,
                const util::durable_immutable_record::RecordSnapshot& snapshot) {
    return {
        .result =
            {
                .status = OOCCleanupStatus::HandoffPresent,
                .stage = OOCCleanupStage::None,
                .native_error = {},
            },
        .state = OOCPrivateHandoffState::Canonical,
        .record = record,
        .identity = snapshot.identity,
    };
}

enum class PrivateHandoffLeafState : std::uint8_t {
    Missing,
    Exact,
    Rejected,
};

struct LoadedPrivateHandoffLeaf final {
    PrivateHandoffLeafState state = PrivateHandoffLeafState::Rejected;
    std::vector<std::byte> bytes;
    std::optional<OOCPrivateHandoffRecordV1> record;
    std::optional<util::durable_immutable_record::RecordSnapshot> snapshot;
};

[[nodiscard]] inline LoadedPrivateHandoffLeaf
read_private_handoff_leaf(util::durable_immutable_record::NativeHandle directory_handle,
                          const std::filesystem::path& leaf) {
    using namespace util::durable_immutable_record;
    const auto read =
        read_bounded_at(directory_handle, leaf, OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1,
                        OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES);
    switch (read.state()) {
    case BoundedReadState::missing:
        return {.state = PrivateHandoffLeafState::Missing};
    case BoundedReadState::exact:
        break;
    case BoundedReadState::rejected:
        return {.state = PrivateHandoffLeafState::Rejected};
    case BoundedReadState::unsupported:
        fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None, read.native_error());
    case BoundedReadState::interrupted:
    case BoundedReadState::failed:
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, read.native_error());
    }

    if (!read.bytes() || !read.snapshot()) {
        return {.state = PrivateHandoffLeafState::Rejected};
    }
    const auto decoded = decode_ooc_private_handoff_record(*read.bytes());
    if (!decoded || !decoded.value || !validate_ooc_private_handoff_record(*decoded.value, true)) {
        return {.state = PrivateHandoffLeafState::Rejected};
    }
    return {
        .state = PrivateHandoffLeafState::Exact,
        .bytes = *read.bytes(),
        .record = *decoded.value,
        .snapshot = *read.snapshot(),
    };
}

[[nodiscard]] inline bool handoff_pair_matches_context(const OOCPrivateHandoffRecordV1& record,
                                                       const OOCCleanupPaths& paths) {
    try {
        const auto source = capture_source_pair(paths, record.pair.store_id);
        if (record.pair.format_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
            record.pair.generation == 0 ||
            source.index.header_magic != OOCRelationStoreFormat::MAGIC_V3_FINAL ||
            source.index.header_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
            source.index.header_count != record.pair.count ||
            source.data.header_magic != OOCRelationStoreFormat::MAGIC_V3_DATA ||
            source.data.header_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
            source.index.identity.size != record.pair.index_extent ||
            source.data.identity.size != record.pair.data_extent ||
            record.index.extent != record.pair.index_extent ||
            record.data.extent != record.pair.data_extent ||
            record.index.identity != handoff_native_identity(source.index.identity) ||
            record.data.identity != handoff_native_identity(source.data.identity)) {
            return false;
        }
        require_source_pair_unchanged(paths, source);
        return true;
    } catch (const Failure& failure) {
        if (failure.status == OOCCleanupStatus::IoFailure ||
            failure.status == OOCCleanupStatus::DurabilityFailure ||
            failure.status == OOCCleanupStatus::PlatformUnsupported) {
            throw;
        }
        return false;
    }
}

[[nodiscard]] inline bool handoff_record_matches_context(
    const OOCPrivateHandoffRecordV1& record, const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 3>& directory_identity,
    const std::optional<LoadedPrivateLeaseMarker>& reserved,
    const std::optional<LoadedPrivateLeaseMarker>& owned, bool require_reserved) {
    if (!owned || (require_reserved && !reserved) || record.lease_id != owned->record.lease_id ||
        record.lock_identity != handoff_native_identity(lock.identity()) ||
        record.directory_identity != handoff_native_identity(directory_identity) ||
        record.owner_marker_identity != handoff_native_identity(owned->record.owner_identity) ||
        record.owned_marker_identity != handoff_native_identity(owned->identity) ||
        owned->record.directory_identity != directory_identity) {
        return false;
    }
    try {
        const auto parent_identity =
            capture_directory_identity_locked(paths.private_directory.parent_path());
        validate_private_lease_record_context(owned->record, paths, parent_identity,
                                              lock.identity());
        if (owned->record.phase != PrivateLeasePhase::Owned) {
            return false;
        }
        if (reserved) {
            validate_private_lease_record_context(reserved->record, paths, parent_identity,
                                                  lock.identity());
            validate_private_lease_record_chain(reserved->record, owned->record);
            if (record.lease_id != reserved->record.lease_id) {
                return false;
            }
        }
        validate_private_lease_owner_at(paths.private_directory, owned->record);
        return handoff_pair_matches_context(record, paths);
    } catch (const Failure& failure) {
        if (failure.status == OOCCleanupStatus::IoFailure ||
            failure.status == OOCCleanupStatus::DurabilityFailure ||
            failure.status == OOCCleanupStatus::PlatformUnsupported) {
            throw;
        }
        return false;
    }
}

#ifndef _WIN32
inline void
remove_exact_private_handoff_pending(PrivateDirectoryHandle& directory,
                                     const util::durable_immutable_record::RecordSnapshot& expected,
                                     const std::filesystem::path& leaf) {
    struct stat before{};
    int result = -1;
    do {
        result = ::fstatat(static_cast<int>(directory.native_handle()), leaf.c_str(), &before,
                           AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    if (result != 0 || !S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) != expected.size ||
        static_cast<std::uint64_t>(before.st_uid) != static_cast<std::uint64_t>(::geteuid()) ||
        (before.st_mode & static_cast<mode_t>(07777)) != (S_IRUSR | S_IWUSR) ||
        util::durable_immutable_record::NativeIdentity{
            .first = static_cast<std::uint64_t>(before.st_dev),
            .second = static_cast<std::uint64_t>(before.st_ino),
            .third = 0,
        } != expected.identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    do {
        result = ::unlinkat(static_cast<int>(directory.native_handle()), leaf.c_str(), 0);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             posix_error(errno));
    }
    do {
#if defined(__APPLE__)
        result = ::fcntl(static_cast<int>(directory.native_handle()), F_FULLFSYNC);
#else
        result = ::fsync(static_cast<int>(directory.native_handle()));
#endif
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        fail(OOCCleanupStatus::DurabilityFailure, OOCCleanupStage::None, posix_error(errno));
    }
    do {
        result = ::fstatat(static_cast<int>(directory.native_handle()), leaf.c_str(), &before,
                           AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    if (result == 0 || errno != ENOENT) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             result == 0 ? protocol_error() : posix_error(errno));
    }
    directory.require_stable();
}
#endif

struct PrivateHandoffDirectoryEntries final {
    bool valid = true;
    bool legacy_authority = false;
};

[[nodiscard]] inline PrivateHandoffDirectoryEntries
inspect_private_handoff_directory_entries(const OOCCleanupPaths& paths) {
    const std::array<std::filesystem::path, 11> allowed{
        std::filesystem::path(".gnfs-private-lease-v1.owner"),
        paths.index_path.filename(),
        paths.data_path.filename(),
        paths.private_handoff_path.filename(),
        paths.private_handoff_pending_path.filename(),
        paths.intent_path.filename(),
        paths.intent_pending_path.filename(),
        paths.staged_path.filename(),
        paths.staged_pending_path.filename(),
        paths.quarantine_index_path.filename(),
        paths.quarantine_data_path.filename(),
    };
    std::array<bool, allowed.size()> seen{};
    std::error_code error;
    std::filesystem::directory_iterator cursor(paths.private_directory, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    PrivateHandoffDirectoryEntries result;
    for (const auto& entry : cursor) {
        const auto leaf = entry.path().filename();
        std::size_t slot = allowed.size();
        for (std::size_t index = 0; index < allowed.size(); ++index) {
            if (path_leaf_equals(leaf, allowed[index])) {
                if (leaf.native() != allowed[index].native()) {
                    result.valid = false;
                    return result;
                }
                slot = index;
                break;
            }
        }
        if (slot == allowed.size() || seen[slot]) {
            result.valid = false;
            return result;
        }
        seen[slot] = true;
        if (slot >= 5) {
            result.legacy_authority = true;
        }
    }
    return result;
}

#ifndef _WIN32
struct PrivateHandoffLeafClassification final {
    OOCPrivateHandoffInspectResult inspection;
    /// True after the canonical record independently passed its complete
    /// lease, directory, and pair context, even if a conflicting pending leaf
    /// makes the aggregate state tainted.
    bool canonical_context_valid = false;
    bool pending_is_preactive = false;
};

/// Classify two already-read handoff leaves against the lease and pair
/// context. The caller owns the directory handle, inventory, and leaf reads;
/// this helper performs no handoff reconciliation or publication.
[[nodiscard]] inline PrivateHandoffLeafClassification classify_private_handoff_leaves_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 3>& directory_identity,
    const LoadedPrivateHandoffLeaf& canonical, const LoadedPrivateHandoffLeaf& pending,
    std::optional<LoadedPrivateLeaseMarker>* reserved_witness = nullptr,
    std::optional<LoadedPrivateLeaseMarker>* owned_witness = nullptr) {
    if (canonical.state == PrivateHandoffLeafState::Rejected ||
        pending.state == PrivateHandoffLeafState::Rejected) {
        return {
            .inspection = handoff_failure(OOCCleanupStatus::ForeignReplacementPreserved,
                                          OOCPrivateHandoffState::TaintedPreserved),
        };
    }
    if (canonical.state == PrivateHandoffLeafState::Missing &&
        pending.state == PrivateHandoffLeafState::Missing) {
        return {
            .inspection = handoff_none(),
        };
    }

    const auto reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
    const auto owned = load_optional_private_lease_marker(paths.lease_owned_path);
    if (reserved_witness != nullptr) {
        *reserved_witness = reserved;
    }
    if (owned_witness != nullptr) {
        *owned_witness = owned;
    }
    if (canonical.state == PrivateHandoffLeafState::Exact) {
        if (!canonical.record || !canonical.snapshot ||
            !handoff_record_matches_context(*canonical.record, paths, lock, directory_identity,
                                            reserved, owned, false)) {
            return {
                .inspection = handoff_failure(OOCCleanupStatus::ForeignReplacementPreserved,
                                              OOCPrivateHandoffState::TaintedPreserved),
            };
        }
        if (pending.state == PrivateHandoffLeafState::Exact &&
            (!pending.record || !pending.snapshot || pending.bytes != canonical.bytes)) {
            return {
                .inspection = handoff_failure(OOCCleanupStatus::ForeignReplacementPreserved,
                                              OOCPrivateHandoffState::TaintedPreserved),
                .canonical_context_valid = true,
            };
        }
        return {
            .inspection = handoff_present(*canonical.record, *canonical.snapshot),
            .canonical_context_valid = true,
        };
    }

    if (pending.state == PrivateHandoffLeafState::Exact) {
        if (!pending.record || !pending.snapshot) {
            return {
                .inspection = handoff_failure(OOCCleanupStatus::ForeignReplacementPreserved,
                                              OOCPrivateHandoffState::TaintedPreserved),
            };
        }
        if (!reserved) {
            return {
                .inspection = handoff_pending(*pending.record, *pending.snapshot),
            };
        }
        if (!handoff_record_matches_context(*pending.record, paths, lock, directory_identity,
                                            reserved, owned, true)) {
            return {
                .inspection = handoff_failure(OOCCleanupStatus::ForeignReplacementPreserved,
                                              OOCPrivateHandoffState::TaintedPreserved),
            };
        }
        return {
            .inspection = handoff_pending(*pending.record, *pending.snapshot),
            .pending_is_preactive = true,
        };
    }
    return {
        .inspection = handoff_none(),
    };
}
#endif

/// Move-only result from one pure generic-handoff namespace observation.
/// Exact POSIX witnesses retain the same no-follow directory handle so the
/// separate legacy transition can consume only what this observation proved.
struct PrivateHandoffObservationWitness final {
    OOCPrivateHandoffInspectResult inspection;
#ifndef _WIN32
    std::unique_ptr<PrivateDirectoryHandle> directory;
    std::optional<LoadedPrivateHandoffLeaf> canonical;
    std::optional<LoadedPrivateHandoffLeaf> pending;
    bool pending_is_preactive = false;
#endif
};

/// Pure reusable classifier for every generic-handoff-aware cleanup or lease
/// recovery entry point. It is production-reader-only: the injectable durable
/// record seam is deliberately unavailable here, and it never publishes,
/// removes, or rewrites a namespace leaf.
[[nodiscard]] inline PrivateHandoffObservationWitness
observe_private_handoff_locked(const OOCCleanupPaths& paths, const BaseLock& lock,
                               bool allow_cleanup_markers = false) {
    if (paths.private_directory.empty() || !lock.matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    lock.require_stable();
    const auto finish = [&lock](PrivateHandoffObservationWitness result) {
        lock.require_stable();
        return result;
    };
    const auto directory_identity = inspect_directory_identity_locked(paths.private_directory);
    if (!directory_identity) {
        return finish({.inspection = handoff_none()});
    }

#ifdef _WIN32
    const auto canonical = inspect_file(paths.private_handoff_path, 0, false);
    const auto pending = inspect_file(paths.private_handoff_pending_path, 0, false);
    if (canonical.kind == InspectKind::Error || pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
             canonical.kind == InspectKind::Error ? canonical.error : pending.error);
    }
    if (canonical.kind == InspectKind::Missing && pending.kind == InspectKind::Missing) {
        return finish({.inspection = handoff_none()});
    }
    return finish({
        .inspection = handoff_failure(OOCCleanupStatus::PlatformUnsupported,
                                      OOCPrivateHandoffState::TaintedPreserved,
                                      std::make_error_code(std::errc::operation_not_supported)),
    });
#else
    auto directory = std::make_unique<PrivateDirectoryHandle>(paths.private_directory);
    if (directory->identity() != *directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    const auto canonical_leaf = paths.private_handoff_path.filename();
    const auto pending_leaf = paths.private_handoff_pending_path.filename();
    const bool canonical_exists = directory->leaf_exists(canonical_leaf);
    const bool pending_exists = directory->leaf_exists(pending_leaf);
    if (!canonical_exists && !pending_exists) {
        return finish({.inspection = handoff_none()});
    }
    directory->require_private_policy();

    auto canonical = read_private_handoff_leaf(directory->native_handle(), canonical_leaf);
    auto pending = read_private_handoff_leaf(directory->native_handle(), pending_leaf);
    directory->require_private_policy();
    if (canonical.state == PrivateHandoffLeafState::Rejected ||
        pending.state == PrivateHandoffLeafState::Rejected) {
        return finish({
            .inspection = handoff_failure(OOCCleanupStatus::ForeignReplacementPreserved,
                                          OOCPrivateHandoffState::TaintedPreserved),
        });
    }
    const auto entries = inspect_private_handoff_directory_entries(paths);
    directory->require_private_policy();
    if (!entries.valid) {
        return finish({
            .inspection = handoff_failure(OOCCleanupStatus::ForeignReplacementPreserved,
                                          OOCPrivateHandoffState::TaintedPreserved),
        });
    }
    if (entries.legacy_authority && !allow_cleanup_markers) {
        return finish({
            .inspection = handoff_failure(OOCCleanupStatus::NamespaceConflict,
                                          OOCPrivateHandoffState::TaintedPreserved),
        });
    }

    const auto classified = classify_private_handoff_leaves_locked(paths, lock, *directory_identity,
                                                                   canonical, pending);
    directory->require_private_policy();
    if (classified.inspection.state == OOCPrivateHandoffState::Canonical) {
        return finish({
            .inspection = classified.inspection,
            .directory = std::move(directory),
            .canonical = std::move(canonical),
            .pending = pending.state == PrivateHandoffLeafState::Exact
                           ? std::optional<LoadedPrivateHandoffLeaf>(std::move(pending))
                           : std::nullopt,
        });
    }

    if (classified.inspection.state == OOCPrivateHandoffState::PendingOnly) {
        return finish({
            .inspection = classified.inspection,
            .directory = std::move(directory),
            .pending = std::move(pending),
            .pending_is_preactive = classified.pending_is_preactive,
        });
    }
    return finish({.inspection = classified.inspection});
#endif
}

/// Authority-free projection used by every observation-only caller.
[[nodiscard]] inline OOCPrivateHandoffInspectResult
classify_private_handoff_locked(const OOCCleanupPaths& paths, const BaseLock& lock) {
    if (!paths.private_handoff_rollback_path.empty()) {
        const auto rollback = inspect_file(paths.private_handoff_rollback_path, 0, false);
        if (rollback.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, rollback.error);
        }
        if (rollback.kind != InspectKind::Missing) {
            return handoff_failure(OOCCleanupStatus::NamespaceConflict,
                                   OOCPrivateHandoffState::TaintedPreserved, protocol_error());
        }
    }
    auto observed = observe_private_handoff_locked(paths, lock);
    return std::move(observed.inspection);
}

[[nodiscard]] inline bool
private_lease_staging_leaf_matches(const OOCCleanupPaths& paths,
                                   const std::filesystem::path& candidate) {
    auto prefix = paths.private_directory.filename().native();
    prefix.append(std::filesystem::path(".gnfs-private-lease-v1.stage-").native());
    const auto& native = candidate.native();
    constexpr std::size_t generation_hex_digits = 32;
    if (native.size() != prefix.size() + generation_hex_digits ||
        native.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    for (std::size_t index = prefix.size(); index < native.size(); ++index) {
        const auto value = native[index];
        const auto zero = static_cast<std::filesystem::path::value_type>('0');
        const auto nine = static_cast<std::filesystem::path::value_type>('9');
        const auto lower_a = static_cast<std::filesystem::path::value_type>('a');
        const auto lower_f = static_cast<std::filesystem::path::value_type>('f');
        if (!((value >= zero && value <= nine) || (value >= lower_a && value <= lower_f))) {
            return false;
        }
    }
    return true;
}

/// Scan the lock parent without following it or any child. Merely naming an
/// exact fixed protocol entry or generation-specific staging entry is enough:
/// symlink/reparse and malformed object types must block lock creation too.
[[nodiscard]] inline bool
private_protocol_artifact_exists_without_lock(const OOCCleanupPaths& paths) {
    const auto parent = paths.private_directory.parent_path();
    const auto parent_identity = inspect_directory_identity_locked(parent);
    if (!parent_identity) {
        return false;
    }
    const std::array<std::filesystem::path, 6> fixed_leaves{
        paths.private_directory.filename(),
        paths.lease_reserved_path.filename(),
        paths.lease_reserved_pending_path.filename(),
        paths.lease_owned_path.filename(),
        paths.lease_owned_pending_path.filename(),
        paths.private_handoff_rollback_path.filename(),
    };

    bool found = false;
    std::error_code error;
    std::filesystem::directory_iterator cursor(parent, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    const std::filesystem::directory_iterator end;
    while (cursor != end) {
        const auto leaf = cursor->path().filename();
        found = found ||
                std::any_of(fixed_leaves.begin(), fixed_leaves.end(),
                            [&](const auto& fixed) { return path_leaf_equals(leaf, fixed); }) ||
                private_lease_staging_leaf_matches(paths, leaf);
        cursor.increment(error);
        if (error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
        }
    }
    if (capture_directory_identity_locked(parent) != *parent_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    return found;
}

struct PrivateHandoffRecordHookBridge final {
    OOCPrivateHandoffTestHooks hooks;
    const BaseLock* lock = nullptr;
    std::optional<OOCPrivateHandoffFaultPoint> stopped_at;
};

[[nodiscard]] inline bool
bridge_private_handoff_fault(util::durable_immutable_record::RecordFaultPoint point,
                             void* context) noexcept {
    auto& bridge = *static_cast<PrivateHandoffRecordHookBridge*>(context);
    OOCPrivateHandoffFaultPoint mapped = OOCPrivateHandoffFaultPoint::PendingDurable;
    switch (point) {
    case util::durable_immutable_record::RecordFaultPoint::PendingDurable:
        mapped = OOCPrivateHandoffFaultPoint::PendingDurable;
        break;
    case util::durable_immutable_record::RecordFaultPoint::CanonicalPromoted:
        mapped = OOCPrivateHandoffFaultPoint::CanonicalPromoted;
        break;
    case util::durable_immutable_record::RecordFaultPoint::CanonicalDurable:
        mapped = OOCPrivateHandoffFaultPoint::CanonicalDurable;
        break;
    }
    const bool requested_stop =
        bridge.hooks.stop_after != nullptr && bridge.hooks.stop_after(mapped, bridge.hooks.context);
    if (bridge.lock != nullptr && !bridge.lock->stable_noexcept()) {
        return true;
    }
    if (requested_stop) {
        bridge.stopped_at = mapped;
        return true;
    }
    return false;
}

[[nodiscard]] inline OOCCleanupStatus
handoff_publish_status(util::durable_immutable_record::RecordPublishStatus status) noexcept {
    using Status = util::durable_immutable_record::RecordPublishStatus;
    switch (status) {
    case Status::durable:
        return OOCCleanupStatus::HandoffPresent;
    case Status::interrupted:
        return OOCCleanupStatus::Interrupted;
    case Status::invalid_request:
    case Status::input_too_large:
        return OOCCleanupStatus::InvalidRequest;
    case Status::platform_unsupported:
        return OOCCleanupStatus::PlatformUnsupported;
    case Status::pending_conflict:
    case Status::canonical_conflict:
    case Status::ops_contract_violation:
        return OOCCleanupStatus::ForeignReplacementPreserved;
    case Status::parent_sync_failed:
    case Status::canonical_confirm_failed:
    case Status::pending_cleanup_failed:
        return OOCCleanupStatus::DurabilityFailure;
    case Status::pending_publish_failed:
    case Status::promotion_failed:
        return OOCCleanupStatus::IoFailure;
    case Status::unexpected_failure:
        return OOCCleanupStatus::UnexpectedFailure;
    }
    return OOCCleanupStatus::UnexpectedFailure;
}

[[nodiscard]] inline bool remove_preactive_pair_leaf_durable_locked(
    const std::filesystem::path& path, const std::filesystem::path& directory_path,
    OOCPrivateLeaseFaultPoint fault_point, const OOCPrivateLeaseTestHooks& hooks) {
    const auto before = inspect_file(path, 0, false);
    if (before.kind == InspectKind::Missing) {
        sync_parent_directory(directory_path, OOCCleanupStage::None);
        return false;
    }
    if (before.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, before.error);
    }
    if (before.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    // Establish and then recheck the exact regular single-link target before
    // the path-based unlink. The surrounding directory identity and external
    // BaseLock remain fixed for the entire operation.
    confirm_file_durable(path, before.identity, directory_path, OOCCleanupStage::None);
    const auto confirmed = inspect_file(path, 0, false);
    if (confirmed.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, confirmed.error);
    }
    if (confirmed.kind != InspectKind::Present ||
        !same_native_file(confirmed.identity, before.identity)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    remove_file(path, OOCCleanupStage::None);
    sync_parent_directory(directory_path, OOCCleanupStage::None);
    const auto after = inspect_file(path, 0, false);
    if (after.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, after.error);
    }
    if (after.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    return should_interrupt_private_lease(hooks, fault_point);
}

inline void validate_preactive_pair_leaf_before_quarantine(const std::filesystem::path& path,
                                                           bool expected_present) {
    const auto inspected = inspect_file(path, 0, false);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (expected_present) {
        if (inspected.kind == InspectKind::Rejected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        if (inspected.kind != InspectKind::Present) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
    } else if (inspected.kind != InspectKind::Missing) {
        fail(inspected.kind == InspectKind::Rejected ? OOCCleanupStatus::ForeignReplacementPreserved
                                                     : OOCCleanupStatus::NamespaceConflict,
             OOCCleanupStage::None, protocol_error());
    }
}

/// Roll back a fresh pair only while the exact owned lease still has its
/// canonical RESERVED predecessor. The fixed directory is first moved back to
/// its generation-specific staging name, making the directory identity itself
/// the deletion capability. Only the owner marker and the two expected pair
/// leaves are accepted inside that directory.
[[nodiscard]] inline OOCCleanupResult
rollback_owned_preactive_pair_locked(const OOCCleanupPaths& paths, const PrivateLeaseRecord& owned,
                                     const BaseLock& lock, const OOCPrivateLeaseTestHooks& hooks) {
    if (owned.capability != PrivateLeaseCapability::RollbackPreactivePairAndLease) {
        fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None, protocol_error());
    }

    const auto staging_path = private_lease_staging_path(paths, owned.lease_id);
    auto staging_identity = inspect_directory_identity_locked(staging_path);
    auto final_identity = inspect_directory_identity_locked(paths.private_directory);
    if (staging_identity && final_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (staging_identity && *staging_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (final_identity && *final_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    if (final_identity) {
        const auto entries =
            inspect_private_lease_preactive_entries(paths.private_directory, paths);
        if (!entries.owner) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        validate_private_lease_owner_at(paths.private_directory, owned);
        validate_preactive_pair_leaf_before_quarantine(paths.index_path, entries.index);
        validate_preactive_pair_leaf_before_quarantine(paths.data_path, entries.data);

        const auto renamed = invoke_with_stable_base_lock(
            lock, [&] { return rename_no_replace(paths.private_directory, staging_path); });
        switch (renamed.result) {
        case RenameResult::Succeeded:
            invoke_with_stable_base_lock(lock, [&] {
                sync_parent_directory(staging_path.parent_path(), OOCCleanupStage::None);
            });
            break;
        case RenameResult::DestinationExists:
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, renamed.error);
        case RenameResult::Unsupported:
            fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None, renamed.error);
        case RenameResult::Failed:
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, renamed.error);
        }
        if (inspect_directory_identity_locked(paths.private_directory)) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        staging_identity = inspect_directory_identity_locked(staging_path);
        if (!staging_identity || *staging_identity != owned.directory_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable);
            })) {
            return private_lease_interrupted();
        }
    }

    if (staging_identity) {
        const auto entries = inspect_private_lease_preactive_entries(staging_path, paths);
        if (entries.owner) {
            validate_private_lease_owner_at(staging_path, owned);
        } else if (entries.index || entries.data) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }

        const auto staged_data = staging_path / paths.data_path.filename();
        if (entries.data && invoke_with_stable_base_lock(lock, [&] {
                return remove_preactive_pair_leaf_durable_locked(
                    staged_data, staging_path,
                    OOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable, hooks);
            })) {
            return private_lease_interrupted();
        }
        const auto staged_index = staging_path / paths.index_path.filename();
        if (entries.index && invoke_with_stable_base_lock(lock, [&] {
                return remove_preactive_pair_leaf_durable_locked(
                    staged_index, staging_path,
                    OOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable, hooks);
            })) {
            return private_lease_interrupted();
        }

        invoke_with_stable_base_lock(
            lock, [&] { remove_owner_marker_durable_locked(staging_path, owned, true); });
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::OwnerRemovedDurable);
            })) {
            return private_lease_interrupted();
        }
        invoke_with_stable_base_lock(lock, [&] {
            remove_empty_directory_durable_locked(staging_path, owned.directory_identity);
        });
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable);
            })) {
            return private_lease_interrupted();
        }
    } else {
        invoke_with_stable_base_lock(lock, [&] {
            sync_parent_directory(paths.private_directory.parent_path(), OOCCleanupStage::None);
        });
    }
    lock.require_stable();
    return private_lease_completed();
}

inline void rollback_reserved_staging_locked(const OOCCleanupPaths& paths, const BaseLock& lock,
                                             const std::array<std::uint64_t, 3>& parent_identity,
                                             const LoadedPrivateLeaseMarker& loaded_reserved) {
    const auto& reserved = loaded_reserved.record;
    validate_private_lease_record_context(reserved, paths, parent_identity, lock.identity());
    if (reserved.phase != PrivateLeasePhase::Reserved) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(paths.lease_reserved_path, reserved, loaded_reserved.identity);

    // RESERVED has authority over only its unpredictable staging leaf. It
    // never permits touching the fixed lease directory.
    if (inspect_directory_identity_locked(paths.private_directory)) {
        fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None, protocol_error());
    }

    const auto staging_path = private_lease_staging_path(paths, reserved.lease_id);
    const auto staging_identity = inspect_directory_identity_locked(staging_path);
    auto owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
    if (!staging_identity) {
        if (owned_pending) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
    } else {
        const auto entries = inspect_private_lease_control_entries(staging_path);
        const auto expected_owner = make_private_lease_owner_record(reserved, *staging_identity);
        std::optional<std::array<std::uint64_t, 3>> owner_identity;
        if (entries.owner) {
            FileIdentity identity;
            const auto owner =
                load_private_lease_marker(private_lease_owner_path(staging_path), &identity);
            if (owner != expected_owner) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            owner_identity = stable_identity(identity);
        }
        if (owned_pending) {
            if (!owner_identity) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            const auto expected_owned =
                make_private_lease_owned_record(expected_owner, *owner_identity);
            validate_private_lease_record_context(owned_pending->record, paths, parent_identity,
                                                  lock.identity());
            validate_private_lease_record_chain(reserved, owned_pending->record);
            if (owned_pending->record != expected_owned) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            invoke_with_stable_base_lock(lock, [&] {
                remove_matching_private_lease_pending(paths.lease_owned_pending_path,
                                                      expected_owned);
            });
        }
        if (entries.owner_pending) {
            invoke_with_stable_base_lock(lock, [&] {
                remove_matching_private_lease_pending(
                    private_lease_owner_pending_path(staging_path), expected_owner);
            });
        }
        if (entries.owner) {
            const auto owner_path = private_lease_owner_path(staging_path);
            confirm_private_lease_marker(owner_path, expected_owner, *owner_identity);
            invoke_with_stable_base_lock(lock,
                                         [&] { remove_file(owner_path, OOCCleanupStage::None); });
            invoke_with_stable_base_lock(
                lock, [&] { sync_parent_directory(staging_path, OOCCleanupStage::None); });
        }
        invoke_with_stable_base_lock(
            lock, [&] { remove_empty_directory_durable_locked(staging_path, *staging_identity); });
    }

    invoke_with_stable_base_lock(lock, [&] {
        remove_matching_private_lease_pending(paths.lease_reserved_pending_path, reserved);
    });
    invoke_with_stable_base_lock(lock, [&] {
        remove_private_lease_marker_durable(paths.lease_reserved_path, reserved,
                                            loaded_reserved.identity);
    });
}

/// A crash while publishing a deferred worker handoff may leave only the
/// no-authority pending intent. RESERVED still authorizes rollback of the
/// exact preactivation directory, but the pending leaf must first prove that
/// it describes the current pair and then be durably discarded. Malformed,
/// linked, or foreign pending leaves remain fail-closed.
inline void discard_matching_preactive_intent_pending_locked(const OOCCleanupPaths& paths) {
    const auto canonical = inspect_file(paths.intent_path, MARKER_BYTES, true);
    if (canonical.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, canonical.error);
    }
    if (canonical.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (canonical.kind == InspectKind::Present) {
        return;
    }

    FileIdentity pending_identity;
    const auto pending = inspect_file(paths.intent_pending_path, MARKER_BYTES, true);
    if (pending.kind == InspectKind::Missing) {
        return;
    }
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const IntentRecord expected = parse_marker(pending.bytes, INTENT_MAGIC);
    pending_identity = pending.identity;
    confirm_existing_marker(paths.intent_pending_path, expected, INTENT_MAGIC,
                            OOCCleanupStage::None);
    const IntentRecord source = capture_source_pair(paths, expected.store_id);
    if (source != expected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    require_source_pair_unchanged(paths, expected);

    const auto rechecked = inspect_file(paths.intent_pending_path, MARKER_BYTES, true);
    if (rechecked.kind != InspectKind::Present || rechecked.identity != pending_identity ||
        !marker_bytes_equal(rechecked.bytes, pending.bytes)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    remove_file(paths.intent_pending_path, OOCCleanupStage::None);
    sync_parent_directory(paths.intent_pending_path.parent_path(), OOCCleanupStage::None);
    const auto absent = inspect_file(paths.intent_pending_path, 0, false);
    if (absent.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, absent.error);
    }
    if (absent.kind != InspectKind::Missing) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    require_source_pair_unchanged(paths, expected);
}

[[nodiscard]] inline OOCCleanupResult
recover_owned_private_lease_locked(const OOCCleanupPaths& paths, const BaseLock& lock,
                                   const std::array<std::uint64_t, 3>& parent_identity,
                                   const LoadedPrivateLeaseMarker& loaded_owned,
                                   const std::optional<LoadedPrivateLeaseMarker>& loaded_reserved,
                                   const OOCPrivateLeaseTestHooks& hooks) {
    lock.require_stable();
    const auto& owned = loaded_owned.record;
    validate_private_lease_record_context(owned, paths, parent_identity, lock.identity());
    if (owned.phase != PrivateLeasePhase::Owned) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(paths.lease_owned_path, owned, loaded_owned.identity);

    if (loaded_reserved) {
        validate_private_lease_record_context(loaded_reserved->record, paths, parent_identity,
                                              lock.identity());
        validate_private_lease_record_chain(loaded_reserved->record, owned);
        confirm_private_lease_marker(paths.lease_reserved_path, loaded_reserved->record,
                                     loaded_reserved->identity);
    }

    const bool preactive_pair_rollback =
        loaded_reserved &&
        owned.capability == PrivateLeaseCapability::RollbackPreactivePairAndLease;
    const auto staging_path = private_lease_staging_path(paths, owned.lease_id);
    const auto staging_identity = inspect_directory_identity_locked(staging_path);
    const auto final_identity = inspect_directory_identity_locked(paths.private_directory);
    if (staging_identity && final_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (staging_identity && *staging_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (final_identity && *final_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (staging_identity) {
        if (preactive_pair_rollback) {
            (void)inspect_private_lease_preactive_entries(staging_path, paths);
        } else {
            (void)inspect_private_lease_control_entries(staging_path);
        }
    }
    if (final_identity) {
        inspect_private_lease_transaction_entries(paths.private_directory, paths);
    }

    // No protocol leaf is removed until every extant owned directory has
    // passed its phase-appropriate full child allowlist scan.
    invoke_with_stable_base_lock(lock, [&] {
        remove_matching_private_lease_pending(paths.lease_owned_pending_path, owned);
    });
    if (loaded_reserved) {
        invoke_with_stable_base_lock(lock, [&] {
            remove_matching_private_lease_pending(paths.lease_reserved_pending_path,
                                                  loaded_reserved->record);
        });
    }
    if (preactive_pair_rollback) {
        invoke_with_stable_base_lock(
            lock, [&] { discard_matching_preactive_intent_pending_locked(paths); });
    }

    if (staging_identity) {
        if (!loaded_reserved) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        if (preactive_pair_rollback) {
            const auto rolled_back =
                rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
            if (!rolled_back.completed()) {
                return rolled_back;
            }
        } else {
            const auto entries = inspect_private_lease_control_entries(staging_path);
            if (!entries.owner) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            validate_private_lease_owner_at(staging_path, owned);
            invoke_with_stable_base_lock(
                lock, [&] { remove_owner_marker_durable_locked(staging_path, owned, false); });
            invoke_with_stable_base_lock(lock, [&] {
                remove_empty_directory_durable_locked(staging_path, owned.directory_identity);
            });
        }
    } else if (final_identity) {
        const auto owner_path = private_lease_owner_path(paths.private_directory);
        const auto owner_inspection = inspect_private_lease_marker(owner_path);
        if (owner_inspection.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, owner_inspection.error);
        }
        if (owner_inspection.kind == InspectKind::Rejected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        const bool owner_present = owner_inspection.kind == InspectKind::Present;
        if (owner_present) {
            validate_private_lease_owner_at(paths.private_directory, owned);
            inspect_private_lease_transaction_entries(paths.private_directory, paths);
            const auto pair_result =
                run_transaction_locked(paths, lock, nullptr, false, nullptr, nullptr, {});
            if (!pair_result.transaction_terminal()) {
                return pair_result;
            }
        }
        if (preactive_pair_rollback) {
            const auto rolled_back =
                rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
            if (!rolled_back.completed()) {
                return rolled_back;
            }
        } else {
            try {
                require_pair_namespace_reusable_locked(paths);
            } catch (const Failure& failure) {
                if (failure.status == OOCCleanupStatus::NamespaceConflict) {
                    return OOCCleanupResult{
                        .status = OOCCleanupStatus::RecoveryRequired,
                        .stage = OOCCleanupStage::None,
                        .native_error = failure.error,
                    };
                }
                throw;
            }

            const auto entries = inspect_private_lease_control_entries(paths.private_directory);
            if (entries.owner != owner_present) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            invoke_with_stable_base_lock(lock, [&] {
                remove_owner_marker_durable_locked(paths.private_directory, owned, true);
            });
            if (invoke_with_stable_base_lock(lock, [&] {
                    return should_interrupt_private_lease(
                        hooks, OOCPrivateLeaseFaultPoint::OwnerRemovedDurable);
                })) {
                return private_lease_interrupted();
            }
            invoke_with_stable_base_lock(lock, [&] {
                remove_empty_directory_durable_locked(paths.private_directory,
                                                      owned.directory_identity);
            });
            if (invoke_with_stable_base_lock(lock, [&] {
                    return should_interrupt_private_lease(
                        hooks, OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable);
                })) {
                return private_lease_interrupted();
            }
        }
    } else {
        // Cover rmdir-visible-but-not-yet-confirmed recovery. The parent sync
        // establishes the absence before external authority is consumed.
        invoke_with_stable_base_lock(lock, [&] {
            sync_parent_directory(paths.private_directory.parent_path(), OOCCleanupStage::None);
        });
    }

    if (loaded_reserved) {
        invoke_with_stable_base_lock(lock, [&] {
            remove_private_lease_marker_durable(paths.lease_reserved_path, loaded_reserved->record,
                                                loaded_reserved->identity);
        });
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::ReservedRemovedDurable);
            })) {
            return private_lease_interrupted();
        }
    }
    invoke_with_stable_base_lock(lock, [&] {
        remove_private_lease_marker_durable(paths.lease_owned_path, owned, loaded_owned.identity);
    });
    if (invoke_with_stable_base_lock(lock, [&] {
            return should_interrupt_private_lease(hooks,
                                                  OOCPrivateLeaseFaultPoint::OwnedRemovedDurable);
        })) {
        return private_lease_interrupted();
    }
    lock.require_stable();
    return private_lease_completed();
}

} // namespace ooc_cleanup_detail

/// Recoverable cleanup for one closed V3 `.relidx`/`.reldata` pair.
///
/// An unspent move-only ownership receipt is the only authority that can
/// create an intent. The immutable SHA-256-protected intent records native
/// file identities, exact sizes, the complete V3 index header, and paired data
/// ownership. Live
/// files move to same-directory quarantine names with no-replace rename. A
/// marker is first made durable in its deterministic pending leaf and then
/// atomically renamed to its canonical name; pending leaves never authorize
/// deletion. A second immutable staged marker is published only after both
/// quarantined leaves revalidate; no unlink is authorized without both
/// canonical markers. Data and index are then unlinked, intent is durably
/// consumed to close delete authority, and staged is removed as a no-authority
/// completion tail.
///
/// The caller must close all writers, mappings, and reader handles first. A
/// per-base cross-process lock serializes cooperating callers. Parent namespace
/// control remains a trusted boundary; this prototype does not promise
/// non-interference against a malicious same-user process.
class OOCCleanupTransaction final {
private:
    friend class ooc_cleanup_detail::PathPrivateLeaseReservationTarget;
    friend class ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1;

    struct PrivateLeaseActionAdmission final {
        OOCCleanupResult result;
        std::shared_ptr<ooc_cleanup_detail::PrivateCleanupActionPermit> permit;

        PrivateLeaseActionAdmission(OOCCleanupResult admitted_result,
                                    std::shared_ptr<ooc_cleanup_detail::PrivateCleanupActionPermit>
                                        admitted_permit = nullptr) noexcept
            : result(admitted_result), permit(std::move(admitted_permit)) {}
        PrivateLeaseActionAdmission(const PrivateLeaseActionAdmission&) = delete;
        PrivateLeaseActionAdmission& operator=(const PrivateLeaseActionAdmission&) = delete;
        PrivateLeaseActionAdmission(PrivateLeaseActionAdmission&&) noexcept = default;
        PrivateLeaseActionAdmission& operator=(PrivateLeaseActionAdmission&&) noexcept = default;

        [[nodiscard]] bool admitted() const noexcept {
            return result.completed() && permit != nullptr;
        }
    };

    enum class PrivateFreshWriterBoundary : std::uint8_t {
        BeforeIndexReservation,
        IndexReserved,
        BeforeDataReservation,
        DataReserved,
        BeforeHeaderWrite,
        HeadersValidated,
        PairOwnershipCaptured,
        Complete,
    };

    enum class PrivateLeaseActivationBoundary : std::uint8_t {
        BeforeReservedRemoval,
        Complete,
    };

    struct PrivateLeaseMarkerGuardContext final {
        ooc_cleanup_detail::PrivateCleanupActionPermit* permit = nullptr;
        const OOCCleanupPaths* paths = nullptr;
        const ooc_cleanup_detail::BaseLock* lock = nullptr;
    };

    [[nodiscard]] static PrivateLeaseActionAdmission admit_private_lease_reservation_action(
        const OOCCleanupPaths& paths, std::shared_ptr<ooc_cleanup_detail::BaseLock> lock) noexcept;
    [[nodiscard]] static OOCCleanupResult initialize_private_lease_reservation_action(
        ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
        const ooc_cleanup_detail::BaseLock& lock,
        const ooc_cleanup_detail::PrivateLeaseRecord& reserved) noexcept;
    static void private_lease_reservation_marker_transition(
        ooc_cleanup_detail::PrivateLeaseMarkerPublicationPoint point,
        const std::filesystem::path& canonical_path, const std::filesystem::path& pending_path,
        const ooc_cleanup_detail::PrivateLeaseRecord& record,
        const ooc_cleanup_detail::FileIdentity* identity, void* context);
    [[nodiscard]] static OOCCleanupResult record_private_lease_reservation_directory(
        ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
        const ooc_cleanup_detail::BaseLock& lock, const std::filesystem::path& directory_path,
        const std::array<std::uint64_t, 3>& identity, bool final_directory) noexcept;
    [[nodiscard]] static OOCCleanupResult authorize_private_lease_reservation_staging_directory(
        ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
        const ooc_cleanup_detail::BaseLock& lock) noexcept;
    [[nodiscard]] static OOCCleanupResult authorize_private_lease_reservation_final_rename(
        ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
        const ooc_cleanup_detail::BaseLock& lock) noexcept;
    [[nodiscard]] static OOCCleanupResult complete_private_lease_reservation_action(
        ooc_cleanup_detail::PrivateCleanupActionPermit& permit, const OOCCleanupPaths& paths,
        const ooc_cleanup_detail::BaseLock& lock) noexcept;

    [[nodiscard]] static PrivateLeaseActionAdmission
    admit_private_fresh_writer_action(const OOCPrivateLeaseOwnershipReceipt& lease,
                                      bool deferred_mode) noexcept;
    [[nodiscard]] static OOCCleanupResult advance_private_fresh_writer_action(
        ooc_cleanup_detail::PrivateCleanupActionPermit& permit,
        const OOCPrivateLeaseOwnershipReceipt& lease, PrivateFreshWriterBoundary boundary,
        std::optional<std::array<std::uint64_t, 3>> index_identity = std::nullopt,
        std::optional<std::array<std::uint64_t, 3>> data_identity = std::nullopt,
        std::uint64_t store_id = 0) noexcept;
    [[nodiscard]] static bool private_fresh_writer_rollback_allowed(
        ooc_cleanup_detail::PrivateCleanupActionPermit& permit,
        const OOCPrivateLeaseOwnershipReceipt& lease,
        std::optional<std::array<std::uint64_t, 3>> index_identity,
        std::optional<std::array<std::uint64_t, 3>> data_identity) noexcept;

    [[nodiscard]] static PrivateLeaseActionAdmission admit_private_lease_activation_action(
        const OOCPrivateLeaseOwnershipReceipt& lease,
        const OOCCleanupOwnershipReceipt& pair_ownership) noexcept;
    [[nodiscard]] static OOCCleanupResult
    advance_private_lease_activation_action(ooc_cleanup_detail::PrivateCleanupActionPermit& permit,
                                            const OOCPrivateLeaseOwnershipReceipt& lease,
                                            const OOCCleanupOwnershipReceipt& pair_ownership,
                                            PrivateLeaseActivationBoundary boundary) noexcept;
    [[nodiscard]] static OOCCleanupResult
    commit_private_lease_activation_action(ooc_cleanup_detail::PrivateCleanupActionPermit& permit,
                                           OOCPrivateLeaseOwnershipReceipt& lease,
                                           OOCPrivateLeaseTestHooks hooks) noexcept;
    [[nodiscard]] static bool commit_private_lease_activation_noexcept(
        ooc_cleanup_detail::PrivateCleanupActionPermit& permit) noexcept;

public:
    [[nodiscard]] static OOCCleanupResult
    begin_or_resume(OOCCleanupOwnershipReceipt& ownership,
                    std::optional<OOCExactCleanupExpectation> exact = std::nullopt,
                    OOCCleanupTestHooks hooks = {}) noexcept {
        if (ownership.spent_ || ownership.base_path_.empty() || ownership.store_id_ == 0) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = ooc_cleanup_detail::invalid_argument_error(),
            };
        }
        const OOCCleanupRequest request{
            .base_path = ownership.base_path_,
            .store_id = ownership.store_id_,
            .exact = std::move(exact),
        };
        const ooc_cleanup_detail::OwnershipProof proof{
            .base_path = ownership.base_path_,
            .store_id = ownership.store_id_,
            .index_identity =
                {
                    ownership.index_identity_.first,
                    ownership.index_identity_.second,
                    ownership.index_identity_.third,
                },
            .data_identity =
                {
                    ownership.data_identity_.first,
                    ownership.data_identity_.second,
                    ownership.data_identity_.third,
                },
        };
        bool consume_receipt = false;
        const auto result = invoke([&] {
            return ooc_cleanup_detail::run_transaction(request.base_path, &request, true, &proof,
                                                       &consume_receipt, hooks);
        });
        if (consume_receipt) {
            ownership.spent_ = true;
        }
        return result;
    }

    [[nodiscard]] static OOCCleanupResult resume(const OOCCleanupRequest& request,
                                                 OOCCleanupTestHooks hooks = {}) noexcept {
        return invoke([&] {
            return ooc_cleanup_detail::run_transaction(request.base_path, &request, false, nullptr,
                                                       nullptr, hooks);
        });
    }

    [[nodiscard]] static OOCCleanupResult resume(const std::filesystem::path& base_path,
                                                 OOCCleanupTestHooks hooks = {}) noexcept {
        return invoke([&] {
            return ooc_cleanup_detail::run_transaction(base_path, nullptr, false, nullptr, nullptr,
                                                       hooks);
        });
    }

    [[nodiscard]] static OOCCleanupPaths paths_for(const std::filesystem::path& base_path) {
        return ooc_cleanup_detail::freeze_paths(base_path);
    }

    /// Classify the generic private handoff under the exact persistent BaseLock.
    /// This is observation only: exact duplicate pending leaves are retained,
    /// and no returned value grants cleanup or adoption authority.
    [[nodiscard]] static OOCPrivateHandoffInspectResult
    inspect_private_handoff(const std::filesystem::path& base_path) noexcept {
        auto observation = ooc_cleanup_detail::handoff_failure(
            OOCCleanupStatus::UnexpectedFailure, OOCPrivateHandoffState::TaintedPreserved, {});
        bool assigned = false;
        const auto result = invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(base_path);
            if (paths.private_directory.empty()) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            const auto lock_inspection =
                ooc_cleanup_detail::inspect_file(paths.lock_path, 0, false);
            if (lock_inspection.kind == ooc_cleanup_detail::InspectKind::Error) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                         lock_inspection.error);
            }
            if (lock_inspection.kind == ooc_cleanup_detail::InspectKind::Missing) {
                observation =
                    ooc_cleanup_detail::private_protocol_artifact_exists_without_lock(paths)
                        ? ooc_cleanup_detail::handoff_failure(
                              OOCCleanupStatus::NamespaceConflict,
                              OOCPrivateHandoffState::TaintedPreserved)
                        : ooc_cleanup_detail::handoff_none();
                assigned = true;
                return observation.result;
            }
            if (lock_inspection.kind != ooc_cleanup_detail::InspectKind::Present) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            ooc_cleanup_detail::BaseLock lock(paths.lock_path, false);
            observation = ooc_cleanup_detail::classify_private_handoff_locked(paths, lock);
            assigned = true;
            return observation.result;
        });
        if (!assigned) {
            observation = ooc_cleanup_detail::handoff_failure(
                result.status, OOCPrivateHandoffState::TaintedPreserved, result.native_error);
        }
        return observation;
    }

    /// Adopt one exact canonical generic handoff under its persistent lock.
    ///
    /// Success retains the lock, both directory bindings, and both exact
    /// same-handle validated files in a move-only receipt. The operation never
    /// creates, removes, or rewrites a namespace entry and grants no cleanup
    /// authority.
    [[nodiscard]] static OOCPrivateHandoffAdoptionResult
    adopt_private_handoff(const std::filesystem::path& base_path,
                          OOCPrivateHandoffAdoptionTestHooks hooks = {}) noexcept;

    /// Publish an immutable application payload bound to one exact finalized V3
    /// pair and its still-preactive private lease. Canonical durability consumes
    /// pair ownership. RESERVED is then durably revoked; the lease receipt is
    /// deliberately left move-only but stale, with its live lock released.
    [[nodiscard]] static OOCPrivateHandoffPublishResult publish_private_handoff(
        OOCCleanupOwnershipReceipt& pair_ownership, OOCPrivateLeaseOwnershipReceipt& lease,
        const OOCPrivateHandoffPairDescriptorV1& pair, std::uint32_t payload_kind,
        std::uint32_t payload_version, std::span<const std::byte> opaque_payload,
        OOCPrivateHandoffTestHooks hooks = {}) noexcept {
        if (pair_ownership.spent_ || pair_ownership.store_id_ == 0 || lease.spent_ ||
            lease.active_ || !lease.live_lock_ || payload_kind == 0 || payload_version == 0 ||
            opaque_payload.size() > OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES ||
            pair.format_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
            pair.store_id != pair_ownership.store_id_ || pair.generation == 0 ||
            pair.index_extent < OOCRelationStoreFormat::INDEX_HEADER_BYTES ||
            pair.data_extent < OOCRelationStoreFormat::DATA_HEADER_BYTES) {
            return ooc_cleanup_detail::handoff_failure(
                OOCCleanupStatus::InvalidRequest, OOCPrivateHandoffState::TaintedPreserved,
                ooc_cleanup_detail::invalid_argument_error());
        }

        auto publication = ooc_cleanup_detail::handoff_failure(
            OOCCleanupStatus::UnexpectedFailure, OOCPrivateHandoffState::TaintedPreserved, {});
        bool assigned = false;
        bool release_live_lock = false;
        const auto result = invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(lease.base_path_);
            if (paths.private_directory.empty() || paths.base_path != lease.base_path_ ||
                paths.private_directory != lease.private_directory_ ||
                paths.lock_path != lease.lock_path_ ||
                pair_ownership.base_path_ != lease.base_path_ ||
                !lease.live_lock_->matches(paths.lock_path)) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            auto& lock = *lease.live_lock_;
            lock.require_stable();
            const auto rollback =
                ooc_cleanup_detail::inspect_file(paths.private_handoff_rollback_path, 0, false);
            if (rollback.kind == ooc_cleanup_detail::InspectKind::Error) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                         rollback.error);
            }
            if (rollback.kind != ooc_cleanup_detail::InspectKind::Missing) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            const auto directory_identity =
                ooc_cleanup_detail::capture_directory_identity_locked(paths.private_directory);
            if (directory_identity != lease.directory_identity_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

#ifdef _WIN32
            ooc_cleanup_detail::fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
                                     std::make_error_code(std::errc::operation_not_supported));
#else
            ooc_cleanup_detail::PrivateDirectoryHandle directory(paths.private_directory);
            if (directory.identity() != directory_identity) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            directory.require_private_policy();
            const auto canonical_leaf = paths.private_handoff_path.filename();
            const auto pending_leaf = paths.private_handoff_pending_path.filename();

            // This production read is the strict platform/ACL probe. On Linux
            // and other unsupported platforms it returns before observing or
            // mutating the generic namespace.
            const auto canonical_before = ooc_cleanup_detail::read_private_handoff_leaf(
                directory.native_handle(), canonical_leaf);
            const auto pending_before = ooc_cleanup_detail::read_private_handoff_leaf(
                directory.native_handle(), pending_leaf);
            directory.require_private_policy();
            if (canonical_before.state == ooc_cleanup_detail::PrivateHandoffLeafState::Rejected ||
                pending_before.state == ooc_cleanup_detail::PrivateHandoffLeafState::Rejected) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                         OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

            const auto entries =
                ooc_cleanup_detail::inspect_private_handoff_directory_entries(paths);
            directory.require_private_policy();
            if (!entries.valid) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                         OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            if (entries.legacy_authority) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

            const auto parent_identity = ooc_cleanup_detail::capture_directory_identity_locked(
                paths.private_directory.parent_path());
            auto reserved =
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_reserved_path);
            auto owned =
                ooc_cleanup_detail::load_optional_private_lease_marker(paths.lease_owned_path);
            if (!owned || owned->record.lease_id != lease.lease_id_ ||
                owned->record.directory_identity != lease.directory_identity_ ||
                owned->record.owner_identity != lease.owner_identity_ ||
                owned->identity != lease.owned_identity_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            ooc_cleanup_detail::validate_private_lease_record_context(
                owned->record, paths, parent_identity, lock.identity());
            if (reserved) {
                ooc_cleanup_detail::validate_private_lease_record_context(
                    reserved->record, paths, parent_identity, lock.identity());
                ooc_cleanup_detail::validate_private_lease_record_chain(reserved->record,
                                                                        owned->record);
            }
            ooc_cleanup_detail::validate_private_lease_owner_at(paths.private_directory,
                                                                owned->record);
            if (ooc_cleanup_detail::inspect_directory_identity_locked(
                    ooc_cleanup_detail::private_lease_staging_path(paths, lease.lease_id_))) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

            const ooc_cleanup_detail::OwnershipProof proof{
                .base_path = pair_ownership.base_path_,
                .store_id = pair_ownership.store_id_,
                .index_identity =
                    {
                        pair_ownership.index_identity_.first,
                        pair_ownership.index_identity_.second,
                        pair_ownership.index_identity_.third,
                    },
                .data_identity =
                    {
                        pair_ownership.data_identity_.first,
                        pair_ownership.data_identity_.second,
                        pair_ownership.data_identity_.third,
                    },
            };
            const auto source =
                ooc_cleanup_detail::capture_source_pair(paths, pair_ownership.store_id_);
            if (!ooc_cleanup_detail::ownership_proof_matches(proof, paths, source)) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            ooc_cleanup_detail::require_source_pair_unchanged(paths, source);

            OOCPrivateHandoffRecordV1 record{
                .schema_version = OOC_PRIVATE_HANDOFF_SCHEMA_VERSION_V1,
                .platform_id = OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1,
                .lease_id = lease.lease_id_,
                .lock_identity = ooc_cleanup_detail::handoff_native_identity(lock.identity()),
                .directory_identity =
                    ooc_cleanup_detail::handoff_native_identity(directory_identity),
                .owner_marker_identity =
                    ooc_cleanup_detail::handoff_native_identity(lease.owner_identity_),
                .owned_marker_identity =
                    ooc_cleanup_detail::handoff_native_identity(lease.owned_identity_),
                .pair = pair,
                .index =
                    {
                        .identity =
                            ooc_cleanup_detail::handoff_native_identity(source.index.identity),
                        .extent = source.index.identity.size,
                    },
                .data =
                    {
                        .identity =
                            ooc_cleanup_detail::handoff_native_identity(source.data.identity),
                        .extent = source.data.identity.size,
                    },
                .payload_kind = payload_kind,
                .payload_version = payload_version,
                .opaque_payload =
                    std::vector<std::byte>(opaque_payload.begin(), opaque_payload.end()),
            };
            if (!ooc_cleanup_detail::handoff_pair_matches_context(record, paths)) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            const auto sealed = seal_ooc_private_handoff_record(record);
            if (!sealed) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            const auto encoded = encode_ooc_private_handoff_record(record);
            if (!encoded || !encoded.bytes) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            if (!reserved &&
                (canonical_before.state != ooc_cleanup_detail::PrivateHandoffLeafState::Exact ||
                 canonical_before.bytes != *encoded.bytes)) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

            ooc_cleanup_detail::PrivateHandoffRecordHookBridge bridge{
                .hooks = hooks,
                .lock = &lock,
                .stopped_at = std::nullopt,
            };
            lock.require_stable();
            const auto published = util::durable_immutable_record::publish_at(
                directory.native_handle(), pending_leaf, canonical_leaf, *encoded.bytes,
                util::durable_immutable_record::RecordTestHooks{
                    .stop_after = ooc_cleanup_detail::bridge_private_handoff_fault,
                    .context = &bridge,
                });
            lock.require_stable();
            if (published.status() ==
                util::durable_immutable_record::RecordPublishStatus::interrupted) {
                publication = ooc_cleanup_detail::handoff_failure(
                    OOCCleanupStatus::Interrupted,
                    bridge.stopped_at == OOCPrivateHandoffFaultPoint::PendingDurable
                        ? OOCPrivateHandoffState::PendingOnly
                        : OOCPrivateHandoffState::Canonical,
                    published.native_error());
                publication.record = record;
                if (published.canonical_snapshot()) {
                    publication.identity = published.canonical_snapshot()->identity;
                }
                assigned = true;
                return publication.result;
            }
            if (!published.is_durable() || !published.canonical_snapshot()) {
                publication = ooc_cleanup_detail::handoff_failure(
                    ooc_cleanup_detail::handoff_publish_status(published.status()),
                    OOCPrivateHandoffState::TaintedPreserved, published.native_error());
                assigned = true;
                return publication.result;
            }
            directory.require_stable();
            lock.require_stable();

            if (reserved) {
                ooc_cleanup_detail::invoke_with_stable_base_lock(lock, [&] {
                    ooc_cleanup_detail::remove_matching_private_lease_pending(
                        paths.lease_reserved_pending_path, reserved->record);
                });
                ooc_cleanup_detail::invoke_with_stable_base_lock(lock, [&] {
                    ooc_cleanup_detail::remove_private_lease_marker_durable(
                        paths.lease_reserved_path, reserved->record, reserved->identity);
                });
            }
            const bool stop_after_reserved =
                ooc_cleanup_detail::invoke_with_stable_base_lock(lock, [&] {
                    return hooks.stop_after != nullptr &&
                           hooks.stop_after(OOCPrivateHandoffFaultPoint::ReservedRevokedDurable,
                                            hooks.context);
                });
            if (stop_after_reserved) {
                publication = ooc_cleanup_detail::handoff_failure(
                    OOCCleanupStatus::Interrupted, OOCPrivateHandoffState::Canonical,
                    std::make_error_code(std::errc::operation_canceled));
                publication.record = record;
                publication.identity = published.canonical_snapshot()->identity;
                assigned = true;
                return publication.result;
            }

            publication =
                ooc_cleanup_detail::handoff_present(record, *published.canonical_snapshot());
            lock.require_stable();
            release_live_lock = true;
            assigned = true;
            return publication.result;
#endif
        });
        if (release_live_lock) {
            pair_ownership.spent_ = true;
            lease.live_lock_.reset();
        }
        if (!assigned) {
            publication = ooc_cleanup_detail::handoff_failure(
                result.status, OOCPrivateHandoffState::TaintedPreserved, result.native_error);
        }
        return publication;
    }

    /// Reserve one fresh RelationSink private directory. The directory name is
    /// derived from the recognized `<requested>.gnfs-sink-lease/corpus`
    /// layout, and its lock is a persistent sibling outside the removable
    /// directory. The returned receipt retains that same lock until a fresh
    /// writer has created both O_EXCL artifacts and activates the lease.
    [[nodiscard]] static OOCPrivateLeaseReservation
    reserve_private_lease(const std::filesystem::path& base_path,
                          OOCPrivateLeaseTestHooks hooks = {}) noexcept;

    /// Remove one exact private lease after its pair namespace is fully empty.
    /// The external lock remains permanently in place, so directory reuse
    /// cannot create a second simultaneously valid lock inode.
    [[nodiscard]] static OOCCleanupResult
    remove_private_lease(OOCPrivateLeaseOwnershipReceipt& ownership,
                         OOCPrivateLeaseTestHooks hooks = {}) noexcept;

    /// Recover a crash-left lease only through durable protocol authority.
    /// A new-capability lease with RESERVED still present may quarantine and
    /// roll back its exact preactivation directory. Active live pairs, for
    /// which activation already consumed RESERVED, remain preserved without a
    /// canonical pair cleanup intent.
    [[nodiscard]] static OOCCleanupResult
    recover_private_lease(const std::filesystem::path& base_path,
                          OOCPrivateLeaseTestHooks hooks = {}) noexcept {
        return invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(base_path);
            if (paths.private_directory.empty()) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            const auto lock_inspection =
                ooc_cleanup_detail::inspect_file(paths.lock_path, 0, false);
            if (lock_inspection.kind == ooc_cleanup_detail::InspectKind::Error) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                         lock_inspection.error);
            }
            if (lock_inspection.kind == ooc_cleanup_detail::InspectKind::Missing) {
                if (!ooc_cleanup_detail::private_protocol_artifact_exists_without_lock(paths)) {
                    return ooc_cleanup_detail::private_lease_no_transaction();
                }
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            if (lock_inspection.kind != ooc_cleanup_detail::InspectKind::Present) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
            auto lock = std::make_shared<ooc_cleanup_detail::BaseLock>(paths.lock_path, false);
            return ooc_cleanup_detail::recover_private_lease_locked(paths, std::move(lock), hooks);
        });
    }

    /// Confirm that no live, quarantined, pending, or canonical cleanup leaf
    /// remains for this pair. The persistent regular lock leaf is deliberately
    /// retained and is not part of the pair-reuse decision.
    [[nodiscard]] static OOCCleanupResult
    confirm_pair_namespace_reusable(const std::filesystem::path& base_path) noexcept {
        return invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(base_path);
            ooc_cleanup_detail::BaseLock lock(paths.lock_path, paths.private_directory.empty());
            if (!paths.private_directory.empty()) {
                const auto handoff =
                    ooc_cleanup_detail::classify_private_handoff_locked(paths, lock);
                if (handoff.state != OOCPrivateHandoffState::None) {
                    return handoff.result;
                }
            }
            ooc_cleanup_detail::require_pair_namespace_reusable_locked(paths);
            lock.require_stable();
            return OOCCleanupResult{
                .status = OOCCleanupStatus::Completed,
                .stage = OOCCleanupStage::Completed,
                .native_error = {},
            };
        });
    }

private:
    /// Read-only authority-union admission before the public deferred writer
    /// finalizes its pair. This preserves fail-early zero-mutation behavior, but
    /// never authorizes across finalize; publication mints a new action-bound
    /// permit from the finalized pair and retained live lease.
    [[nodiscard]] static OOCCleanupResult
    preflight_private_lease_cleanup_handoff(const OOCPrivateLeaseOwnershipReceipt& lease) noexcept {
        if (lease.spent_ || lease.active_ || !lease.live_lock_ || lease.base_path_.empty() ||
            lease.private_directory_.empty() || lease.lock_path_.empty()) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = ooc_cleanup_detail::invalid_argument_error(),
            };
        }
        return invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(lease.base_path_);
            if (paths.base_path != lease.base_path_ ||
                paths.private_directory != lease.private_directory_ ||
                paths.lock_path != lease.lock_path_ ||
                !lease.live_lock_->matches(paths.lock_path)) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            if (const auto blocked =
                    ooc_cleanup_detail::preflight_private_cleanup_union_for_transaction_locked(
                        paths, *lease.live_lock_, true)) {
                return *blocked;
            }
            lease.live_lock_->require_stable();
            return ooc_cleanup_detail::private_lease_completed();
        });
    }

    /// Publish the exact fresh pair's canonical cleanup intent while retaining
    /// the private lease's RESERVED predecessor and held BaseLock. The
    /// out-of-line implementation owns the source-private post-finalize permit.
    [[nodiscard]] static OOCCleanupResult publish_private_lease_cleanup_handoff(
        OOCCleanupOwnershipReceipt& pair_ownership, OOCPrivateLeaseOwnershipReceipt& lease,
        const OOCExactCleanupExpectation& exact, OOCCleanupTestHooks hooks = {}) noexcept;

    [[nodiscard]] static OOCCleanupResult
    activate_private_lease_for_fresh_writer(OOCPrivateLeaseOwnershipReceipt& lease,
                                            const OOCCleanupOwnershipReceipt& pair_ownership,
                                            OOCPrivateLeaseTestHooks hooks = {}) noexcept {
        if (lease.spent_ || lease.active_ || !lease.live_lock_ || pair_ownership.spent_ ||
            pair_ownership.store_id_ == 0 ||
            lease.owner_process_id_ != static_cast<std::uint64_t>(gnfs::util::process_id())) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::InvalidRequest,
                .stage = OOCCleanupStage::None,
                .native_error = ooc_cleanup_detail::invalid_argument_error(),
            };
        }
        return invoke([&] {
            const auto paths = ooc_cleanup_detail::freeze_paths(lease.base_path_);
            if (paths.base_path != lease.base_path_ ||
                paths.private_directory != lease.private_directory_ ||
                paths.lock_path != lease.lock_path_ ||
                pair_ownership.base_path_ != lease.base_path_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            auto activation_admission =
                admit_private_lease_activation_action(lease, pair_ownership);
            if (!activation_admission.admitted()) {
                return activation_admission.result;
            }
            auto& activation_permit = *activation_admission.permit;
            auto retained_lock = lease.live_lock_;
            auto& lock = *retained_lock;
            if (ooc_cleanup_detail::invoke_with_stable_base_lock(lock, [&] {
                    return ooc_cleanup_detail::should_interrupt_private_lease(
                        hooks, OOCPrivateLeaseFaultPoint::ActivationPermitAcquired);
                })) {
                return ooc_cleanup_detail::private_lease_interrupted();
            }
            const auto removal_authorized = advance_private_lease_activation_action(
                activation_permit, lease, pair_ownership,
                PrivateLeaseActivationBoundary::BeforeReservedRemoval);
            if (!removal_authorized.completed()) {
                return removal_authorized;
            }
            const auto committed =
                commit_private_lease_activation_action(activation_permit, lease, hooks);
            // Durable RESERVED removal is the capability commit point. From
            // here onward OWNED alone cannot authorize preactivation pair
            // rollback, even when a test interruption or later lock-identity
            // failure makes the activation call return non-success.
            if (!committed.completed()) {
                return committed;
            }
            const auto completed = advance_private_lease_activation_action(
                activation_permit, lease, pair_ownership, PrivateLeaseActivationBoundary::Complete);
            return completed;
        });
    }

    [[nodiscard]] static OOCCleanupOwnershipReceipt
    capture_fresh_ownership_receipt(const std::filesystem::path& base_path, std::uint64_t store_id,
                                    const std::array<std::uint64_t, 3>& expected_index_identity,
                                    const std::array<std::uint64_t, 3>& expected_data_identity) {
        const auto paths = ooc_cleanup_detail::freeze_paths(base_path);
        const auto source = ooc_cleanup_detail::capture_source_pair(paths, store_id);
        ooc_cleanup_detail::require_source_pair_unchanged(paths, source);
        const auto index_identity = ooc_cleanup_detail::stable_identity(source.index.identity);
        const auto data_identity = ooc_cleanup_detail::stable_identity(source.data.identity);
        if (index_identity != expected_index_identity || data_identity != expected_data_identity ||
            index_identity == data_identity) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        return OOCCleanupOwnershipReceipt(paths.base_path, store_id,
                                          OOCCleanupOwnershipReceipt::NativeIdentity{
                                              .first = source.index.identity.first,
                                              .second = source.index.identity.second,
                                              .third = source.index.identity.third,
                                          },
                                          OOCCleanupOwnershipReceipt::NativeIdentity{
                                              .first = source.data.identity.first,
                                              .second = source.data.identity.second,
                                              .third = source.data.identity.third,
                                          });
    }

    template <typename Operation>
    [[nodiscard]] static OOCCleanupResult invoke(Operation&& operation) noexcept {
        try {
            return std::forward<Operation>(operation)();
        } catch (const ooc_cleanup_detail::Failure& failure) {
            return OOCCleanupResult{
                .status = failure.status,
                .stage = failure.stage,
                .native_error = failure.error,
            };
        } catch (const std::filesystem::filesystem_error& error) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::IoFailure,
                .stage = OOCCleanupStage::None,
                .native_error = error.code(),
            };
        } catch (const std::system_error& error) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::IoFailure,
                .stage = OOCCleanupStage::None,
                .native_error = error.code(),
            };
        } catch (...) {
            return OOCCleanupResult{
                .status = OOCCleanupStatus::UnexpectedFailure,
                .stage = OOCCleanupStage::None,
                .native_error = {},
            };
        }
    }

    friend class OOCRelationWriter;
};

} // namespace gnfs::relation
