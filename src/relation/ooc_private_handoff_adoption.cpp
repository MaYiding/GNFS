#include "ooc_private_handoff_adoption_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::relation::ooc_cleanup_detail {

#if defined(__APPLE__)

/// Held handle for the directory containing the persistent private-lease lock
/// and its RESERVED/OWNED records. Adoption uses this handle for every external
/// marker observation and for binding the lock and private directory names.
class AdoptionParentDirectoryHandle final {
public:
    AdoptionParentDirectoryHandle(const AdoptionParentDirectoryHandle&) = delete;
    AdoptionParentDirectoryHandle& operator=(const AdoptionParentDirectoryHandle&) = delete;
    AdoptionParentDirectoryHandle(AdoptionParentDirectoryHandle&&) = delete;
    AdoptionParentDirectoryHandle& operator=(AdoptionParentDirectoryHandle&&) = delete;

    explicit AdoptionParentDirectoryHandle(const std::filesystem::path& path) : path_(path) {
        do {
            descriptor_ = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } while (descriptor_ < 0 && errno == EINTR);
        if (descriptor_ < 0) {
            const int saved_errno = errno;
            fail(saved_errno == ELOOP || saved_errno == ENOTDIR
                     ? OOCCleanupStatus::NamespaceConflict
                     : OOCCleanupStatus::IoFailure,
                 OOCCleanupStage::None, posix_error(saved_errno));
        }

        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor_, &held) != 0) {
            const int saved_errno = errno;
            release_noexcept();
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(saved_errno));
        }
        if (::lstat(path_.c_str(), &named) != 0 || !S_ISDIR(held.st_mode) ||
            !S_ISDIR(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino) {
            const int saved_errno = errno == 0 ? EACCES : errno;
            release_noexcept();
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(saved_errno));
        }
        identity_ = stable_identity(posix_identity(held));
    }

    ~AdoptionParentDirectoryHandle() {
        release_noexcept();
    }

    [[nodiscard]] util::durable_immutable_record::NativeHandle native_handle() const noexcept {
        return static_cast<util::durable_immutable_record::NativeHandle>(descriptor_);
    }

    [[nodiscard]] const std::array<std::uint64_t, 3>& identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] bool leaf_exists(const std::filesystem::path& leaf) const {
        const auto metadata = inspect_leaf(leaf);
        return metadata.has_value();
    }

    [[nodiscard]] std::optional<std::array<std::uint64_t, 3>>
    child_directory_identity(const std::filesystem::path& leaf) const {
        const auto metadata = inspect_leaf(leaf);
        if (!metadata) {
            return std::nullopt;
        }
        if (!S_ISDIR(metadata->st_mode)) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        return stable_identity(posix_identity(*metadata));
    }

    void require_stable() const {
        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor_, &held) != 0) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
        }
        if (::lstat(path_.c_str(), &named) != 0 || !S_ISDIR(held.st_mode) ||
            !S_ISDIR(named.st_mode) || held.st_dev != named.st_dev || held.st_ino != named.st_ino ||
            stable_identity(posix_identity(held)) != identity_) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 errno == 0 ? protocol_error() : posix_error(errno));
        }
    }

    void require_lock_binding(const std::filesystem::path& leaf, const BaseLock& lock) const {
        const auto metadata = inspect_leaf(leaf);
        if (!metadata || !S_ISREG(metadata->st_mode) || metadata->st_nlink != 1 ||
            static_cast<std::uint64_t>(metadata->st_uid) !=
                static_cast<std::uint64_t>(::geteuid()) ||
            (metadata->st_mode & static_cast<mode_t>(07777)) != static_cast<mode_t>(0600) ||
            stable_identity(posix_identity(*metadata)) != lock.identity()) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
    }

    void require_private_directory_binding(const std::filesystem::path& leaf,
                                           const PrivateDirectoryHandle& directory) const {
        const auto metadata = inspect_leaf(leaf);
        if (!metadata || !S_ISDIR(metadata->st_mode) ||
            stable_identity(posix_identity(*metadata)) != directory.identity()) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
    }

private:
    [[nodiscard]] std::optional<struct stat> inspect_leaf(const std::filesystem::path& leaf) const {
        if (leaf.empty() || leaf.has_parent_path() || path_contains_nul(leaf)) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        struct stat metadata{};
        int result = -1;
        do {
            result = ::fstatat(descriptor_, leaf.c_str(), &metadata, AT_SYMLINK_NOFOLLOW);
        } while (result != 0 && errno == EINTR);
        if (result == 0) {
            return metadata;
        }
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return std::nullopt;
        }
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(saved_errno));
    }

    void release_noexcept() noexcept {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
            descriptor_ = -1;
        }
    }

    int descriptor_ = -1;
    std::filesystem::path path_;
    std::array<std::uint64_t, 3> identity_{};
};

#endif

