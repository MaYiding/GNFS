#include "shadow_proof_rss_campaign_journal_store_internal.hpp"

#include <gnfs/util/durable_immutable_file.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#endif
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {
namespace {

using StoreDiagnostic = SIQSShadowProofRssCampaignJournalStoreDiagnostic;
using StoreError = SIQSShadowProofRssCampaignJournalStoreError;
using StoreObject = SIQSShadowProofRssCampaignJournalStoreObject;
using LayoutDiagnostic = SIQSShadowProofRssCampaignJournalLayoutDiagnostic;
using LayoutEntry = SIQSShadowProofRssCampaignJournalLayoutEntry;
using LayoutEntryKind = SIQSShadowProofRssCampaignJournalLayoutEntryKind;
using LayoutError = SIQSShadowProofRssCampaignJournalLayoutError;
namespace durable = gnfs::util::durable_immutable_file;

inline constexpr char SESSION_LOCK_LEAF[] = ".session.lock";
inline constexpr char HEADER_LEAF[] = "campaign-header.rjhd";

static_assert(std::string_view(SESSION_LOCK_LEAF) ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF);
static_assert(std::string_view(HEADER_LEAF) == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF);

[[nodiscard]] std::error_code native_error(int value) noexcept {
    return {value, std::generic_category()};
}

[[nodiscard]] bool descriptor_has_trusted_access(int fd, const struct stat& metadata,
                                                 uint64_t expected_owner, bool allow_root_owner,
                                                 std::error_code& error) noexcept {
    const uint64_t owner = static_cast<uint64_t>(metadata.st_uid);
    if ((owner != expected_owner && !(allow_root_owner && owner == 0)) ||
        (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return false;
    }

#if defined(__APPLE__)
    errno = 0;
    acl_t acl = ::acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (acl == nullptr) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return true;
        }
        error = native_error(saved_errno);
        return false;
    }
    // Any extended ACL is outside the closed deployment trust model. Mode
    // bits alone cannot show DELETE_CHILD or named-principal grants.
    (void)::acl_free(acl);
    return false;
#else
    (void)fd;
    (void)error;
    return true;
#endif
}

[[nodiscard]] StoreDiagnostic make_diagnostic(
    StoreError error, StoreObject object = StoreObject::none, std::error_code error_code = {},
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE) noexcept {
    StoreDiagnostic diagnostic;
    diagnostic.error = error;
    diagnostic.object = object;
    diagnostic.native_error = error_code;
    diagnostic.record_sequence = record_sequence;
    return diagnostic;
}

class UniqueFd final {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return fd_ >= 0;
    }

    void reset(int replacement = -1) noexcept {
        if (fd_ >= 0) {
            // POSIX leaves the descriptor state unspecified after EINTR from
            // close(). Retrying could close an unrelated reused descriptor.
            (void)::close(fd_);
        }
        fd_ = replacement;
    }

private:
    int fd_ = -1;
};

class UniqueDirectory final {
public:
    UniqueDirectory() noexcept = default;
    explicit UniqueDirectory(DIR* directory) noexcept : directory_(directory) {}

    ~UniqueDirectory() {
        if (directory_ != nullptr) {
            (void)::closedir(directory_);
        }
    }

    UniqueDirectory(const UniqueDirectory&) = delete;
    UniqueDirectory& operator=(const UniqueDirectory&) = delete;
    UniqueDirectory(UniqueDirectory&&) = delete;
    UniqueDirectory& operator=(UniqueDirectory&&) = delete;

    [[nodiscard]] DIR* get() const noexcept {
        return directory_;
    }

private:
    DIR* directory_ = nullptr;
};

struct FileFingerprint final {
    uint64_t device = 0;
    uint64_t inode = 0;
    uint64_t owner = 0;
    uint64_t group = 0;
    uint64_t mode = 0;
    uint64_t link_count = 0;
    int64_t size = 0;
    int64_t modified_seconds = 0;
    int64_t modified_nanoseconds = 0;
    int64_t changed_seconds = 0;
    int64_t changed_nanoseconds = 0;

    [[nodiscard]] friend bool operator==(const FileFingerprint&,
                                         const FileFingerprint&) noexcept = default;
};

struct DirectoryAuthorityFingerprint final {
    uint64_t device = 0;
    uint64_t inode = 0;
    uint64_t owner = 0;
    uint64_t group = 0;
    uint64_t mode = 0;
    uint64_t link_count = 0;

    [[nodiscard]] friend bool operator==(const DirectoryAuthorityFingerprint&,
                                         const DirectoryAuthorityFingerprint&) noexcept = default;
};

[[nodiscard]] FileFingerprint fingerprint(const struct stat& metadata) noexcept {
    FileFingerprint result;
    result.device = static_cast<uint64_t>(metadata.st_dev);
    result.inode = static_cast<uint64_t>(metadata.st_ino);
    result.owner = static_cast<uint64_t>(metadata.st_uid);
    result.group = static_cast<uint64_t>(metadata.st_gid);
    result.mode = static_cast<uint64_t>(metadata.st_mode);
    result.link_count = static_cast<uint64_t>(metadata.st_nlink);
    result.size = static_cast<int64_t>(metadata.st_size);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    result.modified_seconds = static_cast<int64_t>(metadata.st_mtimespec.tv_sec);
    result.modified_nanoseconds = static_cast<int64_t>(metadata.st_mtimespec.tv_nsec);
    result.changed_seconds = static_cast<int64_t>(metadata.st_ctimespec.tv_sec);
    result.changed_nanoseconds = static_cast<int64_t>(metadata.st_ctimespec.tv_nsec);
#else
    result.modified_seconds = static_cast<int64_t>(metadata.st_mtim.tv_sec);
    result.modified_nanoseconds = static_cast<int64_t>(metadata.st_mtim.tv_nsec);
    result.changed_seconds = static_cast<int64_t>(metadata.st_ctim.tv_sec);
    result.changed_nanoseconds = static_cast<int64_t>(metadata.st_ctim.tv_nsec);
#endif
    return result;
}

[[nodiscard]] DirectoryAuthorityFingerprint
directory_authority_fingerprint(const struct stat& metadata) noexcept {
    return {
        .device = static_cast<uint64_t>(metadata.st_dev),
        .inode = static_cast<uint64_t>(metadata.st_ino),
        .owner = static_cast<uint64_t>(metadata.st_uid),
        .group = static_cast<uint64_t>(metadata.st_gid),
        .mode = static_cast<uint64_t>(metadata.st_mode),
        .link_count = static_cast<uint64_t>(metadata.st_nlink),
    };
}

[[nodiscard]] bool
same_directory_authority_except_link_count(const DirectoryAuthorityFingerprint& lhs,
                                           const DirectoryAuthorityFingerprint& rhs) noexcept {
    return lhs.device == rhs.device && lhs.inode == rhs.inode && lhs.owner == rhs.owner &&
           lhs.group == rhs.group && lhs.mode == rhs.mode;
}

[[nodiscard]] bool same_identity_security_links_and_size(const struct stat& lhs,
                                                         const struct stat& rhs) noexcept {
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino && lhs.st_mode == rhs.st_mode &&
           lhs.st_uid == rhs.st_uid && lhs.st_gid == rhs.st_gid && lhs.st_nlink == rhs.st_nlink &&
           lhs.st_size == rhs.st_size;
}

[[nodiscard]] LayoutEntryKind layout_kind(const struct stat& metadata) noexcept {
    if (S_ISREG(metadata.st_mode)) {
        return LayoutEntryKind::regular_file;
    }
    if (S_ISDIR(metadata.st_mode)) {
        return LayoutEntryKind::directory;
    }
    if (S_ISLNK(metadata.st_mode)) {
        return LayoutEntryKind::link_or_reparse_point;
    }
    return LayoutEntryKind::other;
}

[[nodiscard]] uint64_t observed_size(const struct stat& metadata) noexcept {
    if (metadata.st_size < 0) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(metadata.st_size);
}

