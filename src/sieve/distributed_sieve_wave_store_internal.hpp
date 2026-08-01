#pragma once

// Source-private durable authority boundary for one distributed-sieve wave.
// This file is intentionally not installed as public API.

#include "distributed_sieve_merge_prepared_admission_internal.hpp"
#include "distributed_sieve_worker_launcher_fwd_internal.hpp"

#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/durable_immutable_record.hpp>
#include <gnfs/util/process.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace gnfs::core {
class PolynomialContext;
}

namespace gnfs::factor_base {
class FactorBase;
}

namespace gnfs::relation {
struct OOCCleanupResult;
struct OOCPrivateLeaseTestHooks;
struct OOCPrivateHandoffAdoptionResult;
class OOCPrivateHandoffReader;
class OOCRelationReader;
class OOCRelationWriter;
} // namespace gnfs::relation

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {
struct DistributedSieveFrozenExecutionPolicyV1;
}

namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail {
class DistributedSieveMergeWriterAuthorityV1;
}

namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail {
class DistributedSieveWorkerCleanupReceiptMintAuthorityV1;
}

namespace gnfs::sieve::distributed_sieve_resume_detail {

class DistributedSieveExternalCleanupAuthorizationState;
[[nodiscard]] bool distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
    const DistributedSieveExternalCleanupAuthorizationState& state) noexcept;
void distributed_sieve_external_cleanup_authorization_state_release_receipt_claim(
    const DistributedSieveExternalCleanupAuthorizationState& state) noexcept;

inline constexpr std::string_view DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF = ".gnfs-wave-v1.lock";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF = ".gnfs-wave-v1.manifest";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF =
    ".gnfs-wave-v1.manifest.pending";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX =
    ".gnfs-wave-v1.attempt-c";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR = "-a";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PENDING_SUFFIX =
    ".pending";
inline constexpr std::string_view DISTRIBUTED_SIEVE_MERGE_GENERATION_STEM_PREFIX_V1 =
    "gnfs-wave-v1-merge-a";
inline constexpr std::string_view DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PREFIX =
    ".gnfs-wave-v1.merge-start-a";
inline constexpr std::string_view DISTRIBUTED_SIEVE_MERGE_STARTED_RECORD_PENDING_SUFFIX =
    ".pending";
inline constexpr std::string_view DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PREFIX =
    ".gnfs-wave-v1.chunk-terminal-failure-c";
inline constexpr std::string_view DISTRIBUTED_SIEVE_CHUNK_TERMINAL_FAILURE_RECORD_PENDING_SUFFIX =
    ".pending";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_LEAF =
    ".gnfs-wave-v1.merge-commit";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WAVE_MERGE_COMMIT_RECORD_PENDING_LEAF =
    ".gnfs-wave-v1.merge-commit.pending";
inline constexpr std::string_view DISTRIBUTED_SIEVE_CLEANUP_AUTHORIZED_WORKER_RECORD_PREFIX =
    ".gnfs-wave-v1.cleanup-authorized-worker-c";
inline constexpr std::string_view DISTRIBUTED_SIEVE_CLEANUP_COMPLETED_WORKER_RECORD_PREFIX =
    ".gnfs-wave-v1.cleanup-completed-worker-c";
inline constexpr std::string_view DISTRIBUTED_SIEVE_CLEANUP_AUTHORIZED_MERGED_RECORD_LEAF =
    ".gnfs-wave-v1.cleanup-authorized-merged";
inline constexpr std::string_view DISTRIBUTED_SIEVE_CLEANUP_COMPLETED_MERGED_RECORD_LEAF =
    ".gnfs-wave-v1.cleanup-completed-merged";
inline constexpr std::string_view DISTRIBUTED_SIEVE_ROOT_RECORD_PENDING_SUFFIX = ".pending";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_DIRECTORY_SUFFIX =
    ".gnfs-sink-lease";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX =
    ".gnfs-sink-lease.gnfs-ooc-cleanup-v1.lock";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_SUFFIX =
    ".gnfs-sink-lease.gnfs-private-lease-v1.reserved";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_PENDING_SUFFIX =
    ".gnfs-sink-lease.gnfs-private-lease-v1.reserved.pending";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_SUFFIX =
    ".gnfs-sink-lease.gnfs-private-lease-v1.owned";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_PENDING_SUFFIX =
    ".gnfs-sink-lease.gnfs-private-lease-v1.owned.pending";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_HANDOFF_ROLLBACK_SUFFIX =
    ".gnfs-sink-lease.gnfs-ooc-private-handoff-v1.rollback";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG =
    ".gnfs-sink-lease.gnfs-private-lease-v1.stage-";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF =
    ".gnfs-private-lease-v1.owner";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF =
    ".gnfs-private-lease-v1.owner.pending";
inline constexpr std::uint32_t DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1 = 1;

struct DistributedSievePrivateLeaseNamesV1 {
    std::string relative_lease_stem;
    std::string private_directory_leaf;
    std::string base_lock_leaf;
    std::string reserved_leaf;
    std::string reserved_pending_leaf;
    std::string owned_leaf;
    std::string owned_pending_leaf;
    std::string rollback_handoff_leaf;

    [[nodiscard]] friend bool operator==(const DistributedSievePrivateLeaseNamesV1&,
                                         const DistributedSievePrivateLeaseNamesV1&) = default;
};

struct DistributedSieveWorkerAttemptNamesV1 final : DistributedSievePrivateLeaseNamesV1 {
    std::string canonical_record_leaf;
    std::string pending_record_leaf;

    [[nodiscard]] friend bool operator==(const DistributedSieveWorkerAttemptNamesV1&,
                                         const DistributedSieveWorkerAttemptNamesV1&) = default;
};

struct DistributedSieveMergeGenerationNamesV1 final : DistributedSievePrivateLeaseNamesV1 {
    std::string canonical_record_leaf;
    std::string pending_record_leaf;

    [[nodiscard]] friend bool operator==(const DistributedSieveMergeGenerationNamesV1&,
                                         const DistributedSieveMergeGenerationNamesV1&) = default;
};

struct DistributedSieveParsedWorkerAttemptLeafV1 final {
    std::uint32_t chunk_id = 0;
    std::uint32_t attempt_ordinal = 0;
    bool pending = false;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveParsedWorkerAttemptLeafV1&,
               const DistributedSieveParsedWorkerAttemptLeafV1&) noexcept = default;
};

struct DistributedSieveParsedMergeStartedLeafV1 final {
    std::uint32_t merge_attempt_ordinal = 0;
    bool pending = false;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveParsedMergeStartedLeafV1&,
               const DistributedSieveParsedMergeStartedLeafV1&) noexcept = default;
};

struct DistributedSieveChunkTerminalFailureNamesV1 final {
    std::string canonical_record_leaf;
    std::string pending_record_leaf;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveChunkTerminalFailureNamesV1&,
               const DistributedSieveChunkTerminalFailureNamesV1&) = default;
};

struct DistributedSieveParsedChunkTerminalFailureLeafV1 final {
    std::uint32_t chunk_id = 0;
    bool pending = false;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveParsedChunkTerminalFailureLeafV1&,
               const DistributedSieveParsedChunkTerminalFailureLeafV1&) noexcept = default;
};

struct DistributedSieveWaveMergeCommitNamesV1 final {
    std::string canonical_record_leaf;
    std::string pending_record_leaf;

    [[nodiscard]] friend bool operator==(const DistributedSieveWaveMergeCommitNamesV1&,
                                         const DistributedSieveWaveMergeCommitNamesV1&) = default;
};

struct DistributedSieveParsedWaveMergeCommitLeafV1 final {
    bool pending = false;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveParsedWaveMergeCommitLeafV1&,
               const DistributedSieveParsedWaveMergeCommitLeafV1&) noexcept = default;
};

enum class DistributedSieveCleanupRecordCoordinateKindV1 : std::uint8_t {
    authorized_worker,
    completed_worker,
    authorized_merged,
    completed_merged,
};

struct DistributedSieveParsedCleanupRecordLeafV1 final {
    DistributedSieveCleanupRecordCoordinateKindV1 kind =
        DistributedSieveCleanupRecordCoordinateKindV1::authorized_worker;
    std::optional<std::uint32_t> manifest_order_ordinal;
    bool pending = false;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveParsedCleanupRecordLeafV1&,
               const DistributedSieveParsedCleanupRecordLeafV1&) noexcept = default;
};

struct DistributedSieveWorkerCleanupRecordNamesV1 final {
    std::string authorization_canonical_record_leaf;
    std::string authorization_pending_record_leaf;
    std::string completion_canonical_record_leaf;
    std::string completion_pending_record_leaf;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveWorkerCleanupRecordNamesV1&,
               const DistributedSieveWorkerCleanupRecordNamesV1&) = default;
};

/// Derive the exact V1 lease stem, private directory, permanent BaseLock, and
/// immutable record leaves for one bounded worker attempt. This is naming only
/// and grants no filesystem authority.
[[nodiscard]] std::optional<DistributedSieveWorkerAttemptNamesV1>
distributed_sieve_worker_attempt_names_v1(std::string_view chunk_relative_artifact_stem,
                                          std::uint32_t chunk_id, std::uint32_t attempt_ordinal);

/// Parse only the exact lowercase, fixed-width V1 canonical or pending record
/// leaf. This pure parser grants no namespace membership: attempt leaves remain
/// foreign to WaveStore inventory until manifest-aware record loading is wired
/// in. No other leaf is normalized or accepted as an alias.
[[nodiscard]] std::optional<DistributedSieveParsedWorkerAttemptLeafV1>
parse_distributed_sieve_worker_attempt_leaf_v1(std::string_view leaf) noexcept;

/// Derive the exact fixed-width V1 private-lease and immutable start-record
/// leaves for one bounded merged-build generation. This is naming only and
/// grants no namespace, reservation, publication, or cleanup authority.
[[nodiscard]] std::optional<DistributedSieveMergeGenerationNamesV1>
distributed_sieve_merge_generation_names_v1(std::uint32_t merge_attempt_ordinal);

/// Parse only the exact lowercase, fixed-width V1 canonical or pending
/// MergeStarted leaf. No variable-width, suffixed, or case-folded alias is
/// accepted.
[[nodiscard]] std::optional<DistributedSieveParsedMergeStartedLeafV1>
parse_distributed_sieve_merge_started_leaf_v1(std::string_view leaf) noexcept;

/// Derive the exact root-level canonical and pending leaves for one terminal
/// chunk failure. This pure naming helper grants no record-publication or
/// namespace authority.
[[nodiscard]] std::optional<DistributedSieveChunkTerminalFailureNamesV1>
distributed_sieve_chunk_terminal_failure_names_v1(std::uint32_t chunk_id);

/// Parse only the exact lowercase, fixed-width V1 canonical or pending
/// terminal-failure leaf. No variable-width, suffixed, or case-folded alias is
/// accepted.
[[nodiscard]] std::optional<DistributedSieveParsedChunkTerminalFailureLeafV1>
parse_distributed_sieve_chunk_terminal_failure_leaf_v1(std::string_view leaf) noexcept;

/// Return the sole exact root coordinate for WaveMergeCommitV1.
[[nodiscard]] DistributedSieveWaveMergeCommitNamesV1 distributed_sieve_wave_merge_commit_names_v1();

/// Parse only `.gnfs-wave-v1.merge-commit[.pending]`.
[[nodiscard]] std::optional<DistributedSieveParsedWaveMergeCommitLeafV1>
parse_distributed_sieve_wave_merge_commit_leaf_v1(std::string_view leaf) noexcept;

/// Reserve the exact fixed-width worker and singleton merged cleanup
/// coordinates. This parser grants no cleanup or record-publication authority.
[[nodiscard]] std::optional<DistributedSieveParsedCleanupRecordLeafV1>
parse_distributed_sieve_cleanup_record_leaf_v1(std::string_view leaf) noexcept;

/// Derive the four exact root-level record leaves for one manifest-order
/// worker cleanup coordinate. This is naming only and grants no record-read,
/// publication, or cleanup authority.
[[nodiscard]] std::optional<DistributedSieveWorkerCleanupRecordNamesV1>
distributed_sieve_worker_cleanup_record_names_v1(std::uint32_t manifest_order_ordinal);