OOCPrivateHandoffBorrowedBaseLockV1::OOCPrivateHandoffBorrowedBaseLockV1(
    int parent_descriptor, int lock_descriptor, std::string_view lock_leaf,
    std::array<std::uint64_t, 3> lock_identity,
    std::uint64_t creator_process_id) noexcept
    : parent_descriptor_(parent_descriptor), lock_descriptor_(lock_descriptor),
      lock_leaf_(lock_leaf), lock_identity_(lock_identity),
      creator_process_id_(creator_process_id) {}

OOCPrivateHandoffBorrowedBaseLockV1::OOCPrivateHandoffBorrowedBaseLockV1(
    OOCPrivateHandoffBorrowedBaseLockV1&& other) noexcept
    : parent_descriptor_(std::exchange(other.parent_descriptor_, -1)),
      lock_descriptor_(std::exchange(other.lock_descriptor_, -1)),
      lock_leaf_(std::exchange(other.lock_leaf_, {})), lock_identity_(other.lock_identity_),
      creator_process_id_(std::exchange(other.creator_process_id_, 0)),
      consumed_(std::exchange(other.consumed_, true)) {}

std::shared_ptr<BaseLock> OOCPrivateHandoffBorrowedBaseLockV1::consume(
    const OOCCleanupPaths& paths, AdoptionParentDirectoryHandle& parent) {
#if !defined(__APPLE__)
    (void)paths;
    (void)parent;
    fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
         std::make_error_code(std::errc::operation_not_supported));
#else
    if (consumed_ || parent_descriptor_ < 0 || lock_descriptor_ < 0 || lock_leaf_.empty() ||
        creator_process_id_ == 0 ||
        creator_process_id_ != static_cast<std::uint64_t>(gnfs::util::process_id())) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }
    consumed_ = true;

    const auto expected_leaf = paths.lock_path.filename().string();
    if (paths.private_directory.empty() || lock_leaf_ != expected_leaf) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    struct stat held_parent{};
    struct stat held_lock{};
    struct stat named_lock{};
    if (::fstat(parent_descriptor_, &held_parent) != 0 ||
        ::fstat(lock_descriptor_, &held_lock) != 0 ||
        ::fstatat(parent_descriptor_, expected_leaf.c_str(), &named_lock, AT_SYMLINK_NOFOLLOW) !=
            0) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
             posix_error(errno == 0 ? EACCES : errno));
    }
    const auto held_parent_identity = stable_identity(posix_identity(held_parent));
    const auto held_lock_identity = stable_identity(posix_identity(held_lock));
    const auto named_lock_identity = stable_identity(posix_identity(named_lock));
    if (!S_ISDIR(held_parent.st_mode) || parent.identity() != held_parent_identity ||
        !S_ISREG(held_lock.st_mode) || held_lock.st_nlink != 1 ||
        !S_ISREG(named_lock.st_mode) || named_lock.st_nlink != 1 ||
        held_lock_identity != lock_identity_ || named_lock_identity != lock_identity_ ||
        held_lock.st_dev != named_lock.st_dev || held_lock.st_ino != named_lock.st_ino) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    std::string retained_leaf(lock_leaf_);
    int duplicated = -1;
    do {
        duplicated = ::fcntl(lock_descriptor_, F_DUPFD_CLOEXEC, 0);
    } while (duplicated < 0 && errno == EINTR);
    if (duplicated < 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, posix_error(errno));
    }

    try {
        auto adopted = std::unique_ptr<BaseLock>(new BaseLock(
            paths.lock_path, duplicated, static_cast<int>(parent.native_handle()),
            std::move(retained_leaf), held_parent_identity, lock_identity_,
            BaseLock::AdoptInheritedOpenFileDescription{}));
        duplicated = -1;
        return std::shared_ptr<BaseLock>(std::move(adopted));
    } catch (...) {
        if (duplicated >= 0) {
            (void)::close(duplicated);
        }
        throw;
    }
#endif
}

} // namespace gnfs::relation::ooc_cleanup_detail

namespace gnfs::relation {

class ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1 final {
public:
    template <typename Operation>
    [[nodiscard]] static OOCCleanupResult invoke(Operation&& operation) noexcept {
        return OOCCleanupTransaction::invoke(std::forward<Operation>(operation));
    }

