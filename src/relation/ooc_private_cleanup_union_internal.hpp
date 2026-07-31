#pragma once

/// @file ooc_private_cleanup_union_internal.hpp
/// @brief Source-private policy for reducing cleanup and handoff observations.

#include <gnfs/relation/ooc_cleanup_transaction.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace gnfs::sieve::distributed_sieve_resume_detail {
class WorkerHandoffTypedValidatorAuthorityV1;
}

namespace gnfs::relation::ooc_cleanup_detail {

enum class PrivateCleanupMarkerSlot : std::uint8_t {
    Intent,
    IntentPending,
    Staged,
    StagedPending,
    Count,
};

enum class PrivateHandoffLeafSlot : std::uint8_t {
    Canonical,
    Pending,
    Count,
};

/// Result of decoding one role-bound V1/V2 cleanup marker leaf.
///
/// `AuthorizedV2` means that the full V2 codec accepted the leaf for its
/// pathname role. `WrongRoleV2` means that decoding for the pathname role
/// failed, but a full decode for the opposite V2 role succeeded. V1 wrong-role
/// bytes are `Malformed` in canonical leaves and `Foreign` in pending leaves,
/// preserving the established pending-replacement contract. The raw adapter
/// must also map an established V1 intent/staged metadata, type, or link
/// rejection to `Malformed` when the existing runtime contract is
/// `IntentCorrupt`; `Foreign` is reserved for a leaf condition whose
/// established result is foreign-replacement preservation.
///
/// `LegacyPendingCandidate` is valid only for `IntentPending` and
/// `StagedPending`. It covers a stable regular single-link leaf whose extent is
/// within the V1 pending limit, but whose exact ownership/conflict meaning
/// depends on the expected transaction record. It carries no new authority and
/// must be delegated to the existing V1 runtime for the context-bound repair or
/// preservation decision; in particular it does not set `has_legacy_v1`. If it
/// is ever supplied for a canonical slot, the reducer treats that
/// adapter-invariant violation as a foreign namespace.
enum class PrivateCleanupMarkerObservationKind : std::uint8_t {
    Missing,
    LegacyV1,
    LegacyPendingCandidate,
    AuthorizedV2,
    WrongRoleV2,
    Malformed,
    Foreign,
    Count,
};

/// Result of validating one generic-handoff leaf.
///
/// `Exact` is reserved for a context-bound, protocol-valid observation.
/// Each policy-compatible leaf is validated independently, so a rejected
/// sibling cannot erase an exact fact. If two independently exact records
/// disagree, canonical remains `Exact` and pending is `Malformed` before this
/// pure policy is called.
enum class PrivateHandoffLeafObservationKind : std::uint8_t {
    Missing,
    Exact,
    /// The platform can prove that the leaf exists but cannot run the strict
    /// production record reader. Stronger cleanup/namespace facts still take
    /// precedence; otherwise the action returns PlatformUnsupported.
    Unsupported,
    Malformed,
    Foreign,
    Count,
};

/// Source-private metadata-only reader used when the strict handoff record
/// adapter is unavailable. It never grants `Exact`: a stable,
/// policy-compatible regular single-link leaf is `Unsupported`, while a
/// directory, link, reparse point, invalid POSIX owner/mode, or unstable
/// replacement is `Foreign`.
[[nodiscard]] PrivateHandoffLeafObservationKind
observe_platform_limited_handoff_leaf(const std::filesystem::path& path);

struct PrivateCleanupUnionRawObservation final {
    /// Unknown/case-fold duplicate leaves or a replaced/unstable namespace.
    /// On macOS, during strict handoff diagnostics, a sibling I/O failure
    /// short-circuits only when no terminal namespace or handoff-leaf fact is
    /// already established. Cleanup-marker and platform-limited reads retain
    /// their existing failure semantics.
    bool namespace_foreign = false;
    std::array<PrivateCleanupMarkerObservationKind,
               static_cast<std::size_t>(PrivateCleanupMarkerSlot::Count)>
        cleanup_markers{};
    std::array<PrivateHandoffLeafObservationKind,
               static_cast<std::size_t>(PrivateHandoffLeafSlot::Count)>
        handoff_markers{};

