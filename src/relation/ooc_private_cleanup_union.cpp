#include "ooc_private_cleanup_union_internal.hpp"

#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <system_error>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <dirent.h>
#endif

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
decode_cleanup_marker_inspection(const InspectResult& inspected, bool pending,
                                 std::uint64_t expected_v1_magic,
                                 OOCAuthorizedCleanupMarkerKindV2 expected_v2_kind) {
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

#if !defined(__APPLE__)
[[nodiscard]] PrivateCleanupMarkerObservationKind
decode_cleanup_marker_leaf(const std::filesystem::path& path, bool pending,
                           std::uint64_t expected_v1_magic,
                           OOCAuthorizedCleanupMarkerKindV2 expected_v2_kind) {
    return decode_cleanup_marker_inspection(
        inspect_pending_file(path, OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2), pending,
        expected_v1_magic, expected_v2_kind);
}
#endif

#if defined(__APPLE__)

enum class PrivateCleanupUnionDirectoryEntry : std::size_t {
    Owner,
    Index,
    Data,
    Handoff,
    HandoffPending,
    Intent,
    IntentPending,
    Staged,
    StagedPending,
    QuarantineIndex,
    QuarantineData,
    Count,
};

struct PrivateCleanupUnionLeafMetadata final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t mode = 0;
    std::uint64_t owner = 0;
    std::uint64_t group = 0;
    std::uint64_t link_count = 0;
    std::int64_t size = 0;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
    std::int64_t changed_seconds = 0;
    std::int64_t changed_nanoseconds = 0;

    friend bool operator==(const PrivateCleanupUnionLeafMetadata&,
                           const PrivateCleanupUnionLeafMetadata&) = default;
};

struct PrivateCleanupUnionDirectoryInventory final {
    bool foreign = false;
    std::array<std::optional<PrivateCleanupUnionLeafMetadata>,
               static_cast<std::size_t>(PrivateCleanupUnionDirectoryEntry::Count)>
        leaves{};

    friend bool operator==(const PrivateCleanupUnionDirectoryInventory&,
                           const PrivateCleanupUnionDirectoryInventory&) = default;
};

[[nodiscard]] PrivateCleanupUnionLeafMetadata
private_cleanup_union_leaf_metadata(const struct stat& metadata) noexcept {
    const auto modified = metadata.st_mtimespec;
    const auto changed = metadata.st_ctimespec;
    return {
        .device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
        .mode = static_cast<std::uint64_t>(metadata.st_mode),
        .owner = static_cast<std::uint64_t>(metadata.st_uid),
        .group = static_cast<std::uint64_t>(metadata.st_gid),
        .link_count = static_cast<std::uint64_t>(metadata.st_nlink),
        .size = static_cast<std::int64_t>(metadata.st_size),
        .modified_seconds = static_cast<std::int64_t>(modified.tv_sec),
        .modified_nanoseconds = static_cast<std::int64_t>(modified.tv_nsec),
        .changed_seconds = static_cast<std::int64_t>(changed.tv_sec),
        .changed_nanoseconds = static_cast<std::int64_t>(changed.tv_nsec),
    };
}

[[nodiscard]] bool private_cleanup_union_leaf_has_file_policy(
    const PrivateCleanupUnionLeafMetadata& metadata) noexcept {
    return (metadata.mode & static_cast<std::uint64_t>(S_IFMT)) ==
               static_cast<std::uint64_t>(S_IFREG) &&
           metadata.link_count == 1 && metadata.owner == static_cast<std::uint64_t>(::geteuid()) &&
           (metadata.mode & static_cast<std::uint64_t>(07777)) ==
               static_cast<std::uint64_t>(S_IRUSR | S_IWUSR) &&
           metadata.size >= 0;
}

[[nodiscard]] std::array<std::filesystem::path,
                         static_cast<std::size_t>(PrivateCleanupUnionDirectoryEntry::Count)>
private_cleanup_union_allowed_leaves(const OOCCleanupPaths& paths) {
    return {
        std::filesystem::path(".gnfs-private-lease-v1.owner"),
        paths.index_path.filename(),
        paths.data_path.filename(),
        paths.private_handoff_path.filename(),
        paths.private_handoff_pending_path.filename(),
        paths.intent_path.filename(),
        paths.intent_pending_path.filename(),
        paths.staged_path.filename(),
        paths.staged_pending_path.filename(),
        paths.quarantine_index_path.filename(),
        paths.quarantine_data_path.filename(),
    };
}

