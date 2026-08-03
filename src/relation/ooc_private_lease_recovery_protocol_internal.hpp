#pragma once

/// @file ooc_private_lease_recovery_protocol_internal.hpp
/// @brief Source-private closed driver for private-lease reservation rollback.

#include "ooc_private_lease_reservation_protocol_internal.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace gnfs::relation::ooc_cleanup_detail {

/// The only namespace mutations admitted while rolling one durable
/// reservation prefix back to its permanent BaseLock-only state.
enum class PrivateLeaseRecoveryAction : std::uint8_t {
    UnlinkExactReservedPending,
    RenameReservedCanonicalToPendingNoReplace,
    RemoveExactEmptyStagingDirectory,
    UnlinkExactOwnerPending,
    RenameOwnerCanonicalToPendingNoReplace,
    UnlinkExactOwnedPending,
    RenameOwnedCanonicalToPendingNoReplace,
    RenameFinalDirectoryToStagingNoReplace,
    Count,
};

struct PrivateLeaseRecoveryTransition final {
    PrivateLeaseReservationBoundary source;
    PrivateLeaseRecoveryAction action;
    PrivateLeaseReservationBoundary successor;

    [[nodiscard]] friend bool operator==(const PrivateLeaseRecoveryTransition&,
                                         const PrivateLeaseRecoveryTransition&) = default;
};

/// One transition for each nonterminal P1-P8 reservation prefix.
///
/// The order is keyed by the numeric reservation boundary minus one. Canonical
/// markers are first renamed no-replace to their pending name, then removed on
/// the next edge. P8 first restores the unpredictable staging name. The strict
/// P8-to-P0 descent therefore exposes every crash-visible successor as the
/// immediately preceding approved prefix.
inline constexpr std::array PRIVATE_LEASE_RECOVERY_TRANSITIONS{
    PrivateLeaseRecoveryTransition{
        .source = PrivateLeaseReservationBoundary::ReservedPendingDurable,
        .action = PrivateLeaseRecoveryAction::UnlinkExactReservedPending,
        .successor = PrivateLeaseReservationBoundary::PermitAcquired,
    },
    PrivateLeaseRecoveryTransition{
        .source = PrivateLeaseReservationBoundary::ReservedCanonicalDurable,
        .action = PrivateLeaseRecoveryAction::RenameReservedCanonicalToPendingNoReplace,
        .successor = PrivateLeaseReservationBoundary::ReservedPendingDurable,
    },
    PrivateLeaseRecoveryTransition{
        .source = PrivateLeaseReservationBoundary::StagingDirectoryDurable,
        .action = PrivateLeaseRecoveryAction::RemoveExactEmptyStagingDirectory,
        .successor = PrivateLeaseReservationBoundary::ReservedCanonicalDurable,
    },
    PrivateLeaseRecoveryTransition{
        .source = PrivateLeaseReservationBoundary::OwnerPendingDurable,
        .action = PrivateLeaseRecoveryAction::UnlinkExactOwnerPending,
        .successor = PrivateLeaseReservationBoundary::StagingDirectoryDurable,
    },
    PrivateLeaseRecoveryTransition{
        .source = PrivateLeaseReservationBoundary::OwnerCanonicalDurable,
        .action = PrivateLeaseRecoveryAction::RenameOwnerCanonicalToPendingNoReplace,
        .successor = PrivateLeaseReservationBoundary::OwnerPendingDurable,
    },
    PrivateLeaseRecoveryTransition{
        .source = PrivateLeaseReservationBoundary::OwnedPendingDurable,
        .action = PrivateLeaseRecoveryAction::UnlinkExactOwnedPending,
        .successor = PrivateLeaseReservationBoundary::OwnerCanonicalDurable,
    },
    PrivateLeaseRecoveryTransition{
        .source = PrivateLeaseReservationBoundary::OwnedCanonicalDurable,
        .action = PrivateLeaseRecoveryAction::RenameOwnedCanonicalToPendingNoReplace,
        .successor = PrivateLeaseReservationBoundary::OwnedPendingDurable,
    },
    PrivateLeaseRecoveryTransition{
        .source = PrivateLeaseReservationBoundary::FinalDirectoryDurable,
        .action = PrivateLeaseRecoveryAction::RenameFinalDirectoryToStagingNoReplace,
        .successor = PrivateLeaseReservationBoundary::OwnedCanonicalDurable,
    },
};

