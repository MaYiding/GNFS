#include "ooc_private_cleanup_action_permit_internal.hpp"

#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

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

struct PrivateCleanupLeafWitness final {
    InspectResult inspection;
    std::optional<util::durable_immutable_record::RecordSnapshot> snapshot;
};

[[nodiscard]] PrivateCleanupLeafWitness
inspect_relative_cleanup_leaf(const PrivateDirectoryHandle& directory,
                              const std::filesystem::path& leaf, std::size_t maximum_bytes) {
    using namespace util::durable_immutable_record;
    const auto read = read_bounded_at(directory.native_handle(), leaf, 0, maximum_bytes);
    switch (read.state()) {
    case BoundedReadState::missing:
        return {
            .inspection =
                {
                    .kind = InspectKind::Missing,
                    .identity = {},
                    .bytes = {},
                    .error = {},
                },
        };
    case BoundedReadState::exact:
        if (!read.bytes() || !read.snapshot()) {
            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
        }
        return {
            .inspection =
                {
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
                },
            .snapshot = *read.snapshot(),
        };
    case BoundedReadState::rejected:
        return {
            .inspection =
                {
                    .kind = InspectKind::Rejected,
                    .identity = {},
                    .bytes = {},
                    .error = read.native_error(),
                },
        };
    case BoundedReadState::interrupted:
    case BoundedReadState::failed:
        return {
            .inspection =
                {
                    .kind = InspectKind::Error,
                    .identity = {},
                    .bytes = {},
                    .error = read.native_error(),
                },
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
    bool& inventory_mismatch, std::optional<PrivateCleanupLeafWitness>& retained) {
    auto inspected =
        inspect_relative_cleanup_leaf(directory, leaf, OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2);
    inventory_mismatch = inventory_mismatch || !private_cleanup_union_inspection_matches_inventory(
                                                   inspected.inspection, inventory_metadata);
    const auto decoded = decode_cleanup_marker_inspection(inspected.inspection, pending,
                                                          expected_v1_magic, expected_v2_kind);
    retained = std::move(inspected);
    return decoded;
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

[[nodiscard]] PrivateHandoffLeafObservationKind
project_tainted_handoff_leaf(const PrivateHandoffLeafClassification& classified) noexcept {
    return classified.inspection.result.status == OOCCleanupStatus::ForeignReplacementPreserved
               ? PrivateHandoffLeafObservationKind::Foreign
               : PrivateHandoffLeafObservationKind::Malformed;
}

enum class StrictHandoffObservationBlocker : std::uint8_t {
    None,
    Unsupported,
    Foreign,
};

[[nodiscard]] StrictHandoffObservationBlocker
strict_handoff_observation_blocker(const PrivateCleanupUnionRawObservation& raw) noexcept {
    if (raw.namespace_foreign) {
        return StrictHandoffObservationBlocker::Foreign;
    }
    StrictHandoffObservationBlocker blocker = StrictHandoffObservationBlocker::None;
    for (const auto marker : raw.handoff_markers) {
        switch (marker) {
        case PrivateHandoffLeafObservationKind::Missing:
        case PrivateHandoffLeafObservationKind::Exact:
            break;
        case PrivateHandoffLeafObservationKind::Unsupported:
            if (blocker == StrictHandoffObservationBlocker::None) {
                blocker = StrictHandoffObservationBlocker::Unsupported;
            }
            break;
        case PrivateHandoffLeafObservationKind::Malformed:
        case PrivateHandoffLeafObservationKind::Foreign:
        case PrivateHandoffLeafObservationKind::Count:
            return StrictHandoffObservationBlocker::Foreign;
        }
    }
    return blocker;
}

[[nodiscard]] PrivateHandoffLeafObservationKind
strict_handoff_blocker_fallback(StrictHandoffObservationBlocker blocker) noexcept {
    return blocker == StrictHandoffObservationBlocker::Foreign
               ? PrivateHandoffLeafObservationKind::Foreign
               : PrivateHandoffLeafObservationKind::Unsupported;
}

[[nodiscard]] std::optional<PrivateHandoffLeafClassification>
project_strict_handoff_leaves(const OOCCleanupPaths& paths, const BaseLock& lock,
                              const std::array<std::uint64_t, 3>& directory_identity,
                              const std::optional<LoadedPrivateHandoffLeaf>& canonical,
                              const std::optional<LoadedPrivateHandoffLeaf>& pending,
                              PrivateCleanupUnionRawObservation& raw,
                              std::optional<LoadedPrivateLeaseMarker>& reserved_witness,
                              std::optional<LoadedPrivateLeaseMarker>& owned_witness) {
    const auto canonical_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical);
    const auto pending_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending);
    if (!canonical && !pending) {
        return std::nullopt;
    }

    const LoadedPrivateHandoffLeaf missing{
        .state = PrivateHandoffLeafState::Missing,
    };
    const auto& canonical_leaf = canonical ? *canonical : missing;
    const auto& pending_leaf = pending ? *pending : missing;
    if (canonical_leaf.state == PrivateHandoffLeafState::Exact &&
        pending_leaf.state == PrivateHandoffLeafState::Exact &&
        canonical_leaf.bytes != pending_leaf.bytes) {
        raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Malformed;
    }
    const auto existing_blocker = strict_handoff_observation_blocker(raw);
    std::optional<PrivateHandoffLeafClassification> classified;
    try {
        classified =
            classify_private_handoff_leaves_locked(paths, lock, directory_identity, canonical_leaf,
                                                   pending_leaf, &reserved_witness, &owned_witness);
    } catch (const Failure&) {
        if (existing_blocker == StrictHandoffObservationBlocker::None) {
            throw;
        }
        const auto fallback = strict_handoff_blocker_fallback(existing_blocker);
        if (canonical_leaf.state == PrivateHandoffLeafState::Exact &&
            raw.handoff_markers[canonical_slot] == PrivateHandoffLeafObservationKind::Missing) {
            raw.handoff_markers[canonical_slot] = fallback;
        }
        if (pending_leaf.state == PrivateHandoffLeafState::Exact &&
            raw.handoff_markers[pending_slot] == PrivateHandoffLeafObservationKind::Missing) {
            raw.handoff_markers[pending_slot] = fallback;
        }
        return std::nullopt;
    }
    switch (classified->inspection.state) {
    case OOCPrivateHandoffState::None:
        if (canonical_leaf.state == PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Foreign;
        }
        if (pending_leaf.state == PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Foreign;
        }
        return classified;
    case OOCPrivateHandoffState::Canonical:
        if (canonical_leaf.state != PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Foreign;
            if (pending_leaf.state == PrivateHandoffLeafState::Exact) {
                raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Foreign;
            }
            return classified;
        }
        raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Exact;
        if (pending_leaf.state == PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Exact;
        }
        return classified;
    case OOCPrivateHandoffState::PendingOnly:
        if (pending_leaf.state != PrivateHandoffLeafState::Exact) {
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Foreign;
            if (canonical_leaf.state == PrivateHandoffLeafState::Exact) {
                raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Foreign;
            }
            return classified;
        }
        raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Exact;
        return classified;
    case OOCPrivateHandoffState::TaintedPreserved:
        break;
    }

    const bool canonical_exact = canonical_leaf.state == PrivateHandoffLeafState::Exact;
    const bool pending_exact = pending_leaf.state == PrivateHandoffLeafState::Exact;
    if (canonical_exact && pending_exact) {
        if (classified->canonical_context_valid) {
            raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Exact;
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Malformed;
            return classified;
        }
        raw.handoff_markers[canonical_slot] = project_tainted_handoff_leaf(*classified);
        raw.handoff_markers[pending_slot] = canonical_leaf.bytes == pending_leaf.bytes
                                                ? project_tainted_handoff_leaf(*classified)
                                                : PrivateHandoffLeafObservationKind::Malformed;
        try {
            const auto pending_only = classify_private_handoff_leaves_locked(
                paths, lock, directory_identity, missing, pending_leaf);
            if (pending_only.inspection.state == OOCPrivateHandoffState::PendingOnly &&
                pending_only.pending_is_preactive) {
                raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Exact;
            }
        } catch (const Failure&) {
            // The aggregate is already terminal. Companion refinement is
            // diagnostic only and may not replace the established blocker.
        }
        return classified;
    }
    if (canonical_exact) {
        raw.handoff_markers[canonical_slot] = project_tainted_handoff_leaf(*classified);
    }
    if (pending_exact) {
        raw.handoff_markers[pending_slot] = project_tainted_handoff_leaf(*classified);
    }
    return classified;
}

#endif

struct PrivateCleanupUnionObservationWitness final {
    PrivateCleanupUnionRawObservation raw;
#if defined(__APPLE__)
    std::unique_ptr<PrivateDirectoryHandle> directory;
    std::optional<PrivateCleanupUnionDirectoryInventory> before_inventory;
    std::optional<PrivateCleanupUnionDirectoryInventory> after_inventory;
    std::array<std::optional<PrivateCleanupLeafWitness>,
               static_cast<std::size_t>(PrivateCleanupMarkerSlot::Count)>
        cleanup_leaves;
    std::optional<LoadedPrivateHandoffLeaf> canonical_handoff;
    std::optional<LoadedPrivateHandoffLeaf> pending_handoff;
    std::optional<PrivateHandoffLeafClassification> handoff_classification;
    std::optional<LoadedPrivateLeaseMarker> reserved_marker;
    std::optional<LoadedPrivateLeaseMarker> owned_marker;
#endif
};

[[nodiscard]] PrivateCleanupUnionObservationWitness
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
        PrivateCleanupUnionObservationWitness witness;
        auto& raw = witness.raw;
#if defined(__APPLE__)
        const auto directory_identity = inspect_directory_identity_locked(paths.private_directory);
        if (!directory_identity) {
            return witness;
        }

        witness.directory = std::make_unique<PrivateDirectoryHandle>(paths.private_directory);
        auto& directory = *witness.directory;
        if (directory.identity() != *directory_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        // Enumerating names and no-follow metadata grants no record authority.
        // Do it before the private-mode check so a concrete foreign leaf keeps
        // the established ForeignReplacementPreserved precedence. No leaf
        // bytes are read until the directory policy has passed.
        witness.before_inventory = scan_private_cleanup_union_directory(paths, directory);
        const auto& before = *witness.before_inventory;
        if (before.foreign) {
            raw.namespace_foreign = true;
            return witness;
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
                metadata(PrivateCleanupUnionDirectoryEntry::Intent), inventory_mismatch,
                witness.cleanup_leaves[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Intent)]);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::IntentPending)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.intent_pending_path.filename(), true, INTENT_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::intent,
                metadata(PrivateCleanupUnionDirectoryEntry::IntentPending), inventory_mismatch,
                witness.cleanup_leaves[static_cast<std::size_t>(
                    PrivateCleanupMarkerSlot::IntentPending)]);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Staged)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.staged_path.filename(), false, STAGED_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::staged,
                metadata(PrivateCleanupUnionDirectoryEntry::Staged), inventory_mismatch,
                witness.cleanup_leaves[static_cast<std::size_t>(PrivateCleanupMarkerSlot::Staged)]);
        raw.cleanup_markers[static_cast<std::size_t>(PrivateCleanupMarkerSlot::StagedPending)] =
            decode_relative_cleanup_marker_leaf(
                directory, paths.staged_pending_path.filename(), true, STAGED_MAGIC,
                OOCAuthorizedCleanupMarkerKindV2::staged,
                metadata(PrivateCleanupUnionDirectoryEntry::StagedPending), inventory_mismatch,
                witness.cleanup_leaves[static_cast<std::size_t>(
                    PrivateCleanupMarkerSlot::StagedPending)]);
        raw.namespace_foreign = inventory_mismatch;

        const auto handoff_metadata = metadata(PrivateCleanupUnionDirectoryEntry::Handoff);
        const auto handoff_pending_metadata =
            metadata(PrivateCleanupUnionDirectoryEntry::HandoffPending);
        const auto canonical_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical);
        const auto pending_slot = static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending);
        if (handoff_metadata && !private_cleanup_union_leaf_has_file_policy(*handoff_metadata)) {
            raw.handoff_markers[canonical_slot] = PrivateHandoffLeafObservationKind::Foreign;
        }
        if (handoff_pending_metadata &&
            !private_cleanup_union_leaf_has_file_policy(*handoff_pending_metadata)) {
            raw.handoff_markers[pending_slot] = PrivateHandoffLeafObservationKind::Foreign;
        }
        const auto read_handoff_leaf =
            [&](const std::optional<PrivateCleanupUnionLeafMetadata>& leaf_metadata,
                const std::filesystem::path& leaf,
                PrivateHandoffLeafSlot slot) -> std::optional<LoadedPrivateHandoffLeaf> {
            const auto raw_slot = static_cast<std::size_t>(slot);
            if (raw.handoff_markers[raw_slot] == PrivateHandoffLeafObservationKind::Foreign) {
                return std::nullopt;
            }
            try {
                auto loaded = read_private_handoff_leaf(directory.native_handle(), leaf);
                const bool matches_inventory =
                    private_handoff_leaf_matches_inventory(loaded, leaf_metadata);
                inventory_mismatch = inventory_mismatch || !matches_inventory;
                raw.namespace_foreign = raw.namespace_foreign || !matches_inventory;
                if (loaded.state == PrivateHandoffLeafState::Rejected) {
                    raw.handoff_markers[raw_slot] = PrivateHandoffLeafObservationKind::Foreign;
                    return std::nullopt;
                }
                return loaded;
            } catch (const Failure& failure) {
                if (failure.status == OOCCleanupStatus::PlatformUnsupported) {
                    raw.handoff_markers[raw_slot] =
                        leaf_metadata ? PrivateHandoffLeafObservationKind::Unsupported
                                      : PrivateHandoffLeafObservationKind::Missing;
                    return std::nullopt;
                }
                const auto blocker = strict_handoff_observation_blocker(raw);
                if (blocker != StrictHandoffObservationBlocker::None) {
                    raw.handoff_markers[raw_slot] = strict_handoff_blocker_fallback(blocker);
                    return std::nullopt;
                }
                throw;
            }
        };
        witness.canonical_handoff =
            read_handoff_leaf(handoff_metadata, paths.private_handoff_path.filename(),
                              PrivateHandoffLeafSlot::Canonical);
        witness.pending_handoff = read_handoff_leaf(handoff_pending_metadata,
                                                    paths.private_handoff_pending_path.filename(),
                                                    PrivateHandoffLeafSlot::Pending);
        witness.handoff_classification = project_strict_handoff_leaves(
            paths, lock, directory.identity(), witness.canonical_handoff, witness.pending_handoff,
            raw, witness.reserved_marker, witness.owned_marker);
        if (hooks.observe != nullptr) {
            hooks.observe(PrivateCleanupUnionObservationPoint::LeafReadsComplete, hooks.context);
        }
        witness.after_inventory = scan_private_cleanup_union_directory(paths, directory);
        const auto& after = *witness.after_inventory;
        raw.namespace_foreign =
            raw.namespace_foreign || inventory_mismatch || after.foreign || before != after;
        directory.require_stable();
        directory.require_private_policy();
        return witness;
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
            return witness;
        }
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Canonical)] =
            observe_platform_limited_handoff_leaf(paths.private_handoff_path);
        raw.handoff_markers[static_cast<std::size_t>(PrivateHandoffLeafSlot::Pending)] =
            observe_platform_limited_handoff_leaf(paths.private_handoff_pending_path);
        return witness;