[[nodiscard]] int open_retrying_eintr(const char* path, int flags) noexcept {
    int fd = -1;
    do {
        fd = ::open(path, flags);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

[[nodiscard]] int openat_retrying_eintr(int directory_fd, const char* leaf, int flags) noexcept {
    int fd = -1;
    do {
        fd = ::openat(directory_fd, leaf, flags);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

[[nodiscard]] int openat_retrying_eintr(int directory_fd, const char* leaf, int flags,
                                        mode_t mode) noexcept {
    int fd = -1;
    do {
        fd = ::openat(directory_fd, leaf, flags, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

struct FdOpenResult final {
    UniqueFd fd;
    StoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(fd) && diagnostic.error == StoreError::none;
    }
};

[[nodiscard]] bool deployment_matches_policy(const SIQSShadowProofRssGatePolicy& policy,
                                             const DeploymentEntry& deployment) noexcept {
    return deployment.trusted_base_id == policy.journal_store.trusted_base_id &&
           deployment.store_id == policy.journal_store.store_id &&
           deployment.relative_locator == policy.journal_store.relative_locator &&
           !deployment.relative_locator.empty();
}

[[nodiscard]] bool parse_absolute_base_components(std::string_view path,
                                                  std::vector<std::string_view>& components) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string_view::npos) {
        return false;
    }
    if (path == "/") {
        return true;
    }
    if (path.back() == '/') {
        return false;
    }

    std::size_t begin = 1;
    while (begin < path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::size_t component_end = end == std::string_view::npos ? path.size() : end;
        if (component_end == begin) {
            return false;
        }
        const std::string_view component = path.substr(begin, component_end - begin);
        if (component == "." || component == "..") {
            return false;
        }
        components.push_back(component);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

[[nodiscard]] FdOpenResult open_trusted_base(const DeploymentEntry& deployment) {
    FdOpenResult result;
    const auto& native_path = deployment.trusted_base_path.native();
    const std::string_view path(native_path.data(), native_path.size());
    std::vector<std::string_view> components;
    if (!parse_absolute_base_components(path, components)) {
        result.diagnostic = make_diagnostic(StoreError::base_invalid, StoreObject::trusted_base);
        return result;
    }

    constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int raw_fd = open_retrying_eintr("/", directory_flags);
    if (raw_fd < 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::base_open_failed, StoreObject::trusted_base,
                                            native_error(saved_errno));
        return result;
    }
    UniqueFd current(raw_fd);

    const auto validate_current_directory =
        [&](bool allow_root_owner) -> std::optional<StoreDiagnostic> {
        struct stat metadata{};
        if (::fstat(current.get(), &metadata) != 0) {
            const int saved_errno = errno;
            return make_diagnostic(StoreError::base_invalid, StoreObject::trusted_base,
                                   native_error(saved_errno));
        }
        std::error_code trust_error;
        if (!S_ISDIR(metadata.st_mode) ||
            !descriptor_has_trusted_access(current.get(), metadata, deployment.expected_owner,
                                           allow_root_owner, trust_error)) {
            return make_diagnostic(StoreError::base_invalid, StoreObject::trusted_base,
                                   trust_error);
        }
        return std::nullopt;
    };

    if (const auto diagnostic = validate_current_directory(true); diagnostic.has_value()) {
        result.diagnostic = *diagnostic;
        return result;
    }

    for (const std::string_view component : components) {
        // Each syscall receives one owned, NUL-terminated component derived
        // exclusively from the private deployment registry path.
        const std::string owned_component(component);
        raw_fd = openat_retrying_eintr(current.get(), owned_component.c_str(), directory_flags);
        if (raw_fd < 0) {
            const int saved_errno = errno;
            result.diagnostic = make_diagnostic(
                StoreError::base_open_failed, StoreObject::trusted_base, native_error(saved_errno));
            return result;
        }
        current.reset(raw_fd);
        if (const auto diagnostic = validate_current_directory(true); diagnostic.has_value()) {
            result.diagnostic = *diagnostic;
            return result;
        }
    }

    if (const auto diagnostic = validate_current_directory(false); diagnostic.has_value()) {
        result.diagnostic = *diagnostic;
        return result;
    }

    result.fd = std::move(current);
    return result;
}

[[nodiscard]] FdOpenResult open_store_root(int base_fd, const SIQSShadowProofRssGatePolicy& policy,
                                           const DeploymentEntry& deployment) noexcept {
    FdOpenResult result;
    if (!deployment_matches_policy(policy, deployment)) {
        result.diagnostic = make_diagnostic(StoreError::registry_binding_mismatch,
                                            StoreObject::deployment_registry);
        return result;
    }

    constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    // The syscall locator is the deployment-owned string, never the policy's
    // borrowed string_view.
    const int raw_fd =
        openat_retrying_eintr(base_fd, deployment.relative_locator.c_str(), directory_flags);
    if (raw_fd < 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::root_open_failed, StoreObject::store_root,
                                            native_error(saved_errno));
        return result;
    }
    UniqueFd root(raw_fd);

    struct stat metadata{};
    if (::fstat(root.get(), &metadata) != 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::root_invalid, StoreObject::store_root,
                                            native_error(saved_errno));
        return result;
    }
    std::error_code trust_error;
    if (!S_ISDIR(metadata.st_mode) ||
        !descriptor_has_trusted_access(root.get(), metadata, deployment.expected_owner, false,
                                       trust_error)) {
        result.diagnostic =
            make_diagnostic(StoreError::root_invalid, StoreObject::store_root, trust_error);
        return result;
    }

    result.fd = std::move(root);
    return result;
}

struct LockOpenResult final {
    UniqueFd fd;
    struct stat metadata{};
    StoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(fd) && diagnostic.error == StoreError::none;
    }
};

[[nodiscard]] bool valid_lock_metadata(const struct stat& metadata,
                                       uint64_t expected_owner) noexcept {
    return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 && metadata.st_size == 0 &&
           static_cast<uint64_t>(metadata.st_uid) == expected_owner &&
           (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

[[nodiscard]] LockOpenResult open_and_lock_session(int root_fd, uint64_t expected_owner) noexcept {
    LockOpenResult result;
    constexpr int common_flags = O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    int raw_fd =
        openat_retrying_eintr(root_fd, SESSION_LOCK_LEAF, common_flags | O_CREAT | O_EXCL, 0600);
    if (raw_fd < 0 && errno == EEXIST) {
        raw_fd = openat_retrying_eintr(root_fd, SESSION_LOCK_LEAF, common_flags);
    }
    if (raw_fd < 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::lock_open_failed, StoreObject::session_lock,
                                            native_error(saved_errno));
        return result;
    }
    UniqueFd lock(raw_fd);

    if (::fstat(lock.get(), &result.metadata) != 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock,
                                            native_error(saved_errno));
        return result;
    }
    std::error_code trust_error;
    if (!valid_lock_metadata(result.metadata, expected_owner) ||
        !descriptor_has_trusted_access(lock.get(), result.metadata, expected_owner, false,
                                       trust_error)) {
        result.diagnostic =
            make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock, trust_error);
        return result;
    }

    int flock_result = -1;
    do {
        flock_result = ::flock(lock.get(), LOCK_EX | LOCK_NB);
    } while (flock_result != 0 && errno == EINTR);
    if (flock_result != 0) {
        const int saved_errno = errno;
        const StoreError error = saved_errno == EWOULDBLOCK || saved_errno == EAGAIN
                                     ? StoreError::lock_busy
                                     : StoreError::lock_failed;
        result.diagnostic =
            make_diagnostic(error, StoreObject::session_lock, native_error(saved_errno));
        return result;
    }

    struct stat path_metadata{};
    if (::fstatat(root_fd, SESSION_LOCK_LEAF, &path_metadata, AT_SYMLINK_NOFOLLOW) != 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock,
                                            native_error(saved_errno));
        return result;
    }
    if (!valid_lock_metadata(path_metadata, expected_owner) ||
        !same_identity_security_links_and_size(result.metadata, path_metadata)) {
        result.diagnostic = make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock);
        return result;
    }

    result.fd = std::move(lock);
    return result;
}