    template <typename... Arguments>
    [[nodiscard]] static OOCPrivateHandoffAdoptionReceipt
    make_receipt(Arguments&&... arguments) {
        return OOCPrivateHandoffAdoptionReceipt(std::forward<Arguments>(arguments)...);
    }
};

namespace {

[[nodiscard]] OOCPrivateHandoffAdoptionResult
adoption_failure(OOCCleanupStatus status, OOCPrivateHandoffState state,
                 std::error_code error = {}) noexcept {
    if (!error && status != OOCCleanupStatus::NoTransaction &&
        status != OOCCleanupStatus::HandoffPresent) {
        error = ooc_cleanup_detail::protocol_error();
    }
    return {
        .result =
            {
                .status = status,
                .stage = OOCCleanupStage::None,
                .native_error = error,
            },
        .state = state,
        .adoption = std::nullopt,
    };
}

[[nodiscard]] OOCPrivateHandoffAdoptionResult
adoption_from_inspection(const OOCPrivateHandoffInspectResult& inspection) noexcept {
    return {
        .result = inspection.result,
        .state = inspection.state,
        .adoption = std::nullopt,
    };
}

#if defined(__APPLE__)

using ooc_cleanup_detail::AdoptionParentDirectoryHandle;
using ooc_cleanup_detail::BaseLock;
using ooc_cleanup_detail::PrivateDirectoryHandle;
using util::durable_immutable_record::NativeHandle;
using util::durable_immutable_record::RecordSnapshot;

struct ExactOpenHookBridge final {
    OOCPrivateHandoffAdoptionTestHooks hooks;
    OOCPrivateHandoffAdoptionFaultPoint point =
        OOCPrivateHandoffAdoptionFaultPoint::IndexInitialValidationComplete;
};

[[nodiscard]] bool bridge_exact_open_fault(util::durable_immutable_record::OwnedFileOpenFaultPoint,
                                           void* opaque) noexcept {
    auto& bridge = *static_cast<ExactOpenHookBridge*>(opaque);
    return bridge.hooks.stop_after != nullptr &&
           bridge.hooks.stop_after(bridge.point, bridge.hooks.context);
}

void require_adoption_authority(const OOCCleanupPaths& paths,
                                const AdoptionParentDirectoryHandle& parent,
                                const PrivateDirectoryHandle& directory, const BaseLock& lock) {
    parent.require_stable();
    parent.require_lock_binding(paths.lock_path.filename(), lock);
    parent.require_private_directory_binding(paths.private_directory.filename(), directory);
    directory.require_private_policy();
}

template <typename Operation>
decltype(auto) with_stable_adoption_authority(const OOCCleanupPaths& paths,
                                              AdoptionParentDirectoryHandle& parent,
                                              PrivateDirectoryHandle& directory,
                                              const BaseLock& lock, Operation&& operation) {
    return ooc_cleanup_detail::invoke_with_stable_base_lock(lock, [&]() -> decltype(auto) {
        require_adoption_authority(paths, parent, directory, lock);
        if constexpr (std::is_void_v<std::invoke_result_t<Operation>>) {
            std::forward<Operation>(operation)();
            require_adoption_authority(paths, parent, directory, lock);
            return;
        } else {
            decltype(auto) result = std::forward<Operation>(operation)();
            require_adoption_authority(paths, parent, directory, lock);
            return result;
        }
    });
}

void observe_adoption_boundary(const OOCCleanupPaths& paths, AdoptionParentDirectoryHandle& parent,
                               PrivateDirectoryHandle& directory, const BaseLock& lock,
                               const OOCPrivateHandoffAdoptionTestHooks& hooks,
                               OOCPrivateHandoffAdoptionFaultPoint point) {
    with_stable_adoption_authority(paths, parent, directory, lock, [&] {
        if (hooks.stop_after != nullptr && hooks.stop_after(point, hooks.context)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::Interrupted, OOCCleanupStage::None,
                                     std::make_error_code(std::errc::operation_canceled));
        }
    });
}

[[nodiscard]] util::durable_immutable_record::OpenedOwnedFile
take_exact_open(util::durable_immutable_record::OwnedFileOpenResult result) {
    using util::durable_immutable_record::OwnedFileOpenState;
    switch (result.state()) {
    case OwnedFileOpenState::missing:
    case OwnedFileOpenState::rejected:
        ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                 OOCCleanupStage::None, result.native_error());
    case OwnedFileOpenState::interrupted:
        ooc_cleanup_detail::fail(OOCCleanupStatus::Interrupted, OOCCleanupStage::None,
                                 result.native_error());
    case OwnedFileOpenState::unsupported:
        ooc_cleanup_detail::fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
                                 result.native_error());
    case OwnedFileOpenState::failed:
        ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                 result.native_error());
    case OwnedFileOpenState::exact:
        break;
    }
    auto opened = std::move(result).take_opened();
    if (!opened || !opened->file.valid()) {
        ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                 ooc_cleanup_detail::protocol_error());
    }
    return std::move(*opened);
}

[[nodiscard]] util::durable_immutable_record::OpenedOwnedFile
open_private_leaf_exact(const OOCCleanupPaths& paths, AdoptionParentDirectoryHandle& parent,
                        PrivateDirectoryHandle& directory, const BaseLock& lock,
                        const std::filesystem::path& leaf, const RecordSnapshot& expected,
                        const OOCPrivateHandoffAdoptionTestHooks& hooks,
                        OOCPrivateHandoffAdoptionFaultPoint initial_validation_point) {
    ExactOpenHookBridge bridge{
        .hooks = hooks,
        .point = initial_validation_point,
    };
    return with_stable_adoption_authority(paths, parent, directory, lock, [&] {
        auto opened = util::durable_immutable_record::open_owned_exact_at(
            directory.native_handle(), leaf, expected,
            util::durable_immutable_record::OwnedFileOpenTestHooks{
                .stop_after = bridge_exact_open_fault,
                .context = &bridge,
            });
        return take_exact_open(std::move(opened));
    });
}