#endif
    });
    lock.require_stable();
    return observation;
}

[[nodiscard]] std::optional<OOCCleanupResult>
private_cleanup_union_blocked_result(const PrivateNamespaceActionDecision& decision) {
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

[[nodiscard]] std::optional<OOCCleanupResult>
project_private_cleanup_union_preflight(const OOCCleanupPaths& paths, const BaseLock& lock,
                                        PrivateNamespaceAction action) {
    if (paths.private_directory.empty()) {
        return std::nullopt;
    }
    const auto observation = observe_private_cleanup_union_locked(paths, lock);
    const auto decision = decide_private_namespace_action(observation.raw, action);
    lock.require_stable();
    return private_cleanup_union_blocked_result(decision);
}

} // namespace

enum class PrivateHandoffConsumptionState : std::uint8_t {
    Fresh,
    LegacyObserved,
    LegacyMutationAuthorized,
    PublicationObserved,
    PublicationMutationAuthorized,
    RecoverConsumed,
    RemoveConsumed,
    Failed,
};

bool try_claim_private_cleanup_action(BaseLock& lock) noexcept {
    bool expected = false;
    return lock.private_cleanup_action_claimed_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
}

void release_private_cleanup_action(BaseLock& lock) noexcept {
    lock.private_cleanup_action_claimed_.store(false, std::memory_order_release);
}

struct PrivateCleanupActionPermit::State final {
    State(OOCCleanupPaths frozen_paths, std::shared_ptr<BaseLock> retained_lock,
          PrivateNamespaceActionDecision retained_decision,
          PrivateCleanupUnionObservationWitness retained_witness)
        : paths(std::move(frozen_paths)), lock(std::move(retained_lock)),
          decision(retained_decision),
          creator_process_id(static_cast<std::uint64_t>(gnfs::util::process_id())),
          witness(std::move(retained_witness)) {}

    ~State() {
        if (lock) {
            release_private_cleanup_action(*lock);
        }
    }

    OOCCleanupPaths paths;
    std::shared_ptr<BaseLock> lock;
    PrivateNamespaceActionDecision decision;
    std::uint64_t creator_process_id = 0;
    PrivateCleanupUnionObservationWitness witness;
    std::optional<PrivateCleanupUnionObservationWitness> publication_phase_witness;
    std::optional<PrivateLeaseRemovalGenerationProof> removal_generation;
    std::optional<PrivateLeaseRemovalGenerationProof> publication_generation;
    std::optional<IntentRecord> publication_intent;
    std::optional<FileIdentity> publication_original_canonical_identity;
    bool removal_generation_binding_attempted = false;
    bool action_started = false;
    PrivateHandoffConsumptionState handoff_state = PrivateHandoffConsumptionState::Fresh;
};