enum class LeafTargetKind : uint8_t {
    unknown,
    session_lock,
    header,
    record,
};

struct LeafTarget final {
    LeafTargetKind kind = LeafTargetKind::unknown;
    StoreObject object = StoreObject::directory;
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
    std::size_t expected_size = 0;
};

[[nodiscard]] LeafTarget classify_leaf(std::string_view leaf_name) noexcept {
    if (leaf_name == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF) {
        return {LeafTargetKind::session_lock, StoreObject::session_lock,
                SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE, 0};
    }
    if (leaf_name == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF) {
        return {LeafTargetKind::header, StoreObject::journal_header,
                SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE,
                SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE};
    }
    const auto sequence = parse_siqs_shadow_proof_rss_campaign_journal_record_leaf(leaf_name);
    if (sequence.has_value()) {
        return {LeafTargetKind::record, StoreObject::journal_record, *sequence,
                SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE};
    }
    return {};
}

struct CapturedEntry final {
    std::string leaf_name;
    LayoutEntryKind kind = LayoutEntryKind::unknown;
    uint64_t link_count = 0;
    uint64_t size = 0;
    std::vector<std::byte> bytes;
    std::optional<FileFingerprint> file_fingerprint;
};

struct DirectorySnapshot final {
    std::vector<CapturedEntry> entries;
};

struct SnapshotResult final {
    std::optional<DirectorySnapshot> snapshot;
    StoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return snapshot.has_value() && diagnostic.error == StoreError::none;
    }
};

enum class FrameReadStatus : uint8_t {
    complete,
    native_failure,
    unstable,
};

struct FrameReadResult final {
    FrameReadStatus status = FrameReadStatus::complete;
    std::error_code error;
};

[[nodiscard]] FrameReadResult read_exact_frame_once(int fd, std::span<std::byte> output) noexcept {
    std::size_t offset = 0;
    while (offset < output.size()) {
        const ssize_t count =
            ::pread(fd, output.data() + offset, output.size() - offset, static_cast<off_t>(offset));
        if (count < 0) {
            const int saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            return {FrameReadStatus::native_failure, native_error(saved_errno)};
        }
        if (count == 0) {
            return {FrameReadStatus::unstable, {}};
        }
        offset += static_cast<std::size_t>(count);
    }

    std::byte trailing_byte{};
    for (;;) {
        const ssize_t count = ::pread(fd, &trailing_byte, 1, static_cast<off_t>(output.size()));
        if (count < 0) {
            const int saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            return {FrameReadStatus::native_failure, native_error(saved_errno)};
        }
        if (count != 0) {
            return {FrameReadStatus::unstable, {}};
        }
        return {};
    }
}

struct CaptureResult final {
    std::optional<CapturedEntry> entry;
    StoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return entry.has_value() && diagnostic.error == StoreError::none;
    }
};

[[nodiscard]] CaptureResult capture_lock_entry(std::string leaf_name, int lock_fd) {
    CaptureResult result;
    struct stat metadata{};
    if (::fstat(lock_fd, &metadata) != 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::entry_metadata_failed,
                                            StoreObject::session_lock, native_error(saved_errno));
        return result;
    }

    CapturedEntry entry;
    entry.leaf_name = std::move(leaf_name);
    entry.kind = layout_kind(metadata);
    entry.link_count = static_cast<uint64_t>(metadata.st_nlink);
    entry.size = observed_size(metadata);
    entry.file_fingerprint = fingerprint(metadata);
    result.entry = std::move(entry);
    return result;
}

[[nodiscard]] CaptureResult capture_journal_entry(int root_fd, std::string leaf_name,
                                                  const LeafTarget& target,
                                                  uint64_t expected_owner) {
    CaptureResult result;
    struct stat path_metadata{};
    if (::fstatat(root_fd, leaf_name.c_str(), &path_metadata, AT_SYMLINK_NOFOLLOW) != 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::entry_metadata_failed, target.object,
                                            native_error(saved_errno), target.record_sequence);
        return result;
    }

    CapturedEntry entry;
    entry.leaf_name = std::move(leaf_name);
    entry.kind = layout_kind(path_metadata);
    entry.link_count = static_cast<uint64_t>(path_metadata.st_nlink);
    entry.size = observed_size(path_metadata);
    entry.file_fingerprint = fingerprint(path_metadata);

    // Unsafe or structurally invalid objects are projected to the pure layout
    // inspector without opening a directory, link, device, FIFO, or hardlink.
    if (!S_ISREG(path_metadata.st_mode) || path_metadata.st_nlink != 1 ||
        path_metadata.st_size < 0 ||
        static_cast<uint64_t>(path_metadata.st_size) != target.expected_size) {
        result.entry = std::move(entry);
        return result;
    }

    constexpr int file_flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    const int raw_fd = openat_retrying_eintr(root_fd, entry.leaf_name.c_str(), file_flags);
    if (raw_fd < 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::entry_open_failed, target.object,
                                            native_error(saved_errno), target.record_sequence);
        return result;
    }
    UniqueFd file(raw_fd);

    struct stat before_read{};
    if (::fstat(file.get(), &before_read) != 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::entry_metadata_failed, target.object,
                                            native_error(saved_errno), target.record_sequence);
        return result;
    }
    if (!same_identity_security_links_and_size(path_metadata, before_read)) {
        result.diagnostic = make_diagnostic(StoreError::entry_identity_mismatch, target.object, {},
                                            target.record_sequence);
        return result;
    }
    std::error_code trust_error;
    if (!descriptor_has_trusted_access(file.get(), before_read, expected_owner, false,
                                       trust_error)) {
        result.diagnostic = make_diagnostic(StoreError::entry_trust_invalid, target.object,
                                            trust_error, target.record_sequence);
        return result;
    }

    std::vector<std::byte> first_read(target.expected_size);
    std::vector<std::byte> second_read(target.expected_size);
    const FrameReadResult first = read_exact_frame_once(file.get(), first_read);
    if (first.status == FrameReadStatus::native_failure) {
        result.diagnostic = make_diagnostic(StoreError::entry_read_failed, target.object,
                                            first.error, target.record_sequence);
        return result;
    }
    if (first.status == FrameReadStatus::unstable) {
        result.diagnostic = make_diagnostic(StoreError::entry_changed_during_read, target.object,
                                            {}, target.record_sequence);
        return result;
    }
    const FrameReadResult second = read_exact_frame_once(file.get(), second_read);
    if (second.status == FrameReadStatus::native_failure) {
        result.diagnostic = make_diagnostic(StoreError::entry_read_failed, target.object,
                                            second.error, target.record_sequence);
        return result;
    }
    if (second.status == FrameReadStatus::unstable || first_read != second_read) {
        result.diagnostic = make_diagnostic(StoreError::entry_changed_during_read, target.object,
                                            {}, target.record_sequence);
        return result;
    }

    struct stat after_read{};
    if (::fstat(file.get(), &after_read) != 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::entry_metadata_failed, target.object,
                                            native_error(saved_errno), target.record_sequence);
        return result;
    }
    if (fingerprint(before_read) != fingerprint(after_read)) {
        result.diagnostic = make_diagnostic(StoreError::entry_changed_during_read, target.object,
                                            {}, target.record_sequence);
        return result;
    }
    trust_error.clear();
    if (!descriptor_has_trusted_access(file.get(), after_read, expected_owner, false,
                                       trust_error)) {
        result.diagnostic = make_diagnostic(StoreError::entry_trust_invalid, target.object,
                                            trust_error, target.record_sequence);
        return result;
    }

    struct stat current_path_metadata{};
    if (::fstatat(root_fd, entry.leaf_name.c_str(), &current_path_metadata, AT_SYMLINK_NOFOLLOW) !=
        0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::entry_metadata_failed, target.object,
                                            native_error(saved_errno), target.record_sequence);
        return result;
    }
    if (!same_identity_security_links_and_size(after_read, current_path_metadata)) {
        result.diagnostic = make_diagnostic(StoreError::entry_identity_mismatch, target.object, {},
                                            target.record_sequence);
        return result;
    }

    entry.file_fingerprint = fingerprint(after_read);
    entry.bytes = std::move(second_read);
    result.entry = std::move(entry);
    return result;
}