struct RelativeRecordLeaf final {
    std::vector<std::byte> bytes;
    RecordSnapshot snapshot;

    friend bool operator==(const RelativeRecordLeaf&, const RelativeRecordLeaf&) = default;
};

struct RelativeLeaseMarker final {
    ooc_cleanup_detail::PrivateLeaseRecord record;
    RelativeRecordLeaf leaf;

    friend bool operator==(const RelativeLeaseMarker&, const RelativeLeaseMarker&) = default;
};

[[nodiscard]] std::optional<RelativeLeaseMarker>
load_relative_lease_marker(NativeHandle parent_handle, const std::filesystem::path& leaf) {
    using namespace util::durable_immutable_record;
    const auto read =
        read_bounded_at(parent_handle, leaf, ooc_cleanup_detail::PRIVATE_LEASE_MARKER_BYTES,
                        ooc_cleanup_detail::PRIVATE_LEASE_MARKER_BYTES);
    switch (read.state()) {
    case BoundedReadState::missing:
        return std::nullopt;
    case BoundedReadState::exact:
        break;
    case BoundedReadState::rejected:
        ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                 OOCCleanupStage::None, read.native_error());
    case BoundedReadState::interrupted:
        ooc_cleanup_detail::fail(OOCCleanupStatus::Interrupted, OOCCleanupStage::None,
                                 read.native_error());
    case BoundedReadState::unsupported:
        ooc_cleanup_detail::fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
                                 read.native_error());
    case BoundedReadState::failed:
        ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                 read.native_error());
    }
    if (!read.bytes() || !read.snapshot()) {
        ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                 ooc_cleanup_detail::protocol_error());
    }
    return RelativeLeaseMarker{
        .record = ooc_cleanup_detail::parse_private_lease_marker(*read.bytes()),
        .leaf =
            {
                .bytes = *read.bytes(),
                .snapshot = *read.snapshot(),
            },
    };
}

void require_relative_leaf_absent(NativeHandle parent_handle, const std::filesystem::path& leaf) {
    using namespace util::durable_immutable_record;
    const auto read = read_bounded_at(parent_handle, leaf, 0, MAX_BOUNDED_READ_BYTES);
    switch (read.state()) {
    case BoundedReadState::missing:
        return;
    case BoundedReadState::exact:
        ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                 ooc_cleanup_detail::protocol_error());
    case BoundedReadState::rejected:
        ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                 OOCCleanupStage::None, read.native_error());
    case BoundedReadState::interrupted:
        ooc_cleanup_detail::fail(OOCCleanupStatus::Interrupted, OOCCleanupStage::None,
                                 read.native_error());
    case BoundedReadState::unsupported:
        ooc_cleanup_detail::fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None,
                                 read.native_error());
    case BoundedReadState::failed:
        ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                 read.native_error());
    }
}

enum class AdoptionEntry : std::size_t {
    Owner,
    Index,
    Data,
    Canonical,
    Pending,
    Intent,
    IntentPending,
    Staged,
    StagedPending,
    QuarantineIndex,
    QuarantineData,
    Count,
};

struct AdoptionDirectoryEntries final {
    std::array<bool, static_cast<std::size_t>(AdoptionEntry::Count)> present{};

    friend bool operator==(const AdoptionDirectoryEntries&,
                           const AdoptionDirectoryEntries&) = default;
};