PrivateCleanupActionPermit::PrivateCleanupActionPermit(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

PrivateCleanupActionPermit::PrivateCleanupActionPermit(PrivateCleanupActionPermit&& other) noexcept
    : state_(std::move(other.state_)) {}

PrivateCleanupActionPermit::~PrivateCleanupActionPermit() = default;

static_assert(!std::is_default_constructible_v<PrivateCleanupActionPermit>);
static_assert(!std::is_copy_constructible_v<PrivateCleanupActionPermit>);
static_assert(!std::is_copy_assignable_v<PrivateCleanupActionPermit>);
static_assert(std::is_nothrow_move_constructible_v<PrivateCleanupActionPermit>);
static_assert(!std::is_move_assignable_v<PrivateCleanupActionPermit>);

namespace {

class PrivateCleanupActionClaimGuard final {
public:
    explicit PrivateCleanupActionClaimGuard(BaseLock& lock) noexcept
        : lock_(&lock), acquired_(try_claim_private_cleanup_action(lock)) {}

    PrivateCleanupActionClaimGuard(const PrivateCleanupActionClaimGuard&) = delete;
    PrivateCleanupActionClaimGuard& operator=(const PrivateCleanupActionClaimGuard&) = delete;

    ~PrivateCleanupActionClaimGuard() {
        if (acquired_ && lock_ != nullptr) {
            release_private_cleanup_action(*lock_);
        }
    }

    [[nodiscard]] bool acquired() const noexcept {
        return acquired_;
    }

    void transfer_to_permit() noexcept {
        acquired_ = false;
        lock_ = nullptr;
    }

private:
    BaseLock* lock_ = nullptr;
    bool acquired_ = false;
};

[[nodiscard]] OOCCleanupResult private_cleanup_action_busy() noexcept {
    return OOCCleanupResult{
        .status = OOCCleanupStatus::Busy,
        .stage = OOCCleanupStage::None,
        .native_error = std::make_error_code(std::errc::device_or_resource_busy),
    };
}

[[nodiscard]] bool private_cleanup_paths_equal(const OOCCleanupPaths& lhs,
                                               const OOCCleanupPaths& rhs) noexcept {
    return lhs.base_path == rhs.base_path && lhs.private_directory == rhs.private_directory &&
           lhs.index_path == rhs.index_path && lhs.data_path == rhs.data_path &&
           lhs.intent_path == rhs.intent_path &&
           lhs.intent_pending_path == rhs.intent_pending_path &&
           lhs.staged_path == rhs.staged_path &&
           lhs.staged_pending_path == rhs.staged_pending_path && lhs.lock_path == rhs.lock_path &&
           lhs.lease_reserved_path == rhs.lease_reserved_path &&
           lhs.lease_reserved_pending_path == rhs.lease_reserved_pending_path &&
           lhs.lease_owned_path == rhs.lease_owned_path &&
           lhs.lease_owned_pending_path == rhs.lease_owned_pending_path &&
           lhs.private_handoff_path == rhs.private_handoff_path &&
           lhs.private_handoff_pending_path == rhs.private_handoff_pending_path &&
           lhs.quarantine_index_path == rhs.quarantine_index_path &&
           lhs.quarantine_data_path == rhs.quarantine_data_path;
}

void require_private_lease_removal_generation_unchanged(PrivateCleanupActionPermit::State& state) {
    if (!state.removal_generation || !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    const auto& expected = *state.removal_generation;
    const auto current = capture_private_lease_removal_generation_locked(
        state.paths, *state.lock, expected.expected_lease_id, expected.expected_directory_identity,
        expected.expected_owner_identity, expected.expected_owned_identity);
    if (current != expected) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}

#if defined(__APPLE__)
[[nodiscard]] bool cleanup_leaf_witness_equal(const PrivateCleanupLeafWitness& lhs,
                                              const PrivateCleanupLeafWitness& rhs) noexcept {
    return lhs.inspection.kind == rhs.inspection.kind &&
           lhs.inspection.identity == rhs.inspection.identity &&
           lhs.inspection.bytes == rhs.inspection.bytes && lhs.snapshot == rhs.snapshot;
}

[[nodiscard]] bool handoff_leaf_witness_equal(const LoadedPrivateHandoffLeaf& lhs,
                                              const LoadedPrivateHandoffLeaf& rhs) noexcept {
    return lhs.state == rhs.state && lhs.bytes == rhs.bytes && lhs.record == rhs.record &&
           lhs.snapshot == rhs.snapshot;
}

[[nodiscard]] bool
lease_marker_witness_equal(const std::optional<LoadedPrivateLeaseMarker>& lhs,
                           const std::optional<LoadedPrivateLeaseMarker>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs || (lhs->record == rhs->record && lhs->identity == rhs->identity);
}

[[noreturn]] void fail_private_cleanup_witness_replacement() {
    fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None, protocol_error());
}

void require_private_cleanup_witness_unchanged(const OOCCleanupPaths& paths, const BaseLock& lock,
                                               PrivateCleanupUnionObservationWitness& witness) {
    if (!witness.directory) {
        if (inspect_directory_identity_locked(paths.private_directory)) {
            fail_private_cleanup_witness_replacement();
        }
        return;
    }
    if (!witness.before_inventory || !witness.after_inventory ||
        *witness.before_inventory != *witness.after_inventory) {
        fail_private_cleanup_witness_replacement();
    }

    witness.directory->require_stable();
    witness.directory->require_private_policy();
    const auto before_reads = scan_private_cleanup_union_directory(paths, *witness.directory);
    if (before_reads != *witness.after_inventory) {
        fail_private_cleanup_witness_replacement();
    }

    const std::array<std::filesystem::path,
                     static_cast<std::size_t>(PrivateCleanupMarkerSlot::Count)>
        cleanup_leaves{
            paths.intent_path.filename(),
            paths.intent_pending_path.filename(),
            paths.staged_path.filename(),
            paths.staged_pending_path.filename(),
        };
    for (std::size_t slot = 0; slot < cleanup_leaves.size(); ++slot) {
        if (!witness.cleanup_leaves[slot]) {
            fail_private_cleanup_witness_replacement();
        }
        const auto current = inspect_relative_cleanup_leaf(
            *witness.directory, cleanup_leaves[slot], OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2);
        if (current.inspection.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, current.inspection.error);
        }
        if (!cleanup_leaf_witness_equal(current, *witness.cleanup_leaves[slot])) {
            fail_private_cleanup_witness_replacement();
        }
    }

    const auto require_handoff_leaf_unchanged =
        [&](const std::optional<LoadedPrivateHandoffLeaf>& retained,
            const std::filesystem::path& leaf) {
            if (!retained) {
                fail_private_cleanup_witness_replacement();
            }
            const auto current =
                read_private_handoff_leaf(witness.directory->native_handle(), leaf);
            if (!handoff_leaf_witness_equal(current, *retained)) {
                fail_private_cleanup_witness_replacement();
            }
        };
    require_handoff_leaf_unchanged(witness.canonical_handoff,
                                   paths.private_handoff_path.filename());
    require_handoff_leaf_unchanged(witness.pending_handoff,
                                   paths.private_handoff_pending_path.filename());

    const bool handoff_context_was_observed =
        (witness.canonical_handoff &&
         witness.canonical_handoff->state == PrivateHandoffLeafState::Exact) ||
        (witness.pending_handoff &&
         witness.pending_handoff->state == PrivateHandoffLeafState::Exact);
    if (handoff_context_was_observed) {
        const auto current_reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
        const auto current_owned = load_optional_private_lease_marker(paths.lease_owned_path);
        if (!lease_marker_witness_equal(current_reserved, witness.reserved_marker) ||
            !lease_marker_witness_equal(current_owned, witness.owned_marker)) {
            fail_private_cleanup_witness_replacement();
        }
    }

    const auto after_reads = scan_private_cleanup_union_directory(paths, *witness.directory);
    if (after_reads != *witness.after_inventory) {
        fail_private_cleanup_witness_replacement();
    }
    witness.directory->require_stable();
    witness.directory->require_private_policy();
    lock.require_stable();
}
#else
void require_private_cleanup_witness_unchanged(const OOCCleanupPaths& paths, const BaseLock& lock,
                                               PrivateCleanupUnionObservationWitness& witness) {
    const auto current = observe_private_cleanup_union_locked(paths, lock);
    if (current.raw != witness.raw) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
}
#endif

void require_private_cleanup_action_witness_unchanged(PrivateCleanupActionPermit::State& state) {
    if (!state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    require_private_cleanup_witness_unchanged(state.paths, *state.lock, state.witness);
}

} // namespace

PrivateLeaseRemovalGenerationProof capture_private_lease_removal_generation_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 2>& expected_lease_id,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    const std::array<std::uint64_t, 3>& expected_owner_identity,
    const std::array<std::uint64_t, 3>& expected_owned_identity) {
    if (paths.private_directory.empty() || !lock.matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    lock.require_stable();

    PrivateLeaseRemovalGenerationProof proof{
        .parent_identity = capture_directory_identity_locked(paths.private_directory.parent_path()),
        .expected_lease_id = expected_lease_id,
        .expected_directory_identity = expected_directory_identity,
        .expected_owner_identity = expected_owner_identity,
        .expected_owned_identity = expected_owned_identity,
    };
    proof.owned = load_optional_private_lease_marker(paths.lease_owned_path);
    proof.final_directory_identity = inspect_directory_identity_locked(paths.private_directory);

    if (!proof.owned) {
        proof.reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
        proof.reserved_pending =
            load_optional_private_lease_marker(paths.lease_reserved_pending_path);
        proof.owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
        proof.staging_directory_identity =
            inspect_directory_identity_locked(private_lease_staging_path(paths, expected_lease_id));
        if (proof.reserved || proof.reserved_pending || proof.owned_pending ||
            proof.final_directory_identity || proof.staging_directory_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        lock.require_stable();
        return proof;
    }

    if (proof.owned->record.lease_id != expected_lease_id ||
        proof.owned->record.directory_identity != expected_directory_identity ||
        proof.owned->record.owner_identity != expected_owner_identity ||
        proof.owned->identity != expected_owned_identity) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    proof.owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
    if (proof.owned_pending) {
        if (proof.owned_pending->record != proof.owned->record) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
    }
    validate_private_lease_record_context(proof.owned->record, paths, proof.parent_identity,
                                          lock.identity());
    if (proof.owned->record.phase != PrivateLeasePhase::Owned) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    proof.reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
    if (proof.reserved) {
        validate_private_lease_record_context(proof.reserved->record, paths, proof.parent_identity,
                                              lock.identity());
        validate_private_lease_record_chain(proof.reserved->record, proof.owned->record);
    }
    proof.reserved_pending = load_optional_private_lease_marker(paths.lease_reserved_pending_path);
    if (proof.reserved_pending) {
        if (!proof.reserved || proof.reserved_pending->record != proof.reserved->record) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
    }

    const auto staging_path = private_lease_staging_path(paths, proof.owned->record.lease_id);
    proof.staging_directory_identity = inspect_directory_identity_locked(staging_path);
    if (proof.staging_directory_identity && proof.final_directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (proof.staging_directory_identity &&
        *proof.staging_directory_identity != proof.owned->record.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (proof.final_directory_identity &&
        *proof.final_directory_identity != proof.owned->record.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    const auto owner_directory =
        proof.final_directory_identity
            ? std::optional<std::filesystem::path>(paths.private_directory)
            : (proof.staging_directory_identity ? std::optional<std::filesystem::path>(staging_path)
                                                : std::nullopt);
    if (owner_directory) {
        const auto owner = inspect_private_lease_marker(private_lease_owner_path(*owner_directory));
        if (owner.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, owner.error);
        }
        if (owner.kind == InspectKind::Rejected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        proof.owner_present = owner.kind == InspectKind::Present;
        if (proof.owner_present) {
            const auto owner_record = parse_private_lease_marker(owner.bytes);
            if (owner_record != owner_record_for(proof.owned->record) ||
                stable_identity(owner.identity) != proof.owned->record.owner_identity) {
                fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                     protocol_error());
            }
        }
    }

    if (proof.staging_directory_identity) {
        const bool preactive_pair_rollback =
            proof.reserved &&
            proof.owned->record.capability == PrivateLeaseCapability::RollbackPreactivePairAndLease;
        const bool owner_in_inventory =
            preactive_pair_rollback
                ? inspect_private_lease_preactive_entries(staging_path, paths).owner
                : inspect_private_lease_control_entries(staging_path).owner;
        if (owner_in_inventory != proof.owner_present) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
    }

    lock.require_stable();
    return proof;
}

namespace {

void require_private_lease_cleanup_handoff_generation_shape(
    const OOCCleanupPaths& paths, const PrivateLeaseRemovalGenerationProof& proof) {
    if (!proof.owned || !proof.reserved || !proof.final_directory_identity ||
        proof.staging_directory_identity || !proof.owner_present ||
        proof.owned->record.lease_id != proof.expected_lease_id ||
        proof.reserved->record.lease_id != proof.expected_lease_id ||
        proof.owned->record.directory_identity != proof.expected_directory_identity ||
        proof.owned->record.owner_identity != proof.expected_owner_identity ||
        proof.owned->identity != proof.expected_owned_identity ||
        *proof.final_directory_identity != proof.expected_directory_identity) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    inspect_private_lease_transaction_entries(paths.private_directory, paths);
}

[[nodiscard]] PrivateLeaseRemovalGenerationProof
capture_private_lease_cleanup_handoff_generation_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 2>& expected_lease_id,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    const std::array<std::uint64_t, 3>& expected_owner_identity,
    const std::array<std::uint64_t, 3>& expected_owned_identity) {
    auto proof = capture_private_lease_removal_generation_locked(
        paths, lock, expected_lease_id, expected_directory_identity, expected_owner_identity,
        expected_owned_identity);
    require_private_lease_cleanup_handoff_generation_shape(paths, proof);
    lock.require_stable();
    return proof;
}

} // namespace

PrivateCleanupActionAdmission admit_private_cleanup_action_locked(const OOCCleanupPaths& paths,
                                                                  std::shared_ptr<BaseLock> lock,
                                                                  PrivateNamespaceAction action) {
    if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    PrivateCleanupActionClaimGuard claim(*lock);
    if (!claim.acquired()) {
        return PrivateCleanupActionAdmission(private_cleanup_action_busy());
    }
    lock->require_stable();
    auto witness = observe_private_cleanup_union_locked(paths, *lock);
    const auto decision = decide_private_namespace_action(witness.raw, action);
    lock->require_stable();
    if (const auto blocked = private_cleanup_union_blocked_result(decision)) {
        return PrivateCleanupActionAdmission(*blocked);
    }
    auto state =
        std::unique_ptr<PrivateCleanupActionPermit::State>(new PrivateCleanupActionPermit::State(
            paths, std::move(lock), decision, std::move(witness)));
    claim.transfer_to_permit();
    return PrivateCleanupActionAdmission(PrivateCleanupActionPermit(std::move(state)));
}

PrivateCleanupActionAdmission admit_private_lease_cleanup_handoff_locked(
    const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock, const OOCCleanupRequest& request,
    const OwnershipProof& ownership_proof, const std::array<std::uint64_t, 2>& expected_lease_id,
    const std::array<std::uint64_t, 3>& expected_directory_identity,
    const std::array<std::uint64_t, 3>& expected_owner_identity,
    const std::array<std::uint64_t, 3>& expected_owned_identity) {
    if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path) ||
        request.base_path != paths.base_path || request.store_id == 0 || !request.exact ||
        !expectation_is_well_formed(*request.exact) ||
        ownership_proof.base_path != paths.base_path ||
        ownership_proof.store_id != request.store_id) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    PrivateCleanupActionClaimGuard claim(*lock);
    if (!claim.acquired()) {
        return PrivateCleanupActionAdmission(private_cleanup_action_busy());
    }
    lock->require_stable();
    auto witness = observe_private_cleanup_union_locked(paths, *lock);
    const auto decision = decide_private_namespace_action(
        witness.raw, PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff);
    lock->require_stable();
    if (const auto blocked = private_cleanup_union_blocked_result(decision)) {
        return PrivateCleanupActionAdmission(*blocked);
    }

    auto generation = capture_private_lease_cleanup_handoff_generation_locked(
        paths, *lock, expected_lease_id, expected_directory_identity, expected_owner_identity,
        expected_owned_identity);
    const auto intent = capture_source_pair(paths, request.store_id);
    if (!request_matches_intent(request, intent) ||
        !ownership_proof_matches(ownership_proof, paths, intent)) {
        fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None, protocol_error());
    }
    require_source_pair_unchanged(paths, intent);
    std::optional<FileIdentity> original_canonical_identity;
    const auto original_canonical = inspect_file(paths.intent_path, MARKER_BYTES, true);
    if (original_canonical.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, original_canonical.error);
    }
    if (original_canonical.kind == InspectKind::Rejected) {
        fail(OOCCleanupStatus::IntentCorrupt, OOCCleanupStage::None, protocol_error());
    }
    if (original_canonical.kind == InspectKind::Present) {
        if (parse_marker(original_canonical.bytes, INTENT_MAGIC) != intent) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        original_canonical_identity = original_canonical.identity;
    }
    lock->require_stable();

    auto state =
        std::unique_ptr<PrivateCleanupActionPermit::State>(new PrivateCleanupActionPermit::State(
            paths, std::move(lock), decision, std::move(witness)));
    claim.transfer_to_permit();
    state->publication_generation = std::move(generation);
    state->publication_intent = intent;
    state->publication_original_canonical_identity = original_canonical_identity;
    return PrivateCleanupActionAdmission(PrivateCleanupActionPermit(std::move(state)));
}

