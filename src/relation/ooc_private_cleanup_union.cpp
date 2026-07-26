#include "ooc_private_cleanup_union_internal.hpp"

namespace gnfs::relation::ooc_cleanup_detail {

static_assert(static_cast<std::size_t>(PrivateCleanupMarkerSlot::Count) == 4);
static_assert(static_cast<std::size_t>(PrivateHandoffLeafSlot::Count) == 2);
static_assert(static_cast<std::uint8_t>(PrivateCleanupMarkerObservationKind::Count) == 6);
static_assert(static_cast<std::uint8_t>(PrivateHandoffLeafObservationKind::Count) == 4);
static_assert(static_cast<std::uint8_t>(PrivateCleanupUnionBlock::Count) == 5);
static_assert(static_cast<std::uint8_t>(PrivateNamespaceAction::Count) == 11);
static_assert(static_cast<std::uint8_t>(PrivateNamespaceActionDisposition::Count) == 5);

PrivateCleanupUnionClassification
classify_private_cleanup_union(const PrivateCleanupUnionRawObservation& observation) noexcept {
    PrivateCleanupUnionClassification result;
    bool marker_corrupt = false;
    bool foreign = observation.namespace_foreign;

    for (const auto marker : observation.cleanup_markers) {
        switch (marker) {
        case PrivateCleanupMarkerObservationKind::Missing:
            break;
        case PrivateCleanupMarkerObservationKind::LegacyV1:
            result.has_legacy_v1 = true;
            break;
        case PrivateCleanupMarkerObservationKind::AuthorizedV2:
            result.has_v2_record = true;
            break;
        case PrivateCleanupMarkerObservationKind::WrongRoleV2:
            result.has_v2_record = true;
            marker_corrupt = true;
            break;
        case PrivateCleanupMarkerObservationKind::Malformed:
            marker_corrupt = true;
            break;
        case PrivateCleanupMarkerObservationKind::Foreign:
        case PrivateCleanupMarkerObservationKind::Count:
            foreign = true;
            break;
        default:
            foreign = true;
            break;
        }
    }

    for (const auto handoff : observation.handoff_markers) {
        switch (handoff) {
        case PrivateHandoffLeafObservationKind::Missing:
            break;
        case PrivateHandoffLeafObservationKind::Exact:
            result.has_handoff = true;
            break;
        case PrivateHandoffLeafObservationKind::Malformed:
        case PrivateHandoffLeafObservationKind::Foreign:
        case PrivateHandoffLeafObservationKind::Count:
            foreign = true;
            break;
        default:
            foreign = true;
            break;
        }
    }

    if (foreign) {
        result.block = PrivateCleanupUnionBlock::Foreign;
    } else if (marker_corrupt) {
        result.block = PrivateCleanupUnionBlock::MarkerCorrupt;
    } else if (result.has_v2_record) {
        result.block = PrivateCleanupUnionBlock::AuthorizedV2Present;
    } else if (result.has_legacy_v1 && result.has_handoff) {
        result.block = PrivateCleanupUnionBlock::MixedLegacyAuthorities;
    }
    return result;
}

PrivateNamespaceActionDecision
decide_private_namespace_action(const PrivateCleanupUnionRawObservation& observation,
                                PrivateNamespaceAction action) noexcept {
    const auto classification = classify_private_cleanup_union(observation);
    switch (action) {
    case PrivateNamespaceAction::InspectHandoff:
    case PrivateNamespaceAction::AdoptHandoff:
    case PrivateNamespaceAction::RunLegacyCleanup:
    case PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff:
    case PrivateNamespaceAction::RecoverPrivateLease:
    case PrivateNamespaceAction::RemovePrivateLease:
    case PrivateNamespaceAction::ReservePrivateLease:
    case PrivateNamespaceAction::ConfirmPairReusable:
    case PrivateNamespaceAction::PublishPrivateHandoff:
    case PrivateNamespaceAction::ValidateFreshWriter:
    case PrivateNamespaceAction::ActivateFreshLease:
        break;
    case PrivateNamespaceAction::Count:
        return {
            .classification =
                {
                    .block = PrivateCleanupUnionBlock::Foreign,
                },
            .action = action,
            .disposition = PrivateNamespaceActionDisposition::RejectForeignPreserved,
        };
    default:
        return {
            .classification =
                {
                    .block = PrivateCleanupUnionBlock::Foreign,
                },
            .action = action,
            .disposition = PrivateNamespaceActionDisposition::RejectForeignPreserved,
        };
    }

    PrivateNamespaceActionDisposition disposition =
        PrivateNamespaceActionDisposition::RejectForeignPreserved;
    switch (classification.block) {
    case PrivateCleanupUnionBlock::None:
        disposition = PrivateNamespaceActionDisposition::DelegateExistingRuntime;
        break;
    case PrivateCleanupUnionBlock::Foreign:
        disposition = PrivateNamespaceActionDisposition::RejectForeignPreserved;
        break;
    case PrivateCleanupUnionBlock::MarkerCorrupt:
        disposition = PrivateNamespaceActionDisposition::RejectIntentCorrupt;
        break;
    case PrivateCleanupUnionBlock::AuthorizedV2Present:
        disposition = PrivateNamespaceActionDisposition::RejectV2Unsupported;
        break;
    case PrivateCleanupUnionBlock::MixedLegacyAuthorities:
        disposition = PrivateNamespaceActionDisposition::RejectNamespaceConflict;
        break;
    case PrivateCleanupUnionBlock::Count:
        disposition = PrivateNamespaceActionDisposition::RejectForeignPreserved;
        break;
    }
    return {
        .classification = classification,
        .action = action,
        .disposition = disposition,
    };
}

} // namespace gnfs::relation::ooc_cleanup_detail
