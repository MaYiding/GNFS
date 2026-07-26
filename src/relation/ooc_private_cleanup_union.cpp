#include "ooc_private_cleanup_union_internal.hpp"

#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>

namespace gnfs::relation::ooc_cleanup_detail {

static_assert(static_cast<std::size_t>(PrivateCleanupMarkerSlot::Count) == 4);
static_assert(static_cast<std::size_t>(PrivateHandoffLeafSlot::Count) == 2);
static_assert(static_cast<std::uint8_t>(PrivateCleanupMarkerObservationKind::Count) == 7);
static_assert(static_cast<std::uint8_t>(PrivateHandoffLeafObservationKind::Count) == 5);
static_assert(static_cast<std::uint8_t>(PrivateCleanupUnionBlock::Count) == 6);
static_assert(static_cast<std::uint8_t>(PrivateNamespaceAction::Count) == 11);
static_assert(static_cast<std::uint8_t>(PrivateNamespaceActionDisposition::Count) == 5);

PrivateCleanupUnionClassification
classify_private_cleanup_union(const PrivateCleanupUnionRawObservation& observation) noexcept {
    PrivateCleanupUnionClassification result;
    bool marker_corrupt = false;
    bool foreign = observation.namespace_foreign;

    for (std::size_t slot = 0; slot < observation.cleanup_markers.size(); ++slot) {
        const auto marker = observation.cleanup_markers[slot];
        switch (marker) {
        case PrivateCleanupMarkerObservationKind::Missing:
            break;
        case PrivateCleanupMarkerObservationKind::LegacyV1:
            result.has_legacy_v1 = true;
            break;
        case PrivateCleanupMarkerObservationKind::LegacyPendingCandidate:
            if (slot != static_cast<std::size_t>(PrivateCleanupMarkerSlot::IntentPending) &&
                slot != static_cast<std::size_t>(PrivateCleanupMarkerSlot::StagedPending)) {
                foreign = true;
            }
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
        case PrivateHandoffLeafObservationKind::Unsupported:
            result.handoff_unsupported = true;
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
    } else if (result.handoff_unsupported) {
        result.block = PrivateCleanupUnionBlock::HandoffUnsupported;
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
    case PrivateCleanupUnionBlock::HandoffUnsupported:
        disposition = PrivateNamespaceActionDisposition::RejectPlatformUnsupported;
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

PrivateHandoffLeafObservationKind
observe_platform_limited_handoff_leaf(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return PrivateHandoffLeafObservationKind::Missing;
        }
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }

    BY_HANDLE_FILE_INFORMATION before{};
    if (!::GetFileInformationByHandle(handle, &before)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }
    const auto before_identity = windows_identity(handle, before);
    if (!before_identity) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }

    BY_HANDLE_FILE_INFORMATION after{};
    if (!::GetFileInformationByHandle(handle, &after)) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }
    const auto after_identity = windows_identity(handle, after);
    if (!after_identity) {
        const DWORD code = ::GetLastError();
        (void)::CloseHandle(handle);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(code));
    }
    if (!::CloseHandle(handle)) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, windows_error(::GetLastError()));
    }
    if (!windows_regular_single_link(before) || !windows_regular_single_link(after) ||
        *before_identity != *after_identity) {
        return PrivateHandoffLeafObservationKind::Foreign;
    }
    return PrivateHandoffLeafObservationKind::Unsupported;