PrivateLeaseRemovalAdmission
admit_private_lease_removal_locked(const OOCCleanupPaths& paths, std::shared_ptr<BaseLock> lock,
                                   const std::array<std::uint64_t, 2>& expected_lease_id,
                                   const std::array<std::uint64_t, 3>& expected_directory_identity,
                                   const std::array<std::uint64_t, 3>& expected_owner_identity,
                                   const std::array<std::uint64_t, 3>& expected_owned_identity) {
    if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    PrivateCleanupActionClaimGuard claim(*lock);
    if (!claim.acquired()) {
        return PrivateLeaseRemovalAdmission(private_cleanup_action_busy());
    }
    lock->require_stable();
    auto witness = observe_private_cleanup_union_locked(paths, *lock);
    const auto decision =
        decide_private_namespace_action(witness.raw, PrivateNamespaceAction::RemovePrivateLease);
    lock->require_stable();
    if (const auto blocked = private_cleanup_union_blocked_result(decision)) {
        return PrivateLeaseRemovalAdmission(*blocked);
    }

    auto generation = capture_private_lease_removal_generation_locked(
        paths, *lock, expected_lease_id, expected_directory_identity, expected_owner_identity,
        expected_owned_identity);
    lock->require_stable();
    auto state =
        std::unique_ptr<PrivateCleanupActionPermit::State>(new PrivateCleanupActionPermit::State(
            paths, std::move(lock), decision, std::move(witness)));
    claim.transfer_to_permit();
    return PrivateLeaseRemovalAdmission(PrivateCleanupActionPermit(std::move(state)),
                                        std::move(generation));
}