[[nodiscard]] AdoptionDirectoryEntries
scan_adoption_directory(const OOCCleanupPaths& paths, const PrivateDirectoryHandle& directory) {
    const int held_descriptor = static_cast<int>(directory.native_handle());
    int scan_descriptor = -1;
    do {
        scan_descriptor =
            ::openat(held_descriptor, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (scan_descriptor < 0 && errno == EINTR);
    if (scan_descriptor < 0) {
        ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                 ooc_cleanup_detail::posix_error(errno));
    }

    struct stat scan_metadata{};
    if (::fstat(scan_descriptor, &scan_metadata) != 0 ||
        ooc_cleanup_detail::stable_identity(ooc_cleanup_detail::posix_identity(scan_metadata)) !=
            directory.identity()) {
        const int saved_errno = errno == 0 ? EACCES : errno;
        (void)::close(scan_descriptor);
        ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                 ooc_cleanup_detail::posix_error(saved_errno));
    }

    DIR* raw_stream = ::fdopendir(scan_descriptor);
    if (raw_stream == nullptr) {
        const int saved_errno = errno;
        (void)::close(scan_descriptor);
        ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                 ooc_cleanup_detail::posix_error(saved_errno));
    }
    std::unique_ptr<DIR, decltype(&::closedir)> stream(raw_stream, &::closedir);

    const std::array<std::filesystem::path, static_cast<std::size_t>(AdoptionEntry::Count)> allowed{
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

    AdoptionDirectoryEntries entries;
    errno = 0;
    while (const auto* entry = ::readdir(stream.get())) {
        const std::filesystem::path leaf(entry->d_name);
        if (ooc_cleanup_detail::path_leaf_equals_ascii(leaf, ".") ||
            ooc_cleanup_detail::path_leaf_equals_ascii(leaf, "..")) {
            errno = 0;
            continue;
        }

        std::size_t slot = allowed.size();
        for (std::size_t index = 0; index < allowed.size(); ++index) {
            if (ooc_cleanup_detail::path_leaf_equals(leaf, allowed[index])) {
                slot = index;
                break;
            }
        }
        if (slot == allowed.size() || entries.present[slot]) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                     OOCCleanupStage::None, ooc_cleanup_detail::protocol_error());
        }
        if (slot >= static_cast<std::size_t>(AdoptionEntry::Intent)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        entries.present[slot] = true;
        errno = 0;
    }
    if (errno != 0) {
        ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                 ooc_cleanup_detail::posix_error(errno));
    }

    struct stat final_metadata{};
    if (::fstat(::dirfd(stream.get()), &final_metadata) != 0 ||
        ooc_cleanup_detail::stable_identity(ooc_cleanup_detail::posix_identity(final_metadata)) !=
            directory.identity()) {
        ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                 errno == 0 ? ooc_cleanup_detail::protocol_error()
                                            : ooc_cleanup_detail::posix_error(errno));
    }
    DIR* close_stream = stream.release();
    if (::closedir(close_stream) != 0) {
        ooc_cleanup_detail::fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
                                 ooc_cleanup_detail::posix_error(errno));
    }
    return entries;
}

struct AdoptionControlWitness final {
    AdoptionDirectoryEntries entries;
    RelativeLeaseMarker owner;
    RelativeLeaseMarker owned;
    std::optional<RelativeLeaseMarker> reserved;

    friend bool operator==(const AdoptionControlWitness&, const AdoptionControlWitness&) = default;
};

[[nodiscard]] bool
marker_snapshot_matches(const RecordSnapshot& snapshot,
                        const util::durable_immutable_record::NativeIdentity& expected) noexcept {
    return snapshot.identity == expected;
}

[[nodiscard]] AdoptionControlWitness
load_adoption_control_witness(const OOCCleanupPaths& paths, AdoptionParentDirectoryHandle& parent,
                              PrivateDirectoryHandle& directory, const BaseLock& lock,
                              const OOCPrivateHandoffRecordV1& record, bool canonical) {
    return with_stable_adoption_authority(paths, parent, directory, lock, [&] {
        const auto entries = scan_adoption_directory(paths, directory);
        const auto slot = [&](AdoptionEntry entry) {
            return entries.present[static_cast<std::size_t>(entry)];
        };
        if (!slot(AdoptionEntry::Owner) || !slot(AdoptionEntry::Index) ||
            !slot(AdoptionEntry::Data) || slot(AdoptionEntry::Canonical) != canonical ||
            (!canonical && !slot(AdoptionEntry::Pending))) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                     OOCCleanupStage::None, ooc_cleanup_detail::protocol_error());
        }

        auto owner = load_relative_lease_marker(
            directory.native_handle(),
            ooc_cleanup_detail::private_lease_owner_path(paths.private_directory).filename());
        auto owned =
            load_relative_lease_marker(parent.native_handle(), paths.lease_owned_path.filename());
        auto reserved = load_relative_lease_marker(parent.native_handle(),
                                                   paths.lease_reserved_path.filename());
        require_relative_leaf_absent(parent.native_handle(),
                                     paths.lease_reserved_pending_path.filename());
        require_relative_leaf_absent(parent.native_handle(),
                                     paths.lease_owned_pending_path.filename());
        if (!owner || !owned) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                     OOCCleanupStage::None, ooc_cleanup_detail::protocol_error());
        }

        if (record.lease_id != owned->record.lease_id ||
            record.lock_identity != ooc_cleanup_detail::handoff_native_identity(lock.identity()) ||
            record.directory_identity !=
                ooc_cleanup_detail::handoff_native_identity(directory.identity()) ||
            record.owner_marker_identity != owner->leaf.snapshot.identity ||
            record.owned_marker_identity != owned->leaf.snapshot.identity ||
            owned->record.directory_identity != directory.identity() ||
            owned->record.phase != ooc_cleanup_detail::PrivateLeasePhase::Owned ||
            owner->record != ooc_cleanup_detail::owner_record_for(owned->record) ||
            !marker_snapshot_matches(
                owner->leaf.snapshot,
                ooc_cleanup_detail::handoff_native_identity(owned->record.owner_identity))) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                     OOCCleanupStage::None, ooc_cleanup_detail::protocol_error());
        }

        try {
            ooc_cleanup_detail::validate_private_lease_record_context(
                owned->record, paths, parent.identity(), lock.identity());
            if (reserved) {
                ooc_cleanup_detail::validate_private_lease_record_context(
                    reserved->record, paths, parent.identity(), lock.identity());
                ooc_cleanup_detail::validate_private_lease_record_chain(reserved->record,
                                                                        owned->record);
                if (record.lease_id != reserved->record.lease_id) {
                    ooc_cleanup_detail::fail(OOCCleanupStatus::IntentConflict,
                                             OOCCleanupStage::None,
                                             ooc_cleanup_detail::protocol_error());
                }
            }
        } catch (const ooc_cleanup_detail::Failure& failure) {
            if (failure.status == OOCCleanupStatus::IoFailure ||
                failure.status == OOCCleanupStatus::PlatformUnsupported) {
                throw;
            }
            ooc_cleanup_detail::fail(
                OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                failure.error ? failure.error : ooc_cleanup_detail::protocol_error());
        }

        return AdoptionControlWitness{
            .entries = entries,
            .owner = std::move(*owner),
            .owned = std::move(*owned),
            .reserved = std::move(reserved),
        };
    });
}

