#pragma once

// Source-private durable authority boundary for one distributed-sieve wave.
// This file is intentionally not installed as public API.

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
#include <vector>

namespace gnfs::sieve::distributed_sieve_resume_detail {

class DistributedSieveExternalCleanupAuthorizationState;
[[nodiscard]] bool distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
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
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG =
    ".gnfs-sink-lease.gnfs-private-lease-v1.stage-";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF =
    ".gnfs-private-lease-v1.owner";
inline constexpr std::string_view DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF =
    ".gnfs-private-lease-v1.owner.pending";
inline constexpr std::uint32_t DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1 = 1;

struct DistributedSieveWorkerAttemptNamesV1 final {
    std::string relative_lease_stem;
    std::string private_directory_leaf;
    std::string base_lock_leaf;
    std::string reserved_leaf;
    std::string reserved_pending_leaf;
    std::string owned_leaf;
    std::string owned_pending_leaf;
    std::string canonical_record_leaf;
    std::string pending_record_leaf;

    [[nodiscard]] friend bool operator==(const DistributedSieveWorkerAttemptNamesV1&,
                                         const DistributedSieveWorkerAttemptNamesV1&) = default;
};

struct DistributedSieveParsedWorkerAttemptLeafV1 final {
    std::uint32_t chunk_id = 0;
    std::uint32_t attempt_ordinal = 0;
    bool pending = false;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveParsedWorkerAttemptLeafV1&,
               const DistributedSieveParsedWorkerAttemptLeafV1&) noexcept = default;
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

enum class DistributedSieveWaveStoreStatus : std::uint8_t {
    ready,
    interrupted,
    invalid_request,
    platform_unsupported,
    root_missing,
    root_invalid,
    lock_missing,
    lock_busy,
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

struct DistributedSieveWaveStoreTestHooks final {
    using StopAfter = bool (*)(DistributedSieveWaveStoreFaultPoint point, void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

enum class DistributedSievePrivateLeaseBaseLockSyncPoint : std::uint8_t {
    TargetInitial,
    RootDirectory,
    TargetFinal,
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

struct DistributedSievePrivateLeaseReservationInventoryWitness final {
    std::string base_lock_leaf;
    DistributedSievePrivateLeaseReservationBoundary boundary =
        DistributedSievePrivateLeaseReservationBoundary::PermitAcquired;
    std::array<std::uint64_t, 2> lease_id{};
    std::optional<NativeIdentityV1> reserved_marker_identity;
    std::optional<NativeIdentityV1> directory_identity;
    std::optional<NativeIdentityV1> owner_marker_identity;
    std::optional<NativeIdentityV1> owned_marker_identity;

    [[nodiscard]] friend bool
    operator==(const DistributedSievePrivateLeaseReservationInventoryWitness&,
               const DistributedSievePrivateLeaseReservationInventoryWitness&) = default;
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

/// Trusted test-only interruption boundary for open-existing rollback.
///
/// `stop_after` is offered only after the named reverse successor has survived
/// its parent-directory durability barrier and two closed successor
/// observations. Production callers leave it empty.
struct DistributedSievePrivateLeaseRecoveryTestHooks final {
    using StopAfter = bool (*)(DistributedSievePrivateLeaseReservationBoundary boundary,
                               void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

struct DistributedSieveWaveStoreDiagnostic final {
    DistributedSieveWaveStoreStatus status = DistributedSieveWaveStoreStatus::ready;
    std::error_code native_error;
    std::optional<DistributedSieveProtocolStatus> protocol_status;
    std::optional<util::durable_immutable_record::RecordPublishStatus> publication_status;
    std::optional<DistributedSieveWaveStoreFaultPoint> last_durable_fault_point;
    std::optional<DistributedSievePrivateLeaseBaseLockSyncPoint>
        failed_private_lease_base_lock_sync_point;
    std::optional<DistributedSievePrivateLeaseReservationBoundary>
        last_private_lease_reservation_boundary;
    std::optional<DistributedSievePrivateLeaseReservationBoundary>
        last_private_lease_recovery_boundary;
    std::optional<DistributedSievePrivateLeaseReservationSyncFailureSite>
        failed_private_lease_reservation_sync_site;
};

struct DistributedSieveWaveStoreOpenResult;
struct DistributedSievePrivateLeaseRootClaimResult;
struct DistributedSievePrivateLeaseReservationResult;
class DistributedSievePrivateLeaseRootClaim;
class DistributedSievePrivateLeaseBaseLockAt;
class DistributedSievePrivateLeaseReservationReceipt;
class DistributedSieveFdPrivateLeaseReservationTarget;
class DistributedSieveFdPrivateLeaseRecoveryTarget;

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

    /// Re-establish every held/named identity and immutable manifest binding.
    /// This operation never repairs or mutates the namespace. The optional
    /// source-private test hook runs between the two inventory snapshots.
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    revalidate(DistributedSieveWaveStoreInventoryTestHooks hooks = {}) const noexcept;

    /// Exclusively claim this exact shared WaveStore state for one future
    /// private-lease root action. The claim is process-bound, keeps the
    /// permanent wave lock alive, and exposes no descriptor, path, or
    /// caller-chosen namespace operation.
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult
    claim_private_lease_root() const noexcept;

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

private:
    struct State;
    enum class AttemptBaseLockExpectation : std::uint8_t {
        absent,
        present,
    };

    explicit DistributedSieveWaveStore(std::shared_ptr<const State> state) noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic revalidate_authority() const noexcept;
    [[nodiscard]] DistributedSievePrivateLeaseRootClaimResult
    claim_worker_attempt_private_lease_root(
        std::uint32_t chunk_id, std::uint32_t attempt_ordinal,
        AttemptBaseLockExpectation expectation,
        DistributedSievePrivateLeaseBaseLockTestHooks hooks) const noexcept;

    std::shared_ptr<const State> state_;

    friend class DistributedSieveExternalCleanupAuthorizationState;
    friend class DistributedSieveFdPrivateLeaseRecoveryTarget;
    friend class DistributedSieveFdPrivateLeaseReservationTarget;
    friend class DistributedSievePrivateLeaseRootClaim;
    friend class DistributedSievePrivateLeaseReservationReceipt;
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

private:
    DistributedSievePrivateLeaseBaseLockAt(int root_fd, std::string leaf,
                                           std::uint64_t creator_process_id) noexcept;

    [[nodiscard]] static std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>
    create_new_locked(int root_fd, std::string leaf, std::uint64_t creator_process_id,
                      DistributedSievePrivateLeaseBaseLockTestHooks hooks,
                      DistributedSieveWaveStoreDiagnostic& outcome) noexcept;

    [[nodiscard]] static std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>
    open_existing_locked(int root_fd, std::string leaf, const NativeIdentityV1& expected_identity,
                         std::uint64_t creator_process_id,
                         DistributedSievePrivateLeaseBaseLockTestHooks hooks,
                         DistributedSieveWaveStoreDiagnostic& outcome) noexcept;

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
    friend class DistributedSieveWaveStore;
    friend class DistributedSievePrivateLeaseRootClaim;
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
        std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state) noexcept;

    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state_;
    std::uint64_t creator_process_id_ = 0;
    std::optional<DistributedSieveWorkerAttemptNamesV1> worker_attempt_names_;
    std::optional<std::vector<std::string>> expected_private_lease_base_lock_leaves_;
    std::optional<std::vector<NativeIdentityV1>> expected_private_lease_base_lock_identities_;
    std::optional<std::vector<DistributedSievePrivateLeaseReservationInventoryWitness>>
        expected_private_lease_reservation_witnesses_;
    std::optional<BaseLockAcquisition> base_lock_acquisition_;
    std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> base_lock_at_;

    friend class DistributedSieveFdPrivateLeaseRecoveryTarget;
    friend class DistributedSieveFdPrivateLeaseReservationTarget;
    friend class DistributedSieveWaveStore;
    friend DistributedSievePrivateLeaseRootClaimResult recover_worker_attempt_private_lease(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSievePrivateLeaseRecoveryTestHooks hooks) noexcept;
    friend DistributedSievePrivateLeaseReservationResult reserve_worker_attempt_private_lease(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSievePrivateLeaseProtocolTestHooks hooks) noexcept;
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
};

struct DistributedSievePrivateLeaseReservationResult final {
    std::optional<DistributedSievePrivateLeaseReservationReceipt> receipt;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return receipt.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready &&
               receipt->owned_by_current_process();
    }
};

/// Source-private lifetime anchor for one future external cleanup
/// authorization.
///
/// This type deliberately has no factory, mint route, record accessor, or
/// namespace operation. Its only future constructor authority is the WaveStore,
/// and retaining it keeps the exact shared WaveStore backing state (including
/// the permanent wave lock) alive. The creator PID makes an inherited
/// post-fork copy invalid even though its descriptors still refer to the same
/// open file descriptions.
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
    DistributedSieveExternalCleanupAuthorizationState(
        std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state) noexcept;

    [[nodiscard]] bool owned_by_current_process() const noexcept {
        return wave_store_state_ != nullptr && creator_process_id_ != 0 &&
               creator_process_id_ == static_cast<std::uint64_t>(gnfs::util::process_id());
    }

    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state_;
    std::uint64_t creator_process_id_ = 0;

    friend class DistributedSieveWaveStore;
    friend bool distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
        const DistributedSieveExternalCleanupAuthorizationState& state) noexcept;
};

struct DistributedSieveWaveStoreOpenResult final {
    std::unique_ptr<DistributedSieveWaveStore> store;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return store != nullptr && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

} // namespace gnfs::sieve::distributed_sieve_resume_detail