    friend constexpr bool operator==(const PrivateCleanupUnionRawObservation&,
                                     const PrivateCleanupUnionRawObservation&) noexcept = default;
};

enum class PrivateCleanupUnionObservationPoint : std::uint8_t {
    InitialInventoryComplete,
    LeafReadsComplete,
    Count,
};

struct PrivateCleanupUnionObservationTestHooks final {
    using Observe = void (*)(PrivateCleanupUnionObservationPoint point, void* context);

    Observe observe = nullptr;
    void* context = nullptr;
};

/// Source-private deterministic observation seam. On macOS, tests may replace
/// names between the held-handle inventory and relative reads. Other platforms
/// retain the explicitly path-limited observer and reject non-empty hooks.
/// The returned raw facts are not an authority permit and no mutator accepts
/// them. Production entry points always pass an empty hook.
[[nodiscard]] PrivateCleanupUnionRawObservation
observe_private_cleanup_union_for_test(const std::filesystem::path& base_path,
                                       PrivateCleanupUnionObservationTestHooks hooks = {});

/// Closed precedence result plus the independent axes needed by future V2
/// crash-prefix handling.
enum class PrivateCleanupUnionBlock : std::uint8_t {
    None,
    Foreign,
    MarkerCorrupt,
    AuthorizedV2Present,
    HandoffUnsupported,
    MixedLegacyAuthorities,
    Count,
};

struct PrivateCleanupUnionClassification final {
    PrivateCleanupUnionBlock block = PrivateCleanupUnionBlock::None;
    bool has_legacy_v1 = false;
    /// Includes a role-correct V2 record and a fully decoded V2 record found
    /// in the opposite marker role.
    bool has_v2_record = false;
    bool has_handoff = false;
    bool handoff_unsupported = false;

    friend constexpr bool operator==(const PrivateCleanupUnionClassification&,
                                     const PrivateCleanupUnionClassification&) noexcept = default;
};

/// Every current entry group must name itself before it can consume the
/// decision. V2 deliberately has no executor action.
enum class PrivateNamespaceAction : std::uint8_t {
    InspectHandoff,
    AdoptHandoff,
    RunLegacyCleanup,
    PublishPrivateLeaseCleanupHandoff,
    RecoverPrivateLease,
    RemovePrivateLease,
    ReservePrivateLease,
    ConfirmPairReusable,
    PublishPrivateHandoff,
    ValidateFreshWriter,
    ActivateFreshLease,
    Count,
};

enum class PrivateNamespaceActionDisposition : std::uint8_t {
    DelegateExistingRuntime,
    RejectForeignPreserved,
    RejectIntentCorrupt,
    RejectPlatformUnsupported,
    RejectNamespaceConflict,
    Count,
};

struct PrivateNamespaceActionDecision final {
    PrivateCleanupUnionClassification classification;
    PrivateNamespaceAction action = PrivateNamespaceAction::InspectHandoff;
    PrivateNamespaceActionDisposition disposition =
        PrivateNamespaceActionDisposition::RejectForeignPreserved;