#else
    struct stat named{};
    int result = -1;
    do {
        result = ::lstat(path.c_str(), &named);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return PrivateHandoffLeafObservationKind::Missing;
        }
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(saved_errno));
    }
    if (!posix_regular_single_link(named) ||
        static_cast<std::uint64_t>(named.st_uid) != static_cast<std::uint64_t>(::geteuid()) ||
        (named.st_mode & static_cast<mode_t>(07777)) != (S_IRUSR | S_IWUSR)) {
        return PrivateHandoffLeafObservationKind::Foreign;
    }

    const auto inspected = inspect_file(path, 0, false);
    switch (inspected.kind) {
    case InspectKind::Present:
        return inspected.identity == posix_identity(named)
                   ? PrivateHandoffLeafObservationKind::Unsupported
                   : PrivateHandoffLeafObservationKind::Foreign;
    case InspectKind::Missing:
    case InspectKind::Rejected:
        return PrivateHandoffLeafObservationKind::Foreign;
    case InspectKind::Error:
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    return PrivateHandoffLeafObservationKind::Foreign;
#endif
}

namespace {

[[nodiscard]] constexpr OOCAuthorizedCleanupMarkerKindV2
opposite_v2_marker_kind(OOCAuthorizedCleanupMarkerKindV2 expected) noexcept {
    return expected == OOCAuthorizedCleanupMarkerKindV2::intent
               ? OOCAuthorizedCleanupMarkerKindV2::staged
               : OOCAuthorizedCleanupMarkerKindV2::intent;
}

[[nodiscard]] constexpr std::uint64_t opposite_v1_marker_magic(std::uint64_t expected) noexcept {
    return expected == INTENT_MAGIC ? STAGED_MAGIC : INTENT_MAGIC;
}

[[nodiscard]] bool has_v2_magic_crash_prefix(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return false;
    }
    const auto compared = (std::min)(bytes.size(), OOC_AUTHORIZED_CLEANUP_INTENT_MAGIC_V2.size());
    for (std::size_t index = 0; index < compared; ++index) {
        const auto expected = static_cast<std::byte>(
            static_cast<unsigned char>(OOC_AUTHORIZED_CLEANUP_INTENT_MAGIC_V2[index]));
        if (bytes[index] != expected) {
            return false;
        }
    }
    return true;
}

inline void reject_v2_decoder_infrastructure_failure(OOCAuthorizedCleanupIntentProtocolCode code) {
    switch (code) {
    case OOCAuthorizedCleanupIntentProtocolCode::resource_exhausted:
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
             std::make_error_code(std::errc::not_enough_memory));
    case OOCAuthorizedCleanupIntentProtocolCode::digest_unavailable:
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
             std::make_error_code(std::errc::operation_not_supported));
    default:
        return;
    }
}

[[nodiscard]] PrivateCleanupMarkerObservationKind
decode_cleanup_marker_leaf(const std::filesystem::path& path, bool pending,
                           std::uint64_t expected_v1_magic,
                           OOCAuthorizedCleanupMarkerKindV2 expected_v2_kind) {
    const auto inspected = inspect_pending_file(path, OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2);
    switch (inspected.kind) {
    case InspectKind::Missing:
        return PrivateCleanupMarkerObservationKind::Missing;
    case InspectKind::Error:
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    case InspectKind::Rejected:
        return pending ? PrivateCleanupMarkerObservationKind::Foreign
                       : PrivateCleanupMarkerObservationKind::Malformed;
    case InspectKind::Present:
        break;
    }

    if (inspected.bytes.size() == MARKER_BYTES) {
        if (marker_is_valid(inspected.bytes, expected_v1_magic)) {
            return PrivateCleanupMarkerObservationKind::LegacyV1;
        }
        if (pending &&
            marker_is_valid(inspected.bytes, opposite_v1_marker_magic(expected_v1_magic))) {
            return PrivateCleanupMarkerObservationKind::Foreign;
        }
        if (!pending || has_v2_magic_crash_prefix(inspected.bytes)) {
            return PrivateCleanupMarkerObservationKind::Malformed;
        }
        return PrivateCleanupMarkerObservationKind::LegacyPendingCandidate;
    }

    if (inspected.bytes.size() == OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2) {
        const auto expected =
            decode_ooc_authorized_cleanup_intent(inspected.bytes, expected_v2_kind);
        if (expected) {
            return PrivateCleanupMarkerObservationKind::AuthorizedV2;
        }
        reject_v2_decoder_infrastructure_failure(expected.status.code);
        if (expected.status.code ==
            OOCAuthorizedCleanupIntentProtocolCode::unexpected_marker_kind) {
            const auto opposite = decode_ooc_authorized_cleanup_intent(
                inspected.bytes, opposite_v2_marker_kind(expected_v2_kind));
            if (opposite) {
                return PrivateCleanupMarkerObservationKind::WrongRoleV2;
            }
            reject_v2_decoder_infrastructure_failure(opposite.status.code);
        }
        // A V2-sized pending leaf exceeded the legacy pending bound before
        // this preflight existed, so an invalid record retains the established
        // foreign-replacement result. Canonical corruption remains
        // IntentCorrupt.
        return !pending || has_v2_magic_crash_prefix(inspected.bytes)
                   ? PrivateCleanupMarkerObservationKind::Malformed
                   : PrivateCleanupMarkerObservationKind::Foreign;
    }

    if (!pending) {
        return PrivateCleanupMarkerObservationKind::Malformed;
    }
    if (has_v2_magic_crash_prefix(inspected.bytes)) {
        return PrivateCleanupMarkerObservationKind::Malformed;
    }
    // A bounded V1 pending leaf is deliberately opaque here. The established
    // V1 code must compare it with the context-bound expected record before it
    // can decide whether to rewrite, reclaim, or preserve that exact inode.
    return inspected.bytes.size() <= MARKER_BYTES
               ? PrivateCleanupMarkerObservationKind::LegacyPendingCandidate
               : PrivateCleanupMarkerObservationKind::Foreign;
}