const BaseLock& begin_private_cleanup_action(PrivateCleanupActionPermit& permit,
                                             const OOCCleanupPaths& paths,
                                             PrivateNamespaceAction expected_action) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (state.action_started) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.action_started = true;
    if (state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        state.decision.action != expected_action ||
        state.decision.action == PrivateNamespaceAction::Count ||
        !decision_delegates_existing_runtime(state.decision) ||
        !private_cleanup_paths_equal(state.paths, paths) || !state.lock ||
        !state.lock->matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
    return *state.lock;
}

void bind_private_lease_removal_generation(PrivateCleanupActionPermit& permit,
                                           const PrivateLeaseRemovalGenerationProof& proof) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (state.removal_generation_binding_attempted) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.removal_generation_binding_attempted = true;
    const auto prior_handoff_state = state.handoff_state;
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (!state.action_started || prior_handoff_state != PrivateHandoffConsumptionState::Fresh ||
        state.decision.action != PrivateNamespaceAction::RemovePrivateLease ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    const auto current = capture_private_lease_removal_generation_locked(
        state.paths, *state.lock, proof.expected_lease_id, proof.expected_directory_identity,
        proof.expected_owner_identity, proof.expected_owned_identity);
    if (current != proof) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }

    const bool retained_handoff = std::any_of(
        state.witness.raw.handoff_markers.begin(), state.witness.raw.handoff_markers.end(),
        [](const auto marker) { return marker == PrivateHandoffLeafObservationKind::Exact; });
    if (retained_handoff && (!proof.owned || !proof.owner_present)) {
        fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
             protocol_error());
    }
    state.removal_generation = proof;
    state.handoff_state = PrivateHandoffConsumptionState::Fresh;
}

OOCPrivateHandoffInspectResult
reconcile_private_handoff_from_permit(PrivateCleanupActionPermit& permit,
                                      PrivateNamespaceAction expected_action) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (!state.action_started || state.handoff_state != PrivateHandoffConsumptionState::Fresh) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (state.decision.action != expected_action ||
        (expected_action != PrivateNamespaceAction::RecoverPrivateLease &&
         expected_action != PrivateNamespaceAction::RemovePrivateLease) ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
    if (expected_action == PrivateNamespaceAction::RemovePrivateLease) {
        require_private_lease_removal_generation_unchanged(state);
    }
    require_private_cleanup_action_witness_unchanged(state);
    state.handoff_state = expected_action == PrivateNamespaceAction::RecoverPrivateLease
                              ? PrivateHandoffConsumptionState::RecoverConsumed
                              : PrivateHandoffConsumptionState::RemoveConsumed;

#if defined(__APPLE__)
    auto& witness = state.witness;
    if (!witness.handoff_classification) {
        return handoff_none();
    }
    const auto& classified = *witness.handoff_classification;
    if (classified.inspection.state == OOCPrivateHandoffState::Canonical) {
        if (!witness.directory || !witness.canonical_handoff ||
            !witness.canonical_handoff->record || !witness.canonical_handoff->snapshot) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        const auto confirmed = invoke_with_stable_base_lock(*state.lock, [&] {
            return util::durable_immutable_record::publish_at(
                witness.directory->native_handle(),
                state.paths.private_handoff_pending_path.filename(),
                state.paths.private_handoff_path.filename(), witness.canonical_handoff->bytes);
        });
        if (!confirmed.is_durable() || !confirmed.canonical_snapshot()) {
            const auto status =
                confirmed.status() ==
                        util::durable_immutable_record::RecordPublishStatus::platform_unsupported
                    ? OOCCleanupStatus::PlatformUnsupported
                    : OOCCleanupStatus::DurabilityFailure;
            return handoff_failure(status, OOCPrivateHandoffState::TaintedPreserved,
                                   confirmed.native_error());
        }
        witness.directory->require_stable();
        witness.directory->require_private_policy();
        state.lock->require_stable();
        return handoff_present(*witness.canonical_handoff->record, *confirmed.canonical_snapshot());
    }

    if (classified.inspection.state == OOCPrivateHandoffState::PendingOnly &&
        classified.pending_is_preactive) {
        if (!witness.directory || !witness.pending_handoff || !witness.pending_handoff->snapshot) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        invoke_with_stable_base_lock(*state.lock, [&] {
            remove_exact_private_handoff_pending(
                *witness.directory, *witness.pending_handoff->snapshot,
                state.paths.private_handoff_pending_path.filename());
        });
        witness.directory->require_private_policy();
        state.lock->require_stable();
        return handoff_none();
    }
    return classified.inspection;
#else
    return handoff_none();
#endif
}

OOCPrivateHandoffInspectResult
inspect_private_handoff_from_permit(PrivateCleanupActionPermit& permit) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (!state.action_started || state.handoff_state != PrivateHandoffConsumptionState::Fresh) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (state.decision.action != PrivateNamespaceAction::RunLegacyCleanup ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
    require_private_cleanup_action_witness_unchanged(state);

#if defined(__APPLE__)
    if (state.witness.handoff_classification) {
        const auto inspection = state.witness.handoff_classification->inspection;
        if (inspection.state != OOCPrivateHandoffState::None) {
            return inspection;
        }
    }
#endif
    state.handoff_state = PrivateHandoffConsumptionState::LegacyObserved;
    return handoff_none();
}

namespace {

void require_private_lease_cleanup_handoff_bindings_unchanged(
    PrivateCleanupActionPermit::State& state) {
    if (!state.publication_generation || !state.publication_intent || !state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    try {
        const auto current_generation = capture_private_lease_cleanup_handoff_generation_locked(
            state.paths, *state.lock, state.publication_generation->expected_lease_id,
            state.publication_generation->expected_directory_identity,
            state.publication_generation->expected_owner_identity,
            state.publication_generation->expected_owned_identity);
        if (current_generation != *state.publication_generation) {
            fail_private_cleanup_witness_replacement();
        }
        require_source_pair_unchanged(state.paths, *state.publication_intent);
    } catch (const Failure& failure) {
        if (failure.status == OOCCleanupStatus::IoFailure ||
            failure.status == OOCCleanupStatus::UnexpectedFailure) {
            throw;
        }
        fail_private_cleanup_witness_replacement();
    }
    state.lock->require_stable();
}

#if defined(__APPLE__)
[[nodiscard]] bool
optional_cleanup_leaf_witness_equal(const std::optional<PrivateCleanupLeafWitness>& lhs,
                                    const std::optional<PrivateCleanupLeafWitness>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs || cleanup_leaf_witness_equal(*lhs, *rhs);
}

[[nodiscard]] bool
optional_handoff_leaf_witness_equal(const std::optional<LoadedPrivateHandoffLeaf>& lhs,
                                    const std::optional<LoadedPrivateHandoffLeaf>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs || handoff_leaf_witness_equal(*lhs, *rhs);
}

[[nodiscard]] bool publication_inventory_equal_except_intent(
    const std::optional<PrivateCleanupUnionDirectoryInventory>& lhs,
    const std::optional<PrivateCleanupUnionDirectoryInventory>& rhs) noexcept {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    if (lhs->foreign != rhs->foreign) {
        return false;
    }
    for (std::size_t slot = 0; slot < lhs->leaves.size(); ++slot) {
        if (slot == static_cast<std::size_t>(PrivateCleanupUnionDirectoryEntry::Intent) ||
            slot == static_cast<std::size_t>(PrivateCleanupUnionDirectoryEntry::IntentPending)) {
            continue;
        }
        if (lhs->leaves[slot] != rhs->leaves[slot]) {
            return false;
        }
    }
    return true;
}
#endif

void require_publication_non_intent_witness_unchanged(
    PrivateCleanupActionPermit::State& state,
    const PrivateCleanupUnionObservationWitness& current) {
    const auto& original = state.witness;
    const auto staged_slot = static_cast<std::size_t>(PrivateCleanupMarkerSlot::Staged);
    const auto staged_pending_slot =
        static_cast<std::size_t>(PrivateCleanupMarkerSlot::StagedPending);
    if (current.raw.namespace_foreign != original.raw.namespace_foreign ||
        current.raw.cleanup_markers[staged_slot] != original.raw.cleanup_markers[staged_slot] ||
        current.raw.cleanup_markers[staged_pending_slot] !=
            original.raw.cleanup_markers[staged_pending_slot] ||
        current.raw.handoff_markers != original.raw.handoff_markers) {
        fail_private_cleanup_witness_replacement();
    }

#if defined(__APPLE__)
    if (current.directory == nullptr || original.directory == nullptr ||
        current.directory->identity() != original.directory->identity() ||
        !current.before_inventory || !current.after_inventory ||
        *current.before_inventory != *current.after_inventory || !original.before_inventory ||
        !original.after_inventory || *original.before_inventory != *original.after_inventory ||
        !publication_inventory_equal_except_intent(current.before_inventory,
                                                   original.before_inventory) ||
        !publication_inventory_equal_except_intent(current.after_inventory,
                                                   original.after_inventory) ||
        !optional_cleanup_leaf_witness_equal(current.cleanup_leaves[staged_slot],
                                             original.cleanup_leaves[staged_slot]) ||
        !optional_cleanup_leaf_witness_equal(current.cleanup_leaves[staged_pending_slot],
                                             original.cleanup_leaves[staged_pending_slot]) ||
        !optional_handoff_leaf_witness_equal(current.canonical_handoff,
                                             original.canonical_handoff) ||
        !optional_handoff_leaf_witness_equal(current.pending_handoff, original.pending_handoff) ||
        !lease_marker_witness_equal(current.reserved_marker, original.reserved_marker) ||
        !lease_marker_witness_equal(current.owned_marker, original.owned_marker)) {
        fail_private_cleanup_witness_replacement();
    }
#endif
    state.lock->require_stable();
}

[[nodiscard]] InspectResult
require_exact_publication_marker(const std::filesystem::path& path, bool pending,
                                 const IntentRecord& expected,
                                 std::optional<FileIdentity> expected_identity = std::nullopt) {
    auto inspected = inspect_file(path, MARKER_BYTES, pending);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind != InspectKind::Present ||
        parse_marker(inspected.bytes, INTENT_MAGIC) != expected ||
        (expected_identity && inspected.identity != *expected_identity)) {
        fail_private_cleanup_witness_replacement();
    }
    return inspected;
}

void require_publication_marker_missing(const std::filesystem::path& path) {
    const auto inspected = inspect_file(path, 0, false);
    if (inspected.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, inspected.error);
    }
    if (inspected.kind != InspectKind::Missing) {
        fail_private_cleanup_witness_replacement();
    }
}