    friend constexpr bool operator==(const PrivateNamespaceActionDecision&,
                                     const PrivateNamespaceActionDecision&) noexcept = default;
};

[[nodiscard]] PrivateCleanupUnionClassification
classify_private_cleanup_union(const PrivateCleanupUnionRawObservation& observation) noexcept;

[[nodiscard]] PrivateNamespaceActionDecision
decide_private_namespace_action(const PrivateCleanupUnionRawObservation& observation,
                                PrivateNamespaceAction action) noexcept;

[[nodiscard]] constexpr bool
decision_delegates_existing_runtime(const PrivateNamespaceActionDecision& decision) noexcept {
    return decision.disposition == PrivateNamespaceActionDisposition::DelegateExistingRuntime;
}

/// Exact, authority-free view of one legal generic-handoff publication prefix.
///
/// `Canonical` has no duplicate pending leaf. `IdenticalDual` has canonical
/// and pending leaves with identical bytes but separately retained immutable
/// file snapshots. `PendingOnly` is admitted only while the exact RESERVED
/// marker still proves the preactive rollback generation. `PendingRollback`
/// is the root-level durable tombstone that preserves the same exact record
/// while the RESERVED/OWNED rollback protocol converges.
enum class PrivateHandoffPublicationPrefixStateV1 : std::uint8_t {
    PendingOnly,
    PendingRollback,
    Canonical,
    IdenticalDual,
    Count,
};

/// Exact marker generation captured while the caller's BaseLock and the
/// source-private action claim remain held. These values are facts only; they
/// grant no marker-removal or pair-cleanup authority.
struct PrivateHandoffPublicationLeaseMarkerWitnessV1 final {
    PrivateLeaseRecord record;
    std::array<std::uint64_t, 3> identity{};

    friend bool operator==(const PrivateHandoffPublicationLeaseMarkerWitnessV1&,
                           const PrivateHandoffPublicationLeaseMarkerWitnessV1&) = default;
};

/// Comparable observation exposed by a resume permit.
///
/// The record carries the exact pair bindings. The additional lock, parent,
/// directory, and lease-marker generations close the publication-prefix
/// identity chain. No path, native handle, ownership receipt, cleanup permit,
/// or deletion callback is exposed.
struct PrivateHandoffPublicationPrefixWitnessV1 final {
    PrivateHandoffPublicationPrefixStateV1 state = PrivateHandoffPublicationPrefixStateV1::Count;
    OOCPrivateHandoffRecordV1 record;
    std::optional<util::durable_immutable_record::RecordSnapshot> canonical_snapshot;
    std::optional<util::durable_immutable_record::RecordSnapshot> pending_snapshot;
    std::optional<util::durable_immutable_record::RecordSnapshot> rollback_snapshot;
    std::array<std::uint64_t, 3> parent_identity{};
    std::array<std::uint64_t, 3> lock_identity{};
    std::array<std::uint64_t, 3> directory_identity{};
    std::optional<PrivateHandoffPublicationLeaseMarkerWitnessV1> owner;
    std::optional<PrivateHandoffPublicationLeaseMarkerWitnessV1> owned;
    std::optional<PrivateHandoffPublicationLeaseMarkerWitnessV1> reserved;

    [[nodiscard]] bool pending_only() const noexcept {
        return state == PrivateHandoffPublicationPrefixStateV1::PendingOnly;
    }

    [[nodiscard]] bool rollback_armed() const noexcept {
        return state == PrivateHandoffPublicationPrefixStateV1::PendingRollback;
    }

    [[nodiscard]] bool canonical_terminal() const noexcept {
        return state == PrivateHandoffPublicationPrefixStateV1::Canonical && !reserved.has_value();
    }

    friend bool operator==(const PrivateHandoffPublicationPrefixWitnessV1&,
                           const PrivateHandoffPublicationPrefixWitnessV1&) = default;
};

class PrivateHandoffPublicationObservedPermitV1;
class PrivateHandoffPublicationValidatedPermitV1;
class PrivateHandoffPublicationTypedValidatorV1;
class PrivateHandoffPublicationTypedValidatorTestAuthorityV1;
struct PrivateHandoffPublicationResumeAdmissionV1;
struct PrivateHandoffPublicationResumeValidationV1;
struct PrivateHandoffPublicationResumeRevalidationV1;
struct PrivateHandoffPublicationResumeResultV1;

enum class PrivateHandoffPublicationResumeObservationPointV1 : std::uint8_t {
    AfterExpectedPrefixValidated,
    // The cross-directory rollback move and its two durability barriers are
    // emitted only by the Apple implementation. Other platforms return
    // PlatformUnsupported and provide no evidence for this ordering.
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

/// Source-private authority bridge and trusted durability-test seam.
/// `stop_after` returning true interrupts after the named boundary.
/// `fail_before` returning true
/// injects a durability failure before the named sync. The existing
/// private-lease rollback boundaries are adapted into the same observation
/// enum while the retained publication permit remains held. A false outer
/// callback may inject namespace drift; every pre-authority and adapted
/// rollback boundary is followed by a complete exact re-observation before
/// the next mutation.
/// Ordinary relation callers pass an empty hook. The WaveStore source-private
/// bridge uses `stop_after` as a global authority gate: false continues after
/// validation, while true aborts the resume step.
struct PrivateHandoffPublicationResumeTestHooksV1 final {
    using StopAfter = bool (*)(PrivateHandoffPublicationResumeObservationPointV1 point,
                               void* context) noexcept;
    using FailBefore = bool (*)(PrivateHandoffPublicationResumeObservationPointV1 point,
                                void* context) noexcept;