struct AdoptionWitness final {
    OOCPrivateHandoffRecordV1 record;
    RelativeRecordLeaf canonical;
    std::optional<RelativeRecordLeaf> pending;
    AdoptionControlWitness control;

    friend bool operator==(const AdoptionWitness&, const AdoptionWitness&) = default;
};

struct AdoptionClassification final {
    OOCPrivateHandoffInspectResult inspection;
    std::optional<AdoptionWitness> witness;
};

[[nodiscard]] AdoptionClassification classify_adoption_locked(const OOCCleanupPaths& paths,
                                                              AdoptionParentDirectoryHandle& parent,
                                                              PrivateDirectoryHandle& directory,
                                                              const BaseLock& lock) {
    return with_stable_adoption_authority(paths, parent, directory, lock, [&] {
        const auto canonical = ooc_cleanup_detail::read_private_handoff_leaf(
            directory.native_handle(), paths.private_handoff_path.filename());
        const auto pending = ooc_cleanup_detail::read_private_handoff_leaf(
            directory.native_handle(), paths.private_handoff_pending_path.filename());
        if (canonical.state == ooc_cleanup_detail::PrivateHandoffLeafState::Rejected ||
            pending.state == ooc_cleanup_detail::PrivateHandoffLeafState::Rejected) {
            return AdoptionClassification{
                .inspection = ooc_cleanup_detail::handoff_failure(
                    OOCCleanupStatus::ForeignReplacementPreserved,
                    OOCPrivateHandoffState::TaintedPreserved),
                .witness = std::nullopt,
            };
        }
        if (canonical.state == ooc_cleanup_detail::PrivateHandoffLeafState::Missing &&
            pending.state == ooc_cleanup_detail::PrivateHandoffLeafState::Missing) {
            return AdoptionClassification{
                .inspection = ooc_cleanup_detail::handoff_none(),
                .witness = std::nullopt,
            };
        }

        if (canonical.state == ooc_cleanup_detail::PrivateHandoffLeafState::Missing) {
            auto control = load_adoption_control_witness(paths, parent, directory, lock,
                                                         *pending.record, false);
            (void)control;
            return AdoptionClassification{
                .inspection =
                    ooc_cleanup_detail::handoff_pending(*pending.record, *pending.snapshot),
                .witness = std::nullopt,
            };
        }

        std::optional<RelativeRecordLeaf> pending_witness;
        if (pending.state == ooc_cleanup_detail::PrivateHandoffLeafState::Exact) {
            if (!pending.record || !pending.snapshot || pending.bytes != canonical.bytes) {
                return AdoptionClassification{
                    .inspection = ooc_cleanup_detail::handoff_failure(
                        OOCCleanupStatus::ForeignReplacementPreserved,
                        OOCPrivateHandoffState::TaintedPreserved),
                    .witness = std::nullopt,
                };
            }
            pending_witness.emplace(RelativeRecordLeaf{
                .bytes = pending.bytes,
                .snapshot = *pending.snapshot,
            });
        }
        auto control =
            load_adoption_control_witness(paths, parent, directory, lock, *canonical.record, true);
        return AdoptionClassification{
            .inspection =
                ooc_cleanup_detail::handoff_present(*canonical.record, *canonical.snapshot),
            .witness =
                AdoptionWitness{
                    .record = *canonical.record,
                    .canonical =
                        {
                            .bytes = canonical.bytes,
                            .snapshot = *canonical.snapshot,
                        },
                    .pending = std::move(pending_witness),
                    .control = std::move(control),
                },
        };
    });
}

[[nodiscard]] RecordSnapshot
artifact_snapshot(const OOCPrivateHandoffArtifactBindingV1& binding) noexcept {
    return {
        .identity = binding.identity,
        .size = binding.extent,
    };
}