enum class DistributedSieveWaveStoreStatus : std::uint8_t {
    ready,
    interrupted,
    reconciliation_required,
    invalid_request,
    platform_unsupported,
    root_missing,
    root_invalid,
    lock_missing,
    lock_busy,
    worker_coordinator_busy,
    private_lease_root_busy,
    private_lease_lock_busy,
    lock_invalid,
    namespace_conflict,
    manifest_missing,
    manifest_conflict,
    manifest_invalid,
    publication_failed,
    durability_failed,
    io_failed,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_wave_store_status_name(DistributedSieveWaveStoreStatus status) noexcept {
    switch (status) {
    case DistributedSieveWaveStoreStatus::ready:
        return "ready";
    case DistributedSieveWaveStoreStatus::interrupted:
        return "interrupted";
    case DistributedSieveWaveStoreStatus::reconciliation_required:
        return "reconciliation_required";
    case DistributedSieveWaveStoreStatus::invalid_request:
        return "invalid_request";
    case DistributedSieveWaveStoreStatus::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveWaveStoreStatus::root_missing:
        return "root_missing";
    case DistributedSieveWaveStoreStatus::root_invalid:
        return "root_invalid";
    case DistributedSieveWaveStoreStatus::lock_missing:
        return "lock_missing";
    case DistributedSieveWaveStoreStatus::lock_busy:
        return "lock_busy";
    case DistributedSieveWaveStoreStatus::worker_coordinator_busy:
        return "worker_coordinator_busy";
    case DistributedSieveWaveStoreStatus::private_lease_root_busy:
        return "private_lease_root_busy";
    case DistributedSieveWaveStoreStatus::private_lease_lock_busy:
        return "private_lease_lock_busy";
    case DistributedSieveWaveStoreStatus::lock_invalid:
        return "lock_invalid";
    case DistributedSieveWaveStoreStatus::namespace_conflict:
        return "namespace_conflict";
    case DistributedSieveWaveStoreStatus::manifest_missing:
        return "manifest_missing";
    case DistributedSieveWaveStoreStatus::manifest_conflict:
        return "manifest_conflict";
    case DistributedSieveWaveStoreStatus::manifest_invalid:
        return "manifest_invalid";
    case DistributedSieveWaveStoreStatus::publication_failed:
        return "publication_failed";
    case DistributedSieveWaveStoreStatus::durability_failed:
        return "durability_failed";
    case DistributedSieveWaveStoreStatus::io_failed:
        return "io_failed";
    case DistributedSieveWaveStoreStatus::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWaveStoreStatus::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

/// Trusted test-only interruption boundaries. Each point is offered only after
/// the named state has been established. `ManifestCanonicalPromoted`
/// intentionally precedes the following root-directory durability barrier.
enum class DistributedSieveWaveStoreFaultPoint : std::uint8_t {
    RootDurable,
    LockDurable,
    ManifestPendingDurable,
    ManifestCanonicalPromoted,
    ManifestCanonicalDurable,
    Count,
};

/// Trusted test-only observation boundaries forwarded while `open()` consumes
/// one exact relation-layer handoff-publication resume permit. Durable points
/// are emitted only after the named mutation and durability barrier.
enum class DistributedSieveWorkerHandoffResumeObservationPointV1 : std::uint8_t {
    AfterExpectedPrefixValidated,
    BeforePendingRollbackSourceDirectorySync,
    AfterPendingRollbackSourceDirectoryDurable,
    BeforePendingRollbackDestinationDirectorySync,
    AfterPendingRollbackDestinationDirectoryDurable,
    AfterPendingRollbackPreactiveDirectoryQuarantinedDurable,
    AfterPendingRollbackPreactiveDataRemovedDurable,
    AfterPendingRollbackPreactiveIndexRemovedDurable,
    AfterPendingRollbackOwnerRemovedDurable,
    AfterPendingRollbackLeaseDirectoryRemovedDurable,
    AfterPendingRollbackReservedRemovedDurable,
    AfterPendingRollbackOwnedRemovedDurable,
    BeforePendingRollbackTombstoneRemovalValidated,
    AfterPendingRollbackTombstoneRemovedDurable,
    AfterCanonicalConfirmedDurable,
    AfterReservedRevokedDurable,
    Count,
};

struct DistributedSieveWorkerHandoffResumeTestHooksV1 final {
    using StopAfter = bool (*)(DistributedSieveWorkerHandoffResumeObservationPointV1 point,
                               void* context) noexcept;
    using AfterRoundLocksReleased = void (*)(void* context) noexcept;

    StopAfter stop_after = nullptr;
    AfterRoundLocksReleased after_round_locks_released = nullptr;
    void* context = nullptr;
};

/// Trusted test-only observation boundaries forwarded while cold `open()`
/// consumes one exact MergePrepared publication-resume permit. The values are
/// intentionally kept one-for-one with the relation-layer resume protocol.
enum class DistributedSieveMergePreparedResumeObservationPointV1 : std::uint8_t {
    AfterExpectedPrefixValidated,
    BeforePendingRollbackSourceDirectorySync,
    AfterPendingRollbackSourceDirectoryDurable,
    BeforePendingRollbackDestinationDirectorySync,
    AfterPendingRollbackDestinationDirectoryDurable,
    AfterPendingRollbackPreactiveDirectoryQuarantinedDurable,
    AfterPendingRollbackPreactiveDataRemovedDurable,
    AfterPendingRollbackPreactiveIndexRemovedDurable,
    AfterPendingRollbackOwnerRemovedDurable,
    AfterPendingRollbackLeaseDirectoryRemovedDurable,
    AfterPendingRollbackReservedRemovedDurable,
    AfterPendingRollbackOwnedRemovedDurable,
    BeforePendingRollbackTombstoneRemovalValidated,
    AfterPendingRollbackTombstoneRemovedDurable,
    AfterCanonicalConfirmedDurable,
    AfterReservedRevokedDurable,
    Count,
};

enum class DistributedSieveRecoveredPreparedPublicationSubjectV1 : std::uint8_t {
    Target,
    Worker,
    Count,
};

/// The trusted recovered-admission callback is offered exactly three times for
/// each target or worker publication. The first two relation callbacks have no
/// reader; the final callback runs only after the same-handle reader is fully
/// constructed and before permit ownership commits.
enum class DistributedSieveRecoveredPreparedAggregatePhaseV1 : std::uint8_t {
    InitialNullReader,
    ReceiptCommitNullReader,
    LiveReaderFinal,
    Count,
};

struct DistributedSieveMergePreparedResumeTestHooksV1 final {
    using StopAfter = bool (*)(DistributedSieveMergePreparedResumeObservationPointV1 point,
                               void* context) noexcept;
    using FailBefore = bool (*)(DistributedSieveMergePreparedResumeObservationPointV1 point,
                                void* context) noexcept;
    using AfterRoundLocksReleased = void (*)(void* context) noexcept;
    using StopBeforeRecoveredAggregateRevalidation = bool (*)(
        DistributedSieveRecoveredPreparedPublicationSubjectV1 subject, std::size_t manifest_slot,
        DistributedSieveRecoveredPreparedAggregatePhaseV1 phase, void* context) noexcept;

    StopAfter stop_after = nullptr;
    FailBefore fail_before = nullptr;
    AfterRoundLocksReleased after_round_locks_released = nullptr;
    /// A true result injects interruption while the current publication and all
    /// other recovered-admission locks remain held. A false result may mutate
    /// the test namespace; the production aggregate revalidation still runs
    /// immediately afterward and must detect any drift.
    StopBeforeRecoveredAggregateRevalidation stop_before_recovered_aggregate_revalidation = nullptr;
    void* context = nullptr;
};

enum class DistributedSievePrivateLeaseBaseLockSyncPoint : std::uint8_t {
    TargetInitial,
    RootDirectory,
    TargetFinal,
    Count,
};

/// Durable immutable-record boundaries for one worker-attempt start.
/// `CanonicalPromoted` intentionally precedes the following root-directory
/// durability barrier.
enum class DistributedSieveWorkerAttemptStartFaultPoint : std::uint8_t {
    PendingDurable,
    CanonicalPromoted,
    CanonicalDurable,
    Count,
};

/// Authorization outcome of one consumed reservation receipt. A durable
/// non-fresh record is deliberately distinct from both failure and a freshly
/// minted worker-start authority.
enum class DistributedSieveWorkerAttemptStartDisposition : std::uint8_t {
    failed,
    fresh_start,
    reconcile_required,
};

/// Durable immutable-record boundaries for one merge-generation start.
/// `CanonicalPromoted` intentionally precedes the following wave-root
/// directory durability barrier.
enum class DistributedSieveMergeStartFaultPointV1 : std::uint8_t {
    PendingDurable,
    CanonicalPromoted,
    CanonicalDurable,
    Count,
};

/// Durable immutable-record boundaries for the singleton wave merge commit.
/// A continuation is spent once publication is attempted, including an
/// interruption at any of these boundaries.
enum class DistributedSieveWaveMergeCommitFaultPointV1 : std::uint8_t {
    PendingDurable,
    CanonicalPromoted,
    CanonicalDurable,
    Count,
};

/// Authorization outcome of one consumed merge-generation reservation.
/// Only a fresh canonical publication retains the exact generation BaseLock.
enum class DistributedSieveMergeStartDispositionV1 : std::uint8_t {
    failed,
    fresh_start,
    reconcile_required,
};

/// Durable immutable-record boundaries for one existing worker-attempt
/// reconciliation. `CanonicalPromoted` intentionally precedes the following
/// root-directory durability barrier. `RecordNormalized` is offered only
/// after the target record is canonical-only and has survived a closed,
/// pinned, double observation.
enum class DistributedSieveWorkerAttemptReconcileFaultPoint : std::uint8_t {
    PendingDurable,
    CanonicalPromoted,
    CanonicalDurable,
    RecordNormalized,
    Count,
};

/// Durable immutable-record boundaries for one chunk-terminal-failure
/// transaction. `CanonicalPromoted` precedes the root-directory durability
/// barrier. The no-replace rename has already made the prefix canonical-only,
/// but a successful retry must still confirm that exact record and complete
/// the parent-directory durability barrier.
enum class DistributedSieveChunkTerminalFailureFaultPoint : std::uint8_t {
    PendingDurable,
    CanonicalPromoted,
    CanonicalDurable,
    Count,
};

/// Durable cleanup prefixes reported by the fixed work-package residue
/// carrier. `AfterNameUnlinked` intentionally precedes the attempt-directory
/// durability barrier.
enum class DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint : std::uint8_t {
    AfterNameUnlinked,
    AfterDirectoryDurable,
    Count,
};

/// The nine and only durable prefixes of one private-lease reservation. Values
/// intentionally mirror the source-private relation driver and are checked
/// against it in the WaveStore implementation.
enum class DistributedSievePrivateLeaseReservationBoundary : std::uint8_t {
    PermitAcquired,
    ReservedPendingDurable,
    ReservedCanonicalDurable,
    StagingDirectoryDurable,
    OwnerPendingDurable,
    OwnerCanonicalDurable,
    OwnedPendingDurable,
    OwnedCanonicalDurable,
    FinalDirectoryDurable,
    Count,
};

inline constexpr std::array DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES{
    DistributedSievePrivateLeaseReservationBoundary::PermitAcquired,
    DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable,
    DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable,
    DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
    DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable,
    DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable,
    DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable,
    DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable,
    DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable,
};

static_assert(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES.size() ==
              static_cast<std::size_t>(DistributedSievePrivateLeaseReservationBoundary::Count));

/// Durability operations that can be failed deterministically by the
/// source-private reservation test hook. The boundary supplied with the point
/// identifies the exact P1-P8 edge being synchronized.
enum class DistributedSievePrivateLeaseReservationSyncPoint : std::uint8_t {
    MarkerFileInitial,
    ParentDirectory,
    MarkerFileFinal,
    StagingDirectory,
    Count,
};

inline constexpr std::array DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_SYNC_POINTS{
    DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileInitial,
    DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
    DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileFinal,
    DistributedSievePrivateLeaseReservationSyncPoint::StagingDirectory,
};

static_assert(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_SYNC_POINTS.size() ==
              static_cast<std::size_t>(DistributedSievePrivateLeaseReservationSyncPoint::Count));

struct DistributedSievePrivateLeaseReservationSyncFailureSite final {
    DistributedSievePrivateLeaseReservationBoundary boundary =
        DistributedSievePrivateLeaseReservationBoundary::PermitAcquired;
    DistributedSievePrivateLeaseReservationSyncPoint point =
        DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileInitial;

    [[nodiscard]] friend bool
    operator==(const DistributedSievePrivateLeaseReservationSyncFailureSite&,
               const DistributedSievePrivateLeaseReservationSyncFailureSite&) = default;
};

/// Compact, process-independent inventory proof for one authenticated fixed
/// work-package residue inside a final private-lease directory. The decoded
/// work identity is validated against the manifest while observing the
/// residue, but is deliberately not retained in the closed WaveStore
/// snapshot. Native identity plus the package digest keeps replacement
/// visible across the mandatory double observations.
struct DistributedSieveWorkerWorkPackageResidueInventoryWitnessV1 final {
    std::uint64_t body_bytes = 0;
    std::uint64_t total_bytes = 0;
    util::Sha256Digest work_sha256;
    util::Sha256Digest package_sha256;
    NativeIdentityV1 file_identity;
    std::uint64_t file_extent = 0;
    std::uint64_t owner_user_id = 0;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveWorkerWorkPackageResidueInventoryWitnessV1&,
               const DistributedSieveWorkerWorkPackageResidueInventoryWitnessV1&) = default;
};

/// Exact no-delete terminal handoff observed under its persistent BaseLock.
///
/// The generic envelope binds the three immutable named leaves and the retained
/// lease markers. The decoded worker record additionally binds those native
/// facts to one exact manifest chunk and durable attempt.
struct DistributedSieveWorkerHandoffInventoryWitnessV1 final {
    WorkerHandoffV1 handoff;
    util::Sha256Digest envelope_digest;
    NativeIdentityV1 owned_marker_identity;
    util::durable_immutable_record::RecordSnapshot handoff_snapshot;
    util::durable_immutable_record::RecordSnapshot index_snapshot;
    util::durable_immutable_record::RecordSnapshot data_snapshot;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveWorkerHandoffInventoryWitnessV1& left,
               const DistributedSieveWorkerHandoffInventoryWitnessV1& right) noexcept {
        const auto& left_handoff = left.handoff;
        const auto& right_handoff = right.handoff;
        return left_handoff.manifest_digest == right_handoff.manifest_digest &&
               left_handoff.work_digest == right_handoff.work_digest &&
               left_handoff.wave_id == right_handoff.wave_id &&
               left_handoff.chunk_id == right_handoff.chunk_id &&
               left_handoff.sq_begin == right_handoff.sq_begin &&
               left_handoff.sq_end == right_handoff.sq_end &&
               left_handoff.attempt_ordinal == right_handoff.attempt_ordinal &&
               left_handoff.attempt_started_digest == right_handoff.attempt_started_digest &&
               left_handoff.lease == right_handoff.lease &&
               left_handoff.artifact == right_handoff.artifact &&
               left_handoff.processed_sq_count == right_handoff.processed_sq_count &&
               left_handoff.next_sq_index == right_handoff.next_sq_index &&
               left_handoff.completion_reason == right_handoff.completion_reason &&
               left_handoff.relation_count == right_handoff.relation_count &&
               left_handoff.cleanup_intent_absent == right_handoff.cleanup_intent_absent &&
               left_handoff.self_digest == right_handoff.self_digest &&
               left.envelope_digest == right.envelope_digest &&
               left.owned_marker_identity == right.owned_marker_identity &&
               left.handoff_snapshot == right.handoff_snapshot &&
               left.index_snapshot == right.index_snapshot &&
               left.data_snapshot == right.data_snapshot;
    }
};

/// Exact no-delete merged-corpus handoff observed under its persistent
/// BaseLock.
///
/// The generic envelope binds the three immutable named leaves and retained
/// lease markers. The decoded MergePreparedV1 additionally binds those native
/// facts to one exact MergeStarted generation. This witness is deliberately
/// read-only: observing a canonical prepared corpus is never rollback or
/// rebuild authority.
struct DistributedSieveMergePreparedInventoryWitnessV1 final {
    MergePreparedV1 prepared;
    util::Sha256Digest envelope_digest;
    NativeIdentityV1 owned_marker_identity;
    util::durable_immutable_record::RecordSnapshot handoff_snapshot;
    util::durable_immutable_record::RecordSnapshot index_snapshot;
    util::durable_immutable_record::RecordSnapshot data_snapshot;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveMergePreparedInventoryWitnessV1& left,
               const DistributedSieveMergePreparedInventoryWitnessV1& right) noexcept {
        return left.prepared.manifest_digest == right.prepared.manifest_digest &&
               left.prepared.work_digest == right.prepared.work_digest &&
               left.prepared.merge_policy_version == right.prepared.merge_policy_version &&
               left.prepared.merge_started_digest == right.prepared.merge_started_digest &&
               left.prepared.ordered_inputs == right.prepared.ordered_inputs &&
               left.prepared.input_relation_count == right.prepared.input_relation_count &&
               left.prepared.duplicate_relation_count == right.prepared.duplicate_relation_count &&
               left.prepared.output_relation_count == right.prepared.output_relation_count &&
               left.prepared.per_chunk_retained_counts ==
                   right.prepared.per_chunk_retained_counts &&
               left.prepared.merged_artifact == right.prepared.merged_artifact &&
               left.prepared.merged_lease == right.prepared.merged_lease &&
               left.prepared.self_digest == right.prepared.self_digest &&
               left.envelope_digest == right.envelope_digest &&
               left.owned_marker_identity == right.owned_marker_identity &&
               left.handoff_snapshot == right.handoff_snapshot &&
               left.index_snapshot == right.index_snapshot &&
               left.data_snapshot == right.data_snapshot;
    }
};

/// Exact read-only observation of one raw relation-corpus leaf left by the
/// merge writer before it could publish MergePreparedV1.  Contents are
/// deliberately opaque at this boundary: native identity and extent are only
/// TOCTOU/replacement detectors.  Deletion authority still comes from the
/// exact RESERVED/OWNED marker chain and latest canonical MergeStarted
/// aggregate.
struct DistributedSieveMergeRawWriterLeafInventoryWitnessV1 final {
    NativeIdentityV1 identity;
    std::uint64_t extent = 0;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveMergeRawWriterLeafInventoryWitnessV1&,
               const DistributedSieveMergeRawWriterLeafInventoryWitnessV1&) = default;
};

/// Relation-layer preactive cleanup phases that are not all expressible by
/// the ordinary P0-P8 reservation state machine.  Raw phases always contain
/// the index leaf and may contain the data leaf.  The remaining phases contain
/// neither leaf and retain the historical directory/owner identities from the
/// canonical OWNED marker even after those names have been removed.
enum class DistributedSieveMergeRawWriterRecoveryPhaseV1 : std::uint8_t {
    FinalDirectoryRawPair,
    StagingDirectoryRawPair,
    StagingDirectoryOwnerOnly,
    StagingDirectoryOwnerRemoved,
    DirectoryAbsentReservedAndOwned,
    DirectoryAbsentOwnedOnly,
    Count,
};

struct DistributedSieveMergeRawWriterRecoveryInventoryWitnessV1 final {
    DistributedSieveMergeRawWriterRecoveryPhaseV1 phase =
        DistributedSieveMergeRawWriterRecoveryPhaseV1::Count;
    std::array<std::uint64_t, 2> lease_id{};
    NativeIdentityV1 directory_identity;
    NativeIdentityV1 owner_marker_identity;
    NativeIdentityV1 owned_marker_identity;
    std::optional<NativeIdentityV1> reserved_marker_identity;
    std::optional<DistributedSieveMergeRawWriterLeafInventoryWitnessV1> index;
    std::optional<DistributedSieveMergeRawWriterLeafInventoryWitnessV1> data;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveMergeRawWriterRecoveryInventoryWitnessV1&,
               const DistributedSieveMergeRawWriterRecoveryInventoryWitnessV1&) = default;
};

struct DistributedSievePrivateLeaseReservationInventoryWitness final {
    std::string base_lock_leaf;
    DistributedSievePrivateLeaseReservationBoundary boundary =
        DistributedSievePrivateLeaseReservationBoundary::PermitAcquired;
    std::array<std::uint64_t, 2> lease_id{};
    std::optional<NativeIdentityV1> reserved_marker_identity;
    std::optional<NativeIdentityV1> directory_identity;
    std::optional<NativeIdentityV1> owner_marker_identity;
    std::optional<NativeIdentityV1> owned_marker_identity;
    std::optional<DistributedSieveWorkerWorkPackageResidueInventoryWitnessV1> work_package_residue;
    std::optional<DistributedSieveWorkerHandoffInventoryWitnessV1> worker_handoff;
    std::optional<DistributedSieveMergePreparedInventoryWitnessV1> merge_prepared;
    std::optional<DistributedSieveMergeRawWriterRecoveryInventoryWitnessV1>
        merge_raw_writer_recovery;

