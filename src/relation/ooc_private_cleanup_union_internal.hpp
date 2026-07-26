#pragma once

/// @file ooc_private_cleanup_union_internal.hpp
/// @brief Source-private policy for reducing cleanup and handoff observations.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

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
/// Inconsistent canonical/pending records are reported as `Malformed` before
/// this pure policy is called.
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
    /// I/O failures short-circuit before the pure reducer.
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

} // namespace gnfs::relation::ooc_cleanup_detail