[[nodiscard]] StoreDiagnostic too_many_entries_diagnostic() noexcept {
    StoreDiagnostic diagnostic =
        make_diagnostic(StoreError::layout_invalid, StoreObject::directory);
    diagnostic.layout.layout_error = LayoutError::too_many_entries;
    return diagnostic;
}

[[nodiscard]] SnapshotResult capture_directory_snapshot(int root_fd, int lock_fd,
                                                        uint64_t expected_owner) {
    SnapshotResult result;
    constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    const int raw_directory_fd = openat_retrying_eintr(root_fd, ".", directory_flags);
    if (raw_directory_fd < 0) {
        const int saved_errno = errno;
        result.diagnostic = make_diagnostic(StoreError::directory_open_failed,
                                            StoreObject::directory, native_error(saved_errno));
        return result;
    }

    DIR* raw_directory = ::fdopendir(raw_directory_fd);
    if (raw_directory == nullptr) {
        const int saved_errno = errno;
        (void)::close(raw_directory_fd);
        result.diagnostic = make_diagnostic(StoreError::directory_open_failed,
                                            StoreObject::directory, native_error(saved_errno));
        return result;
    }
    UniqueDirectory directory(raw_directory);
    DirectorySnapshot snapshot;
    snapshot.entries.reserve(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_ENTRIES);

    for (;;) {
        errno = 0;
        dirent* directory_entry = ::readdir(directory.get());
        if (directory_entry == nullptr) {
            if (errno != 0) {
                const int saved_errno = errno;
                result.diagnostic =
                    make_diagnostic(StoreError::directory_read_failed, StoreObject::directory,
                                    native_error(saved_errno));
                return result;
            }
            break;
        }

        const std::string_view borrowed_name(directory_entry->d_name);
        if (borrowed_name == "." || borrowed_name == "..") {
            continue;
        }
        if (snapshot.entries.size() == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_ENTRIES) {
            result.diagnostic = too_many_entries_diagnostic();
            return result;
        }

        std::string leaf_name(borrowed_name);
        const LeafTarget target = classify_leaf(leaf_name);
        if (target.kind == LeafTargetKind::unknown) {
            // Unknown leaves are names only. Do not stat or open a
            // caller-controlled device, FIFO, link, or other special object.
            CapturedEntry entry;
            entry.leaf_name = std::move(leaf_name);
            snapshot.entries.push_back(std::move(entry));
            continue;
        }

        CaptureResult capture =
            target.kind == LeafTargetKind::session_lock
                ? capture_lock_entry(std::move(leaf_name), lock_fd)
                : capture_journal_entry(root_fd, std::move(leaf_name), target, expected_owner);
        if (!capture) {
            result.diagnostic = std::move(capture.diagnostic);
            return result;
        }
        snapshot.entries.push_back(std::move(*capture.entry));
    }

    std::sort(snapshot.entries.begin(), snapshot.entries.end(),
              [](const CapturedEntry& lhs, const CapturedEntry& rhs) {
                  return lhs.leaf_name < rhs.leaf_name;
              });
    result.snapshot = std::move(snapshot);
    return result;
}

struct SnapshotDifference final {
    bool different = false;
    StoreObject object = StoreObject::directory;
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
};

[[nodiscard]] SnapshotDifference compare_snapshots(const DirectorySnapshot& lhs,
                                                   const DirectorySnapshot& rhs) noexcept {
    if (lhs.entries.size() != rhs.entries.size()) {
        return {true, StoreObject::directory,
                SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE};
    }
    for (std::size_t index = 0; index < lhs.entries.size(); ++index) {
        const CapturedEntry& left = lhs.entries[index];
        const CapturedEntry& right = rhs.entries[index];
        if (left.leaf_name != right.leaf_name) {
            return {true, StoreObject::directory,
                    SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE};
        }
        if (left.file_fingerprint != right.file_fingerprint) {
            const LeafTarget target = classify_leaf(left.leaf_name);
            return {true, target.object, target.record_sequence};
        }
        const LeafTarget target = classify_leaf(left.leaf_name);
        if ((target.kind == LeafTargetKind::header || target.kind == LeafTargetKind::record) &&
            left.bytes != right.bytes) {
            return {true, target.object, target.record_sequence};
        }
    }
    return {};
}

[[nodiscard]] StoreDiagnostic
verify_root_identity(int base_fd, int root_fd, const DeploymentEntry& deployment,
                     const DirectoryAuthorityFingerprint& initial_root_fingerprint,
                     DirectoryAuthorityFingerprint* rebased_root_fingerprint = nullptr) noexcept {
    struct stat held_before{};
    if (::fstat(root_fd, &held_before) != 0) {
        const int saved_errno = errno;
        return make_diagnostic(StoreError::root_invalid, StoreObject::store_root,
                               native_error(saved_errno));
    }
    std::error_code trust_error;
    if (!S_ISDIR(held_before.st_mode) ||
        !descriptor_has_trusted_access(root_fd, held_before, deployment.expected_owner, false,
                                       trust_error)) {
        return make_diagnostic(StoreError::root_invalid, StoreObject::store_root, trust_error);
    }

    struct stat path_metadata{};
    if (::fstatat(base_fd, deployment.relative_locator.c_str(), &path_metadata,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        const int saved_errno = errno;
        return make_diagnostic(StoreError::root_invalid, StoreObject::store_root,
                               native_error(saved_errno));
    }
    if (!S_ISDIR(path_metadata.st_mode) ||
        static_cast<uint64_t>(path_metadata.st_uid) != deployment.expected_owner ||
        (path_metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return make_diagnostic(StoreError::root_invalid, StoreObject::store_root);
    }

    struct stat held_after{};
    if (::fstat(root_fd, &held_after) != 0) {
        const int saved_errno = errno;
        return make_diagnostic(StoreError::root_invalid, StoreObject::store_root,
                               native_error(saved_errno));
    }
    trust_error.clear();
    if (!S_ISDIR(held_after.st_mode) ||
        !descriptor_has_trusted_access(root_fd, held_after, deployment.expected_owner, false,
                                       trust_error)) {
        return make_diagnostic(StoreError::root_invalid, StoreObject::store_root, trust_error);
    }
    const auto held_before_fingerprint = directory_authority_fingerprint(held_before);
    const auto path_fingerprint = directory_authority_fingerprint(path_metadata);
    const auto held_after_fingerprint = directory_authority_fingerprint(held_after);
    if (held_before_fingerprint != path_fingerprint ||
        held_before_fingerprint != held_after_fingerprint ||
        (rebased_root_fingerprint == nullptr
             ? held_before_fingerprint != initial_root_fingerprint
             : !same_directory_authority_except_link_count(held_before_fingerprint,
                                                           initial_root_fingerprint))) {
        return make_diagnostic(StoreError::snapshot_changed, StoreObject::directory);
    }
    if (rebased_root_fingerprint != nullptr) {
        *rebased_root_fingerprint = held_before_fingerprint;
    }
    return {};
}

[[nodiscard]] StoreDiagnostic
verify_root_namespace_generation(int root_fd,
                                 const FileFingerprint& expected_fingerprint) noexcept {
    struct stat metadata{};
    if (::fstat(root_fd, &metadata) != 0) {
        const int saved_errno = errno;
        return make_diagnostic(StoreError::root_invalid, StoreObject::store_root,
                               native_error(saved_errno));
    }
    if (fingerprint(metadata) != expected_fingerprint) {
        return make_diagnostic(StoreError::snapshot_changed, StoreObject::directory);
    }
    return {};
}

[[nodiscard]] StoreDiagnostic verify_lock_identity(int root_fd, int lock_fd,
                                                   const FileFingerprint& initial_lock_fingerprint,
                                                   uint64_t expected_owner) noexcept {
    struct stat held_before{};
    if (::fstat(lock_fd, &held_before) != 0) {
        const int saved_errno = errno;
        return make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock,
                               native_error(saved_errno));
    }
    std::error_code trust_error;
    if (!valid_lock_metadata(held_before, expected_owner) ||
        !descriptor_has_trusted_access(lock_fd, held_before, expected_owner, false, trust_error)) {
        return make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock, trust_error);
    }

    struct stat path_metadata{};
    if (::fstatat(root_fd, SESSION_LOCK_LEAF, &path_metadata, AT_SYMLINK_NOFOLLOW) != 0) {
        const int saved_errno = errno;
        return make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock,
                               native_error(saved_errno));
    }
    if (!valid_lock_metadata(path_metadata, expected_owner)) {
        return make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock);
    }

    struct stat held_after{};
    if (::fstat(lock_fd, &held_after) != 0) {
        const int saved_errno = errno;
        return make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock,
                               native_error(saved_errno));
    }
    trust_error.clear();
    if (!valid_lock_metadata(held_after, expected_owner) ||
        !descriptor_has_trusted_access(lock_fd, held_after, expected_owner, false, trust_error)) {
        return make_diagnostic(StoreError::lock_invalid, StoreObject::session_lock, trust_error);
    }
    if (fingerprint(held_before) != initial_lock_fingerprint ||
        fingerprint(path_metadata) != initial_lock_fingerprint ||
        fingerprint(held_after) != initial_lock_fingerprint) {
        return make_diagnostic(StoreError::snapshot_changed, StoreObject::session_lock);
    }
    return {};
}