#endif

} // namespace

namespace {

template <typename AcquireLock>
OOCPrivateHandoffAdoptionResult
adopt_private_handoff_impl(const std::filesystem::path& base_path,
                           OOCPrivateHandoffAdoptionTestHooks hooks,
                           bool require_existing_lock_binding,
                           AcquireLock&& acquire_lock) noexcept {
    if (base_path.empty() || ooc_cleanup_detail::path_contains_nul(base_path)) {
        return adoption_failure(OOCCleanupStatus::InvalidRequest,
                                OOCPrivateHandoffState::TaintedPreserved,
                                ooc_cleanup_detail::invalid_argument_error());
    }

#if !defined(__APPLE__)
    (void)hooks;
    (void)require_existing_lock_binding;
    (void)acquire_lock;
    return adoption_failure(OOCCleanupStatus::PlatformUnsupported,
                            OOCPrivateHandoffState::TaintedPreserved,
                            std::make_error_code(std::errc::operation_not_supported));
#else
    const auto adopter_process_id = static_cast<std::uint64_t>(gnfs::util::process_id());
    std::optional<OOCPrivateHandoffAdoptionResult> adoption;
    bool assigned = false;
    const auto assign = [&](OOCPrivateHandoffAdoptionResult value) {
        adoption.emplace(std::move(value));
        assigned = true;
        return adoption->result;
    };
    const auto result = ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1::invoke([&] {
        const auto paths = ooc_cleanup_detail::freeze_paths(base_path);
        if (paths.private_directory.empty()) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }

        auto parent = std::make_shared<ooc_cleanup_detail::AdoptionParentDirectoryHandle>(
            paths.private_directory.parent_path());
        if (!parent->leaf_exists(paths.lock_path.filename())) {
            if (require_existing_lock_binding) {
                return assign(adoption_failure(OOCCleanupStatus::NamespaceConflict,
                                               OOCPrivateHandoffState::TaintedPreserved,
                                               ooc_cleanup_detail::protocol_error()));
            }
            const bool protocol_artifact =
                parent->leaf_exists(paths.private_directory.filename()) ||
                parent->leaf_exists(paths.lease_reserved_path.filename()) ||
                parent->leaf_exists(paths.lease_reserved_pending_path.filename()) ||
                parent->leaf_exists(paths.lease_owned_path.filename()) ||
                parent->leaf_exists(paths.lease_owned_pending_path.filename()) ||
                parent->leaf_exists(paths.private_handoff_rollback_path.filename());
            return assign(protocol_artifact
                              ? adoption_failure(OOCCleanupStatus::NamespaceConflict,
                                                 OOCPrivateHandoffState::TaintedPreserved)
                              : adoption_failure(OOCCleanupStatus::NoTransaction,
                                                 OOCPrivateHandoffState::None, {}));
        }

        auto lock = std::forward<AcquireLock>(acquire_lock)(paths, *parent);
        lock->require_stable();
        parent->require_lock_binding(paths.lock_path.filename(), *lock);
        if (parent->leaf_exists(paths.private_handoff_rollback_path.filename())) {
            return assign(adoption_failure(OOCCleanupStatus::NamespaceConflict,
                                           OOCPrivateHandoffState::TaintedPreserved,
                                           ooc_cleanup_detail::protocol_error()));
        }
        const auto directory_identity =
            parent->child_directory_identity(paths.private_directory.filename());
        if (!directory_identity) {
            const bool external_protocol_artifact =
                parent->leaf_exists(paths.lease_reserved_path.filename()) ||
                parent->leaf_exists(paths.lease_reserved_pending_path.filename()) ||
                parent->leaf_exists(paths.lease_owned_path.filename()) ||
                parent->leaf_exists(paths.lease_owned_pending_path.filename()) ||
                parent->leaf_exists(paths.private_handoff_rollback_path.filename());
            return assign(external_protocol_artifact
                              ? adoption_failure(OOCCleanupStatus::NamespaceConflict,
                                                 OOCPrivateHandoffState::TaintedPreserved)
                              : adoption_failure(OOCCleanupStatus::NoTransaction,
                                                 OOCPrivateHandoffState::None, {}));
        }

        auto directory =
            std::make_shared<ooc_cleanup_detail::PrivateDirectoryHandle>(paths.private_directory);
        if (directory->identity() != *directory_identity) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        const auto classified = classify_adoption_locked(paths, *parent, *directory, *lock);
        if (!classified.inspection.canonical() || !classified.witness) {
            return assign(adoption_from_inspection(classified.inspection));
        }

        observe_adoption_boundary(paths, *parent, *directory, *lock, hooks,
                                  OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified);

        const auto index_expected = artifact_snapshot(classified.witness->record.index);
        const auto data_expected = artifact_snapshot(classified.witness->record.data);
        if (index_expected.identity == data_expected.identity) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::SourcePairInvalid, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }

        auto index = open_private_leaf_exact(
            paths, *parent, *directory, *lock, paths.index_path.filename(), index_expected, hooks,
            OOCPrivateHandoffAdoptionFaultPoint::IndexInitialValidationComplete);
        observe_adoption_boundary(paths, *parent, *directory, *lock, hooks,
                                  OOCPrivateHandoffAdoptionFaultPoint::IndexOpened);
        auto data = open_private_leaf_exact(
            paths, *parent, *directory, *lock, paths.data_path.filename(), data_expected, hooks,
            OOCPrivateHandoffAdoptionFaultPoint::DataInitialValidationComplete);
        observe_adoption_boundary(paths, *parent, *directory, *lock, hooks,
                                  OOCPrivateHandoffAdoptionFaultPoint::DataOpened);

        observe_adoption_boundary(paths, *parent, *directory, *lock, hooks,
                                  OOCPrivateHandoffAdoptionFaultPoint::BeforeFinalRevalidation);
        const auto revalidate_before_receipt = [&] {
            if (static_cast<std::uint64_t>(gnfs::util::process_id()) != adopter_process_id) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            const auto current = classify_adoption_locked(paths, *parent, *directory, *lock);
            if (!current.inspection.canonical() || !current.witness) {
                const auto& failed = current.inspection.result;
                const auto status = failed.status == OOCCleanupStatus::NoTransaction ||
                                            failed.status == OOCCleanupStatus::RecoveryRequired
                                        ? OOCCleanupStatus::ForeignReplacementPreserved
                                        : failed.status;
                ooc_cleanup_detail::fail(status, OOCCleanupStage::None,
                                         failed.native_error
                                             ? failed.native_error
                                             : ooc_cleanup_detail::protocol_error());
            }
            if (*current.witness != *classified.witness) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                         OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }

            auto index_confirmation = open_private_leaf_exact(
                paths, *parent, *directory, *lock, paths.index_path.filename(), index_expected, {},
                OOCPrivateHandoffAdoptionFaultPoint::IndexInitialValidationComplete);
            auto data_confirmation = open_private_leaf_exact(
                paths, *parent, *directory, *lock, paths.data_path.filename(), data_expected, {},
                OOCPrivateHandoffAdoptionFaultPoint::DataInitialValidationComplete);
            if (index_confirmation.snapshot != index_expected ||
                data_confirmation.snapshot != data_expected) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                         OOCCleanupStage::None,
                                         ooc_cleanup_detail::protocol_error());
            }
        };
        revalidate_before_receipt();
        observe_adoption_boundary(
            paths, *parent, *directory, *lock, hooks,
            OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation);
        revalidate_before_receipt();
        const auto pending_handoff_snapshot =
            classified.witness->pending
                ? std::optional<RecordSnapshot>(classified.witness->pending->snapshot)
                : std::nullopt;
        auto receipt =
            ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1::make_receipt(
            paths.base_path, paths.private_directory, paths.lock_path, classified.witness->record,
            classified.witness->canonical.snapshot, pending_handoff_snapshot, std::move(index),
            std::move(data), std::move(lock), std::move(parent), std::move(directory),
            adopter_process_id);
        return assign(OOCPrivateHandoffAdoptionResult{
            .result =
                {
                    .status = OOCCleanupStatus::HandoffPresent,
                    .stage = OOCCleanupStage::None,
                    .native_error = {},
                },
            .state = OOCPrivateHandoffState::Canonical,
            .adoption = std::optional<OOCPrivateHandoffAdoptionReceipt>(std::move(receipt)),
        });
    });
    if (!assigned) {
        return adoption_failure(result.status, OOCPrivateHandoffState::TaintedPreserved,
                                result.native_error);
    }
    return std::move(*adoption);
#endif
}

} // namespace

OOCPrivateHandoffAdoptionResult
OOCCleanupTransaction::adopt_private_handoff(const std::filesystem::path& base_path,
                                             OOCPrivateHandoffAdoptionTestHooks hooks) noexcept {
    return adopt_private_handoff_impl(
        base_path, hooks, false,
        [](const OOCCleanupPaths& paths,
           ooc_cleanup_detail::AdoptionParentDirectoryHandle&) {
            return std::make_shared<ooc_cleanup_detail::BaseLock>(paths.lock_path, false);
        });
}

OOCPrivateHandoffAdoptionResult
ooc_cleanup_detail::adopt_private_handoff_with_borrowed_base_lock_v1(
    const std::filesystem::path& base_path,
    OOCPrivateHandoffBorrowedBaseLockV1&& borrowed,
    OOCPrivateHandoffAdoptionTestHooks hooks) noexcept {
    return adopt_private_handoff_impl(
        base_path, hooks, true,
        [&](const OOCCleanupPaths& paths, AdoptionParentDirectoryHandle& parent) {
            return borrowed.consume(paths, parent);
        });
}

} // namespace gnfs::relation