[[nodiscard]] PrivateCleanupUnionRawObservation
observe_private_cleanup_union_locked(const OOCCleanupPaths& paths, const BaseLock& lock) {
    if (!lock.matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    lock.require_stable();

    // This is the first runtime slice of the authority-union observer. It is
    // intentionally read-only. Cleanup leaves still use the portable stable
    // path reader, while the generic handoff witness retains C1's held private
    // directory handle. A later slice will move all six reads behind that same
    // handle and share one before/after inventory.
    auto observation = invoke_with_stable_base_lock(lock, [&] {
        PrivateCleanupUnionRawObservation raw;
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Intent)] =
            decode_cleanup_marker_leaf(paths.intent_path, false, INTENT_MAGIC,
                                       OOCAuthorizedCleanupMarkerKindV2::intent);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::IntentPending)] =
            decode_cleanup_marker_leaf(paths.intent_pending_path, true, INTENT_MAGIC,
                                       OOCAuthorizedCleanupMarkerKindV2::intent);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Staged)] =
            decode_cleanup_marker_leaf(paths.staged_path, false, STAGED_MAGIC,
                                       OOCAuthorizedCleanupMarkerKindV2::staged);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::StagedPending)] =
            decode_cleanup_marker_leaf(paths.staged_pending_path, true, STAGED_MAGIC,
                                       OOCAuthorizedCleanupMarkerKindV2::staged);

        if (inspect_directory_identity_locked(paths.private_directory)) {
            const auto entries = inspect_private_handoff_directory_entries(paths);
            raw.namespace_foreign = !entries.valid;
        }
        if (raw.namespace_foreign) {
            return raw;
        }

#if defined(__APPLE__)
        auto handoff = observe_private_handoff_locked(paths, lock, true);
        switch (handoff.inspection.state) {
        case OOCPrivateHandoffState::None:
            break;
        case OOCPrivateHandoffState::Canonical:
            raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
                PrivateHandoffLeafObservationKind::Exact;
            if (handoff.pending) {
                raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
                    PrivateHandoffLeafObservationKind::Exact;
            }
            break;
        case OOCPrivateHandoffState::PendingOnly:
            raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
                PrivateHandoffLeafObservationKind::Exact;
            break;
        case OOCPrivateHandoffState::TaintedPreserved:
            if (handoff.inspection.result.status == OOCCleanupStatus::PlatformUnsupported) {
                fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
                     handoff.inspection.result.native_error);
            }
            raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
                handoff.inspection.result.status == OOCCleanupStatus::ForeignReplacementPreserved
                    ? PrivateHandoffLeafObservationKind::Foreign
                    : PrivateHandoffLeafObservationKind::Malformed;
            break;
        }