[[nodiscard]] std::vector<LayoutEntry> project_layout_entries(const DirectorySnapshot& snapshot) {
    std::vector<LayoutEntry> entries;
    entries.reserve(snapshot.entries.size());
    for (const CapturedEntry& captured : snapshot.entries) {
        entries.push_back({
            .leaf_name = captured.leaf_name,
            .kind = captured.kind,
            .link_count = captured.link_count,
            .observed_size = captured.size,
            .bytes = captured.bytes,
        });
    }
    return entries;
}

[[nodiscard]] StoreObject layout_failure_object(const LayoutDiagnostic& diagnostic,
                                                const DirectorySnapshot& snapshot) noexcept {
    if (diagnostic.record_sequence != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE) {
        return StoreObject::journal_record;
    }
    switch (diagnostic.layout_error) {
    case LayoutError::duplicate_session_lock:
    case LayoutError::session_lock_missing:
    case LayoutError::session_lock_not_empty:
        return StoreObject::session_lock;
    case LayoutError::duplicate_header:
    case LayoutError::header_size_invalid:
    case LayoutError::record_without_header:
    case LayoutError::header_codec_invalid:
        return StoreObject::journal_header;
    case LayoutError::entry_not_regular_file:
    case LayoutError::link_count_invalid: {
        const auto lock = std::find_if(
            snapshot.entries.begin(), snapshot.entries.end(), [](const CapturedEntry& entry) {
                return entry.leaf_name == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF;
            });
        if (lock != snapshot.entries.end()) {
            const bool lock_failed = diagnostic.layout_error == LayoutError::entry_not_regular_file
                                         ? lock->kind != LayoutEntryKind::regular_file
                                         : lock->link_count != 1;
            if (lock_failed) {
                return StoreObject::session_lock;
            }
        }
        return StoreObject::journal_header;
    }
    case LayoutError::none:
    case LayoutError::too_many_entries:
    case LayoutError::unknown_entry:
        return StoreObject::directory;
    case LayoutError::duplicate_record:
    case LayoutError::record_size_invalid:
    case LayoutError::record_gap:
    case LayoutError::record_codec_invalid:
    case LayoutError::record_sequence_mismatch:
        return StoreObject::journal_record;
    }
    return StoreObject::directory;
}

[[nodiscard]] StoreDiagnostic make_layout_diagnostic(const LayoutDiagnostic& layout,
                                                     const DirectorySnapshot& snapshot) noexcept {
    StoreDiagnostic diagnostic =
        make_diagnostic(StoreError::layout_invalid, layout_failure_object(layout, snapshot), {},
                        layout.record_sequence);
    diagnostic.layout = layout;
    return diagnostic;
}

[[nodiscard]] bool replay_status_is_leasable(SIQSShadowProofRssJournalStatus status) noexcept {
    return status == SIQSShadowProofRssJournalStatus::ready ||
           status == SIQSShadowProofRssJournalStatus::tainted ||
           status == SIQSShadowProofRssJournalStatus::complete;
}

[[nodiscard]] uint32_t first_invalid_record_sequence(
    const SIQSShadowProofRssGatePolicy& policy,
    const SIQSShadowProofRssCampaignRuntimeFacts& runtime_facts,
    const SIQSShadowProofRssCampaignJournalLayoutSnapshot& snapshot) noexcept {
    if (!snapshot.header.has_value()) {
        return SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
    }
    for (std::size_t count = 1; count <= snapshot.record_count; ++count) {
        const auto prefix = snapshot.record_span().first(count);
        const auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &runtime_facts, SIQSShadowProofRssJournalPresence::present, &*snapshot.header,
            prefix);
        if (replay.status == SIQSShadowProofRssJournalStatus::invalid) {
            return snapshot.records[count - 1].sequence_number;
        }
    }
    return SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
}

[[nodiscard]] StoreDiagnostic
make_replay_diagnostic(const SIQSShadowProofRssCampaignJournalResume& replay,
                       const SIQSShadowProofRssGatePolicy& policy,
                       const SIQSShadowProofRssCampaignRuntimeFacts& runtime_facts,
                       const SIQSShadowProofRssCampaignJournalLayoutSnapshot& snapshot) noexcept {
    StoreObject object = StoreObject::none;
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
    if (replay.reason == SIQSShadowProofRssJournalReason::header_invalid ||
        replay.reason == SIQSShadowProofRssJournalReason::present_journal_missing_header) {
        object = StoreObject::journal_header;
    } else if (replay.reason == SIQSShadowProofRssJournalReason::record_invalid ||
               replay.reason == SIQSShadowProofRssJournalReason::record_order_invalid ||
               replay.reason == SIQSShadowProofRssJournalReason::committed_sample_invalid) {
        object = StoreObject::journal_record;
        record_sequence = first_invalid_record_sequence(policy, runtime_facts, snapshot);
    }
    StoreDiagnostic diagnostic =
        make_diagnostic(StoreError::replay_rejected, object, {}, record_sequence);
    diagnostic.journal_reason = replay.reason;
    return diagnostic;
}

[[nodiscard]] StoreDiagnostic make_publication_diagnostic(
    const durable::PublishResult& publication, StoreObject object,
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE) noexcept {
    StoreDiagnostic diagnostic =
        make_diagnostic(publication.status() == durable::PublishStatus::already_exists
                            ? StoreError::publication_conflict
                            : StoreError::publication_failed,
                        object, publication.native_error(), record_sequence);
    diagnostic.publication_status = publication.status();
    diagnostic.publication_bytes_written = publication.bytes_written();
    return diagnostic;
}

[[nodiscard]] StoreDiagnostic
with_completed_publication(StoreDiagnostic diagnostic,
                           const durable::PublishResult& publication) noexcept {
    diagnostic.publication_status = publication.status();
    diagnostic.publication_bytes_written = publication.bytes_written();
    return diagnostic;
}