void require_publication_non_intent_names_missing(const OOCCleanupPaths& paths) {
    require_publication_marker_missing(paths.staged_path);
    require_publication_marker_missing(paths.staged_pending_path);
    require_publication_marker_missing(paths.quarantine_index_path);
    require_publication_marker_missing(paths.quarantine_data_path);
}

void require_publication_original_witness(PrivateCleanupActionPermit::State& state) {
    require_private_cleanup_action_witness_unchanged(state);
    require_publication_non_intent_names_missing(state.paths);
    require_private_lease_cleanup_handoff_bindings_unchanged(state);
}

void capture_publication_phase_witness(PrivateCleanupActionPermit::State& state) {
    if (!state.lock) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto current = observe_private_cleanup_union_locked(state.paths, *state.lock);
    require_publication_non_intent_witness_unchanged(state, current);
    require_publication_non_intent_names_missing(state.paths);
    require_private_lease_cleanup_handoff_bindings_unchanged(state);
    state.publication_phase_witness = std::move(current);
}

void require_publication_phase_witness_unchanged(PrivateCleanupActionPermit::State& state) {
    if (!state.lock || !state.publication_phase_witness) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    require_private_cleanup_witness_unchanged(state.paths, *state.lock,
                                              *state.publication_phase_witness);
    require_publication_non_intent_names_missing(state.paths);
    require_private_lease_cleanup_handoff_bindings_unchanged(state);
}

void require_publication_pending_owned_or_missing(const OOCCleanupPaths& paths,
                                                  const IntentRecord& intent) {
    const auto expected = serialize_marker(intent, INTENT_MAGIC);
    const auto pending = inspect_pending_file(paths.intent_pending_path);
    if (pending.kind == InspectKind::Error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, pending.error);
    }
    if (pending.kind == InspectKind::Missing) {
        return;
    }
    if (pending.kind != InspectKind::Present ||
        (!marker_bytes_equal(pending.bytes, expected) &&
         !pending_is_recognizable_owned(pending.bytes, expected))) {
        fail_private_cleanup_witness_replacement();
    }
}

} // namespace