    [[nodiscard]] friend bool
    operator==(const DistributedSievePrivateLeaseReservationInventoryWitness&,
               const DistributedSievePrivateLeaseReservationInventoryWitness&) = default;
};

/// Exact manifest-bound observation of one immutable AttemptStartedV1 slot.
///
/// `bytes` are the canonical protocol encoding shared by the canonical and
/// optional duplicate-pending leaves. Native snapshots keep same-byte inode
/// replacement visible to every root-claim inventory comparison.
struct DistributedSieveWorkerAttemptRecordInventoryWitness final {
    std::uint32_t chunk_id = 0;
    std::uint32_t attempt_ordinal = 0;
    AttemptStartedV1 record;
    std::vector<std::byte> bytes;
    std::optional<util::durable_immutable_record::RecordSnapshot> canonical_snapshot;
    std::optional<util::durable_immutable_record::RecordSnapshot> pending_snapshot;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveWorkerAttemptRecordInventoryWitness& left,
               const DistributedSieveWorkerAttemptRecordInventoryWitness& right) noexcept {
        return left.chunk_id == right.chunk_id && left.attempt_ordinal == right.attempt_ordinal &&
               left.bytes == right.bytes && left.canonical_snapshot == right.canonical_snapshot &&
               left.pending_snapshot == right.pending_snapshot;
    }
};

/// Exact manifest-bound observation of one immutable MergeStartedV1 slot.
///
/// Canonical and optional duplicate-pending leaves must carry identical sealed
/// bytes. Native snapshots keep same-byte inode replacement visible while the
/// merged generation is reserved, built, or reconciled.
struct DistributedSieveMergeStartedRecordInventoryWitnessV1 final {
    std::uint32_t merge_attempt_ordinal = 0;
    MergeStartedV1 record;
    std::vector<std::byte> bytes;
    std::optional<util::durable_immutable_record::RecordSnapshot> canonical_snapshot;
    std::optional<util::durable_immutable_record::RecordSnapshot> pending_snapshot;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveMergeStartedRecordInventoryWitnessV1& left,
               const DistributedSieveMergeStartedRecordInventoryWitnessV1& right) noexcept {
        return left.merge_attempt_ordinal == right.merge_attempt_ordinal &&
               left.bytes == right.bytes && left.canonical_snapshot == right.canonical_snapshot &&
               left.pending_snapshot == right.pending_snapshot;
    }
};

/// Exact manifest-bound observation of one root-level
/// ChunkTerminalFailureV1 prefix.
///
/// The sealed bytes are shared by canonical and optional duplicate-pending
/// leaves. Native snapshots make same-byte inode replacement visible to every
/// retained root-claim baseline.
struct DistributedSieveChunkTerminalFailureRecordInventoryWitnessV1 final {
    std::uint32_t chunk_id = 0;
    ChunkTerminalFailureV1 record;
    std::vector<std::byte> bytes;
    std::optional<util::durable_immutable_record::RecordSnapshot> canonical_snapshot;
    std::optional<util::durable_immutable_record::RecordSnapshot> pending_snapshot;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveChunkTerminalFailureRecordInventoryWitnessV1& left,
               const DistributedSieveChunkTerminalFailureRecordInventoryWitnessV1& right) noexcept {
        return left.chunk_id == right.chunk_id && left.bytes == right.bytes &&
               left.canonical_snapshot == right.canonical_snapshot &&
               left.pending_snapshot == right.pending_snapshot;
    }
};

/// Exact root-level WaveMergeCommitV1 prefix. Canonical and optional duplicate
/// pending leaves must carry the same sealed bytes.
struct DistributedSieveWaveMergeCommitRecordInventoryWitnessV1 final {
    WaveMergeCommitV1 record;
    std::vector<std::byte> bytes;
    std::optional<util::durable_immutable_record::RecordSnapshot> canonical_snapshot;
    std::optional<util::durable_immutable_record::RecordSnapshot> pending_snapshot;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveWaveMergeCommitRecordInventoryWitnessV1& left,
               const DistributedSieveWaveMergeCommitRecordInventoryWitnessV1& right) noexcept {
        return left.bytes == right.bytes && left.canonical_snapshot == right.canonical_snapshot &&
               left.pending_snapshot == right.pending_snapshot;
    }
};

struct DistributedSieveWaveStoreInventoryTestHooks final {
    using ObserveReservationWitnesses =
        void (*)(std::span<const DistributedSievePrivateLeaseReservationInventoryWitness> witnesses,
                 void* context) noexcept;
    using AfterFirstValidation = void (*)(void* context) noexcept;

    ObserveReservationWitnesses observe_reservation_witnesses = nullptr;
    AfterFirstValidation after_first_validation = nullptr;
    void* context = nullptr;
};

/// Trusted test-only boundary for same-handle worker-handoff adoption.
/// Production callers leave the callback empty.
struct DistributedSieveWorkerHandoffAdoptionTestHooksV1 final {
    using BeforeFinalNamespaceRevalidation = void (*)(void* context) noexcept;

    /// Runs after the exact relation corpus has been streamed and verified,
    /// immediately before the final root, lease-marker, directory, and named
    /// artifact checks.
    BeforeFinalNamespaceRevalidation before_final_namespace_revalidation = nullptr;
    void* context = nullptr;
};

/// Trusted test-only boundaries for the attempt BaseLockAt transaction. The
/// callbacks run inside the short-lived same-State root claim. Production
/// callers leave them empty.
struct DistributedSievePrivateLeaseBaseLockTestHooks final {
    using Boundary = void (*)(void* context) noexcept;
    using FailBeforeSync = bool (*)(DistributedSievePrivateLeaseBaseLockSyncPoint point,
                                    void* context) noexcept;

    /// Runs after the first closed phase witness and before the final
    /// authority/inventory check that immediately precedes target mutation.
    Boundary after_initial_phase_validation = nullptr;

    /// Runs after the exact target descriptor is flocked and before its
    /// held/named identity is accepted.
    Boundary after_target_lock_acquired = nullptr;

    /// Runs after the first successful retained-target revalidation and before
    /// the immediately following WaveStore-authority revalidation.
    Boundary after_target_revalidation = nullptr;

    /// Deterministically fail one durability barrier before issuing its sync.
    /// This hook exists only to prove preservation and explicit recovery of
    /// every permanent-BaseLock durability prefix.
    FailBeforeSync fail_before_sync = nullptr;

    void* context = nullptr;
};

/// Trusted test-only boundaries for the receipt-only AttemptStarted publisher.
/// Production callers leave every callback empty.
struct DistributedSieveWorkerAttemptStartTestHooks final {
    using Boundary = void (*)(void* context) noexcept;
    using StopAfter = bool (*)(DistributedSieveWorkerAttemptStartFaultPoint point,
                               void* context) noexcept;

    DistributedSievePrivateLeaseBaseLockTestHooks base_lock;

    /// Runs with `root claim -> target BaseLock` held after the exact receipt,
    /// P8 lease, and predecessor chain have been established.
    Boundary after_locked_predecessor_validation = nullptr;

    /// Runs after the final closed predecessor confirmation and immediately
    /// before the production immutable-record transaction. This trusted seam
    /// exists only to exercise the unavoidable final POSIX namespace race;
    /// the callback receives no path, descriptor, record, or authority.
    Boundary before_record_publication = nullptr;

    StopAfter stop_after = nullptr;

    /// Runs after the first exact canonical-successor observation and before
    /// its mandatory authority and inventory confirmation.
    Boundary after_first_successor_validation = nullptr;
    void* context = nullptr;
};

/// Trusted test-only boundaries for the receipt-only MergeStarted publisher.
/// Production callers leave every callback empty.
struct DistributedSieveMergeStartTestHooksV1 final {
    using Boundary = void (*)(void* context) noexcept;
    using StopAfter = bool (*)(DistributedSieveMergeStartFaultPointV1 point,
                               void* context) noexcept;

    DistributedSievePrivateLeaseBaseLockTestHooks base_lock;

    /// Runs with `root claim -> predecessor generation BaseLocks -> target
    /// BaseLock` held after the exact receipt, P8 lease, terminal worker
    /// evidence, and predecessor chain have been established.
    Boundary after_locked_predecessor_validation = nullptr;

    /// Runs after the final closed predecessor confirmation and immediately
    /// before the immutable-record transaction.
    Boundary before_record_publication = nullptr;

    StopAfter stop_after = nullptr;

    /// Runs after the first exact canonical successor observation and before
    /// its mandatory authority and inventory confirmation.
    Boundary after_first_successor_validation = nullptr;
    void* context = nullptr;
};

/// Trusted test-only boundaries for the singleton WaveMergeCommitV1
/// publisher. No callback receives a path, descriptor, or record payload.
struct DistributedSieveWaveMergeCommitTestHooksV1 final {
    using Boundary = void (*)(void* context) noexcept;
    using StopAfter = bool (*)(DistributedSieveWaveMergeCommitFaultPointV1 point,
                               void* context) noexcept;

    Boundary before_record_publication = nullptr;
    StopAfter stop_after = nullptr;
    Boundary after_first_successor_validation = nullptr;
    void* context = nullptr;
};

struct DistributedSieveWaveStoreTestHooks final {
    using StopAfter = bool (*)(DistributedSieveWaveStoreFaultPoint point, void* context) noexcept;

    StopAfter stop_after = nullptr;
    DistributedSieveMergePreparedResumeTestHooksV1 merge_prepared_resume;
    DistributedSieveWorkerHandoffResumeTestHooksV1 worker_handoff_resume;
    DistributedSieveWaveMergeCommitTestHooksV1 wave_merge_commit;
    void* context = nullptr;
};

/// Trusted test-only boundaries for the retained-admission terminal publisher.
/// Production callers leave every callback empty.
struct DistributedSieveChunkTerminalFailureTestHooksV1 final {
    using Boundary = void (*)(void* context) noexcept;
    using StopAfter = bool (*)(DistributedSieveChunkTerminalFailureFaultPoint point,
                               void* context) noexcept;

    /// Runs with the original root claim and final-attempt BaseLock still held,
    /// after the exact P0 predecessor and complete attempt chain have been
    /// established.
    Boundary before_record_publication = nullptr;
    StopAfter stop_after = nullptr;

    /// Runs after the first exact canonical-only successor observation and
    /// before mandatory same-lock authority and inventory confirmation.
    Boundary after_first_successor_validation = nullptr;
    void* context = nullptr;
};

struct DistributedSieveWorkerAttemptStartReceiptTestHooks final {
    using AfterFirstValidation = void (*)(void* context) noexcept;

    AfterFirstValidation after_first_validation = nullptr;
    void* context = nullptr;
};

/// Trusted test-only boundary for a returned attempt-bound root claim.
struct DistributedSievePrivateLeaseRootClaimTestHooks final {
    using Boundary = void (*)(void* context) noexcept;

    /// Runs once after WaveStore authority succeeds and before the retained
    /// target is checked. The claim must revalidate authority again afterward.
    Boundary after_first_authority_validation = nullptr;
    void* context = nullptr;
};

/// Trusted test-only controls for the held-dirfd private-lease phase driver.
///
/// `stop_after` is offered only after the named prefix has survived its
/// durability barrier and a closed successor revalidation. The predecessor
/// callback runs after the same closed validation immediately before the one
/// permitted successor mutation.
struct DistributedSievePrivateLeaseProtocolTestHooks final {
    using StopAfter = bool (*)(DistributedSievePrivateLeaseReservationBoundary boundary,
                               void* context) noexcept;
    using AfterPredecessorValidation =
        void (*)(DistributedSievePrivateLeaseReservationBoundary successor, void* context) noexcept;
    using AfterFirstSuccessorValidation =
        void (*)(DistributedSievePrivateLeaseReservationBoundary successor, void* context) noexcept;
    using FailBeforeSync = bool (*)(DistributedSievePrivateLeaseReservationBoundary successor,
                                    DistributedSievePrivateLeaseReservationSyncPoint point,
                                    void* context) noexcept;
    using AfterInjectedSyncFailure = void (*)(
        DistributedSievePrivateLeaseReservationSyncFailureSite site, void* context) noexcept;

    StopAfter stop_after = nullptr;
    AfterPredecessorValidation after_predecessor_validation = nullptr;

    /// Runs after the first exact successor observation and before the second
    /// authority/successor validation. It exists only to prove that a
    /// same-byte replacement cannot inherit this transaction's identity.
    AfterFirstSuccessorValidation after_first_successor_validation = nullptr;

    /// Select one durability result to fail. Selection runs while the exact
    /// predecessor is still closed; the production sync still executes, then
    /// its result is replaced with a fixed durability failure and adjudicated
    /// through the normal visible-successor path.
    FailBeforeSync fail_before_sync = nullptr;

    /// Runs only after a selected production sync has completed and its
    /// result is irreversibly fixed to failure. The callback cannot restore
    /// success; mandatory authority/target/successor adjudication follows.
    AfterInjectedSyncFailure after_injected_sync_failure = nullptr;
    void* context = nullptr;
};

struct DistributedSievePrivateLeaseReservationReceiptTestHooks final {
    using AfterFirstTargetValidation = void (*)(void* context) noexcept;

    /// Runs after the first exact target observation and before the second
    /// authority/target validation.
    AfterFirstTargetValidation after_first_target_validation = nullptr;
    void* context = nullptr;
};

struct DistributedSievePrivateLeaseRecoveryEdge final {
    DistributedSievePrivateLeaseReservationBoundary source =
        DistributedSievePrivateLeaseReservationBoundary::Count;
    DistributedSievePrivateLeaseReservationBoundary successor =
        DistributedSievePrivateLeaseReservationBoundary::Count;

    friend bool operator==(const DistributedSievePrivateLeaseRecoveryEdge&,
                           const DistributedSievePrivateLeaseRecoveryEdge&) = default;
};

inline constexpr std::array DISTRIBUTED_SIEVE_PRIVATE_LEASE_RECOVERY_EDGES{
    DistributedSievePrivateLeaseRecoveryEdge{
        .source = DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable,
        .successor = DistributedSievePrivateLeaseReservationBoundary::PermitAcquired,
    },
    DistributedSievePrivateLeaseRecoveryEdge{
        .source = DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable,
        .successor = DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable,
    },
    DistributedSievePrivateLeaseRecoveryEdge{
        .source = DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
        .successor = DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable,
    },
    DistributedSievePrivateLeaseRecoveryEdge{
        .source = DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable,
        .successor = DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
    },
    DistributedSievePrivateLeaseRecoveryEdge{
        .source = DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable,
        .successor = DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable,
    },
    DistributedSievePrivateLeaseRecoveryEdge{
        .source = DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable,
        .successor = DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable,
    },
    DistributedSievePrivateLeaseRecoveryEdge{
        .source = DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable,
        .successor = DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable,
    },
    DistributedSievePrivateLeaseRecoveryEdge{
        .source = DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable,
        .successor = DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable,
    },
};

/// Trusted test-only interruption boundary for open-existing rollback.
///
/// `stop_after` is offered only after the named reverse successor has survived
/// its parent-directory durability barrier and two closed successor
/// observations. Production callers leave it empty.
struct DistributedSievePrivateLeaseRecoveryTestHooks final {
    using StopAfter = bool (*)(DistributedSievePrivateLeaseReservationBoundary boundary,
                               void* context) noexcept;
    using FailBeforeSync = bool (*)(DistributedSievePrivateLeaseRecoveryEdge edge,
                                    void* context) noexcept;
    using AfterFirstSuccessorValidation = void (*)(DistributedSievePrivateLeaseRecoveryEdge edge,
                                                   void* context) noexcept;
    using BeforeStagingDirectoryRemove = void (*)(DistributedSievePrivateLeaseRecoveryEdge edge,
                                                  void* context) noexcept;

    StopAfter stop_after = nullptr;

    /// Selects an edge while its exact predecessor is still closed. The real
    /// production sync still runs; only a successful sync is then fixed to a
    /// durability failure and adjudicated through the ordinary recovery path.
    FailBeforeSync fail_before_sync = nullptr;