struct DurablePublicationTrace final {
    StoreObject object = StoreObject::none;
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE;
    uint64_t bytes_written = 0;
};

[[nodiscard]] StoreDiagnostic
with_durable_publication_trace(StoreDiagnostic diagnostic,
                               const std::optional<DurablePublicationTrace>& publication) noexcept {
    if (publication.has_value()) {
        diagnostic.last_durable_publication_object = publication->object;
        diagnostic.last_durable_publication_record_sequence = publication->record_sequence;
        diagnostic.last_durable_publication_bytes_written = publication->bytes_written;
    }
    return diagnostic;
}

[[nodiscard]] StoreDiagnostic make_encode_diagnostic(
    SIQSShadowProofRssCampaignJournalCodecError error, std::size_t error_offset, StoreObject object,
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE) noexcept {
    StoreDiagnostic diagnostic =
        make_diagnostic(StoreError::journal_encode_failed, object, {}, record_sequence);
    diagnostic.layout.codec_error = error;
    diagnostic.layout.codec_error_offset = error_offset;
    diagnostic.layout.record_sequence = record_sequence;
    return diagnostic;
}

struct VerifiedReplayResult final {
    std::optional<SIQSShadowProofRssCampaignJournalLayoutSnapshot> snapshot;
    std::optional<SIQSShadowProofRssCampaignJournalResume> replay;
    std::optional<FileFingerprint> root_namespace_fingerprint;
    StoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return snapshot.has_value() && replay.has_value() &&
               root_namespace_fingerprint.has_value() && diagnostic.error == StoreError::none;
    }
};

class PosixSessionCore final : public SessionCore {
public:
    PosixSessionCore(UniqueFd base_fd, UniqueFd root_fd, UniqueFd lock_fd,
                     const SIQSShadowProofRssGatePolicy& policy,
                     const SIQSShadowProofRssCampaignRuntimeFacts& runtime_facts,
                     const DeploymentEntry& deployment,
                     DirectoryAuthorityFingerprint initial_root_fingerprint,
                     FileFingerprint initial_root_namespace_fingerprint,
                     FileFingerprint initial_lock_fingerprint,
                     SIQSShadowProofRssCampaignJournalResume replay)
        : base_fd_(std::move(base_fd)), root_fd_(std::move(root_fd)), lock_fd_(std::move(lock_fd)),
          corpus_id_(policy.corpus_id), policy_candidate_revision_(policy.candidate_revision),
          approval_id_(policy.approval_id),
          runtime_candidate_revision_(runtime_facts.candidate_revision), deployment_(deployment),
          initial_root_fingerprint_(initial_root_fingerprint),
          root_namespace_fingerprint_(initial_root_namespace_fingerprint),
          initial_lock_fingerprint_(initial_lock_fingerprint), policy_(policy),
          runtime_facts_(runtime_facts), replay_(std::move(replay)) {
        policy_.corpus_id = corpus_id_;
        policy_.candidate_revision = policy_candidate_revision_;
        policy_.approval_id = approval_id_;
        policy_.journal_store.relative_locator = deployment_.relative_locator;
        runtime_facts_.candidate_revision = runtime_candidate_revision_;
    }

    [[nodiscard]] SIQSShadowProofRssCampaignJournalSessionView view() const noexcept override {
        return make_session_view(replay_);
    }