#else
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
            observe_platform_limited_handoff_leaf(paths.private_handoff_path);
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
            observe_platform_limited_handoff_leaf(paths.private_handoff_pending_path);
#endif
        return raw;
    });
    lock.require_stable();
    return observation;
}

[[nodiscard]] std::optional<OOCCleanupResult>
project_private_cleanup_union_preflight(const OOCCleanupPaths& paths, const BaseLock& lock,
                                        PrivateNamespaceAction action) {
    if (paths.private_directory.empty()) {
        return std::nullopt;
    }
    const auto observation = observe_private_cleanup_union_locked(paths, lock);
    const auto decision = decide_private_namespace_action(observation, action);
    lock.require_stable();
    switch (decision.disposition) {
    case PrivateNamespaceActionDisposition::DelegateExistingRuntime:
        return std::nullopt;
    case PrivateNamespaceActionDisposition::RejectForeignPreserved:
        return OOCCleanupResult{
            .status = OOCCleanupStatus::ForeignReplacementPreserved,
            .stage = OOCCleanupStage::None,
            .native_error = protocol_error(),
        };
    case PrivateNamespaceActionDisposition::RejectIntentCorrupt:
        return OOCCleanupResult{
            .status = OOCCleanupStatus::IntentCorrupt,
            .stage = OOCCleanupStage::None,
            .native_error = protocol_error(),
        };
    case PrivateNamespaceActionDisposition::RejectPlatformUnsupported:
        return OOCCleanupResult{
            .status = OOCCleanupStatus::PlatformUnsupported,
            .stage = OOCCleanupStage::None,
            .native_error = std::make_error_code(std::errc::operation_not_supported),
        };
    case PrivateNamespaceActionDisposition::RejectNamespaceConflict:
        return OOCCleanupResult{
            .status = OOCCleanupStatus::NamespaceConflict,
            .stage = OOCCleanupStage::None,
            .native_error = protocol_error(),
        };
    case PrivateNamespaceActionDisposition::Count:
        break;
    }
    return OOCCleanupResult{
        .status = OOCCleanupStatus::ForeignReplacementPreserved,
        .stage = OOCCleanupStage::None,
        .native_error = protocol_error(),
    };
}

} // namespace

std::optional<OOCCleanupResult> preflight_private_cleanup_union_for_transaction_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock, bool publish_intent_only) {
    return project_private_cleanup_union_preflight(
        paths, lock,
        publish_intent_only ? PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff
                            : PrivateNamespaceAction::RunLegacyCleanup);
}

std::optional<OOCCleanupResult>
preflight_private_cleanup_union_for_recovery_locked(const OOCCleanupPaths& paths,
                                                    const BaseLock& lock) {
    return project_private_cleanup_union_preflight(paths, lock,
                                                   PrivateNamespaceAction::RecoverPrivateLease);
}

std::optional<OOCCleanupResult>
preflight_private_cleanup_union_for_removal_locked(const OOCCleanupPaths& paths,
                                                   const BaseLock& lock) {
    return project_private_cleanup_union_preflight(paths, lock,
                                                   PrivateNamespaceAction::RemovePrivateLease);
}

std::optional<OOCCleanupResult>
preflight_private_cleanup_union_for_reservation_locked(const OOCCleanupPaths& paths,
                                                       const BaseLock& lock) {
    return project_private_cleanup_union_preflight(paths, lock,
                                                   PrivateNamespaceAction::ReservePrivateLease);
}

} // namespace gnfs::relation::ooc_cleanup_detail