[[nodiscard]] PrivateCleanupUnionDirectoryInventory
scan_private_cleanup_union_directory(const OOCCleanupPaths& paths,
                                     const PrivateDirectoryHandle& directory) {
    directory.require_stable();
    const int held_descriptor = static_cast<int>(directory.native_handle());
    int scan_descriptor = -1;
    do {
        scan_descriptor =
            ::openat(held_descriptor, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (scan_descriptor < 0 && errno == EINTR);
    if (scan_descriptor < 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }

    struct stat scan_metadata{};
    if (::fstat(scan_descriptor, &scan_metadata) != 0 ||
        stable_identity(posix_identity(scan_metadata)) != directory.identity()) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(scan_descriptor);
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, posix_error(saved_errno));
    }

    DIR* raw_stream = ::fdopendir(scan_descriptor);
    if (raw_stream == nullptr) {
        const int saved_errno = errno;
        (void)::close(scan_descriptor);
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(saved_errno));
    }
    std::unique_ptr<DIR, decltype(&::closedir)> stream(raw_stream, &::closedir);
    const auto allowed = private_cleanup_union_allowed_leaves(paths);

    PrivateCleanupUnionDirectoryInventory inventory;
    errno = 0;
    while (const auto* entry = ::readdir(stream.get())) {
        const std::filesystem::path leaf(entry->d_name);
        if (leaf == std::filesystem::path(".") || leaf == std::filesystem::path("..")) {
            errno = 0;
            continue;
        }

        std::size_t slot = allowed.size();
        for (std::size_t index = 0; index < allowed.size(); ++index) {
            if (path_leaf_equals(leaf, allowed[index])) {
                slot = index;
                break;
            }
        }
        if (slot == allowed.size() || leaf.native() != allowed[slot].native() ||
            inventory.leaves[slot]) {
            inventory.foreign = true;
            errno = 0;
            continue;
        }

        struct stat metadata{};
        int inspected = -1;
        do {
            inspected = ::fstatat(held_descriptor, leaf.c_str(), &metadata, AT_SYMLINK_NOFOLLOW);
        } while (inspected != 0 && errno == EINTR);
        if (inspected != 0) {
            if (errno == ENOENT) {
                inventory.foreign = true;
                errno = 0;
                continue;
            }
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
        }
        inventory.leaves[slot] = private_cleanup_union_leaf_metadata(metadata);
        errno = 0;
    }
    if (errno != 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }

    struct stat final_metadata{};
    if (::fstat(::dirfd(stream.get()), &final_metadata) != 0 ||
        stable_identity(posix_identity(final_metadata)) != directory.identity()) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
             errno == 0 ? protocol_error() : posix_error(errno));
    }
    DIR* close_stream = stream.release();
    if (::closedir(close_stream) != 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }
    directory.require_stable();
    return inventory;
}

[[nodiscard]] InspectResult inspect_relative_cleanup_leaf(const PrivateDirectoryHandle& directory,
                                                          const std::filesystem::path& leaf,
                                                          std::size_t maximum_bytes) {
    using namespace util::durable_immutable_record;
    const auto read = read_bounded_at(directory.native_handle(), leaf, 0, maximum_bytes);
    switch (read.state()) {
    case BoundedReadState::missing:
        return InspectResult{
            .kind = InspectKind::Missing,
            .identity = {},
            .bytes = {},
            .error = {},
        };
    case BoundedReadState::exact:
        if (!read.bytes() || !read.snapshot()) {
            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
        }
        return InspectResult{
            .kind = InspectKind::Present,
            .identity =
                {
                    .first = read.snapshot()->identity.first,
                    .second = read.snapshot()->identity.second,
                    .third = read.snapshot()->identity.third,
                    .size = read.snapshot()->size,
                },
            .bytes = *read.bytes(),
            .error = {},
        };
    case BoundedReadState::rejected:
        return InspectResult{
            .kind = InspectKind::Rejected,
            .identity = {},
            .bytes = {},
            .error = read.native_error(),
        };
    case BoundedReadState::interrupted:
    case BoundedReadState::failed:
        return InspectResult{
            .kind = InspectKind::Error,
            .identity = {},
            .bytes = {},
            .error = read.native_error(),
        };
    case BoundedReadState::unsupported:
        fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None, read.native_error());
    }
    fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
}