    StopAfter stop_after = nullptr;
    FailBefore fail_before = nullptr;
    void* context = nullptr;
};

/// Move-only callback capability minted only by the WaveStore typed authority.
///
/// The callback receives the relation layer's fresh exact witness while the
/// retained BaseLock and action claim are held. No public constructor exists,
/// so an arbitrary aggregate projection or caller-owned boolean cannot attest
/// that the opaque payload passed its type-specific protocol validation.
class PrivateHandoffPublicationTypedValidatorV1 final {
public:
    using Validate = bool (*)(const PrivateHandoffPublicationPrefixWitnessV1& witness,
                              void* context) noexcept;

    PrivateHandoffPublicationTypedValidatorV1() = delete;
    PrivateHandoffPublicationTypedValidatorV1(const PrivateHandoffPublicationTypedValidatorV1&) =
        delete;
    PrivateHandoffPublicationTypedValidatorV1&
    operator=(const PrivateHandoffPublicationTypedValidatorV1&) = delete;
    PrivateHandoffPublicationTypedValidatorV1(
        PrivateHandoffPublicationTypedValidatorV1&& other) noexcept;
    PrivateHandoffPublicationTypedValidatorV1&
    operator=(PrivateHandoffPublicationTypedValidatorV1&&) = delete;
    ~PrivateHandoffPublicationTypedValidatorV1() = default;

private:
    explicit PrivateHandoffPublicationTypedValidatorV1(Validate validate, void* context) noexcept;

    Validate validate_ = nullptr;
    void* context_ = nullptr;
    std::uint64_t creator_process_id_ = 0;

    friend class gnfs::sieve::distributed_sieve_resume_detail::
        WorkerHandoffTypedValidatorAuthorityV1;
    friend class PrivateHandoffPublicationTypedValidatorTestAuthorityV1;
    friend PrivateHandoffPublicationResumeValidationV1
    validate_private_handoff_publication_resume_v1(
        PrivateHandoffPublicationObservedPermitV1&& observed,
        PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept;
};

/// Move-only source-private observation for one exact publication prefix.
///
/// On supported platforms, acquisition opens a fresh, independent
/// `BaseLock(paths.lock_path, false)`; inherited open-file descriptions can
/// therefore never mint this authority. The permit retains the exact
/// private-directory handle, the action claim, and the complete prefix
/// generation. It grants observation only and cannot be passed to the
/// reconciliation entry point.
class PrivateHandoffPublicationObservedPermitV1 final {
public:
    struct State;