    /// Runs after the first exact closed-successor and held-object
    /// observation, before mandatory authority and successor confirmation.
    AfterFirstSuccessorValidation after_first_successor_validation = nullptr;

    /// Runs at the exact P3 predecessor after the final held-directory
    /// validation and immediately before rmdir. It exists only to exercise the
    /// unavoidable POSIX final namespace race with a closed predecessor.
    BeforeStagingDirectoryRemove before_staging_directory_remove = nullptr;
    void* context = nullptr;
};

/// Exact relation-layer checkpoints used only by typed recovery of an
/// unsealed merge corpus.  The permit point is offered after relation has
/// matched the complete expectation and before its first mutation; the
/// remaining points follow their named durability barriers.
enum class DistributedSieveMergeRawWriterRecoveryFaultPointV1 : std::uint8_t {
    RecoveryPermitAcquired,
    PreactiveDirectoryQuarantinedDurable,
    PreactiveDataRemovedDurable,
    PreactiveIndexRemovedDurable,
    OwnerRemovedDurable,
    FinalDirectoryRemovedDurable,
    ReservedRemovedDurable,
    OwnedRemovedDurable,
    Count,
};

struct DistributedSieveMergeStartedReconcileTestHooksV1 final {
    using Boundary = void (*)(void* context) noexcept;
    using StopAfter = bool (*)(DistributedSieveMergeStartFaultPointV1 point,
                               void* context) noexcept;
    using RawRecoveryStopAfter = bool (*)(DistributedSieveMergeRawWriterRecoveryFaultPointV1 point,
                                          void* context) noexcept;

    DistributedSievePrivateLeaseBaseLockTestHooks base_lock;
    Boundary before_record_normalization = nullptr;
    StopAfter stop_after = nullptr;
    Boundary after_first_normalized_successor_validation = nullptr;
    DistributedSievePrivateLeaseRecoveryTestHooks recovery;
    RawRecoveryStopAfter raw_recovery_stop_after = nullptr;
    Boundary after_first_raw_recovery_successor_validation = nullptr;
    void* context = nullptr;
};

/// Trusted test-only controls for normalization and cleanup of one already
/// published AttemptStartedV1. Production callers leave every callback empty.
struct DistributedSieveWorkerWorkPackageResidueReconciliationTestHooks final {
    using Boundary = void (*)(void* context) noexcept;
    using FailBeforeDirectorySync = bool (*)(void* context) noexcept;

    /// Runs before the final full claim, AttemptStarted, and final-directory
    /// revalidation that immediately precedes the fixed carrier call.
    Boundary before_reconciliation = nullptr;

    /// Static carrier fault selection. No callback runs between the final
    /// WaveStore validation and unlink; the carrier owns that closed interval.
    FailBeforeDirectorySync fail_before_directory_sync = nullptr;
    std::optional<DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint> stop_after;

    /// Runs after the first exact residue-free successor observation and
    /// before its mandatory full confirmation.
    Boundary after_first_successor_validation = nullptr;
    void* context = nullptr;
};

struct DistributedSieveWorkerAttemptReconcileTestHooks final {
    using Boundary = void (*)(void* context) noexcept;
    using StopAfter = bool (*)(DistributedSieveWorkerAttemptReconcileFaultPoint point,
                               void* context) noexcept;

    DistributedSieveWorkerWorkPackageResidueReconciliationTestHooks work_package_residue;

    /// Runs after the final closed initial-shape confirmation and immediately
    /// before the production immutable-record transaction.
    Boundary before_record_normalization = nullptr;

    StopAfter stop_after = nullptr;

    /// Runs after the first canonical-only successor, exact record pin, and
    /// full inventory validation, before the mandatory confirmation.
    Boundary after_first_normalized_successor_validation = nullptr;

    DistributedSievePrivateLeaseRecoveryTestHooks recovery;
    void* context = nullptr;
};

struct DistributedSieveWaveStoreDiagnostic final {
    DistributedSieveWaveStoreStatus status = DistributedSieveWaveStoreStatus::ready;
    std::error_code native_error;
    std::optional<DistributedSieveProtocolStatus> protocol_status;
    std::optional<util::durable_immutable_record::RecordPublishStatus> publication_status;
    std::optional<util::durable_immutable_record::RecordPublishDisposition> publication_disposition;
    std::optional<DistributedSieveWaveStoreFaultPoint> last_durable_fault_point;
    std::optional<DistributedSieveWorkerAttemptStartFaultPoint>
        last_worker_attempt_start_fault_point;
    std::optional<DistributedSieveMergeStartFaultPointV1> last_merge_start_fault_point;
    std::optional<DistributedSieveWaveMergeCommitFaultPointV1> last_wave_merge_commit_fault_point;
    std::optional<DistributedSieveWorkerAttemptReconcileFaultPoint>
        last_worker_attempt_reconcile_fault_point;
    std::optional<DistributedSieveChunkTerminalFailureFaultPoint>
        last_chunk_terminal_failure_fault_point;
    std::optional<DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint>
        last_worker_work_package_residue_reconciliation_fault_point;
    std::optional<DistributedSievePrivateLeaseBaseLockSyncPoint>
        failed_private_lease_base_lock_sync_point;
    std::optional<DistributedSievePrivateLeaseReservationBoundary>
        last_private_lease_reservation_boundary;
    std::optional<DistributedSievePrivateLeaseReservationBoundary>
        last_private_lease_recovery_boundary;
    std::optional<DistributedSievePrivateLeaseRecoveryEdge> failed_private_lease_recovery_sync_edge;
    std::optional<DistributedSieveMergeRawWriterRecoveryFaultPointV1>
        last_merge_raw_writer_recovery_fault_point;
    std::optional<DistributedSievePrivateLeaseReservationSyncFailureSite>
        failed_private_lease_reservation_sync_site;
};

/// One exact immutable worker-cleanup leaf read relative to an already-held
/// wave-root descriptor. The decoded variant must agree with the reserved leaf
/// role. This witness is observation only: it grants neither publication nor
/// relation cleanup authority.
struct DistributedSieveWorkerCleanupRecordLeafWitnessV1 final {
    DistributedSieveParsedCleanupRecordLeafV1 coordinate;
    std::variant<ArtifactCleanupAuthorizedV1, ArtifactCleanupCompletedV1> record;
    std::vector<std::byte> bytes;
    util::durable_immutable_record::RecordSnapshot snapshot;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveWorkerCleanupRecordLeafWitnessV1& left,
               const DistributedSieveWorkerCleanupRecordLeafWitnessV1& right) noexcept {
        return left.coordinate == right.coordinate && left.bytes == right.bytes &&
               left.snapshot == right.snapshot;
    }
};

struct DistributedSieveWorkerCleanupRecordLeafLoadResultV1 final {
    std::optional<DistributedSieveWorkerCleanupRecordLeafWitnessV1> witness;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return witness.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

enum class DistributedSieveWorkerCleanupPrefixStateV1 : std::uint8_t {
    authorization_pending_only,
    authorization_canonical_only,
    authorization_identical_dual,
    completion_pending_only,
    completion_identical_dual,
    completed,
};

/// Exact canonical/pending root prefix for one nonempty manifest worker.
/// `completed` requires canonical-only authorization and completion; every
/// other state is the sole active cleanup frontier for the whole wave.
struct DistributedSieveWorkerCleanupCoordinateWitnessV1 final {
    std::uint32_t manifest_order_ordinal = 0;
    DistributedSieveWorkerCleanupPrefixStateV1 state =
        DistributedSieveWorkerCleanupPrefixStateV1::authorization_pending_only;
    ArtifactCleanupAuthorizedV1 authorization;
    std::vector<std::byte> authorization_bytes;
    std::optional<util::durable_immutable_record::RecordSnapshot> authorization_canonical_snapshot;
    std::optional<util::durable_immutable_record::RecordSnapshot> authorization_pending_snapshot;
    std::optional<ArtifactCleanupCompletedV1> completion;
    std::vector<std::byte> completion_bytes;
    std::optional<util::durable_immutable_record::RecordSnapshot> completion_canonical_snapshot;
    std::optional<util::durable_immutable_record::RecordSnapshot> completion_pending_snapshot;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveWorkerCleanupCoordinateWitnessV1& left,
               const DistributedSieveWorkerCleanupCoordinateWitnessV1& right) noexcept {
        return left.manifest_order_ordinal == right.manifest_order_ordinal &&
               left.state == right.state && left.authorization_bytes == right.authorization_bytes &&
               left.authorization_canonical_snapshot == right.authorization_canonical_snapshot &&
               left.authorization_pending_snapshot == right.authorization_pending_snapshot &&
               left.completion_bytes == right.completion_bytes &&
               left.completion_canonical_snapshot == right.completion_canonical_snapshot &&
               left.completion_pending_snapshot == right.completion_pending_snapshot;
    }
};

/// Strict manifest-order cleanup prefix. `frontier_manifest_order_ordinal` is
/// the first nonempty worker not durably completed, even when it has no root
/// record yet. `active_manifest_order_ordinal` is populated only when that
/// frontier already has an authorization/completion prefix.
struct DistributedSieveWorkerCleanupPrefixWitnessV1 final {
    std::vector<DistributedSieveWorkerCleanupCoordinateWitnessV1> coordinates;
    std::size_t completed_worker_count = 0;
    std::optional<std::uint32_t> frontier_manifest_order_ordinal;
    std::optional<std::uint32_t> active_manifest_order_ordinal;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveWorkerCleanupPrefixWitnessV1&,
               const DistributedSieveWorkerCleanupPrefixWitnessV1&) = default;
};

struct DistributedSieveWorkerCleanupPrefixClassificationResultV1 final {
    std::optional<DistributedSieveWorkerCleanupPrefixWitnessV1> prefix;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return prefix.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

/// Trusted observation seam for the source-private worker-cleanup cold open.
/// Production callers leave the callback empty. The callback runs only after
/// the first complete immutable observation has released every temporary
/// worker/merged BaseLock and before the mandatory second observation.
struct DistributedSieveWorkerCleanupRootOpenTestHooksV1 final {
    using Boundary = void (*)(void* context) noexcept;

    Boundary after_first_observation = nullptr;
    void* context = nullptr;
};

/// Exact predecessor-generation anchor accepted only by the committed-tail
/// cleanup bridge. The ordinary cleanup-root cold open deliberately supplies
/// no anchor. Every identity and byte vector is copied before the old lock
/// generation is released, then compared while the new WaveLock is held.
struct DistributedSieveWorkerCleanupRootExactAnchorV1 final {
    NativeIdentityV1 wave_root_identity;
    NativeIdentityV1 permanent_lock_identity;
    util::durable_immutable_record::RecordSnapshot manifest_snapshot;
    std::vector<std::byte> manifest_bytes;
    util::Sha256Digest manifest_digest;
    util::durable_immutable_record::RecordSnapshot merge_commit_snapshot;
    std::vector<std::byte> merge_commit_bytes;
    util::Sha256Digest merge_commit_digest;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveWorkerCleanupRootExactAnchorV1&,
               const DistributedSieveWorkerCleanupRootExactAnchorV1&) = default;
};

struct DistributedSieveWorkerCleanupRootExactAnchorCaptureResultV1 final {
    std::optional<DistributedSieveWorkerCleanupRootExactAnchorV1> anchor;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return anchor.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

/// Read and decode exactly one reserved worker cleanup leaf. Merged
/// coordinates, malformed aliases, role mismatches, and record replacement
/// during the held/named observation fail closed.
[[nodiscard]] DistributedSieveWorkerCleanupRecordLeafLoadResultV1
load_distributed_sieve_worker_cleanup_record_leaf_v1(int wave_root_fd, std::string_view leaf,
                                                     std::uint64_t creator_process_id) noexcept;

/// Validate a complete set of already-loaded cleanup leaves against one exact
/// canonical manifest/merge commit and its manifest-slot worker handoffs.
/// Empty chunks admit no records; completed workers form a canonical-only
/// prefix; at most one non-completed coordinate may be active; all later
/// worker coordinates must be absent. This recovery classifier never mints
/// first-use cleanup authority.
[[nodiscard]] DistributedSieveWorkerCleanupPrefixClassificationResultV1
classify_distributed_sieve_worker_cleanup_prefix_v1(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    std::span<const WorkerHandoffV1* const> worker_handoffs,
    std::span<const DistributedSieveWorkerCleanupRecordLeafWitnessV1> records) noexcept;

struct DistributedSieveWaveStoreOpenResult;
struct DistributedSieveWorkerCleanupRootOpenResultV1;
struct DistributedSievePrivateLeaseRootClaimResult;
struct DistributedSievePrivateLeaseReservationResult;
struct DistributedSieveWorkerAttemptStartResult;
struct DistributedSieveMergeLeaseReservationResultV1;
struct DistributedSieveMergeStartResultV1;
struct DistributedSieveMergeStartedWriterMintResultV1;
struct DistributedSieveMergeStartedReconcileResultV1;
struct DistributedSieveMergeGenerationCursorResultV1;
struct DistributedSieveWorkerAttemptReconcileResult;
struct DistributedSieveChunkTerminalFailurePublicationResultV1;
struct DistributedSieveWaveMergeCommitPublicationResultV1;
struct DistributedSieveWorkerChunkInventoryResultV1;
struct DistributedSieveWorkerHandoffAdoptionResultV1;
struct DistributedSieveWorkerCoordinatorClaimResultV1;
class DistributedSieveAdoptedWorkerChunkV1;
class DistributedSieveWorkerCoordinatorClaimV1;
class DistributedSieveWaveStore;
class DistributedSieveWorkerCleanupRootAdmissionV1;
class DistributedSievePrivateLeaseRootClaim;
class DistributedSievePrivateLeaseBaseLockAt;
class DistributedSievePrivateLeaseReservationReceipt;
class DistributedSieveWorkerAttemptStartReceipt;
class DistributedSieveMergeLeaseReservationReceiptV1;
class DistributedSieveMergeStartedReceiptV1;
class DistributedSieveMergeStartedWriterMintV1;
class DistributedSieveChunkTerminalFailureAdmissionV1;
class DistributedSieveFdPrivateLeaseReservationTarget;
class DistributedSieveFdPrivateLeaseRecoveryTarget;
class MergePreparedAdmissionRevalidatorAuthorityV1;
class WorkerCleanupRootRevalidatorAuthorityV1;

[[nodiscard]] DistributedSieveMergeStartedWriterMintResultV1
consume_distributed_sieve_merge_started_writer_v1(
    DistributedSieveMergeStartedReceiptV1&& receipt) noexcept;

[[nodiscard]] DistributedSievePrivateLeaseReservationResult reserve_worker_attempt_private_lease(
    DistributedSievePrivateLeaseRootClaimResult&& claimed,
    DistributedSievePrivateLeaseProtocolTestHooks hooks = {}) noexcept;

/// Consume only an open-existing attempt claim and roll its exact P0-P8
/// reservation prefix back to P0. Success returns the same live root
/// claim/target flock with a refreshed P0 witness, ready for a fresh
/// reservation. Every failure and interruption releases both locks.
[[nodiscard]] DistributedSievePrivateLeaseRootClaimResult recover_worker_attempt_private_lease(
    DistributedSievePrivateLeaseRootClaimResult&& claimed,
    DistributedSievePrivateLeaseRecoveryTestHooks hooks = {}) noexcept;

/// Consume one open-existing recordless merge-generation claim and roll its
/// exact reservation prefix back to P0. This helper never removes a
/// MergeStarted record and is not cleanup authority for a started generation.
[[nodiscard]] DistributedSievePrivateLeaseRootClaimResult recover_merge_generation_private_lease_v1(
    DistributedSievePrivateLeaseRootClaimResult&& claimed,
    DistributedSievePrivateLeaseRecoveryTestHooks hooks = {}) noexcept;

/// Consume one creator-bound P8 reservation snapshot, reacquire
/// `root claim -> target BaseLock`, and durably publish its internally derived
/// AttemptStartedV1. Only a fresh canonical publication can mint the
/// target-lock-retaining start receipt.
[[nodiscard]] DistributedSieveWorkerAttemptStartResult
publish_worker_attempt_started(DistributedSievePrivateLeaseReservationReceipt&& reservation,
                               DistributedSieveWorkerAttemptStartTestHooks hooks = {}) noexcept;

/// Derive and reserve one exact merge-generation private lease through the
/// retained WaveStore. The terminal projection and policy are copied into the
/// move-only P8 receipt only after they match the complete durable worker
/// evidence under `WaveLock -> root claim -> predecessor generation BaseLocks
/// -> target BaseLock`.
[[nodiscard]] DistributedSieveMergeLeaseReservationResultV1
reserve_distributed_sieve_merge_generation_v1(
    DistributedSieveWaveStore& store, std::uint32_t merge_attempt_ordinal,
    std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
    DistributedSievePrivateLeaseProtocolTestHooks hooks = {},
    std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs =
        {}) noexcept;

/// Consume one exact merge-generation P8 receipt and durably publish the
/// internally derived MergeStartedV1. The repeated inputs and policy must
/// exactly match the values frozen by reservation.
[[nodiscard]] DistributedSieveMergeStartResultV1 publish_merge_started_v1(
    DistributedSieveMergeLeaseReservationReceiptV1&& reservation,
    std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
    DistributedSieveMergeStartTestHooksV1 hooks = {},
    std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs =
        {}) noexcept;

/// Normalize one exact latest MergeStarted prefix, pin its canonical record
/// under the same generation BaseLock, and roll only its unprepared private
/// lease back to P0. The durable start remains as an audit/predecessor leaf.
[[nodiscard]] DistributedSieveMergeStartedReconcileResultV1 reconcile_merge_started_generation_v1(
    DistributedSieveWaveStore& store, std::uint32_t merge_attempt_ordinal,
    DistributedSieveMergeStartedReconcileTestHooksV1 hooks = {},
    std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs =
        {}) noexcept;

/// Return the exact next merge ordinal after reconciling and cleaning an
/// unprepared latest start when necessary. Reservation-only prefixes are left
/// for the reservation transaction, which reuses the same ordinal.
[[nodiscard]] DistributedSieveMergeGenerationCursorResultV1
prepare_distributed_sieve_merge_generation_v1(
    DistributedSieveWaveStore& store, std::span<const DistributedSieveAdoptedWorkerChunkV1* const>
                                          held_worker_handoffs = {}) noexcept;

/// Consume only an open-existing attempt claim, normalize its immutable
/// AttemptStartedV1 to one canonical record, and roll the exact record-bound
/// private lease back to P0. Ordinary reconciled facts are read-only and grant
/// no filesystem authority. When the final allowed attempt reaches P0, the
/// result may additionally return one move-only terminal admission that
/// retains the root claim and same-open-file-description BaseLock solely for
/// the typed terminal publisher. It never grants cleanup or worker-start
/// authority.
[[nodiscard]] DistributedSieveWorkerAttemptReconcileResult reconcile_worker_attempt_started(
    DistributedSievePrivateLeaseRootClaimResult&& claimed,
    DistributedSieveWorkerAttemptReconcileTestHooks hooks = {}) noexcept;

/// Consume the same-lock admission minted only when the final durable attempt
/// has converged to P0, then create or normalize the exact root-level terminal
/// record. The caller supplies no record fields, path, reason, or digest.
[[nodiscard]] DistributedSieveChunkTerminalFailurePublicationResultV1
publish_chunk_terminal_failure_v1(
    DistributedSieveChunkTerminalFailureAdmissionV1&& admission,
    DistributedSieveChunkTerminalFailureTestHooksV1 hooks = {}) noexcept;

/// A process-bound lease on one frozen wave root and its permanent lock.
///
/// The object is deliberately non-copyable and non-movable. Its shared backing
/// state closes the lock descriptor only after the store and any future typed
/// authorization anchors have released it. `revalidate()` is fail-closed and
/// rejects use after fork.
class DistributedSieveWaveStore final {
public:
    DistributedSieveWaveStore(const DistributedSieveWaveStore&) = delete;
    DistributedSieveWaveStore& operator=(const DistributedSieveWaveStore&) = delete;
    DistributedSieveWaveStore(DistributedSieveWaveStore&&) = delete;
    DistributedSieveWaveStore& operator=(DistributedSieveWaveStore&&) = delete;
    ~DistributedSieveWaveStore();