    [[nodiscard]] SessionBeginSlotResult begin_next_slot() noexcept override {
        std::optional<DurablePublicationTrace> durable_publication;
        bool published_header_in_this_action = false;
        const auto fail = [&](StoreDiagnostic diagnostic) noexcept {
            return failure(
                with_durable_publication_trace(std::move(diagnostic), durable_publication));
        };
        try {
            if (replay_.status != SIQSShadowProofRssJournalStatus::ready ||
                replay_.reason != SIQSShadowProofRssJournalReason::ready ||
                (replay_.action != SIQSShadowProofRssJournalAction::create_header &&
                 replay_.action != SIQSShadowProofRssJournalAction::append_slot_start)) {
                return fail(action_diagnostic());
            }

            if (replay_.action == SIQSShadowProofRssJournalAction::create_header) {
                if (!replay_.header_to_create.has_value() ||
                    replay_.prepared_slot_start.has_value()) {
                    return fail(action_diagnostic());
                }
                const auto expected_header = *replay_.header_to_create;
                const auto encoded =
                    encode_siqs_shadow_proof_rss_campaign_journal_header(expected_header);
                if (!encoded) {
                    return fail(make_encode_diagnostic(encoded.error, encoded.error_offset,
                                                       StoreObject::journal_header));
                }
                if (StoreDiagnostic diagnostic = verify_authority();
                    diagnostic.error != StoreError::none) {
                    return fail(std::move(diagnostic));
                }

                const std::span<const std::byte> bytes(encoded.bytes->data(),
                                                       encoded.bytes->size());
                const auto publication = durable::publish_at(
                    static_cast<durable::NativeHandle>(root_fd_.get()), HEADER_LEAF, bytes);
                if (!publication.is_durable()) {
                    return fail(
                        make_publication_diagnostic(publication, StoreObject::journal_header));
                }
                durable_publication = DurablePublicationTrace{
                    .object = StoreObject::journal_header,
                    .bytes_written = publication.bytes_written(),
                };
                published_header_in_this_action = true;
                DirectoryAuthorityFingerprint refreshed_root_fingerprint;
                if (StoreDiagnostic diagnostic =
                        capture_authority_after_owned_publication(refreshed_root_fingerprint);
                    diagnostic.error != StoreError::none) {
                    return fail(with_completed_publication(std::move(diagnostic), publication));
                }

                VerifiedReplayResult refreshed = refresh(refreshed_root_fingerprint, nullptr);
                if (!refreshed) {
                    return fail(
                        with_completed_publication(std::move(refreshed.diagnostic), publication));
                }
                if (!refreshed.snapshot->header.has_value() ||
                    *refreshed.snapshot->header != expected_header ||
                    refreshed.snapshot->record_count != 0 ||
                    refreshed.replay->status != SIQSShadowProofRssJournalStatus::ready ||
                    refreshed.replay->reason != SIQSShadowProofRssJournalReason::ready ||
                    refreshed.replay->action !=
                        SIQSShadowProofRssJournalAction::append_slot_start ||
                    !refreshed.replay->prepared_slot_start.has_value()) {
                    return fail(with_completed_publication(
                        make_diagnostic(StoreError::snapshot_changed, StoreObject::journal_header),
                        publication));
                }
                initial_root_fingerprint_ = refreshed_root_fingerprint;
                root_namespace_fingerprint_ = *refreshed.root_namespace_fingerprint;
                replay_ = std::move(*refreshed.replay);
            }

            if (replay_.status != SIQSShadowProofRssJournalStatus::ready ||
                replay_.reason != SIQSShadowProofRssJournalReason::ready ||
                replay_.action != SIQSShadowProofRssJournalAction::append_slot_start ||
                replay_.header_to_create.has_value() || !replay_.prepared_slot_start.has_value()) {
                return fail(action_diagnostic());
            }

            const auto expected_start_record = replay_.prepared_slot_start->record();
            if (!published_header_in_this_action) {
                if (StoreDiagnostic diagnostic = verify_authority();
                    diagnostic.error != StoreError::none) {
                    return fail(std::move(diagnostic));
                }
                const auto header_confirmation = durable::confirm_durable_at(
                    static_cast<durable::NativeHandle>(root_fd_.get()), HEADER_LEAF);
                if (!header_confirmation.is_durable()) {
                    return fail(make_publication_diagnostic(header_confirmation,
                                                            StoreObject::journal_header));
                }
                VerifiedReplayResult confirmed =
                    refresh(initial_root_fingerprint_, &root_namespace_fingerprint_);
                if (!confirmed) {
                    return fail(with_completed_publication(std::move(confirmed.diagnostic),
                                                           header_confirmation));
                }
                if (!confirmed.snapshot->header.has_value() ||
                    confirmed.replay->status != SIQSShadowProofRssJournalStatus::ready ||
                    confirmed.replay->reason != SIQSShadowProofRssJournalReason::ready ||
                    confirmed.replay->action !=
                        SIQSShadowProofRssJournalAction::append_slot_start ||
                    !confirmed.replay->prepared_slot_start.has_value() ||
                    confirmed.replay->prepared_slot_start->record() != expected_start_record) {
                    return fail(with_completed_publication(
                        make_diagnostic(StoreError::snapshot_changed, StoreObject::journal_header),
                        header_confirmation));
                }
                replay_ = std::move(*confirmed.replay);
            }

            const auto start_record = replay_.prepared_slot_start->record();
            const auto leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(
                start_record.sequence_number);
            if (!leaf.has_value()) {
                return fail(action_diagnostic());
            }
            const auto encoded = encode_siqs_shadow_proof_rss_campaign_journal_record(start_record);
            if (!encoded) {
                return fail(make_encode_diagnostic(encoded.error, encoded.error_offset,
                                                   StoreObject::journal_record,
                                                   start_record.sequence_number));
            }
            if (StoreDiagnostic diagnostic = verify_authority();
                diagnostic.error != StoreError::none) {
                return fail(std::move(diagnostic));
            }

            const std::span<const std::byte> bytes(encoded.bytes->data(), encoded.bytes->size());
            const auto publication =
                durable::publish_at(static_cast<durable::NativeHandle>(root_fd_.get()),
                                    std::filesystem::path(leaf->view()), bytes);
            if (!publication.is_durable()) {
                return fail(make_publication_diagnostic(publication, StoreObject::journal_record,
                                                        start_record.sequence_number));
            }
            durable_publication = DurablePublicationTrace{
                .object = StoreObject::journal_record,
                .record_sequence = start_record.sequence_number,
                .bytes_written = publication.bytes_written(),
            };
            DirectoryAuthorityFingerprint refreshed_root_fingerprint;
            if (StoreDiagnostic diagnostic =
                    capture_authority_after_owned_publication(refreshed_root_fingerprint);
                diagnostic.error != StoreError::none) {
                return fail(with_completed_publication(std::move(diagnostic), publication));
            }

            VerifiedReplayResult refreshed = refresh(refreshed_root_fingerprint, nullptr);
            if (!refreshed) {
                return fail(
                    with_completed_publication(std::move(refreshed.diagnostic), publication));
            }
            const std::size_t expected_count =
                static_cast<std::size_t>(start_record.sequence_number);
            if (refreshed.snapshot->record_count != expected_count || expected_count == 0 ||
                refreshed.snapshot->records[expected_count - 1] != start_record ||
                refreshed.replay->status != SIQSShadowProofRssJournalStatus::tainted ||
                refreshed.replay->reason != SIQSShadowProofRssJournalReason::dangling_slot_start ||
                refreshed.replay->action != SIQSShadowProofRssJournalAction::append_taint ||
                refreshed.replay->committed_slot_count != replay_.committed_slot_count ||
                refreshed.replay->next_slot_number != start_record.slot_number) {
                return fail(with_completed_publication(
                    make_diagnostic(StoreError::snapshot_changed, StoreObject::journal_record, {},
                                    start_record.sequence_number),
                    publication));
            }
            initial_root_fingerprint_ = refreshed_root_fingerprint;
            root_namespace_fingerprint_ = *refreshed.root_namespace_fingerprint;
            if (StoreDiagnostic diagnostic = verify_authority();
                diagnostic.error != StoreError::none) {
                return fail(with_completed_publication(std::move(diagnostic), publication));
            }

            auto prepared = std::move(*replay_.prepared_slot_start);
            replay_.prepared_slot_start.reset();
            auto receipt = issue_durable_record_receipt(start_record);
            auto permit = acknowledge_siqs_shadow_proof_rss_durable_slot_start(std::move(prepared),
                                                                               std::move(receipt));
            if (!permit.has_value() || !permit->active()) {
                return fail(with_completed_publication(
                    make_diagnostic(StoreError::receipt_rejected, StoreObject::journal_record, {},
                                    start_record.sequence_number),
                    publication));
            }

            replay_ = std::move(*refreshed.replay);
            SessionBeginSlotResult result;
            result.permit = std::move(*permit);
            return result;
        } catch (const std::bad_alloc&) {
            return fail(make_diagnostic(StoreError::resource_exhausted));
        } catch (...) {
            return fail(make_diagnostic(StoreError::unexpected_failure));
        }
    }

private:
    [[nodiscard]] static SessionBeginSlotResult failure(StoreDiagnostic diagnostic) noexcept {
        SessionBeginSlotResult result;
        result.diagnostic = std::move(diagnostic);
        return result;
    }

    [[nodiscard]] StoreDiagnostic action_diagnostic() const noexcept {
        StoreDiagnostic diagnostic = make_diagnostic(StoreError::session_action_invalid);
        diagnostic.journal_reason = replay_.reason;
        return diagnostic;
    }

    [[nodiscard]] StoreDiagnostic verify_authority() const noexcept {
        StoreDiagnostic authority = verify_static_authority(initial_root_fingerprint_);
        if (authority.error != StoreError::none) {
            return authority;
        }
        return verify_root_namespace_generation(root_fd_.get(), root_namespace_fingerprint_);
    }

    [[nodiscard]] StoreDiagnostic verify_static_authority(
        const DirectoryAuthorityFingerprint& expected_root_fingerprint) const noexcept {
        StoreDiagnostic root = verify_root_identity(base_fd_.get(), root_fd_.get(), deployment_,
                                                    expected_root_fingerprint);
        if (root.error != StoreError::none) {
            return root;
        }
        return verify_lock_identity(root_fd_.get(), lock_fd_.get(), initial_lock_fingerprint_,
                                    deployment_.expected_owner);
    }

    [[nodiscard]] StoreDiagnostic capture_authority_after_owned_publication(
        DirectoryAuthorityFingerprint& refreshed_root_fingerprint) const noexcept {
        StoreDiagnostic root =
            verify_root_identity(base_fd_.get(), root_fd_.get(), deployment_,
                                 initial_root_fingerprint_, &refreshed_root_fingerprint);
        if (root.error != StoreError::none) {
            return root;
        }
        StoreDiagnostic lock = verify_lock_identity(
            root_fd_.get(), lock_fd_.get(), initial_lock_fingerprint_, deployment_.expected_owner);
        if (lock.error != StoreError::none) {
            return lock;
        }
        return {};
    }