[[nodiscard]] bool private_cleanup_union_inspection_matches_inventory(
    const InspectResult& inspection,
    const std::optional<PrivateCleanupUnionLeafMetadata>& metadata) noexcept {
    switch (inspection.kind) {
    case InspectKind::Missing:
        return !metadata;
    case InspectKind::Present:
        return metadata && metadata->device == inspection.identity.first &&
               metadata->inode == inspection.identity.second && metadata->size >= 0 &&
               static_cast<std::uint64_t>(metadata->size) == inspection.identity.size;
    case InspectKind::Rejected:
        return metadata.has_value();
    case InspectKind::Error:
        return false;
    }
    return false;
}

[[nodiscard]] PrivateCleanupMarkerObservationKind decode_relative_cleanup_marker_leaf(
    const PrivateDirectoryHandle& directory, const std::filesystem::path& leaf, bool pending,
    std::uint64_t expected_v1_magic, OOCAuthorizedCleanupMarkerKindV2 expected_v2_kind,
    const std::optional<PrivateCleanupUnionLeafMetadata>& inventory_metadata,
    bool& inventory_mismatch) {
    const auto inspected =
        inspect_relative_cleanup_leaf(directory, leaf, OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2);
    inventory_mismatch = inventory_mismatch || !private_cleanup_union_inspection_matches_inventory(
                                                   inspected, inventory_metadata);
    return decode_cleanup_marker_inspection(inspected, pending, expected_v1_magic,
                                            expected_v2_kind);
}

[[nodiscard]] bool private_handoff_leaf_matches_inventory(
    const LoadedPrivateHandoffLeaf& leaf,
    const std::optional<PrivateCleanupUnionLeafMetadata>& metadata) noexcept {
    switch (leaf.state) {
    case PrivateHandoffLeafState::Missing:
        return !metadata;
    case PrivateHandoffLeafState::Exact:
        return metadata && leaf.snapshot && metadata->device == leaf.snapshot->identity.first &&
               metadata->inode == leaf.snapshot->identity.second && metadata->size >= 0 &&
               static_cast<std::uint64_t>(metadata->size) == leaf.snapshot->size;
    case PrivateHandoffLeafState::Rejected:
        return metadata.has_value();
    }
    return false;
}

void project_strict_handoff_classification(const PrivateHandoffLeafClassification& classified,
                                           const LoadedPrivateHandoffLeaf& pending,
                                           PrivateCleanupUnionRawObservation& raw) {
    switch (classified.inspection.state) {
    case OOCPrivateHandoffState::None:
        break;
    case OOCPrivateHandoffState::Canonical:
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
            PrivateHandoffLeafObservationKind::Exact;
        if (pending.state == PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
                PrivateHandoffLeafObservationKind::Exact;
        }
        break;
    case OOCPrivateHandoffState::PendingOnly:
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
            PrivateHandoffLeafObservationKind::Exact;
        break;
    case OOCPrivateHandoffState::TaintedPreserved:
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
            classified.inspection.result.status == OOCCleanupStatus::ForeignReplacementPreserved
                ? PrivateHandoffLeafObservationKind::Foreign
                : PrivateHandoffLeafObservationKind::Malformed;
        break;
    }
}

#endif

