#pragma once

/// @file ooc_private_lease_reservation_protocol_internal.hpp
/// @brief Source-private closed driver for one private-lease reservation.

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace gnfs::relation::ooc_cleanup_detail {

enum class PrivateLeaseReservationMarkerRole : std::uint8_t {
    Reserved,
    Owner,
    Owned,
    Count,
};

inline constexpr std::array PRIVATE_LEASE_RESERVATION_MARKER_ROLES{
    PrivateLeaseReservationMarkerRole::Reserved,
    PrivateLeaseReservationMarkerRole::Owner,
    PrivateLeaseReservationMarkerRole::Owned,
};

static_assert(PRIVATE_LEASE_RESERVATION_MARKER_ROLES.size() ==
              static_cast<std::size_t>(PrivateLeaseReservationMarkerRole::Count));
static_assert([] {
    for (std::size_t index = 0; index < PRIVATE_LEASE_RESERVATION_MARKER_ROLES.size(); ++index) {
        if (static_cast<std::size_t>(PRIVATE_LEASE_RESERVATION_MARKER_ROLES[index]) != index) {
            return false;
        }
    }
    return true;
}());

/// Durable interruption boundaries in their only valid reservation order.
///
/// Pending checkpoints are emitted by `publish_marker()` while the backend
/// retains and revalidates its authority. Every other checkpoint is emitted
/// directly by the driver after the named successor has been proven durable.
enum class PrivateLeaseReservationBoundary : std::uint8_t {
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

inline constexpr std::array PRIVATE_LEASE_RESERVATION_BOUNDARIES{
    PrivateLeaseReservationBoundary::PermitAcquired,
    PrivateLeaseReservationBoundary::ReservedPendingDurable,
    PrivateLeaseReservationBoundary::ReservedCanonicalDurable,
    PrivateLeaseReservationBoundary::StagingDirectoryDurable,
    PrivateLeaseReservationBoundary::OwnerPendingDurable,
    PrivateLeaseReservationBoundary::OwnerCanonicalDurable,
    PrivateLeaseReservationBoundary::OwnedPendingDurable,
    PrivateLeaseReservationBoundary::OwnedCanonicalDurable,
    PrivateLeaseReservationBoundary::FinalDirectoryDurable,
};

static_assert(PRIVATE_LEASE_RESERVATION_BOUNDARIES.size() ==
              static_cast<std::size_t>(PrivateLeaseReservationBoundary::Count));
static_assert([] {
    for (std::size_t index = 0; index < PRIVATE_LEASE_RESERVATION_BOUNDARIES.size(); ++index) {
        if (static_cast<std::size_t>(PRIVATE_LEASE_RESERVATION_BOUNDARIES[index]) != index) {
            return false;
        }
    }
    return true;
}());

enum class PrivateLeaseReservationRunResult : std::uint8_t {
    Completed,
    Interrupted,
};

template <typename Target>
concept PrivateLeaseReservationTarget =
    requires(Target& target, PrivateLeaseReservationBoundary boundary,
             PrivateLeaseReservationMarkerRole role) {
        { target.checkpoint(boundary) } -> std::same_as<bool>;
        { target.publish_marker(role, boundary) } -> std::same_as<bool>;
        { target.create_staging_directory() } -> std::same_as<void>;
        { target.promote_final_directory() } -> std::same_as<void>;
        { target.complete() } -> std::same_as<void>;
    };

/// Run the single outer reservation protocol shared by every filesystem
/// backend. A backend returns true only when the supplied pending checkpoint
/// interrupted publication after the exact pending successor became durable.
/// Other failures propagate through the backend's fail-closed error channel.
template <PrivateLeaseReservationTarget Target>
[[nodiscard]] PrivateLeaseReservationRunResult
run_private_lease_reservation_protocol(Target& target) {
    using Boundary = PrivateLeaseReservationBoundary;
    using MarkerRole = PrivateLeaseReservationMarkerRole;
    constexpr auto interrupted = PrivateLeaseReservationRunResult::Interrupted;

    if (target.checkpoint(Boundary::PermitAcquired)) {
        return interrupted;
    }
    if (target.publish_marker(MarkerRole::Reserved, Boundary::ReservedPendingDurable)) {
        return interrupted;
    }
    if (target.checkpoint(Boundary::ReservedCanonicalDurable)) {
        return interrupted;
    }

    target.create_staging_directory();
    if (target.checkpoint(Boundary::StagingDirectoryDurable)) {
        return interrupted;
    }
    if (target.publish_marker(MarkerRole::Owner, Boundary::OwnerPendingDurable)) {
        return interrupted;
    }
    if (target.checkpoint(Boundary::OwnerCanonicalDurable)) {
        return interrupted;
    }
    if (target.publish_marker(MarkerRole::Owned, Boundary::OwnedPendingDurable)) {
        return interrupted;
    }
    if (target.checkpoint(Boundary::OwnedCanonicalDurable)) {
        return interrupted;
    }

    target.promote_final_directory();
    if (target.checkpoint(Boundary::FinalDirectoryDurable)) {
        return interrupted;
    }
    target.complete();
    return PrivateLeaseReservationRunResult::Completed;
}

} // namespace gnfs::relation::ooc_cleanup_detail