OOCPrivateHandoffInspectResult
inspect_private_lease_cleanup_handoff_from_permit(PrivateCleanupActionPermit& permit) {
    if (!permit.state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *permit.state_;
    if (!state.action_started || state.handoff_state != PrivateHandoffConsumptionState::Fresh) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (state.decision.action != PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock || !state.publication_generation || !state.publication_intent) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.lock->require_stable();
    require_publication_original_witness(state);

#if defined(__APPLE__)
    if (state.witness.handoff_classification) {
        const auto inspection = state.witness.handoff_classification->inspection;
        if (inspection.state != OOCPrivateHandoffState::None) {
            return inspection;
        }
    }
#endif
    state.handoff_state = PrivateHandoffConsumptionState::PublicationObserved;
    return handoff_none();
}

void authorize_private_cleanup_mutation(PrivateCleanupMutationGate& gate,
                                        const OOCCleanupPaths& paths, const BaseLock& lock,
                                        PrivateCleanupMutationBoundary boundary) {
    if (gate.state_ == PrivateCleanupMutationGate::State::Failed) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    const auto prior_gate_state = gate.state_;
    gate.state_ = PrivateCleanupMutationGate::State::Failed;
    if (gate.permit_ == nullptr || !gate.permit_->state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *gate.permit_->state_;
    const auto prior_handoff_state = state.handoff_state;
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (!state.action_started ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock || state.lock.get() != &lock || !lock.matches(paths.lock_path) ||
        !private_cleanup_paths_equal(state.paths, paths)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    lock.require_stable();

    if (state.decision.action == PrivateNamespaceAction::RunLegacyCleanup) {
        const bool already_authorized =
            prior_gate_state == PrivateCleanupMutationGate::State::LegacyAuthorized;
        if ((prior_gate_state != PrivateCleanupMutationGate::State::Fresh && !already_authorized) ||
            prior_handoff_state != (already_authorized
                                        ? PrivateHandoffConsumptionState::LegacyMutationAuthorized
                                        : PrivateHandoffConsumptionState::LegacyObserved)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        if (!already_authorized) {
            require_private_cleanup_action_witness_unchanged(state);
        }
        state.handoff_state = PrivateHandoffConsumptionState::LegacyMutationAuthorized;
        gate.state_ = PrivateCleanupMutationGate::State::LegacyAuthorized;
        return;
    }

    if (state.decision.action != PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff ||
        (prior_handoff_state != PrivateHandoffConsumptionState::PublicationObserved &&
         prior_handoff_state != PrivateHandoffConsumptionState::PublicationMutationAuthorized)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    switch (boundary) {
    case PrivateCleanupMutationBoundary::PendingPreparation:
        if (prior_gate_state != PrivateCleanupMutationGate::State::Fresh) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_publication_original_witness(state);
        require_publication_marker_missing(paths.intent_path);
        gate.state_ = PrivateCleanupMutationGate::State::PublicationPendingPreparationAuthorized;
        break;
    case PrivateCleanupMutationBoundary::CanonicalRename:
        if (prior_gate_state != PrivateCleanupMutationGate::State::PublicationPendingDurable ||
            !gate.publication_pending_identity_ || !state.publication_intent) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_publication_phase_witness_unchanged(state);
        require_publication_marker_missing(paths.intent_path);
        (void)require_exact_publication_marker(paths.intent_pending_path, true,
                                               *state.publication_intent,
                                               gate.publication_pending_identity_);
        gate.state_ = PrivateCleanupMutationGate::State::PublicationCanonicalRenameAuthorized;
        break;
    case PrivateCleanupMutationBoundary::PendingRemoval:
        if (prior_gate_state != PrivateCleanupMutationGate::State::PublicationReceiptCommitted ||
            !gate.publication_receipt_committed_ || !gate.publication_canonical_identity_ ||
            !state.publication_intent) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        require_publication_phase_witness_unchanged(state);
        (void)require_exact_publication_marker(paths.intent_path, false, *state.publication_intent,
                                               gate.publication_canonical_identity_);
        gate.state_ = PrivateCleanupMutationGate::State::PublicationPendingRemovalAuthorized;
        break;
    case PrivateCleanupMutationBoundary::PublicationComplete:
        if ((prior_gate_state != PrivateCleanupMutationGate::State::PublicationReceiptCommitted &&
             prior_gate_state !=
                 PrivateCleanupMutationGate::State::PublicationPendingRemovalAuthorized) ||
            !gate.publication_receipt_committed_ || !gate.publication_canonical_identity_ ||
            !state.publication_intent) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        capture_publication_phase_witness(state);
        (void)require_exact_publication_marker(paths.intent_path, false, *state.publication_intent,
                                               gate.publication_canonical_identity_);
        require_publication_marker_missing(paths.intent_pending_path);
        gate.publication_pending_identity_.reset();
        gate.state_ = PrivateCleanupMutationGate::State::PublicationCompleted;
        break;
    case PrivateCleanupMutationBoundary::Generic:
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    state.handoff_state = PrivateHandoffConsumptionState::PublicationMutationAuthorized;
}

void record_private_cleanup_pending_successor(PrivateCleanupMutationGate& gate,
                                              const OOCCleanupPaths& paths, const BaseLock& lock,
                                              const std::filesystem::path& pending_path,
                                              const FileIdentity& identity) {
    if (gate.state_ == PrivateCleanupMutationGate::State::Failed || gate.permit_ == nullptr ||
        !gate.permit_->state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *gate.permit_->state_;
    if (state.decision.action == PrivateNamespaceAction::RunLegacyCleanup) {
        return;
    }

    const auto prior_gate_state = gate.state_;
    gate.state_ = PrivateCleanupMutationGate::State::Failed;
    const auto prior_handoff_state = state.handoff_state;
    state.handoff_state = PrivateHandoffConsumptionState::Failed;
    if (!state.action_started ||
        state.decision.action != PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff ||
        (prior_handoff_state != PrivateHandoffConsumptionState::PublicationObserved &&
         prior_handoff_state != PrivateHandoffConsumptionState::PublicationMutationAuthorized) ||
        (prior_gate_state != PrivateCleanupMutationGate::State::Fresh &&
         prior_gate_state !=
             PrivateCleanupMutationGate::State::PublicationPendingPreparationAuthorized) ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock || state.lock.get() != &lock || !lock.matches(paths.lock_path) ||
        !private_cleanup_paths_equal(state.paths, paths) ||
        pending_path != paths.intent_pending_path || !state.publication_intent) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    lock.require_stable();
    if (prior_gate_state == PrivateCleanupMutationGate::State::Fresh) {
        require_publication_original_witness(state);
    }
    require_publication_marker_missing(paths.intent_path);
    const auto pending = require_exact_publication_marker(paths.intent_pending_path, true,
                                                          *state.publication_intent, identity);
    capture_publication_phase_witness(state);
    gate.publication_pending_identity_ = pending.identity;
    gate.state_ = PrivateCleanupMutationGate::State::PublicationPendingDurable;
    state.handoff_state = PrivateHandoffConsumptionState::PublicationMutationAuthorized;
}

void commit_private_cleanup_canonical(PrivateCleanupMutationGate& gate,
                                      const OOCCleanupPaths& paths, const BaseLock& lock,
                                      const FileIdentity& durable_identity,
                                      bool renamed_from_pending, bool pending_must_remain) {
    if (gate.permit_ == nullptr || !gate.permit_->state_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    auto& state = *gate.permit_->state_;
    const auto prior_gate_state = gate.state_;
    const auto prior_handoff_state = state.handoff_state;

    // The caller sets the escrowed receipt's sticky commit bit before entering
    // this bridge. Mirror that fact before any successor observation: a later
    // union, lease, pair, or pending-cleanup failure must never revive it.
    gate.publication_receipt_committed_ = true;
    gate.publication_canonical_identity_ = durable_identity;
    gate.state_ = PrivateCleanupMutationGate::State::Failed;
    state.handoff_state = PrivateHandoffConsumptionState::Failed;

    const bool original_canonical = prior_gate_state == PrivateCleanupMutationGate::State::Fresh;
    const bool promoted_pending =
        prior_gate_state == PrivateCleanupMutationGate::State::PublicationCanonicalRenameAuthorized;
    if (!state.action_started ||
        state.decision.action != PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff ||
        state.creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id()) ||
        !state.lock || state.lock.get() != &lock || !lock.matches(paths.lock_path) ||
        !private_cleanup_paths_equal(state.paths, paths) || !state.publication_intent ||
        (prior_handoff_state != PrivateHandoffConsumptionState::PublicationObserved &&
         prior_handoff_state != PrivateHandoffConsumptionState::PublicationMutationAuthorized) ||
        (!original_canonical && !promoted_pending) ||
        (renamed_from_pending && (!promoted_pending || pending_must_remain)) ||
        (pending_must_remain && (!promoted_pending || renamed_from_pending)) ||
        (promoted_pending && !renamed_from_pending && !pending_must_remain)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    if (original_canonical) {
        if (renamed_from_pending || pending_must_remain ||
            !state.publication_original_canonical_identity ||
            durable_identity != *state.publication_original_canonical_identity) {
            fail_private_cleanup_witness_replacement();
        }
    } else if (!gate.publication_pending_identity_) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    } else if (renamed_from_pending &&
               !same_native_file(durable_identity, *gate.publication_pending_identity_)) {
        fail_private_cleanup_witness_replacement();
    }

    if (renamed_from_pending) {
        require_publication_marker_missing(paths.intent_pending_path);
    } else if (pending_must_remain) {
        (void)require_exact_publication_marker(paths.intent_pending_path, true,
                                               *state.publication_intent,
                                               gate.publication_pending_identity_);
    } else {
        require_publication_pending_owned_or_missing(paths, *state.publication_intent);
    }

    capture_publication_phase_witness(state);
    gate.state_ = PrivateCleanupMutationGate::State::PublicationReceiptCommitted;
    state.handoff_state = PrivateHandoffConsumptionState::PublicationMutationAuthorized;
}

OOCCleanupResult recover_private_lease_locked(const OOCCleanupPaths& paths,
                                              std::shared_ptr<BaseLock> lock,
                                              const OOCPrivateLeaseTestHooks& hooks) {
    if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    auto admission = admit_private_cleanup_action_locked(
        paths, std::move(lock), PrivateNamespaceAction::RecoverPrivateLease);
    if (admission.blocked) {
        return *admission.blocked;
    }
    if (!admission.permit) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    auto permit = std::move(*admission.permit);
    admission.permit.reset();
    const auto& held_lock =
        begin_private_cleanup_action(permit, paths, PrivateNamespaceAction::RecoverPrivateLease);
    if (invoke_with_stable_base_lock(held_lock, [&] {
            return should_interrupt_private_lease(
                hooks, OOCPrivateLeaseFaultPoint::RecoveryPermitAcquired);
        })) {
        return private_lease_interrupted();
    }

    const auto handoff =
        reconcile_private_handoff_from_permit(permit, PrivateNamespaceAction::RecoverPrivateLease);
    if (handoff.state != OOCPrivateHandoffState::None) {
        return handoff.result;
    }
    const auto parent = paths.private_directory.parent_path();
    sync_parent_directory(parent, OOCCleanupStage::None);
    const auto parent_identity = capture_directory_identity_locked(parent);

    auto reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
    if (!reserved) {
        const auto pending = load_optional_private_lease_marker(paths.lease_reserved_pending_path);
        if (pending) {
            validate_private_lease_record_context(pending->record, paths, parent_identity,
                                                  held_lock.identity());
            if (pending->record.phase != PrivateLeasePhase::Reserved) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }

            // A pending leaf is never a deletion capability and recovery must
            // not promote it into one. The pre-canonical crash boundary occurs
            // before mkdir, so only a completely directory-free state may
            // discard this exact no-authority publication leaf.
            const auto staging_path = private_lease_staging_path(paths, pending->record.lease_id);
            if (inspect_directory_identity_locked(staging_path) ||
                inspect_directory_identity_locked(paths.private_directory) ||
                load_optional_private_lease_marker(paths.lease_owned_path) ||
                load_optional_private_lease_marker(paths.lease_owned_pending_path)) {
                return OOCCleanupResult{
                    .status = OOCCleanupStatus::RecoveryRequired,
                    .stage = OOCCleanupStage::None,
                    .native_error = protocol_error(),
                };
            }
            invoke_with_stable_base_lock(held_lock, [&] {
                remove_matching_private_lease_pending(paths.lease_reserved_pending_path,
                                                      pending->record);
            });
            return private_lease_completed();
        }
    }

    auto owned = load_optional_private_lease_marker(paths.lease_owned_path);
    if (owned) {
        return recover_owned_private_lease_locked(paths, held_lock, parent_identity, *owned,
                                                  reserved, hooks);
    }

    if (reserved) {
        rollback_reserved_staging_locked(paths, held_lock, parent_identity, *reserved);
        held_lock.require_stable();
        return private_lease_completed();
    }

    const auto owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
    if (owned_pending || inspect_directory_identity_locked(paths.private_directory)) {
        fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None, protocol_error());
    }
    held_lock.require_stable();
    return private_lease_no_transaction();
}

PrivateCleanupUnionRawObservation
observe_private_cleanup_union_for_test(const std::filesystem::path& base_path,
                                       PrivateCleanupUnionObservationTestHooks hooks) {
    const auto paths = freeze_paths(base_path);
    if (paths.private_directory.empty()) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    BaseLock lock(paths.lock_path, false);
    return observe_private_cleanup_union_locked(paths, lock, hooks).raw;
}

OOCCleanupResult run_transaction(const std::filesystem::path& requested_base,
                                 const OOCCleanupRequest* request, bool allow_begin,
                                 const OwnershipProof* ownership_proof, bool* consume_receipt,
                                 const OOCCleanupTestHooks& hooks) {
    const OOCCleanupPaths paths = freeze_paths(requested_base);
    if (paths.private_directory.empty()) {
        BaseLock lock(paths.lock_path, true);
        return run_transaction_locked(paths, lock, request, allow_begin, ownership_proof,
                                      consume_receipt, hooks, false);
    }

    auto lock = std::make_shared<BaseLock>(paths.lock_path, false);
    auto admission = admit_private_cleanup_action_locked(paths, std::move(lock),
                                                         PrivateNamespaceAction::RunLegacyCleanup);
    if (admission.blocked) {
        return *admission.blocked;
    }
    if (!admission.permit) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    auto permit = std::move(*admission.permit);
    admission.permit.reset();
    const auto& held_lock =
        begin_private_cleanup_action(permit, paths, PrivateNamespaceAction::RunLegacyCleanup);
    if (invoke_with_stable_base_lock(held_lock, [&] {
            return should_interrupt(hooks, OOCCleanupFaultPoint::LegacyCleanupPermitAcquired);
        })) {
        return interrupted(OOCCleanupStage::None);
    }

    const auto handoff = inspect_private_handoff_from_permit(permit);
    if (handoff.state != OOCPrivateHandoffState::None) {
        return handoff.result;
    }
    PrivateCleanupMutationGate mutation_gate(permit);
    return run_transaction_locked(paths, held_lock, request, allow_begin, ownership_proof,
                                  consume_receipt, hooks, false, &mutation_gate);
}

std::optional<OOCCleanupResult> preflight_private_cleanup_union_for_transaction_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock, bool publish_intent_only) {
    return project_private_cleanup_union_preflight(
        paths, lock,
        publish_intent_only ? PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff
                            : PrivateNamespaceAction::RunLegacyCleanup);
}

std::optional<OOCCleanupResult>
preflight_private_cleanup_union_for_reservation_locked(const OOCCleanupPaths& paths,
                                                       const BaseLock& lock) {
    return project_private_cleanup_union_preflight(paths, lock,
                                                   PrivateNamespaceAction::ReservePrivateLease);
}

} // namespace gnfs::relation::ooc_cleanup_detail

namespace gnfs::relation {

OOCCleanupResult OOCCleanupTransaction::publish_private_lease_cleanup_handoff(
    OOCCleanupOwnershipReceipt& pair_ownership, OOCPrivateLeaseOwnershipReceipt& lease,
    const OOCExactCleanupExpectation& exact, OOCCleanupTestHooks hooks) noexcept {
    if (pair_ownership.spent_ || pair_ownership.store_id_ == 0 || lease.spent_ || lease.active_ ||
        !lease.live_lock_ || !ooc_cleanup_detail::expectation_is_well_formed(exact)) {
        return OOCCleanupResult{
            .status = OOCCleanupStatus::InvalidRequest,
            .stage = OOCCleanupStage::None,
            .native_error = ooc_cleanup_detail::invalid_argument_error(),
        };
    }

    const OOCCleanupRequest request{
        .base_path = pair_ownership.base_path_,
        .store_id = pair_ownership.store_id_,
        .exact = exact,
    };
    const ooc_cleanup_detail::OwnershipProof ownership_proof{
        .base_path = pair_ownership.base_path_,
        .store_id = pair_ownership.store_id_,
        .index_identity =
            {
                pair_ownership.index_identity_.first,
                pair_ownership.index_identity_.second,
                pair_ownership.index_identity_.third,
            },
        .data_identity =
            {
                pair_ownership.data_identity_.first,
                pair_ownership.data_identity_.second,
                pair_ownership.data_identity_.third,
            },
    };
    const auto lease_base_path = lease.base_path_;
    const auto lease_private_directory = lease.private_directory_;
    const auto lease_lock_path = lease.lock_path_;
    const auto expected_lease_id = lease.lease_id_;
    const auto expected_directory_identity = lease.directory_identity_;
    const auto expected_owner_identity = lease.owner_identity_;
    const auto expected_owned_identity = lease.owned_identity_;
    auto retained_lock = lease.live_lock_;

    // The caller escrows this receipt outside the writer before entering here.
    // Mark it unavailable during every test callback; only a pre-canonical
    // return may roll this in-memory attempt back to fresh.
    pair_ownership.spent_ = true;
    bool consume_receipt = false;
    const auto result = invoke([&] {
        const auto paths = ooc_cleanup_detail::freeze_paths(lease_base_path);
        if (paths.base_path != lease_base_path ||
            paths.private_directory != lease_private_directory ||
            paths.lock_path != lease_lock_path || request.base_path != lease_base_path ||
            !retained_lock || !retained_lock->matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }

        auto admission = ooc_cleanup_detail::admit_private_lease_cleanup_handoff_locked(
            paths, retained_lock, request, ownership_proof, expected_lease_id,
            expected_directory_identity, expected_owner_identity, expected_owned_identity);
        if (admission.blocked) {
            return *admission.blocked;
        }
        if (!admission.permit) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        const auto& lock = ooc_cleanup_detail::begin_private_cleanup_action(
            permit, paths,
            ooc_cleanup_detail::PrivateNamespaceAction::PublishPrivateLeaseCleanupHandoff);
        if (ooc_cleanup_detail::invoke_with_stable_base_lock(lock, [&] {
                return ooc_cleanup_detail::should_interrupt(
                    hooks, OOCCleanupFaultPoint::PrivateLeaseCleanupHandoffPermitAcquired);
            })) {
            return ooc_cleanup_detail::interrupted(OOCCleanupStage::None);
        }

        const auto handoff =
            ooc_cleanup_detail::inspect_private_lease_cleanup_handoff_from_permit(permit);
        if (handoff.state != OOCPrivateHandoffState::None) {
            return handoff.result;
        }
        ooc_cleanup_detail::PrivateCleanupMutationGate mutation_gate(permit);
        return ooc_cleanup_detail::run_transaction_locked(paths, lock, &request, true,
                                                          &ownership_proof, &consume_receipt, hooks,
                                                          true, &mutation_gate);
    });
    pair_ownership.spent_ = consume_receipt;
    return result;
}

OOCCleanupResult
OOCCleanupTransaction::remove_private_lease(OOCPrivateLeaseOwnershipReceipt& ownership,
                                            OOCPrivateLeaseTestHooks hooks) noexcept {
    if (ownership.spent_ || ownership.base_path_.empty() || ownership.private_directory_.empty() ||
        ownership.lock_path_.empty() ||
        ownership.owner_process_id_ != static_cast<std::uint64_t>(gnfs::util::process_id())) {
        return OOCCleanupResult{
            .status = OOCCleanupStatus::InvalidRequest,
            .stage = OOCCleanupStage::None,
            .native_error = ooc_cleanup_detail::invalid_argument_error(),
        };
    }

    const auto result = invoke([&] {
        const auto paths = ooc_cleanup_detail::freeze_paths(ownership.base_path_);
        if (paths.private_directory.empty() || paths.base_path != ownership.base_path_ ||
            paths.private_directory != ownership.private_directory_ ||
            paths.lock_path != ownership.lock_path_) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        std::shared_ptr<ooc_cleanup_detail::BaseLock> held_lock = ownership.live_lock_;
        if (!held_lock) {
            held_lock = std::make_shared<ooc_cleanup_detail::BaseLock>(paths.lock_path, false);
        }
        if (!held_lock->matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }

        auto admission = ooc_cleanup_detail::admit_private_lease_removal_locked(
            paths, held_lock, ownership.lease_id_, ownership.directory_identity_,
            ownership.owner_identity_, ownership.owned_identity_);
        if (admission.blocked) {
            return *admission.blocked;
        }
        if (!admission.permit || !admission.generation) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        auto generation = std::move(*admission.generation);
        admission.generation.reset();
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        const auto& retained_lock = ooc_cleanup_detail::begin_private_cleanup_action(
            permit, paths, ooc_cleanup_detail::PrivateNamespaceAction::RemovePrivateLease);
        ooc_cleanup_detail::bind_private_lease_removal_generation(permit, generation);
        if (ooc_cleanup_detail::invoke_with_stable_base_lock(retained_lock, [&] {
                return ooc_cleanup_detail::should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::RemovalPermitAcquired);
            })) {
            return ooc_cleanup_detail::private_lease_interrupted();
        }

        const auto handoff = ooc_cleanup_detail::reconcile_private_handoff_from_permit(
            permit, ooc_cleanup_detail::PrivateNamespaceAction::RemovePrivateLease);
        if (handoff.state != OOCPrivateHandoffState::None) {
            return handoff.result;
        }
        if (!generation.owned) {
            ooc_cleanup_detail::invoke_with_stable_base_lock(retained_lock, [&] {
                ooc_cleanup_detail::sync_parent_directory(paths.private_directory.parent_path(),
                                                          OOCCleanupStage::None);
            });
            return ooc_cleanup_detail::private_lease_completed();
        }

        return ooc_cleanup_detail::recover_owned_private_lease_locked(
            paths, retained_lock, generation.parent_identity, *generation.owned,
            generation.reserved, hooks);
    });
    if (result.completed()) {
        ownership.spent_ = true;
        ownership.live_lock_.reset();
    }
    return result;
}

} // namespace gnfs::relation
