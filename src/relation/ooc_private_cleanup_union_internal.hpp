#pragma once

/// @file ooc_private_cleanup_union_internal.hpp
/// @brief Source-private policy for reducing cleanup and handoff observations.

#include <array>
#include <cstddef>
#include <cstdint>

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
/// bytes are `Malformed`. The raw adapter must also map an established V1
/// intent/staged metadata, type, or link rejection to `Malformed` when the
/// existing runtime contract is `IntentCorrupt`; `Foreign` is reserved for a
/// leaf condition whose established result is foreign-replacement
/// preservation.
enum class PrivateCleanupMarkerObservationKind : std::uint8_t {
    Missing,
    LegacyV1,
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
    Malformed,
    Foreign,
    Count,
};

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

/// Closed precedence result plus the independent axes needed by future V2
/// crash-prefix handling.
enum class PrivateCleanupUnionBlock : std::uint8_t {
    None,
    Foreign,
    MarkerCorrupt,
    AuthorizedV2Present,
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
    RejectV2Unsupported,
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
