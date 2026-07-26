#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/relation/ooc_durable_handoff.hpp>
#include <gnfs/relation/ooc_relation_format.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>

#include <ooc_private_cleanup_action_permit_internal.hpp>
#include <ooc_private_lease_reservation_protocol_internal.hpp>

#include "support/child_process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
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
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using gnfs::core::Relation;
using gnfs::relation::OOCAuthorizedCleanupIntentV2;
using gnfs::relation::OOCAuthorizedCleanupMarkerKindV2;
using gnfs::relation::OOCCleanupFaultPoint;
using gnfs::relation::OOCCleanupOwnershipReceipt;
using gnfs::relation::OOCCleanupPaths;
using gnfs::relation::OOCCleanupPublishFaultPoint;
using gnfs::relation::OOCCleanupRequest;
using gnfs::relation::OOCCleanupStage;
using gnfs::relation::OOCCleanupStatus;
using gnfs::relation::OOCCleanupTestHooks;
using gnfs::relation::OOCCleanupTestOperation;
using gnfs::relation::OOCCleanupTransaction;
using gnfs::relation::OOCExactCleanupExpectation;
using gnfs::relation::OOCPrivateHandoffAdoptionFaultPoint;
using gnfs::relation::OOCPrivateHandoffAdoptionReceipt;
using gnfs::relation::OOCPrivateHandoffAdoptionTestHooks;
using gnfs::relation::OOCPrivateHandoffFaultPoint;
using gnfs::relation::OOCPrivateHandoffPairDescriptorV1;
using gnfs::relation::OOCPrivateHandoffPublishResult;
using gnfs::relation::OOCPrivateHandoffReader;
using gnfs::relation::OOCPrivateHandoffRecordV1;
using gnfs::relation::OOCPrivateHandoffState;
using gnfs::relation::OOCPrivateHandoffTestHooks;
using gnfs::relation::OOCPrivateLeaseFaultPoint;
using gnfs::relation::OOCPrivateLeaseOwnershipReceipt;
using gnfs::relation::OOCPrivateLeaseReservation;
using gnfs::relation::OOCPrivateLeaseTestHooks;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationStoreFormat;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::OOCSnapshotDescriptor;
using gnfs::relation::ooc_cleanup_detail::admit_private_cleanup_action_locked;
using gnfs::relation::ooc_cleanup_detail::admit_private_lease_removal_locked;
using gnfs::relation::ooc_cleanup_detail::authorize_private_cleanup_mutation;
using gnfs::relation::ooc_cleanup_detail::BaseLock;
using gnfs::relation::ooc_cleanup_detail::begin_private_cleanup_action;
using gnfs::relation::ooc_cleanup_detail::bind_private_lease_removal_generation;
using gnfs::relation::ooc_cleanup_detail::capture_private_lease_removal_generation_locked;
using gnfs::relation::ooc_cleanup_detail::inspect_private_handoff_from_permit;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupActionAdmission;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupActionPermit;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupMarkerObservationKind;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupMarkerSlot;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupMutationGate;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupUnionBlock;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupUnionClassification;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupUnionObservationPoint;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupUnionObservationTestHooks;
using gnfs::relation::ooc_cleanup_detail::PrivateCleanupUnionRawObservation;
using gnfs::relation::ooc_cleanup_detail::PrivateHandoffLeafObservationKind;
using gnfs::relation::ooc_cleanup_detail::PrivateHandoffLeafSlot;
using gnfs::relation::ooc_cleanup_detail::PrivateLeaseRemovalAdmission;
using gnfs::relation::ooc_cleanup_detail::PrivateLeaseReservationBoundary;
using gnfs::relation::ooc_cleanup_detail::PrivateLeaseReservationMarkerRole;
using gnfs::relation::ooc_cleanup_detail::PrivateLeaseReservationRunResult;
using gnfs::relation::ooc_cleanup_detail::PrivateNamespaceAction;
using gnfs::relation::ooc_cleanup_detail::PrivateNamespaceActionDisposition;
using gnfs::relation::ooc_cleanup_detail::reconcile_private_handoff_from_permit;

static_assert(!std::is_default_constructible_v<OOCCleanupOwnershipReceipt>);
static_assert(!std::is_copy_constructible_v<OOCCleanupOwnershipReceipt>);
static_assert(!std::is_copy_assignable_v<OOCCleanupOwnershipReceipt>);
static_assert(std::is_nothrow_move_constructible_v<OOCCleanupOwnershipReceipt>);
static_assert(!std::is_move_assignable_v<OOCCleanupOwnershipReceipt>);
static_assert(!std::is_default_constructible_v<OOCPrivateLeaseOwnershipReceipt>);
static_assert(!std::is_copy_constructible_v<OOCPrivateLeaseOwnershipReceipt>);
static_assert(std::is_nothrow_move_constructible_v<OOCPrivateLeaseOwnershipReceipt>);
static_assert(!std::is_move_assignable_v<OOCPrivateLeaseOwnershipReceipt>);
static_assert(!std::is_default_constructible_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_copy_constructible_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_copy_assignable_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(std::is_nothrow_move_constructible_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_move_assignable_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_default_constructible_v<OOCPrivateHandoffReader>);
static_assert(!std::is_copy_constructible_v<OOCPrivateHandoffReader>);
static_assert(std::is_nothrow_move_constructible_v<OOCPrivateHandoffReader>);
static_assert(!std::is_move_assignable_v<OOCPrivateHandoffReader>);
static_assert(!std::is_default_constructible_v<PrivateCleanupActionPermit>);
static_assert(!std::is_copy_constructible_v<PrivateCleanupActionPermit>);
static_assert(!std::is_copy_assignable_v<PrivateCleanupActionPermit>);
static_assert(std::is_nothrow_move_constructible_v<PrivateCleanupActionPermit>);
static_assert(!std::is_move_assignable_v<PrivateCleanupActionPermit>);
static_assert(!std::is_default_constructible_v<PrivateCleanupActionAdmission>);
static_assert(!std::is_copy_constructible_v<PrivateCleanupActionAdmission>);
static_assert(!std::is_copy_assignable_v<PrivateCleanupActionAdmission>);
static_assert(std::is_nothrow_move_constructible_v<PrivateCleanupActionAdmission>);
static_assert(!std::is_move_assignable_v<PrivateCleanupActionAdmission>);
static_assert(!std::is_default_constructible_v<PrivateCleanupMutationGate>);
static_assert(!std::is_copy_constructible_v<PrivateCleanupMutationGate>);
static_assert(!std::is_copy_assignable_v<PrivateCleanupMutationGate>);
static_assert(!std::is_move_constructible_v<PrivateCleanupMutationGate>);
static_assert(!std::is_move_assignable_v<PrivateCleanupMutationGate>);
static_assert(!std::is_default_constructible_v<PrivateLeaseRemovalAdmission>);
static_assert(!std::is_copy_constructible_v<PrivateLeaseRemovalAdmission>);
static_assert(!std::is_copy_assignable_v<PrivateLeaseRemovalAdmission>);
static_assert(std::is_nothrow_move_constructible_v<PrivateLeaseRemovalAdmission>);
static_assert(!std::is_move_assignable_v<PrivateLeaseRemovalAdmission>);

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

enum class ReservationProtocolEventKind : std::uint8_t {
    Checkpoint,
    PublishMarker,
    CreateStagingDirectory,
    PromoteFinalDirectory,
    Complete,
};

struct ReservationNamespacePrefix final {
    bool reserved_pending = false;
    bool reserved = false;
    bool staging_directory = false;
    bool owner_pending = false;
    bool owner = false;
    bool owned_pending = false;
    bool owned = false;
    bool final_directory = false;
};

struct ReservationBoundaryContract final {
    PrivateLeaseReservationBoundary boundary;
    ReservationProtocolEventKind event_kind;
    PrivateLeaseReservationMarkerRole role;
    OOCPrivateLeaseFaultPoint fault_point;
    ReservationNamespacePrefix prefix;
};

constexpr std::array PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS{
    ReservationBoundaryContract{
        .boundary = PrivateLeaseReservationBoundary::PermitAcquired,
        .event_kind = ReservationProtocolEventKind::Checkpoint,
        .role = PrivateLeaseReservationMarkerRole::Count,
        .fault_point = OOCPrivateLeaseFaultPoint::ReservationPermitAcquired,
        .prefix = {},
    },
    ReservationBoundaryContract{
        .boundary = PrivateLeaseReservationBoundary::ReservedPendingDurable,
        .event_kind = ReservationProtocolEventKind::PublishMarker,
        .role = PrivateLeaseReservationMarkerRole::Reserved,
        .fault_point = OOCPrivateLeaseFaultPoint::ReservedPendingDurable,
        .prefix = {.reserved_pending = true},
    },
    ReservationBoundaryContract{
        .boundary = PrivateLeaseReservationBoundary::ReservedCanonicalDurable,
        .event_kind = ReservationProtocolEventKind::Checkpoint,
        .role = PrivateLeaseReservationMarkerRole::Count,
        .fault_point = OOCPrivateLeaseFaultPoint::ReservedDurable,
        .prefix = {.reserved = true},
    },
    ReservationBoundaryContract{
        .boundary = PrivateLeaseReservationBoundary::StagingDirectoryDurable,
        .event_kind = ReservationProtocolEventKind::Checkpoint,
        .role = PrivateLeaseReservationMarkerRole::Count,
        .fault_point = OOCPrivateLeaseFaultPoint::StagingDirectoryDurable,
        .prefix = {.reserved = true, .staging_directory = true},
    },
    ReservationBoundaryContract{
        .boundary = PrivateLeaseReservationBoundary::OwnerPendingDurable,
        .event_kind = ReservationProtocolEventKind::PublishMarker,
        .role = PrivateLeaseReservationMarkerRole::Owner,
        .fault_point = OOCPrivateLeaseFaultPoint::OwnerPendingDurable,
        .prefix =
            {
                .reserved = true,
                .staging_directory = true,
                .owner_pending = true,
            },
    },
    ReservationBoundaryContract{
        .boundary = PrivateLeaseReservationBoundary::OwnerCanonicalDurable,
        .event_kind = ReservationProtocolEventKind::Checkpoint,
        .role = PrivateLeaseReservationMarkerRole::Count,
        .fault_point = OOCPrivateLeaseFaultPoint::OwnerDurable,
        .prefix =
            {
                .reserved = true,
                .staging_directory = true,
                .owner = true,
            },
    },
    ReservationBoundaryContract{
        .boundary = PrivateLeaseReservationBoundary::OwnedPendingDurable,
        .event_kind = ReservationProtocolEventKind::PublishMarker,
        .role = PrivateLeaseReservationMarkerRole::Owned,
        .fault_point = OOCPrivateLeaseFaultPoint::OwnedPendingDurable,
        .prefix =
            {
                .reserved = true,
                .staging_directory = true,
                .owner = true,
                .owned_pending = true,
            },
    },
    ReservationBoundaryContract{
        .boundary = PrivateLeaseReservationBoundary::OwnedCanonicalDurable,
        .event_kind = ReservationProtocolEventKind::Checkpoint,
        .role = PrivateLeaseReservationMarkerRole::Count,
        .fault_point = OOCPrivateLeaseFaultPoint::OwnedDurable,
        .prefix =
            {
                .reserved = true,
                .staging_directory = true,
                .owner = true,
                .owned = true,
            },
    },
    ReservationBoundaryContract{
        .boundary = PrivateLeaseReservationBoundary::FinalDirectoryDurable,
        .event_kind = ReservationProtocolEventKind::Checkpoint,
        .role = PrivateLeaseReservationMarkerRole::Count,
        .fault_point = OOCPrivateLeaseFaultPoint::FinalRenameDurable,
        .prefix =
            {
                .reserved = true,
                .owner = true,
                .owned = true,
                .final_directory = true,
            },
    },
};

static_assert(PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS.size() ==
              gnfs::relation::ooc_cleanup_detail::PRIVATE_LEASE_RESERVATION_BOUNDARIES.size());
static_assert([] {
    std::array<bool, static_cast<std::size_t>(PrivateLeaseReservationMarkerRole::Count)>
        role_seen{};
    for (std::size_t index = 0; index < PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS.size();
         ++index) {
        const auto& contract = PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS[index];
        if (contract.boundary !=
                gnfs::relation::ooc_cleanup_detail::PRIVATE_LEASE_RESERVATION_BOUNDARIES[index] ||
            static_cast<std::size_t>(contract.boundary) != index) {
            return false;
        }
        if (contract.event_kind == ReservationProtocolEventKind::PublishMarker) {
            if (contract.role == PrivateLeaseReservationMarkerRole::Count) {
                return false;
            }
            const auto role_index = static_cast<std::size_t>(contract.role);
            if (role_seen[role_index]) {
                return false;
            }
            role_seen[role_index] = true;
        } else if (contract.event_kind != ReservationProtocolEventKind::Checkpoint ||
                   contract.role != PrivateLeaseReservationMarkerRole::Count) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS[prior].fault_point ==
                contract.fault_point) {
                return false;
            }
        }
    }
    for (const bool seen : role_seen) {
        if (!seen) {
            return false;
        }
    }
    return true;
}());

struct ReservationProtocolEvent final {
    ReservationProtocolEventKind kind;
    PrivateLeaseReservationBoundary boundary = PrivateLeaseReservationBoundary::Count;
    PrivateLeaseReservationMarkerRole role = PrivateLeaseReservationMarkerRole::Count;

    friend bool operator==(const ReservationProtocolEvent&,
                           const ReservationProtocolEvent&) = default;
};

struct RecordingReservationTarget final {
    std::optional<PrivateLeaseReservationBoundary> interrupt_at;
    std::vector<ReservationProtocolEvent> events;

    [[nodiscard]] bool checkpoint(PrivateLeaseReservationBoundary boundary) {
        events.push_back({
            .kind = ReservationProtocolEventKind::Checkpoint,
            .boundary = boundary,
        });
        return interrupt_at == boundary;
    }

    [[nodiscard]] bool publish_marker(PrivateLeaseReservationMarkerRole role,
                                      PrivateLeaseReservationBoundary boundary) {
        events.push_back({
            .kind = ReservationProtocolEventKind::PublishMarker,
            .boundary = boundary,
            .role = role,
        });
        return interrupt_at == boundary;
    }

    void create_staging_directory() {
        events.push_back({
            .kind = ReservationProtocolEventKind::CreateStagingDirectory,
        });
    }

    void promote_final_directory() {
        events.push_back({
            .kind = ReservationProtocolEventKind::PromoteFinalDirectory,
        });
    }

    void complete() {
        events.push_back({
            .kind = ReservationProtocolEventKind::Complete,
        });
    }
};

static_assert(
    gnfs::relation::ooc_cleanup_detail::PrivateLeaseReservationTarget<RecordingReservationTarget>);

void test_private_lease_reservation_protocol_order_and_interruptions() {
    using Boundary = PrivateLeaseReservationBoundary;
    using EventKind = ReservationProtocolEventKind;

    std::vector<ReservationProtocolEvent> expected;
    expected.reserve(PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS.size() + 3);
    for (const auto& contract : PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS) {
        if (contract.boundary == Boundary::StagingDirectoryDurable) {
            expected.push_back({.kind = EventKind::CreateStagingDirectory});
        }
        if (contract.boundary == Boundary::FinalDirectoryDurable) {
            expected.push_back({.kind = EventKind::PromoteFinalDirectory});
        }
        expected.push_back({
            .kind = contract.event_kind,
            .boundary = contract.boundary,
            .role = contract.role,
        });
    }
    expected.push_back({.kind = EventKind::Complete});

    RecordingReservationTarget completed;
    CHECK(gnfs::relation::ooc_cleanup_detail::run_private_lease_reservation_protocol(completed) ==
          PrivateLeaseReservationRunResult::Completed);
    CHECK(completed.events == expected);

    for (const auto& contract : PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS) {
        const auto boundary = contract.boundary;
        RecordingReservationTarget interrupted{.interrupt_at = boundary};
        CHECK(gnfs::relation::ooc_cleanup_detail::run_private_lease_reservation_protocol(
                  interrupted) == PrivateLeaseReservationRunResult::Interrupted);
        const auto stop = std::find_if(expected.begin(), expected.end(),
                                       [boundary](const ReservationProtocolEvent& event) {
                                           return event.boundary == boundary;
                                       });
        CHECK(stop != expected.end());
        const auto prefix_size =
            static_cast<std::size_t>(std::distance(expected.begin(), stop)) + 1;
        CHECK(interrupted.events.size() == prefix_size);
        CHECK(std::equal(interrupted.events.begin(), interrupted.events.end(), expected.begin(),
                         stop + 1));
    }
}

[[nodiscard]] PrivateCleanupUnionClassification expected_private_cleanup_union_classification(
    const PrivateCleanupUnionRawObservation& observation) {
    constexpr std::array<std::uint8_t, 7> marker_rank{
        0, // Missing
        1, // LegacyV1
        0, // LegacyPendingCandidate
        2, // AuthorizedV2
        3, // WrongRoleV2
        3, // Malformed
        4, // Foreign
    };
    constexpr std::array<std::uint8_t, 5> handoff_rank{
        0, // Missing
        1, // Exact
        2, // Unsupported
        3, // Malformed
        3, // Foreign
    };
    constexpr std::array<std::array<PrivateCleanupUnionBlock, 4>, 5> block_lattice{{
        {
            PrivateCleanupUnionBlock::None,
            PrivateCleanupUnionBlock::None,
            PrivateCleanupUnionBlock::HandoffUnsupported,
            PrivateCleanupUnionBlock::Foreign,
        },
        {
            PrivateCleanupUnionBlock::None,
            PrivateCleanupUnionBlock::MixedLegacyAuthorities,
            PrivateCleanupUnionBlock::HandoffUnsupported,
            PrivateCleanupUnionBlock::Foreign,
        },
        {
            PrivateCleanupUnionBlock::AuthorizedV2Present,
            PrivateCleanupUnionBlock::AuthorizedV2Present,
            PrivateCleanupUnionBlock::AuthorizedV2Present,
            PrivateCleanupUnionBlock::Foreign,
        },
        {
            PrivateCleanupUnionBlock::MarkerCorrupt,
            PrivateCleanupUnionBlock::MarkerCorrupt,
            PrivateCleanupUnionBlock::MarkerCorrupt,
            PrivateCleanupUnionBlock::Foreign,
        },
        {
            PrivateCleanupUnionBlock::Foreign,
            PrivateCleanupUnionBlock::Foreign,
            PrivateCleanupUnionBlock::Foreign,
            PrivateCleanupUnionBlock::Foreign,
        },
    }};

    std::uint8_t cleanup_level = 0;
    bool has_legacy_v1 = false;
    bool has_v2_record = false;
    for (std::size_t slot = 0; slot < observation.cleanup_markers.size(); ++slot) {
        const auto marker = observation.cleanup_markers[slot];
        cleanup_level = (std::max)(cleanup_level, marker_rank[static_cast<std::size_t>(marker)]);
        const bool pending_slot =
            slot ==
                static_cast<std::size_t>(
                    gnfs::relation::ooc_cleanup_detail::PrivateCleanupMarkerSlot::IntentPending) ||
            slot ==
                static_cast<std::size_t>(
                    gnfs::relation::ooc_cleanup_detail::PrivateCleanupMarkerSlot::StagedPending);
        if (marker == PrivateCleanupMarkerObservationKind::LegacyPendingCandidate &&
            !pending_slot) {
            cleanup_level = 4;
        }
        has_legacy_v1 = has_legacy_v1 || marker == PrivateCleanupMarkerObservationKind::LegacyV1;
        has_v2_record = has_v2_record ||
                        marker == PrivateCleanupMarkerObservationKind::AuthorizedV2 ||
                        marker == PrivateCleanupMarkerObservationKind::WrongRoleV2;
    }

    std::uint8_t handoff_level = 0;
    bool has_handoff = false;
    bool handoff_unsupported = false;
    for (const auto handoff : observation.handoff_markers) {
        handoff_level = (std::max)(handoff_level, handoff_rank[static_cast<std::size_t>(handoff)]);
        has_handoff = has_handoff || handoff == PrivateHandoffLeafObservationKind::Exact;
        handoff_unsupported =
            handoff_unsupported || handoff == PrivateHandoffLeafObservationKind::Unsupported;
    }

    return {
        .block = observation.namespace_foreign ? PrivateCleanupUnionBlock::Foreign
                                               : block_lattice[cleanup_level][handoff_level],
        .has_legacy_v1 = has_legacy_v1,
        .has_v2_record = has_v2_record,
        .has_handoff = has_handoff,
        .handoff_unsupported = handoff_unsupported,
    };
}

[[nodiscard]] PrivateNamespaceActionDisposition
expected_private_namespace_disposition(PrivateCleanupUnionBlock block) {
    constexpr std::array dispositions{
        PrivateNamespaceActionDisposition::DelegateExistingRuntime,
        PrivateNamespaceActionDisposition::RejectForeignPreserved,
        PrivateNamespaceActionDisposition::RejectIntentCorrupt,
        PrivateNamespaceActionDisposition::RejectPlatformUnsupported,
        PrivateNamespaceActionDisposition::RejectPlatformUnsupported,
        PrivateNamespaceActionDisposition::RejectNamespaceConflict,
    };
    static_assert(dispositions.size() == static_cast<std::size_t>(PrivateCleanupUnionBlock::Count));
    return dispositions[static_cast<std::size_t>(block)];
}

void test_private_cleanup_union_policy_exhaustive() {
    constexpr std::array cleanup_states{
        PrivateCleanupMarkerObservationKind::Missing,
        PrivateCleanupMarkerObservationKind::LegacyV1,
        PrivateCleanupMarkerObservationKind::LegacyPendingCandidate,
        PrivateCleanupMarkerObservationKind::AuthorizedV2,
        PrivateCleanupMarkerObservationKind::WrongRoleV2,
        PrivateCleanupMarkerObservationKind::Malformed,
        PrivateCleanupMarkerObservationKind::Foreign,
    };
    static_assert(cleanup_states.size() ==
                  static_cast<std::size_t>(PrivateCleanupMarkerObservationKind::Count));
    constexpr std::array handoff_states{
        PrivateHandoffLeafObservationKind::Missing,
        PrivateHandoffLeafObservationKind::Exact,
        PrivateHandoffLeafObservationKind::Unsupported,
        PrivateHandoffLeafObservationKind::Malformed,
        PrivateHandoffLeafObservationKind::Foreign,
    };
    static_assert(handoff_states.size() ==
                  static_cast<std::size_t>(PrivateHandoffLeafObservationKind::Count));
    constexpr std::array actions{
        PrivateNamespaceAction::InspectHandoff,
        PrivateNamespaceAction::AdoptHandoff,
        PrivateNamespaceAction::RunLegacyCleanup,
        PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff,
        PrivateNamespaceAction::RecoverPrivateLease,
        PrivateNamespaceAction::RemovePrivateLease,
        PrivateNamespaceAction::ReservePrivateLease,
        PrivateNamespaceAction::ConfirmPairReusable,
        PrivateNamespaceAction::PublishPrivateHandoff,
        PrivateNamespaceAction::ValidateFreshWriter,
        PrivateNamespaceAction::ActivateFreshLease,
    };
    static_assert(actions.size() == static_cast<std::size_t>(PrivateNamespaceAction::Count));

    enum class ClassificationBucket : std::size_t {
        NoCleanupAuthority,
        LegacyV1,
        GenericHandoff,
        MixedLegacyAuthorities,
        AuthorizedV2,
        MarkerCorrupt,
        HandoffUnsupported,
        Foreign,
        Count,
    };
    std::array<std::size_t, static_cast<std::size_t>(ClassificationBucket::Count)> bucket_counts{};
    std::size_t combinations = 0;
    for (const auto intent : cleanup_states) {
        for (const auto intent_pending : cleanup_states) {
            for (const auto staged : cleanup_states) {
                for (const auto staged_pending : cleanup_states) {
                    for (const auto handoff : handoff_states) {
                        for (const auto handoff_pending : handoff_states) {
                            const PrivateCleanupUnionRawObservation observation{
                                .namespace_foreign = false,
                                .cleanup_markers =
                                    {
                                        intent,
                                        intent_pending,
                                        staged,
                                        staged_pending,
                                    },
                                .handoff_markers =
                                    {
                                        handoff,
                                        handoff_pending,
                                    },
                            };
                            const auto expected =
                                expected_private_cleanup_union_classification(observation);
                            const auto actual =
                                gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(
                                    observation);
                            CHECK(actual == expected);

                            ClassificationBucket bucket = ClassificationBucket::Foreign;
                            switch (actual.block) {
                            case PrivateCleanupUnionBlock::None:
                                if (actual.has_handoff) {
                                    bucket = ClassificationBucket::GenericHandoff;
                                } else if (actual.has_legacy_v1) {
                                    bucket = ClassificationBucket::LegacyV1;
                                } else {
                                    bucket = ClassificationBucket::NoCleanupAuthority;
                                }
                                break;
                            case PrivateCleanupUnionBlock::MixedLegacyAuthorities:
                                bucket = ClassificationBucket::MixedLegacyAuthorities;
                                break;
                            case PrivateCleanupUnionBlock::AuthorizedV2Present:
                                bucket = ClassificationBucket::AuthorizedV2;
                                break;
                            case PrivateCleanupUnionBlock::MarkerCorrupt:
                                bucket = ClassificationBucket::MarkerCorrupt;
                                break;
                            case PrivateCleanupUnionBlock::HandoffUnsupported:
                                bucket = ClassificationBucket::HandoffUnsupported;
                                break;
                            case PrivateCleanupUnionBlock::Foreign:
                                bucket = ClassificationBucket::Foreign;
                                break;
                            case PrivateCleanupUnionBlock::Count:
                                bucket = ClassificationBucket::Foreign;
                                break;
                            }
                            ++bucket_counts[static_cast<std::size_t>(bucket)];

                            for (const auto action : actions) {
                                const auto decision = gnfs::relation::ooc_cleanup_detail::
                                    decide_private_namespace_action(observation, action);
                                CHECK(decision.classification == actual);
                                CHECK(decision.action == action);
                                CHECK(decision.disposition ==
                                      expected_private_namespace_disposition(actual.block));
                                CHECK(gnfs::relation::ooc_cleanup_detail::
                                          decision_delegates_existing_runtime(decision) ==
                                      (actual.block == PrivateCleanupUnionBlock::None));
                                if (actual.has_v2_record) {
                                    CHECK(!gnfs::relation::ooc_cleanup_detail::
                                              decision_delegates_existing_runtime(decision));
                                }
                            }

                            auto foreign_namespace = observation;
                            foreign_namespace.namespace_foreign = true;
                            const auto dominated =
                                gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(
                                    foreign_namespace);
                            CHECK(dominated.block == PrivateCleanupUnionBlock::Foreign);
                            CHECK(dominated.has_legacy_v1 == actual.has_legacy_v1);
                            CHECK(dominated.has_v2_record == actual.has_v2_record);
                            CHECK(dominated.has_handoff == actual.has_handoff);
                            CHECK(dominated.handoff_unsupported == actual.handoff_unsupported);
                            ++combinations;
                        }
                    }
                }
            }
        }
    }
    CHECK(combinations == 60'025);
    constexpr std::array<std::size_t, static_cast<std::size_t>(ClassificationBucket::Count)>
        expected_bucket_counts{
            4,      // NoCleanupAuthority
            32,     // LegacyV1
            12,     // GenericHandoff
            96,     // MixedLegacyAuthorities
            972,    // AuthorizedV2
            6'804,  // MarkerCorrupt
            180,    // HandoffUnsupported
            51'925, // Foreign
        };
    CHECK(bucket_counts == expected_bucket_counts);

    const PrivateCleanupUnionRawObservation namespace_foreign{
        .namespace_foreign = true,
    };
    const auto foreign =
        gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(namespace_foreign);
    CHECK(foreign.block == PrivateCleanupUnionBlock::Foreign);
    for (const auto action : actions) {
        const auto decision = gnfs::relation::ooc_cleanup_detail::decide_private_namespace_action(
            namespace_foreign, action);
        CHECK(decision.classification == foreign);
        CHECK(decision.action == action);
        CHECK(decision.disposition == PrivateNamespaceActionDisposition::RejectForeignPreserved);
        CHECK(!gnfs::relation::ooc_cleanup_detail::decision_delegates_existing_runtime(decision));
    }

    const auto invalid_action = gnfs::relation::ooc_cleanup_detail::decide_private_namespace_action(
        {}, PrivateNamespaceAction::Count);
    CHECK(invalid_action.classification.block == PrivateCleanupUnionBlock::Foreign);
    CHECK(invalid_action.disposition == PrivateNamespaceActionDisposition::RejectForeignPreserved);
    CHECK(!gnfs::relation::ooc_cleanup_detail::decision_delegates_existing_runtime(invalid_action));

    PrivateCleanupUnionRawObservation invalid_marker;
    invalid_marker.cleanup_markers[0] = static_cast<PrivateCleanupMarkerObservationKind>(0xff);
    CHECK(
        gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(invalid_marker).block ==
        PrivateCleanupUnionBlock::Foreign);

    PrivateCleanupUnionRawObservation invalid_handoff;
    invalid_handoff.handoff_markers[0] = static_cast<PrivateHandoffLeafObservationKind>(0xff);
    CHECK(
        gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(invalid_handoff).block ==
        PrivateCleanupUnionBlock::Foreign);

    const auto out_of_range_action =
        gnfs::relation::ooc_cleanup_detail::decide_private_namespace_action(
            {}, static_cast<PrivateNamespaceAction>(0xff));
    CHECK(out_of_range_action.classification.block == PrivateCleanupUnionBlock::Foreign);
    CHECK(out_of_range_action.disposition ==
          PrivateNamespaceActionDisposition::RejectForeignPreserved);
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("gnfs-ooc-cleanup-transaction-" + std::to_string(tick) + "-" +
                     std::to_string(sequence.fetch_add(1)) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("create temp directory", path_, error);
            }
        }
        throw std::runtime_error("could not reserve a temporary directory");
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

template <typename Operation>
[[nodiscard]] bool rejects_invalid_private_cleanup_permit(Operation&& operation) {
    try {
        std::forward<Operation>(operation)();
    } catch (const gnfs::relation::ooc_cleanup_detail::Failure& failure) {
        return failure.status == OOCCleanupStatus::InvalidRequest &&
               failure.stage == OOCCleanupStage::None &&
               failure.error == std::make_error_code(std::errc::invalid_argument);
    } catch (...) {
    }
    return false;
}

struct PrivateCleanupPermitFixture final {
    PrivateCleanupPermitFixture(const std::filesystem::path& root, std::string_view label,
                                PrivateNamespaceAction action)
        : paths(OOCCleanupTransaction::paths_for(root / (std::string(label) + ".gnfs-sink-lease") /
                                                 "corpus")),
          lock(std::make_shared<BaseLock>(paths.lock_path)),
          admission(admit_private_cleanup_action_locked(paths, lock, action)) {}

    OOCCleanupPaths paths;
    std::shared_ptr<BaseLock> lock;
    PrivateCleanupActionAdmission admission;
};

void test_private_cleanup_action_permit_runtime_guards() {
    TempDirectory temp;

    {
        PrivateCleanupPermitFixture blocked(temp.path(), "permit-blocked",
                                            PrivateNamespaceAction::Count);
        CHECK(blocked.admission.blocked.has_value());
        CHECK(!blocked.admission.permit.has_value());
        if (blocked.admission.blocked) {
            CHECK(blocked.admission.blocked->status ==
                  OOCCleanupStatus::ForeignReplacementPreserved);
        }
    }

    {
        PrivateCleanupPermitFixture fixture(
            temp.path(), "permit-single-active-action",
            PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff);
        CHECK(fixture.admission.permit.has_value());
        auto nested = admit_private_cleanup_action_locked(
            fixture.paths, fixture.lock, PrivateNamespaceAction::RemovePrivateLease);
        CHECK(nested.blocked.has_value());
        CHECK(!nested.permit.has_value());
        if (nested.blocked) {
            CHECK(nested.blocked->status == OOCCleanupStatus::Busy);
            CHECK(nested.blocked->stage == OOCCleanupStage::None);
        }
        fixture.admission.permit.reset();
        auto retried = admit_private_cleanup_action_locked(
            fixture.paths, fixture.lock, PrivateNamespaceAction::RemovePrivateLease);
        CHECK(!retried.blocked.has_value());
        CHECK(retried.permit.has_value());
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-action",
                                            PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(!fixture.admission.blocked.has_value());
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)begin_private_cleanup_action(permit, fixture.paths,
                                               PrivateNamespaceAction::RemovePrivateLease);
        }));
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)begin_private_cleanup_action(permit, fixture.paths,
                                               PrivateNamespaceAction::RecoverPrivateLease);
        }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-path",
                                            PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        auto wrong_paths = fixture.paths;
        wrong_paths.data_path += ".replacement";
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)begin_private_cleanup_action(permit, wrong_paths,
                                               PrivateNamespaceAction::RecoverPrivateLease);
        }));
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)begin_private_cleanup_action(permit, fixture.paths,
                                               PrivateNamespaceAction::RecoverPrivateLease);
        }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-move",
                                            PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto moved_from = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        auto permit = std::move(moved_from);
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)begin_private_cleanup_action(moved_from, fixture.paths,
                                               PrivateNamespaceAction::RecoverPrivateLease);
        }));
        const auto& lock = begin_private_cleanup_action(
            permit, fixture.paths, PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(lock.matches(fixture.paths.lock_path));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-repeat",
                                            PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)begin_private_cleanup_action(permit, fixture.paths,
                                               PrivateNamespaceAction::RecoverPrivateLease);
        }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-handoff-once",
                                            PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(reconcile_private_handoff_from_permit(permit,
                                                    PrivateNamespaceAction::RecoverPrivateLease)
                  .state == OOCPrivateHandoffState::None);
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)reconcile_private_handoff_from_permit(
                permit, PrivateNamespaceAction::RecoverPrivateLease);
        }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-legacy-observe",
                                            PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(inspect_private_handoff_from_permit(permit).state == OOCPrivateHandoffState::None);
        PrivateCleanupMutationGate gate(permit);
        authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock);
        CHECK(gate.authorized());
#if !defined(_WIN32)
        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            const bool rejected = rejects_invalid_private_cleanup_permit(
                [&] { authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock); });
            ::_exit(rejected ? 0 : 91);
        }
        int child_status = 0;
        CHECK(::waitpid(child, &child_status, 0) == child);
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);
#endif
        authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { (void)inspect_private_handoff_from_permit(permit); }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-legacy-wrong-consumer",
                                            PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)reconcile_private_handoff_from_permit(permit,
                                                        PrivateNamespaceAction::RunLegacyCleanup);
        }));
        PrivateCleanupMutationGate gate(permit);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock); }));
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock); }));
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { (void)inspect_private_handoff_from_permit(permit); }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-legacy-failed-gate",
                                            PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(inspect_private_handoff_from_permit(permit).state == OOCPrivateHandoffState::None);
        PrivateCleanupMutationGate gate(permit);

        std::error_code error;
        CHECK(std::filesystem::create_directory(fixture.paths.private_directory, error));
        CHECK(!error);
        bool rejected_drift = false;
        try {
            authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock);
        } catch (const gnfs::relation::ooc_cleanup_detail::Failure& failure) {
            rejected_drift = failure.status == OOCCleanupStatus::ForeignReplacementPreserved;
        }
        CHECK(rejected_drift);
        CHECK(std::filesystem::remove(fixture.paths.private_directory, error));
        CHECK(!error);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock); }));
        PrivateCleanupMutationGate replacement_gate(permit);
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            authorize_private_cleanup_mutation(replacement_gate, fixture.paths, *fixture.lock);
        }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-legacy-premature-gate",
                                            PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RunLegacyCleanup);
        PrivateCleanupMutationGate gate(permit);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock); }));
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { (void)inspect_private_handoff_from_permit(permit); }));
        PrivateCleanupMutationGate replacement_gate(permit);
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            authorize_private_cleanup_mutation(replacement_gate, fixture.paths, *fixture.lock);
        }));
    }

    {
        PrivateCleanupPermitFixture source(temp.path(), "permit-legacy-source",
                                           PrivateNamespaceAction::RunLegacyCleanup);
        PrivateCleanupPermitFixture target(temp.path(), "permit-legacy-target",
                                           PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(source.admission.permit.has_value());
        CHECK(target.admission.permit.has_value());
        if (!source.admission.permit || !target.admission.permit) {
            return;
        }
        auto permit = std::move(*source.admission.permit);
        source.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, source.paths,
                                           PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(inspect_private_handoff_from_permit(permit).state == OOCPrivateHandoffState::None);
        PrivateCleanupMutationGate gate(permit);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, target.paths, *target.lock); }));
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, source.paths, *source.lock); }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-recovery-wrong-gate",
                                            PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RecoverPrivateLease);
        PrivateCleanupMutationGate gate(permit);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock); }));
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)reconcile_private_handoff_from_permit(
                permit, PrivateNamespaceAction::RecoverPrivateLease);
        }));
    }

    for (const auto action :
         {PrivateNamespaceAction::RunLegacyCleanup, PrivateNamespaceAction::RecoverPrivateLease}) {
        const auto label = action == PrivateNamespaceAction::RunLegacyCleanup
                               ? "permit-legacy-wrong-removal-bind"
                               : "permit-recovery-wrong-removal-bind";
        PrivateCleanupPermitFixture fixture(temp.path(), label, action);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        const auto generation = capture_private_lease_removal_generation_locked(
            fixture.paths, *fixture.lock, {}, {}, {}, {});
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths, action);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { bind_private_lease_removal_generation(permit, generation); }));
        if (action == PrivateNamespaceAction::RunLegacyCleanup) {
            CHECK(rejects_invalid_private_cleanup_permit(
                [&] { (void)inspect_private_handoff_from_permit(permit); }));
        } else {
            CHECK(rejects_invalid_private_cleanup_permit([&] {
                (void)reconcile_private_handoff_from_permit(
                    permit, PrivateNamespaceAction::RecoverPrivateLease);
            }));
        }
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-recovery-wrong-legacy",
                                            PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { (void)inspect_private_handoff_from_permit(permit); }));
        PrivateCleanupMutationGate gate(permit);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock); }));
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)reconcile_private_handoff_from_permit(
                permit, PrivateNamespaceAction::RecoverPrivateLease);
        }));
    }

    {
        PrivateCleanupPermitFixture fixture(
            temp.path(), "permit-publication-wrong-legacy",
            PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(
            permit, fixture.paths, PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { (void)inspect_private_handoff_from_permit(permit); }));
        PrivateCleanupMutationGate gate(permit);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, fixture.paths, *fixture.lock); }));
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { (void)inspect_private_lease_cleanup_handoff_from_permit(permit); }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-handoff-action",
                                            PrivateNamespaceAction::RemovePrivateLease);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RemovePrivateLease);
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)reconcile_private_handoff_from_permit(
                permit, PrivateNamespaceAction::RecoverPrivateLease);
        }));
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)reconcile_private_handoff_from_permit(permit,
                                                        PrivateNamespaceAction::RemovePrivateLease);
        }));
    }

    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-removal-unbound",
                                            PrivateNamespaceAction::RemovePrivateLease);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        (void)begin_private_cleanup_action(permit, fixture.paths,
                                           PrivateNamespaceAction::RemovePrivateLease);
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)reconcile_private_handoff_from_permit(permit,
                                                        PrivateNamespaceAction::RemovePrivateLease);
        }));
    }

    {
        const auto paths = OOCCleanupTransaction::paths_for(
            temp.path() / "permit-removal-handoff-once.gnfs-sink-lease" / "corpus");
        auto lock = std::make_shared<BaseLock>(paths.lock_path);
        const auto generation =
            capture_private_lease_removal_generation_locked(paths, *lock, {}, {}, {}, {});
        auto admission = admit_private_cleanup_action_locked(
            paths, lock, PrivateNamespaceAction::RemovePrivateLease);
        CHECK(admission.permit.has_value());
        if (!admission.permit) {
            return;
        }
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        (void)begin_private_cleanup_action(permit, paths,
                                           PrivateNamespaceAction::RemovePrivateLease);
        bind_private_lease_removal_generation(permit, generation);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { bind_private_lease_removal_generation(permit, generation); }));
        CHECK(reconcile_private_handoff_from_permit(permit,
                                                    PrivateNamespaceAction::RemovePrivateLease)
                  .state == OOCPrivateHandoffState::None);
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)reconcile_private_handoff_from_permit(permit,
                                                        PrivateNamespaceAction::RemovePrivateLease);
        }));
    }

    {
        const auto paths = OOCCleanupTransaction::paths_for(
            temp.path() / "permit-removal-failed-bind.gnfs-sink-lease" / "corpus");
        auto lock = std::make_shared<BaseLock>(paths.lock_path);
        auto admission = admit_private_lease_removal_locked(paths, lock, {}, {}, {}, {});
        CHECK(admission.permit.has_value());
        CHECK(admission.generation.has_value());
        if (!admission.permit || !admission.generation) {
            return;
        }
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        const auto generation = std::move(*admission.generation);
        admission.generation.reset();
        (void)begin_private_cleanup_action(permit, paths,
                                           PrivateNamespaceAction::RemovePrivateLease);

        std::error_code error;
        CHECK(std::filesystem::create_directory(paths.private_directory, error));
        CHECK(!error);
        bool failed_bind = false;
        try {
            bind_private_lease_removal_generation(permit, generation);
        } catch (const gnfs::relation::ooc_cleanup_detail::Failure& failure) {
            failed_bind = failure.status == OOCCleanupStatus::NamespaceConflict;
        }
        CHECK(failed_bind);
        CHECK(std::filesystem::remove(paths.private_directory, error));
        CHECK(!error);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { bind_private_lease_removal_generation(permit, generation); }));
        CHECK(rejects_invalid_private_cleanup_permit([&] {
            (void)reconcile_private_handoff_from_permit(permit,
                                                        PrivateNamespaceAction::RemovePrivateLease);
        }));
    }

#if !defined(_WIN32)
    {
        PrivateCleanupPermitFixture fixture(temp.path(), "permit-process",
                                            PrivateNamespaceAction::RecoverPrivateLease);
        CHECK(fixture.admission.permit.has_value());
        if (!fixture.admission.permit) {
            return;
        }
        auto permit = std::move(*fixture.admission.permit);
        fixture.admission.permit.reset();
        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            const bool rejected = rejects_invalid_private_cleanup_permit([&] {
                (void)begin_private_cleanup_action(permit, fixture.paths,
                                                   PrivateNamespaceAction::RecoverPrivateLease);
            });
            ::_exit(rejected ? 0 : 91);
        }
        if (child > 0) {
            int status = 0;
            CHECK(::waitpid(child, &status, 0) == child);
            CHECK(WIFEXITED(status));
            CHECK(WEXITSTATUS(status) == 0);
            const auto& lock = begin_private_cleanup_action(
                permit, fixture.paths, PrivateNamespaceAction::RecoverPrivateLease);
            CHECK(lock.matches(fixture.paths.lock_path));
        }
    }
#endif
}

[[nodiscard]] std::filesystem::file_status
symlink_status_no_follow(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return std::filesystem::file_status(std::filesystem::file_type::not_found);
    }
    if (error) {
        throw std::filesystem::filesystem_error("inspect test path without following links", path,
                                                error);
    }
    return status;
}

[[nodiscard]] bool entry_exists_no_follow(const std::filesystem::path& path) {
    return std::filesystem::exists(symlink_status_no_follow(path));
}

[[nodiscard]] bool entry_is_symlink_no_follow(const std::filesystem::path& path) {
    return std::filesystem::is_symlink(symlink_status_no_follow(path));
}

void write_test_leaf(const std::filesystem::path& path, std::string_view payload) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create test leaf");
    }
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("could not write test leaf");
    }
}

[[maybe_unused, nodiscard]] std::vector<std::byte>
read_test_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("could not open test bytes");
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("could not size test bytes");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("could not read test bytes");
    }
    return bytes;
}

struct NamespaceTreeEntrySnapshot final {
    std::string relative_path;
    std::filesystem::file_type type = std::filesystem::file_type::none;
    std::filesystem::perms permissions = std::filesystem::perms::unknown;
    std::uintmax_t hard_link_count = 0;
    std::array<std::uint64_t, 4> identity{};
    std::vector<std::byte> bytes;
    std::filesystem::path symlink_target;

    friend bool operator==(const NamespaceTreeEntrySnapshot&,
                           const NamespaceTreeEntrySnapshot&) = default;
};

using NamespaceTreeSnapshot = std::vector<NamespaceTreeEntrySnapshot>;

[[nodiscard]] NamespaceTreeSnapshot capture_namespace_tree(
    const std::filesystem::path& root,
    const std::optional<std::filesystem::path>& excluded_relative_path = std::nullopt) {
    NamespaceTreeSnapshot snapshot;
    std::error_code error;
    std::filesystem::recursive_directory_iterator cursor(root, error);
    if (error) {
        throw std::filesystem::filesystem_error("open namespace snapshot root", root, error);
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; cursor != end; cursor.increment(error)) {
        if (error) {
            throw std::filesystem::filesystem_error("advance namespace snapshot", root, error);
        }
        const auto path = cursor->path();
        const auto relative_path = path.lexically_relative(root);
        if (excluded_relative_path && relative_path == *excluded_relative_path) {
            cursor.disable_recursion_pending();
            continue;
        }
        const auto status = std::filesystem::symlink_status(path, error);
        if (error) {
            throw std::filesystem::filesystem_error("inspect namespace snapshot leaf", path, error);
        }

        NamespaceTreeEntrySnapshot entry{
            .relative_path = relative_path.generic_string(),
            .type = status.type(),
            .permissions = status.permissions(),
        };
        if (std::filesystem::is_regular_file(status)) {
            const auto metadata = gnfs::relation::ooc_cleanup_detail::inspect_file(path, 0, false);
            if (metadata.kind == gnfs::relation::ooc_cleanup_detail::InspectKind::Present &&
                metadata.identity.size <=
                    static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                auto exact = gnfs::relation::ooc_cleanup_detail::inspect_file(
                    path, static_cast<std::size_t>(metadata.identity.size), true);
                if (exact.kind != gnfs::relation::ooc_cleanup_detail::InspectKind::Present ||
                    exact.identity != metadata.identity) {
                    throw std::runtime_error("namespace snapshot file changed during capture");
                }
                entry.hard_link_count = std::filesystem::hard_link_count(path, error);
                if (error) {
                    throw std::filesystem::filesystem_error("inspect namespace snapshot link count",
                                                            path, error);
                }
                entry.identity = {
                    exact.identity.first,
                    exact.identity.second,
                    exact.identity.third,
                    exact.identity.size,
                };
                entry.bytes = std::move(exact.bytes);
            } else {
#ifdef _WIN32
                throw std::runtime_error("could not inspect namespace snapshot file");
#else
                const auto inspect_native = [&path] {
                    struct stat result{};
                    int inspected = -1;
                    do {
                        inspected = ::lstat(path.c_str(), &result);
                    } while (inspected != 0 && errno == EINTR);
                    if (inspected != 0) {
                        throw std::filesystem::filesystem_error(
                            "inspect rejected namespace snapshot file", path,
                            std::error_code(errno, std::generic_category()));
                    }
                    return result;
                };
                const auto before = inspect_native();
                if (!S_ISREG(before.st_mode) || before.st_size < 0) {
                    throw std::runtime_error("rejected namespace snapshot leaf was not regular");
                }
                entry.bytes = read_test_bytes(path);
                const auto after = inspect_native();
                if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
                    before.st_mode != after.st_mode || before.st_nlink != after.st_nlink ||
                    before.st_size != after.st_size) {
                    throw std::runtime_error("namespace snapshot file changed during capture");
                }
                entry.hard_link_count = static_cast<std::uintmax_t>(after.st_nlink);
                entry.identity = {
                    static_cast<std::uint64_t>(after.st_dev),
                    static_cast<std::uint64_t>(after.st_ino),
                    0,
                    static_cast<std::uint64_t>(after.st_size),
                };
#endif
            }
        } else if (std::filesystem::is_directory(status)) {
            const auto identity =
                gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(path);
            if (!identity) {
                throw std::runtime_error("could not inspect namespace snapshot directory");
            }
            entry.identity = {(*identity)[0], (*identity)[1], (*identity)[2], 0};
        } else if (std::filesystem::is_symlink(status)) {
            entry.symlink_target = std::filesystem::read_symlink(path, error);
            if (error) {
                throw std::filesystem::filesystem_error("inspect namespace snapshot symlink target",
                                                        path, error);
            }
            cursor.disable_recursion_pending();
        }
        snapshot.push_back(std::move(entry));
    }
    if (error) {
        throw std::filesystem::filesystem_error("finish namespace snapshot", root, error);
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const auto& left, const auto& right) {
        return left.relative_path < right.relative_path;
    });
    return snapshot;
}

[[nodiscard]] NamespaceTreeSnapshot
without_namespace_subtree_contents(NamespaceTreeSnapshot snapshot,
                                   const std::filesystem::path& relative_subtree) {
    auto descendant_prefix = relative_subtree.generic_string();
    descendant_prefix.push_back('/');
    std::erase_if(snapshot, [&](const NamespaceTreeEntrySnapshot& entry) {
        return entry.relative_path.starts_with(descendant_prefix);
    });
    return snapshot;
}

[[nodiscard]] NamespaceTreeSnapshot
capture_private_lease_external_namespace_without_lock(const OOCCleanupPaths& paths) {
    const auto root = paths.private_directory.parent_path();
    return without_namespace_subtree_contents(
        capture_namespace_tree(root, paths.lock_path.filename()),
        paths.private_directory.filename());
}

[[maybe_unused]] void write_test_bytes(const std::filesystem::path& path,
                                       std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create test byte leaf");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("could not write test bytes");
    }
}

void check_test_bytes_preserved(const std::filesystem::path& path,
                                const std::vector<std::byte>& expected) {
    const bool present = entry_exists_no_follow(path);
    CHECK(present);
    if (present) {
        CHECK(read_test_bytes(path) == expected);
    }
}

[[nodiscard]] bool create_symlink_or_explicit_skip(const std::filesystem::path& target,
                                                   const std::filesystem::path& link,
                                                   [[maybe_unused]] std::string_view label) {
    std::error_code error;
    std::filesystem::create_symlink(target, link, error);
    if (!error) {
        return true;
    }
#ifdef _WIN32
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
        error.value() == ERROR_PRIVILEGE_NOT_HELD) {
        std::cout << "[SKIP] " << label << ": symlink privilege unavailable: " << error.message()
                  << '\n';
        return false;
    }
#endif
    CHECK(!error);
    return false;
}

[[nodiscard]] bool create_hard_link_checked(const std::filesystem::path& target,
                                            const std::filesystem::path& link) {
    std::error_code error;
    std::filesystem::create_hard_link(target, link, error);
    CHECK(!error);
    return !error;
}

void check_entries_equivalent(const std::filesystem::path& first,
                              const std::filesystem::path& second) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(first, second, error);
    CHECK(!error);
    CHECK(equivalent);
}

void write_u64(std::ofstream& output, std::uint64_t value) {
    output.write(reinterpret_cast<const char*>(&value),
                 static_cast<std::streamsize>(sizeof(value)));
    if (!output) {
        throw std::runtime_error("could not write test u64");
    }
}

void pad_to(std::ofstream& output, std::uint64_t size) {
    const auto position = output.tellp();
    if (position == std::streampos(-1) || static_cast<std::uint64_t>(position) > size) {
        throw std::runtime_error("invalid test file extent");
    }
    for (std::uint64_t cursor = static_cast<std::uint64_t>(position); cursor < size; ++cursor) {
        output.put(static_cast<char>(cursor & 0xffU));
    }
    if (!output) {
        throw std::runtime_error("could not pad test file");
    }
}

void write_index(const std::filesystem::path& path, std::uint64_t magic, std::uint64_t store_id,
                 std::uint64_t count, std::uint64_t index_size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create test index");
    }
    write_u64(output, magic);
    write_u64(output, OOCRelationStoreFormat::FORMAT_VERSION_V3);
    write_u64(output, store_id);
    write_u64(output, count);
    pad_to(output, index_size);
}

void write_data(const std::filesystem::path& path, std::uint64_t store_id,
                std::uint64_t data_size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create test data");
    }
    write_u64(output, OOCRelationStoreFormat::MAGIC_V3_DATA);
    write_u64(output, OOCRelationStoreFormat::FORMAT_VERSION_V3);
    write_u64(output, store_id);
    pad_to(output, data_size);
}

struct PairShape final {
    std::uint64_t magic = OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE;
    std::uint64_t count = 0;
    std::uint64_t index_size = OOCRelationStoreFormat::INDEX_HEADER_BYTES;
    std::uint64_t data_size = OOCRelationStoreFormat::DATA_HEADER_BYTES + 16;
};

struct RegisteredOwnedPair final {
    std::uint64_t logical_store_id = 0;
    std::uint64_t actual_store_id = 0;
    std::optional<OOCCleanupOwnershipReceipt> ownership;
};

[[nodiscard]] std::string owned_pair_key(const std::filesystem::path& base) {
    return OOCCleanupTransaction::paths_for(base).base_path.string();
}

[[nodiscard]] std::unordered_map<std::string, RegisteredOwnedPair>& owned_pair_registry() {
    static std::unordered_map<std::string, RegisteredOwnedPair> registry;
    return registry;
}

void register_cleanup_ownership(const std::filesystem::path& base, std::uint64_t logical_store_id,
                                std::uint64_t actual_store_id,
                                OOCCleanupOwnershipReceipt ownership) {
    auto& registry = owned_pair_registry();
    const auto key = owned_pair_key(base);
    registry.erase(key);
    registry.emplace(key, RegisteredOwnedPair{
                              .logical_store_id = logical_store_id,
                              .actual_store_id = actual_store_id,
                              .ownership = std::move(ownership),
                          });
}

[[nodiscard]] Relation make_real_relation(std::int64_t a, std::uint64_t b) {
    Relation relation(a, b);
    relation.rational_factors.push_back(static_cast<std::uint32_t>(100 + a));
    relation.algebraic_factors.push_back(static_cast<std::uint32_t>(200 + a));
    return relation;
}

void write_pair(const std::filesystem::path& base, std::uint64_t store_id,
                const PairShape& shape = {}) {
    OOCRelationWriter writer(base.string());
    const std::uint64_t actual_store_id = writer.store_id();
    writer.abort();
    auto ownership = writer.take_cleanup_ownership_receipt();
    write_index(base.string() + ".relidx", shape.magic, actual_store_id, shape.count,
                shape.index_size);
    write_data(base.string() + ".reldata", actual_store_id, shape.data_size);
    register_cleanup_ownership(base, store_id, actual_store_id, std::move(ownership));
}

void write_pair(const std::filesystem::path& base, std::uint64_t store_id,
                OOCPrivateLeaseOwnershipReceipt& private_lease, const PairShape& shape = {}) {
    OOCRelationWriter writer(base.string(), private_lease);
    const std::uint64_t actual_store_id = writer.store_id();
    writer.abort();
    auto ownership = writer.take_cleanup_ownership_receipt();
    write_index(base.string() + ".relidx", shape.magic, actual_store_id, shape.count,
                shape.index_size);
    write_data(base.string() + ".reldata", actual_store_id, shape.data_size);
    register_cleanup_ownership(base, store_id, actual_store_id, std::move(ownership));
}

[[nodiscard]] OOCCleanupOwnershipReceipt
capture_cleanup_ownership(const std::filesystem::path& base_path, std::uint64_t store_id) {
    auto& registry = owned_pair_registry();
    const auto found = registry.find(owned_pair_key(base_path));
    if (found == registry.end() || found->second.logical_store_id != store_id ||
        !found->second.ownership) {
        throw std::logic_error("test pair has no matching production cleanup ownership");
    }
    OOCCleanupOwnershipReceipt ownership(std::move(*found->second.ownership));
    found->second.ownership.reset();
    return ownership;
}

[[nodiscard]] std::uint64_t actual_store_id_for(const std::filesystem::path& base_path,
                                                std::uint64_t logical_store_id) {
    const auto& registry = owned_pair_registry();
    const auto found = registry.find(owned_pair_key(base_path));
    if (found == registry.end() || found->second.logical_store_id != logical_store_id) {
        throw std::logic_error("test pair has no matching production store identity");
    }
    return found->second.actual_store_id;
}

[[nodiscard]] gnfs::relation::OOCCleanupResult begin_cleanup(const OOCCleanupRequest& request,
                                                             OOCCleanupTestHooks hooks = {}) {
    auto ownership = capture_cleanup_ownership(request.base_path, request.store_id);
    return OOCCleanupTransaction::begin_or_resume(ownership, request.exact, hooks);
}

[[nodiscard]] gnfs::relation::OOCCleanupResult begin_cleanup(const std::filesystem::path& base_path,
                                                             std::uint64_t store_id,
                                                             OOCCleanupTestHooks hooks = {}) {
    auto ownership = capture_cleanup_ownership(base_path, store_id);
    return OOCCleanupTransaction::begin_or_resume(ownership, std::nullopt, hooks);
}

[[nodiscard]] OOCExactCleanupExpectation exact_for(const PairShape& shape) {
    return OOCExactCleanupExpectation{
        .index_magic = shape.magic,
        .persisted_count = shape.count,
        .index_size = shape.index_size,
        .data_size = shape.data_size,
    };
}

[[nodiscard]] OOCExactCleanupExpectation exact_for(const OOCSnapshotDescriptor& descriptor) {
    return OOCExactCleanupExpectation{
        .index_magic = OOCRelationStoreFormat::MAGIC_V3_FINAL,
        .persisted_count = descriptor.count,
        .index_size = OOCRelationWriter::index_size_for_count(descriptor.count),
        .data_size = descriptor.data_end,
    };
}

[[maybe_unused, nodiscard]] OOCPrivateHandoffPairDescriptorV1
handoff_pair_descriptor(const OOCSnapshotDescriptor& descriptor) {
    return OOCPrivateHandoffPairDescriptorV1{
        .format_version = descriptor.format_version,
        .store_id = descriptor.store_id,
        .generation = descriptor.generation,
        .count = descriptor.count,
        .index_extent = OOCRelationWriter::index_size_for_count(descriptor.count),
        .data_extent = descriptor.data_end,
    };
}

[[nodiscard]] gnfs::util::Sha256Digest authorized_cleanup_test_digest(std::uint8_t seed) {
    gnfs::util::Sha256Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index * 29U));
    }
    return digest;
}

[[nodiscard]] gnfs::util::durable_immutable_record::NativeIdentity
authorized_cleanup_test_identity(std::uint64_t seed) {
    return {
        .first = seed,
        .second = seed + 1,
        .third = seed + 2,
    };
}

[[nodiscard]] std::vector<std::byte>
authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2 kind) {
    OOCAuthorizedCleanupIntentV2 marker;
    marker.marker_kind = kind;
    marker.base_path_digest = authorized_cleanup_test_digest(0x01);
    marker.external_authorization_digest = authorized_cleanup_test_digest(0x21);
    marker.generic_handoff_self_digest = authorized_cleanup_test_digest(0x41);
    marker.lease_id = {UINT64_C(0x0102030405060708), UINT64_C(0x1112131415161718)};
    marker.parent_directory_identity =
        authorized_cleanup_test_identity(UINT64_C(0x2122232425262728));
    marker.lock_identity = authorized_cleanup_test_identity(UINT64_C(0x3132333435363738));
    marker.directory_identity = authorized_cleanup_test_identity(UINT64_C(0x4142434445464748));
    marker.owner_marker_identity = authorized_cleanup_test_identity(UINT64_C(0x5152535455565758));
    marker.owned_marker_identity = authorized_cleanup_test_identity(UINT64_C(0x6162636465666768));
    marker.pair = OOCPrivateHandoffPairDescriptorV1{
        .format_version = OOCRelationStoreFormat::FORMAT_VERSION_V3,
        .store_id = UINT64_C(0x7172737475767778),
        .generation = UINT64_C(0x8182838485868788),
        .count = 0,
        .index_extent = OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                        OOCRelationStoreFormat::INDEX_SENTINEL_BYTES,
        .data_extent = OOCRelationStoreFormat::DATA_HEADER_BYTES,
    };
    marker.handoff = {
        .identity = authorized_cleanup_test_identity(UINT64_C(0x9192939495969798)),
        .extent = gnfs::relation::OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1,
    };
    marker.index = {
        .identity = authorized_cleanup_test_identity(UINT64_C(0xa1a2a3a4a5a6a7a8)),
        .extent = marker.pair.index_extent,
    };
    marker.data = {
        .identity = authorized_cleanup_test_identity(UINT64_C(0xb1b2b3b4b5b6b7b8)),
        .extent = marker.pair.data_extent,
    };
    const auto sealed = gnfs::relation::seal_ooc_authorized_cleanup_intent(marker);
    if (!sealed) {
        throw std::runtime_error("could not seal authorized cleanup V2 test marker");
    }
    const auto encoded = gnfs::relation::encode_ooc_authorized_cleanup_intent(marker);
    if (!encoded || !encoded.bytes.has_value()) {
        throw std::runtime_error("could not encode authorized cleanup V2 test marker");
    }
    return *encoded.bytes;
}

[[nodiscard]] OOCPrivateLeaseReservation
prepare_private_legacy_cleanup_intent(const std::filesystem::path& base) {
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    if (!reservation.completed()) {
        throw std::runtime_error("could not reserve private legacy cleanup fixture");
    }
    {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        const auto descriptor = writer.finalize_and_publish_cleanup_handoff();
        if (descriptor.count != 0) {
            throw std::runtime_error("unexpected private legacy cleanup fixture count");
        }
    }
    const auto paths = OOCCleanupTransaction::paths_for(base);
    if (!entry_exists_no_follow(paths.intent_path) ||
        entry_exists_no_follow(paths.intent_pending_path)) {
        throw std::runtime_error("private legacy cleanup intent was not canonical");
    }
    return reservation;
}

constexpr std::uint32_t PRIVATE_HANDOFF_PAYLOAD_KIND = 0x474E4653U;
constexpr std::uint32_t PRIVATE_HANDOFF_PAYLOAD_VERSION = 1;
constexpr std::array<std::byte, 5> PRIVATE_HANDOFF_PAYLOAD{
    std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}, std::byte{0x50},
};

struct PreparedPrivateHandoff final {
    OOCSnapshotDescriptor descriptor;
    OOCCleanupOwnershipReceipt pair_ownership;
    OOCPrivateLeaseOwnershipReceipt lease_ownership;
};

[[nodiscard]] PreparedPrivateHandoff prepare_private_handoff(const std::filesystem::path& base,
                                                             bool write_relation = true) {
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    if (!reservation.completed()) {
        throw std::runtime_error("could not reserve private handoff fixture");
    }

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    if (write_relation) {
        (void)writer.write(make_real_relation(17, 19));
    }
    const auto descriptor = writer.finalize();
    auto pair_ownership = writer.take_cleanup_ownership_receipt();
    return PreparedPrivateHandoff{
        .descriptor = descriptor,
        .pair_ownership = std::move(pair_ownership),
        .lease_ownership = std::move(*reservation.ownership),
    };
}

[[nodiscard]] OOCPrivateHandoffPublishResult
publish_private_handoff(PreparedPrivateHandoff& prepared, OOCPrivateHandoffTestHooks hooks = {}) {
    return OOCCleanupTransaction::publish_private_handoff(
        prepared.pair_ownership, prepared.lease_ownership,
        handoff_pair_descriptor(prepared.descriptor), PRIVATE_HANDOFF_PAYLOAD_KIND,
        PRIVATE_HANDOFF_PAYLOAD_VERSION, PRIVATE_HANDOFF_PAYLOAD, hooks);
}

[[nodiscard]] int run_private_handoff_adoption_child(std::string_view operation,
                                                     const std::filesystem::path& base) {
#if defined(__APPLE__)
    try {
        if (operation == "publish-exit" || operation == "publish-empty-exit") {
            auto prepared = prepare_private_handoff(base, operation == "publish-exit");
            if (!publish_private_handoff(prepared).canonical()) {
                return 91;
            }
            std::_Exit(0);
        }
        if (operation == "publish-pending-exit" || operation == "publish-canonical-exit") {
            auto prepared = prepare_private_handoff(base);
            auto target = operation == "publish-pending-exit"
                              ? OOCPrivateHandoffFaultPoint::PendingDurable
                              : OOCPrivateHandoffFaultPoint::CanonicalDurable;
            const auto exit_at_target = [](OOCPrivateHandoffFaultPoint point,
                                           void* opaque) noexcept {
                if (point == *static_cast<const OOCPrivateHandoffFaultPoint*>(opaque)) {
                    std::_Exit(0);
                }
                return false;
            };
            (void)publish_private_handoff(prepared, OOCPrivateHandoffTestHooks{
                                                        .stop_after = exit_at_target,
                                                        .context = &target,
                                                    });
            return 96;
        }
        if (operation == "adopt-exit") {
            auto adopted = OOCCleanupTransaction::adopt_private_handoff(base);
            if (!adopted.adopted()) {
                return 92;
            }
            OOCPrivateHandoffReader reader(std::move(*adopted.adoption));
            if (!reader.valid() || reader.reader().count() != 1) {
                return 93;
            }
            const auto relation = reader.reader().read(0);
            if (relation.a != 17 || relation.b != 19) {
                return 94;
            }
            std::_Exit(0);
        }
    } catch (...) {
        return 95;
    }
#else
    (void)operation;
    (void)base;
#endif
    return 64;
}

void set_private_control_leaf_mode(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, error);
    if (error) {
        throw std::filesystem::filesystem_error("set private control leaf mode", path, error);
    }
}

void write_private_control_bytes(const std::filesystem::path& path,
                                 std::span<const std::byte> bytes) {
    write_test_bytes(path, bytes);
    set_private_control_leaf_mode(path);
}

[[nodiscard]] bool replace_private_control_leaf_same_bytes(const std::filesystem::path& path) {
    const auto bytes = read_test_bytes(path);
    const auto before = gnfs::relation::ooc_cleanup_detail::inspect_file(path, 0, false);
    auto replacement = path;
    replacement += ".test-replacement";
    write_private_control_bytes(replacement, bytes);

    std::error_code error;
    if (!std::filesystem::remove(path, error) || error) {
        throw std::runtime_error("could not remove private control leaf");
    }
    std::filesystem::rename(replacement, path, error);
    if (error) {
        throw std::filesystem::filesystem_error("replace private control leaf", replacement, path,
                                                error);
    }
    const auto after = gnfs::relation::ooc_cleanup_detail::inspect_file(path, 0, false);
    return before.kind == gnfs::relation::ooc_cleanup_detail::InspectKind::Present &&
           after.kind == gnfs::relation::ooc_cleanup_detail::InspectKind::Present &&
           before.identity != after.identity && before.bytes == after.bytes;
}

void store_u32_le(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
        throw std::runtime_error("test u32 mutation is out of range");
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> static_cast<unsigned int>(index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::vector<std::byte>
encode_private_handoff_record(const OOCPrivateHandoffRecordV1& record) {
    const auto encoded = gnfs::relation::encode_ooc_private_handoff_record(record);
    if (!encoded || !encoded.bytes) {
        throw std::runtime_error("could not encode private handoff test record");
    }
    return *encoded.bytes;
}

void flip_last_byte(const std::filesystem::path& path) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        throw std::runtime_error("could not open marker for corruption");
    }
    stream.seekg(-1, std::ios::end);
    char byte = 0;
    stream.read(&byte, 1);
    if (!stream) {
        throw std::runtime_error("could not read marker byte");
    }
    byte ^= static_cast<char>(0x80);
    stream.seekp(-1, std::ios::end);
    stream.write(&byte, 1);
    stream.flush();
    if (!stream) {
        throw std::runtime_error("could not corrupt marker byte");
    }
}

void overwrite_count(const std::filesystem::path& index_path, std::uint64_t count) {
    std::fstream stream(index_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        throw std::runtime_error("could not open index for count mutation");
    }
    stream.seekp(static_cast<std::streamoff>(OOCRelationStoreFormat::INDEX_COUNT_OFFSET));
    stream.write(reinterpret_cast<const char*>(&count),
                 static_cast<std::streamsize>(sizeof(count)));
    stream.flush();
    if (!stream) {
        throw std::runtime_error("could not mutate index count");
    }
}

struct StopContext final {
    OOCCleanupFaultPoint target = OOCCleanupFaultPoint::IntentDurable;
    bool stopped = false;
};

[[nodiscard]] bool stop_at(OOCCleanupFaultPoint point, void* opaque) noexcept {
    auto& context = *static_cast<StopContext*>(opaque);
    if (!context.stopped && point == context.target) {
        context.stopped = true;
        return true;
    }
    return false;
}

[[nodiscard]] OOCCleanupTestHooks stop_hooks(StopContext& context) noexcept {
    return OOCCleanupTestHooks{
        .stop_after = stop_at,
        .stop_after_publish = nullptr,
        .fail_before_operation = nullptr,
        .context = &context,
    };
}

struct PublishStopContext final {
    OOCCleanupPublishFaultPoint target = OOCCleanupPublishFaultPoint::IntentPendingDurable;
    bool stopped = false;
};

[[nodiscard]] bool stop_at_publish(OOCCleanupPublishFaultPoint point, void* opaque) noexcept {
    auto& context = *static_cast<PublishStopContext*>(opaque);
    if (!context.stopped && point == context.target) {
        context.stopped = true;
        return true;
    }
    return false;
}

[[nodiscard]] OOCCleanupTestHooks publish_stop_hooks(PublishStopContext& context) noexcept {
    return OOCCleanupTestHooks{
        .stop_after = nullptr,
        .stop_after_publish = stop_at_publish,
        .fail_before_operation = nullptr,
        .context = &context,
    };
}

struct PrivateHandoffStopContext final {
    OOCPrivateHandoffFaultPoint target = OOCPrivateHandoffFaultPoint::PendingDurable;
    bool stopped = false;
};

[[nodiscard]] bool stop_at_private_handoff(OOCPrivateHandoffFaultPoint point,
                                           void* opaque) noexcept {
    auto& context = *static_cast<PrivateHandoffStopContext*>(opaque);
    if (!context.stopped && point == context.target) {
        context.stopped = true;
        return true;
    }
    return false;
}

[[nodiscard]] OOCPrivateHandoffTestHooks
private_handoff_stop_hooks(PrivateHandoffStopContext& context) noexcept {
    return OOCPrivateHandoffTestHooks{
        .stop_after = stop_at_private_handoff,
        .context = &context,
    };
}

constexpr std::array PRIVATE_HANDOFF_FAULT_POINTS{
    OOCPrivateHandoffFaultPoint::PendingDurable,
    OOCPrivateHandoffFaultPoint::CanonicalPromoted,
    OOCPrivateHandoffFaultPoint::CanonicalDurable,
    OOCPrivateHandoffFaultPoint::ReservedRevokedDurable,
};

struct OperationFailureContext final {
    OOCCleanupTestOperation target = OOCCleanupTestOperation::IndexRename;
    bool failed = false;
};

[[nodiscard]] bool fail_operation_once(OOCCleanupTestOperation operation, void* opaque) noexcept {
    auto& context = *static_cast<OperationFailureContext*>(opaque);
    if (!context.failed && operation == context.target) {
        context.failed = true;
        return true;
    }
    return false;
}

[[nodiscard]] OOCCleanupTestHooks
operation_failure_hooks(OperationFailureContext& context) noexcept {
    return OOCCleanupTestHooks{
        .stop_after = nullptr,
        .stop_after_publish = nullptr,
        .fail_before_operation = fail_operation_once,
        .context = &context,
    };
}

constexpr std::array CLEANUP_FAULT_POINTS{
    OOCCleanupFaultPoint::IntentDurable,        OOCCleanupFaultPoint::FirstRenameDurable,
    OOCCleanupFaultPoint::SecondRenameDurable,  OOCCleanupFaultPoint::DeleteAuthorizedDurable,
    OOCCleanupFaultPoint::FirstUnlinkDurable,   OOCCleanupFaultPoint::SecondUnlinkDurable,
    OOCCleanupFaultPoint::IntentRemovedDurable,
};

[[nodiscard]] OOCCleanupStage expected_stage(OOCCleanupFaultPoint point) {
    switch (point) {
    case OOCCleanupFaultPoint::IntentDurable:
        return OOCCleanupStage::IntentDurable;
    case OOCCleanupFaultPoint::FirstRenameDurable:
        return OOCCleanupStage::IndexQuarantined;
    case OOCCleanupFaultPoint::SecondRenameDurable:
        return OOCCleanupStage::PairQuarantined;
    case OOCCleanupFaultPoint::DeleteAuthorizedDurable:
        return OOCCleanupStage::DeleteAuthorized;
    case OOCCleanupFaultPoint::FirstUnlinkDurable:
        return OOCCleanupStage::DataRemoved;
    case OOCCleanupFaultPoint::SecondUnlinkDurable:
        return OOCCleanupStage::IndexRemoved;
    case OOCCleanupFaultPoint::IntentRemovedDurable:
        return OOCCleanupStage::IntentRemoved;
    case OOCCleanupFaultPoint::LegacyCleanupPermitAcquired:
    case OOCCleanupFaultPoint::PrivateLeaseCleanupHandoffPermitAcquired:
        return OOCCleanupStage::None;
    }
    throw std::runtime_error("unknown fault point");
}

void check_fault_namespace(const gnfs::relation::OOCCleanupPaths& paths,
                           OOCCleanupFaultPoint point) {
    if (point == OOCCleanupFaultPoint::LegacyCleanupPermitAcquired ||
        point == OOCCleanupFaultPoint::PrivateLeaseCleanupHandoffPermitAcquired) {
        CHECK(!exists(paths.intent_path));
        CHECK(!exists(paths.intent_pending_path));
        CHECK(!exists(paths.staged_path));
        CHECK(!exists(paths.staged_pending_path));
        CHECK(exists(paths.index_path));
        CHECK(exists(paths.data_path));
        CHECK(!exists(paths.quarantine_index_path));
        CHECK(!exists(paths.quarantine_data_path));
        return;
    }
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(exists(paths.intent_path) != (point == OOCCleanupFaultPoint::IntentRemovedDurable));
    CHECK(exists(paths.staged_path) == (point == OOCCleanupFaultPoint::DeleteAuthorizedDurable ||
                                        point == OOCCleanupFaultPoint::FirstUnlinkDurable ||
                                        point == OOCCleanupFaultPoint::SecondUnlinkDurable ||
                                        point == OOCCleanupFaultPoint::IntentRemovedDurable));

    switch (point) {
    case OOCCleanupFaultPoint::IntentDurable:
        CHECK(exists(paths.index_path));
        CHECK(exists(paths.data_path));
        CHECK(!exists(paths.quarantine_index_path));
        CHECK(!exists(paths.quarantine_data_path));
        break;
    case OOCCleanupFaultPoint::FirstRenameDurable:
        CHECK(!exists(paths.index_path));
        CHECK(exists(paths.data_path));
        CHECK(exists(paths.quarantine_index_path));
        CHECK(!exists(paths.quarantine_data_path));
        break;
    case OOCCleanupFaultPoint::SecondRenameDurable:
    case OOCCleanupFaultPoint::DeleteAuthorizedDurable:
        CHECK(!exists(paths.index_path));
        CHECK(!exists(paths.data_path));
        CHECK(exists(paths.quarantine_index_path));
        CHECK(exists(paths.quarantine_data_path));
        break;
    case OOCCleanupFaultPoint::FirstUnlinkDurable:
        CHECK(!exists(paths.index_path));
        CHECK(!exists(paths.data_path));
        CHECK(exists(paths.quarantine_index_path));
        CHECK(!exists(paths.quarantine_data_path));
        break;
    case OOCCleanupFaultPoint::SecondUnlinkDurable:
    case OOCCleanupFaultPoint::IntentRemovedDurable:
        CHECK(!exists(paths.index_path));
        CHECK(!exists(paths.data_path));
        CHECK(!exists(paths.quarantine_index_path));
        CHECK(!exists(paths.quarantine_data_path));
        break;
    case OOCCleanupFaultPoint::LegacyCleanupPermitAcquired:
    case OOCCleanupFaultPoint::PrivateLeaseCleanupHandoffPermitAcquired:
        break;
    }
}

void check_cleanup_complete(const gnfs::relation::OOCCleanupPaths& paths) {
    CHECK(!exists(paths.index_path));
    CHECK(!exists(paths.data_path));
    CHECK(!exists(paths.intent_path));
    CHECK(!exists(paths.intent_pending_path));
    CHECK(!exists(paths.staged_path));
    CHECK(!exists(paths.staged_pending_path));
    CHECK(!exists(paths.quarantine_index_path));
    CHECK(!exists(paths.quarantine_data_path));
}

void test_fault_point_recovery() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1234'5678'9abc'def0ULL;

    for (std::size_t index = 0; index < CLEANUP_FAULT_POINTS.size(); ++index) {
        const auto base = temp.path() / ("fault-" + std::to_string(index));
        write_pair(base, store_id + index);
        StopContext stop{.target = CLEANUP_FAULT_POINTS[index]};
        const auto interrupted = begin_cleanup(base, store_id + index, stop_hooks(stop));
        CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
        CHECK(interrupted.stage == expected_stage(CLEANUP_FAULT_POINTS[index]));
        CHECK(stop.stopped);

        const auto paths = OOCCleanupTransaction::paths_for(base);
        check_fault_namespace(paths, CLEANUP_FAULT_POINTS[index]);
        const auto resumed = OOCCleanupTransaction::resume(base);
        CHECK(resumed.completed());
        check_cleanup_complete(paths);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NoTransaction);
    }
}

void test_receipt_authority_and_pending_publication() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x9191'a2a2'b3b3'c4c4ULL;

    {
        const auto base = temp.path() / "intent-pending-retry";
        write_pair(base, store_id);
        auto ownership = capture_cleanup_ownership(base, store_id);
        PublishStopContext stop{
            .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
        };
        const auto interrupted = OOCCleanupTransaction::begin_or_resume(ownership, std::nullopt,
                                                                        publish_stop_hooks(stop));
        const auto paths = OOCCleanupTransaction::paths_for(base);
        CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
        CHECK(interrupted.stage == OOCCleanupStage::None);
        CHECK(stop.stopped);
        CHECK(!ownership.spent());
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));

        // A crash or short write can corrupt only the no-authority pending
        // leaf. The same unspent ownership capability repairs it in place.
        flip_last_byte(paths.intent_pending_path);
        CHECK(OOCCleanupTransaction::begin_or_resume(ownership).completed());
        CHECK(ownership.spent());
        CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
              OOCCleanupStatus::InvalidRequest);
        check_cleanup_complete(paths);
    }

    {
        const auto base = temp.path() / "staged-pending-retry";
        write_pair(base, store_id + 1);
        auto ownership = capture_cleanup_ownership(base, store_id + 1);
        PublishStopContext stop{
            .target = OOCCleanupPublishFaultPoint::StagedPendingDurable,
        };
        const auto interrupted = OOCCleanupTransaction::begin_or_resume(ownership, std::nullopt,
                                                                        publish_stop_hooks(stop));
        const auto paths = OOCCleanupTransaction::paths_for(base);
        CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
        CHECK(interrupted.stage == OOCCleanupStage::PairQuarantined);
        CHECK(stop.stopped);
        CHECK(ownership.spent());
        CHECK(entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.staged_pending_path));
        CHECK(!entry_exists_no_follow(paths.staged_path));
        CHECK(entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(entry_exists_no_follow(paths.quarantine_data_path));
        CHECK(OOCCleanupTransaction::resume(base).completed());
        check_cleanup_complete(paths);
    }

    {
        const auto base = temp.path() / "receipt-identity-replacement";
        write_pair(base, store_id + 2);
        auto ownership = capture_cleanup_ownership(base, store_id + 2);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved_data = temp.path() / "receipt-owned-data";
        std::filesystem::rename(paths.data_path, saved_data);
        write_data(paths.data_path, actual_store_id_for(base, store_id + 2),
                   OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
        CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
              OOCCleanupStatus::SourcePairInvalid);
        CHECK(!ownership.spent());
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(entry_exists_no_follow(saved_data));
        CHECK(entry_exists_no_follow(paths.index_path));
    }

    {
        const auto base = temp.path() / "receipt-empty-terminal";
        write_pair(base, store_id + 3);
        auto ownership = capture_cleanup_ownership(base, store_id + 3);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        std::filesystem::remove(paths.index_path);
        std::filesystem::remove(paths.data_path);
        CHECK(OOCCleanupTransaction::begin_or_resume(ownership).completed());
        CHECK(ownership.spent());
    }

    {
        const auto base = temp.path() / "receipt-one-shot-move";
        OOCRelationWriter writer(base.string());
        const auto descriptor = writer.finalize();
        auto source = writer.take_cleanup_ownership_receipt();
        bool second_transfer_rejected = false;
        try {
            (void)writer.take_cleanup_ownership_receipt();
        } catch (const std::logic_error&) {
            second_transfer_rejected = true;
        }
        CHECK(second_transfer_rejected);

        OOCCleanupOwnershipReceipt destination(std::move(source));
        CHECK(source.spent());
        CHECK(!destination.spent());
        CHECK(OOCCleanupTransaction::begin_or_resume(source).status ==
              OOCCleanupStatus::InvalidRequest);
        const auto moved_cleanup = OOCCleanupTransaction::begin_or_resume(
            destination,
            OOCExactCleanupExpectation{
                .index_magic = OOCRelationStoreFormat::MAGIC_V3_FINAL,
                .persisted_count = descriptor.count,
                .index_size = OOCRelationWriter::index_size_for_count(descriptor.count),
                .data_size = descriptor.data_end,
            });
        CHECK(moved_cleanup.completed());
        CHECK(destination.spent());
        check_cleanup_complete(OOCCleanupTransaction::paths_for(base));
    }
}

void test_namespace_operation_failures_are_retryable() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xa1a1'b2b2'c3c3'd4d0ULL;
    struct FailureCase final {
        OOCCleanupTestOperation operation;
        OOCCleanupStatus expected_status;
        bool retry_reports_no_transaction = false;
    };
    constexpr std::array cases{
        FailureCase{OOCCleanupTestOperation::IndexRename, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::IndexRenameParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::DataRename, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::DataRenameParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::DataUnlink, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::DataUnlinkParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::IndexUnlink, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::IndexUnlinkParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::IntentUnlink, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::IntentUnlinkParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::StagedUnlink, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::StagedUnlinkParentSync,
                    OOCCleanupStatus::DurabilityFailure, true},
    };

    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto base = temp.path() / ("operation-failure-" + std::to_string(index));
        write_pair(base, store_id + index);
        auto ownership = capture_cleanup_ownership(base, store_id + index);
        OperationFailureContext failure{.target = cases[index].operation};
        const auto result = OOCCleanupTransaction::begin_or_resume(
            ownership, std::nullopt, operation_failure_hooks(failure));
        CHECK(failure.failed);
        CHECK(result.status == cases[index].expected_status);
        CHECK(result.retryable());
        CHECK(ownership.spent());

        const auto resumed = OOCCleanupTransaction::resume(base);
        if (cases[index].retry_reports_no_transaction) {
            CHECK(resumed.status == OOCCleanupStatus::NoTransaction);
        } else {
            CHECK(resumed.completed());
        }
        CHECK(resumed.transaction_terminal());
        CHECK(OOCCleanupTransaction::confirm_pair_namespace_reusable(base).completed());
        check_cleanup_complete(OOCCleanupTransaction::paths_for(base));
    }
}

void test_reserved_cleanup_suffix_is_rejected() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xb1b1'c2c2'd3d3'e4e4ULL;
    const auto base = temp.path() / "foreign.gnfs-ooc-cleanup-v1";
    write_index(base.string() + ".relidx", OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE, store_id, 0,
                OOCRelationStoreFormat::INDEX_HEADER_BYTES);
    write_data(base.string() + ".reldata", store_id,
               OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
    const auto result = OOCCleanupTransaction::resume(base);
    CHECK(result.status == OOCCleanupStatus::InvalidRequest);
    CHECK(entry_exists_no_follow(base.string() + ".relidx"));
    CHECK(entry_exists_no_follow(base.string() + ".reldata"));
}

void test_fresh_writer_rejects_nonempty_cleanup_namespace() {
    TempDirectory temp;
    constexpr std::array<std::string_view, 8> labels{
        "index",  "data",           "intent",           "intent-pending",
        "staged", "staged-pending", "quarantine-index", "quarantine-data",
    };

    for (std::size_t index = 0; index < labels.size(); ++index) {
        const auto base = temp.path() / ("fresh-reuse-" + std::to_string(index));
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const std::array<const std::filesystem::path*, 8> leaves{
            &paths.index_path,
            &paths.data_path,
            &paths.intent_path,
            &paths.intent_pending_path,
            &paths.staged_path,
            &paths.staged_pending_path,
            &paths.quarantine_index_path,
            &paths.quarantine_data_path,
        };
        write_test_leaf(*leaves[index], labels[index]);

        bool rejected = false;
        try {
            OOCRelationWriter writer(base.string());
            (void)writer;
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(entry_exists_no_follow(*leaves[index]));
        if (index != 0) {
            CHECK(!entry_exists_no_follow(paths.index_path));
        }
        if (index != 1) {
            CHECK(!entry_exists_no_follow(paths.data_path));
        }
    }
}

void test_windows_sharing_violation_is_retryable() {
#ifdef _WIN32
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xc1c1'd2d2'e3e3'f4f4ULL;
    const auto base = temp.path() / "windows-sharing";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::DeleteAuthorizedDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);

    const HANDLE held = ::CreateFileW(paths.quarantine_data_path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    if (held == INVALID_HANDLE_VALUE) {
        return;
    }
    const auto blocked = OOCCleanupTransaction::resume(base);
    CHECK(blocked.status == OOCCleanupStatus::IoFailure);
    CHECK(blocked.retryable());
    CHECK(entry_exists_no_follow(paths.quarantine_data_path));
    CHECK(::CloseHandle(held) != FALSE);
    CHECK(OOCCleanupTransaction::resume(base).completed());
    check_cleanup_complete(paths);
#endif
}

void test_exact_finalized_expectation() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xabc0'1234'5678'9001ULL;
    const PairShape shape{
        .magic = OOCRelationStoreFormat::MAGIC_V3_FINAL,
        .count = 2,
        .index_size = OOCRelationStoreFormat::INDEX_HEADER_BYTES + 3 * sizeof(std::uint64_t),
        .data_size = OOCRelationStoreFormat::DATA_HEADER_BYTES + 48,
    };

    {
        const auto base = temp.path() / "finalized-convenience-rejected";
        write_pair(base, store_id, shape);
        const auto result = begin_cleanup(base, store_id);
        CHECK(result.status == OOCCleanupStatus::SourcePairInvalid);
        CHECK(std::filesystem::exists(base.string() + ".relidx"));
        CHECK(std::filesystem::exists(base.string() + ".reldata"));
    }

    {
        const auto base = temp.path() / "finalized-exact-mismatch";
        write_pair(base, store_id + 1, shape);
        auto wrong = exact_for(shape);
        ++wrong.data_size;
        const OOCCleanupRequest request{
            .base_path = base,
            .store_id = store_id + 1,
            .exact = wrong,
        };
        const auto result = begin_cleanup(request);
        CHECK(result.status == OOCCleanupStatus::SourcePairInvalid);
        CHECK(std::filesystem::exists(base.string() + ".relidx"));
        CHECK(std::filesystem::exists(base.string() + ".reldata"));
    }

    {
        const auto base = temp.path() / "finalized-exact";
        write_pair(base, store_id + 2, shape);
        const OOCCleanupRequest request{
            .base_path = base,
            .store_id = store_id + 2,
            .exact = exact_for(shape),
        };
        CHECK(begin_cleanup(request).completed());
        check_cleanup_complete(OOCCleanupTransaction::paths_for(base));
    }
}

void test_real_finalized_store_cleanup() {
    TempDirectory temp;
    const auto base = temp.path() / "real-finalized";
    OOCSnapshotDescriptor descriptor;
    std::optional<OOCCleanupOwnershipReceipt> ownership;
    {
        OOCRelationWriter writer(base.string());
        CHECK(writer.write(make_real_relation(11, 12)) == 0);
        CHECK(writer.write(make_real_relation(13, 14)) == 1);
        descriptor = writer.finalize();
        ownership.emplace(writer.take_cleanup_ownership_receipt());
    }
    register_cleanup_ownership(base, descriptor.store_id, descriptor.store_id,
                               std::move(*ownership));

    CHECK(descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(descriptor.store_id != 0);
    CHECK(descriptor.count == 2);
    {
        OOCRelationReader reader(base.string(), descriptor);
        CHECK(reader.count() == descriptor.count);
        CHECK(reader.read(0).a == 11);
        CHECK(reader.read(0).b == 12);
        CHECK(reader.read(1).a == 13);
        CHECK(reader.read(1).b == 14);
    }

    const OOCCleanupRequest request{
        .base_path = base,
        .store_id = descriptor.store_id,
        .exact =
            OOCExactCleanupExpectation{
                .index_magic = OOCRelationStoreFormat::MAGIC_V3_FINAL,
                .persisted_count = descriptor.count,
                .index_size = OOCRelationWriter::index_size_for_count(descriptor.count),
                .data_size = descriptor.data_end,
            },
    };
    CHECK(begin_cleanup(request).completed());
    check_cleanup_complete(OOCCleanupTransaction::paths_for(base));
}

void test_marker_corruption_is_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x0102'0304'0506'0708ULL;

    {
        const auto base = temp.path() / "intent-corrupt";
        write_pair(base, store_id);
        StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
        CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        flip_last_byte(paths.intent_path);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
        CHECK(exists(paths.index_path));
        CHECK(exists(paths.data_path));
    }

    {
        const auto base = temp.path() / "staged-corrupt";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::DeleteAuthorizedDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        flip_last_byte(paths.staged_path);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
        CHECK(exists(paths.quarantine_index_path));
        CHECK(exists(paths.quarantine_data_path));
    }
}

void test_authorized_v2_markers_are_not_legacy_cleanup_authority() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = UINT64_C(0x0a0b0c0d0e0f1011);

    {
        const auto base = temp.path() / "v2-in-legacy-intent";
        write_pair(base, store_id);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        const auto v2_intent =
            authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2::intent);
        write_test_bytes(paths.intent_path, v2_intent);

        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
        check_test_bytes_preserved(paths.index_path, index_bytes);
        check_test_bytes_preserved(paths.data_path, data_bytes);
        check_test_bytes_preserved(paths.intent_path, v2_intent);
        CHECK(!entry_exists_no_follow(paths.staged_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
    }

    {
        const auto base = temp.path() / "v2-in-legacy-staged";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        const auto legacy_intent_bytes = read_test_bytes(paths.intent_path);
        const auto v2_staged =
            authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2::staged);
        write_test_bytes(paths.staged_path, v2_staged);

        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
        check_test_bytes_preserved(paths.index_path, index_bytes);
        check_test_bytes_preserved(paths.data_path, data_bytes);
        check_test_bytes_preserved(paths.intent_path, legacy_intent_bytes);
        check_test_bytes_preserved(paths.staged_path, v2_staged);
        CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
    }
}

void test_platform_limited_handoff_leaf_metadata_observer() {
    using gnfs::relation::ooc_cleanup_detail::observe_platform_limited_handoff_leaf;

    TempDirectory temp;
    const auto missing = temp.path() / "missing";
    CHECK(observe_platform_limited_handoff_leaf(missing) ==
          PrivateHandoffLeafObservationKind::Missing);

    const auto regular = temp.path() / "regular";
    write_test_leaf(regular, "handoff");
#ifndef _WIN32
    std::filesystem::permissions(
        regular, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
#endif
    CHECK(observe_platform_limited_handoff_leaf(regular) ==
          PrivateHandoffLeafObservationKind::Unsupported);

#ifndef _WIN32
    const auto invalid_mode = temp.path() / "invalid-mode";
    write_test_leaf(invalid_mode, "handoff");
    std::filesystem::permissions(invalid_mode, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace);
    CHECK(observe_platform_limited_handoff_leaf(invalid_mode) ==
          PrivateHandoffLeafObservationKind::Foreign);
#endif

    const auto directory = temp.path() / "directory";
    CHECK(std::filesystem::create_directory(directory));
    CHECK(observe_platform_limited_handoff_leaf(directory) ==
          PrivateHandoffLeafObservationKind::Foreign);

    const auto hard_link_source = temp.path() / "hard-link-source";
    const auto hard_link_alias = temp.path() / "hard-link-alias";
    write_test_leaf(hard_link_source, "handoff");
#ifndef _WIN32
    std::filesystem::permissions(
        hard_link_source, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
#endif
    if (create_hard_link_checked(hard_link_source, hard_link_alias)) {
        CHECK(observe_platform_limited_handoff_leaf(hard_link_source) ==
              PrivateHandoffLeafObservationKind::Foreign);
        CHECK(observe_platform_limited_handoff_leaf(hard_link_alias) ==
              PrivateHandoffLeafObservationKind::Foreign);
    }

    const auto symlink_target = temp.path() / "symlink-target";
    const auto symlink = temp.path() / "symlink";
    write_test_leaf(symlink_target, "handoff");
    if (create_symlink_or_explicit_skip(symlink_target, symlink,
                                        "platform-limited handoff symlink")) {
        CHECK(observe_platform_limited_handoff_leaf(symlink) ==
              PrivateHandoffLeafObservationKind::Foreign);
    }
}

void test_private_cleanup_union_observer_baseline_and_exact_names() {
    {
        TempDirectory temp;
        const auto base = temp.path() / "private-observer-baseline.gnfs-sink-lease" / "corpus";
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        const auto raw =
            gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(base);
        CHECK(!raw.namespace_foreign);
        CHECK(raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Intent)] ==
              PrivateCleanupMarkerObservationKind::LegacyV1);
        CHECK(raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] ==
              PrivateHandoffLeafObservationKind::Missing);
        CHECK(raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] ==
              PrivateHandoffLeafObservationKind::Missing);
        const auto classified =
            gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw);
        CHECK(classified.block == PrivateCleanupUnionBlock::None);
        CHECK(classified.has_legacy_v1);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "private-observer-wrong-case.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        auto wrong_case_name = paths.intent_path.filename().string();
        const auto changed = wrong_case_name.find_first_of("abcdefghijklmnopqrstuvwxyz");
        CHECK(changed != std::string::npos);
        if (changed == std::string::npos) {
            return;
        }
        wrong_case_name[changed] = static_cast<char>(wrong_case_name[changed] - 'a' + 'A');
        const auto wrong_case_path = paths.intent_path.parent_path() / wrong_case_name;
        const auto transfer_path = temp.path() / "saved-case-variant";
        std::error_code error;
        std::filesystem::rename(paths.intent_path, transfer_path, error);
        CHECK(!error);
        if (error) {
            return;
        }
        std::filesystem::rename(transfer_path, wrong_case_path, error);
        CHECK(!error);
        if (error) {
            return;
        }

        const auto raw =
            gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(base);
        CHECK(raw.namespace_foreign);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
        CHECK(entry_exists_no_follow(wrong_case_path));
    }
}

#if !defined(__APPLE__)
void record_unexpected_private_cleanup_union_hook(PrivateCleanupUnionObservationPoint,
                                                  void* opaque) {
    *static_cast<bool*>(opaque) = true;
}

void test_private_cleanup_union_limited_platform_rejects_hooks() {
    TempDirectory temp;
    const auto base = temp.path() / "private-observer-limited-hook.gnfs-sink-lease" / "corpus";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    reservation.ownership.reset();

    bool callback_invoked = false;
    std::optional<gnfs::relation::ooc_cleanup_detail::Failure> rejected;
    try {
        (void)gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(
            base, PrivateCleanupUnionObservationTestHooks{
                      .observe = record_unexpected_private_cleanup_union_hook,
                      .context = &callback_invoked,
                  });
    } catch (const gnfs::relation::ooc_cleanup_detail::Failure& failure) {
        rejected = failure;
    }
    CHECK(!callback_invoked);
    CHECK(rejected.has_value());
    if (rejected) {
        CHECK(rejected->status == OOCCleanupStatus::PlatformUnsupported);
    }
}
#endif

#if defined(__APPLE__)
enum class PrivateCleanupUnionMutationKind : std::uint8_t {
    ReplaceLeaf,
    ReplaceDirectory,
};

struct PrivateCleanupUnionMutationContext final {
    PrivateCleanupUnionObservationPoint target_point =
        PrivateCleanupUnionObservationPoint::InitialInventoryComplete;
    PrivateCleanupUnionMutationKind kind = PrivateCleanupUnionMutationKind::ReplaceLeaf;
    std::filesystem::path target_path;
    std::filesystem::path saved_path;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> expected_after_mutation;
    bool fired = false;
};

void mutate_private_cleanup_union_observation(PrivateCleanupUnionObservationPoint point,
                                              void* opaque) {
    auto& context = *static_cast<PrivateCleanupUnionMutationContext*>(opaque);
    if (context.fired || point != context.target_point) {
        return;
    }

    std::error_code error;
    std::filesystem::rename(context.target_path, context.saved_path, error);
    if (error) {
        throw std::filesystem::filesystem_error("save private observer target", context.target_path,
                                                context.saved_path, error);
    }

    if (context.kind == PrivateCleanupUnionMutationKind::ReplaceLeaf) {
        std::filesystem::copy_file(context.saved_path, context.target_path, error);
        if (error) {
            throw std::filesystem::filesystem_error("replace private observer leaf",
                                                    context.saved_path, context.target_path, error);
        }
        std::filesystem::permissions(context.target_path,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, error);
    } else {
        (void)std::filesystem::create_directory(context.target_path, error);
        if (!error) {
            std::filesystem::permissions(context.target_path, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
        }
    }
    if (error) {
        throw std::filesystem::filesystem_error("finish private observer replacement",
                                                context.target_path, error);
    }
    context.fired = true;
    if (!context.snapshot_root.empty()) {
        context.expected_after_mutation = capture_namespace_tree(context.snapshot_root);
    }
}

void test_private_cleanup_union_same_handle_inventory() {
    constexpr std::array observation_points{
        PrivateCleanupUnionObservationPoint::InitialInventoryComplete,
        PrivateCleanupUnionObservationPoint::LeafReadsComplete,
    };
    constexpr auto canonical_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical);
    constexpr auto pending_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending);

    for (std::size_t index = 0; index < observation_points.size(); ++index) {
        TempDirectory temp;
        const auto base =
            temp.path() /
            ("private-observer-leaf-replacement-" + std::to_string(index) + ".gnfs-sink-lease") /
            "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        PrivateCleanupUnionMutationContext context{
            .target_point = observation_points[index],
            .kind = PrivateCleanupUnionMutationKind::ReplaceLeaf,
            .target_path = paths.intent_path,
            .saved_path = temp.path() / ("saved-intent-" + std::to_string(index)),
            .snapshot_root = temp.path(),
        };
        const auto raw = gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(
            base, PrivateCleanupUnionObservationTestHooks{
                      .observe = mutate_private_cleanup_union_observation,
                      .context = &context,
                  });
        CHECK(context.fired);
        CHECK(context.expected_after_mutation.has_value());
        if (context.expected_after_mutation) {
            CHECK(capture_namespace_tree(temp.path()) == *context.expected_after_mutation);
        }
        CHECK(raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Missing);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Missing);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
        CHECK(entry_exists_no_follow(context.target_path));
        CHECK(entry_exists_no_follow(context.saved_path));
        CHECK(read_test_bytes(context.target_path) == read_test_bytes(context.saved_path));
        std::error_code equivalent_error;
        CHECK(!std::filesystem::equivalent(context.target_path, context.saved_path,
                                           equivalent_error));
        CHECK(!equivalent_error);
    }

    for (std::size_t index = 0; index < observation_points.size(); ++index) {
        TempDirectory temp;
        const auto base =
            temp.path() /
            ("private-observer-pending-replacement-" + std::to_string(index) + ".gnfs-sink-lease") /
            "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        {
            auto prepared = prepare_private_handoff(base);
            PrivateHandoffStopContext stop{
                .target = OOCPrivateHandoffFaultPoint::PendingDurable,
            };
            const auto interrupted =
                publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
            CHECK(stop.stopped);
            CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
            CHECK(interrupted.state == OOCPrivateHandoffState::PendingOnly);
        }

        PrivateCleanupUnionMutationContext context{
            .target_point = observation_points[index],
            .kind = PrivateCleanupUnionMutationKind::ReplaceLeaf,
            .target_path = paths.private_handoff_pending_path,
            .saved_path = temp.path() / ("saved-pending-" + std::to_string(index)),
            .snapshot_root = temp.path(),
        };
        const auto raw = gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(
            base, PrivateCleanupUnionObservationTestHooks{
                      .observe = mutate_private_cleanup_union_observation,
                      .context = &context,
                  });
        CHECK(context.fired);
        CHECK(context.expected_after_mutation.has_value());
        if (context.expected_after_mutation) {
            CHECK(capture_namespace_tree(temp.path()) == *context.expected_after_mutation);
        }
        CHECK(raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Missing);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Exact);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
        CHECK(entry_exists_no_follow(context.target_path));
        CHECK(entry_exists_no_follow(context.saved_path));
        CHECK(read_test_bytes(context.target_path) == read_test_bytes(context.saved_path));
        std::error_code equivalent_error;
        CHECK(!std::filesystem::equivalent(context.target_path, context.saved_path,
                                           equivalent_error));
        CHECK(!equivalent_error);
    }

    for (std::size_t index = 0; index < observation_points.size(); ++index) {
        TempDirectory temp;
        const auto base =
            temp.path() /
            ("private-observer-handoff-replacement-" + std::to_string(index) + ".gnfs-sink-lease") /
            "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());

        PrivateCleanupUnionMutationContext context{
            .target_point = observation_points[index],
            .kind = PrivateCleanupUnionMutationKind::ReplaceLeaf,
            .target_path = paths.private_handoff_path,
            .saved_path = temp.path() / ("saved-handoff-" + std::to_string(index)),
            .snapshot_root = temp.path(),
        };
        const auto raw = gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(
            base, PrivateCleanupUnionObservationTestHooks{
                      .observe = mutate_private_cleanup_union_observation,
                      .context = &context,
                  });
        CHECK(context.fired);
        CHECK(context.expected_after_mutation.has_value());
        if (context.expected_after_mutation) {
            CHECK(capture_namespace_tree(temp.path()) == *context.expected_after_mutation);
        }
        CHECK(raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Exact);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Missing);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
        CHECK(entry_exists_no_follow(context.target_path));
        CHECK(entry_exists_no_follow(context.saved_path));
        CHECK(read_test_bytes(context.target_path) == read_test_bytes(context.saved_path));
        std::error_code equivalent_error;
        CHECK(!std::filesystem::equivalent(context.target_path, context.saved_path,
                                           equivalent_error));
        CHECK(!equivalent_error);
    }

    for (std::size_t index = 0; index < observation_points.size(); ++index) {
        TempDirectory temp;
        const auto base = temp.path() /
                          ("private-observer-directory-replacement-" + std::to_string(index) +
                           ".gnfs-sink-lease") /
                          "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        PrivateCleanupUnionMutationContext context{
            .target_point = observation_points[index],
            .kind = PrivateCleanupUnionMutationKind::ReplaceDirectory,
            .target_path = paths.private_directory,
            .saved_path = temp.path() / ("saved-private-directory-" + std::to_string(index)),
            .snapshot_root = temp.path(),
        };
        std::optional<gnfs::relation::ooc_cleanup_detail::Failure> rejected;
        try {
            (void)gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(
                base, PrivateCleanupUnionObservationTestHooks{
                          .observe = mutate_private_cleanup_union_observation,
                          .context = &context,
                      });
        } catch (const gnfs::relation::ooc_cleanup_detail::Failure& failure) {
            rejected = failure;
        }
        CHECK(context.fired);
        CHECK(context.expected_after_mutation.has_value());
        if (context.expected_after_mutation) {
            CHECK(capture_namespace_tree(temp.path()) == *context.expected_after_mutation);
        }
        CHECK(rejected.has_value());
        if (rejected) {
            CHECK(rejected->status == OOCCleanupStatus::NamespaceConflict);
        }
        CHECK(entry_exists_no_follow(context.target_path));
        CHECK(entry_exists_no_follow(context.saved_path));
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "private-observer-hard-link.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();
        if (create_hard_link_checked(paths.intent_path, temp.path() / "intent-hard-link")) {
            const auto raw =
                gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(base);
            CHECK(!raw.namespace_foreign);
            CHECK(raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Intent)] ==
                  PrivateCleanupMarkerObservationKind::Malformed);
            CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
                  PrivateCleanupUnionBlock::MarkerCorrupt);
        }
    }
}

void test_private_cleanup_union_handoff_slots_are_independent() {
    const auto observe_preserving = [](const std::filesystem::path& base,
                                       const std::filesystem::path& root) {
        const auto before = capture_namespace_tree(root);
        const auto raw =
            gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(base);
        CHECK(capture_namespace_tree(root) == before);
        return raw;
    };
    const auto canonical_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical);
    const auto pending_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending);

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-duplicate-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        write_private_control_bytes(paths.private_handoff_pending_path,
                                    read_test_bytes(paths.private_handoff_path));

        const auto raw = observe_preserving(base, temp.path());
        CHECK(!raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Exact);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Exact);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::None);
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-corrupt-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        constexpr std::array corrupt{
            std::byte{0xde},
            std::byte{0xad},
            std::byte{0xbe},
            std::byte{0xef},
        };
        write_private_control_bytes(paths.private_handoff_pending_path, corrupt);

        const auto raw = observe_preserving(base, temp.path());
        CHECK(!raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Exact);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Foreign);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-corrupt-canonical.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        {
            auto prepared = prepare_private_handoff(base);
            PrivateHandoffStopContext stop{
                .target = OOCPrivateHandoffFaultPoint::PendingDurable,
            };
            const auto interrupted =
                publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
            CHECK(stop.stopped);
            CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
            CHECK(interrupted.state == OOCPrivateHandoffState::PendingOnly);
        }
        write_private_control_bytes(paths.private_handoff_path,
                                    read_test_bytes(paths.private_handoff_pending_path));
        flip_last_byte(paths.private_handoff_path);

        const auto raw = observe_preserving(base, temp.path());
        CHECK(!raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Foreign);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Exact);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-foreign-canonical.gnfs-sink-lease" / "corpus";
        const auto foreign_base =
            temp.path() / "private-observer-foreign-canonical-source.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign_paths = OOCCleanupTransaction::paths_for(foreign_base);
        {
            auto prepared = prepare_private_handoff(base);
            PrivateHandoffStopContext stop{
                .target = OOCPrivateHandoffFaultPoint::PendingDurable,
            };
            const auto interrupted =
                publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
            CHECK(stop.stopped);
            CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
            CHECK(interrupted.state == OOCPrivateHandoffState::PendingOnly);
        }
        auto foreign_prepared = prepare_private_handoff(foreign_base);
        CHECK(publish_private_handoff(foreign_prepared).canonical());
        write_private_control_bytes(paths.private_handoff_path,
                                    read_test_bytes(foreign_paths.private_handoff_path));

        const auto raw = observe_preserving(base, temp.path());
        CHECK(!raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Foreign);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Exact);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-unreserved-foreign-pair.gnfs-sink-lease" / "corpus";
        const auto foreign_base =
            temp.path() / "private-observer-unreserved-foreign-source.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign_paths = OOCCleanupTransaction::paths_for(foreign_base);
        auto prepared = prepare_private_handoff(base);
        auto foreign_prepared = prepare_private_handoff(foreign_base);
        CHECK(publish_private_handoff(prepared).canonical());
        CHECK(publish_private_handoff(foreign_prepared).canonical());
        const auto foreign_bytes = read_test_bytes(foreign_paths.private_handoff_path);
        write_private_control_bytes(paths.private_handoff_path, foreign_bytes);
        write_private_control_bytes(paths.private_handoff_pending_path, foreign_bytes);
        CHECK(!entry_exists_no_follow(paths.lease_reserved_path));

        const auto raw = observe_preserving(base, temp.path());
        CHECK(!raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Foreign);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Foreign);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-hardlink-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        write_private_control_bytes(paths.private_handoff_pending_path,
                                    read_test_bytes(paths.private_handoff_path));
        if (create_hard_link_checked(paths.private_handoff_pending_path,
                                     temp.path() / "pending-hard-link")) {
            const auto raw = observe_preserving(base, temp.path());
            CHECK(!raw.namespace_foreign);
            CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Exact);
            CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Foreign);
            CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
                  PrivateCleanupUnionBlock::Foreign);
        }
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-foreign-first-context.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        write_private_control_bytes(paths.private_handoff_pending_path,
                                    read_test_bytes(paths.private_handoff_path));
        if (create_hard_link_checked(paths.private_handoff_pending_path,
                                     temp.path() / "foreign-first-pending-hard-link")) {
            flip_last_byte(paths.lease_owned_path);
            const auto raw = observe_preserving(base, temp.path());
            CHECK(!raw.namespace_foreign);
            CHECK(raw.handoff_markers[canonical_slot] ==
                  PrivateHandoffLeafObservationKind::Foreign);
            CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Foreign);
            CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
                  PrivateCleanupUnionBlock::Foreign);
        }
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-inventory-first-context.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        flip_last_byte(paths.lease_owned_path);

        PrivateCleanupUnionMutationContext context{
            .target_point = PrivateCleanupUnionObservationPoint::InitialInventoryComplete,
            .kind = PrivateCleanupUnionMutationKind::ReplaceLeaf,
            .target_path = paths.private_handoff_path,
            .saved_path = temp.path() / "saved-inventory-first-handoff",
            .snapshot_root = temp.path(),
        };
        const auto raw = gnfs::relation::ooc_cleanup_detail::observe_private_cleanup_union_for_test(
            base, PrivateCleanupUnionObservationTestHooks{
                      .observe = mutate_private_cleanup_union_observation,
                      .context = &context,
                  });
        CHECK(context.fired);
        CHECK(context.expected_after_mutation.has_value());
        if (context.expected_after_mutation) {
            CHECK(capture_namespace_tree(temp.path()) == *context.expected_after_mutation);
        }
        CHECK(raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Foreign);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Missing);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-hardlink-canonical.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        {
            auto prepared = prepare_private_handoff(base);
            PrivateHandoffStopContext stop{
                .target = OOCPrivateHandoffFaultPoint::PendingDurable,
            };
            const auto interrupted =
                publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
            CHECK(stop.stopped);
            CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
            CHECK(interrupted.state == OOCPrivateHandoffState::PendingOnly);
        }
        write_private_control_bytes(paths.private_handoff_path,
                                    read_test_bytes(paths.private_handoff_pending_path));
        if (create_hard_link_checked(paths.private_handoff_path,
                                     temp.path() / "canonical-hard-link")) {
            const auto raw = observe_preserving(base, temp.path());
            CHECK(!raw.namespace_foreign);
            CHECK(raw.handoff_markers[canonical_slot] ==
                  PrivateHandoffLeafObservationKind::Foreign);
            CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Exact);
            CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
                  PrivateCleanupUnionBlock::Foreign);
        }
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-conflicting-pending.gnfs-sink-lease" / "corpus";
        const auto foreign_base =
            temp.path() / "private-observer-conflicting-source.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign_paths = OOCCleanupTransaction::paths_for(foreign_base);
        auto prepared = prepare_private_handoff(base);
        auto foreign_prepared = prepare_private_handoff(foreign_base);
        CHECK(publish_private_handoff(prepared).canonical());
        CHECK(publish_private_handoff(foreign_prepared).canonical());
        write_private_control_bytes(paths.private_handoff_pending_path,
                                    read_test_bytes(foreign_paths.private_handoff_path));

        const auto raw = observe_preserving(base, temp.path());
        CHECK(!raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Exact);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Malformed);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-observer-conflict-first-context.gnfs-sink-lease" / "corpus";
        const auto foreign_base =
            temp.path() / "private-observer-conflict-first-source.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign_paths = OOCCleanupTransaction::paths_for(foreign_base);
        auto prepared = prepare_private_handoff(base);
        auto foreign_prepared = prepare_private_handoff(foreign_base);
        CHECK(publish_private_handoff(prepared).canonical());
        CHECK(publish_private_handoff(foreign_prepared).canonical());
        write_private_control_bytes(paths.private_handoff_pending_path,
                                    read_test_bytes(foreign_paths.private_handoff_path));
        flip_last_byte(paths.lease_owned_path);

        const auto raw = observe_preserving(base, temp.path());
        CHECK(!raw.namespace_foreign);
        CHECK(raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Foreign);
        CHECK(raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Malformed);
        CHECK(gnfs::relation::ooc_cleanup_detail::classify_private_cleanup_union(raw).block ==
              PrivateCleanupUnionBlock::Foreign);
    }
}
#endif

void test_private_authority_union_preflight_is_zero_mutation() {
    const auto v2_intent =
        authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2::intent);
    const auto v2_staged =
        authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2::staged);
    const auto unsupported = std::make_error_code(std::errc::operation_not_supported);

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-v2-cleanup-handoff-preflight.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());

        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        (void)writer.write(make_real_relation(11, 13));
        write_private_control_bytes(paths.staged_pending_path, v2_staged);
        const auto before = capture_namespace_tree(temp.path());

        std::error_code rejected_error;
        try {
            (void)writer.finalize_and_publish_cleanup_handoff();
        } catch (const std::system_error& error) {
            rejected_error = error.code();
        }
        CHECK(rejected_error == unsupported);
        CHECK(writer.has_cleanup_ownership_receipt());
        CHECK(capture_namespace_tree(temp.path()) == before);
        writer.abort();
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-v2-staged-pending-resume.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        write_private_control_bytes(paths.staged_pending_path, v2_staged);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(rejected.native_error == unsupported);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "private-v2-intent-pending-tail.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        StopContext stop{.target = OOCCleanupFaultPoint::IntentRemovedDurable};
        CHECK(OOCCleanupTransaction::resume(base, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.staged_path));
        write_private_control_bytes(paths.intent_pending_path, v2_intent);

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(rejected.native_error == unsupported);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "private-v2-remove.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        write_private_control_bytes(paths.lease_reserved_pending_path,
                                    read_test_bytes(paths.lease_reserved_path));
        write_private_control_bytes(paths.lease_owned_pending_path,
                                    read_test_bytes(paths.lease_owned_path));
        write_private_control_bytes(paths.staged_pending_path, v2_staged);

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
        CHECK(rejected.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(rejected.native_error == unsupported);
        CHECK(!reservation.ownership->spent());
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "private-v2-recover.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        write_private_control_bytes(paths.lease_reserved_pending_path,
                                    read_test_bytes(paths.lease_reserved_path));
        write_private_control_bytes(paths.lease_owned_pending_path,
                                    read_test_bytes(paths.lease_owned_path));
        write_private_control_bytes(paths.staged_pending_path, v2_staged);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(rejected.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(rejected.native_error == unsupported);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "private-v2-reserve.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto original = prepare_private_legacy_cleanup_intent(base);
        write_private_control_bytes(paths.staged_pending_path, v2_staged);
        original.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(rejected.result.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.result.stage == OOCCleanupStage::None);
        CHECK(rejected.result.native_error == unsupported);
        CHECK(!rejected.ownership.has_value());
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "private-v2-canonical-intent.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        write_private_control_bytes(paths.intent_path, v2_intent);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(rejected.native_error == unsupported);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "private-v2-wrong-role.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        write_private_control_bytes(paths.intent_pending_path, v2_staged);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::IntentCorrupt);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "private-marker-corrupt-before-v2.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        flip_last_byte(paths.intent_path);
        write_private_control_bytes(paths.staged_pending_path, v2_staged);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::IntentCorrupt);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "private-v2-crash-prefix.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        const std::array crash_prefix{static_cast<std::byte>('G')};
        write_private_control_bytes(paths.staged_pending_path, crash_prefix);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::IntentCorrupt);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    for (const bool empty : {false, true}) {
        TempDirectory temp;
        const auto base = temp.path() /
                          (std::string(empty ? "private-empty-pending" : "private-opaque-pending") +
                           ".gnfs-sink-lease") /
                          "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        const std::array opaque{static_cast<std::byte>('X')};
        const std::span<const std::byte> bytes =
            empty ? std::span<const std::byte>{}
                  : std::span<const std::byte>{opaque.data(), opaque.size()};
        write_private_control_bytes(paths.intent_pending_path, bytes);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto delegated = OOCCleanupTransaction::resume(base);
        CHECK(delegated.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(delegated.stage == OOCCleanupStage::IntentDurable);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }
}

void test_absence_before_staged_has_no_delete_authority() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1111'2222'3333'4444ULL;
    const auto base = temp.path() / "unauthorized-absence";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_data = temp.path() / "saved-data";
    std::filesystem::rename(paths.data_path, saved_data);

    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NamespaceConflict);
    CHECK(exists(paths.intent_path));
    CHECK(!exists(paths.staged_path));
    CHECK(exists(paths.quarantine_index_path));
    CHECK(exists(saved_data));
}

void test_reverse_pre_staged_state_is_rejected() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1212'2323'3434'4545ULL;
    const auto base = temp.path() / "reverse-pre-staged";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);

    const auto paths = OOCCleanupTransaction::paths_for(base);
    std::filesystem::rename(paths.data_path, paths.quarantine_data_path);
    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NamespaceConflict);
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.data_path));
    CHECK(entry_exists_no_follow(paths.quarantine_data_path));
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
}

void test_source_link_attacks_are_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1313'2424'3535'4646ULL;

    {
        const auto base = temp.path() / "source-symlink";
        write_pair(base, store_id);
        auto ownership = capture_cleanup_ownership(base, store_id);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved_index = temp.path() / "source-symlink-owned-index";
        std::filesystem::rename(paths.index_path, saved_index);
        if (create_symlink_or_explicit_skip(saved_index, paths.index_path, "source symlink")) {
            CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
                  OOCCleanupStatus::SourcePairInvalid);
            CHECK(entry_is_symlink_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(saved_index));
            CHECK(entry_exists_no_follow(paths.data_path));
        }
    }

    {
        const auto base = temp.path() / "source-hardlink";
        write_pair(base, store_id + 1);
        auto ownership = capture_cleanup_ownership(base, store_id + 1);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto alias = temp.path() / "source-hardlink-alias";
        if (create_hard_link_checked(paths.index_path, alias)) {
            CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
                  OOCCleanupStatus::SourcePairInvalid);
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(alias));
            CHECK(entry_exists_no_follow(paths.data_path));
            check_entries_equivalent(paths.index_path, alias);
        }
    }
}

void test_quarantine_link_attacks_are_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1414'2525'3636'4747ULL;

    {
        const auto base = temp.path() / "quarantine-symlink";
        write_pair(base, store_id);
        StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
        CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved_index = temp.path() / "quarantine-symlink-owned-index";
        std::filesystem::rename(paths.quarantine_index_path, saved_index);
        if (create_symlink_or_explicit_skip(saved_index, paths.quarantine_index_path,
                                            "quarantine symlink")) {
            CHECK(OOCCleanupTransaction::resume(base).status ==
                  OOCCleanupStatus::ForeignReplacementPreserved);
            CHECK(entry_is_symlink_no_follow(paths.quarantine_index_path));
            CHECK(entry_exists_no_follow(saved_index));
            CHECK(entry_exists_no_follow(paths.data_path));
            CHECK(entry_exists_no_follow(paths.intent_path));
        }
    }

    {
        const auto base = temp.path() / "quarantine-hardlink";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto alias = temp.path() / "quarantine-hardlink-alias";
        if (create_hard_link_checked(paths.quarantine_index_path, alias)) {
            CHECK(OOCCleanupTransaction::resume(base).status ==
                  OOCCleanupStatus::ForeignReplacementPreserved);
            CHECK(entry_exists_no_follow(paths.quarantine_index_path));
            CHECK(entry_exists_no_follow(alias));
            CHECK(entry_exists_no_follow(paths.data_path));
            CHECK(entry_exists_no_follow(paths.intent_path));
            check_entries_equivalent(paths.quarantine_index_path, alias);
        }
    }
}

void test_intent_link_attacks_are_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1515'2626'3737'4848ULL;

    {
        const auto base = temp.path() / "intent-symlink";
        write_pair(base, store_id);
        StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
        CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved_intent = temp.path() / "intent-symlink-owned-marker";
        std::filesystem::rename(paths.intent_path, saved_intent);
        if (create_symlink_or_explicit_skip(saved_intent, paths.intent_path, "intent symlink")) {
            CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
            CHECK(entry_is_symlink_no_follow(paths.intent_path));
            CHECK(entry_exists_no_follow(saved_intent));
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
        }
    }

    {
        const auto base = temp.path() / "intent-hardlink";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto alias = temp.path() / "intent-hardlink-alias";
        if (create_hard_link_checked(paths.intent_path, alias)) {
            CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
            CHECK(entry_exists_no_follow(paths.intent_path));
            CHECK(entry_exists_no_follow(alias));
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
            check_entries_equivalent(paths.intent_path, alias);
        }
    }
}

void test_lock_link_attacks_are_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1616'2727'3838'4949ULL;

    {
        const auto base = temp.path() / "lock-symlink";
        write_pair(base, store_id);
        auto ownership = capture_cleanup_ownership(base, store_id);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        write_test_leaf(paths.lock_path, "owned cleanup lock");
        const auto saved_lock = temp.path() / "lock-symlink-owned-lock";
        std::filesystem::rename(paths.lock_path, saved_lock);
        const auto target = temp.path() / "lock-symlink-target";
        write_test_leaf(target, "foreign lock target");
        if (create_symlink_or_explicit_skip(target, paths.lock_path, "lock symlink")) {
            CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
                  OOCCleanupStatus::NamespaceConflict);
            CHECK(entry_is_symlink_no_follow(paths.lock_path));
            CHECK(entry_exists_no_follow(saved_lock));
            CHECK(entry_exists_no_follow(target));
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
        }
    }

    {
        const auto base = temp.path() / "lock-hardlink";
        write_pair(base, store_id + 1);
        auto ownership = capture_cleanup_ownership(base, store_id + 1);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        write_test_leaf(paths.lock_path, "owned cleanup lock");
        const auto alias = temp.path() / "lock-hardlink-alias";
        if (create_hard_link_checked(paths.lock_path, alias)) {
            CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
                  OOCCleanupStatus::NamespaceConflict);
            CHECK(entry_exists_no_follow(paths.lock_path));
            CHECK(entry_exists_no_follow(alias));
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
            check_entries_equivalent(paths.lock_path, alias);
        }
    }
}

void test_foreign_replacements_are_preserved() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x2222'3333'4444'5555ULL;

    {
        const auto base = temp.path() / "foreign-original-index";
        write_pair(base, store_id);
        StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
        CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        write_index(paths.index_path, OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE, store_id + 100,
                    0, OOCRelationStoreFormat::INDEX_HEADER_BYTES);
        CHECK(OOCCleanupTransaction::resume(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(exists(paths.index_path));
        CHECK(exists(paths.quarantine_index_path));
        CHECK(exists(paths.data_path));
    }

    {
        const auto base = temp.path() / "foreign-data-same-header";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved = temp.path() / "saved-owned-data";
        std::filesystem::rename(paths.data_path, saved);
        write_data(paths.data_path, actual_store_id_for(base, store_id + 1),
                   OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
        CHECK(OOCCleanupTransaction::resume(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(exists(paths.data_path));
        CHECK(exists(saved));
        CHECK(exists(paths.quarantine_index_path));
    }

    {
        const auto base = temp.path() / "foreign-quarantine-data";
        write_pair(base, store_id + 2);
        StopContext stop{.target = OOCCleanupFaultPoint::DeleteAuthorizedDurable};
        CHECK(begin_cleanup(base, store_id + 2, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved = temp.path() / "saved-quarantine-data";
        std::filesystem::rename(paths.quarantine_data_path, saved);
        write_data(paths.quarantine_data_path, actual_store_id_for(base, store_id + 2),
                   OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
        CHECK(OOCCleanupTransaction::resume(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(exists(paths.quarantine_data_path));
        CHECK(exists(saved));
        CHECK(exists(paths.quarantine_index_path));
    }
}

void test_index_count_drift_is_preserved() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x3333'4444'5555'6666ULL;
    const auto base = temp.path() / "count-drift";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    overwrite_count(paths.index_path, 1);
    CHECK(OOCCleanupTransaction::resume(base).status ==
          OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(exists(paths.index_path));
    CHECK(exists(paths.data_path));
    CHECK(exists(paths.intent_path));
}

void test_staged_only_tail_has_no_delete_authority() {
    TempDirectory temp;
    constexpr std::uint64_t old_store_id = 0x4444'5555'6666'7777ULL;
    constexpr std::uint64_t new_store_id = 0x5555'6666'7777'8888ULL;
    const auto base = temp.path() / "staged-only";
    write_pair(base, old_store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::SecondUnlinkDurable};
    CHECK(begin_cleanup(base, old_store_id, stop_hooks(stop)).status ==
          OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    CHECK(exists(paths.intent_path));
    CHECK(exists(paths.staged_path));
    std::filesystem::remove(paths.intent_path);

    // A cooperating fresh writer must reject a staged-only namespace. Model an
    // external/noncooperating replacement directly to verify that staged alone
    // still cannot authorize deletion of the live pair.
    write_index(paths.index_path, OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE, new_store_id, 0,
                OOCRelationStoreFormat::INDEX_HEADER_BYTES);
    write_data(paths.data_path, new_store_id, OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
    CHECK(OOCCleanupTransaction::resume(base).completed());
    CHECK(exists(paths.index_path));
    CHECK(exists(paths.data_path));
    CHECK(!exists(paths.staged_path));
}

void test_quarantine_collision_is_preserved() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x6666'7777'8888'9999ULL;
    const auto base = temp.path() / "quarantine-collision";
    write_pair(base, store_id);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    write_index(paths.quarantine_index_path, OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE,
                store_id + 1, 0, OOCRelationStoreFormat::INDEX_HEADER_BYTES);
    CHECK(begin_cleanup(base, store_id).status == OOCCleanupStatus::NamespaceConflict);
    CHECK(exists(paths.index_path));
    CHECK(exists(paths.data_path));
    CHECK(exists(paths.quarantine_index_path));
}

class HeldBaseLock final {
public:
    explicit HeldBaseLock(const std::filesystem::path& path) {
#ifdef _WIN32
        handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("could not hold Win32 cleanup lock");
        }
#else
        descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            if (descriptor_ >= 0) {
                (void)::close(descriptor_);
            }
            throw std::runtime_error("could not hold POSIX cleanup lock");
        }
#endif
    }

    HeldBaseLock(const HeldBaseLock&) = delete;
    HeldBaseLock& operator=(const HeldBaseLock&) = delete;

    ~HeldBaseLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

constexpr int LOCK_CONTENDER_BUSY_EXIT = 91;
constexpr int LOCK_HOLDER_CONFIRMED_EXIT = 92;

int run_lock_contender_child(const std::filesystem::path& base, std::uint64_t store_id) {
    (void)store_id;
    const auto result = OOCCleanupTransaction::resume(base);
    return result.status == OOCCleanupStatus::Busy ? LOCK_CONTENDER_BUSY_EXIT : 66;
}

int run_lock_holder_child(const std::string& executable, const std::filesystem::path& base,
                          std::uint64_t store_id) {
    const auto paths = OOCCleanupTransaction::paths_for(base);
    HeldBaseLock held(paths.lock_path);
    const auto contender = gnfs::test::run_child_process(
        executable, {"--contend-cleanup-lock", base.string(), std::to_string(store_id)});
    if (!contender.exited || contender.signaled ||
        contender.exit_code != LOCK_CONTENDER_BUSY_EXIT) {
        return 67;
    }
    return LOCK_HOLDER_CONFIRMED_EXIT;
}

void test_cross_process_lock_reports_busy(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x7777'8888'9999'aaaaULL;
    const auto base = temp.path() / "busy";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto holder = gnfs::test::run_child_process(
        executable, {"--hold-cleanup-lock", base.string(), std::to_string(store_id)});
    CHECK(holder.exited);
    CHECK(!holder.signaled);
    CHECK(holder.exit_code == LOCK_HOLDER_CONFIRMED_EXIT);
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(OOCCleanupTransaction::resume(base).completed());
    check_cleanup_complete(paths);
}

void test_private_lease_uses_one_persistent_external_lock(const std::string& executable) {
    TempDirectory temp;
    const auto lease = temp.path() / "private.gnfs-sink-lease";
    const auto base = lease / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto frozen_temp = std::filesystem::weakly_canonical(temp.path());
    CHECK(paths.private_directory == frozen_temp / "private.gnfs-sink-lease");
    CHECK(paths.lock_path.parent_path() == frozen_temp);
    CHECK(paths.lock_path.parent_path() != paths.private_directory);

    auto first = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(first.completed());
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));

    constexpr std::uint64_t unused_store_id = 0x8899'aabb'ccdd'eeffULL;
    const auto contender = gnfs::test::run_child_process(
        executable, {"--contend-cleanup-lock", base.string(), std::to_string(unused_store_id)});
    CHECK(contender.exited);
    CHECK(!contender.signaled);
    CHECK(contender.exit_code == LOCK_CONTENDER_BUSY_EXIT);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));

    CHECK(OOCCleanupTransaction::remove_private_lease(*first.ownership).completed());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));

    auto second = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(second.completed());
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));
    CHECK(OOCCleanupTransaction::remove_private_lease(*second.ownership).completed());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));
}

void test_private_lease_receipt_rejects_replacement_directory() {
    TempDirectory temp;
    const auto lease = temp.path() / "aba.gnfs-sink-lease";
    const auto base = lease / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto replacement = temp.path() / "replacement-lease";
    const auto saved_owned = temp.path() / "saved-owned-lease";

    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    CHECK(std::filesystem::create_directory(replacement));

    std::error_code error;
    std::filesystem::rename(paths.private_directory, saved_owned, error);
    CHECK(!error);
    error.clear();
    std::filesystem::rename(replacement, paths.private_directory, error);
    CHECK(!error);

    const auto rejected = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
    CHECK(rejected.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));

    error.clear();
    CHECK(std::filesystem::remove(paths.private_directory, error));
    CHECK(!error);
    error.clear();
    std::filesystem::rename(saved_owned, paths.private_directory, error);
    CHECK(!error);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    CHECK(reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));
}

constexpr std::array PRIVATE_LEASE_REMOVE_FAULT_POINTS{
    OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
    OOCPrivateLeaseFaultPoint::OwnerRemovedDurable,
    OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable,
    OOCPrivateLeaseFaultPoint::OwnedRemovedDurable,
};

constexpr std::array PRIVATE_WRITER_FAULT_POINTS{
    OOCPrivateLeaseFaultPoint::FreshWriterPermitAcquired,
    OOCPrivateLeaseFaultPoint::FreshIndexReserved,
    OOCPrivateLeaseFaultPoint::FreshDataReserved,
    OOCPrivateLeaseFaultPoint::FreshHeadersValidated,
    OOCPrivateLeaseFaultPoint::FreshPairOwnershipCaptured,
    OOCPrivateLeaseFaultPoint::ActivationPermitAcquired,
    OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
};

constexpr std::array PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS{
    OOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable,
    OOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable,
    OOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable,
    OOCPrivateLeaseFaultPoint::OwnerRemovedDurable,
    OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable,
    OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
    OOCPrivateLeaseFaultPoint::OwnedRemovedDurable,
};

constexpr int PRIVATE_LEASE_CRASH_EXIT_BASE = 140;

struct PrivateLeaseCrashContext final {
    OOCPrivateLeaseFaultPoint target = OOCPrivateLeaseFaultPoint::ReservedPendingDurable;
};

[[nodiscard]] bool crash_private_lease_at(OOCPrivateLeaseFaultPoint point, void* opaque) noexcept {
    const auto& context = *static_cast<const PrivateLeaseCrashContext*>(opaque);
    if (point == context.target) {
        std::_Exit(PRIVATE_LEASE_CRASH_EXIT_BASE + static_cast<int>(point));
    }
    return false;
}

[[nodiscard]] OOCPrivateLeaseTestHooks
private_lease_crash_hooks(PrivateLeaseCrashContext& context) noexcept {
    return OOCPrivateLeaseTestHooks{
        .stop_after = crash_private_lease_at,
        .context = &context,
    };
}

int run_private_lease_crash_child(std::string_view operation, std::size_t point_index,
                                  const std::filesystem::path& base) {
    if (operation == "reserve") {
        if (point_index >= PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS.size()) {
            return 64;
        }
        PrivateLeaseCrashContext context{
            .target = PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS[point_index].fault_point,
        };
        const auto result =
            OOCCleanupTransaction::reserve_private_lease(base, private_lease_crash_hooks(context));
        (void)result;
        return 65;
    }
    if (operation == "remove") {
        if (point_index >= PRIVATE_LEASE_REMOVE_FAULT_POINTS.size()) {
            return 64;
        }
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        if (!reservation.completed()) {
            return 66;
        }
        PrivateLeaseCrashContext context{
            .target = PRIVATE_LEASE_REMOVE_FAULT_POINTS[point_index],
        };
        const auto result = OOCCleanupTransaction::remove_private_lease(
            *reservation.ownership, private_lease_crash_hooks(context));
        (void)result;
        return 67;
    }
    if (operation == "preactive-recover") {
        if (point_index >= PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS.size()) {
            return 64;
        }
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            if (!reservation.completed()) {
                return 68;
            }
            const auto paths = OOCCleanupTransaction::paths_for(base);
            write_test_leaf(paths.index_path, "partial preactive index");
            write_test_leaf(paths.data_path, "partial preactive data");
        }
        PrivateLeaseCrashContext context{
            .target = PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS[point_index],
        };
        const auto result =
            OOCCleanupTransaction::recover_private_lease(base, private_lease_crash_hooks(context));
        (void)result;
        return 69;
    }
    return 64;
}

int run_private_writer_crash_child(std::size_t point_index, const std::filesystem::path& base) {
    if (point_index >= PRIVATE_WRITER_FAULT_POINTS.size()) {
        return 64;
    }
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    if (!reservation.completed()) {
        return 70;
    }
    PrivateLeaseCrashContext context{
        .target = PRIVATE_WRITER_FAULT_POINTS[point_index],
    };
    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             private_lease_crash_hooks(context));
    (void)writer;
    return 71;
}

constexpr int PRIVATE_LEASE_ABANDONED_EXIT = 93;

int run_private_lease_abandon_child(std::string_view scenario, const std::filesystem::path& base,
                                    std::uint64_t store_id) {
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    if (!reservation.completed() || store_id == 0) {
        return 66;
    }
    write_pair(base, store_id, *reservation.ownership);
    if (scenario == "live-pair") {
        return PRIVATE_LEASE_ABANDONED_EXIT;
    }
    if (scenario == "pending-only") {
        PublishStopContext stop{
            .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
        };
        const auto interrupted = begin_cleanup(base, store_id, publish_stop_hooks(stop));
        return interrupted.status == OOCCleanupStatus::Interrupted ? PRIVATE_LEASE_ABANDONED_EXIT
                                                                   : 67;
    }
    if (scenario == "canonical-intent") {
        StopContext stop{
            .target = OOCCleanupFaultPoint::IntentDurable,
        };
        const auto interrupted = begin_cleanup(base, store_id, stop_hooks(stop));
        return interrupted.status == OOCCleanupStatus::Interrupted ? PRIVATE_LEASE_ABANDONED_EXIT
                                                                   : 68;
    }
    return 64;
}

[[nodiscard]] std::vector<std::filesystem::path>
private_lease_staging_entries(const gnfs::relation::OOCCleanupPaths& paths) {
    std::vector<std::filesystem::path> entries;
    const auto prefix =
        paths.private_directory.filename().generic_string() + ".gnfs-private-lease-v1.stage-";
    std::error_code error;
    std::filesystem::directory_iterator cursor(paths.private_directory.parent_path(), error);
    if (error) {
        throw std::filesystem::filesystem_error("inspect private lease staging entries",
                                                paths.private_directory.parent_path(), error);
    }
    for (const auto& entry : cursor) {
        if (entry.path().filename().generic_string().starts_with(prefix)) {
            entries.push_back(entry.path());
        }
    }
    return entries;
}

void check_private_lease_reservation_crash_prefix(const std::filesystem::path& base,
                                                  const ReservationBoundaryContract& contract) {
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto& expected = contract.prefix;

    CHECK(entry_exists_no_follow(paths.lock_path));
    CHECK(std::filesystem::is_regular_file(symlink_status_no_follow(paths.lock_path)));
    CHECK(entry_exists_no_follow(paths.lease_reserved_pending_path) == expected.reserved_pending);
    CHECK(entry_exists_no_follow(paths.lease_reserved_path) == expected.reserved);
    CHECK(entry_exists_no_follow(paths.lease_owned_pending_path) == expected.owned_pending);
    CHECK(entry_exists_no_follow(paths.lease_owned_path) == expected.owned);
    CHECK(entry_exists_no_follow(paths.private_directory) == expected.final_directory);

    const auto staging = private_lease_staging_entries(paths);
    CHECK(staging.size() == (expected.staging_directory ? 1U : 0U));

    std::optional<std::filesystem::path> protocol_directory;
    if (expected.staging_directory && staging.size() == 1) {
        protocol_directory = staging.front();
    } else if (expected.final_directory && entry_exists_no_follow(paths.private_directory)) {
        protocol_directory = paths.private_directory;
    }
    if (protocol_directory) {
        CHECK(std::filesystem::is_directory(symlink_status_no_follow(*protocol_directory)));
        const auto owner_path =
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(*protocol_directory);
        const auto owner_pending_path =
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_pending_path(
                *protocol_directory);
        CHECK(entry_exists_no_follow(owner_path) == expected.owner);
        CHECK(entry_exists_no_follow(owner_pending_path) == expected.owner_pending);

        std::error_code error;
        const auto entry_count = static_cast<std::size_t>(
            std::distance(std::filesystem::directory_iterator(*protocol_directory, error),
                          std::filesystem::directory_iterator{}));
        CHECK(!error);
        CHECK(entry_count == static_cast<std::size_t>(expected.owner) +
                                 static_cast<std::size_t>(expected.owner_pending));
    }

    const std::array unrelated{
        paths.index_path,
        paths.data_path,
        paths.intent_path,
        paths.intent_pending_path,
        paths.staged_path,
        paths.staged_pending_path,
        paths.private_handoff_path,
        paths.private_handoff_pending_path,
        paths.quarantine_index_path,
        paths.quarantine_data_path,
    };
    for (const auto& path : unrelated) {
        CHECK(!entry_exists_no_follow(path));
    }
}

void check_empty_private_lease_recovery(
    const std::filesystem::path& base,
    std::optional<OOCCleanupStatus> expected_initial_status = std::nullopt) {
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    if (!recovered.transaction_terminal()) {
        std::cerr << "private lease recovery did not converge for " << base
                  << ": status=" << static_cast<int>(recovered.status)
                  << " error=" << recovered.native_error.message() << '\n';
    }
    CHECK(recovered.transaction_terminal());
    if (expected_initial_status) {
        CHECK(recovered.status == *expected_initial_status);
    }
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_pending_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(private_lease_staging_entries(paths).empty());
    CHECK(entry_exists_no_follow(paths.lock_path));

    const auto repeated = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(repeated.status == OOCCleanupStatus::NoTransaction);

    auto fresh = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(fresh.completed());
    CHECK(entry_exists_no_follow(paths.private_directory));

    // A completed recovery must not retain stale authority over a fresh
    // generation created at the same path.
    const auto stale_retry = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(stale_retry.status == OOCCleanupStatus::Busy);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(OOCCleanupTransaction::remove_private_lease(*fresh.ownership).completed());
    CHECK(!entry_exists_no_follow(paths.private_directory));
}

void test_private_lease_process_crash_recovery(const std::string& executable) {
    TempDirectory temp;

    for (std::size_t index = 0; index < PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS.size();
         ++index) {
        const auto& contract = PRIVATE_LEASE_RESERVATION_BOUNDARY_CONTRACTS[index];
        const auto lease = temp.path() / ("reserve-" + std::to_string(index) + ".gnfs-sink-lease");
        const auto base = lease / "corpus";
        const auto child = gnfs::test::run_child_process(
            executable, {"--crash-private-lease", "reserve", std::to_string(index), base.string()});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code ==
              PRIVATE_LEASE_CRASH_EXIT_BASE + static_cast<int>(contract.fault_point));
        check_private_lease_reservation_crash_prefix(base, contract);
        const auto expected_recovery =
            contract.boundary == PrivateLeaseReservationBoundary::PermitAcquired
                ? OOCCleanupStatus::NoTransaction
                : OOCCleanupStatus::Completed;
        check_empty_private_lease_recovery(base, expected_recovery);
    }

    for (std::size_t index = 0; index < PRIVATE_LEASE_REMOVE_FAULT_POINTS.size(); ++index) {
        const auto lease = temp.path() / ("remove-" + std::to_string(index) + ".gnfs-sink-lease");
        const auto base = lease / "corpus";
        const auto child = gnfs::test::run_child_process(
            executable, {"--crash-private-lease", "remove", std::to_string(index), base.string()});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code == PRIVATE_LEASE_CRASH_EXIT_BASE +
                                     static_cast<int>(PRIVATE_LEASE_REMOVE_FAULT_POINTS[index]));
        check_empty_private_lease_recovery(base);
    }
}

void test_private_lease_preactive_rollback_crash_recovery(const std::string& executable) {
    TempDirectory temp;

    for (std::size_t index = 0; index < PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS.size(); ++index) {
        const auto lease =
            temp.path() / ("preactive-recovery-" + std::to_string(index) + ".gnfs-sink-lease");
        const auto base = lease / "corpus";
        const auto child =
            gnfs::test::run_child_process(executable, {"--crash-private-lease", "preactive-recover",
                                                       std::to_string(index), base.string()});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code ==
              PRIVATE_LEASE_CRASH_EXIT_BASE +
                  static_cast<int>(PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS[index]));
        check_empty_private_lease_recovery(base);
    }
}

void test_private_writer_preactivation_crash_recovery(const std::string& executable) {
    TempDirectory temp;

    // Every writer boundary before RESERVED is consumed remains rollback
    // state. Recovery owns the exact lease generation and removes any zero,
    // partial, or fully header-written pair left by process termination.
    for (std::size_t index = 0; index + 1 < PRIVATE_WRITER_FAULT_POINTS.size(); ++index) {
        const auto lease =
            temp.path() / ("writer-precommit-" + std::to_string(index) + ".gnfs-sink-lease");
        const auto base = lease / "corpus";
        const auto child = gnfs::test::run_child_process(
            executable, {"--crash-private-writer", std::to_string(index), base.string()});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code ==
              PRIVATE_LEASE_CRASH_EXIT_BASE + static_cast<int>(PRIVATE_WRITER_FAULT_POINTS[index]));
        check_empty_private_lease_recovery(base);
    }

    // RESERVED removal is the activation commit point. A crash after that
    // durable boundary must preserve the live pair and may not recreate
    // cleanup authority from OWNED alone.
    const std::size_t commit_index = PRIVATE_WRITER_FAULT_POINTS.size() - 1;
    const auto committed_base = temp.path() / "writer-committed.gnfs-sink-lease" / "corpus";
    const auto committed_paths = OOCCleanupTransaction::paths_for(committed_base);
    const auto child = gnfs::test::run_child_process(
        executable,
        {"--crash-private-writer", std::to_string(commit_index), committed_base.string()});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_CRASH_EXIT_BASE +
                                 static_cast<int>(PRIVATE_WRITER_FAULT_POINTS[commit_index]));
    CHECK(!entry_exists_no_follow(committed_paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(committed_paths.lease_owned_path));
    CHECK(entry_exists_no_follow(committed_paths.private_directory));
    CHECK(entry_exists_no_follow(committed_paths.index_path));
    CHECK(entry_exists_no_follow(committed_paths.data_path));
    CHECK(private_lease_staging_entries(committed_paths).empty());
    CHECK(OOCCleanupTransaction::recover_private_lease(committed_base).status ==
          OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(committed_paths.index_path));
    CHECK(entry_exists_no_follow(committed_paths.data_path));
}

void test_private_lease_preactive_link_attacks_are_preserved() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "preactive-hardlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign = temp.path() / "preactive-hardlink-target";
        write_test_leaf(foreign, "foreign hardlink target");
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
            CHECK(create_hard_link_checked(foreign, paths.index_path));
        }

        const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(recovered.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(entry_exists_no_follow(foreign));
        CHECK(entry_exists_no_follow(paths.private_directory));
        CHECK(entry_exists_no_follow(paths.index_path));
        check_entries_equivalent(foreign, paths.index_path);
        CHECK(private_lease_staging_entries(paths).empty());
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
    }

    {
        const auto base = temp.path() / "preactive-symlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign = temp.path() / "preactive-symlink-target";
        write_test_leaf(foreign, "foreign symlink target");
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
            if (!create_symlink_or_explicit_skip(foreign, paths.data_path,
                                                 "preactive pair symlink")) {
                return;
            }
        }

        const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(recovered.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(entry_exists_no_follow(foreign));
        CHECK(entry_exists_no_follow(paths.private_directory));
        CHECK(entry_is_symlink_no_follow(paths.data_path));
        CHECK(private_lease_staging_entries(paths).empty());
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
    }
}

void test_private_lease_recovery_preserves_live_pair_without_intent(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x9a9a'abab'bcbc'cdc0ULL;
    const auto base = temp.path() / "live-pair.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto child =
        gnfs::test::run_child_process(executable, {"--abandon-private-lease", "live-pair",
                                                   base.string(), std::to_string(store_id)});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_ABANDONED_EXIT);
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
}

void test_private_lease_recovery_preserves_pending_only_pair(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xa0a0'b1b1'c2c2'd3d3ULL;
    const auto base = temp.path() / "pending-only.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto child =
        gnfs::test::run_child_process(executable, {"--abandon-private-lease", "pending-only",
                                                   base.string(), std::to_string(store_id)});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_ABANDONED_EXIT);
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
}

void test_private_lease_writer_activation_closes_reservation() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xb0b0'c1c1'd2d2'e3e3ULL;
    const auto base = temp.path() / "writer-activation.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));

    write_pair(base, store_id, *reservation.ownership);
    CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(!reservation.ownership->spent());

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(begin_cleanup(base, store_id).completed());
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    CHECK(reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
}

void test_private_lease_recovery_finishes_canonical_pair_intent(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xc0c0'd1d1'e2e2'f3f3ULL;
    const auto base = temp.path() / "canonical-intent.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto child =
        gnfs::test::run_child_process(executable, {"--abandon-private-lease", "canonical-intent",
                                                   base.string(), std::to_string(store_id)});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_ABANDONED_EXIT);
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));

    CHECK(OOCCleanupTransaction::recover_private_lease(base).completed());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(paths.lock_path));
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NoTransaction);
}

enum class DeferredPublicationPermitHookAction : std::uint8_t {
    Interrupt,
    InsertHandoff,
    NestedActions,
    ReplaceOwnedSameBytes,
};

struct DeferredPublicationPermitHookContext final {
    DeferredPublicationPermitHookAction action = DeferredPublicationPermitHookAction::Interrupt;
    OOCRelationWriter* writer = nullptr;
    OOCPrivateLeaseOwnershipReceipt* lease = nullptr;
    std::filesystem::path base;
    OOCCleanupPaths paths;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> before_action;
    std::optional<NamespaceTreeSnapshot> after_action;
    gnfs::relation::OOCCleanupResult nested_remove;
    gnfs::relation::OOCCleanupResult nested_recover;
    bool invoked = false;
    bool pair_escrowed = false;
    bool lease_fresh = false;
    bool take_rejected = false;
    bool reentrant_publication_rejected = false;
    bool identity_replaced = false;
    bool hook_failed = false;
};

[[nodiscard]] bool deferred_publication_permit_hook(OOCCleanupFaultPoint point,
                                                    void* opaque) noexcept {
    auto& context = *static_cast<DeferredPublicationPermitHookContext*>(opaque);
    if (context.invoked ||
        point != OOCCleanupFaultPoint::PrivateLeaseCleanupHandoffPermitAcquired) {
        return false;
    }
    context.invoked = true;
    try {
        context.pair_escrowed =
            context.writer != nullptr && !context.writer->has_cleanup_ownership_receipt();
        context.lease_fresh = context.lease != nullptr && !context.lease->spent();
        try {
            (void)context.writer->take_cleanup_ownership_receipt();
        } catch (const std::logic_error&) {
            context.take_rejected = true;
        }
        try {
            (void)context.writer->finalize_and_publish_cleanup_handoff();
        } catch (const std::logic_error&) {
            context.reentrant_publication_rejected = true;
        }
        context.before_action = capture_namespace_tree(context.snapshot_root);
        switch (context.action) {
        case DeferredPublicationPermitHookAction::Interrupt:
            break;
        case DeferredPublicationPermitHookAction::InsertHandoff:
            write_test_leaf(context.paths.private_handoff_pending_path,
                            "post-publication-permit handoff");
            break;
        case DeferredPublicationPermitHookAction::NestedActions:
            context.nested_remove = OOCCleanupTransaction::remove_private_lease(*context.lease);
            context.nested_recover = OOCCleanupTransaction::recover_private_lease(context.base);
            break;
        case DeferredPublicationPermitHookAction::ReplaceOwnedSameBytes:
            context.identity_replaced =
                replace_private_control_leaf_same_bytes(context.paths.lease_owned_path);
            break;
        }
        context.after_action = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.hook_failed = true;
    }
    return context.action == DeferredPublicationPermitHookAction::Interrupt;
}

#if defined(__APPLE__)
void test_deferred_publication_revalidates_lease_generation() {
    TempDirectory temp;
    const auto base = temp.path() / "publication-owned-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    DeferredPublicationPermitHookContext context{
        .action = DeferredPublicationPermitHookAction::ReplaceOwnedSameBytes,
        .writer = &writer,
        .lease = &*reservation.ownership,
        .base = base,
        .paths = paths,
        .snapshot_root = temp.path(),
    };
    bool rejected = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
            .stop_after = deferred_publication_permit_hook,
            .context = &context,
        });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(context.invoked);
    CHECK(!context.hook_failed);
    CHECK(context.identity_replaced);
    CHECK(context.pair_escrowed);
    CHECK(context.lease_fresh);
    CHECK(context.after_action.has_value());
    if (context.after_action) {
        CHECK(capture_namespace_tree(temp.path()) == *context.after_action);
    }
    CHECK(writer.has_cleanup_ownership_receipt());
    CHECK(!reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
}
#endif

enum class DeferredPublicationPendingHookAction : std::uint8_t {
    Interrupt,
    InsertHandoff,
    InsertStaged,
};

struct DeferredPublicationPendingHookContext final {
    DeferredPublicationPendingHookAction action = DeferredPublicationPendingHookAction::Interrupt;
    OOCRelationWriter* writer = nullptr;
    OOCPrivateLeaseOwnershipReceipt* lease = nullptr;
    OOCCleanupPaths paths;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> after_action;
    bool invoked = false;
    bool pair_escrowed = false;
    bool lease_fresh = false;
    bool hook_failed = false;
};

void write_valid_staged_from_intent_pending(const OOCCleanupPaths& paths,
                                            const std::filesystem::path& destination) {
    const auto pending = read_test_bytes(paths.intent_pending_path);
    const auto intent = gnfs::relation::ooc_cleanup_detail::parse_marker(
        pending, gnfs::relation::ooc_cleanup_detail::INTENT_MAGIC);
    const auto staged = gnfs::relation::ooc_cleanup_detail::serialize_marker(
        intent, gnfs::relation::ooc_cleanup_detail::STAGED_MAGIC);
    write_private_control_bytes(destination, staged);
}

[[nodiscard]] bool deferred_publication_pending_hook(OOCCleanupPublishFaultPoint point,
                                                     void* opaque) noexcept {
    auto& context = *static_cast<DeferredPublicationPendingHookContext*>(opaque);
    if (context.invoked || point != OOCCleanupPublishFaultPoint::IntentPendingDurable) {
        return false;
    }
    context.invoked = true;
    try {
        context.pair_escrowed =
            context.writer != nullptr && !context.writer->has_cleanup_ownership_receipt();
        context.lease_fresh = context.lease != nullptr && !context.lease->spent();
        if (context.action == DeferredPublicationPendingHookAction::InsertHandoff) {
            write_test_leaf(context.paths.private_handoff_pending_path,
                            "post-pending-durable handoff");
        } else if (context.action == DeferredPublicationPendingHookAction::InsertStaged) {
            write_valid_staged_from_intent_pending(context.paths, context.paths.staged_path);
        }
        context.after_action = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.hook_failed = true;
    }
    return context.action == DeferredPublicationPendingHookAction::Interrupt;
}

enum class DeferredPublicationOperationHookAction : std::uint8_t {
    InsertStagedPending,
    ReplacePendingSameBytes,
    CreateCanonicalFromPending,
};

struct DeferredPublicationOperationHookContext final {
    OOCCleanupTestOperation target = OOCCleanupTestOperation::MarkerRename;
    DeferredPublicationOperationHookAction action =
        DeferredPublicationOperationHookAction::InsertStagedPending;
    OOCCleanupPaths paths;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> after_action;
    bool invoked = false;
    bool identity_replaced = false;
    bool canonical_and_pending_present = false;
    bool hook_failed = false;
};

[[nodiscard]] bool deferred_publication_operation_hook(OOCCleanupTestOperation operation,
                                                       void* opaque) noexcept {
    auto& context = *static_cast<DeferredPublicationOperationHookContext*>(opaque);
    if (context.invoked || operation != context.target) {
        return false;
    }
    context.invoked = true;
    try {
        switch (context.action) {
        case DeferredPublicationOperationHookAction::InsertStagedPending:
            write_valid_staged_from_intent_pending(context.paths,
                                                   context.paths.staged_pending_path);
            break;
        case DeferredPublicationOperationHookAction::ReplacePendingSameBytes:
            context.identity_replaced =
                replace_private_control_leaf_same_bytes(context.paths.intent_pending_path);
            break;
        case DeferredPublicationOperationHookAction::CreateCanonicalFromPending:
            write_private_control_bytes(context.paths.intent_path,
                                        read_test_bytes(context.paths.intent_pending_path));
            context.canonical_and_pending_present =
                entry_exists_no_follow(context.paths.intent_path) &&
                entry_exists_no_follow(context.paths.intent_pending_path);
            break;
        }
        context.after_action = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.hook_failed = true;
    }
    return false;
}

struct DeferredPublicationFailureSnapshotContext final {
    OOCCleanupTestOperation target = OOCCleanupTestOperation::MarkerPendingUnlink;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> before_failure;
    bool invoked = false;
    bool hook_failed = false;
};

[[nodiscard]] bool
fail_deferred_publication_operation_with_snapshot(OOCCleanupTestOperation operation,
                                                  void* opaque) noexcept {
    auto& context = *static_cast<DeferredPublicationFailureSnapshotContext*>(opaque);
    if (context.invoked || operation != context.target) {
        return false;
    }
    context.invoked = true;
    try {
        context.before_failure = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.hook_failed = true;
    }
    return true;
}

struct DeferredPublicationCanonicalHookContext final {
    OOCRelationWriter* writer = nullptr;
    OOCPrivateLeaseOwnershipReceipt* lease = nullptr;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> snapshot;
    bool invoked = false;
    bool pair_escrowed = false;
    bool lease_fresh = false;
    bool hook_failed = false;
};

[[nodiscard]] bool deferred_publication_canonical_hook(OOCCleanupFaultPoint point,
                                                       void* opaque) noexcept {
    auto& context = *static_cast<DeferredPublicationCanonicalHookContext*>(opaque);
    if (context.invoked || point != OOCCleanupFaultPoint::IntentDurable) {
        return false;
    }
    context.invoked = true;
    try {
        context.pair_escrowed =
            context.writer != nullptr && !context.writer->has_cleanup_ownership_receipt();
        context.lease_fresh = context.lease != nullptr && !context.lease->spent();
        context.snapshot = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.hook_failed = true;
    }
    return true;
}

void test_deferred_publication_permit_interrupt_and_retry() {
    TempDirectory temp;
    const auto base = temp.path() / "publication-permit-interrupt.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    (void)writer.write(make_real_relation(59, 61));
    DeferredPublicationPermitHookContext context{
        .action = DeferredPublicationPermitHookAction::Interrupt,
        .writer = &writer,
        .lease = &*reservation.ownership,
        .base = base,
        .paths = paths,
        .snapshot_root = temp.path(),
    };
    bool interrupted = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
            .stop_after = deferred_publication_permit_hook,
            .context = &context,
        });
    } catch (const std::system_error&) {
        interrupted = true;
    }
    CHECK(interrupted);
    CHECK(context.invoked);
    CHECK(!context.hook_failed);
    CHECK(context.pair_escrowed);
    CHECK(context.lease_fresh);
    CHECK(context.take_rejected);
    CHECK(context.reentrant_publication_rejected);
    CHECK(context.after_action.has_value());
    if (context.after_action) {
        CHECK(capture_namespace_tree(temp.path()) == *context.after_action);
    }
    CHECK(writer.has_cleanup_ownership_receipt());
    CHECK(!reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto descriptor = writer.finalize_and_publish_cleanup_handoff();
    CHECK(descriptor.count == 1);
    CHECK(!writer.has_cleanup_ownership_receipt());
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
}

void test_deferred_publication_permit_revalidates_handoff_insertion() {
    TempDirectory temp;
    const auto base = temp.path() / "publication-permit-handoff.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    DeferredPublicationPermitHookContext context{
        .action = DeferredPublicationPermitHookAction::InsertHandoff,
        .writer = &writer,
        .lease = &*reservation.ownership,
        .base = base,
        .paths = paths,
        .snapshot_root = temp.path(),
    };
    bool rejected = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
            .stop_after = deferred_publication_permit_hook,
            .context = &context,
        });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(context.invoked);
    CHECK(!context.hook_failed);
    CHECK(context.pair_escrowed);
    CHECK(context.lease_fresh);
    CHECK(context.take_rejected);
    CHECK(context.reentrant_publication_rejected);
    CHECK(context.after_action.has_value());
    if (context.after_action) {
        CHECK(capture_namespace_tree(temp.path()) == *context.after_action);
    }
    CHECK(writer.has_cleanup_ownership_receipt());
    CHECK(!reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    std::error_code error;
    CHECK(std::filesystem::remove(paths.private_handoff_pending_path, error));
    CHECK(!error);
    (void)writer.finalize_and_publish_cleanup_handoff();
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
}

void test_deferred_publication_claim_blocks_nested_actions() {
    TempDirectory temp;
    const auto base = temp.path() / "publication-nested-actions.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    DeferredPublicationPermitHookContext context{
        .action = DeferredPublicationPermitHookAction::NestedActions,
        .writer = &writer,
        .lease = &*reservation.ownership,
        .base = base,
        .paths = paths,
        .snapshot_root = temp.path(),
    };
    (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
        .stop_after = deferred_publication_permit_hook,
        .context = &context,
    });
    CHECK(context.invoked);
    CHECK(!context.hook_failed);
    CHECK(context.pair_escrowed);
    CHECK(context.lease_fresh);
    CHECK(context.take_rejected);
    CHECK(context.reentrant_publication_rejected);
    CHECK(context.nested_remove.status == OOCCleanupStatus::Busy);
    CHECK(context.nested_remove.stage == OOCCleanupStage::None);
    CHECK(context.nested_recover.status == OOCCleanupStatus::Busy);
    CHECK(context.nested_recover.stage == OOCCleanupStage::None);
    CHECK(context.before_action.has_value());
    CHECK(context.after_action.has_value());
    if (context.before_action && context.after_action) {
        CHECK(*context.before_action == *context.after_action);
    }
    CHECK(!writer.has_cleanup_ownership_receipt());
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    CHECK(reservation.ownership->spent());
}

#ifndef _WIN32
struct DeferredPublicationForkCopyContext final {
    int signal_descriptor = -1;
    pid_t child_process = -1;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> child_publication_snapshot;
    int child_status = -1;
    bool invoked = false;
    bool hook_failed = false;
};

[[nodiscard]] bool deferred_publication_release_fork_copy(OOCCleanupFaultPoint point,
                                                          void* opaque) noexcept {
    auto& context = *static_cast<DeferredPublicationForkCopyContext*>(opaque);
    if (context.invoked ||
        point != OOCCleanupFaultPoint::PrivateLeaseCleanupHandoffPermitAcquired) {
        return false;
    }
    context.invoked = true;
    try {
        char signal = 'p';
        ssize_t written = -1;
        do {
            written = ::write(context.signal_descriptor, &signal, sizeof(signal));
        } while (written < 0 && errno == EINTR);
        if (written != static_cast<ssize_t>(sizeof(signal))) {
            throw std::runtime_error("could not release deferred publication fork copy");
        }
        if (::waitpid(context.child_process, &context.child_status, 0) != context.child_process) {
            throw std::runtime_error("could not wait for deferred publication fork copy");
        }
        context.child_publication_snapshot = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.hook_failed = true;
    }
    return false;
}

void test_deferred_publication_fork_copy_is_closed_by_retained_witness() {
    TempDirectory temp;
    const auto base = temp.path() / "publication-fork-copy.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    (void)writer.write(make_real_relation(67, 71));
    const auto finalized = writer.finalize();
    CHECK(finalized.count == 1);

    int signal_pipe[2]{-1, -1};
    CHECK(::pipe(signal_pipe) == 0);
    if (signal_pipe[0] < 0 || signal_pipe[1] < 0) {
        return;
    }
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child < 0) {
        (void)::close(signal_pipe[0]);
        (void)::close(signal_pipe[1]);
        return;
    }
    if (child == 0) {
        (void)::close(signal_pipe[1]);
        char signal = '\0';
        ssize_t read_count = -1;
        do {
            read_count = ::read(signal_pipe[0], &signal, sizeof(signal));
        } while (read_count < 0 && errno == EINTR);
        (void)::close(signal_pipe[0]);
        if (read_count != static_cast<ssize_t>(sizeof(signal)) || signal != 'p') {
            ::_exit(91);
        }
        try {
            const auto descriptor = writer.finalize_and_publish_cleanup_handoff();
            const bool published = descriptor.count == finalized.count &&
                                   !writer.has_cleanup_ownership_receipt() &&
                                   entry_exists_no_follow(paths.intent_path) &&
                                   !entry_exists_no_follow(paths.intent_pending_path);
            ::_exit(published ? 0 : 92);
        } catch (...) {
            ::_exit(93);
        }
    }

    (void)::close(signal_pipe[0]);
    DeferredPublicationForkCopyContext context{
        .signal_descriptor = signal_pipe[1],
        .child_process = child,
        .snapshot_root = temp.path(),
    };
    bool rejected = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
            .stop_after = deferred_publication_release_fork_copy,
            .context = &context,
        });
    } catch (const std::system_error&) {
        rejected = true;
    }
    (void)::close(signal_pipe[1]);

    CHECK(rejected);
    CHECK(context.invoked);
    CHECK(!context.hook_failed);
    CHECK(WIFEXITED(context.child_status));
    CHECK(WEXITSTATUS(context.child_status) == 0);
    CHECK(context.child_publication_snapshot.has_value());
    if (context.child_publication_snapshot) {
        CHECK(capture_namespace_tree(temp.path()) == *context.child_publication_snapshot);
    }
    CHECK(writer.has_cleanup_ownership_receipt());
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto retried = writer.finalize_and_publish_cleanup_handoff();
    CHECK(retried.count == finalized.count);
    CHECK(!writer.has_cleanup_ownership_receipt());
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
}
#endif

void test_deferred_publication_pending_phase_and_receipt_escrow() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "publication-pending-retry.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        DeferredPublicationPendingHookContext context{
            .action = DeferredPublicationPendingHookAction::Interrupt,
            .writer = &writer,
            .lease = &*reservation.ownership,
            .paths = paths,
            .snapshot_root = temp.path(),
        };
        bool interrupted = false;
        try {
            (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
                .stop_after_publish = deferred_publication_pending_hook,
                .context = &context,
            });
        } catch (const std::system_error&) {
            interrupted = true;
        }
        CHECK(interrupted);
        CHECK(context.invoked);
        CHECK(!context.hook_failed);
        CHECK(context.pair_escrowed);
        CHECK(context.lease_fresh);
        CHECK(context.after_action.has_value());
        if (context.after_action) {
            CHECK(capture_namespace_tree(temp.path()) == *context.after_action);
        }
        CHECK(writer.has_cleanup_ownership_receipt());
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));

        (void)writer.finalize_and_publish_cleanup_handoff();
        CHECK(!writer.has_cleanup_ownership_receipt());
        CHECK(entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    }

    {
        const auto base = temp.path() / "publication-pending-drift.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        DeferredPublicationPendingHookContext context{
            .action = DeferredPublicationPendingHookAction::InsertHandoff,
            .writer = &writer,
            .lease = &*reservation.ownership,
            .paths = paths,
            .snapshot_root = temp.path(),
        };
        bool rejected = false;
        try {
            (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
                .stop_after_publish = deferred_publication_pending_hook,
                .context = &context,
            });
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(context.invoked);
        CHECK(!context.hook_failed);
        CHECK(context.pair_escrowed);
        CHECK(context.lease_fresh);
        CHECK(context.after_action.has_value());
        if (context.after_action) {
            CHECK(capture_namespace_tree(temp.path()) == *context.after_action);
        }
        CHECK(writer.has_cleanup_ownership_receipt());
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));

        std::error_code error;
        CHECK(std::filesystem::remove(paths.private_handoff_pending_path, error));
        CHECK(!error);
        (void)writer.finalize_and_publish_cleanup_handoff();
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    }

    {
        const auto base =
            temp.path() / "publication-pending-staged-drift.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        DeferredPublicationPendingHookContext context{
            .action = DeferredPublicationPendingHookAction::InsertStaged,
            .writer = &writer,
            .lease = &*reservation.ownership,
            .paths = paths,
            .snapshot_root = temp.path(),
        };
        bool rejected = false;
        try {
            (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
                .stop_after_publish = deferred_publication_pending_hook,
                .context = &context,
            });
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(context.invoked);
        CHECK(!context.hook_failed);
        CHECK(context.pair_escrowed);
        CHECK(context.lease_fresh);
        CHECK(context.after_action.has_value());
        if (context.after_action) {
            CHECK(capture_namespace_tree(temp.path()) == *context.after_action);
        }
        CHECK(writer.has_cleanup_ownership_receipt());
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.staged_path));

        std::error_code error;
        CHECK(std::filesystem::remove(paths.staged_path, error));
        CHECK(!error);
        (void)writer.finalize_and_publish_cleanup_handoff();
        CHECK(!writer.has_cleanup_ownership_receipt());
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    }
}

void test_deferred_publication_phase_gate_rejects_allowed_slot_and_inode_drift() {
    TempDirectory temp;
    constexpr std::array actions{
        DeferredPublicationOperationHookAction::InsertStagedPending,
        DeferredPublicationOperationHookAction::ReplacePendingSameBytes,
    };

    for (std::size_t index = 0; index < actions.size(); ++index) {
        const auto base =
            temp.path() /
            ("publication-operation-drift-" + std::to_string(index) + ".gnfs-sink-lease") /
            "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        if (!reservation.completed()) {
            continue;
        }

        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        DeferredPublicationOperationHookContext context{
            .target = OOCCleanupTestOperation::MarkerRename,
            .action = actions[index],
            .paths = paths,
            .snapshot_root = temp.path(),
        };
        bool rejected = false;
        try {
            (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
                .fail_before_operation = deferred_publication_operation_hook,
                .context = &context,
            });
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(context.invoked);
        CHECK(!context.hook_failed);
        CHECK(context.after_action.has_value());
        if (context.after_action) {
            CHECK(capture_namespace_tree(temp.path()) == *context.after_action);
        }
        CHECK(writer.has_cleanup_ownership_receipt());
        CHECK(!reservation.ownership->spent());
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));

        if (actions[index] == DeferredPublicationOperationHookAction::InsertStagedPending) {
            CHECK(entry_exists_no_follow(paths.staged_pending_path));
            std::error_code error;
            CHECK(std::filesystem::remove(paths.staged_pending_path, error));
            CHECK(!error);
        } else {
            CHECK(context.identity_replaced);
        }

        (void)writer.finalize_and_publish_cleanup_handoff();
        CHECK(!writer.has_cleanup_ownership_receipt());
        CHECK(entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    }
}

void test_deferred_publication_destination_exists_commits_exact_canonical() {
    TempDirectory temp;
    const auto base = temp.path() / "publication-destination-exists.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    DeferredPublicationOperationHookContext context{
        .target = OOCCleanupTestOperation::MarkerRenameAuthorized,
        .action = DeferredPublicationOperationHookAction::CreateCanonicalFromPending,
        .paths = paths,
        .snapshot_root = temp.path(),
    };
    const auto descriptor = writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
        .fail_before_operation = deferred_publication_operation_hook,
        .context = &context,
    });
    CHECK(context.invoked);
    CHECK(!context.hook_failed);
    CHECK(context.canonical_and_pending_present);
    CHECK(context.after_action.has_value());
    CHECK(descriptor.store_id != 0);
    CHECK(!writer.has_cleanup_ownership_receipt());
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
}

void prepare_deferred_publication_duplicate_canonical(OOCRelationWriter& writer,
                                                      OOCPrivateLeaseOwnershipReceipt& lease,
                                                      const OOCCleanupPaths& paths) {
    PublishStopContext pending_stop{
        .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
    };
    bool interrupted = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff(publish_stop_hooks(pending_stop));
    } catch (const std::system_error&) {
        interrupted = true;
    }
    CHECK(interrupted);
    CHECK(pending_stop.stopped);
    CHECK(writer.has_cleanup_ownership_receipt());
    CHECK(!lease.spent());
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));

    write_private_control_bytes(paths.intent_path, read_test_bytes(paths.intent_pending_path));
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
}

void test_deferred_publication_canonical_commit_survives_pending_cleanup_failures() {
    TempDirectory temp;
    constexpr std::array operations{
        OOCCleanupTestOperation::MarkerPendingUnlink,
        OOCCleanupTestOperation::MarkerPendingUnlinkParentSync,
    };

    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto base =
            temp.path() /
            ("publication-sticky-commit-" + std::to_string(index) + ".gnfs-sink-lease") / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        if (!reservation.completed()) {
            continue;
        }
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        prepare_deferred_publication_duplicate_canonical(writer, *reservation.ownership, paths);

        DeferredPublicationFailureSnapshotContext failure{
            .target = operations[index],
            .snapshot_root = temp.path(),
        };
        bool rejected = false;
        try {
            (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
                .fail_before_operation = fail_deferred_publication_operation_with_snapshot,
                .context = &failure,
            });
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(failure.invoked);
        CHECK(!failure.hook_failed);
        CHECK(failure.before_failure.has_value());
        if (failure.before_failure) {
            CHECK(capture_namespace_tree(temp.path()) == *failure.before_failure);
        }
        CHECK(!writer.has_cleanup_ownership_receipt());
        CHECK(!reservation.ownership->spent());
        CHECK(entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.intent_pending_path) ==
              (operations[index] == OOCCleanupTestOperation::MarkerPendingUnlink));

        bool retry_rejected = false;
        try {
            (void)writer.finalize_and_publish_cleanup_handoff();
        } catch (const std::logic_error&) {
            retry_rejected = true;
        }
        CHECK(retry_rejected);
    }
}

void test_deferred_publication_pending_unlink_rejects_hook_replacement() {
    TempDirectory temp;
    const auto base =
        temp.path() / "publication-pending-unlink-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }
    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    prepare_deferred_publication_duplicate_canonical(writer, *reservation.ownership, paths);

    DeferredPublicationOperationHookContext context{
        .target = OOCCleanupTestOperation::MarkerPendingUnlink,
        .action = DeferredPublicationOperationHookAction::ReplacePendingSameBytes,
        .paths = paths,
        .snapshot_root = temp.path(),
    };
    bool rejected = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
            .fail_before_operation = deferred_publication_operation_hook,
            .context = &context,
        });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(context.invoked);
    CHECK(!context.hook_failed);
    CHECK(context.identity_replaced);
    CHECK(context.after_action.has_value());
    if (context.after_action) {
        CHECK(capture_namespace_tree(temp.path()) == *context.after_action);
    }
    CHECK(!writer.has_cleanup_ownership_receipt());
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
}

void test_deferred_publication_commits_receipt_before_canonical_hook() {
    TempDirectory temp;
    const auto base = temp.path() / "publication-canonical-escrow.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    DeferredPublicationCanonicalHookContext context{
        .writer = &writer,
        .lease = &*reservation.ownership,
        .snapshot_root = temp.path(),
    };
    bool interrupted = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
            .stop_after = deferred_publication_canonical_hook,
            .context = &context,
        });
    } catch (const std::system_error&) {
        interrupted = true;
    }
    CHECK(interrupted);
    CHECK(context.invoked);
    CHECK(!context.hook_failed);
    CHECK(context.pair_escrowed);
    CHECK(context.lease_fresh);
    CHECK(context.snapshot.has_value());
    if (context.snapshot) {
        CHECK(capture_namespace_tree(temp.path()) == *context.snapshot);
    }
    CHECK(!writer.has_cleanup_ownership_receipt());
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    bool retry_rejected = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff();
    } catch (const std::logic_error&) {
        retry_rejected = true;
    }
    CHECK(retry_rejected);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
}

void test_deferred_private_writer_handoff_and_pending_recovery() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "deferred-handoff.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
            const auto descriptor = writer.finalize_and_publish_cleanup_handoff();
            CHECK(descriptor.count == 0);
        }
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(OOCRelationReader(base.string()).count() == 0);
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    }

    {
        const auto base = temp.path() / "deferred-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        bool interrupted = false;
        {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
            PublishStopContext stop{
                .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
            };
            try {
                (void)writer.finalize_and_publish_cleanup_handoff(publish_stop_hooks(stop));
            } catch (const std::system_error&) {
                interrupted = true;
            }
        }
        CHECK(interrupted);
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    }
}

void test_deferred_handoff_foreign_leaf_blocks_pair_mutation() {
    TempDirectory temp;
    const auto base = temp.path() / "handoff-foreign.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        (void)writer.finalize_and_publish_cleanup_handoff();
    }

    const auto foreign = paths.private_directory / "foreign-control-leaf";
    write_test_leaf(foreign, "foreign");
    const auto rejected = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
    CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(entry_exists_no_follow(foreign));
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));

    std::error_code error;
    CHECK(std::filesystem::remove(foreign, error));
    CHECK(!error);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
}

void test_private_handoff_writer_rejects_metadata_before_finalize() {
    TempDirectory temp;
    const auto base = temp.path() / "private-handoff-invalid-metadata.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());

    std::optional<OOCSnapshotDescriptor> descriptor;
    {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        (void)writer.write(make_real_relation(31, 37));

        bool zero_kind_rejected = false;
        try {
            (void)writer.finalize_and_publish_private_handoff(0, PRIVATE_HANDOFF_PAYLOAD_VERSION,
                                                              PRIVATE_HANDOFF_PAYLOAD);
        } catch (const std::invalid_argument&) {
            zero_kind_rejected = true;
        }
        CHECK(zero_kind_rejected);
        (void)writer.write(make_real_relation(41, 43));

        bool zero_version_rejected = false;
        try {
            (void)writer.finalize_and_publish_private_handoff(PRIVATE_HANDOFF_PAYLOAD_KIND, 0,
                                                              PRIVATE_HANDOFF_PAYLOAD);
        } catch (const std::invalid_argument&) {
            zero_version_rejected = true;
        }
        CHECK(zero_version_rejected);

        const std::vector<std::byte> oversized_payload(
            gnfs::relation::OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES + 1U, std::byte{0x7f});
        bool oversized_rejected = false;
        try {
            (void)writer.finalize_and_publish_private_handoff(
                PRIVATE_HANDOFF_PAYLOAD_KIND, PRIVATE_HANDOFF_PAYLOAD_VERSION, oversized_payload);
        } catch (const std::invalid_argument&) {
            oversized_rejected = true;
        }
        CHECK(oversized_rejected);
        (void)writer.write(make_real_relation(47, 53));

        descriptor = writer.finalize_and_publish_cleanup_handoff();
    }
    CHECK(descriptor.has_value());
    if (descriptor) {
        CHECK(descriptor->count == 3);
    }
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_directory));
}

void test_private_handoff_transaction_rejects_oversize_before_mutation() {
    TempDirectory temp;
    const auto base =
        temp.path() / "private-handoff-oversize-transaction.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const std::vector<std::byte> oversized_payload(
        gnfs::relation::OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES + 1U, std::byte{0x6a});

    const auto rejected = OOCCleanupTransaction::publish_private_handoff(
        prepared.pair_ownership, prepared.lease_ownership,
        handoff_pair_descriptor(prepared.descriptor), PRIVATE_HANDOFF_PAYLOAD_KIND,
        PRIVATE_HANDOFF_PAYLOAD_VERSION, oversized_payload);
    CHECK(rejected.result.status == OOCCleanupStatus::InvalidRequest);
    CHECK(!rejected.canonical());
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());

    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(read_test_bytes(paths.lease_reserved_path) == reserved_bytes);
    CHECK(read_test_bytes(paths.lease_owned_path) == owned_bytes);
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));

    CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).completed());
    CHECK(prepared.lease_ownership.spent());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_directory));
}

#if !defined(__APPLE__)
void test_private_handoff_unsupported_publish_is_non_mutating() {
    TempDirectory temp;
    const auto base =
        temp.path() / "unsupported-private-handoff-publish.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);

    const auto rejected = publish_private_handoff(prepared);
    CHECK(rejected.result.status == OOCCleanupStatus::PlatformUnsupported);
    CHECK(rejected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(!rejected.canonical());
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());

    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(read_test_bytes(paths.lease_reserved_path) == reserved_bytes);
    CHECK(read_test_bytes(paths.lease_owned_path) == owned_bytes);
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));

    CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).completed());
    CHECK(prepared.lease_ownership.spent());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_directory));
}

struct UnsupportedAdoptionHookContext final {
    std::size_t observations = 0;
};

[[nodiscard]] bool observe_unsupported_adoption(OOCPrivateHandoffAdoptionFaultPoint,
                                                void* opaque) noexcept {
    auto& context = *static_cast<UnsupportedAdoptionHookContext*>(opaque);
    ++context.observations;
    return false;
}

void test_private_handoff_unsupported_adoption_is_non_observing() {
    TempDirectory temp;
    UnsupportedAdoptionHookContext context;
    const auto hooks = OOCPrivateHandoffAdoptionTestHooks{
        .stop_after = observe_unsupported_adoption,
        .context = &context,
    };

    {
        const auto base = temp.path() / "unsupported-adoption-missing.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto unsupported = OOCCleanupTransaction::adopt_private_handoff(base, hooks);
        CHECK(unsupported.result.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(!unsupported.adopted());
        CHECK(!unsupported.adoption.has_value());
        CHECK(context.observations == 0);
        CHECK(!entry_exists_no_follow(paths.lock_path));
        CHECK(!entry_exists_no_follow(paths.private_directory));
    }

    {
        const auto base = temp.path() / "unsupported-adoption-existing.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        CHECK(std::filesystem::create_directory(paths.private_directory));
        const auto sentinel = paths.private_directory / "unobserved-sentinel";
        write_test_leaf(sentinel, "unsupported adoption must not inspect this directory");
        const auto sentinel_bytes = read_test_bytes(sentinel);
        const auto unsupported = OOCCleanupTransaction::adopt_private_handoff(base, hooks);
        CHECK(unsupported.result.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(!unsupported.adopted());
        CHECK(!unsupported.adoption.has_value());
        CHECK(context.observations == 0);
        CHECK(read_test_bytes(sentinel) == sentinel_bytes);
        CHECK(!entry_exists_no_follow(paths.lock_path));
    }

    const auto invalid = OOCCleanupTransaction::adopt_private_handoff({}, hooks);
    CHECK(invalid.result.status == OOCCleanupStatus::InvalidRequest);
    CHECK(!invalid.adopted());
    CHECK(!invalid.adoption.has_value());
    CHECK(context.observations == 0);
}
#endif

[[nodiscard]] std::filesystem::path
private_lease_stage_path_for_token(const gnfs::relation::OOCCleanupPaths& paths,
                                   std::string_view token) {
    auto leaf = paths.private_directory.filename().native();
    leaf.append(std::filesystem::path(".gnfs-private-lease-v1.stage-").native());
    leaf.append(std::filesystem::path(std::string(token)).native());
    return paths.private_directory.parent_path() / std::filesystem::path(std::move(leaf));
}

void check_missing_lock_orphan_stage_conflict(
    const std::filesystem::path& base, const std::filesystem::path& stage_path,
    const std::filesystem::path& sentinel_path,
    const std::optional<std::array<std::uint64_t, 3>>& expected_identity) {
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto sentinel_bytes = read_test_bytes(sentinel_path);
    CHECK(!entry_exists_no_follow(paths.lock_path));

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NamespaceConflict);
    const auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership.has_value());

    CHECK(!entry_exists_no_follow(paths.lock_path));
    CHECK(entry_exists_no_follow(stage_path));
    CHECK(read_test_bytes(sentinel_path) == sentinel_bytes);
    if (expected_identity) {
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(stage_path) ==
              expected_identity);
    }
}

void test_private_handoff_missing_lock_orphan_stage_is_preserved() {
    TempDirectory temp;
    constexpr std::string_view EXACT_TOKEN = "0123456789abcdef0123456789abcdef";

    {
        const auto base = temp.path() / "orphan-stage.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto stage_path = private_lease_stage_path_for_token(paths, EXACT_TOKEN);
        const auto sentinel_path = stage_path / "sentinel";
        std::error_code error;
        CHECK(std::filesystem::create_directory(stage_path, error));
        CHECK(!error);
        write_test_leaf(sentinel_path, "orphan stage sentinel");
        const auto identity =
            gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(stage_path);
        CHECK(identity.has_value());

        check_missing_lock_orphan_stage_conflict(base, stage_path, sentinel_path, identity);
    }

    {
        const auto base = temp.path() / "orphan-stage-symlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto stage_path = private_lease_stage_path_for_token(paths, EXACT_TOKEN);
        const auto target = temp.path() / "orphan-stage-symlink-target";
        const auto sentinel_path = target / "sentinel";
        std::error_code error;
        CHECK(std::filesystem::create_directory(target, error));
        CHECK(!error);
        write_test_leaf(sentinel_path, "orphan symlink stage sentinel");
        if (create_symlink_or_explicit_skip(target, stage_path,
                                            "missing-lock private lease stage symlink")) {
            check_missing_lock_orphan_stage_conflict(base, stage_path, sentinel_path, std::nullopt);
            CHECK(entry_is_symlink_no_follow(stage_path));
            check_entries_equivalent(stage_path, target);
        }
    }
}

void test_private_handoff_invalid_orphan_stage_names_are_ignored() {
    TempDirectory temp;
    const auto base = temp.path() / "invalid-orphan-stages.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    constexpr std::array<std::string_view, 5> INVALID_TOKENS{
        "0123456789abcdef0123456789abcde",        "0123456789abcdef0123456789abcdef0",
        "0123456789abcdef0123456789abcdeF",       "0123456789abcdef0123456789abcdeg",
        "0123456789abcdef0123456789abcdef.extra",
    };

    std::array<std::filesystem::path, INVALID_TOKENS.size()> stage_paths;
    std::array<std::optional<std::array<std::uint64_t, 3>>, INVALID_TOKENS.size()> identities;
    std::array<std::vector<std::byte>, INVALID_TOKENS.size()> sentinel_bytes;
    for (std::size_t index = 0; index < INVALID_TOKENS.size(); ++index) {
        stage_paths[index] = private_lease_stage_path_for_token(paths, INVALID_TOKENS[index]);
        std::error_code error;
        CHECK(std::filesystem::create_directory(stage_paths[index], error));
        CHECK(!error);
        write_test_leaf(stage_paths[index] / "sentinel", std::to_string(index));
        sentinel_bytes[index] = read_test_bytes(stage_paths[index] / "sentinel");
        identities[index] = gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
            stage_paths[index]);
        CHECK(identities[index].has_value());
    }

    CHECK(!entry_exists_no_follow(paths.lock_path));
    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::NoTransaction);
    CHECK(inspected.state == OOCPrivateHandoffState::None);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NoTransaction);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (reservation.completed()) {
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
        CHECK(reservation.ownership->spent());
    }

    for (std::size_t index = 0; index < INVALID_TOKENS.size(); ++index) {
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                  stage_paths[index]) == identities[index]);
        CHECK(read_test_bytes(stage_paths[index] / "sentinel") == sentinel_bytes[index]);
    }
}

[[nodiscard]] bool is_preserving_namespace_status(OOCCleanupStatus status) {
    return status == OOCCleanupStatus::NamespaceConflict ||
           status == OOCCleanupStatus::ForeignReplacementPreserved;
}

struct PrivateLeaseStopOnceContext final {
    OOCPrivateLeaseFaultPoint target = OOCPrivateLeaseFaultPoint::OwnedDurable;
    bool stopped = false;
};

[[nodiscard]] bool stop_private_lease_once(OOCPrivateLeaseFaultPoint point, void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseStopOnceContext*>(opaque);
    if (!context.stopped && point == context.target) {
        context.stopped = true;
        return true;
    }
    return false;
}

struct PrivateCleanupPermitNewHandoffContext final {
    OOCPrivateLeaseFaultPoint target = OOCPrivateLeaseFaultPoint::RecoveryPermitAcquired;
    std::filesystem::path handoff_path;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> expected_after_hook;
    bool invoked = false;
    bool created = false;
};

[[nodiscard]] bool
inject_handoff_after_private_cleanup_permit_acquired(OOCPrivateLeaseFaultPoint point,
                                                     void* opaque) noexcept {
    auto& context = *static_cast<PrivateCleanupPermitNewHandoffContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    try {
        write_test_leaf(context.handoff_path, "post-permit foreign handoff");
        context.created = true;
        context.expected_after_hook = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.created = false;
        context.expected_after_hook.reset();
    }
    return false;
}

struct LegacyCleanupPermitInjectionContext final {
    std::filesystem::path handoff_path;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> expected_after_hook;
    bool invoked = false;
    bool created = false;
};

[[nodiscard]] bool inject_handoff_after_legacy_cleanup_permit(OOCCleanupFaultPoint point,
                                                              void* opaque) noexcept {
    auto& context = *static_cast<LegacyCleanupPermitInjectionContext*>(opaque);
    if (context.invoked || point != OOCCleanupFaultPoint::LegacyCleanupPermitAcquired) {
        return false;
    }
    context.invoked = true;
    try {
        write_test_leaf(context.handoff_path, "post-permit foreign handoff");
        context.created = true;
        context.expected_after_hook = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.created = false;
        context.expected_after_hook.reset();
    }
    return false;
}

struct LegacyCleanupOperationInjectionContext final {
    OOCCleanupTestOperation target = OOCCleanupTestOperation::IndexRename;
    std::filesystem::path handoff_path;
    std::filesystem::path snapshot_root;
    std::optional<NamespaceTreeSnapshot> expected_after_hook;
    bool invoked = false;
    bool created = false;
};

[[nodiscard]] bool inject_handoff_before_legacy_cleanup_operation(OOCCleanupTestOperation operation,
                                                                  void* opaque) noexcept {
    auto& context = *static_cast<LegacyCleanupOperationInjectionContext*>(opaque);
    if (context.invoked || operation != context.target) {
        return false;
    }
    context.invoked = true;
    try {
        write_test_leaf(context.handoff_path, "pre-operation foreign handoff");
        context.created = true;
        context.expected_after_hook = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.created = false;
        context.expected_after_hook.reset();
    }
    return false;
}

void test_legacy_cleanup_permit_interrupt_is_non_mutating() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "legacy-permit-interrupt-resume.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        StopContext stop{
            .target = OOCCleanupFaultPoint::LegacyCleanupPermitAcquired,
        };
        const auto interrupted = OOCCleanupTransaction::resume(base, stop_hooks(stop));
        CHECK(stop.stopped);
        CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
        CHECK(interrupted.stage == OOCCleanupStage::None);
        CHECK(capture_namespace_tree(temp.path()) == before);

        const auto retried = OOCCleanupTransaction::resume(base);
        CHECK(retried.completed());
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.index_path));
        CHECK(!entry_exists_no_follow(paths.data_path));
    }

    {
        const auto base = temp.path() / "legacy-permit-interrupt-begin.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base, false);
        const auto exact = exact_for(prepared.descriptor);
        std::optional<OOCPrivateLeaseOwnershipReceipt> dropped_lease;
        dropped_lease.emplace(std::move(prepared.lease_ownership));
        dropped_lease.reset();

        const auto before = capture_namespace_tree(temp.path());
        StopContext stop{
            .target = OOCCleanupFaultPoint::LegacyCleanupPermitAcquired,
        };
        const auto interrupted = OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership,
                                                                        exact, stop_hooks(stop));
        CHECK(stop.stopped);
        CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
        CHECK(interrupted.stage == OOCCleanupStage::None);
        CHECK(!prepared.pair_ownership.spent());
        CHECK(capture_namespace_tree(temp.path()) == before);

        CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership, exact).completed());
        CHECK(prepared.pair_ownership.spent());
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.index_path));
        CHECK(!entry_exists_no_follow(paths.data_path));
    }
}

void test_legacy_cleanup_permit_revalidates_new_handoff() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "legacy-permit-insert-resume.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        LegacyCleanupPermitInjectionContext injection{
            .handoff_path = paths.private_handoff_pending_path,
            .snapshot_root = temp.path(),
        };
        const OOCCleanupTestHooks hooks{
            .stop_after = inject_handoff_after_legacy_cleanup_permit,
            .stop_after_publish = nullptr,
            .fail_before_operation = nullptr,
            .context = &injection,
        };
        const auto rejected = OOCCleanupTransaction::resume(base, hooks);
        CHECK(injection.invoked);
        CHECK(injection.created);
        CHECK(injection.expected_after_hook.has_value());
        CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(rejected.stage == OOCCleanupStage::None);
        if (injection.expected_after_hook) {
            CHECK(capture_namespace_tree(temp.path()) == *injection.expected_after_hook);
        }
        CHECK(entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
    }

    {
        const auto base = temp.path() / "legacy-permit-insert-begin.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base, false);
        const auto exact = exact_for(prepared.descriptor);
        std::optional<OOCPrivateLeaseOwnershipReceipt> dropped_lease;
        dropped_lease.emplace(std::move(prepared.lease_ownership));
        dropped_lease.reset();

        LegacyCleanupPermitInjectionContext injection{
            .handoff_path = paths.private_handoff_pending_path,
            .snapshot_root = temp.path(),
        };
        const OOCCleanupTestHooks hooks{
            .stop_after = inject_handoff_after_legacy_cleanup_permit,
            .stop_after_publish = nullptr,
            .fail_before_operation = nullptr,
            .context = &injection,
        };
        const auto rejected =
            OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership, exact, hooks);
        CHECK(injection.invoked);
        CHECK(injection.created);
        CHECK(injection.expected_after_hook.has_value());
        CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(!prepared.pair_ownership.spent());
        if (injection.expected_after_hook) {
            CHECK(capture_namespace_tree(temp.path()) == *injection.expected_after_hook);
        }
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
    }
}

void test_legacy_cleanup_mutation_gate_runs_after_operation_hook() {
    TempDirectory temp;
    const auto base = temp.path() / "legacy-mutation-gate-after-hook.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = prepare_private_legacy_cleanup_intent(base);
    reservation.ownership.reset();

    LegacyCleanupOperationInjectionContext injection{
        .handoff_path = paths.private_handoff_pending_path,
        .snapshot_root = temp.path(),
    };
    const auto rejected = OOCCleanupTransaction::resume(
        base, OOCCleanupTestHooks{
                  .stop_after = nullptr,
                  .stop_after_publish = nullptr,
                  .fail_before_operation = inject_handoff_before_legacy_cleanup_operation,
                  .context = &injection,
              });
    CHECK(injection.invoked);
    CHECK(injection.created);
    CHECK(injection.expected_after_hook.has_value());
    CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(rejected.stage == OOCCleanupStage::None);
    if (injection.expected_after_hook) {
        CHECK(capture_namespace_tree(temp.path()) == *injection.expected_after_hook);
    }
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
}

void test_legacy_cleanup_mutation_gate_covers_resumed_tails() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "legacy-delete-authorized-tail.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        StopContext stop{
            .target = OOCCleanupFaultPoint::DeleteAuthorizedDurable,
        };
        const auto delete_authorized = OOCCleanupTransaction::resume(base, stop_hooks(stop));
        CHECK(stop.stopped);
        CHECK(delete_authorized.status == OOCCleanupStatus::Interrupted);
        CHECK(delete_authorized.stage == OOCCleanupStage::DeleteAuthorized);

        LegacyCleanupOperationInjectionContext injection{
            .target = OOCCleanupTestOperation::DataUnlink,
            .handoff_path = paths.private_handoff_pending_path,
            .snapshot_root = temp.path(),
        };
        const auto rejected = OOCCleanupTransaction::resume(
            base, OOCCleanupTestHooks{
                      .stop_after = nullptr,
                      .stop_after_publish = nullptr,
                      .fail_before_operation = inject_handoff_before_legacy_cleanup_operation,
                      .context = &injection,
                  });
        CHECK(injection.invoked);
        CHECK(injection.created);
        CHECK(injection.expected_after_hook.has_value());
        CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
        if (injection.expected_after_hook) {
            CHECK(capture_namespace_tree(temp.path()) == *injection.expected_after_hook);
        }
        CHECK(entry_exists_no_follow(paths.quarantine_data_path));
        CHECK(entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.staged_path));
    }

    {
        const auto base = temp.path() / "legacy-staged-only-tail.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        StopContext stop{
            .target = OOCCleanupFaultPoint::IntentRemovedDurable,
        };
        const auto intent_removed = OOCCleanupTransaction::resume(base, stop_hooks(stop));
        CHECK(stop.stopped);
        CHECK(intent_removed.status == OOCCleanupStatus::Interrupted);
        CHECK(intent_removed.stage == OOCCleanupStage::IntentRemoved);

        LegacyCleanupOperationInjectionContext injection{
            .target = OOCCleanupTestOperation::StagedUnlink,
            .handoff_path = paths.private_handoff_pending_path,
            .snapshot_root = temp.path(),
        };
        const auto rejected = OOCCleanupTransaction::resume(
            base, OOCCleanupTestHooks{
                      .stop_after = nullptr,
                      .stop_after_publish = nullptr,
                      .fail_before_operation = inject_handoff_before_legacy_cleanup_operation,
                      .context = &injection,
                  });
        CHECK(injection.invoked);
        CHECK(injection.created);
        CHECK(injection.expected_after_hook.has_value());
        CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
        if (injection.expected_after_hook) {
            CHECK(capture_namespace_tree(temp.path()) == *injection.expected_after_hook);
        }
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.staged_path));
    }
}

void test_legacy_cleanup_empty_terminal_consumes_private_receipt() {
    TempDirectory temp;
    const auto base = temp.path() / "legacy-empty-terminal.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base, false);
    const auto exact = exact_for(prepared.descriptor);
    std::optional<OOCPrivateLeaseOwnershipReceipt> dropped_lease;
    dropped_lease.emplace(std::move(prepared.lease_ownership));
    dropped_lease.reset();

    std::error_code error;
    CHECK(std::filesystem::remove(paths.index_path, error));
    CHECK(!error);
    error.clear();
    CHECK(std::filesystem::remove(paths.data_path, error));
    CHECK(!error);

    CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership, exact).completed());
    CHECK(prepared.pair_ownership.spent());
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
}

void test_legacy_cleanup_marker_rename_failure_is_retryable() {
    TempDirectory temp;
    const auto base = temp.path() / "legacy-marker-rename-failure.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base, false);
    const auto exact = exact_for(prepared.descriptor);
    std::optional<OOCPrivateLeaseOwnershipReceipt> dropped_lease;
    dropped_lease.emplace(std::move(prepared.lease_ownership));
    dropped_lease.reset();

    OperationFailureContext failure{
        .target = OOCCleanupTestOperation::MarkerRename,
    };
    const auto rejected = OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership, exact,
                                                                 operation_failure_hooks(failure));
    CHECK(failure.failed);
    CHECK(rejected.status == OOCCleanupStatus::IoFailure);
    CHECK(rejected.retryable());
    CHECK(!prepared.pair_ownership.spent());
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));

    CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership, exact).completed());
    CHECK(prepared.pair_ownership.spent());
    check_cleanup_complete(paths);
}

void test_legacy_cleanup_mutation_gate_defers_for_exact_pending() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "legacy-exact-intent-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base, false);
        const auto exact = exact_for(prepared.descriptor);
        std::optional<OOCPrivateLeaseOwnershipReceipt> dropped_lease;
        dropped_lease.emplace(std::move(prepared.lease_ownership));
        dropped_lease.reset();

        PublishStopContext pending_stop{
            .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
        };
        const auto pending_only = OOCCleanupTransaction::begin_or_resume(
            prepared.pair_ownership, exact, publish_stop_hooks(pending_stop));
        CHECK(pending_stop.stopped);
        CHECK(pending_only.status == OOCCleanupStatus::Interrupted);
        CHECK(!prepared.pair_ownership.spent());
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));

        LegacyCleanupOperationInjectionContext injection{
            .target = OOCCleanupTestOperation::MarkerRename,
            .handoff_path = paths.private_handoff_pending_path,
            .snapshot_root = temp.path(),
        };
        const auto rejected = OOCCleanupTransaction::begin_or_resume(
            prepared.pair_ownership, exact,
            OOCCleanupTestHooks{
                .stop_after = nullptr,
                .stop_after_publish = nullptr,
                .fail_before_operation = inject_handoff_before_legacy_cleanup_operation,
                .context = &injection,
            });
        CHECK(injection.invoked);
        CHECK(injection.created);
        CHECK(injection.expected_after_hook.has_value());
        CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(!prepared.pair_ownership.spent());
        if (injection.expected_after_hook) {
            CHECK(capture_namespace_tree(temp.path()) == *injection.expected_after_hook);
        }
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
    }

    {
        const auto base = temp.path() / "legacy-exact-staged-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        reservation.ownership.reset();

        StopContext pair_stop{
            .target = OOCCleanupFaultPoint::SecondRenameDurable,
        };
        const auto pair_quarantined = OOCCleanupTransaction::resume(base, stop_hooks(pair_stop));
        CHECK(pair_stop.stopped);
        CHECK(pair_quarantined.status == OOCCleanupStatus::Interrupted);
        CHECK(entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(entry_exists_no_follow(paths.quarantine_data_path));
        CHECK(!entry_exists_no_follow(paths.staged_path));

        PublishStopContext pending_stop{
            .target = OOCCleanupPublishFaultPoint::StagedPendingDurable,
        };
        const auto pending_only =
            OOCCleanupTransaction::resume(base, publish_stop_hooks(pending_stop));
        CHECK(pending_stop.stopped);
        CHECK(pending_only.status == OOCCleanupStatus::Interrupted);
        CHECK(entry_exists_no_follow(paths.staged_pending_path));
        CHECK(!entry_exists_no_follow(paths.staged_path));

        LegacyCleanupOperationInjectionContext injection{
            .target = OOCCleanupTestOperation::MarkerRename,
            .handoff_path = paths.private_handoff_pending_path,
            .snapshot_root = temp.path(),
        };
        const auto rejected = OOCCleanupTransaction::resume(
            base, OOCCleanupTestHooks{
                      .stop_after = nullptr,
                      .stop_after_publish = nullptr,
                      .fail_before_operation = inject_handoff_before_legacy_cleanup_operation,
                      .context = &injection,
                  });
        CHECK(injection.invoked);
        CHECK(injection.created);
        CHECK(injection.expected_after_hook.has_value());
        CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
        if (injection.expected_after_hook) {
            CHECK(capture_namespace_tree(temp.path()) == *injection.expected_after_hook);
        }
        CHECK(entry_exists_no_follow(paths.staged_pending_path));
        CHECK(!entry_exists_no_follow(paths.staged_path));
    }
}

void test_private_lease_reservation_permit_interrupt_is_non_mutating_and_retryable() {
    TempDirectory temp;
    const auto base = temp.path() / "reservation-permit-interrupt.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);

    // Seed the permanent sibling lock so the operation-level snapshot starts
    // at the exact empty namespace retained by the reservation permit.
    auto seed = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(seed.completed());
    if (!seed.completed()) {
        return;
    }
    CHECK(OOCCleanupTransaction::remove_private_lease(*seed.ownership).completed());
    CHECK(entry_exists_no_follow(paths.lock_path));
    CHECK(!entry_exists_no_follow(paths.private_directory));

    const auto before = capture_namespace_tree(temp.path());
    PrivateLeaseStopOnceContext interruption{
        .target = OOCPrivateLeaseFaultPoint::ReservationPermitAcquired,
    };
    const auto interrupted = OOCCleanupTransaction::reserve_private_lease(
        base, OOCPrivateLeaseTestHooks{
                  .stop_after = stop_private_lease_once,
                  .context = &interruption,
              });
    CHECK(interruption.stopped);
    CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
    CHECK(!interrupted.ownership.has_value());
    CHECK(capture_namespace_tree(temp.path()) == before);

    auto retried = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(retried.completed());
    if (!retried.completed()) {
        return;
    }
    CHECK(OOCCleanupTransaction::remove_private_lease(*retried.ownership).completed());
    CHECK(retried.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));
}

void test_private_fresh_writer_permit_interrupt_is_non_mutating_and_retryable() {
    TempDirectory temp;
    const auto base = temp.path() / "fresh-writer-permit-interrupt.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    const auto before = capture_namespace_tree(temp.path());
    PrivateLeaseStopOnceContext interruption{
        .target = OOCPrivateLeaseFaultPoint::FreshWriterPermitAcquired,
    };
    bool rejected = false;
    std::optional<OOCRelationWriter> writer;
    try {
        writer.emplace(base.string(), *reservation.ownership,
                       OOCPrivateLeaseTestHooks{
                           .stop_after = stop_private_lease_once,
                           .context = &interruption,
                       });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(interruption.stopped);
    CHECK(rejected);
    CHECK(!writer.has_value());
    CHECK(!reservation.ownership->spent());
    CHECK(capture_namespace_tree(temp.path()) == before);
    CHECK(!entry_exists_no_follow(paths.index_path));
    CHECK(!entry_exists_no_follow(paths.data_path));
    if (!rejected) {
        writer.reset();
        return;
    }

    {
        OOCRelationWriter retried(base.string(), *reservation.ownership);
        CHECK(retried.has_cleanup_ownership_receipt());
    }
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
}

void test_private_lease_activation_permit_interrupt_preserves_pair_for_recovery() {
    TempDirectory temp;
    const auto base = temp.path() / "activation-permit-interrupt.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    const auto owner_bytes = read_test_bytes(owner_path);
    PrivateLeaseStopOnceContext interruption{
        .target = OOCPrivateLeaseFaultPoint::ActivationPermitAcquired,
    };
    bool rejected = false;
    std::optional<OOCRelationWriter> writer;
    try {
        writer.emplace(base.string(), *reservation.ownership,
                       OOCPrivateLeaseTestHooks{
                           .stop_after = stop_private_lease_once,
                           .context = &interruption,
                       });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(interruption.stopped);
    CHECK(rejected);
    CHECK(!writer.has_value());
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    if (!rejected) {
        writer.reset();
        return;
    }

    // Activation admission is a separate action from fresh construction.
    // Once Fresh has completed, a pre-commit activation interruption retains
    // the exact pair for durable RESERVED recovery instead of reviving an
    // already-ended Fresh rollback authority.
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    CHECK(reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.index_path));
    CHECK(!entry_exists_no_follow(paths.data_path));

    auto retried_reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(retried_reservation.completed());
    if (!retried_reservation.completed()) {
        return;
    }
    {
        OOCRelationWriter retried(base.string(), *retried_reservation.ownership);
        CHECK(retried.has_cleanup_ownership_receipt());
    }
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
}

void test_private_lease_recovery_permit_interrupt_is_non_mutating(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xd0d0'e1e1'f2f2'a3a3ULL;
    const auto base = temp.path() / "permit-interrupt.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto child =
        gnfs::test::run_child_process(executable, {"--abandon-private-lease", "pending-only",
                                                   base.string(), std::to_string(store_id)});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_ABANDONED_EXIT);

    const auto before = capture_namespace_tree(temp.path());
    PrivateLeaseStopOnceContext interruption{
        .target = OOCPrivateLeaseFaultPoint::RecoveryPermitAcquired,
    };
    const auto interrupted = OOCCleanupTransaction::recover_private_lease(
        base, OOCPrivateLeaseTestHooks{
                  .stop_after = stop_private_lease_once,
                  .context = &interruption,
              });
    CHECK(interruption.stopped);
    CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
    CHECK(capture_namespace_tree(temp.path()) == before);

    const auto retried = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(retried.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
}

void test_private_lease_recovery_permit_revalidates_new_handoff(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xe0e0'f1f1'a2a2'b3b3ULL;
    const auto base = temp.path() / "permit-new-handoff.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto child =
        gnfs::test::run_child_process(executable, {"--abandon-private-lease", "pending-only",
                                                   base.string(), std::to_string(store_id)});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_ABANDONED_EXIT);

    const auto external_root = paths.private_directory.parent_path();
    const auto external_before = without_namespace_subtree_contents(
        capture_namespace_tree(external_root), paths.private_directory.filename());
    const auto lock_bytes = read_test_bytes(paths.lock_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    PrivateCleanupPermitNewHandoffContext injection{
        .handoff_path = paths.private_handoff_pending_path,
        .snapshot_root = paths.private_directory,
    };
    const auto rejected = OOCCleanupTransaction::recover_private_lease(
        base, OOCPrivateLeaseTestHooks{
                  .stop_after = inject_handoff_after_private_cleanup_permit_acquired,
                  .context = &injection,
              });
    CHECK(injection.invoked);
    CHECK(injection.created);
    CHECK(injection.expected_after_hook.has_value());
    CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
    if (injection.expected_after_hook) {
        CHECK(capture_namespace_tree(paths.private_directory) == *injection.expected_after_hook);
    }
    const auto external_after = without_namespace_subtree_contents(
        capture_namespace_tree(external_root), paths.private_directory.filename());
    CHECK(external_after == external_before);
    CHECK(read_test_bytes(paths.lock_path) == lock_bytes);
    CHECK(read_test_bytes(paths.lease_owned_path) == owned_bytes);
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));

    std::error_code error;
    CHECK(std::filesystem::remove(paths.private_handoff_pending_path, error));
    CHECK(!error);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
}

void test_private_lease_removal_permit_interrupt_is_non_mutating() {
    TempDirectory temp;
    const auto base = temp.path() / "removal-permit-interrupt.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    const auto private_before = capture_namespace_tree(paths.private_directory);
    const auto external_before = capture_private_lease_external_namespace_without_lock(paths);
    PrivateLeaseStopOnceContext interruption{
        .target = OOCPrivateLeaseFaultPoint::RemovalPermitAcquired,
    };
    const auto interrupted = OOCCleanupTransaction::remove_private_lease(
        *reservation.ownership, OOCPrivateLeaseTestHooks{
                                    .stop_after = stop_private_lease_once,
                                    .context = &interruption,
                                });
    CHECK(interruption.stopped);
    CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
    CHECK(!reservation.ownership->spent());
    CHECK(capture_namespace_tree(paths.private_directory) == private_before);
    CHECK(capture_private_lease_external_namespace_without_lock(paths) == external_before);

    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    CHECK(reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(paths.lock_path));
}

void test_private_lease_removal_permit_revalidates_new_handoff() {
    TempDirectory temp;
    const auto base = temp.path() / "removal-permit-new-handoff.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    const auto external_before = capture_private_lease_external_namespace_without_lock(paths);
    PrivateCleanupPermitNewHandoffContext injection{
        .target = OOCPrivateLeaseFaultPoint::RemovalPermitAcquired,
        .handoff_path = paths.private_handoff_pending_path,
        .snapshot_root = paths.private_directory,
    };
    const auto rejected = OOCCleanupTransaction::remove_private_lease(
        *reservation.ownership,
        OOCPrivateLeaseTestHooks{
            .stop_after = inject_handoff_after_private_cleanup_permit_acquired,
            .context = &injection,
        });
    CHECK(injection.invoked);
    CHECK(injection.created);
    CHECK(injection.expected_after_hook.has_value());
    CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
    if (injection.expected_after_hook) {
        CHECK(capture_namespace_tree(paths.private_directory) == *injection.expected_after_hook);
    }
    CHECK(capture_private_lease_external_namespace_without_lock(paths) == external_before);

    std::error_code error;
    CHECK(std::filesystem::remove(paths.private_handoff_pending_path, error));
    CHECK(!error);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    CHECK(reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(paths.lock_path));
}

void test_private_lease_removal_preserves_corrupt_cleanup_marker() {
    TempDirectory temp;
    const auto base = temp.path() / "removal-permit-corrupt-intent.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }

    constexpr std::array corrupt_intent{
        std::byte{0xde},
        std::byte{0xad},
        std::byte{0xbe},
        std::byte{0xef},
    };
    write_private_control_bytes(paths.intent_path, corrupt_intent);
    const auto private_before = capture_namespace_tree(paths.private_directory);
    const auto external_before = capture_private_lease_external_namespace_without_lock(paths);

    const auto rejected = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
    CHECK(rejected.status == OOCCleanupStatus::IntentCorrupt);
    CHECK(!reservation.ownership->spent());
    CHECK(capture_namespace_tree(paths.private_directory) == private_before);
    CHECK(capture_private_lease_external_namespace_without_lock(paths) == external_before);

    std::error_code error;
    CHECK(std::filesystem::remove(paths.intent_path, error));
    CHECK(!error);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    CHECK(reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(paths.lock_path));
}

void test_private_lease_removal_admission_projects_union_blocker_first() {
    TempDirectory temp;
    const auto base = temp.path() / "removal-admission-blocker-first.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (!reservation.completed()) {
        return;
    }
    reservation.ownership.reset();

    constexpr std::array corrupt_intent{
        std::byte{0xde},
        std::byte{0xad},
        std::byte{0xbe},
        std::byte{0xef},
    };
    write_private_control_bytes(paths.intent_path, corrupt_intent);
    const auto before = capture_namespace_tree(temp.path());

    auto lock = std::make_shared<BaseLock>(paths.lock_path);
    auto admission = admit_private_lease_removal_locked(paths, lock, {}, {}, {}, {});
    CHECK(admission.blocked.has_value());
    CHECK(!admission.permit.has_value());
    CHECK(!admission.generation.has_value());
    if (admission.blocked) {
        CHECK(admission.blocked->status == OOCCleanupStatus::IntentCorrupt);
        CHECK(admission.blocked->stage == OOCCleanupStage::None);
    }
    CHECK(capture_namespace_tree(temp.path()) == before);
}

struct PrivateLeasePostSyncReplacementContext final {
    std::filesystem::path reserved_path;
    bool invoked = false;
    bool injected = false;
};

[[nodiscard]] bool inject_reserved_replacement_after_commit(OOCPrivateLeaseFaultPoint point,
                                                            void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeasePostSyncReplacementContext*>(opaque);
    if (context.invoked || point != OOCPrivateLeaseFaultPoint::ReservedRemovedDurable) {
        return false;
    }
    context.invoked = true;
    try {
        std::ofstream stream(context.reserved_path, std::ios::binary | std::ios::out);
        constexpr std::string_view payload = "foreign RESERVED replacement";
        stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        stream.flush();
        stream.close();
        context.injected = stream.good();
    } catch (...) {
        context.injected = false;
    }
    return false;
}

void test_private_lease_unknown_child_preserves_matching_pending() {
    TempDirectory temp;

    {
        const auto base =
            temp.path() / "final-unknown-before-pending-cleanup.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto foreign_path = paths.private_directory / "foreign-control-leaf";
        write_private_control_bytes(paths.lease_reserved_pending_path,
                                    read_test_bytes(paths.lease_reserved_path));
        write_private_control_bytes(paths.lease_owned_pending_path,
                                    read_test_bytes(paths.lease_owned_path));
        write_test_leaf(foreign_path, "foreign final child");

        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto reserved_pending_bytes = read_test_bytes(paths.lease_reserved_pending_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owned_pending_bytes = read_test_bytes(paths.lease_owned_pending_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        const auto foreign_bytes = read_test_bytes(foreign_path);
        const auto directory_identity =
            gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                paths.private_directory);

        const auto removed = OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership);
        CHECK(is_preserving_namespace_status(removed.status));
        CHECK(!prepared.lease_ownership.spent());
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_reserved_pending_path, reserved_pending_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(paths.lease_owned_pending_path, owned_pending_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        check_test_bytes_preserved(paths.index_path, index_bytes);
        check_test_bytes_preserved(paths.data_path, data_bytes);
        check_test_bytes_preserved(foreign_path, foreign_bytes);
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                  paths.private_directory) == directory_identity);

        std::optional<OOCPrivateLeaseOwnershipReceipt> stale_lease;
        stale_lease.emplace(std::move(prepared.lease_ownership));
        stale_lease.reset();
        const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(is_preserving_namespace_status(recovered.status));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_reserved_pending_path, reserved_pending_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(paths.lease_owned_pending_path, owned_pending_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        check_test_bytes_preserved(paths.index_path, index_bytes);
        check_test_bytes_preserved(paths.data_path, data_bytes);
        check_test_bytes_preserved(foreign_path, foreign_bytes);
        CHECK(!prepared.pair_ownership.spent());
    }

    {
        const auto base =
            temp.path() / "staging-unknown-before-pending-cleanup.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        PrivateLeaseStopOnceContext stop{
            .target = OOCPrivateLeaseFaultPoint::OwnedDurable,
        };
        const auto reservation = OOCCleanupTransaction::reserve_private_lease(
            base, OOCPrivateLeaseTestHooks{
                      .stop_after = stop_private_lease_once,
                      .context = &stop,
                  });
        CHECK(reservation.result.status == OOCCleanupStatus::Interrupted);
        CHECK(!reservation.ownership.has_value());
        CHECK(stop.stopped);
        const auto staging = private_lease_staging_entries(paths);
        CHECK(staging.size() == 1);
        if (staging.size() != 1) {
            return;
        }
        const auto owner_path = staging.front() / ".gnfs-private-lease-v1.owner";
        const auto foreign_path = staging.front() / "foreign-control-leaf";
        write_private_control_bytes(paths.lease_reserved_pending_path,
                                    read_test_bytes(paths.lease_reserved_path));
        write_private_control_bytes(paths.lease_owned_pending_path,
                                    read_test_bytes(paths.lease_owned_path));
        write_test_leaf(foreign_path, "foreign staging child");

        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto reserved_pending_bytes = read_test_bytes(paths.lease_reserved_pending_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owned_pending_bytes = read_test_bytes(paths.lease_owned_pending_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        const auto foreign_bytes = read_test_bytes(foreign_path);
        const auto staging_identity =
            gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(staging.front());

        const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(is_preserving_namespace_status(recovered.status));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_reserved_pending_path, reserved_pending_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(paths.lease_owned_pending_path, owned_pending_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        check_test_bytes_preserved(foreign_path, foreign_bytes);
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                  staging.front()) == staging_identity);
    }
}

struct FreshWriterBoundaryContext final {
    OOCPrivateLeaseFaultPoint observe = OOCPrivateLeaseFaultPoint::FreshIndexReserved;
    std::filesystem::path foreign_path;
    bool observed = false;
    bool injected = false;
    bool injection_failed = false;
};

[[nodiscard]] bool observe_or_inject_fresh_writer_boundary(OOCPrivateLeaseFaultPoint point,
                                                           void* opaque) noexcept {
    auto& context = *static_cast<FreshWriterBoundaryContext*>(opaque);
    if (point != context.observe) {
        return false;
    }
    context.observed = true;
    if (!context.foreign_path.empty()) {
        try {
            write_test_leaf(context.foreign_path, "injected activation foreign child");
            context.injected = true;
        } catch (...) {
            context.injection_failed = true;
        }
    }
    return point == OOCPrivateLeaseFaultPoint::FreshIndexReserved;
}

struct FreshWriterSameInodeSizeDriftContext final {
    OOCPrivateLeaseFaultPoint target = OOCPrivateLeaseFaultPoint::FreshIndexReserved;
    std::filesystem::path mutation_path;
    std::filesystem::path snapshot_root;
    std::optional<std::array<std::uint64_t, 3>> identity_before;
    std::optional<std::array<std::uint64_t, 3>> identity_after;
    std::optional<NamespaceTreeSnapshot> expected_failure_scene;
    std::uint64_t size_before = 0;
    std::uint64_t size_after = 0;
    bool invoked = false;
    bool appended = false;
    bool injection_failed = false;
};

[[nodiscard]] bool append_same_inode_at_fresh_writer_boundary(OOCPrivateLeaseFaultPoint point,
                                                              void* opaque) noexcept {
    auto& context = *static_cast<FreshWriterSameInodeSizeDriftContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    try {
        const auto before =
            gnfs::relation::ooc_cleanup_detail::inspect_file(context.mutation_path, 0, false);
        if (before.kind != gnfs::relation::ooc_cleanup_detail::InspectKind::Present) {
            context.injection_failed = true;
            return false;
        }
        context.identity_before =
            gnfs::relation::ooc_cleanup_detail::stable_identity(before.identity);
        context.size_before = before.identity.size;

        std::fstream stream(context.mutation_path,
                            std::ios::binary | std::ios::in | std::ios::out | std::ios::ate);
        constexpr char APPENDED_BYTE = '\x5a';
        stream.write(&APPENDED_BYTE, 1);
        stream.flush();
        stream.close();
        if (!stream) {
            context.injection_failed = true;
            return false;
        }

        const auto after =
            gnfs::relation::ooc_cleanup_detail::inspect_file(context.mutation_path, 0, false);
        if (after.kind != gnfs::relation::ooc_cleanup_detail::InspectKind::Present) {
            context.injection_failed = true;
            return false;
        }
        context.identity_after =
            gnfs::relation::ooc_cleanup_detail::stable_identity(after.identity);
        context.size_after = after.identity.size;
        context.appended = context.identity_before == context.identity_after &&
                           context.size_before < (std::numeric_limits<std::uint64_t>::max)() &&
                           context.size_after == context.size_before + 1;
        context.expected_failure_scene = capture_namespace_tree(context.snapshot_root);
    } catch (...) {
        context.injection_failed = true;
        context.expected_failure_scene.reset();
    }
    // Continue into the next Authorized boundary. It must reject this exact
    // inode's unapproved size drift instead of recording that size as a
    // successor.
    return false;
}

void test_private_fresh_writer_authorized_gate_rejects_same_inode_size_drift() {
    constexpr std::array TARGETS{
        OOCPrivateLeaseFaultPoint::FreshIndexReserved,
        OOCPrivateLeaseFaultPoint::FreshDataReserved,
    };

    for (std::size_t index = 0; index < TARGETS.size(); ++index) {
        TempDirectory temp;
        const auto base = temp.path() /
                          ("fresh-size-drift-" + std::to_string(index) + ".gnfs-sink-lease") /
                          "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        if (!reservation.completed()) {
            continue;
        }

        const auto mutation_path = TARGETS[index] == OOCPrivateLeaseFaultPoint::FreshIndexReserved
                                       ? paths.index_path
                                       : paths.data_path;
        FreshWriterSameInodeSizeDriftContext injection{
            .target = TARGETS[index],
            .mutation_path = mutation_path,
            .snapshot_root = temp.path(),
        };
        bool rejected = false;
        std::optional<OOCRelationWriter> writer;
        try {
            writer.emplace(base.string(), *reservation.ownership,
                           OOCPrivateLeaseTestHooks{
                               .stop_after = append_same_inode_at_fresh_writer_boundary,
                               .context = &injection,
                           });
        } catch (const std::system_error&) {
            rejected = true;
        }

        CHECK(injection.invoked);
        CHECK(injection.appended);
        CHECK(!injection.injection_failed);
        CHECK(injection.identity_before.has_value());
        CHECK(injection.identity_before == injection.identity_after);
        CHECK(injection.size_before == 0);
        CHECK(injection.size_after == 1);
        CHECK(injection.expected_failure_scene.has_value());
        CHECK(rejected);
        CHECK(!writer.has_value());
        CHECK(!reservation.ownership->spent());
        if (injection.expected_failure_scene) {
            CHECK(capture_namespace_tree(temp.path()) == *injection.expected_failure_scene);
        }
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(std::filesystem::file_size(paths.index_path) ==
              (TARGETS[index] == OOCPrivateLeaseFaultPoint::FreshIndexReserved ? 1U : 0U));
        CHECK(entry_exists_no_follow(mutation_path));
        CHECK(read_test_bytes(mutation_path) == std::vector<std::byte>{std::byte{0x5a}});
        if (TARGETS[index] == OOCPrivateLeaseFaultPoint::FreshIndexReserved) {
            // The next DataReservationAuthorized gate rejected before the
            // second O_EXCL mutation.
            CHECK(!entry_exists_no_follow(paths.data_path));
        } else {
            // The next HeaderWriteAuthorized gate rejected before either
            // protocol header was written.
            CHECK(entry_exists_no_follow(paths.data_path));
            CHECK(std::filesystem::file_size(paths.data_path) == 1);
        }

        if (!rejected) {
            writer.reset();
            continue;
        }
        reservation.ownership.reset();
        CHECK(!reservation.ownership.has_value());
        check_empty_private_lease_recovery(base);
    }
}

void test_private_lease_unknown_scan_precedes_writer_mutation() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "unknown-before-fresh-writer.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto foreign_path = paths.private_directory / "foreign-control-leaf";
        write_test_leaf(foreign_path, "foreign before writer");
        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        const auto foreign_bytes = read_test_bytes(foreign_path);
        FreshWriterBoundaryContext boundary;

        bool rejected = false;
        try {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCPrivateLeaseTestHooks{
                                         .stop_after = observe_or_inject_fresh_writer_boundary,
                                         .context = &boundary,
                                     });
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(!boundary.observed);
        CHECK(!entry_exists_no_follow(paths.index_path));
        CHECK(!entry_exists_no_follow(paths.data_path));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        check_test_bytes_preserved(foreign_path, foreign_bytes);
        CHECK(!reservation.ownership->spent());
    }

    {
        const auto base = temp.path() / "unknown-before-reserved-revoke.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto foreign_path = paths.private_directory / "activation-foreign-control-leaf";
        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        FreshWriterBoundaryContext boundary{
            .observe = OOCPrivateLeaseFaultPoint::FreshPairOwnershipCaptured,
            .foreign_path = foreign_path,
        };

        bool rejected = false;
        try {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCPrivateLeaseTestHooks{
                                         .stop_after = observe_or_inject_fresh_writer_boundary,
                                         .context = &boundary,
                                     });
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(boundary.observed);
        CHECK(boundary.injected);
        CHECK(!boundary.injection_failed);
        CHECK(rejected);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        if (boundary.injected) {
            constexpr std::string_view EXPECTED = "injected activation foreign child";
            const auto actual = read_test_bytes(foreign_path);
            CHECK(actual.size() == EXPECTED.size());
            CHECK(std::equal(actual.begin(), actual.end(), EXPECTED.begin(), EXPECTED.end(),
                             [](std::byte byte, char character) {
                                 return std::to_integer<unsigned char>(byte) ==
                                        static_cast<unsigned char>(character);
                             }));
        }
        CHECK(!reservation.ownership->spent());
    }
}

void test_unscoped_writer_rejects_existing_preactive_private_lease() {
    TempDirectory temp;

    for (const bool add_unknown_child : {false, true}) {
        const std::string lease_name = add_unknown_child ? "unscoped-writer-preactive-unknown"
                                                         : "unscoped-writer-preactive-clean";
        const auto base = temp.path() / (lease_name + ".gnfs-sink-lease") / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());

        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto unknown_path = paths.private_directory / "foreign-control-leaf";
        if (add_unknown_child) {
            write_test_leaf(unknown_path, "unscoped writer foreign child");
        }

        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        std::optional<std::vector<std::byte>> unknown_bytes;
        if (add_unknown_child) {
            unknown_bytes = read_test_bytes(unknown_path);
        }
        const auto directory_identity =
            gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                paths.private_directory);
        CHECK(directory_identity.has_value());
        CHECK(entry_exists_no_follow(paths.lock_path));
        CHECK(!entry_exists_no_follow(paths.index_path));
        CHECK(!entry_exists_no_follow(paths.data_path));
        CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
        CHECK(!entry_exists_no_follow(paths.lease_owned_pending_path));

        // Leave the durable preactive namespace in place but release the live
        // lock, so rejection must come from the namespace gate rather than Busy.
        reservation.ownership.reset();
        CHECK(!reservation.ownership.has_value());
        CHECK(entry_exists_no_follow(paths.lock_path));

        bool rejected = false;
        std::optional<OOCRelationWriter> writer;
        try {
            writer.emplace(base.string());
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(!writer.has_value());

        // Keep an erroneously constructed writer alive through these checks so
        // its destructor cannot hide an O_EXCL pair mutation.
        CHECK(!entry_exists_no_follow(paths.index_path));
        CHECK(!entry_exists_no_follow(paths.data_path));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        if (unknown_bytes.has_value()) {
            check_test_bytes_preserved(unknown_path, *unknown_bytes);
        }
        CHECK(entry_exists_no_follow(paths.lock_path));
        CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
        CHECK(!entry_exists_no_follow(paths.lease_owned_pending_path));
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                  paths.private_directory) == directory_identity);

        writer.reset();
    }
}

void test_private_lease_unknown_scan_precedes_legacy_intent_publication() {
    TempDirectory temp;
    const auto base = temp.path() / "unknown-before-legacy-intent.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    (void)writer.write(make_real_relation(59, 61));
    const auto descriptor = writer.finalize();
    CHECK(descriptor.count == 1);
    const auto foreign_path = paths.private_directory / "foreign-control-leaf";
    write_test_leaf(foreign_path, "foreign before legacy intent");

    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto foreign_bytes = read_test_bytes(foreign_path);
    bool rejected = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff();
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(foreign_path, foreign_bytes);
    CHECK(!reservation.ownership->spent());
}

void test_private_lease_activation_interrupt_after_commit_preserves_pair() {
    TempDirectory temp;
    const auto base =
        temp.path() / "private-activation-commit-interrupt.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    PrivateLeaseStopOnceContext interruption{
        .target = OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
    };

    bool rejected = false;
    try {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCPrivateLeaseTestHooks{
                                     .stop_after = stop_private_lease_once,
                                     .context = &interruption,
                                 });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(interruption.stopped);
    CHECK(rejected);
    CHECK(!reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(owner_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::RecoveryRequired);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
}

void test_private_lease_activation_post_sync_failure_preserves_pair() {
    TempDirectory temp;
    const auto base =
        temp.path() / "private-activation-post-sync-failure.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    PrivateLeasePostSyncReplacementContext replacement{
        .reserved_path = paths.lease_reserved_path,
    };

    bool rejected = false;
    try {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCPrivateLeaseTestHooks{
                                     .stop_after = inject_reserved_replacement_after_commit,
                                     .context = &replacement,
                                 });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(replacement.invoked);
    CHECK(replacement.injected);
    CHECK(rejected);
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(owner_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::IntentCorrupt);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
}

#if !defined(_WIN32)
[[nodiscard]] bool replace_test_lock_leaf(const std::filesystem::path& lock_path,
                                          const std::filesystem::path& saved_lock_path) noexcept {
    if (::rename(lock_path.c_str(), saved_lock_path.c_str()) != 0) {
        return false;
    }
    const int descriptor = ::open(lock_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }

    constexpr std::string_view payload = "foreign replacement lock";
    std::size_t written = 0;
    while (written < payload.size()) {
        const auto result = ::write(descriptor, payload.data() + written, payload.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    int sync_result = -1;
    if (written == payload.size()) {
        do {
            sync_result = ::fsync(descriptor);
        } while (sync_result != 0 && errno == EINTR);
    }
    const bool synced = sync_result == 0;
    const bool closed = ::close(descriptor) == 0;
    return synced && closed;
}

[[nodiscard]] bool replace_test_leaf_with_bytes(const std::filesystem::path& path,
                                                const std::filesystem::path& saved_path,
                                                std::span<const std::byte> bytes) noexcept {
    if (::rename(path.c_str(), saved_path.c_str()) != 0) {
        return false;
    }
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }

    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    int sync_result = -1;
    if (written == bytes.size()) {
        do {
            sync_result = ::fsync(descriptor);
        } while (sync_result != 0 && errno == EINTR);
    }
    const bool synced = sync_result == 0;
    const bool closed = ::close(descriptor) == 0;
    return synced && closed;
}

[[nodiscard]] bool create_test_leaf_noexcept(const std::filesystem::path& path,
                                             std::string_view payload) noexcept {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    std::size_t written = 0;
    while (written < payload.size()) {
        const auto result = ::write(descriptor, payload.data() + written, payload.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    const bool complete = written == payload.size();
    const bool closed = ::close(descriptor) == 0;
    return complete && closed;
}

#if defined(__APPLE__)
struct ProtectedPrivateHandoffBytes final {
    std::vector<std::byte> canonical;
    std::vector<std::byte> index;
    std::vector<std::byte> data;
    std::vector<std::byte> owner;
    std::vector<std::byte> owned;
};

[[nodiscard]] ProtectedPrivateHandoffBytes
capture_protected_private_handoff_bytes(const OOCCleanupPaths& paths) {
    return {
        .canonical = read_test_bytes(paths.private_handoff_path),
        .index = read_test_bytes(paths.index_path),
        .data = read_test_bytes(paths.data_path),
        .owner = read_test_bytes(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory)),
        .owned = read_test_bytes(paths.lease_owned_path),
    };
}

void check_protected_private_handoff_bytes(const OOCCleanupPaths& paths,
                                           const ProtectedPrivateHandoffBytes& expected) {
    check_test_bytes_preserved(paths.private_handoff_path, expected.canonical);
    check_test_bytes_preserved(paths.index_path, expected.index);
    check_test_bytes_preserved(paths.data_path, expected.data);
    check_test_bytes_preserved(
        gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory),
        expected.owner);
    check_test_bytes_preserved(paths.lease_owned_path, expected.owned);
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_pending_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
}

[[nodiscard]] std::size_t count_open_test_descriptors() {
    std::error_code error;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator cursor("/dev/fd", error), end; !error && cursor != end;
         cursor.increment(error)) {
        ++count;
    }
    if (error) {
        throw std::filesystem::filesystem_error("enumerate /dev/fd", "/dev/fd", error);
    }
    return count;
}

void check_adopted_private_handoff_reader(OOCPrivateHandoffReader& adopted,
                                          std::size_t expected_count) {
    CHECK(adopted.valid());
    CHECK(adopted.reader().count() == expected_count);
    CHECK(adopted.record().pair.count == expected_count);
    CHECK(adopted.record().index.identity == adopted.index_snapshot().identity);
    CHECK(adopted.record().index.extent == adopted.index_snapshot().size);
    CHECK(adopted.record().data.identity == adopted.data_snapshot().identity);
    CHECK(adopted.record().data.extent == adopted.data_snapshot().size);
    if (expected_count == 1) {
        const auto relation = adopted.reader().read(0);
        CHECK(relation.a == 17);
        CHECK(relation.b == 19);
        CHECK(relation.rational_factors == std::vector<std::uint32_t>{117});
        CHECK(relation.algebraic_factors == std::vector<std::uint32_t>{217});
    }
}

void test_private_handoff_cross_process_adoption(const std::string& executable) {
    TempDirectory temp;
    const auto base = temp.path() / "cross-process-adoption.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto publisher = gnfs::test::run_child_process(
        executable, {"--private-handoff-adoption-child", "publish-exit", base.string()});
    CHECK(publisher.exited);
    CHECK(!publisher.signaled);
    CHECK(publisher.exit_code == 0);
    CHECK(entry_exists_no_follow(paths.lock_path));
    CHECK(entry_exists_no_follow(paths.private_directory));
    const auto expected = capture_protected_private_handoff_bytes(paths);
    check_protected_private_handoff_bytes(paths, expected);

    {
        auto adopted = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(adopted.adopted());
        CHECK(adopted.result.status == OOCCleanupStatus::HandoffPresent);
        CHECK(adopted.state == OOCPrivateHandoffState::Canonical);
        CHECK(adopted.adoption.has_value());
        OOCPrivateHandoffAdoptionReceipt receipt(std::move(*adopted.adoption));
        CHECK(adopted.adoption->spent());
        CHECK(!receipt.spent());
        OOCPrivateHandoffReader reader(std::move(receipt));
        CHECK(receipt.spent());
        check_adopted_private_handoff_reader(reader, 1);

        const auto busy = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(busy.result.status == OOCCleanupStatus::Busy);
        CHECK(!busy.adopted());
        CHECK(!busy.adoption.has_value());
        check_protected_private_handoff_bytes(paths, expected);

        OOCPrivateHandoffReader moved(std::move(reader));
        CHECK(!reader.valid());
        check_adopted_private_handoff_reader(moved, 1);
    }

    {
        auto reopened = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(reopened.adopted());
        OOCPrivateHandoffReader reader(std::move(*reopened.adoption));
        check_adopted_private_handoff_reader(reader, 1);
    }
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::HandoffPresent);
    check_protected_private_handoff_bytes(paths, expected);
}

struct PrivateHandoffAdoptionForkContext final {
    OOCPrivateHandoffAdoptionFaultPoint target =
        OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation;
    pid_t child_process = -1;
    bool invoked = false;
    bool fork_failed = false;
    bool is_child = false;
};

[[nodiscard]] bool fork_during_private_handoff_adoption(OOCPrivateHandoffAdoptionFaultPoint point,
                                                        void* opaque) noexcept {
    auto& context = *static_cast<PrivateHandoffAdoptionForkContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    context.child_process = ::fork();
    if (context.child_process < 0) {
        context.fork_failed = true;
        return true;
    }
    context.is_child = context.child_process == 0;
    return false;
}

void test_private_handoff_adoption_is_process_bound(const std::string& executable) {
    TempDirectory temp;
    const auto base = temp.path() / "process-bound-adoption.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto publisher = gnfs::test::run_child_process(
        executable, {"--private-handoff-adoption-child", "publish-exit", base.string()});
    CHECK(publisher.exited);
    CHECK(!publisher.signaled);
    CHECK(publisher.exit_code == 0);
    const auto expected = capture_protected_private_handoff_bytes(paths);

    const pid_t original_process = ::getpid();
    PrivateHandoffAdoptionForkContext adoption_fork;
    auto adopted = OOCCleanupTransaction::adopt_private_handoff(
        base, OOCPrivateHandoffAdoptionTestHooks{
                  .stop_after = fork_during_private_handoff_adoption,
                  .context = &adoption_fork,
              });
    if (::getpid() != original_process) {
        const bool rejected = adoption_fork.invoked && adoption_fork.is_child &&
                              !adopted.adopted() && !adopted.adoption.has_value() &&
                              adopted.result.status == OOCCleanupStatus::InvalidRequest;
        ::_exit(rejected ? 0 : 89);
    }
    CHECK(adoption_fork.invoked);
    CHECK(!adoption_fork.fork_failed);
    CHECK(!adoption_fork.is_child);
    CHECK(adoption_fork.child_process > 0);
    CHECK(adopted.adopted());
    CHECK(adopted.adoption.has_value());
    int adoption_child_status = 0;
    CHECK(::waitpid(adoption_fork.child_process, &adoption_child_status, 0) ==
          adoption_fork.child_process);
    CHECK(WIFEXITED(adoption_child_status));
    CHECK(WEXITSTATUS(adoption_child_status) == 0);
    OOCPrivateHandoffAdoptionReceipt receipt(std::move(*adopted.adoption));
    CHECK(!receipt.spent());

    const pid_t receipt_child = ::fork();
    CHECK(receipt_child >= 0);
    if (receipt_child == 0) {
        bool rejected = receipt.spent();
        try {
            OOCPrivateHandoffReader forbidden(std::move(receipt));
            (void)forbidden;
            rejected = false;
        } catch (const std::invalid_argument&) {
        } catch (...) {
            rejected = false;
        }
        ::_exit(rejected ? 0 : 90);
    }
    int receipt_status = 0;
    CHECK(::waitpid(receipt_child, &receipt_status, 0) == receipt_child);
    CHECK(WIFEXITED(receipt_status));
    CHECK(WEXITSTATUS(receipt_status) == 0);
    CHECK(!receipt.spent());

    OOCPrivateHandoffReader reader(std::move(receipt));
    check_adopted_private_handoff_reader(reader, 1);
    const pid_t reader_child = ::fork();
    CHECK(reader_child >= 0);
    if (reader_child == 0) {
        bool rejected = !reader.valid();
        try {
            (void)reader.reader();
            rejected = false;
        } catch (const std::logic_error&) {
        } catch (...) {
            rejected = false;
        }
        ::_exit(rejected ? 0 : 91);
    }
    int reader_status = 0;
    CHECK(::waitpid(reader_child, &reader_status, 0) == reader_child);
    CHECK(WIFEXITED(reader_status));
    CHECK(WEXITSTATUS(reader_status) == 0);
    check_adopted_private_handoff_reader(reader, 1);
    check_protected_private_handoff_bytes(paths, expected);
}

void test_private_handoff_adopter_owner_death(const std::string& executable) {
    TempDirectory temp;
    const auto base = temp.path() / "adopter-owner-death.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto publisher = gnfs::test::run_child_process(
        executable, {"--private-handoff-adoption-child", "publish-exit", base.string()});
    CHECK(publisher.exited);
    CHECK(!publisher.signaled);
    CHECK(publisher.exit_code == 0);
    const auto expected = capture_protected_private_handoff_bytes(paths);

    const auto adopter = gnfs::test::run_child_process(
        executable, {"--private-handoff-adoption-child", "adopt-exit", base.string()});
    CHECK(adopter.exited);
    CHECK(!adopter.signaled);
    CHECK(adopter.exit_code == 0);
    check_protected_private_handoff_bytes(paths, expected);

    {
        auto recovered = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(recovered.adopted());
        OOCPrivateHandoffReader reader(std::move(*recovered.adoption));
        check_adopted_private_handoff_reader(reader, 1);
    }
    check_protected_private_handoff_bytes(paths, expected);
}

void test_private_handoff_zero_row_adoption(const std::string& executable) {
    TempDirectory temp;
    const auto base = temp.path() / "zero-row-adoption.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto publisher = gnfs::test::run_child_process(
        executable, {"--private-handoff-adoption-child", "publish-empty-exit", base.string()});
    CHECK(publisher.exited);
    CHECK(!publisher.signaled);
    CHECK(publisher.exit_code == 0);
    const auto expected = capture_protected_private_handoff_bytes(paths);

    {
        auto adopted = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(adopted.adopted());
        OOCPrivateHandoffReader reader(std::move(*adopted.adoption));
        check_adopted_private_handoff_reader(reader, 0);
    }
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::HandoffPresent);
    check_protected_private_handoff_bytes(paths, expected);
}

void test_private_handoff_adoption_publication_prefixes(const std::string& executable) {
    TempDirectory temp;

    {
        const auto base = temp.path() / "pending-prefix-adoption.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto publisher =
            gnfs::test::run_child_process(executable, {"--private-handoff-adoption-child",
                                                       "publish-pending-exit", base.string()});
        CHECK(publisher.exited);
        CHECK(!publisher.signaled);
        CHECK(publisher.exit_code == 0);
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
        const auto pending = read_test_bytes(paths.private_handoff_pending_path);
        const auto index = read_test_bytes(paths.index_path);
        const auto data = read_test_bytes(paths.data_path);
        const auto reserved = read_test_bytes(paths.lease_reserved_path);
        const auto owned = read_test_bytes(paths.lease_owned_path);
        const auto owner = read_test_bytes(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory));
        const auto descriptors_before = count_open_test_descriptors();

        const auto observed = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(observed.result.status == OOCCleanupStatus::RecoveryRequired);
        CHECK(observed.state == OOCPrivateHandoffState::PendingOnly);
        CHECK(!observed.adopted());
        CHECK(!observed.adoption.has_value());
        CHECK(count_open_test_descriptors() == descriptors_before);
        check_test_bytes_preserved(paths.private_handoff_pending_path, pending);
        check_test_bytes_preserved(paths.index_path, index);
        check_test_bytes_preserved(paths.data_path, data);
        check_test_bytes_preserved(paths.lease_reserved_path, reserved);
        check_test_bytes_preserved(paths.lease_owned_path, owned);
        check_test_bytes_preserved(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory),
            owner);
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
    }

    {
        const auto base = temp.path() / "canonical-prefix-adoption.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto publisher =
            gnfs::test::run_child_process(executable, {"--private-handoff-adoption-child",
                                                       "publish-canonical-exit", base.string()});
        CHECK(publisher.exited);
        CHECK(!publisher.signaled);
        CHECK(publisher.exit_code == 0);
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        const auto canonical = read_test_bytes(paths.private_handoff_path);
        const auto index = read_test_bytes(paths.index_path);
        const auto data = read_test_bytes(paths.data_path);
        const auto reserved = read_test_bytes(paths.lease_reserved_path);
        const auto owned = read_test_bytes(paths.lease_owned_path);
        const auto owner = read_test_bytes(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory));
        const bool pending_present = entry_exists_no_follow(paths.private_handoff_pending_path);
        const auto pending = pending_present
                                 ? std::optional<std::vector<std::byte>>(
                                       read_test_bytes(paths.private_handoff_pending_path))
                                 : std::nullopt;

        {
            auto adopted = OOCCleanupTransaction::adopt_private_handoff(base);
            CHECK(adopted.adopted());
            OOCPrivateHandoffReader reader(std::move(*adopted.adoption));
            check_adopted_private_handoff_reader(reader, 1);
        }
        check_test_bytes_preserved(paths.private_handoff_path, canonical);
        check_test_bytes_preserved(paths.index_path, index);
        check_test_bytes_preserved(paths.data_path, data);
        check_test_bytes_preserved(paths.lease_reserved_path, reserved);
        check_test_bytes_preserved(paths.lease_owned_path, owned);
        check_test_bytes_preserved(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory),
            owner);
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path) == pending_present);
        if (pending) {
            check_test_bytes_preserved(paths.private_handoff_pending_path, *pending);
        }
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::HandoffPresent);
    }
}

constexpr std::array PRIVATE_HANDOFF_ADOPTION_FAULT_POINTS{
    OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
    OOCPrivateHandoffAdoptionFaultPoint::IndexInitialValidationComplete,
    OOCPrivateHandoffAdoptionFaultPoint::IndexOpened,
    OOCPrivateHandoffAdoptionFaultPoint::DataInitialValidationComplete,
    OOCPrivateHandoffAdoptionFaultPoint::DataOpened,
    OOCPrivateHandoffAdoptionFaultPoint::BeforeFinalRevalidation,
    OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
};

struct PrivateHandoffAdoptionStopContext final {
    OOCPrivateHandoffAdoptionFaultPoint target =
        OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified;
    bool stopped = false;
};

[[nodiscard]] bool stop_private_handoff_adoption(OOCPrivateHandoffAdoptionFaultPoint point,
                                                 void* opaque) noexcept {
    auto& context = *static_cast<PrivateHandoffAdoptionStopContext*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.stopped = true;
    return true;
}

void test_private_handoff_adoption_interruptions(const std::string& executable) {
    TempDirectory temp;
    for (std::size_t index = 0; index < PRIVATE_HANDOFF_ADOPTION_FAULT_POINTS.size(); ++index) {
        const auto base = temp.path() /
                          ("adoption-interruption-" + std::to_string(index) + ".gnfs-sink-lease") /
                          "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto publisher = gnfs::test::run_child_process(
            executable, {"--private-handoff-adoption-child", "publish-exit", base.string()});
        CHECK(publisher.exited);
        CHECK(!publisher.signaled);
        CHECK(publisher.exit_code == 0);
        const auto expected = capture_protected_private_handoff_bytes(paths);
        const auto descriptors_before = count_open_test_descriptors();

        PrivateHandoffAdoptionStopContext context{
            .target = PRIVATE_HANDOFF_ADOPTION_FAULT_POINTS[index],
        };
        {
            const auto interrupted = OOCCleanupTransaction::adopt_private_handoff(
                base, OOCPrivateHandoffAdoptionTestHooks{
                          .stop_after = stop_private_handoff_adoption,
                          .context = &context,
                      });
            CHECK(context.stopped);
            CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
            CHECK(!interrupted.adopted());
            CHECK(!interrupted.adoption.has_value());
        }
        CHECK(count_open_test_descriptors() == descriptors_before);
        check_protected_private_handoff_bytes(paths, expected);

        {
            auto retried = OOCCleanupTransaction::adopt_private_handoff(base);
            CHECK(retried.adopted());
            OOCPrivateHandoffReader reader(std::move(*retried.adoption));
            check_adopted_private_handoff_reader(reader, 1);
        }
        check_protected_private_handoff_bytes(paths, expected);
    }
}

enum class PrivateHandoffAdoptionMutation : std::uint8_t {
    ReplaceCanonical,
    RemoveCanonical,
    RemoveIndex,
    ReplaceIndex,
    ReplaceData,
    ReplaceOwner,
    ReplaceOwned,
    AddUnknown,
    AddLegacyIntent,
    ReplaceDirectory,
    ReplaceLock,
};

struct PrivateHandoffAdoptionMutationContext final {
    OOCPrivateHandoffAdoptionFaultPoint target =
        OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified;
    PrivateHandoffAdoptionMutation mutation = PrivateHandoffAdoptionMutation::ReplaceCanonical;
    std::filesystem::path path;
    std::filesystem::path saved_path;
    std::filesystem::path replacement_path;
    std::vector<std::byte> bytes;
    bool invoked = false;
    bool succeeded = false;
};

[[nodiscard]] bool mutate_private_handoff_adoption(OOCPrivateHandoffAdoptionFaultPoint point,
                                                   void* opaque) noexcept {
    auto& context = *static_cast<PrivateHandoffAdoptionMutationContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    switch (context.mutation) {
    case PrivateHandoffAdoptionMutation::ReplaceCanonical:
    case PrivateHandoffAdoptionMutation::ReplaceIndex:
    case PrivateHandoffAdoptionMutation::ReplaceData:
    case PrivateHandoffAdoptionMutation::ReplaceOwner:
    case PrivateHandoffAdoptionMutation::ReplaceOwned:
        context.succeeded =
            replace_test_leaf_with_bytes(context.path, context.saved_path, context.bytes);
        break;
    case PrivateHandoffAdoptionMutation::RemoveCanonical:
    case PrivateHandoffAdoptionMutation::RemoveIndex:
        context.succeeded = ::rename(context.path.c_str(), context.saved_path.c_str()) == 0;
        break;
    case PrivateHandoffAdoptionMutation::AddUnknown:
    case PrivateHandoffAdoptionMutation::AddLegacyIntent:
        context.succeeded = create_test_leaf_noexcept(context.path, "adoption attack leaf");
        break;
    case PrivateHandoffAdoptionMutation::ReplaceDirectory:
        context.succeeded = ::rename(context.path.c_str(), context.saved_path.c_str()) == 0 &&
                            ::rename(context.replacement_path.c_str(), context.path.c_str()) == 0;
        break;
    case PrivateHandoffAdoptionMutation::ReplaceLock:
        context.succeeded = replace_test_lock_leaf(context.path, context.saved_path);
        break;
    }
    return false;
}

struct PrivateHandoffAdoptionAttackCase final {
    std::string_view label;
    OOCPrivateHandoffAdoptionFaultPoint point;
    PrivateHandoffAdoptionMutation mutation;
    OOCCleanupStatus expected_status;
};

constexpr std::array PRIVATE_HANDOFF_ADOPTION_ATTACKS{
    PrivateHandoffAdoptionAttackCase{
        .label = "canonical-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceCanonical,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "canonical-missing-before-final",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeFinalRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::RemoveCanonical,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "index-missing-before-open",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::RemoveIndex,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "index-replacement-after-open",
        .point = OOCPrivateHandoffAdoptionFaultPoint::IndexOpened,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceIndex,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "data-replacement-during-open",
        .point = OOCPrivateHandoffAdoptionFaultPoint::DataInitialValidationComplete,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceData,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "data-replacement-after-open",
        .point = OOCPrivateHandoffAdoptionFaultPoint::DataOpened,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceData,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "owner-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceOwner,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "owned-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceOwned,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "unknown-leaf",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::AddUnknown,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "legacy-intent",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::AddLegacyIntent,
        .expected_status = OOCCleanupStatus::NamespaceConflict,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "directory-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::DataOpened,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceDirectory,
        .expected_status = OOCCleanupStatus::NamespaceConflict,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "lock-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeFinalRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceLock,
        .expected_status = OOCCleanupStatus::NamespaceConflict,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "late-canonical-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceCanonical,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "late-index-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceIndex,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "late-owned-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceOwned,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "late-unknown-leaf",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::AddUnknown,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
};

void configure_private_handoff_adoption_attack(const PrivateHandoffAdoptionAttackCase& attack,
                                               const OOCCleanupPaths& paths,
                                               const std::filesystem::path& scratch,
                                               PrivateHandoffAdoptionMutationContext& context) {
    context.target = attack.point;
    context.mutation = attack.mutation;
    context.saved_path = scratch / (std::string(attack.label) + ".saved");
    switch (attack.mutation) {
    case PrivateHandoffAdoptionMutation::ReplaceCanonical:
    case PrivateHandoffAdoptionMutation::RemoveCanonical:
        context.path = paths.private_handoff_path;
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::RemoveIndex:
    case PrivateHandoffAdoptionMutation::ReplaceIndex:
        context.path = paths.index_path;
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::ReplaceData:
        context.path = paths.data_path;
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::ReplaceOwner:
        context.path =
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory);
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::ReplaceOwned:
        context.path = paths.lease_owned_path;
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::AddUnknown:
        context.path = paths.private_directory / "foreign-adoption-leaf";
        break;
    case PrivateHandoffAdoptionMutation::AddLegacyIntent:
        context.path = paths.intent_path;
        break;
    case PrivateHandoffAdoptionMutation::ReplaceDirectory:
        context.path = paths.private_directory;
        context.replacement_path = scratch / (std::string(attack.label) + ".replacement");
        CHECK(::mkdir(context.replacement_path.c_str(), 0700) == 0);
        break;
    case PrivateHandoffAdoptionMutation::ReplaceLock:
        context.path = paths.lock_path;
        break;
    }
}

void check_private_handoff_adoption_attack_preserved(
    const PrivateHandoffAdoptionAttackCase& attack, const OOCCleanupPaths& paths,
    const ProtectedPrivateHandoffBytes& expected,
    const PrivateHandoffAdoptionMutationContext& context) {
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
    if (attack.mutation != PrivateHandoffAdoptionMutation::AddLegacyIntent) {
        CHECK(!entry_exists_no_follow(paths.intent_path));
    }

    if (attack.mutation == PrivateHandoffAdoptionMutation::ReplaceDirectory) {
        CHECK(entry_exists_no_follow(paths.private_directory));
        CHECK(entry_exists_no_follow(context.saved_path));
        check_test_bytes_preserved(context.saved_path / paths.private_handoff_path.filename(),
                                   expected.canonical);
        check_test_bytes_preserved(context.saved_path / paths.index_path.filename(),
                                   expected.index);
        check_test_bytes_preserved(context.saved_path / paths.data_path.filename(), expected.data);
        check_test_bytes_preserved(context.saved_path /
                                       gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(
                                           paths.private_directory)
                                           .filename(),
                                   expected.owner);
        check_test_bytes_preserved(paths.lease_owned_path, expected.owned);
        return;
    }

    if (attack.mutation == PrivateHandoffAdoptionMutation::RemoveCanonical) {
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        check_test_bytes_preserved(context.saved_path, expected.canonical);
    } else {
        check_test_bytes_preserved(paths.private_handoff_path, expected.canonical);
    }
    check_test_bytes_preserved(paths.data_path, expected.data);
    check_test_bytes_preserved(
        gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory),
        expected.owner);
    check_test_bytes_preserved(paths.lease_owned_path, expected.owned);
    if (attack.mutation == PrivateHandoffAdoptionMutation::RemoveIndex) {
        CHECK(!entry_exists_no_follow(paths.index_path));
        check_test_bytes_preserved(context.saved_path, expected.index);
    } else {
        check_test_bytes_preserved(paths.index_path, expected.index);
    }
    if (attack.mutation == PrivateHandoffAdoptionMutation::AddUnknown ||
        attack.mutation == PrivateHandoffAdoptionMutation::AddLegacyIntent) {
        CHECK(entry_exists_no_follow(context.path));
    }
    if (attack.mutation == PrivateHandoffAdoptionMutation::ReplaceLock) {
        CHECK(entry_exists_no_follow(paths.lock_path));
        CHECK(entry_exists_no_follow(context.saved_path));
    }
}

void test_private_handoff_adoption_rejects_namespace_drift(const std::string& executable) {
    TempDirectory temp;
    for (std::size_t index = 0; index < PRIVATE_HANDOFF_ADOPTION_ATTACKS.size(); ++index) {
        const auto& attack = PRIVATE_HANDOFF_ADOPTION_ATTACKS[index];
        const auto base = temp.path() / (std::string(attack.label) + ".gnfs-sink-lease") / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto publisher = gnfs::test::run_child_process(
            executable, {"--private-handoff-adoption-child", "publish-exit", base.string()});
        CHECK(publisher.exited);
        CHECK(!publisher.signaled);
        CHECK(publisher.exit_code == 0);
        const auto expected = capture_protected_private_handoff_bytes(paths);

        PrivateHandoffAdoptionMutationContext context;
        configure_private_handoff_adoption_attack(attack, paths, temp.path(), context);
        const auto descriptors_before = count_open_test_descriptors();
        {
            const auto rejected = OOCCleanupTransaction::adopt_private_handoff(
                base, OOCPrivateHandoffAdoptionTestHooks{
                          .stop_after = mutate_private_handoff_adoption,
                          .context = &context,
                      });
            CHECK(context.invoked);
            CHECK(context.succeeded);
            CHECK(rejected.result.status == attack.expected_status);
            CHECK(!rejected.adopted());
            CHECK(!rejected.adoption.has_value());
        }
        CHECK(count_open_test_descriptors() == descriptors_before);
        check_private_handoff_adoption_attack_preserved(attack, paths, expected, context);
    }
}

struct PrivateHandoffLockReplacementContext final {
    OOCPrivateHandoffFaultPoint target = OOCPrivateHandoffFaultPoint::CanonicalPromoted;
    std::filesystem::path lock_path;
    std::filesystem::path saved_lock_path;
    bool invoked = false;
    bool replaced = false;
};

[[nodiscard]] bool replace_private_handoff_lock(OOCPrivateHandoffFaultPoint point,
                                                void* opaque) noexcept {
    auto& context = *static_cast<PrivateHandoffLockReplacementContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    context.replaced = replace_test_lock_leaf(context.lock_path, context.saved_lock_path);
    return false;
}
#endif

struct CleanupPublishLockReplacementContext final {
    OOCCleanupPublishFaultPoint target = OOCCleanupPublishFaultPoint::IntentPendingDurable;
    std::filesystem::path lock_path;
    std::filesystem::path saved_lock_path;
    bool invoked = false;
    bool replaced = false;
};

[[nodiscard]] bool replace_cleanup_publish_lock(OOCCleanupPublishFaultPoint point,
                                                void* opaque) noexcept {
    auto& context = *static_cast<CleanupPublishLockReplacementContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    context.replaced = replace_test_lock_leaf(context.lock_path, context.saved_lock_path);
    return false;
}

struct PrivateLeaseLockReplacementContext final {
    OOCPrivateLeaseFaultPoint target = OOCPrivateLeaseFaultPoint::FinalRenameDurable;
    std::filesystem::path lock_path;
    std::filesystem::path saved_lock_path;
    bool invoked = false;
    bool replaced = false;
};

[[nodiscard]] bool replace_private_lease_lock(OOCPrivateLeaseFaultPoint point,
                                              void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseLockReplacementContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    context.replaced = replace_test_lock_leaf(context.lock_path, context.saved_lock_path);
    return false;
}

#if defined(__APPLE__)
void create_abandoned_private_handoff_pending(const std::filesystem::path& base,
                                              bool remove_reserved) {
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            auto prepared = prepare_private_handoff(base);
            PrivateHandoffStopContext stop{
                .target = OOCPrivateHandoffFaultPoint::PendingDurable,
            };
            const auto interrupted =
                publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
            if (!stop.stopped || interrupted.result.status != OOCCleanupStatus::Interrupted ||
                interrupted.state != OOCPrivateHandoffState::PendingOnly) {
                ::_exit(82);
            }
            if (remove_reserved) {
                const auto paths = OOCCleanupTransaction::paths_for(base);
                std::error_code error;
                if (!std::filesystem::remove(paths.lease_reserved_path, error) || error) {
                    ::_exit(83);
                }
            }
            ::_exit(0);
        } catch (...) {
            ::_exit(84);
        }
    }

    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

void test_private_handoff_publish_rejects_replaced_held_lock() {
    TempDirectory temp;
    const auto base = temp.path() / "private-handoff-lock-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_lock_path = temp.path() / "saved-private-handoff-lock";
    auto prepared = prepare_private_handoff(base);

    const auto original_lock_bytes = read_test_bytes(paths.lock_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    PrivateHandoffLockReplacementContext replacement{
        .target = OOCPrivateHandoffFaultPoint::CanonicalPromoted,
        .lock_path = paths.lock_path,
        .saved_lock_path = saved_lock_path,
    };

    const auto rejected =
        publish_private_handoff(prepared, OOCPrivateHandoffTestHooks{
                                              .stop_after = replace_private_handoff_lock,
                                              .context = &replacement,
                                          });
    CHECK(replacement.invoked);
    CHECK(replacement.replaced);
    CHECK(rejected.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(rejected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());
    CHECK(entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));

    const auto handoff_bytes = read_test_bytes(paths.private_handoff_path);
    const auto replacement_lock_bytes = read_test_bytes(paths.lock_path);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);

    const auto retried = publish_private_handoff(prepared);
    CHECK(retried.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(retried.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());
    check_test_bytes_preserved(paths.private_handoff_path, handoff_bytes);
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
    check_test_bytes_preserved(paths.private_handoff_path, handoff_bytes);
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
}
#endif

void test_private_cleanup_intent_rejects_replaced_held_lock() {
    TempDirectory temp;
    const auto base = temp.path() / "private-intent-lock-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_lock_path = temp.path() / "saved-private-intent-lock";
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    (void)writer.write(make_real_relation(31, 37));
    const auto original_lock_bytes = read_test_bytes(paths.lock_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    CleanupPublishLockReplacementContext replacement{
        .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
        .lock_path = paths.lock_path,
        .saved_lock_path = saved_lock_path,
    };

    bool rejected = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
            .stop_after = nullptr,
            .stop_after_publish = replace_cleanup_publish_lock,
            .fail_before_operation = nullptr,
            .context = &replacement,
        });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(replacement.invoked);
    CHECK(replacement.replaced);
    CHECK(writer.has_cleanup_ownership_receipt());
    CHECK(!reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));

    const auto replacement_lock_bytes = read_test_bytes(paths.lock_path);
    const auto pending_bytes = read_test_bytes(paths.intent_pending_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).status ==
          OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership->spent());
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.intent_pending_path, pending_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
}

void test_private_lease_reservation_rejects_replaced_held_lock() {
    TempDirectory temp;
    const auto base = temp.path() / "private-reserve-lock-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_lock_path = temp.path() / "saved-private-reserve-lock";
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    PrivateLeaseLockReplacementContext replacement{
        .target = OOCPrivateLeaseFaultPoint::FinalRenameDurable,
        .lock_path = paths.lock_path,
        .saved_lock_path = saved_lock_path,
    };

    const auto reservation = OOCCleanupTransaction::reserve_private_lease(
        base, OOCPrivateLeaseTestHooks{
                  .stop_after = replace_private_lease_lock,
                  .context = &replacement,
              });
    CHECK(replacement.invoked);
    CHECK(replacement.replaced);
    CHECK(reservation.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership.has_value());
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(owner_path));
    CHECK(!entry_exists_no_follow(paths.index_path));
    CHECK(!entry_exists_no_follow(paths.data_path));

    const auto original_lock_bytes = read_test_bytes(saved_lock_path);
    const auto replacement_lock_bytes = read_test_bytes(paths.lock_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::IntentConflict);
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    CHECK(!entry_exists_no_follow(paths.index_path));
    CHECK(!entry_exists_no_follow(paths.data_path));
}

void test_private_lease_activation_rejects_replaced_held_lock() {
    TempDirectory temp;
    const auto base = temp.path() / "private-activate-lock-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_lock_path = temp.path() / "saved-private-activate-lock";
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    const auto original_lock_bytes = read_test_bytes(paths.lock_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    PrivateLeaseLockReplacementContext replacement{
        .target = OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
        .lock_path = paths.lock_path,
        .saved_lock_path = saved_lock_path,
    };

    bool rejected = false;
    try {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCPrivateLeaseTestHooks{
                                     .stop_after = replace_private_lease_lock,
                                     .context = &replacement,
                                 });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(replacement.invoked);
    CHECK(replacement.replaced);
    CHECK(!reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(owner_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto replacement_lock_bytes = read_test_bytes(paths.lock_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    bool retry_rejected = false;
    try {
        OOCRelationWriter retry(base.string(), *reservation.ownership);
    } catch (const std::exception&) {
        retry_rejected = true;
    }
    CHECK(retry_rejected);
    CHECK(!reservation.ownership->spent());
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::IntentConflict);
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
}

#endif

#if defined(__APPLE__)
void test_private_handoff_writer_round_trip() {
    TempDirectory temp;

    for (const std::size_t relation_count : {std::size_t{0}, std::size_t{2}}) {
        const auto base =
            temp.path() /
            ("writer-private-handoff-" + std::to_string(relation_count) + ".gnfs-sink-lease") /
            "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());

        std::optional<OOCSnapshotDescriptor> descriptor;
        {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
            for (std::size_t index = 0; index < relation_count; ++index) {
                (void)writer.write(
                    make_real_relation(23 + static_cast<std::int64_t>(index), 29 + index));
            }
            descriptor = writer.finalize_and_publish_private_handoff(
                PRIVATE_HANDOFF_PAYLOAD_KIND, PRIVATE_HANDOFF_PAYLOAD_VERSION,
                PRIVATE_HANDOFF_PAYLOAD);
        }

        CHECK(descriptor.has_value());
        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.canonical());
        CHECK(inspected.result.status == OOCCleanupStatus::HandoffPresent);
        CHECK(inspected.state == OOCPrivateHandoffState::Canonical);
        CHECK(inspected.record.has_value());
        if (descriptor && inspected.record) {
            CHECK(inspected.record->pair == handoff_pair_descriptor(*descriptor));
            CHECK(inspected.record->payload_kind == PRIVATE_HANDOFF_PAYLOAD_KIND);
            CHECK(inspected.record->payload_version == PRIVATE_HANDOFF_PAYLOAD_VERSION);
            CHECK(inspected.record->opaque_payload.size() == PRIVATE_HANDOFF_PAYLOAD.size());
            CHECK(std::equal(inspected.record->opaque_payload.begin(),
                             inspected.record->opaque_payload.end(),
                             PRIVATE_HANDOFF_PAYLOAD.begin()));
        }
        CHECK(OOCRelationReader(base.string()).count() == relation_count);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));

        const auto retained = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
        CHECK(retained.status == OOCCleanupStatus::HandoffPresent);
        CHECK(!reservation.ownership->spent());
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
    }
}

struct PrivateCleanupPermitReplacementContext final {
    OOCPrivateLeaseFaultPoint target = OOCPrivateLeaseFaultPoint::RecoveryPermitAcquired;
    std::filesystem::path pending_path;
    std::filesystem::path saved_path;
    std::filesystem::path snapshot_root;
    std::vector<std::byte> bytes;
    std::optional<NamespaceTreeSnapshot> expected_after_hook;
    bool invoked = false;
    bool replaced = false;
};

[[nodiscard]] bool
replace_handoff_after_private_cleanup_permit_acquired(OOCPrivateLeaseFaultPoint point,
                                                      void* opaque) noexcept {
    auto& context = *static_cast<PrivateCleanupPermitReplacementContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    context.replaced =
        replace_test_leaf_with_bytes(context.pending_path, context.saved_path, context.bytes);
    if (context.replaced) {
        try {
            context.expected_after_hook = capture_namespace_tree(context.snapshot_root);
        } catch (...) {
            context.expected_after_hook.reset();
        }
    }
    return false;
}

[[nodiscard]] bool replace_handoff_after_legacy_cleanup_permit(OOCCleanupFaultPoint point,
                                                               void* opaque) noexcept {
    auto& context = *static_cast<PrivateCleanupPermitReplacementContext*>(opaque);
    if (context.invoked || point != OOCCleanupFaultPoint::LegacyCleanupPermitAcquired) {
        return false;
    }
    context.invoked = true;
    context.replaced =
        replace_test_leaf_with_bytes(context.pending_path, context.saved_path, context.bytes);
    if (context.replaced) {
        try {
            context.expected_after_hook = capture_namespace_tree(context.snapshot_root);
        } catch (...) {
            context.expected_after_hook.reset();
        }
    }
    return false;
}

void test_legacy_cleanup_handoff_observation_is_terminal() {
    TempDirectory temp;
    for (const bool canonical : {false, true}) {
        const auto label = canonical ? "legacy-terminal-canonical" : "legacy-terminal-pending";
        const auto base = temp.path() / (std::string(label) + ".gnfs-sink-lease") / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        OOCPrivateHandoffState expected_state = OOCPrivateHandoffState::Canonical;
        if (canonical) {
            CHECK(publish_private_handoff(prepared).canonical());
        } else {
            PrivateHandoffStopContext stop{
                .target = OOCPrivateHandoffFaultPoint::PendingDurable,
            };
            const auto pending =
                publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
            CHECK(stop.stopped);
            CHECK(pending.result.status == OOCCleanupStatus::Interrupted);
            expected_state = OOCPrivateHandoffState::PendingOnly;
        }

        std::optional<OOCPrivateLeaseOwnershipReceipt> dropped_lease;
        dropped_lease.emplace(std::move(prepared.lease_ownership));
        dropped_lease.reset();
        const auto before = capture_namespace_tree(temp.path());
        auto lock = std::make_shared<BaseLock>(paths.lock_path);
        auto admission = admit_private_cleanup_action_locked(
            paths, lock, PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(!admission.blocked.has_value());
        CHECK(admission.permit.has_value());
        if (!admission.permit) {
            return;
        }
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        const auto& held_lock =
            begin_private_cleanup_action(permit, paths, PrivateNamespaceAction::RunLegacyCleanup);
        CHECK(inspect_private_handoff_from_permit(permit).state == expected_state);

        PrivateCleanupMutationGate gate(permit);
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, paths, held_lock); }));
        CHECK(rejects_invalid_private_cleanup_permit(
            [&] { authorize_private_cleanup_mutation(gate, paths, held_lock); }));
        CHECK(capture_namespace_tree(temp.path()) == before);
    }
}

void test_legacy_cleanup_permit_rejects_byte_identical_handoff_replacement() {
    TempDirectory temp;
    const auto base = temp.path() / "legacy-permit-handoff-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);
    CHECK(publish_private_handoff(prepared).canonical());

    std::optional<OOCPrivateLeaseOwnershipReceipt> dropped_lease;
    dropped_lease.emplace(std::move(prepared.lease_ownership));
    dropped_lease.reset();
    const auto handoff_bytes = read_test_bytes(paths.private_handoff_path);
    PrivateCleanupPermitReplacementContext context{
        .pending_path = paths.private_handoff_path,
        .saved_path = temp.path() / "saved-legacy-handoff",
        .snapshot_root = temp.path(),
        .bytes = handoff_bytes,
    };
    const auto rejected = OOCCleanupTransaction::resume(
        base, OOCCleanupTestHooks{
                  .stop_after = replace_handoff_after_legacy_cleanup_permit,
                  .stop_after_publish = nullptr,
                  .fail_before_operation = nullptr,
                  .context = &context,
              });

    CHECK(context.invoked);
    CHECK(context.replaced);
    CHECK(context.expected_after_hook.has_value());
    CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(entry_exists_no_follow(context.saved_path));
    CHECK(entry_exists_no_follow(paths.private_handoff_path));
    CHECK(read_test_bytes(context.saved_path) == handoff_bytes);
    CHECK(read_test_bytes(paths.private_handoff_path) == handoff_bytes);
    std::error_code equivalent_error;
    CHECK(!std::filesystem::equivalent(context.saved_path, paths.private_handoff_path,
                                       equivalent_error));
    CHECK(!equivalent_error);
    if (context.expected_after_hook) {
        CHECK(capture_namespace_tree(temp.path()) == *context.expected_after_hook);
    }
}

void test_recovery_permit_rejects_byte_identical_pending_replacement() {
    TempDirectory temp;
    const auto base =
        temp.path() / "recovery-permit-pending-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    create_abandoned_private_handoff_pending(base, false);

    const auto pending_bytes = read_test_bytes(paths.private_handoff_pending_path);
    PrivateCleanupPermitReplacementContext context{
        .pending_path = paths.private_handoff_pending_path,
        .saved_path = temp.path() / "saved-original-handoff-pending",
        .snapshot_root = temp.path(),
        .bytes = pending_bytes,
    };
    const auto recovered = OOCCleanupTransaction::recover_private_lease(
        base, OOCPrivateLeaseTestHooks{
                  .stop_after = replace_handoff_after_private_cleanup_permit_acquired,
                  .context = &context,
              });

    CHECK(context.invoked);
    CHECK(context.replaced);
    CHECK(context.expected_after_hook.has_value());
    CHECK(recovered.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(entry_exists_no_follow(context.saved_path));
    CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(read_test_bytes(context.saved_path) == pending_bytes);
    CHECK(read_test_bytes(paths.private_handoff_pending_path) == pending_bytes);
    std::error_code equivalent_error;
    CHECK(!std::filesystem::equivalent(context.saved_path, paths.private_handoff_pending_path,
                                       equivalent_error));
    CHECK(!equivalent_error);
    if (context.expected_after_hook) {
        CHECK(capture_namespace_tree(temp.path()) == *context.expected_after_hook);
    }
}

void test_removal_permit_rejects_byte_identical_pending_replacement() {
    TempDirectory temp;
    const auto base = temp.path() / "removal-permit-pending-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);
    PrivateHandoffStopContext stop{
        .target = OOCPrivateHandoffFaultPoint::PendingDurable,
    };
    const auto interrupted = publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
    CHECK(stop.stopped);
    CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
    CHECK(interrupted.state == OOCPrivateHandoffState::PendingOnly);

    const auto pending_bytes = read_test_bytes(paths.private_handoff_pending_path);
    PrivateCleanupPermitReplacementContext context{
        .target = OOCPrivateLeaseFaultPoint::RemovalPermitAcquired,
        .pending_path = paths.private_handoff_pending_path,
        .saved_path = temp.path() / "saved-removal-handoff-pending",
        .snapshot_root = temp.path(),
        .bytes = pending_bytes,
    };
    const auto removed = OOCCleanupTransaction::remove_private_lease(
        prepared.lease_ownership,
        OOCPrivateLeaseTestHooks{
            .stop_after = replace_handoff_after_private_cleanup_permit_acquired,
            .context = &context,
        });

    CHECK(context.invoked);
    CHECK(context.replaced);
    CHECK(context.expected_after_hook.has_value());
    CHECK(removed.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(!prepared.lease_ownership.spent());
    CHECK(entry_exists_no_follow(context.saved_path));
    CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(read_test_bytes(context.saved_path) == pending_bytes);
    CHECK(read_test_bytes(paths.private_handoff_pending_path) == pending_bytes);
    std::error_code equivalent_error;
    CHECK(!std::filesystem::equivalent(context.saved_path, paths.private_handoff_pending_path,
                                       equivalent_error));
    CHECK(!equivalent_error);
    if (context.expected_after_hook) {
        CHECK(capture_namespace_tree(temp.path()) == *context.expected_after_hook);
    }
}

void test_private_lease_removal_reconciles_matching_pending_handoff() {
    TempDirectory temp;
    const auto base = temp.path() / "removal-permit-matching-pending.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);
    PrivateHandoffStopContext stop{
        .target = OOCPrivateHandoffFaultPoint::PendingDurable,
    };
    const auto interrupted = publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
    CHECK(stop.stopped);
    CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
    CHECK(interrupted.state == OOCPrivateHandoffState::PendingOnly);
    CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));

    const auto removed = OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership);
    CHECK(removed.completed());
    CHECK(prepared.lease_ownership.spent());
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(paths.lock_path));
}

void test_private_lease_removal_preserves_foreign_lease_pending_siblings() {
    TempDirectory temp;
    constexpr std::array corrupt_pending{
        std::byte{0xba},
        std::byte{0xad},
        std::byte{0xf0},
        std::byte{0x0d},
    };

    enum class Case : std::uint8_t {
        PendingWithOwnedPending,
        PendingWithReservedPending,
        CanonicalDuplicateWithOwnedPending,
    };
    for (const auto test_case : {Case::PendingWithOwnedPending, Case::PendingWithReservedPending,
                                 Case::CanonicalDuplicateWithOwnedPending}) {
        const auto label = test_case == Case::PendingWithOwnedPending
                               ? "removal-foreign-owned-pending"
                               : (test_case == Case::PendingWithReservedPending
                                      ? "removal-foreign-reserved-pending"
                                      : "removal-canonical-duplicate-foreign-owned-pending");
        const auto base = temp.path() / (std::string(label) + ".gnfs-sink-lease") / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);

        if (test_case == Case::CanonicalDuplicateWithOwnedPending) {
            CHECK(publish_private_handoff(prepared).canonical());
            const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
            write_private_control_bytes(paths.private_handoff_pending_path, canonical_bytes);
        } else {
            PrivateHandoffStopContext stop{
                .target = OOCPrivateHandoffFaultPoint::PendingDurable,
            };
            const auto interrupted =
                publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
            CHECK(stop.stopped);
            CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
            CHECK(interrupted.state == OOCPrivateHandoffState::PendingOnly);
        }

        const auto blocker = test_case == Case::PendingWithReservedPending
                                 ? paths.lease_reserved_pending_path
                                 : paths.lease_owned_pending_path;
        write_private_control_bytes(blocker, corrupt_pending);
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
        const auto before = capture_namespace_tree(temp.path());

        const auto rejected = OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership);
        CHECK(rejected.status == OOCCleanupStatus::IntentCorrupt);
        CHECK(!prepared.lease_ownership.spent());
        CHECK(capture_namespace_tree(temp.path()) == before);
    }
}

void test_removal_permit_rejects_byte_identical_owned_pending_replacement() {
    TempDirectory temp;
    const auto base =
        temp.path() / "removal-permit-owned-pending-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);
    PrivateHandoffStopContext stop{
        .target = OOCPrivateHandoffFaultPoint::PendingDurable,
    };
    const auto interrupted = publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
    CHECK(stop.stopped);
    CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
    CHECK(interrupted.state == OOCPrivateHandoffState::PendingOnly);

    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    write_private_control_bytes(paths.lease_owned_pending_path, owned_bytes);
    PrivateCleanupPermitReplacementContext context{
        .target = OOCPrivateLeaseFaultPoint::RemovalPermitAcquired,
        .pending_path = paths.lease_owned_pending_path,
        .saved_path = temp.path() / "saved-removal-owned-pending",
        .snapshot_root = temp.path(),
        .bytes = owned_bytes,
    };
    const auto removed = OOCCleanupTransaction::remove_private_lease(
        prepared.lease_ownership,
        OOCPrivateLeaseTestHooks{
            .stop_after = replace_handoff_after_private_cleanup_permit_acquired,
            .context = &context,
        });

    CHECK(context.invoked);
    CHECK(context.replaced);
    CHECK(context.expected_after_hook.has_value());
    CHECK(removed.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(!prepared.lease_ownership.spent());
    CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(entry_exists_no_follow(context.saved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_pending_path));
    CHECK(read_test_bytes(context.saved_path) == owned_bytes);
    CHECK(read_test_bytes(paths.lease_owned_pending_path) == owned_bytes);
    std::error_code equivalent_error;
    CHECK(!std::filesystem::equivalent(context.saved_path, paths.lease_owned_pending_path,
                                       equivalent_error));
    CHECK(!equivalent_error);
    if (context.expected_after_hook) {
        CHECK(capture_namespace_tree(temp.path()) == *context.expected_after_hook);
    }
}

void test_stale_private_lease_receipt_cannot_mutate_new_handoff_generation(
    const std::string& executable) {
    TempDirectory temp;

    for (const bool canonical_with_duplicate : {false, true}) {
        const auto base =
            temp.path() /
            (std::string(canonical_with_duplicate ? "stale-removal-canonical-duplicate"
                                                  : "stale-removal-pending-only") +
             ".gnfs-sink-lease") /
            "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto stale = prepare_private_handoff(base);
        CHECK(publish_private_handoff(stale).canonical());
        CHECK(!stale.lease_ownership.spent());

        // Model the externally authorized consumer completing generation A:
        // it consumes the canonical handoff and its exact pair, after which
        // ordinary lease recovery removes only A's remaining lease controls.
        for (const auto& path : {paths.private_handoff_path, paths.index_path, paths.data_path}) {
            std::error_code error;
            CHECK(std::filesystem::remove(path, error));
            CHECK(!error);
        }
        CHECK(OOCCleanupTransaction::recover_private_lease(base).completed());
        CHECK(!entry_exists_no_follow(paths.private_directory));
        CHECK(!entry_exists_no_follow(paths.lease_owned_path));

        const auto operation = canonical_with_duplicate ? "publish-exit" : "publish-pending-exit";
        const auto publisher = gnfs::test::run_child_process(
            executable, {"--private-handoff-adoption-child", operation, base.string()});
        CHECK(publisher.exited);
        CHECK(!publisher.signaled);
        CHECK(publisher.exit_code == 0);
        CHECK(entry_exists_no_follow(paths.lease_owned_path));

        if (canonical_with_duplicate) {
            const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
            write_private_control_bytes(paths.private_handoff_pending_path, canonical_bytes);
            CHECK(entry_exists_no_follow(paths.private_handoff_path));
        } else {
            CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        }
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::remove_private_lease(stale.lease_ownership);
        CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(!stale.lease_ownership.spent());
        CHECK(capture_namespace_tree(temp.path()) == before);

        if (!canonical_with_duplicate) {
            CHECK(OOCCleanupTransaction::recover_private_lease(base).completed());
        }
    }
}

void test_private_handoff_pending_only_never_authorizes_cleanup() {
    TempDirectory temp;
    {
        const auto base = temp.path() / "pending-private-handoff.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        create_abandoned_private_handoff_pending(base, false);

        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::RecoveryRequired);
        CHECK(inspected.state == OOCPrivateHandoffState::PendingOnly);
        CHECK(!inspected.canonical());

        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::RecoveryRequired);
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));

        const auto rolled_back = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(rolled_back.completed());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(!entry_exists_no_follow(paths.private_directory));
    }

    {
        const auto base = temp.path() / "pending-private-handoff-begin.gnfs-sink-lease" / "corpus";
        auto prepared = prepare_private_handoff(base);
        PrivateHandoffStopContext stop{
            .target = OOCPrivateHandoffFaultPoint::PendingDurable,
        };
        const auto pending_only =
            publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
        CHECK(stop.stopped);
        CHECK(pending_only.result.status == OOCCleanupStatus::Interrupted);
        CHECK(!prepared.pair_ownership.spent());

        std::optional<OOCPrivateLeaseOwnershipReceipt> dropped_lease;
        dropped_lease.emplace(std::move(prepared.lease_ownership));
        dropped_lease.reset();
        const auto before = capture_namespace_tree(temp.path());
        CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership).status ==
              OOCCleanupStatus::RecoveryRequired);
        CHECK(!prepared.pair_ownership.spent());
        CHECK(capture_namespace_tree(temp.path()) == before);
    }
}

void test_private_handoff_pending_without_reserved_is_preserved() {
    TempDirectory temp;
    const auto base = temp.path() / "pending-without-reserved.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    create_abandoned_private_handoff_pending(base, true);
    const auto pending_bytes = read_test_bytes(paths.private_handoff_pending_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(inspected.state == OOCPrivateHandoffState::PendingOnly);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::RecoveryRequired);
    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::RecoveryRequired);

    CHECK(read_test_bytes(paths.private_handoff_pending_path) == pending_bytes);
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
}

void test_private_handoff_missing_lock_conflicts_before_mutation() {
    TempDirectory temp;
    const auto base = temp.path() / "missing-lock.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    std::error_code error;
    CHECK(std::filesystem::create_directories(paths.private_directory, error));
    CHECK(!error);
    write_private_control_bytes(paths.private_handoff_pending_path, PRIVATE_HANDOFF_PAYLOAD);
    const auto original = read_test_bytes(paths.private_handoff_pending_path);
    CHECK(!entry_exists_no_follow(paths.lock_path));

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NamespaceConflict);
    const auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership.has_value());

    CHECK(!entry_exists_no_follow(paths.lock_path));
    CHECK(read_test_bytes(paths.private_handoff_pending_path) == original);
}

void test_private_handoff_canonical_pending_convergence_and_taint() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "canonical-duplicate-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        write_private_control_bytes(paths.private_handoff_pending_path, canonical_bytes);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.canonical());
        CHECK(inspected.state == OOCPrivateHandoffState::Canonical);
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(read_test_bytes(paths.private_handoff_pending_path) == canonical_bytes);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::HandoffPresent);
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(OOCCleanupTransaction::confirm_pair_namespace_reusable(base).status ==
              OOCCleanupStatus::HandoffPresent);
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::HandoffPresent);
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
    }

    {
        const auto base =
            temp.path() / "canonical-duplicate-pending-remove.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        write_private_control_bytes(paths.private_handoff_pending_path, canonical_bytes);

        CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
              OOCCleanupStatus::HandoffPresent);
        CHECK(!prepared.lease_ownership.spent());
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        check_test_bytes_preserved(paths.private_handoff_path, canonical_bytes);
        check_test_bytes_preserved(paths.index_path, index_bytes);
        check_test_bytes_preserved(paths.data_path, data_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    }

    {
        const auto base = temp.path() / "canonical-corrupt-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        constexpr std::array corrupt{
            std::byte{0xde},
            std::byte{0xad},
            std::byte{0xbe},
            std::byte{0xef},
        };
        write_private_control_bytes(paths.private_handoff_pending_path, corrupt);
        const auto pending_bytes = read_test_bytes(paths.private_handoff_pending_path);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(!inspected.canonical());
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
        CHECK(read_test_bytes(paths.private_handoff_pending_path) == pending_bytes);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
    }

    {
        const auto base = temp.path() / "canonical-foreign-pending.gnfs-sink-lease" / "corpus";
        const auto foreign_base = temp.path() / "foreign-pending-source.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign_paths = OOCCleanupTransaction::paths_for(foreign_base);
        auto prepared = prepare_private_handoff(base);
        auto foreign_prepared = prepare_private_handoff(foreign_base);
        CHECK(publish_private_handoff(prepared).canonical());
        CHECK(publish_private_handoff(foreign_prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        const auto foreign_bytes = read_test_bytes(foreign_paths.private_handoff_path);
        write_private_control_bytes(paths.private_handoff_pending_path, foreign_bytes);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(!inspected.canonical());
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
        CHECK(read_test_bytes(paths.private_handoff_pending_path) == foreign_bytes);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
    }
}

enum class PrivateHandoffCanonicalMutation : std::uint8_t {
    Truncated,
    DigestCorrupt,
    WireVersionMismatch,
    IdentityMismatch,
};

void mutate_private_handoff_canonical(const std::filesystem::path& path,
                                      PrivateHandoffCanonicalMutation mutation) {
    auto bytes = read_test_bytes(path);
    switch (mutation) {
    case PrivateHandoffCanonicalMutation::Truncated:
        if (bytes.empty()) {
            throw std::runtime_error("cannot truncate empty private handoff");
        }
        bytes.pop_back();
        break;
    case PrivateHandoffCanonicalMutation::DigestCorrupt:
        if (bytes.empty()) {
            throw std::runtime_error("cannot corrupt empty private handoff");
        }
        bytes.back() ^= std::byte{0x80};
        break;
    case PrivateHandoffCanonicalMutation::WireVersionMismatch:
        store_u32_le(bytes, 8, gnfs::relation::OOC_PRIVATE_HANDOFF_WIRE_VERSION_V1 + 1U);
        break;
    case PrivateHandoffCanonicalMutation::IdentityMismatch: {
        const auto decoded = gnfs::relation::decode_ooc_private_handoff_record(bytes);
        if (!decoded || !decoded.value) {
            throw std::runtime_error("could not decode private handoff mutation fixture");
        }
        auto record = *decoded.value;
        record.index.identity.first ^= UINT64_C(0x8000000000000000);
        if (!gnfs::relation::seal_ooc_private_handoff_record(record)) {
            throw std::runtime_error("could not seal private handoff mutation fixture");
        }
        bytes = encode_private_handoff_record(record);
        break;
    }
    }
    write_private_control_bytes(path, bytes);
}

void test_private_handoff_canonical_corruption_is_preserved() {
    TempDirectory temp;
    constexpr std::array mutations{
        PrivateHandoffCanonicalMutation::Truncated,
        PrivateHandoffCanonicalMutation::DigestCorrupt,
        PrivateHandoffCanonicalMutation::WireVersionMismatch,
        PrivateHandoffCanonicalMutation::IdentityMismatch,
    };

    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto base = temp.path() /
                          ("canonical-mutation-" + std::to_string(index) + ".gnfs-sink-lease") /
                          "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        mutate_private_handoff_canonical(paths.private_handoff_path, mutations[index]);
        const auto mutated = read_test_bytes(paths.private_handoff_path);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(!inspected.canonical());
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(!prepared.lease_ownership.spent());

        CHECK(read_test_bytes(paths.private_handoff_path) == mutated);
        CHECK(read_test_bytes(paths.index_path) == index_bytes);
        CHECK(read_test_bytes(paths.data_path) == data_bytes);
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    }
}

void test_private_handoff_macos_path_policy_is_fail_closed() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "handoff-mode.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        std::error_code error;
        std::filesystem::permissions(paths.private_handoff_path,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write |
                                         std::filesystem::perms::group_read,
                                     std::filesystem::perm_options::replace, error);
        CHECK(!error);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
    }

    {
        const auto base = temp.path() / "handoff-symlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        const auto foreign = temp.path() / "handoff-symlink-target";
        write_private_control_bytes(foreign, canonical_bytes);
        std::error_code error;
        CHECK(std::filesystem::remove(paths.private_handoff_path, error));
        CHECK(!error);
        if (create_symlink_or_explicit_skip(foreign, paths.private_handoff_path,
                                            "private handoff canonical symlink")) {
            const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
            CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
            CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
            CHECK(entry_is_symlink_no_follow(paths.private_handoff_path));
            CHECK(read_test_bytes(foreign) == canonical_bytes);
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
        }
    }
}

void test_private_handoff_mixed_with_legacy_authority_is_preserved() {
    TempDirectory temp;
    const auto base = temp.path() / "mixed-generic-and-legacy.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);

    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const auto published = publish_private_handoff(prepared);
        ::_exit(published.canonical() ? 0 : 81);
    }

    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());
    CHECK(entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));

    const auto legacy = gnfs::relation::ooc_cleanup_detail::capture_source_pair(
        paths, prepared.descriptor.store_id);
    const auto intent_bytes = gnfs::relation::ooc_cleanup_detail::serialize_marker(
        legacy, gnfs::relation::ooc_cleanup_detail::INTENT_MAGIC);
    const auto staged_bytes = gnfs::relation::ooc_cleanup_detail::serialize_marker(
        legacy, gnfs::relation::ooc_cleanup_detail::STAGED_MAGIC);
    write_private_control_bytes(paths.intent_path, intent_bytes);
    write_private_control_bytes(paths.staged_path, staged_bytes);

    const auto generic_bytes = read_test_bytes(paths.private_handoff_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);

    CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
          OOCCleanupStatus::NamespaceConflict);
    CHECK(!prepared.lease_ownership.spent());
    std::optional<OOCPrivateLeaseOwnershipReceipt> stale_lease;
    stale_lease.emplace(std::move(prepared.lease_ownership));
    stale_lease.reset();

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(!inspected.canonical());
    CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership).status ==
          OOCCleanupStatus::NamespaceConflict);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NamespaceConflict);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NamespaceConflict);

    CHECK(read_test_bytes(paths.private_handoff_path) == generic_bytes);
    CHECK(read_test_bytes(paths.intent_path) == intent_bytes);
    CHECK(read_test_bytes(paths.staged_path) == staged_bytes);
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(read_test_bytes(paths.lease_owned_path) == owned_bytes);
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
}

void test_private_handoff_corruption_precedes_legacy_authority() {
    TempDirectory temp;
    const auto base = temp.path() / "corrupt-generic-and-legacy.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const auto published = publish_private_handoff(prepared);
        ::_exit(published.canonical() ? 0 : 82);
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());

    const auto legacy = gnfs::relation::ooc_cleanup_detail::capture_source_pair(
        paths, prepared.descriptor.store_id);
    write_private_control_bytes(paths.intent_path,
                                gnfs::relation::ooc_cleanup_detail::serialize_marker(
                                    legacy, gnfs::relation::ooc_cleanup_detail::INTENT_MAGIC));
    write_private_control_bytes(paths.staged_path,
                                gnfs::relation::ooc_cleanup_detail::serialize_marker(
                                    legacy, gnfs::relation::ooc_cleanup_detail::STAGED_MAGIC));
    flip_last_byte(paths.private_handoff_path);
    const auto before = capture_namespace_tree(temp.path());

    CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
          OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(!prepared.lease_ownership.spent());
    std::optional<OOCPrivateLeaseOwnershipReceipt> stale_lease;
    stale_lease.emplace(std::move(prepared.lease_ownership));
    stale_lease.reset();

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(!inspected.canonical());

    CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership).status ==
          OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(OOCCleanupTransaction::resume(base).status ==
          OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(capture_namespace_tree(temp.path()) == before);
}

void test_private_authority_union_handoff_precedence_is_zero_mutation() {
    const auto v2_staged =
        authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2::staged);
    const auto unsupported = std::make_error_code(std::errc::operation_not_supported);
    const auto release_lease = [](PreparedPrivateHandoff& prepared) {
        std::optional<OOCPrivateLeaseOwnershipReceipt> dropped;
        dropped.emplace(std::move(prepared.lease_ownership));
        dropped.reset();
    };

    {
        TempDirectory temp;
        const auto base = temp.path() / "foreign-handoff-before-v2.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        flip_last_byte(paths.private_handoff_path);
        write_private_control_bytes(paths.staged_pending_path, v2_staged);
        release_lease(prepared);

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "exact-handoff-with-v2.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        write_private_control_bytes(paths.staged_pending_path, v2_staged);
        release_lease(prepared);

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(rejected.native_error == unsupported);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "exact-handoff-with-v1.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto legacy = gnfs::relation::ooc_cleanup_detail::capture_source_pair(
            paths, prepared.descriptor.store_id);
        write_private_control_bytes(paths.intent_path,
                                    gnfs::relation::ooc_cleanup_detail::serialize_marker(
                                        legacy, gnfs::relation::ooc_cleanup_detail::INTENT_MAGIC));
        release_lease(prepared);

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::NamespaceConflict);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "exact-handoff-v2-remove-order.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        write_private_control_bytes(paths.private_handoff_pending_path,
                                    read_test_bytes(paths.private_handoff_path));
        write_private_control_bytes(paths.staged_pending_path, v2_staged);

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership);
        CHECK(rejected.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(rejected.native_error == unsupported);
        CHECK(!prepared.lease_ownership.spent());
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "exact-handoff-v2-recover-order.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        write_private_control_bytes(paths.private_handoff_pending_path,
                                    read_test_bytes(paths.private_handoff_path));
        write_private_control_bytes(paths.staged_pending_path, v2_staged);
        release_lease(prepared);

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(rejected.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(rejected.native_error == unsupported);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }
}

void test_private_handoff_canonical_blocks_stale_pair_receipt() {
    TempDirectory temp;
    const auto base = temp.path() / "canonical-stale-pair.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);

    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const auto published = publish_private_handoff(prepared);
        ::_exit(published.canonical() ? 0 : 85);
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());

    std::optional<OOCPrivateLeaseOwnershipReceipt> stale_lease;
    stale_lease.emplace(std::move(prepared.lease_ownership));
    stale_lease.reset();

    const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.canonical());
    CHECK(inspected.result.status == OOCCleanupStatus::HandoffPresent);
    CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership).status ==
          OOCCleanupStatus::HandoffPresent);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::HandoffPresent);

    write_private_control_bytes(paths.private_handoff_pending_path, canonical_bytes);
    const auto duplicate_before = capture_namespace_tree(temp.path());
    CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership).status ==
          OOCCleanupStatus::HandoffPresent);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::HandoffPresent);
    CHECK(capture_namespace_tree(temp.path()) == duplicate_before);

    const auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.result.status == OOCCleanupStatus::HandoffPresent);
    CHECK(!reservation.ownership.has_value());

    CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
}

constexpr int PRIVATE_HANDOFF_CRASH_EXIT_BASE = 180;

struct PrivateHandoffCrashContext final {
    OOCPrivateHandoffFaultPoint target = OOCPrivateHandoffFaultPoint::PendingDurable;
};

[[nodiscard]] bool crash_at_private_handoff(OOCPrivateHandoffFaultPoint point,
                                            void* opaque) noexcept {
    const auto& context = *static_cast<PrivateHandoffCrashContext*>(opaque);
    if (point == context.target) {
        ::_exit(PRIVATE_HANDOFF_CRASH_EXIT_BASE + static_cast<int>(point));
    }
    return false;
}

void test_private_handoff_process_crash_and_cow_retry() {
    TempDirectory temp;

    for (std::size_t index = 0; index < PRIVATE_HANDOFF_FAULT_POINTS.size(); ++index) {
        const auto point = PRIVATE_HANDOFF_FAULT_POINTS[index];
        const auto base = temp.path() /
                          ("private-handoff-crash-" + std::to_string(index) + ".gnfs-sink-lease") /
                          "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);

        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            PrivateHandoffCrashContext context{.target = point};
            (void)publish_private_handoff(prepared, OOCPrivateHandoffTestHooks{
                                                        .stop_after = crash_at_private_handoff,
                                                        .context = &context,
                                                    });
            ::_exit(79);
        }

        int status = 0;
        CHECK(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == PRIVATE_HANDOFF_CRASH_EXIT_BASE + static_cast<int>(point));
        CHECK(!prepared.pair_ownership.spent());
        CHECK(!prepared.lease_ownership.spent());
        CHECK(read_test_bytes(paths.index_path) == index_bytes);
        CHECK(read_test_bytes(paths.data_path) == data_bytes);
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));

        if (point == OOCPrivateHandoffFaultPoint::PendingDurable) {
            CHECK(!entry_exists_no_follow(paths.private_handoff_path));
            CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
            CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        } else {
            CHECK(entry_exists_no_follow(paths.private_handoff_path));
            CHECK(entry_exists_no_follow(paths.lease_reserved_path) ==
                  (point != OOCPrivateHandoffFaultPoint::ReservedRevokedDurable));

            const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
            bool stale_writer_rejected = false;
            try {
                OOCRelationWriter stale_writer(base.string(), prepared.lease_ownership);
            } catch (const std::system_error&) {
                stale_writer_rejected = true;
            }
            CHECK(stale_writer_rejected);
            CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
            CHECK(read_test_bytes(paths.index_path) == index_bytes);
            CHECK(read_test_bytes(paths.data_path) == data_bytes);

            CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
                  OOCCleanupStatus::HandoffPresent);
            CHECK(!prepared.lease_ownership.spent());
            CHECK(read_test_bytes(paths.index_path) == index_bytes);
            CHECK(read_test_bytes(paths.data_path) == data_bytes);
            CHECK(entry_exists_no_follow(paths.private_handoff_path));
            CHECK(!entry_exists_no_follow(paths.intent_path));
            CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        }

        const auto retried = publish_private_handoff(prepared);
        CHECK(retried.canonical());
        CHECK(retried.result.status == OOCCleanupStatus::HandoffPresent);
        CHECK(prepared.pair_ownership.spent());
        CHECK(!prepared.lease_ownership.spent());
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(read_test_bytes(paths.index_path) == index_bytes);
        CHECK(read_test_bytes(paths.data_path) == data_bytes);
        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.canonical());
        CHECK(inspected.result.status == OOCCleanupStatus::HandoffPresent);
        CHECK(inspected.state == OOCPrivateHandoffState::Canonical);
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::HandoffPresent);
    }
}
#endif

#if !defined(__APPLE__)
void test_private_handoff_limited_platform_policy() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "limited-legacy-no-generic.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).completed());
        CHECK(prepared.lease_ownership.spent());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
    }

    for (const bool canonical : {false, true}) {
        const auto base =
            temp.path() /
            (std::string(canonical ? "limited-generic-canonical" : "limited-generic-pending") +
             ".gnfs-sink-lease") /
            "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        const auto generic_path =
            canonical ? paths.private_handoff_path : paths.private_handoff_pending_path;
        write_private_control_bytes(generic_path, PRIVATE_HANDOFF_PAYLOAD);

        const auto generic_bytes = read_test_bytes(generic_path);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);

        CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
              OOCCleanupStatus::PlatformUnsupported);
        CHECK(!prepared.lease_ownership.spent());
        std::optional<OOCPrivateLeaseOwnershipReceipt> stale_lease;
        stale_lease.emplace(std::move(prepared.lease_ownership));
        stale_lease.reset();

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership).status ==
              OOCCleanupStatus::PlatformUnsupported);
        CHECK(!prepared.pair_ownership.spent());
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::PlatformUnsupported);
        const auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.result.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(!reservation.ownership.has_value());

        CHECK(read_test_bytes(generic_path) == generic_bytes);
        CHECK(read_test_bytes(paths.index_path) == index_bytes);
        CHECK(read_test_bytes(paths.data_path) == data_bytes);
        CHECK(read_test_bytes(paths.lease_reserved_path) == reserved_bytes);
        CHECK(read_test_bytes(paths.lease_owned_path) == owned_bytes);
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_data_path));

        std::error_code error;
        CHECK(std::filesystem::remove(generic_path, error));
        CHECK(!error);
        CHECK(OOCCleanupTransaction::recover_private_lease(base).completed());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
    }
}
#endif

#if !defined(__APPLE__)
void test_private_handoff_limited_platform_cleanup_precedence() {
    {
        TempDirectory temp;
        const auto base = temp.path() / "limited-handoff-marker-corrupt.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        flip_last_byte(paths.intent_path);
        write_private_control_bytes(paths.private_handoff_path, PRIVATE_HANDOFF_PAYLOAD);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::IntentCorrupt);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base =
            temp.path() / "limited-handoff-cleanup-foreign.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        const std::vector<std::byte> foreign_pending(
            gnfs::relation::ooc_cleanup_detail::MARKER_BYTES + 1, static_cast<std::byte>('X'));
        write_private_control_bytes(paths.staged_pending_path, foreign_pending);
        write_private_control_bytes(paths.private_handoff_path, PRIVATE_HANDOFF_PAYLOAD);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(capture_namespace_tree(temp.path()) == before);
    }

    {
        TempDirectory temp;
        const auto base = temp.path() / "limited-handoff-with-v2.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = prepare_private_legacy_cleanup_intent(base);
        write_private_control_bytes(
            paths.staged_pending_path,
            authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2::staged));
        write_private_control_bytes(paths.private_handoff_path, PRIVATE_HANDOFF_PAYLOAD);
        reservation.ownership.reset();

        const auto before = capture_namespace_tree(temp.path());
        const auto rejected = OOCCleanupTransaction::resume(base);
        CHECK(rejected.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(rejected.stage == OOCCleanupStage::None);
        CHECK(rejected.native_error == std::make_error_code(std::errc::operation_not_supported));
        CHECK(capture_namespace_tree(temp.path()) == before);
    }
}
#endif

#ifndef _WIN32
void test_fork_copy_cannot_remove_or_unlock_parent_lease() {
    TempDirectory temp;
    const auto base = temp.path() / "fork-lock.gnfs-sink-lease" / "corpus";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());

    int ready_pipe[2]{-1, -1};
    int release_pipe[2]{-1, -1};
    CHECK(::pipe(ready_pipe) == 0);
    CHECK(::pipe(release_pipe) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        const auto forbidden = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
        const char result = forbidden.status == OOCCleanupStatus::InvalidRequest ? '1' : '0';
        (void)::write(ready_pipe[1], &result, 1);
        char release = 0;
        (void)::read(release_pipe[0], &release, 1);
        ::_exit(result == '1' && release == 'x' ? 0 : 72);
    }

    (void)::close(ready_pipe[1]);
    (void)::close(release_pipe[0]);
    char child_result = 0;
    CHECK(::read(ready_pipe[0], &child_result, 1) == 1);
    CHECK(child_result == '1');

    // Dropping the parent's COW receipt closes only its descriptor. The child
    // still holds the inherited open-file description, so path recovery must
    // remain Busy until that child exits.
    reservation.ownership.reset();
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status == OOCCleanupStatus::Busy);

    const char release = 'x';
    CHECK(::write(release_pipe[1], &release, 1) == 1);
    (void)::close(ready_pipe[0]);
    (void)::close(release_pipe[1]);
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).completed());
}
#endif

void test_private_lease_recovery_preserves_unknown_owner_leaf() {
    TempDirectory temp;
    const auto base = temp.path() / "unknown-owner-leaf.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    {
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
    }
    const auto foreign = paths.private_directory / "foreign-control-leaf";
    write_test_leaf(foreign, "foreign");

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(foreign));
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
}

void test_private_lease_marker_attacks_fail_closed() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "owned-corrupt.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
        }
        flip_last_byte(paths.lease_owned_path);
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::IntentCorrupt);
        CHECK(entry_exists_no_follow(paths.private_directory));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
    }

    {
        const auto base = temp.path() / "owner-hardlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
        }
        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto alias = temp.path() / "owner-hardlink-alias";
        if (create_hard_link_checked(owner_path, alias)) {
            const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
            CHECK(recovered.status == OOCCleanupStatus::ForeignReplacementPreserved);
            CHECK(entry_exists_no_follow(paths.private_directory));
            CHECK(entry_exists_no_follow(owner_path));
            CHECK(entry_exists_no_follow(alias));
            check_entries_equivalent(owner_path, alias);
        }
    }

    {
        const auto base = temp.path() / "owned-symlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
        }
        const auto saved = temp.path() / "saved-owned-marker";
        std::error_code error;
        std::filesystem::rename(paths.lease_owned_path, saved, error);
        CHECK(!error);
        if (create_symlink_or_explicit_skip(saved, paths.lease_owned_path,
                                            "private lease owned marker symlink")) {
            CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
                  OOCCleanupStatus::IntentCorrupt);
            CHECK(entry_exists_no_follow(paths.private_directory));
            CHECK(entry_is_symlink_no_follow(paths.lease_owned_path));
            CHECK(entry_exists_no_follow(saved));
        }
    }
}

void test_private_lease_recovery_rejects_directory_aba() {
    TempDirectory temp;
    const auto base = temp.path() / "directory-aba.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_owned = temp.path() / "saved-owned-directory";
    const auto replacement = temp.path() / "replacement-directory";
    const auto sentinel = replacement / "sentinel";

    {
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
    }
    CHECK(std::filesystem::create_directory(replacement));
    write_test_leaf(sentinel, "foreign replacement");

    std::error_code error;
    std::filesystem::rename(paths.private_directory, saved_owned, error);
    CHECK(!error);
    error.clear();
    std::filesystem::rename(replacement, paths.private_directory, error);
    CHECK(!error);

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.private_directory / "sentinel"));
    CHECK(entry_exists_no_follow(saved_owned));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
}

void test_private_lease_recovery_rejects_marker_replacement() {
    TempDirectory temp;
    const auto left_root = temp.path() / "left";
    const auto right_root = temp.path() / "right";
    CHECK(std::filesystem::create_directory(left_root));
    CHECK(std::filesystem::create_directory(right_root));
    const auto left_base = left_root / "shared.gnfs-sink-lease" / "corpus";
    const auto right_base = right_root / "shared.gnfs-sink-lease" / "corpus";
    const auto left_paths = OOCCleanupTransaction::paths_for(left_base);
    const auto right_paths = OOCCleanupTransaction::paths_for(right_base);

    {
        auto left = OOCCleanupTransaction::reserve_private_lease(left_base);
        auto right = OOCCleanupTransaction::reserve_private_lease(right_base);
        CHECK(left.completed());
        CHECK(right.completed());
    }
    CHECK(entry_exists_no_follow(left_paths.lease_owned_path));
    CHECK(entry_exists_no_follow(right_paths.lease_owned_path));

    const auto saved_marker = temp.path() / "saved-left-owned-marker";
    std::error_code error;
    std::filesystem::rename(left_paths.lease_owned_path, saved_marker, error);
    CHECK(!error);
    error.clear();
    std::filesystem::copy_file(right_paths.lease_owned_path, left_paths.lease_owned_path,
                               std::filesystem::copy_options::none, error);
    CHECK(!error);

    const auto recovered = OOCCleanupTransaction::recover_private_lease(left_base);
    CHECK(recovered.status == OOCCleanupStatus::IntentConflict);
    CHECK(entry_exists_no_follow(left_paths.private_directory));
    CHECK(entry_exists_no_follow(left_paths.lease_owned_path));
    CHECK(entry_exists_no_follow(saved_marker));
    CHECK(entry_exists_no_follow(right_paths.private_directory));
}

struct CrashContext final {
    OOCCleanupFaultPoint target = OOCCleanupFaultPoint::IntentDurable;
};

[[nodiscard]] bool crash_at(OOCCleanupFaultPoint point, void* opaque) noexcept {
    const auto& context = *static_cast<const CrashContext*>(opaque);
    if (point == context.target) {
        std::_Exit(100 + static_cast<int>(point));
    }
    return false;
}

[[nodiscard]] PairShape finalized_crash_shape() {
    return PairShape{
        .magic = OOCRelationStoreFormat::MAGIC_V3_FINAL,
        .count = 2,
        .index_size = OOCRelationStoreFormat::INDEX_HEADER_BYTES + 3 * sizeof(std::uint64_t),
        .data_size = OOCRelationStoreFormat::DATA_HEADER_BYTES + 48,
    };
}

int run_crash_child(std::size_t point_index, const std::filesystem::path& base,
                    std::uint64_t store_id) {
    if (point_index >= CLEANUP_FAULT_POINTS.size() || store_id == 0) {
        return 64;
    }
    const PairShape shape = finalized_crash_shape();
    write_pair(base, store_id, shape);
    const OOCCleanupRequest request{
        .base_path = base,
        .store_id = store_id,
        .exact = exact_for(shape),
    };
    CrashContext context{.target = CLEANUP_FAULT_POINTS[point_index]};
    const auto result = begin_cleanup(request, OOCCleanupTestHooks{
                                                   .stop_after = crash_at,
                                                   .stop_after_publish = nullptr,
                                                   .fail_before_operation = nullptr,
                                                   .context = &context,
                                               });
    (void)result;
    return 65;
}

void test_process_crash_recovery(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t initial_store_id = 0x8888'9999'aaaa'bbb0ULL;

    for (std::size_t index = 0; index < CLEANUP_FAULT_POINTS.size(); ++index) {
        const auto base = temp.path() / ("process-crash-" + std::to_string(index));
        const std::uint64_t store_id = initial_store_id + index;
        const auto child =
            gnfs::test::run_child_process(executable, {"--crash-cleanup", std::to_string(index),
                                                       base.string(), std::to_string(store_id)});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code == 100 + static_cast<int>(CLEANUP_FAULT_POINTS[index]));

        const auto paths = OOCCleanupTransaction::paths_for(base);
        check_fault_namespace(paths, CLEANUP_FAULT_POINTS[index]);
        CHECK(OOCCleanupTransaction::resume(base).completed());
        check_cleanup_complete(paths);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NoTransaction);

        const std::uint64_t replacement_store_id = store_id + 0x1000;
        write_pair(base, replacement_store_id);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NoTransaction);
        CHECK(exists(paths.index_path));
        CHECK(exists(paths.data_path));
        CHECK(begin_cleanup(base, replacement_store_id).completed());
    }
}

void run_core_suite(const std::string& executable) {
    test_fault_point_recovery();
    test_receipt_authority_and_pending_publication();
    test_namespace_operation_failures_are_retryable();
    test_reserved_cleanup_suffix_is_rejected();
    test_fresh_writer_rejects_nonempty_cleanup_namespace();
    test_windows_sharing_violation_is_retryable();
    test_exact_finalized_expectation();
    test_real_finalized_store_cleanup();
    test_marker_corruption_is_fail_closed();
    test_authorized_v2_markers_are_not_legacy_cleanup_authority();
    test_platform_limited_handoff_leaf_metadata_observer();
    test_private_authority_union_preflight_is_zero_mutation();
    test_absence_before_staged_has_no_delete_authority();
    test_reverse_pre_staged_state_is_rejected();
    test_source_link_attacks_are_fail_closed();
    test_quarantine_link_attacks_are_fail_closed();
    test_intent_link_attacks_are_fail_closed();
    test_lock_link_attacks_are_fail_closed();
    test_foreign_replacements_are_preserved();
    test_index_count_drift_is_preserved();
    test_staged_only_tail_has_no_delete_authority();
    test_quarantine_collision_is_preserved();
    test_cross_process_lock_reports_busy(executable);
    test_private_lease_uses_one_persistent_external_lock(executable);
    test_private_lease_receipt_rejects_replacement_directory();
#if !defined(__APPLE__)
    test_private_handoff_unsupported_adoption_is_non_observing();
    test_private_handoff_limited_platform_cleanup_precedence();
#endif
}

void run_authority_union_suite() {
    test_private_lease_reservation_protocol_order_and_interruptions();
    test_private_cleanup_union_policy_exhaustive();
}

void run_authority_observer_suite() {
    test_private_cleanup_action_permit_runtime_guards();
    test_private_cleanup_union_observer_baseline_and_exact_names();
#if defined(__APPLE__)
    test_private_cleanup_union_same_handle_inventory();
    test_private_cleanup_union_handoff_slots_are_independent();
#else
    test_private_cleanup_union_limited_platform_rejects_hooks();
#endif
}

void run_private_lease_crash_suite(const std::string& executable) {
    test_private_lease_process_crash_recovery(executable);
    test_private_lease_preactive_rollback_crash_recovery(executable);
    test_private_writer_preactivation_crash_recovery(executable);
    test_private_lease_preactive_link_attacks_are_preserved();
    test_private_lease_recovery_preserves_live_pair_without_intent(executable);
    test_private_lease_recovery_preserves_pending_only_pair(executable);
    test_private_lease_writer_activation_closes_reservation();
    test_private_lease_recovery_finishes_canonical_pair_intent(executable);
    test_deferred_publication_permit_interrupt_and_retry();
    test_deferred_publication_permit_revalidates_handoff_insertion();
    test_deferred_publication_claim_blocks_nested_actions();
#ifndef _WIN32
    test_deferred_publication_fork_copy_is_closed_by_retained_witness();
#endif
    test_deferred_publication_pending_phase_and_receipt_escrow();
    test_deferred_publication_phase_gate_rejects_allowed_slot_and_inode_drift();
    test_deferred_publication_destination_exists_commits_exact_canonical();
    test_deferred_publication_canonical_commit_survives_pending_cleanup_failures();
    test_deferred_publication_pending_unlink_rejects_hook_replacement();
    test_deferred_publication_commits_receipt_before_canonical_hook();
    test_deferred_private_writer_handoff_and_pending_recovery();
    test_deferred_handoff_foreign_leaf_blocks_pair_mutation();
    test_private_handoff_writer_rejects_metadata_before_finalize();
    test_private_handoff_transaction_rejects_oversize_before_mutation();
    test_private_handoff_missing_lock_orphan_stage_is_preserved();
    test_private_handoff_invalid_orphan_stage_names_are_ignored();
    test_private_lease_unknown_child_preserves_matching_pending();
    test_private_fresh_writer_authorized_gate_rejects_same_inode_size_drift();
    test_private_lease_unknown_scan_precedes_writer_mutation();
    test_unscoped_writer_rejects_existing_preactive_private_lease();
    test_private_lease_unknown_scan_precedes_legacy_intent_publication();
    test_private_lease_activation_interrupt_after_commit_preserves_pair();
    test_private_lease_activation_post_sync_failure_preserves_pair();
    test_legacy_cleanup_permit_interrupt_is_non_mutating();
    test_legacy_cleanup_permit_revalidates_new_handoff();
    test_legacy_cleanup_mutation_gate_runs_after_operation_hook();
    test_legacy_cleanup_mutation_gate_covers_resumed_tails();
    test_legacy_cleanup_empty_terminal_consumes_private_receipt();
    test_legacy_cleanup_marker_rename_failure_is_retryable();
    test_legacy_cleanup_mutation_gate_defers_for_exact_pending();
    test_private_lease_reservation_permit_interrupt_is_non_mutating_and_retryable();
    test_private_fresh_writer_permit_interrupt_is_non_mutating_and_retryable();
    test_private_lease_activation_permit_interrupt_preserves_pair_for_recovery();
    test_private_lease_recovery_permit_interrupt_is_non_mutating(executable);
    test_private_lease_recovery_permit_revalidates_new_handoff(executable);
    test_private_lease_removal_permit_interrupt_is_non_mutating();
    test_private_lease_removal_permit_revalidates_new_handoff();
    test_private_lease_removal_preserves_corrupt_cleanup_marker();
    test_private_lease_removal_admission_projects_union_blocker_first();
#if !defined(__APPLE__)
    test_private_handoff_unsupported_publish_is_non_mutating();
#endif
#if !defined(_WIN32)
    test_private_cleanup_intent_rejects_replaced_held_lock();
    test_private_lease_reservation_rejects_replaced_held_lock();
    test_private_lease_activation_rejects_replaced_held_lock();
#endif
#if defined(__APPLE__)
    test_deferred_publication_revalidates_lease_generation();
    test_private_handoff_cross_process_adoption(executable);
    test_private_handoff_adoption_is_process_bound(executable);
    test_private_handoff_adopter_owner_death(executable);
    test_private_handoff_zero_row_adoption(executable);
    test_private_handoff_adoption_publication_prefixes(executable);
    test_private_handoff_adoption_interruptions(executable);
    test_private_handoff_adoption_rejects_namespace_drift(executable);
    test_private_handoff_publish_rejects_replaced_held_lock();
    test_private_handoff_writer_round_trip();
    test_legacy_cleanup_handoff_observation_is_terminal();
    test_legacy_cleanup_permit_rejects_byte_identical_handoff_replacement();
    test_recovery_permit_rejects_byte_identical_pending_replacement();
    test_removal_permit_rejects_byte_identical_pending_replacement();
    test_private_lease_removal_reconciles_matching_pending_handoff();
    test_private_lease_removal_preserves_foreign_lease_pending_siblings();
    test_removal_permit_rejects_byte_identical_owned_pending_replacement();
    test_stale_private_lease_receipt_cannot_mutate_new_handoff_generation(executable);
    test_private_handoff_pending_only_never_authorizes_cleanup();
    test_private_handoff_pending_without_reserved_is_preserved();
    test_private_handoff_missing_lock_conflicts_before_mutation();
    test_private_handoff_canonical_pending_convergence_and_taint();
    test_private_handoff_canonical_corruption_is_preserved();
    test_private_handoff_macos_path_policy_is_fail_closed();
    test_private_handoff_mixed_with_legacy_authority_is_preserved();
    test_private_handoff_corruption_precedes_legacy_authority();
    test_private_authority_union_handoff_precedence_is_zero_mutation();
    test_private_handoff_canonical_blocks_stale_pair_receipt();
    test_private_handoff_process_crash_and_cow_retry();
#endif
#if !defined(__APPLE__)
    test_private_handoff_limited_platform_policy();
#endif
#ifndef _WIN32
    test_fork_copy_cannot_remove_or_unlock_parent_lease();
#endif
    test_private_lease_recovery_preserves_unknown_owner_leaf();
    test_private_lease_marker_attacks_fail_closed();
    test_private_lease_recovery_rejects_directory_aba();
    test_private_lease_recovery_rejects_marker_replacement();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 4 && std::string_view(argv[1]) == "--private-handoff-adoption-child") {
        return run_private_handoff_adoption_child(std::string_view(argv[2]),
                                                  std::filesystem::path(argv[3]));
    }
    if (argc == 4 && std::string_view(argv[1]) == "--contend-cleanup-lock") {
        try {
            const auto store_id = static_cast<std::uint64_t>(std::stoull(argv[3]));
            return run_lock_contender_child(std::filesystem::path(argv[2]), store_id);
        } catch (...) {
            return 64;
        }
    }
    if (argc == 4 && std::string_view(argv[1]) == "--hold-cleanup-lock") {
        try {
            const auto executable =
                std::filesystem::absolute(std::filesystem::path(argv[0])).string();
            const auto store_id = static_cast<std::uint64_t>(std::stoull(argv[3]));
            return run_lock_holder_child(executable, std::filesystem::path(argv[2]), store_id);
        } catch (...) {
            return 64;
        }
    }
    if (argc == 5 && std::string_view(argv[1]) == "--crash-cleanup") {
        try {
            const auto point_index = static_cast<std::size_t>(std::stoull(argv[2]));
            const auto store_id = static_cast<std::uint64_t>(std::stoull(argv[4]));
            return run_crash_child(point_index, std::filesystem::path(argv[3]), store_id);
        } catch (...) {
            return 64;
        }
    }
    if (argc == 5 && std::string_view(argv[1]) == "--crash-private-lease") {
        try {
            const auto point_index = static_cast<std::size_t>(std::stoull(argv[3]));
            return run_private_lease_crash_child(std::string_view(argv[2]), point_index,
                                                 std::filesystem::path(argv[4]));
        } catch (...) {
            return 64;
        }
    }
    if (argc == 4 && std::string_view(argv[1]) == "--crash-private-writer") {
        try {
            const auto point_index = static_cast<std::size_t>(std::stoull(argv[2]));
            return run_private_writer_crash_child(point_index, std::filesystem::path(argv[3]));
        } catch (...) {
            return 64;
        }
    }
    if (argc == 5 && std::string_view(argv[1]) == "--abandon-private-lease") {
        try {
            const auto store_id = static_cast<std::uint64_t>(std::stoull(argv[4]));
            return run_private_lease_abandon_child(std::string_view(argv[2]),
                                                   std::filesystem::path(argv[3]), store_id);
        } catch (...) {
            return 64;
        }
    }

    bool run_core = argc == 1;
    bool run_crash = argc == 1;
    bool run_private_lease_crash = argc == 1;
    bool run_authority_union = argc == 1;
    bool run_authority_observer = argc == 1;
    if (argc == 3 && std::string_view(argv[1]) == "--suite") {
        const std::string_view suite(argv[2]);
        run_core = suite == "core";
        run_crash = suite == "crash";
        run_private_lease_crash = suite == "lease-crash";
        run_authority_union = suite == "authority-union";
        run_authority_observer = suite == "authority-observer";
        if (!run_core && !run_crash && !run_private_lease_crash && !run_authority_union &&
            !run_authority_observer) {
            std::cerr << "unknown suite: " << suite << '\n';
            return 64;
        }
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " [--suite authority-observer|authority-union|core|crash|lease-crash]\n";
        return 64;
    }

    try {
        const auto executable = std::filesystem::absolute(std::filesystem::path(argv[0])).string();
        if (run_core) {
            run_core_suite(executable);
        }
        if (run_crash) {
            test_process_crash_recovery(executable);
        }
        if (run_private_lease_crash) {
            run_private_lease_crash_suite(executable);
        }
        if (run_authority_union) {
            run_authority_union_suite();
        }
        if (run_authority_observer) {
            run_authority_observer_suite();
        }
    } catch (const std::exception& error) {
        ++checks_failed;
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
    }

    std::cout << "OOC cleanup transaction checks passed: " << checks_passed << '\n';
    if (checks_failed != 0) {
        std::cerr << "OOC cleanup transaction checks failed: " << checks_failed << '\n';
        return 1;
    }
    return 0;
}