    PrivateHandoffPublicationObservedPermitV1() = delete;
    PrivateHandoffPublicationObservedPermitV1(const PrivateHandoffPublicationObservedPermitV1&) =
        delete;
    PrivateHandoffPublicationObservedPermitV1&
    operator=(const PrivateHandoffPublicationObservedPermitV1&) = delete;
    PrivateHandoffPublicationObservedPermitV1(
        PrivateHandoffPublicationObservedPermitV1&& other) noexcept;
    PrivateHandoffPublicationObservedPermitV1&
    operator=(PrivateHandoffPublicationObservedPermitV1&&) = delete;
    ~PrivateHandoffPublicationObservedPermitV1();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const PrivateHandoffPublicationPrefixWitnessV1* witness() const noexcept;

private:
    explicit PrivateHandoffPublicationObservedPermitV1(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend PrivateHandoffPublicationResumeAdmissionV1 acquire_private_handoff_publication_resume_v1(
        const OOCCleanupPaths& paths,
        const std::array<std::uint64_t, 3>& expected_directory_identity) noexcept;
    friend PrivateHandoffPublicationResumeValidationV1
    validate_private_handoff_publication_resume_v1(
        PrivateHandoffPublicationObservedPermitV1&& observed,
        PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept;
    friend class PrivateHandoffPublicationValidatedPermitV1;
};

/// Move-only authority minted only by the trusted typed-validator callback.
///
/// There is no public/default construction and the raw observed permit is not a
/// reconciliation argument, so source callers cannot accidentally skip the
/// type-specific handoff validation stage.
class PrivateHandoffPublicationValidatedPermitV1 final {
public:
    PrivateHandoffPublicationValidatedPermitV1() = delete;
    PrivateHandoffPublicationValidatedPermitV1(const PrivateHandoffPublicationValidatedPermitV1&) =
        delete;
    PrivateHandoffPublicationValidatedPermitV1&
    operator=(const PrivateHandoffPublicationValidatedPermitV1&) = delete;
    PrivateHandoffPublicationValidatedPermitV1(
        PrivateHandoffPublicationValidatedPermitV1&& other) noexcept;
    PrivateHandoffPublicationValidatedPermitV1&
    operator=(PrivateHandoffPublicationValidatedPermitV1&&) = delete;
    ~PrivateHandoffPublicationValidatedPermitV1();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool held() const noexcept;

private:
    explicit PrivateHandoffPublicationValidatedPermitV1(
        std::unique_ptr<PrivateHandoffPublicationObservedPermitV1::State> state) noexcept;

    std::unique_ptr<PrivateHandoffPublicationObservedPermitV1::State> state_;

    friend PrivateHandoffPublicationResumeValidationV1
    validate_private_handoff_publication_resume_v1(
        PrivateHandoffPublicationObservedPermitV1&& observed,
        PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept;
    friend PrivateHandoffPublicationResumeRevalidationV1
    revalidate_private_handoff_publication_resume_v1(
        const PrivateHandoffPublicationValidatedPermitV1& permit) noexcept;
    friend PrivateHandoffPublicationResumeResultV1
    reconcile_private_handoff_publication_for_resume_v1(
        PrivateHandoffPublicationValidatedPermitV1& permit,
        PrivateHandoffPublicationResumeTestHooksV1 hooks) noexcept;
    friend OOCPrivateHandoffAdoptionResult adopt_consumed_canonical_private_handoff_publication_v1(
        PrivateHandoffPublicationValidatedPermitV1&& permit,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
};

/// Typed, non-throwing acquisition result. A successful permit may describe a
/// recoverable PendingOnly prefix or a canonical prefix; `result` preserves the
/// established RecoveryRequired/HandoffPresent status distinction.
struct PrivateHandoffPublicationResumeAdmissionV1 final {
    OOCCleanupResult result;
    std::optional<PrivateHandoffPublicationObservedPermitV1> observed;

    [[nodiscard]] bool acquired() const noexcept {
        return observed.has_value() && observed->valid() && observed->witness() != nullptr;
    }
};

/// Non-throwing transition from an observation to typed reconciliation
/// authority. Failure consumes the observation and releases its action claim.
struct PrivateHandoffPublicationResumeValidationV1 final {
    OOCCleanupResult result;
    std::optional<PrivateHandoffPublicationValidatedPermitV1> permit;

    [[nodiscard]] bool validated() const noexcept {
        return permit.has_value() && permit->valid();
    }
};

/// Non-mutating exact re-observation of one still-validated held permit.
///
/// This never updates the retained witness or consumes reconciliation
/// authority. A consumed permit remains held for LIFO release but is rejected
/// by this API.
struct PrivateHandoffPublicationResumeRevalidationV1 final {
    OOCCleanupResult result;
    std::optional<PrivateHandoffPublicationPrefixWitnessV1> witness;

    [[nodiscard]] bool revalidated() const noexcept {
        return witness.has_value();
    }
};

enum class PrivateHandoffPublicationResumeDispositionV1 : std::uint8_t {
    Failed,
    PendingRolledBack,
    CanonicalConverged,
    CanonicalTerminal,
    Count,
};

/// Typed convergence result. `expected_prefix` is the authority-free witness
/// consumed from the permit. `terminal_prefix` is populated only when a
/// canonical handoff remains and has passed the post-mutation full
/// re-observation.
struct PrivateHandoffPublicationResumeResultV1 final {
    OOCCleanupResult result;
    PrivateHandoffPublicationResumeDispositionV1 disposition =
        PrivateHandoffPublicationResumeDispositionV1::Failed;
    std::optional<PrivateHandoffPublicationPrefixWitnessV1> expected_prefix;
    std::optional<PrivateHandoffPublicationPrefixWitnessV1> terminal_prefix;

    [[nodiscard]] bool converged() const noexcept {
        return disposition == PrivateHandoffPublicationResumeDispositionV1::PendingRolledBack ||
               disposition == PrivateHandoffPublicationResumeDispositionV1::CanonicalConverged ||
               disposition == PrivateHandoffPublicationResumeDispositionV1::CanonicalTerminal;
    }
};

/// Acquire one exact legal publication prefix without mutating its namespace.
/// Supported platforms open an internal non-creating BaseLock; unsupported
/// platforms return before observing the filesystem. The supplied directory
/// identity must be the caller's exact expected private generation.
[[nodiscard]] PrivateHandoffPublicationResumeAdmissionV1
acquire_private_handoff_publication_resume_v1(
    const OOCCleanupPaths& paths,
    const std::array<std::uint64_t, 3>& expected_directory_identity) noexcept;

/// Re-observe the complete prefix under the retained lock, invoke the trusted
/// type-specific validator, then require one more exact full observation.
[[nodiscard]] PrivateHandoffPublicationResumeValidationV1
validate_private_handoff_publication_resume_v1(
    PrivateHandoffPublicationObservedPermitV1&& observed,
    PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept;

/// Re-observe and compare one still-validated permit without mutating or
/// consuming it.
[[nodiscard]] PrivateHandoffPublicationResumeRevalidationV1
revalidate_private_handoff_publication_resume_v1(
    const PrivateHandoffPublicationValidatedPermitV1& permit) noexcept;

/// Consume one validated permit in place while retaining its BaseLock/action
/// claim for caller-managed LIFO release. PendingOnly atomically moves the
/// exact pending leaf to the root rollback tombstone, syncs both directories,
/// and rolls back the exact preactive pair/lease generation. PendingRollback
/// resumes any durable partial of that rollback.
/// Canonical prefixes durably reconfirm canonical bytes, converge an identical
/// duplicate pending leaf, and revoke only the exact RESERVED generation. A
/// terminal canonical prefix is observation-only.
[[nodiscard]] PrivateHandoffPublicationResumeResultV1
reconcile_private_handoff_publication_for_resume_v1(
    PrivateHandoffPublicationValidatedPermitV1& permit,
    PrivateHandoffPublicationResumeTestHooksV1 hooks = {}) noexcept;

/// Consume a successfully reconciled canonical permit and adopt the exact
/// retained handoff without duplicating or reacquiring its BaseLock. Failed,
/// interrupted, and pending-rollback reconciliation results never mint this
/// authority. The returned receipt retains the original lock object and its
/// source-private action claim until the receipt is destroyed.
[[nodiscard]] OOCPrivateHandoffAdoptionResult
adopt_consumed_canonical_private_handoff_publication_v1(
    PrivateHandoffPublicationValidatedPermitV1&& permit,
    OOCPrivateHandoffAdoptionTestHooks hooks = {}) noexcept;

} // namespace gnfs::relation::ooc_cleanup_detail