    /// Create or idempotently recover one wave. `absolute_root` must already be
    /// in strict component form: no empty, '.', '..', repeated-separator,
    /// trailing-separator, or NUL component is accepted. The four store-owned
    /// manifest fields (root identity, lock identity, lock-semantics version,
    /// and self-digest) must all carry their nil/zero draft values.
    [[nodiscard]] static DistributedSieveWaveStoreOpenResult
    create(const std::filesystem::path& absolute_root, WaveManifestV1 manifest_draft,
           DistributedSieveWaveStoreTestHooks hooks = {}) noexcept;

    /// Open or recover one existing wave using only the expected canonical
    /// manifest digest as caller authority.
    [[nodiscard]] static DistributedSieveWaveStoreOpenResult
    open(const std::filesystem::path& absolute_root,
         const util::Sha256Digest& expected_manifest_digest,
         DistributedSieveWaveStoreTestHooks hooks = {}) noexcept;

    [[nodiscard]] const std::filesystem::path& absolute_root() const noexcept;
    [[nodiscard]] const WaveManifestV1& manifest() const noexcept;
    [[nodiscard]] const util::Sha256Digest& manifest_digest() const noexcept;
    [[nodiscard]] const NativeIdentityV1& wave_root_identity() const noexcept;
    [[nodiscard]] const NativeIdentityV1& permanent_lock_identity() const noexcept;
    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    manifest_snapshot() const noexcept;

    /// Re-read and freeze the exact canonical manifest and WaveMergeCommit
    /// leaves while this store still owns the predecessor WaveLock. The
    /// returned value contains no descriptor or namespace capability.
    [[nodiscard]] DistributedSieveWorkerCleanupRootExactAnchorCaptureResultV1
    freeze_worker_cleanup_exact_anchor_v1(const WaveMergeCommitV1& expected_commit,
                                          const util::durable_immutable_record::RecordSnapshot&
                                              expected_commit_snapshot) const noexcept;

    /// Re-establish every held/named identity and immutable manifest binding.
    /// This operation never repairs or mutates the namespace. The optional
    /// source-private test hook runs between the two inventory snapshots.
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    revalidate(DistributedSieveWaveStoreInventoryTestHooks hooks = {}) const noexcept;

    /// Double-observe the complete manifest-bound attempt inventory and
    /// classify every chunk in manifest order. The returned facts are
    /// read-only; a durable handoff observation is not relation-read
    /// authority and an incomplete attempt is never reported as missing.
    [[nodiscard]] DistributedSieveWorkerChunkInventoryResultV1
    observe_worker_chunks_v1() const noexcept;

    /// Adopt one exact canonical worker handoff through the relation layer's
    /// same-handle reader path. The returned owner retains the exact pair and
    /// BaseLock, streams the full relation sequence and corpus digest before
    /// success, and keeps this WaveStore state alive. It exposes no path,
    /// descriptor, cleanup, or namespace-mutation capability.
    [[nodiscard]] DistributedSieveWorkerHandoffAdoptionResultV1 adopt_worker_handoff_v1(
        std::uint32_t chunk_id,
        DistributedSieveWorkerHandoffAdoptionTestHooksV1 hooks = {}) const noexcept;

    /// Adopt only the exact canonical handoff witness captured while the old
    /// attempt BaseLock was held by the typed reconciler. Native marker and
    /// artifact snapshots must still match before the same-handle reader is
    /// returned.
    [[nodiscard]] DistributedSieveWorkerHandoffAdoptionResultV1 adopt_expected_worker_handoff_v1(
        const DistributedSieveWorkerHandoffInventoryWitnessV1& expected,
        DistributedSieveWorkerHandoffAdoptionTestHooksV1 hooks = {}) const noexcept;

    /// Claim the whole-round source-private worker-coordinator gate. The
    /// process-bound claim retains the WaveStore state and releases only on
    /// destruction. It grants no root action, launch, retry, or cleanup
    /// authority by itself.
    [[nodiscard]] DistributedSieveWorkerCoordinatorClaimResultV1
    claim_worker_coordinator_v1() const noexcept;

    /// Exclusively claim this exact shared WaveStore state for one future
    /// private-lease root action. The claim is process-bound, keeps the
    /// permanent wave lock alive, and exposes no descriptor, path, or
    /// caller-chosen namespace operation. A nonempty held-handoff span must be
    /// the complete manifest-ordered set of this same store's nonempty worker
    /// chunks; their already-held BaseLock open-file descriptions are borrowed
    /// only for closed inventory validation.
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult claim_private_lease_root(
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs = {})
        const noexcept;

    /// Start a short-lived root transaction for an exact nonempty manifest
    /// chunk/attempt whose permanent BaseLock must not yet exist. The leaf is
    /// derived internally and created only with O_EXCL relative to the held
    /// wave-root descriptor. The returned claim owns the same live flock.
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult
    create_worker_attempt_private_lease_root(
        std::uint32_t chunk_id, std::uint32_t attempt_ordinal,
        DistributedSievePrivateLeaseBaseLockTestHooks hooks = {}) const noexcept;

    /// Start the corresponding recovery transaction when the exact permanent
    /// BaseLock is already present in the closed phase witness. This entry
    /// point never creates and never falls back to create-on-missing.
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult
    open_worker_attempt_private_lease_root(
        std::uint32_t chunk_id, std::uint32_t attempt_ordinal,
        DistributedSievePrivateLeaseBaseLockTestHooks hooks = {}) const noexcept;

    /// Start one merge-generation root transaction. The fixed-width lease and
    /// BaseLock names are derived internally from the bounded ordinal. These
    /// source-private entry points grant no writer, record-publication, or
    /// cleanup authority by themselves.
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult
    create_merge_generation_private_lease_root(
        std::uint32_t merge_attempt_ordinal,
        DistributedSievePrivateLeaseBaseLockTestHooks hooks = {}) const noexcept;
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult
    open_merge_generation_private_lease_root(
        std::uint32_t merge_attempt_ordinal,
        DistributedSievePrivateLeaseBaseLockTestHooks hooks = {}) const noexcept;

    /// Consume a complete fixed batch of fresh AttemptStartedV1 receipts and
    /// launch their exact self-exec worker processes.
    ///
    /// The request owns all receipts and argv strings. Bootstrap frames and
    /// child capabilities are derived internally; this entry never accepts a
    /// raw filesystem descriptor or caller-provided attempt record.
    [[nodiscard]] distributed_sieve_worker_launcher_detail::
        DistributedSieveWorkerLaunchBatchResultV1
        launch_worker_process_batch_v1(
            distributed_sieve_worker_launcher_detail::DistributedSieveWorkerLaunchRequestV1&&
                request,
            const DistributedSieveWorkIdentityV1& identity,
            const distributed_sieve_execution_policy_detail::
                DistributedSieveFrozenExecutionPolicyV1& frozen_policy,
            const core::PolynomialContext& polynomial,
            const factor_base::FactorBase& factor_base) const noexcept;

private:
    struct State;
    enum class AttemptBaseLockExpectation : std::uint8_t {
        absent,
        present,
    };

    explicit DistributedSieveWaveStore(std::shared_ptr<const State> state) noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic revalidate_authority() const noexcept;
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult claim_private_lease_root_impl(
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs,
        bool allow_merge_raw_recovery_pending) const noexcept;
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult
    claim_worker_attempt_private_lease_root(
        std::uint32_t chunk_id, std::uint32_t attempt_ordinal,
        AttemptBaseLockExpectation expectation,
        DistributedSievePrivateLeaseBaseLockTestHooks hooks) const noexcept;
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult
    claim_merge_generation_private_lease_root(
        std::uint32_t merge_attempt_ordinal, AttemptBaseLockExpectation expectation,
        DistributedSievePrivateLeaseBaseLockTestHooks hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs = {},
        bool allow_merge_raw_recovery_pending = false) const noexcept;
    [[nodiscard]] DistributedSieveWorkerHandoffAdoptionResultV1 adopt_worker_handoff_impl_v1(
        std::uint32_t chunk_id, const DistributedSieveWorkerHandoffInventoryWitnessV1* expected,
        DistributedSieveWorkerHandoffAdoptionTestHooksV1 hooks) const noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic capture_merge_commit_predecessor_snapshots_v1(
        const MergePreparedV1& prepared, std::span<const MergeStartedV1> merge_started_chain,
        std::span<const WorkerHandoffV1* const> worker_handoffs,
        const WaveMergeCommitV1* existing_commit,
        distributed_sieve_merge_writer_authority_detail::
            DistributedSieveMergeCommitPredecessorSnapshotsV1& snapshots) const noexcept;
    [[nodiscard]] DistributedSieveWaveMergeCommitPublicationResultV1 publish_wave_merge_commit_v1(
        const MergePreparedV1& prepared, std::span<const MergeStartedV1> merge_started_chain,
        std::span<const WorkerHandoffV1* const> worker_handoffs,
        const distributed_sieve_merge_writer_authority_detail::
            DistributedSieveMergeCommitPredecessorSnapshotsV1& predecessor_snapshots,
        DistributedSieveWaveMergeCommitTestHooksV1 hooks) const noexcept;
    [[nodiscard]] bool revalidate_committed_tail_v1(
        const MergePreparedV1& prepared, std::span<const MergeStartedV1> merge_started_chain,
        std::span<const WorkerHandoffV1* const> worker_handoffs,
        const distributed_sieve_merge_writer_authority_detail::
            DistributedSieveMergeCommitPredecessorSnapshotsV1& predecessor_snapshots,
        const WaveMergeCommitV1& commit,
        const util::durable_immutable_record::RecordSnapshot& canonical_snapshot) const noexcept;

    std::shared_ptr<const State> state_;