[[nodiscard]] PrivateCleanupUnionRawObservation
observe_private_cleanup_union_locked(const OOCCleanupPaths& paths, const BaseLock& lock,
                                     PrivateCleanupUnionObservationTestHooks hooks = {}) {
    if (!lock.matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
#if !defined(__APPLE__)
    if (hooks.observe != nullptr) {
        fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
             std::make_error_code(std::errc::operation_not_supported));
    }
#endif
    lock.require_stable();

    auto observation = invoke_with_stable_base_lock(lock, [&] {
        PrivateCleanupUnionRawObservation raw;
#if defined(__APPLE__)
        const auto directory_identity = inspect_directory_identity_locked(paths.private_directory);
        if (!directory_identity) {
            return raw;
        }

        PrivateDirectoryHandle directory(paths.private_directory);
        if (directory.identity() != *directory_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        // Enumerating names and no-follow metadata grants no record authority.
        // Do it before the private-mode check so a concrete foreign leaf keeps
        // the established ForeignReplacementPreserved precedence. No leaf
        // bytes are read until the directory policy has passed.
        const auto before = scan_private_cleanup_union_directory(paths, directory);
        if (before.foreign) {
            raw.namespace_foreign = true;
            return raw;
        }
        directory.require_private_policy();
        if (hooks.observe != nullptr) {
            hooks.observe(PrivateCleanupUnionObservationPoint::InitialInventoryComplete,
                          hooks.context);
        }
        const auto metadata = [&](PrivateCleanupUnionDirectoryEntry entry)
            -> const std::optional<PrivateCleanupUnionLeafMetadata>& {
            return before.leaves[static_cast<std::size_t>(entry)];
        };
        bool inventory_mismatch = false;
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Intent)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.intent_path.filename(), false, INTENT_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::intent,
                metadata(PrivateCleanupUnionDirectoryEntry::Intent), inventory_mismatch);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::IntentPending)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.intent_pending_path.filename(), true, INTENT_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::intent,
                metadata(PrivateCleanupUnionDirectoryEntry::IntentPending), inventory_mismatch);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Staged)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.staged_path.filename(), false, STAGED_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::staged,
                metadata(PrivateCleanupUnionDirectoryEntry::Staged), inventory_mismatch);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::StagedPending)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.staged_pending_path.filename(), true, STAGED_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::staged,
                metadata(PrivateCleanupUnionDirectoryEntry::StagedPending), inventory_mismatch);

        const auto handoff_metadata = metadata(PrivateCleanupUnionDirectoryEntry::Handoff);
        const auto handoff_pending_metadata =
            metadata(PrivateCleanupUnionDirectoryEntry::HandoffPending);
        const bool handoff_metadata_foreign =
            (handoff_metadata && !private_cleanup_union_leaf_has_file_policy(*handoff_metadata)) ||
            (handoff_pending_metadata &&
             !private_cleanup_union_leaf_has_file_policy(*handoff_pending_metadata));
        if (handoff_metadata_foreign) {
            if (handoff_metadata &&
                !private_cleanup_union_leaf_has_file_policy(*handoff_metadata)) {
                raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
                    PrivateHandoffLeafObservationKind::Foreign;
            }
            if (handoff_pending_metadata &&
                !private_cleanup_union_leaf_has_file_policy(*handoff_pending_metadata)) {
                raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
                    PrivateHandoffLeafObservationKind::Foreign;
            }
        } else {
            try {
                const auto canonical = read_private_handoff_leaf(
                    directory.native_handle(), paths.private_handoff_path.filename());
                const auto pending = read_private_handoff_leaf(
                    directory.native_handle(), paths.private_handoff_pending_path.filename());
                inventory_mismatch =
                    inventory_mismatch ||
                    !private_handoff_leaf_matches_inventory(canonical, handoff_metadata) ||
                    !private_handoff_leaf_matches_inventory(pending, handoff_pending_metadata);
                const auto classified = classify_private_handoff_leaves_locked(
                    paths, lock, directory.identity(), canonical, pending);
                project_strict_handoff_classification(classified, pending, raw);
            } catch (const Failure& failure) {
                if (failure.status != OOCCleanupStatus::PlatformUnsupported) {
                    throw;
                }
                raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
                    handoff_metadata ? PrivateHandoffLeafObservationKind::Unsupported
                                     : PrivateHandoffLeafObservationKind::Missing;
                raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
                    handoff_pending_metadata ? PrivateHandoffLeafObservationKind::Unsupported
                                             : PrivateHandoffLeafObservationKind::Missing;
            }
        }
        if (hooks.observe != nullptr) {
            hooks.observe(PrivateCleanupUnionObservationPoint::LeafReadsComplete, hooks.context);
        }
        const auto after = scan_private_cleanup_union_directory(paths, directory);
        raw.namespace_foreign = inventory_mismatch || after.foreign || before != after;
        directory.require_stable();
        directory.require_private_policy();
        return raw;
#else
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
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
            observe_platform_limited_handoff_leaf(paths.private_handoff_path);
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
            observe_platform_limited_handoff_leaf(paths.private_handoff_pending_path);
        return raw;
#endif
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

PrivateCleanupUnionRawObservation
observe_private_cleanup_union_for_test(const std::filesystem::path& base_path,
                                       PrivateCleanupUnionObservationTestHooks hooks) {
    const auto paths = freeze_paths(base_path);
    if (paths.private_directory.empty()) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    BaseLock lock(paths.lock_path, false);
    return observe_private_cleanup_union_locked(paths, lock, hooks);
}

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