    [[nodiscard]] VerifiedReplayResult
    refresh(const DirectoryAuthorityFingerprint& expected_root_fingerprint,
            const FileFingerprint* expected_namespace_fingerprint) const {
        VerifiedReplayResult result;
        if (StoreDiagnostic diagnostic = verify_static_authority(expected_root_fingerprint);
            diagnostic.error != StoreError::none) {
            result.diagnostic = std::move(diagnostic);
            return result;
        }
        struct stat namespace_before{};
        if (::fstat(root_fd_.get(), &namespace_before) != 0) {
            const int saved_errno = errno;
            result.diagnostic = make_diagnostic(StoreError::root_invalid, StoreObject::store_root,
                                                native_error(saved_errno));
            return result;
        }
        const FileFingerprint namespace_before_fingerprint = fingerprint(namespace_before);
        if (expected_namespace_fingerprint != nullptr &&
            namespace_before_fingerprint != *expected_namespace_fingerprint) {
            result.diagnostic =
                make_diagnostic(StoreError::snapshot_changed, StoreObject::directory);
            return result;
        }

        SnapshotResult first =
            capture_directory_snapshot(root_fd_.get(), lock_fd_.get(), deployment_.expected_owner);
        if (!first) {
            result.diagnostic = std::move(first.diagnostic);
            return result;
        }
        SnapshotResult second =
            capture_directory_snapshot(root_fd_.get(), lock_fd_.get(), deployment_.expected_owner);
        if (!second) {
            result.diagnostic = std::move(second.diagnostic);
            return result;
        }
        const SnapshotDifference difference = compare_snapshots(*first.snapshot, *second.snapshot);
        if (difference.different) {
            result.diagnostic = make_diagnostic(StoreError::snapshot_changed, difference.object, {},
                                                difference.record_sequence);
            return result;
        }
        struct stat namespace_after{};
        if (::fstat(root_fd_.get(), &namespace_after) != 0) {
            const int saved_errno = errno;
            result.diagnostic = make_diagnostic(StoreError::root_invalid, StoreObject::store_root,
                                                native_error(saved_errno));
            return result;
        }
        const FileFingerprint namespace_after_fingerprint = fingerprint(namespace_after);
        if (namespace_after_fingerprint != namespace_before_fingerprint ||
            (expected_namespace_fingerprint != nullptr &&
             namespace_after_fingerprint != *expected_namespace_fingerprint)) {
            result.diagnostic =
                make_diagnostic(StoreError::snapshot_changed, StoreObject::directory);
            return result;
        }
        if (StoreDiagnostic diagnostic = verify_static_authority(expected_root_fingerprint);
            diagnostic.error != StoreError::none) {
            result.diagnostic = std::move(diagnostic);
            return result;
        }

        const std::vector<LayoutEntry> projected = project_layout_entries(*second.snapshot);
        auto layout = inspect_siqs_shadow_proof_rss_campaign_journal_layout(projected);
        if (!layout) {
            result.diagnostic = make_layout_diagnostic(layout.diagnostic, *second.snapshot);
            return result;
        }
        const SIQSShadowProofRssCampaignJournalHeader* header =
            layout.value->header.has_value() ? &*layout.value->header : nullptr;
        auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy_, &runtime_facts_, layout.value->presence, header, layout.value->record_span());
        if (!replay_status_is_leasable(replay.status)) {
            result.diagnostic =
                make_replay_diagnostic(replay, policy_, runtime_facts_, *layout.value);
            return result;
        }
        result.snapshot = std::move(*layout.value);
        result.replay = std::move(replay);
        result.root_namespace_fingerprint = namespace_after_fingerprint;
        return result;
    }

    // Declaration order makes destruction release the lease first, then the
    // root and trusted-base capabilities.
    UniqueFd base_fd_;
    UniqueFd root_fd_;
    UniqueFd lock_fd_;
    std::string corpus_id_;
    std::string policy_candidate_revision_;
    std::string approval_id_;
    std::string runtime_candidate_revision_;
    DeploymentEntry deployment_;
    DirectoryAuthorityFingerprint initial_root_fingerprint_;
    FileFingerprint root_namespace_fingerprint_;
    FileFingerprint initial_lock_fingerprint_;
    SIQSShadowProofRssGatePolicy policy_;
    SIQSShadowProofRssCampaignRuntimeFacts runtime_facts_;
    SIQSShadowProofRssCampaignJournalResume replay_;
};

} // namespace

PlatformOpenResult open_siqs_shadow_proof_rss_campaign_journal_platform_session(
    const SIQSShadowProofRssGatePolicy& policy,
    const SIQSShadowProofRssCampaignRuntimeFacts& runtime_facts,
    const DeploymentEntry& deployment) noexcept {
    try {
        if (!deployment_matches_policy(policy, deployment)) {
            return {nullptr, make_diagnostic(StoreError::registry_binding_mismatch,
                                             StoreObject::deployment_registry)};
        }
        if (static_cast<uint64_t>(::geteuid()) != deployment.expected_owner) {
            return {nullptr, make_diagnostic(StoreError::registry_binding_mismatch,
                                             StoreObject::deployment_registry)};
        }
        FdOpenResult base = open_trusted_base(deployment);
        if (!base) {
            return {nullptr, std::move(base.diagnostic)};
        }

        FdOpenResult root = open_store_root(base.fd.get(), policy, deployment);
        if (!root) {
            return {nullptr, std::move(root.diagnostic)};
        }

        // The persistent lock is created/acquired before the first directory
        // open or readdir call and remains held by the returned session.
        LockOpenResult lock = open_and_lock_session(root.fd.get(), deployment.expected_owner);
        if (!lock) {
            return {nullptr, std::move(lock.diagnostic)};
        }

        struct stat root_metadata{};
        if (::fstat(root.fd.get(), &root_metadata) != 0) {
            const int saved_errno = errno;
            return {nullptr, make_diagnostic(StoreError::root_invalid, StoreObject::store_root,
                                             native_error(saved_errno))};
        }
        if (!S_ISDIR(root_metadata.st_mode)) {
            return {nullptr, make_diagnostic(StoreError::root_invalid, StoreObject::store_root)};
        }
        const DirectoryAuthorityFingerprint initial_root_fingerprint =
            directory_authority_fingerprint(root_metadata);
        const FileFingerprint initial_root_namespace_fingerprint = fingerprint(root_metadata);
        const FileFingerprint initial_lock_fingerprint = fingerprint(lock.metadata);

        SnapshotResult first =
            capture_directory_snapshot(root.fd.get(), lock.fd.get(), deployment.expected_owner);
        if (!first) {
            return {nullptr, std::move(first.diagnostic)};
        }
        SnapshotResult second =
            capture_directory_snapshot(root.fd.get(), lock.fd.get(), deployment.expected_owner);
        if (!second) {
            return {nullptr, std::move(second.diagnostic)};
        }

        const SnapshotDifference difference = compare_snapshots(*first.snapshot, *second.snapshot);
        if (difference.different) {
            return {nullptr, make_diagnostic(StoreError::snapshot_changed, difference.object, {},
                                             difference.record_sequence)};
        }

        StoreDiagnostic root_identity = verify_root_identity(base.fd.get(), root.fd.get(),
                                                             deployment, initial_root_fingerprint);
        if (root_identity.error != StoreError::none) {
            return {nullptr, std::move(root_identity)};
        }
        StoreDiagnostic root_namespace =
            verify_root_namespace_generation(root.fd.get(), initial_root_namespace_fingerprint);
        if (root_namespace.error != StoreError::none) {
            return {nullptr, std::move(root_namespace)};
        }
        StoreDiagnostic lock_identity = verify_lock_identity(
            root.fd.get(), lock.fd.get(), initial_lock_fingerprint, deployment.expected_owner);
        if (lock_identity.error != StoreError::none) {
            return {nullptr, std::move(lock_identity)};
        }

        const std::vector<LayoutEntry> projected = project_layout_entries(*second.snapshot);
        auto layout = inspect_siqs_shadow_proof_rss_campaign_journal_layout(projected);
        if (!layout) {
            return {nullptr, make_layout_diagnostic(layout.diagnostic, *second.snapshot)};
        }

        const SIQSShadowProofRssCampaignJournalHeader* header =
            layout.value->header.has_value() ? &*layout.value->header : nullptr;
        auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &runtime_facts, layout.value->presence, header, layout.value->record_span());
        if (!replay_status_is_leasable(replay.status)) {
            return {nullptr, make_replay_diagnostic(replay, policy, runtime_facts, *layout.value)};
        }

        PlatformOpenResult result;
        result.core = std::make_unique<PosixSessionCore>(
            std::move(base.fd), std::move(root.fd), std::move(lock.fd), policy, runtime_facts,
            deployment, initial_root_fingerprint, initial_root_namespace_fingerprint,
            initial_lock_fingerprint, std::move(replay));
        return result;
    } catch (const std::bad_alloc&) {
        PlatformOpenResult result;
        result.diagnostic.error = StoreError::resource_exhausted;
        return result;
    } catch (...) {
        PlatformOpenResult result;
        result.diagnostic.error = StoreError::unexpected_failure;
        return result;
    }
}

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