    friend class DistributedSieveExternalCleanupAuthorizationState;
    friend class DistributedSieveAdoptedWorkerChunkV1;
    friend class DistributedSieveWorkerCoordinatorClaimV1;
    friend class DistributedSieveFdPrivateLeaseRecoveryTarget;
    friend class DistributedSieveFdPrivateLeaseReservationTarget;
    friend class DistributedSievePrivateLeaseRootClaim;
    friend class DistributedSievePrivateLeaseReservationReceipt;
    friend class DistributedSieveWorkerAttemptStartReceipt;
    friend class DistributedSieveMergeLeaseReservationReceiptV1;
    friend class DistributedSieveMergeStartedReceiptV1;
    friend class DistributedSieveMergeStartedWriterMintV1;
    friend class DistributedSieveChunkTerminalFailureAdmissionV1;
    friend class MergePreparedAdmissionRevalidatorAuthorityV1;
    friend class WorkerCleanupRootRevalidatorAuthorityV1;
    friend DistributedSieveWorkerCleanupRootOpenResultV1 open_worker_cleanup_root_v1(
        const std::filesystem::path& absolute_root,
        const util::Sha256Digest& expected_manifest_digest,
        DistributedSieveWorkerCleanupRootOpenTestHooksV1 hooks,
        const DistributedSieveWorkerCleanupRootExactAnchorV1* expected_anchor) noexcept;
    friend class ::gnfs::sieve::distributed_sieve_merge_commit_authority_detail::
        DistributedSieveWaveMergeCommitAuthorityV1;
    friend class ::gnfs::sieve::distributed_sieve_merge_writer_authority_detail::
        DistributedSieveMergeWriterAuthorityV1;
    friend class ::gnfs::sieve::distributed_sieve_merge_writer_authority_detail::
        DistributedSieveCommittedTailAdmissionV1;
    friend DistributedSieveWorkerAttemptStartResult
    publish_worker_attempt_started(DistributedSievePrivateLeaseReservationReceipt&& reservation,
                                   DistributedSieveWorkerAttemptStartTestHooks hooks) noexcept;
    friend DistributedSieveMergeLeaseReservationResultV1
    reserve_distributed_sieve_merge_generation_v1(
        DistributedSieveWaveStore& store, std::uint32_t merge_attempt_ordinal,
        std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        DistributedSievePrivateLeaseProtocolTestHooks hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeStartResultV1 publish_merge_started_v1(
        DistributedSieveMergeLeaseReservationReceiptV1&& reservation,
        std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        DistributedSieveMergeStartTestHooksV1 hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeStartedWriterMintResultV1
    consume_distributed_sieve_merge_started_writer_v1(
        DistributedSieveMergeStartedReceiptV1&& receipt) noexcept;
    friend DistributedSieveMergeStartedReconcileResultV1 reconcile_merge_started_generation_v1(
        DistributedSieveWaveStore& store, std::uint32_t merge_attempt_ordinal,
        DistributedSieveMergeStartedReconcileTestHooksV1 hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeGenerationCursorResultV1
    prepare_distributed_sieve_merge_generation_v1(
        DistributedSieveWaveStore& store,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveWorkerAttemptReconcileResult reconcile_worker_attempt_started(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSieveWorkerAttemptReconcileTestHooks hooks) noexcept;
    friend DistributedSieveChunkTerminalFailurePublicationResultV1
    publish_chunk_terminal_failure_v1(
        DistributedSieveChunkTerminalFailureAdmissionV1&& admission,
        DistributedSieveChunkTerminalFailureTestHooksV1 hooks) noexcept;
};

/// Move-only cold-open authority for a committed worker-cleanup root.
///
/// The admission retains the exclusive WaveLock and one stable, same-handle
/// read-only view of the merged relation corpus. All temporary worker and
/// merged BaseLocks, action claims, private-directory handles, and coordinator
/// authority are released before construction succeeds. It proves the exact
/// root/commit/dependency/cleanup-record chain; for a canonical worker cleanup
/// authorization the relation namespace may be live, partial, or absent, but
/// this admission does not assert that a live/partial T2b prefix is legal. The
/// authorization-bound relation executor must revalidate that exact prefix
/// before any mutation.
class DistributedSieveWorkerCleanupRootAdmissionV1 final {
public:
    DistributedSieveWorkerCleanupRootAdmissionV1() = delete;
    DistributedSieveWorkerCleanupRootAdmissionV1(
        const DistributedSieveWorkerCleanupRootAdmissionV1&) = delete;
    DistributedSieveWorkerCleanupRootAdmissionV1&
    operator=(const DistributedSieveWorkerCleanupRootAdmissionV1&) = delete;
    DistributedSieveWorkerCleanupRootAdmissionV1(
        DistributedSieveWorkerCleanupRootAdmissionV1&&) noexcept;
    DistributedSieveWorkerCleanupRootAdmissionV1&
    operator=(DistributedSieveWorkerCleanupRootAdmissionV1&&) noexcept;
    ~DistributedSieveWorkerCleanupRootAdmissionV1();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const WaveMergeCommitV1& commit() const;
    [[nodiscard]] const DistributedSieveWorkerCleanupPrefixWitnessV1& cleanup_prefix() const;
    [[nodiscard]] const relation::OOCRelationReader& reader() const;

private:
    struct State;
    explicit DistributedSieveWorkerCleanupRootAdmissionV1(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend DistributedSieveWorkerCleanupRootOpenResultV1 open_worker_cleanup_root_v1(
        const std::filesystem::path& absolute_root,
        const util::Sha256Digest& expected_manifest_digest,
        DistributedSieveWorkerCleanupRootOpenTestHooksV1 hooks,
        const DistributedSieveWorkerCleanupRootExactAnchorV1* expected_anchor) noexcept;
    friend class ::gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail::
        DistributedSieveWorkerCleanupReceiptMintAuthorityV1;
};

struct DistributedSieveWorkerCleanupRootOpenResultV1 final {
    DistributedSieveWorkerCleanupRootOpenResultV1() = default;
    DistributedSieveWorkerCleanupRootOpenResultV1(
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> admission_value,
        DistributedSieveWaveStoreDiagnostic diagnostic_value) noexcept
        : admission(std::move(admission_value)), diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWorkerCleanupRootOpenResultV1(
        const DistributedSieveWorkerCleanupRootOpenResultV1&) = delete;
    DistributedSieveWorkerCleanupRootOpenResultV1&
    operator=(const DistributedSieveWorkerCleanupRootOpenResultV1&) = delete;
    DistributedSieveWorkerCleanupRootOpenResultV1(
        DistributedSieveWorkerCleanupRootOpenResultV1&&) noexcept = default;
    DistributedSieveWorkerCleanupRootOpenResultV1&
    operator=(DistributedSieveWorkerCleanupRootOpenResultV1&&) noexcept = default;

    std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> admission;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               admission.has_value() && admission->valid();
    }
};

/// Open only a committed worker-cleanup root. This entry point validates root
/// cleanup records but deliberately does not classify authorization-bound
/// relation T2b prefixes. It never repairs, publishes, resumes, or removes a
/// record and is separate from ordinary `DistributedSieveWaveStore::open()`.
[[nodiscard]] DistributedSieveWorkerCleanupRootOpenResultV1 open_worker_cleanup_root_v1(
    const std::filesystem::path& absolute_root, const util::Sha256Digest& expected_manifest_digest,
    DistributedSieveWorkerCleanupRootOpenTestHooksV1 hooks = {},
    const DistributedSieveWorkerCleanupRootExactAnchorV1* expected_anchor = nullptr) noexcept;

enum class DistributedSieveWorkerChunkDurableStateV1 : std::uint8_t {
    empty,
    missing,
    incomplete_attempt,
    terminal_failure_pending,
    terminal_failure,
    handoff,
};

/// Stable read-only classification of one manifest chunk.
///
/// Every raw terminal prefix, including canonical-only, is reported as
/// `terminal_failure_pending` until the same-lock typed publisher has
/// idempotently completed the current parent-directory durability barrier.
/// `terminal_failure` is reserved for the in-process fact built from that
/// successful publisher result and is never minted by raw observation.
/// `latest_attempt` is present for incomplete and terminal attempt chains;
/// `handoff` is present only for a fully validated canonical handoff. None of
/// these values carries a live BaseLock or relation-reader capability.
struct DistributedSieveWorkerChunkInventoryV1 final {
    ChunkPlanV1 chunk;
    DistributedSieveWorkerChunkDurableStateV1 state =
        DistributedSieveWorkerChunkDurableStateV1::missing;
    std::optional<AttemptStartedV1> latest_attempt;
    std::optional<ChunkTerminalFailureV1> terminal_failure;
    std::optional<WorkerHandoffV1> handoff;
};

struct DistributedSieveWorkerChunkInventoryResultV1 final {
    std::vector<DistributedSieveWorkerChunkInventoryV1> chunks;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

/// Move-only same-handle owner for one fully validated worker corpus.
///
/// The relation reader is destroyed before the WaveStore lifetime anchor.
/// This type intentionally exposes only immutable record facts and a const
/// reader; it cannot reopen paths or authorize artifact cleanup.
class DistributedSieveAdoptedWorkerChunkV1 final {
public:
    DistributedSieveAdoptedWorkerChunkV1() = delete;
    DistributedSieveAdoptedWorkerChunkV1(const DistributedSieveAdoptedWorkerChunkV1&) = delete;
    DistributedSieveAdoptedWorkerChunkV1&
    operator=(const DistributedSieveAdoptedWorkerChunkV1&) = delete;
    DistributedSieveAdoptedWorkerChunkV1(DistributedSieveAdoptedWorkerChunkV1&&) noexcept;
    DistributedSieveAdoptedWorkerChunkV1&
    operator=(DistributedSieveAdoptedWorkerChunkV1&&) = delete;
    ~DistributedSieveAdoptedWorkerChunkV1();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const WorkerHandoffV1& handoff() const noexcept;
    [[nodiscard]] const relation::OOCRelationReader& reader() const;

private:
    DistributedSieveAdoptedWorkerChunkV1(
        std::shared_ptr<const void> wave_store_state_anchor, WorkerHandoffV1 handoff,
        std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> retained_base_lock,
        std::unique_ptr<relation::OOCPrivateHandoffReader> reader,
        std::uint64_t creator_process_id) noexcept;

    std::shared_ptr<const void> wave_store_state_anchor_;
    WorkerHandoffV1 handoff_;
    std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> retained_base_lock_;
    std::unique_ptr<relation::OOCPrivateHandoffReader> reader_;
    std::uint64_t creator_process_id_ = 0;

    friend class DistributedSieveWaveStore;
};

struct DistributedSieveWorkerHandoffAdoptionResultV1 final {
    std::optional<DistributedSieveAdoptedWorkerChunkV1> adopted;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return adopted.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               adopted->valid();
    }
};

/// Process-bound whole-round coordinator claim.
///
/// Type-erased shared state is used only as a lifetime anchor; the claim
/// exposes no WaveStore internals. The retained atomic flag is always cleared
/// after all result readers are destroyed or an incomplete result is dropped.
class DistributedSieveWorkerCoordinatorClaimV1 final {
public:
    DistributedSieveWorkerCoordinatorClaimV1() = delete;
    DistributedSieveWorkerCoordinatorClaimV1(const DistributedSieveWorkerCoordinatorClaimV1&) =
        delete;
    DistributedSieveWorkerCoordinatorClaimV1&
    operator=(const DistributedSieveWorkerCoordinatorClaimV1&) = delete;
    DistributedSieveWorkerCoordinatorClaimV1(DistributedSieveWorkerCoordinatorClaimV1&&) = delete;
    DistributedSieveWorkerCoordinatorClaimV1&
    operator=(DistributedSieveWorkerCoordinatorClaimV1&&) = delete;
    ~DistributedSieveWorkerCoordinatorClaimV1() noexcept;

    [[nodiscard]] bool owned_by_current_process() const noexcept;

private:
    DistributedSieveWorkerCoordinatorClaimV1(std::shared_ptr<const void> wave_store_state_anchor,
                                             std::atomic_flag* gate,
                                             std::uint64_t creator_process_id) noexcept;

    std::shared_ptr<const void> wave_store_state_anchor_;
    std::atomic_flag* gate_ = nullptr;
    std::uint64_t creator_process_id_ = 0;

    friend class DistributedSieveWaveStore;
};

struct DistributedSieveWorkerCoordinatorClaimResultV1 final {
    std::unique_ptr<DistributedSieveWorkerCoordinatorClaimV1> claim;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return claim != nullptr && diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               claim->owned_by_current_process();
    }
};

/// Source-private root-relative permanent BaseLock capability.
///
/// Only a WaveStore attempt transaction can acquire this type. It retains the
/// first and only openat descriptor used for flock, stores only a frozen leaf
/// plus the already-held root descriptor, and exposes no path, leaf, fd, or
/// arbitrary namespace operation. Destruction is close-only so a forked child
/// cannot explicitly unlock the parent's shared open-file description.
class DistributedSievePrivateLeaseBaseLockAt final {
public:
    DistributedSievePrivateLeaseBaseLockAt() = delete;
    DistributedSievePrivateLeaseBaseLockAt(const DistributedSievePrivateLeaseBaseLockAt&) = delete;
    DistributedSievePrivateLeaseBaseLockAt&
    operator=(const DistributedSievePrivateLeaseBaseLockAt&) = delete;
    DistributedSievePrivateLeaseBaseLockAt(DistributedSievePrivateLeaseBaseLockAt&&) = delete;
    DistributedSievePrivateLeaseBaseLockAt&
    operator=(DistributedSievePrivateLeaseBaseLockAt&&) = delete;
    ~DistributedSievePrivateLeaseBaseLockAt() noexcept;

    /// Validate one exact terminal handoff through a duplicated descriptor
    /// that shares this already-held BaseLock open-file description.
    [[nodiscard]] gnfs::relation::OOCPrivateHandoffAdoptionResult
    adopt_exact_private_handoff(const std::filesystem::path& base_path) const noexcept;
    [[nodiscard]] gnfs::relation::OOCCleanupResult recover_exact_merge_raw_writer_private_lease(
        const std::filesystem::path& base_path,
        const DistributedSieveMergeRawWriterRecoveryInventoryWitnessV1& expected,
        gnfs::relation::OOCPrivateLeaseTestHooks hooks) const noexcept;
    [[nodiscard]] bool
    matches_exact_binding(std::string_view base_lock_leaf,
                          const NativeIdentityV1& expected_identity) const noexcept;

private:
    DistributedSievePrivateLeaseBaseLockAt(int root_fd, std::string leaf,
                                           std::uint64_t creator_process_id) noexcept;

    [[nodiscard]] static std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>
    create_new_locked(int root_fd, std::string leaf, std::uint64_t creator_process_id,
                      DistributedSieveWaveStoreDiagnostic& outcome) noexcept;

    [[nodiscard]] static std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>
    open_existing_locked(int root_fd, std::string leaf, const NativeIdentityV1& expected_identity,
                         std::uint64_t creator_process_id,
                         DistributedSieveWaveStoreDiagnostic& outcome) noexcept;

    [[nodiscard]] std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>
    duplicate_same_open_file_description(
        DistributedSieveWaveStoreDiagnostic& outcome) const noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic revalidate() const noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    synchronize(DistributedSievePrivateLeaseBaseLockTestHooks hooks) const noexcept;
    [[nodiscard]] bool owned_by_current_process() const noexcept;
    [[nodiscard]] const NativeIdentityV1& identity() const noexcept;
    void invalidate() const noexcept;

    int root_fd_ = -1;
    int lock_fd_ = -1;
    std::string leaf_;
    NativeIdentityV1 identity_{};
    std::uint64_t creator_process_id_ = 0;
    mutable std::atomic_bool invalidated_ = false;

    friend class DistributedSieveFdPrivateLeaseRecoveryTarget;
    friend class DistributedSieveFdPrivateLeaseReservationTarget;
    friend class DistributedSieveAdoptedWorkerChunkV1;
    friend class DistributedSieveWaveStore;
    friend class DistributedSievePrivateLeaseRootClaim;
    friend class DistributedSieveWorkerAttemptStartReceipt;
    friend class DistributedSieveMergeLeaseReservationReceiptV1;
    friend class DistributedSieveMergeStartedReceiptV1;
    friend class DistributedSieveMergeStartedWriterMintV1;
    friend DistributedSieveWorkerAttemptStartResult
    publish_worker_attempt_started(DistributedSievePrivateLeaseReservationReceipt&& reservation,
                                   DistributedSieveWorkerAttemptStartTestHooks hooks) noexcept;
    friend DistributedSieveMergeStartResultV1 publish_merge_started_v1(
        DistributedSieveMergeLeaseReservationReceiptV1&& reservation,
        std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        DistributedSieveMergeStartTestHooksV1 hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeStartedWriterMintResultV1
    consume_distributed_sieve_merge_started_writer_v1(
        DistributedSieveMergeStartedReceiptV1&& receipt) noexcept;
    friend DistributedSieveMergeLeaseReservationResultV1
    reserve_distributed_sieve_merge_generation_v1(
        DistributedSieveWaveStore& store, std::uint32_t merge_attempt_ordinal,
        std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        DistributedSievePrivateLeaseProtocolTestHooks hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeStartedReconcileResultV1 reconcile_merge_started_generation_v1(
        DistributedSieveWaveStore& store, std::uint32_t merge_attempt_ordinal,
        DistributedSieveMergeStartedReconcileTestHooksV1 hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeGenerationCursorResultV1
    prepare_distributed_sieve_merge_generation_v1(
        DistributedSieveWaveStore& store,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveWorkerAttemptReconcileResult reconcile_worker_attempt_started(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSieveWorkerAttemptReconcileTestHooks hooks) noexcept;
};

/// Source-private, process-bound exclusive authority for one private-lease
/// root action.
///
/// Only the owning WaveStore can mint this opaque claim. It deliberately
/// exposes no filesystem handle, path, manifest, record, or arbitrary-name
/// primitive. An attempt-bound claim also retains the exact successful
/// BaseLock inventory witness. Destruction releases its target flock before
/// the same-State claim slot.
class DistributedSievePrivateLeaseRootClaim final {
public:
    DistributedSievePrivateLeaseRootClaim() = delete;
    DistributedSievePrivateLeaseRootClaim(const DistributedSievePrivateLeaseRootClaim&) = delete;
    DistributedSievePrivateLeaseRootClaim&
    operator=(const DistributedSievePrivateLeaseRootClaim&) = delete;
    DistributedSievePrivateLeaseRootClaim(DistributedSievePrivateLeaseRootClaim&&) = delete;
    DistributedSievePrivateLeaseRootClaim&
    operator=(DistributedSievePrivateLeaseRootClaim&&) = delete;
    ~DistributedSievePrivateLeaseRootClaim() noexcept;

    /// Report only process ownership and live claim-slot ownership. This is
    /// not a namespace revalidation and is never sufficient mutation
    /// authority by itself.
    [[nodiscard]] bool owned_by_current_process() const noexcept;

    /// Re-establish the exact WaveStore authority while retaining the claim.
    /// A generic claim requires a closed manifest-bound inventory. An
    /// attempt-bound claim instead requires the exact successful BaseLock
    /// leaf-and-identity witness in two observations. Any bound namespace
    /// failure sticky-invalidates the target capability; repair requires
    /// destroying the old claim and using the explicit open-existing route.
    /// This operation never repairs or mutates the namespace.
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    revalidate(DistributedSievePrivateLeaseRootClaimTestHooks hooks = {}) const noexcept;

    /// Re-establish process, root, permanent-lock, and immutable-manifest
    /// authority. An attempt-bound claim also revalidates its retained target
    /// capability, but ordinary root children are deliberately not
    /// enumerated. This result is never sufficient mutation authority without
    /// a separate phase-aware inventory check.
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic revalidate_authority() const noexcept;

private:
    enum class BaseLockAcquisition : std::uint8_t {
        CreatedNew,
        OpenedExisting,
    };

    explicit DistributedSievePrivateLeaseRootClaim(
        std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state,
        std::vector<const DistributedSievePrivateLeaseBaseLockAt*>
            borrowed_worker_base_locks) noexcept;

    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state_;
    std::uint64_t creator_process_id_ = 0;
    std::vector<const DistributedSievePrivateLeaseBaseLockAt*> borrowed_worker_base_locks_;
    std::optional<DistributedSieveWorkerAttemptNamesV1> worker_attempt_names_;
    std::optional<DistributedSieveMergeGenerationNamesV1> merge_generation_names_;
    std::optional<std::vector<std::string>> expected_private_lease_base_lock_leaves_;
    std::optional<std::vector<NativeIdentityV1>> expected_private_lease_base_lock_identities_;
    std::optional<std::vector<DistributedSievePrivateLeaseReservationInventoryWitness>>
        expected_private_lease_reservation_witnesses_;
    std::optional<std::vector<DistributedSieveWorkerAttemptRecordInventoryWitness>>
        expected_worker_attempt_record_witnesses_;
    std::optional<std::vector<DistributedSieveMergeStartedRecordInventoryWitnessV1>>
        expected_merge_started_record_witnesses_;
    std::optional<std::vector<DistributedSieveChunkTerminalFailureRecordInventoryWitnessV1>>
        expected_chunk_terminal_failure_record_witnesses_;
    std::optional<BaseLockAcquisition> base_lock_acquisition_;
    std::vector<std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>> predecessor_base_locks_;
    std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> base_lock_at_;

    friend class DistributedSieveFdPrivateLeaseRecoveryTarget;
    friend class DistributedSieveFdPrivateLeaseReservationTarget;
    friend class DistributedSieveMergeLeaseReservationReceiptV1;
    friend class DistributedSieveMergeStartedReceiptV1;
    friend class DistributedSieveChunkTerminalFailureAdmissionV1;
    friend class DistributedSieveWaveStore;
    friend DistributedSievePrivateLeaseRootClaimResult recover_worker_attempt_private_lease(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSievePrivateLeaseRecoveryTestHooks hooks) noexcept;
    friend DistributedSievePrivateLeaseRootClaimResult recover_merge_generation_private_lease_v1(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSievePrivateLeaseRecoveryTestHooks hooks) noexcept;
    friend DistributedSievePrivateLeaseReservationResult reserve_worker_attempt_private_lease(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSievePrivateLeaseProtocolTestHooks hooks) noexcept;
    friend DistributedSieveMergeLeaseReservationResultV1
    reserve_distributed_sieve_merge_generation_v1(
        DistributedSieveWaveStore& store, std::uint32_t merge_attempt_ordinal,
        std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        DistributedSievePrivateLeaseProtocolTestHooks hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveWorkerAttemptStartResult
    publish_worker_attempt_started(DistributedSievePrivateLeaseReservationReceipt&& reservation,
                                   DistributedSieveWorkerAttemptStartTestHooks hooks) noexcept;
    friend DistributedSieveMergeStartResultV1 publish_merge_started_v1(
        DistributedSieveMergeLeaseReservationReceiptV1&& reservation,
        std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        DistributedSieveMergeStartTestHooksV1 hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeStartedReconcileResultV1 reconcile_merge_started_generation_v1(
        DistributedSieveWaveStore& store, std::uint32_t merge_attempt_ordinal,
        DistributedSieveMergeStartedReconcileTestHooksV1 hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeGenerationCursorResultV1
    prepare_distributed_sieve_merge_generation_v1(
        DistributedSieveWaveStore& store,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveWorkerAttemptReconcileResult reconcile_worker_attempt_started(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSieveWorkerAttemptReconcileTestHooks hooks) noexcept;
    friend DistributedSieveChunkTerminalFailurePublicationResultV1
    publish_chunk_terminal_failure_v1(
        DistributedSieveChunkTerminalFailureAdmissionV1&& admission,
        DistributedSieveChunkTerminalFailureTestHooksV1 hooks) noexcept;
};

struct DistributedSievePrivateLeaseRootClaimResult final {
    std::unique_ptr<DistributedSievePrivateLeaseRootClaim> claim;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return claim != nullptr && diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               claim->owned_by_current_process();
    }
};

/// Creator-bound snapshot of one freshly completed P8 reservation.
///
/// This source-private object is intentionally not a live filesystem
/// capability. It retains the WaveStore lifetime and exact final witness, but
/// it retains neither the target flock nor the same-State root claim. It
/// cannot create a writer, mutate the root, recover a lease, or authorize
/// cleanup. A future AttemptStarted publisher must consume it only after
/// reacquiring `root claim -> target BaseLock` and revalidating the exact P8
/// witness.
class DistributedSievePrivateLeaseReservationReceipt final {
public:
    DistributedSievePrivateLeaseReservationReceipt() = delete;
    DistributedSievePrivateLeaseReservationReceipt(
        const DistributedSievePrivateLeaseReservationReceipt&) = delete;
    DistributedSievePrivateLeaseReservationReceipt&
    operator=(const DistributedSievePrivateLeaseReservationReceipt&) = delete;
    DistributedSievePrivateLeaseReservationReceipt(
        DistributedSievePrivateLeaseReservationReceipt&&) noexcept = default;
    DistributedSievePrivateLeaseReservationReceipt&
    operator=(DistributedSievePrivateLeaseReservationReceipt&&) = delete;
    ~DistributedSievePrivateLeaseReservationReceipt() = default;

    [[nodiscard]] bool owned_by_current_process() const noexcept;

    /// Read-only current-snapshot check. Success is not mutation authority and
    /// must not replace the ordered root-claim/BaseLock acquisition required
    /// by any future consumer.
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    revalidate(DistributedSievePrivateLeaseReservationReceiptTestHooks hooks = {}) const noexcept;

    [[nodiscard]] std::string_view relative_lease_stem() const noexcept;
    [[nodiscard]] const std::array<std::uint64_t, 2>& lease_id() const noexcept;
    [[nodiscard]] const NativeIdentityV1& directory_identity() const noexcept;
    [[nodiscard]] const NativeIdentityV1& owner_marker_identity() const noexcept;
    [[nodiscard]] const NativeIdentityV1& owned_marker_identity() const noexcept;

private:
    DistributedSievePrivateLeaseReservationReceipt(
        std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state,
        DistributedSieveWorkerAttemptNamesV1 worker_attempt_names,
        NativeIdentityV1 base_lock_identity,
        DistributedSievePrivateLeaseReservationInventoryWitness final_witness,
        std::uint64_t creator_process_id) noexcept;

    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state_;
    DistributedSieveWorkerAttemptNamesV1 worker_attempt_names_;
    NativeIdentityV1 base_lock_identity_{};
    DistributedSievePrivateLeaseReservationInventoryWitness final_witness_;
    std::uint64_t creator_process_id_ = 0;

    friend DistributedSievePrivateLeaseReservationResult reserve_worker_attempt_private_lease(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSievePrivateLeaseProtocolTestHooks hooks) noexcept;
    friend DistributedSieveWorkerAttemptStartResult
    publish_worker_attempt_started(DistributedSievePrivateLeaseReservationReceipt&& reservation,
                                   DistributedSieveWorkerAttemptStartTestHooks hooks) noexcept;
};

struct DistributedSievePrivateLeaseReservationResult final {
    std::optional<DistributedSievePrivateLeaseReservationReceipt> receipt;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return receipt.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               receipt->owned_by_current_process();
    }
};

/// Creator-bound proof that one freshly published AttemptStartedV1 still owns
/// the exact target BaseLock which guarded publication.
///
/// The receipt retains no root claim and exposes no cleanup, recovery, path,
/// descriptor, or arbitrary record-publication authority. Destruction is
/// close-only for the target lock.
class DistributedSieveWorkerAttemptStartReceipt final {
public:
    DistributedSieveWorkerAttemptStartReceipt() = delete;
    DistributedSieveWorkerAttemptStartReceipt(const DistributedSieveWorkerAttemptStartReceipt&) =
        delete;
    DistributedSieveWorkerAttemptStartReceipt&
    operator=(const DistributedSieveWorkerAttemptStartReceipt&) = delete;
    DistributedSieveWorkerAttemptStartReceipt(
        DistributedSieveWorkerAttemptStartReceipt&&) noexcept = default;
    DistributedSieveWorkerAttemptStartReceipt&
    operator=(DistributedSieveWorkerAttemptStartReceipt&&) = delete;
    ~DistributedSieveWorkerAttemptStartReceipt() = default;

    [[nodiscard]] bool owned_by_current_process() const noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    revalidate(DistributedSieveWorkerAttemptStartReceiptTestHooks hooks = {}) const noexcept;

    [[nodiscard]] const AttemptStartedV1& record() const noexcept;
    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    canonical_snapshot() const noexcept;

private:
    DistributedSieveWorkerAttemptStartReceipt(
        std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state,
        DistributedSieveWorkerAttemptNamesV1 worker_attempt_names, AttemptStartedV1 record,
        util::durable_immutable_record::RecordSnapshot canonical_snapshot,
        DistributedSievePrivateLeaseReservationInventoryWitness final_witness,
        std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> base_lock_at,
        std::uint64_t creator_process_id) noexcept;

    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state_;
    DistributedSieveWorkerAttemptNamesV1 worker_attempt_names_;
    AttemptStartedV1 record_;
    util::durable_immutable_record::RecordSnapshot canonical_snapshot_;
    DistributedSievePrivateLeaseReservationInventoryWitness final_witness_;
    std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> base_lock_at_;
    std::uint64_t creator_process_id_ = 0;

    friend class DistributedSieveWaveStore;
    friend DistributedSieveWorkerAttemptStartResult
    publish_worker_attempt_started(DistributedSievePrivateLeaseReservationReceipt&& reservation,
                                   DistributedSieveWorkerAttemptStartTestHooks hooks) noexcept;
};

struct DistributedSieveWorkerAttemptStartResult final {
    std::optional<DistributedSieveWorkerAttemptStartReceipt> receipt;
    DistributedSieveWaveStoreDiagnostic diagnostic;
    DistributedSieveWorkerAttemptStartDisposition disposition =
        DistributedSieveWorkerAttemptStartDisposition::failed;

    [[nodiscard]] explicit operator bool() const noexcept {
        return receipt.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               disposition == DistributedSieveWorkerAttemptStartDisposition::fresh_start &&
               receipt->owned_by_current_process();
    }
};

/// Creator-bound, process-bound P8 snapshot for one merge generation.
///
/// The receipt retains neither the root claim nor the target flock. It does
/// retain same-open-file-description duplicates of every adopted worker
/// BaseLock so its inventory proof never depends on the adopted owners'
/// lifetime. It binds the exact terminal projection and merge policy validated
/// while the generation BaseLock was created, and exposes no path, descriptor,
/// writer, recovery, or cleanup authority.
class DistributedSieveMergeLeaseReservationReceiptV1 final {
public:
    DistributedSieveMergeLeaseReservationReceiptV1() = delete;
    DistributedSieveMergeLeaseReservationReceiptV1(
        const DistributedSieveMergeLeaseReservationReceiptV1&) = delete;
    DistributedSieveMergeLeaseReservationReceiptV1&
    operator=(const DistributedSieveMergeLeaseReservationReceiptV1&) = delete;
    DistributedSieveMergeLeaseReservationReceiptV1(
        DistributedSieveMergeLeaseReservationReceiptV1&&) noexcept = default;
    DistributedSieveMergeLeaseReservationReceiptV1&
    operator=(DistributedSieveMergeLeaseReservationReceiptV1&&) = delete;
    ~DistributedSieveMergeLeaseReservationReceiptV1() = default;

    [[nodiscard]] bool owned_by_current_process() const noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    revalidate(DistributedSievePrivateLeaseReservationReceiptTestHooks hooks = {}) const noexcept;
    [[nodiscard]] std::uint32_t merge_attempt_ordinal() const noexcept;

private:
    DistributedSieveMergeLeaseReservationReceiptV1(
        std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state,
        DistributedSieveMergeGenerationNamesV1 merge_generation_names,
        NativeIdentityV1 base_lock_identity,
        DistributedSievePrivateLeaseReservationInventoryWitness final_witness,
        std::vector<TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        std::vector<std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>>
            retained_worker_base_locks,
        std::vector<const DistributedSievePrivateLeaseBaseLockAt*> retained_worker_base_lock_views,
        std::uint64_t creator_process_id) noexcept;

    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state_;
    DistributedSieveMergeGenerationNamesV1 merge_generation_names_;
    NativeIdentityV1 base_lock_identity_{};
    DistributedSievePrivateLeaseReservationInventoryWitness final_witness_;
    std::vector<TerminalChunkInputV1> terminal_inputs_;
    std::uint32_t merge_policy_version_ = 0;
    std::vector<std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>>
        retained_worker_base_locks_;
    std::vector<const DistributedSievePrivateLeaseBaseLockAt*> retained_worker_base_lock_views_;
    std::uint64_t creator_process_id_ = 0;

    friend DistributedSieveMergeLeaseReservationResultV1
    reserve_distributed_sieve_merge_generation_v1(
        DistributedSieveWaveStore& store, std::uint32_t merge_attempt_ordinal,
        std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        DistributedSievePrivateLeaseProtocolTestHooks hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeStartResultV1 publish_merge_started_v1(
        DistributedSieveMergeLeaseReservationReceiptV1&& reservation,
        std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        DistributedSieveMergeStartTestHooksV1 hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
};

struct DistributedSieveMergeLeaseReservationResultV1 final {
    std::optional<DistributedSieveMergeLeaseReservationReceiptV1> receipt;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return receipt.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               receipt->owned_by_current_process();
    }
};

/// Creator-bound proof that a freshly published MergeStartedV1 still retains
/// the exact target generation BaseLock and independently owns the same-OFD
/// worker BaseLock duplicates inherited from its P8 receipt. It also privately
/// retains the complete validated merge-start predecessor chain.
class DistributedSieveMergeStartedReceiptV1 final {
public:
    DistributedSieveMergeStartedReceiptV1() = delete;
    DistributedSieveMergeStartedReceiptV1(const DistributedSieveMergeStartedReceiptV1&) = delete;
    DistributedSieveMergeStartedReceiptV1&
    operator=(const DistributedSieveMergeStartedReceiptV1&) = delete;
    DistributedSieveMergeStartedReceiptV1(DistributedSieveMergeStartedReceiptV1&&) noexcept =
        default;
    DistributedSieveMergeStartedReceiptV1&
    operator=(DistributedSieveMergeStartedReceiptV1&&) = delete;
    ~DistributedSieveMergeStartedReceiptV1() = default;

    [[nodiscard]] bool owned_by_current_process() const noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic revalidate() const noexcept;
    [[nodiscard]] const MergeStartedV1& record() const noexcept;
    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    canonical_snapshot() const noexcept;

private:
    DistributedSieveMergeStartedReceiptV1(
        std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state,
        DistributedSieveMergeGenerationNamesV1 merge_generation_names, MergeStartedV1 record,
        std::vector<MergeStartedV1> merge_started_chain,
        util::durable_immutable_record::RecordSnapshot canonical_snapshot,
        DistributedSievePrivateLeaseReservationInventoryWitness final_witness,
        std::vector<std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>>
            retained_worker_base_locks,
        std::vector<const DistributedSievePrivateLeaseBaseLockAt*> retained_worker_base_lock_views,
        std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> base_lock_at,
        std::uint64_t creator_process_id) noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    revalidate_for_merge_writer_lifetime(int directory_fd) const noexcept;

    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state_;
    DistributedSieveMergeGenerationNamesV1 merge_generation_names_;
    MergeStartedV1 record_;
    std::vector<MergeStartedV1> merge_started_chain_;
    util::durable_immutable_record::RecordSnapshot canonical_snapshot_;
    DistributedSievePrivateLeaseReservationInventoryWitness final_witness_;
    std::vector<std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>>
        retained_worker_base_locks_;
    std::vector<const DistributedSievePrivateLeaseBaseLockAt*> retained_worker_base_lock_views_;
    std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> base_lock_at_;
    std::uint64_t creator_process_id_ = 0;

    friend class DistributedSieveMergeStartedWriterMintV1;
    friend DistributedSieveMergeStartResultV1 publish_merge_started_v1(
        DistributedSieveMergeLeaseReservationReceiptV1&& reservation,
        std::span<const TerminalChunkInputV1> terminal_inputs, std::uint32_t merge_policy_version,
        DistributedSieveMergeStartTestHooksV1 hooks,
        std::span<const DistributedSieveAdoptedWorkerChunkV1* const> held_worker_handoffs) noexcept;
    friend DistributedSieveMergeStartedWriterMintResultV1
    consume_distributed_sieve_merge_started_writer_v1(
        DistributedSieveMergeStartedReceiptV1&& receipt) noexcept;
};

/// One-shot, source-private transfer from a fully revalidated merge-start
/// receipt to the exact merged-corpus writer. The object exposes no path,
/// descriptor, lock, marker, or generic relation-writer capability.
class DistributedSieveMergeStartedWriterMintV1 final {
public:
    DistributedSieveMergeStartedWriterMintV1(const DistributedSieveMergeStartedWriterMintV1&) =
        delete;
    DistributedSieveMergeStartedWriterMintV1&
    operator=(const DistributedSieveMergeStartedWriterMintV1&) = delete;
    DistributedSieveMergeStartedWriterMintV1(
        DistributedSieveMergeStartedWriterMintV1&& other) noexcept;
    DistributedSieveMergeStartedWriterMintV1&
    operator=(DistributedSieveMergeStartedWriterMintV1&&) = delete;
    ~DistributedSieveMergeStartedWriterMintV1() noexcept;

private:
    DistributedSieveMergeStartedWriterMintV1(
        DistributedSieveMergeStartedReceiptV1&& receipt,
        std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> target_base_lock_duplicate,
        int root_fd, int directory_fd, std::filesystem::path base_path,
        std::filesystem::path private_directory, std::filesystem::path lock_path,
        std::array<std::uint64_t, 3> root_identity, std::array<std::uint64_t, 3> directory_identity,
        std::array<std::uint64_t, 2> lease_id, std::array<std::uint64_t, 3> owner_marker_identity,
        std::array<std::uint64_t, 3> owned_marker_identity,
        std::uint64_t creator_process_id) noexcept;

    [[nodiscard]] std::unique_ptr<gnfs::relation::OOCRelationWriter> create_exact_writer();
    [[nodiscard]] const WaveManifestV1& manifest() const noexcept;
    [[nodiscard]] std::span<const MergeStartedV1> merge_started_chain() const noexcept;
    [[nodiscard]] bool writer_lifetime_stable() const noexcept;
    void close_directory_noexcept() noexcept;

    DistributedSieveMergeStartedReceiptV1 receipt_;
    std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> target_base_lock_duplicate_;
    int root_fd_ = -1;
    int directory_fd_ = -1;
    std::filesystem::path base_path_;
    std::filesystem::path private_directory_;
    std::filesystem::path lock_path_;
    std::array<std::uint64_t, 3> root_identity_{};
    std::array<std::uint64_t, 3> directory_identity_{};
    std::array<std::uint64_t, 2> lease_id_{};
    std::array<std::uint64_t, 3> owner_marker_identity_{};
    std::array<std::uint64_t, 3> owned_marker_identity_{};
    std::uint64_t creator_process_id_ = 0;
    bool consumed_ = false;

    friend class ::gnfs::sieve::distributed_sieve_merge_writer_authority_detail::
        DistributedSieveMergeWriterAuthorityV1;
    friend DistributedSieveMergeStartedWriterMintResultV1
    consume_distributed_sieve_merge_started_writer_v1(
        DistributedSieveMergeStartedReceiptV1&& receipt) noexcept;
};

struct DistributedSieveMergeStartedWriterMintResultV1 final {
    std::optional<DistributedSieveMergeStartedWriterMintV1> mint;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return mint.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

struct DistributedSieveMergeStartResultV1 final {
    std::optional<DistributedSieveMergeStartedReceiptV1> receipt;
    DistributedSieveWaveStoreDiagnostic diagnostic;
    DistributedSieveMergeStartDispositionV1 disposition =
        DistributedSieveMergeStartDispositionV1::failed;

    [[nodiscard]] explicit operator bool() const noexcept {
        return receipt.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               disposition == DistributedSieveMergeStartDispositionV1::fresh_start &&
               receipt->owned_by_current_process();
    }
};

struct DistributedSieveReconciledMergeStartedV1 final {
    MergeStartedV1 record;
    util::durable_immutable_record::RecordSnapshot canonical_snapshot;
    std::optional<std::uint32_t> next_merge_attempt_ordinal;
};

struct DistributedSieveMergeStartedReconcileResultV1 final {
    std::optional<DistributedSieveReconciledMergeStartedV1> reconciled;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return reconciled.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

struct DistributedSieveMergeGenerationCursorResultV1 final {
    std::optional<std::uint32_t> merge_attempt_ordinal;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return merge_attempt_ordinal.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

/// Move-only continuation of the exact final-attempt recovery transaction.
///
/// The opaque state retains the same root claim, final BaseLock open-file
/// description, pinned AttemptStarted record, complete chain, and exact P0
/// inventory baseline. It exposes no record constructor, path, descriptor, or
/// cleanup operation and can be consumed only by the typed terminal publisher.
class DistributedSieveChunkTerminalFailureAdmissionV1 final {
public:
    DistributedSieveChunkTerminalFailureAdmissionV1() = delete;
    DistributedSieveChunkTerminalFailureAdmissionV1(
        const DistributedSieveChunkTerminalFailureAdmissionV1&) = delete;
    DistributedSieveChunkTerminalFailureAdmissionV1&
    operator=(const DistributedSieveChunkTerminalFailureAdmissionV1&) = delete;
    DistributedSieveChunkTerminalFailureAdmissionV1(
        DistributedSieveChunkTerminalFailureAdmissionV1&&) noexcept;
    DistributedSieveChunkTerminalFailureAdmissionV1&
    operator=(DistributedSieveChunkTerminalFailureAdmissionV1&&) = delete;
    ~DistributedSieveChunkTerminalFailureAdmissionV1();

    [[nodiscard]] bool owned_by_current_process() const noexcept;

private:
    struct State;
    explicit DistributedSieveChunkTerminalFailureAdmissionV1(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend DistributedSieveWorkerAttemptReconcileResult reconcile_worker_attempt_started(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSieveWorkerAttemptReconcileTestHooks hooks) noexcept;
    friend DistributedSieveChunkTerminalFailurePublicationResultV1
    publish_chunk_terminal_failure_v1(
        DistributedSieveChunkTerminalFailureAdmissionV1&& admission,
        DistributedSieveChunkTerminalFailureTestHooksV1 hooks) noexcept;
};

/// Read-only reconciliation facts for one immutable worker-attempt record.
///
/// `next_attempt_ordinal` is empty when the manifest retry budget is exhausted.
/// This value has no filesystem handle and grants no launch, cleanup, or
/// publication authority.
struct DistributedSieveReconciledWorkerAttemptV1 final {
    AttemptStartedV1 record;
    util::durable_immutable_record::RecordSnapshot canonical_snapshot;
    std::optional<std::uint32_t> next_attempt_ordinal;
};

struct DistributedSieveWorkerAttemptReconcileResult final {
    std::optional<DistributedSieveReconciledWorkerAttemptV1> reconciled;
    DistributedSieveWaveStoreDiagnostic diagnostic;
    /// Present only when the exact attempt became a canonical handoff before
    /// the reconciler obtained its BaseLock. This is immutable continuity
    /// evidence for expected same-handle adoption, not cleanup or launch
    /// authority.
    std::optional<DistributedSieveWorkerHandoffInventoryWitnessV1> terminal_handoff;
    /// Present only when the reconciled record consumed the manifest's final
    /// attempt ordinal. This move-only authority retains the exact cleanup
    /// transaction and is deliberately separate from the read-only facts.
    std::optional<DistributedSieveChunkTerminalFailureAdmissionV1> terminal_failure_admission;

    [[nodiscard]] explicit operator bool() const noexcept {
        return reconciled.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

struct DistributedSieveChunkTerminalFailurePublicationResultV1 final {
    std::optional<ChunkTerminalFailureV1> terminal_failure;
    std::optional<util::durable_immutable_record::RecordSnapshot> canonical_snapshot;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return terminal_failure.has_value() && canonical_snapshot.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

/// Internal result of the singleton merge-commit publisher. `admission_spent`
/// becomes true before the first durable-record operation, or when an existing
/// commit prefix is observed and therefore requires cold-open normalization.
struct DistributedSieveWaveMergeCommitPublicationResultV1 final {
    std::optional<WaveMergeCommitV1> commit;
    std::optional<util::durable_immutable_record::RecordSnapshot> canonical_snapshot;
    DistributedSieveWaveStoreDiagnostic diagnostic;
    bool admission_spent = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return commit.has_value() && canonical_snapshot.has_value() && admission_spent &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

/// Source-private lifetime anchor for one future external cleanup
/// authorization.
///
/// Retaining this state keeps the exact shared WaveStore backing state,
/// including the permanent wave lock, alive. It also freezes the complete
/// canonical root cleanup prefix that admitted one active worker. Relation
/// cleanup may change only the private-lease namespace; any root-control drift
/// sticky-invalidates this generation. The creator PID makes an inherited
/// post-fork copy invalid without changing the parent's generation.
class DistributedSieveExternalCleanupAuthorizationState final {
public:
    DistributedSieveExternalCleanupAuthorizationState() = delete;
    DistributedSieveExternalCleanupAuthorizationState(
        const DistributedSieveExternalCleanupAuthorizationState&) = delete;
    DistributedSieveExternalCleanupAuthorizationState&
    operator=(const DistributedSieveExternalCleanupAuthorizationState&) = delete;
    DistributedSieveExternalCleanupAuthorizationState(
        DistributedSieveExternalCleanupAuthorizationState&&) = delete;
    DistributedSieveExternalCleanupAuthorizationState&
    operator=(DistributedSieveExternalCleanupAuthorizationState&&) = delete;
    ~DistributedSieveExternalCleanupAuthorizationState() = default;

private:
    struct ExactRootLeafV1 final {
        std::string leaf;
        std::vector<std::byte> bytes;
        util::durable_immutable_record::RecordSnapshot snapshot;

        [[nodiscard]] friend bool operator==(const ExactRootLeafV1&,
                                             const ExactRootLeafV1&) = default;
    };

    enum class ReceiptClaimResultV1 : std::uint8_t {
        acquired,
        already_live,
        invalidated,
    };

    struct ReceiptClaimAttemptV1 final {
        ReceiptClaimResultV1 result = ReceiptClaimResultV1::invalidated;
        DistributedSieveWaveStoreDiagnostic diagnostic;
    };

    DistributedSieveExternalCleanupAuthorizationState(
        std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state,
        std::vector<std::byte> merge_commit_bytes,
        util::durable_immutable_record::RecordSnapshot merge_commit_snapshot,
        util::Sha256Digest merge_commit_digest, std::uint32_t active_manifest_order_ordinal,
        ArtifactCleanupAuthorizedV1 authorization,
        std::vector<ExactRootLeafV1> cleanup_prefix_records) noexcept;

    [[nodiscard]] bool live_for_current_process() const noexcept;
    [[nodiscard]] ReceiptClaimAttemptV1 try_claim_receipt() const noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    revalidate_root_only_sticky(bool require_live_claim) const noexcept;
    void release_receipt_claim() const noexcept;

    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state_;
    std::vector<std::byte> merge_commit_bytes_;
    util::durable_immutable_record::RecordSnapshot merge_commit_snapshot_;
    util::Sha256Digest merge_commit_digest_;
    std::uint32_t active_manifest_order_ordinal_ = 0;
    ArtifactCleanupAuthorizedV1 authorization_;
    std::vector<ExactRootLeafV1> cleanup_prefix_records_;
    std::uint64_t creator_process_id_ = 0;
    mutable std::atomic_bool invalidated_ = false;
    mutable std::atomic_bool receipt_claimed_ = false;

    friend DistributedSieveWorkerCleanupRootOpenResultV1 open_worker_cleanup_root_v1(
        const std::filesystem::path& absolute_root,
        const util::Sha256Digest& expected_manifest_digest,
        DistributedSieveWorkerCleanupRootOpenTestHooksV1 hooks,
        const DistributedSieveWorkerCleanupRootExactAnchorV1* expected_anchor) noexcept;
    friend class ::gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail::
        DistributedSieveWorkerCleanupReceiptMintAuthorityV1;
    friend bool distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
        const DistributedSieveExternalCleanupAuthorizationState& state) noexcept;
    friend void distributed_sieve_external_cleanup_authorization_state_release_receipt_claim(
        const DistributedSieveExternalCleanupAuthorizationState& state) noexcept;
};

struct DistributedSieveWaveStoreOpenResult final {
    DistributedSieveWaveStoreOpenResult() = default;
    DistributedSieveWaveStoreOpenResult(
        std::unique_ptr<DistributedSieveWaveStore> store_value,
        std::optional<distributed_sieve_merge_writer_authority_detail::
                          DistributedSieveMergePreparedAdmissionV1>
            prepared_admission_value,
        DistributedSieveWaveStoreDiagnostic diagnostic_value) noexcept
        : DistributedSieveWaveStoreOpenResult(std::move(store_value),
                                              std::move(prepared_admission_value), std::nullopt,
                                              std::move(diagnostic_value)) {}
    DistributedSieveWaveStoreOpenResult(
        std::unique_ptr<DistributedSieveWaveStore> store_value,
        std::optional<distributed_sieve_merge_writer_authority_detail::
                          DistributedSieveMergePreparedAdmissionV1>
            prepared_admission_value,
        std::optional<distributed_sieve_merge_writer_authority_detail::
                          DistributedSieveCommittedTailAdmissionV1>
            committed_tail_admission_value,
        DistributedSieveWaveStoreDiagnostic diagnostic_value) noexcept
        : store(std::move(store_value)), prepared_admission(std::move(prepared_admission_value)),
          committed_tail_admission(std::move(committed_tail_admission_value)),
          diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWaveStoreOpenResult(const DistributedSieveWaveStoreOpenResult&) = delete;
    DistributedSieveWaveStoreOpenResult&
    operator=(const DistributedSieveWaveStoreOpenResult&) = delete;
    DistributedSieveWaveStoreOpenResult(DistributedSieveWaveStoreOpenResult&&) noexcept = default;
    DistributedSieveWaveStoreOpenResult&
    operator=(DistributedSieveWaveStoreOpenResult&& other) noexcept {
        if (this == std::addressof(other)) {
            return *this;
        }
        store = std::move(other.store);
        prepared_admission.reset();
        if (other.prepared_admission.has_value()) {
            prepared_admission.emplace(std::move(*other.prepared_admission));
            other.prepared_admission.reset();
        }
        committed_tail_admission.reset();
        if (other.committed_tail_admission.has_value()) {
            committed_tail_admission.emplace(std::move(*other.committed_tail_admission));
            other.committed_tail_admission.reset();
        }
        diagnostic = std::move(other.diagnostic);
        return *this;
    }

    std::unique_ptr<DistributedSieveWaveStore> store;
    std::optional<
        distributed_sieve_merge_writer_authority_detail::DistributedSieveMergePreparedAdmissionV1>
        prepared_admission;
    std::optional<
        distributed_sieve_merge_writer_authority_detail::DistributedSieveCommittedTailAdmissionV1>
        committed_tail_admission;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        const bool store_ready = store != nullptr && !prepared_admission.has_value() &&
                                 !committed_tail_admission.has_value();
        const bool prepared_ready = store == nullptr && prepared_admission.has_value() &&
                                    !committed_tail_admission.has_value() &&
                                    prepared_admission->valid();
        const bool committed_ready = store == nullptr && !prepared_admission.has_value() &&
                                     committed_tail_admission.has_value() &&
                                     committed_tail_admission->valid();
        return diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               static_cast<unsigned>(store_ready) + static_cast<unsigned>(prepared_ready) +
                       static_cast<unsigned>(committed_ready) ==
                   1U;
    }
};

} // namespace gnfs::sieve::distributed_sieve_resume_detail