static_assert(PRIVATE_LEASE_RECOVERY_TRANSITIONS.size() + 1U ==
              PRIVATE_LEASE_RESERVATION_BOUNDARIES.size());
static_assert(PRIVATE_LEASE_RECOVERY_TRANSITIONS.size() ==
              static_cast<std::size_t>(PrivateLeaseRecoveryAction::Count));
static_assert([] {
    for (std::size_t index = 0; index < PRIVATE_LEASE_RECOVERY_TRANSITIONS.size(); ++index) {
        const auto& transition = PRIVATE_LEASE_RECOVERY_TRANSITIONS[index];
        if (static_cast<std::size_t>(transition.source) != index + 1U ||
            static_cast<std::size_t>(transition.successor) + 1U !=
                static_cast<std::size_t>(transition.source) ||
            static_cast<std::size_t>(transition.action) != index) {
            return false;
        }
    }
    return true;
}());

enum class PrivateLeaseRecoveryRunResult : std::uint8_t {
    Completed,
    Interrupted,
    Rejected,
};

template <typename Target>
concept PrivateLeaseRecoveryTarget =
    requires(Target& target, PrivateLeaseRecoveryTransition transition,
             PrivateLeaseReservationBoundary boundary) {
        { target.boundary() } -> std::same_as<PrivateLeaseReservationBoundary>;
        { target.apply(transition) } -> std::same_as<void>;
        { target.checkpoint(boundary) } -> std::same_as<bool>;
        { target.complete() } -> std::same_as<void>;
        { target.reject_invalid_boundary() } -> std::same_as<void>;
    };

/// Roll one classifier-approved reservation prefix back to P0.
///
/// `apply()` must not return until the named successor is durable and closed.
/// A checkpoint is therefore a safe interruption boundary. P0 is idempotent:
/// recovery after a crash that made the final unlink durable completes without
/// another mutation. Count is never a recoverable state. On a state mismatch,
/// `reject_invalid_boundary()` records one diagnostic without changing state
/// and returns normally; `Rejected` remains the authoritative result.
template <PrivateLeaseRecoveryTarget Target>
[[nodiscard]] PrivateLeaseRecoveryRunResult run_private_lease_recovery_protocol(Target& target) {
    auto boundary = target.boundary();
    for (;;) {
        if (boundary == PrivateLeaseReservationBoundary::PermitAcquired) {
            if (target.boundary() != PrivateLeaseReservationBoundary::PermitAcquired) {
                target.reject_invalid_boundary();
                return PrivateLeaseRecoveryRunResult::Rejected;
            }
            target.complete();
            if (target.boundary() != PrivateLeaseReservationBoundary::PermitAcquired) {
                target.reject_invalid_boundary();
                return PrivateLeaseRecoveryRunResult::Rejected;
            }
            return PrivateLeaseRecoveryRunResult::Completed;
        }
        if (boundary == PrivateLeaseReservationBoundary::Count) {
            target.reject_invalid_boundary();
            return PrivateLeaseRecoveryRunResult::Rejected;
        }

        const std::size_t index = static_cast<std::size_t>(boundary) - 1U;
        if (index >= PRIVATE_LEASE_RECOVERY_TRANSITIONS.size() ||
            PRIVATE_LEASE_RECOVERY_TRANSITIONS[index].source != boundary) {
            target.reject_invalid_boundary();
            return PrivateLeaseRecoveryRunResult::Rejected;
        }
        const auto transition = PRIVATE_LEASE_RECOVERY_TRANSITIONS[index];
        target.apply(transition);
        if (target.boundary() != transition.successor) {
            target.reject_invalid_boundary();
            return PrivateLeaseRecoveryRunResult::Rejected;
        }
        boundary = transition.successor;
        const bool interrupted = target.checkpoint(boundary);
        if (target.boundary() != boundary) {
            target.reject_invalid_boundary();
            return PrivateLeaseRecoveryRunResult::Rejected;
        }
        if (interrupted) {
            return PrivateLeaseRecoveryRunResult::Interrupted;
        }
    }
}

} // namespace gnfs::relation::ooc_cleanup_detail
