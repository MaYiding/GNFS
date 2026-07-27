#include "distributed_sieve_wave_store_internal.hpp"

#include <gnfs/util/process.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#include <unistd.h>
#endif

namespace gnfs::sieve::distributed_sieve_resume_detail {
namespace {

namespace durable_record = gnfs::util::durable_immutable_record;

inline constexpr char LOCK_LEAF[] = ".gnfs-wave-v1.lock";
inline constexpr char MANIFEST_LEAF[] = ".gnfs-wave-v1.manifest";
inline constexpr char MANIFEST_PENDING_LEAF[] = ".gnfs-wave-v1.manifest.pending";

static_assert(std::string_view(LOCK_LEAF) == DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF);
static_assert(std::string_view(MANIFEST_LEAF) == DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF);
static_assert(std::string_view(MANIFEST_PENDING_LEAF) ==
              DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF);

[[nodiscard]] std::error_code invalid_argument_error() noexcept {
    return std::make_error_code(std::errc::invalid_argument);
}

[[nodiscard]] std::error_code protocol_error() noexcept {
    return std::make_error_code(std::errc::protocol_error);
}

[[nodiscard, maybe_unused]] std::error_code unsupported_error() noexcept {
    return std::make_error_code(std::errc::operation_not_supported);
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
diagnostic(DistributedSieveWaveStoreStatus status, std::error_code native_error = {}) noexcept {
    DistributedSieveWaveStoreDiagnostic result;
    result.status = status;
    result.native_error = native_error;
    return result;
}

[[nodiscard]] DistributedSieveWaveStoreOpenResult
open_failure(DistributedSieveWaveStoreDiagnostic failure) noexcept {
    return {nullptr, std::move(failure)};
}

[[nodiscard]] constexpr char decimal_digit(std::uint32_t value) noexcept {
    return static_cast<char>('0' + value);
}

[[nodiscard]] constexpr bool nil_identity(const NativeIdentityV1& identity) noexcept {
    return identity.volume == 0 && identity.object == 0 && identity.generation == 0;
}

[[nodiscard]] constexpr bool nil_digest(const util::Sha256Digest& digest) noexcept {
    return digest == util::Sha256Digest{};
}

struct FrozenRoot final {
    std::filesystem::path absolute;
    std::filesystem::path leaf;
    std::vector<std::string> parent_components;
};

[[nodiscard]] std::optional<FrozenRoot>
freeze_absolute_root(const std::filesystem::path& requested) {
    if (requested.empty() || !requested.is_absolute()) {
        return std::nullopt;
    }

    const auto& native = requested.native();
    if (native.empty() ||
        std::find(native.begin(), native.end(),
                  static_cast<std::filesystem::path::value_type>(0)) != native.end()) {
        return std::nullopt;
    }

#if defined(_WIN32)
    const auto is_separator = [](std::filesystem::path::value_type value) noexcept {
        return value == static_cast<std::filesystem::path::value_type>('/') ||
               value == static_cast<std::filesystem::path::value_type>('\\');
    };
    if (is_separator(native.back())) {
        return std::nullopt;
    }
    for (std::size_t index = 1; index < native.size(); ++index) {
        if (is_separator(native[index]) && is_separator(native[index - 1])) {
            return std::nullopt;
        }
    }
    const std::filesystem::path normalized = requested.lexically_normal();
    if (normalized != requested) {
        return std::nullopt;
    }
    for (const auto& component : requested.relative_path()) {
        if (component.empty() || component == "." || component == "..") {
            return std::nullopt;
        }
    }
    const std::filesystem::path leaf = requested.filename();
    if (leaf.empty() || leaf == "." || leaf == "..") {
        return std::nullopt;
    }
    return FrozenRoot{requested, leaf, {}};
#else
    if (native.front() != '/' || native.size() == 1 || native.back() == '/') {
        return std::nullopt;
    }

    std::vector<std::string> components;
    std::size_t begin = 1;
    while (begin < native.size()) {
        const std::size_t end = native.find('/', begin);
        const std::size_t component_end = end == std::string::npos ? native.size() : end;
        if (component_end == begin) {
            return std::nullopt;
        }
        std::string component = native.substr(begin, component_end - begin);
        if (component == "." || component == "..") {
            return std::nullopt;
        }
        components.push_back(std::move(component));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (components.empty()) {
        return std::nullopt;
    }

    std::filesystem::path leaf(components.back());
    components.pop_back();
    return FrozenRoot{requested, std::move(leaf), std::move(components)};
#endif
}

[[nodiscard]] bool valid_manifest_draft(const WaveManifestV1& draft) noexcept {
    return nil_identity(draft.wave_root_identity) && nil_identity(draft.permanent_lock_identity) &&
           draft.lock_semantics_version == 0 && nil_digest(draft.self_digest);
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_manifest_draft_semantics(const WaveManifestV1& draft) {
    WaveManifestV1 candidate = draft;
    candidate.wave_root_identity = NativeIdentityV1{.object = 1};
    candidate.permanent_lock_identity = NativeIdentityV1{.object = 2};
    candidate.lock_semantics_version = DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1;
    candidate.self_digest = {};
    const DistributedSieveProtocolRecordV1 record(std::move(candidate));
    return validate_distributed_sieve_record(record, false);
}

[[nodiscard]] std::uint64_t current_process_id() noexcept {
    const int process_id = util::process_id();
    return process_id > 0 ? static_cast<std::uint64_t>(process_id) : 0;
}

[[nodiscard]] bool process_matches(std::uint64_t expected_process_id) noexcept {
    return expected_process_id != 0 && current_process_id() == expected_process_id;
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic process_mismatch() noexcept {
    return diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error());
}

#if !defined(_WIN32)

[[nodiscard]] std::error_code posix_error(int value) noexcept {
    return {value, std::generic_category()};
}

class UniqueFd final {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return descriptor_ >= 0;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

    void reset(int replacement = -1) noexcept {
        if (descriptor_ >= 0) {
            // POSIX leaves descriptor ownership unspecified after EINTR.
            // Retrying close could close an unrelated reused descriptor.
            (void)::close(descriptor_);
        }
        descriptor_ = replacement;
    }

private:
    int descriptor_ = -1;
};

class UniqueDirectory final {
public:
    explicit UniqueDirectory(DIR* directory) noexcept : directory_(directory) {}

    ~UniqueDirectory() {
        if (directory_ != nullptr) {
            (void)::closedir(directory_);
        }
    }

    UniqueDirectory(const UniqueDirectory&) = delete;
    UniqueDirectory& operator=(const UniqueDirectory&) = delete;

private:
    DIR* directory_ = nullptr;
};

[[nodiscard]] int open_retrying_eintr(const char* path, int flags) noexcept {
    int descriptor = -1;
    do {
        descriptor = ::open(path, flags);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

[[nodiscard]] int openat_retrying_eintr(int parent, const char* leaf, int flags,
                                        mode_t mode = 0) noexcept {
    int descriptor = -1;
    do {
        descriptor = (flags & O_CREAT) != 0 ? ::openat(parent, leaf, flags, mode)
                                            : ::openat(parent, leaf, flags);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

struct ParentWalkResult final {
    UniqueFd parent;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(parent) &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] ParentWalkResult
walk_parent_no_follow(const std::vector<std::string>& parent_components,
                      std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return {{}, process_mismatch()};
    }
    const int root_fd = open_retrying_eintr("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) {
        return {{}, diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno))};
    }
    UniqueFd current(root_fd);
    for (const auto& component : parent_components) {
        if (!process_matches(creator_process_id)) {
            return {{}, process_mismatch()};
        }
        const int next_fd = openat_retrying_eintr(current.get(), component.c_str(),
                                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next_fd < 0) {
            return {{},
                    diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno))};
        }
        current.reset(next_fd);
    }
    if (!process_matches(creator_process_id)) {
        return {{}, process_mismatch()};
    }
    return {std::move(current), {}};
}

[[nodiscard]] durable_record::NativeIdentity record_identity(const struct stat& metadata) noexcept {
    return {
        .first = static_cast<std::uint64_t>(metadata.st_dev),
        .second = static_cast<std::uint64_t>(metadata.st_ino),
        .third = 0,
    };
}

[[nodiscard]] NativeIdentityV1 protocol_identity(const struct stat& metadata) noexcept {
    return {
        .volume = static_cast<std::uint64_t>(metadata.st_dev),
        .object = static_cast<std::uint64_t>(metadata.st_ino),
        .generation = 0,
    };
}

[[nodiscard]] bool exact_mode(const struct stat& metadata, mode_t expected) noexcept {
    return (metadata.st_mode & static_cast<mode_t>(07777)) == expected;
}

[[nodiscard]] bool stable_metadata(const struct stat& before, const struct stat& after) noexcept {
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_uid != after.st_uid || before.st_gid != after.st_gid ||
        before.st_mode != after.st_mode || before.st_nlink != after.st_nlink ||
        before.st_size != after.st_size) {
        return false;
    }
#if defined(__APPLE__)
    return before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
           before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    return before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
}

enum class AclInspectionState : std::uint8_t {
    absent,
    present,
    unsupported,
    failed,
};

struct AclInspection final {
    AclInspectionState state = AclInspectionState::absent;
    std::error_code error;
};

[[nodiscard]] bool unsupported_acl_error(int value) noexcept {
    return value == ENOTSUP || value == EOPNOTSUPP || value == ENOSYS;
}

#if defined(__linux__)
[[nodiscard]] AclInspection inspect_linux_acl_name(int descriptor, const char* name) noexcept {
    ssize_t size = -1;
    do {
        size = ::fgetxattr(descriptor, name, nullptr, 0);
    } while (size < 0 && errno == EINTR);
    if (size >= 0) {
        return {AclInspectionState::present, protocol_error()};
    }
    const int saved_errno = errno;
    if (saved_errno == ENODATA) {
        return {};
    }
    if (unsupported_acl_error(saved_errno)) {
        return {AclInspectionState::unsupported, posix_error(saved_errno)};
    }
    return {AclInspectionState::failed, posix_error(saved_errno)};
}
#endif

[[nodiscard]] AclInspection inspect_acl(int descriptor, bool directory) noexcept {
#if defined(__APPLE__)
    (void)directory;
    errno = 0;
    acl_t acl = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
    if (acl == nullptr) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return {};
        }
        if (unsupported_acl_error(saved_errno)) {
            return {AclInspectionState::unsupported, posix_error(saved_errno)};
        }
        return {AclInspectionState::failed, posix_error(saved_errno)};
    }
    (void)::acl_free(acl);
    return {AclInspectionState::present, protocol_error()};
#elif defined(__linux__)
    const auto access = inspect_linux_acl_name(descriptor, "system.posix_acl_access");
    if (access.state != AclInspectionState::absent || !directory) {
        return access;
    }
    return inspect_linux_acl_name(descriptor, "system.posix_acl_default");
#else
    (void)descriptor;
    (void)directory;
    return {AclInspectionState::unsupported, unsupported_error()};
#endif
}

[[nodiscard]] std::optional<DistributedSieveWaveStoreDiagnostic>
acl_rejection(int descriptor, bool directory,
              DistributedSieveWaveStoreStatus rejected_status) noexcept {
    const auto inspected = inspect_acl(descriptor, directory);
    switch (inspected.state) {
    case AclInspectionState::absent:
        return std::nullopt;
    case AclInspectionState::present:
        return diagnostic(rejected_status, inspected.error ? inspected.error : protocol_error());
    case AclInspectionState::unsupported:
        return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                          inspected.error ? inspected.error : unsupported_error());
    case AclInspectionState::failed:
        return diagnostic(rejected_status, inspected.error ? inspected.error : protocol_error());
    }
    return diagnostic(rejected_status, protocol_error());
}

[[nodiscard]] bool sync_descriptor(int descriptor, std::error_code& error) noexcept {
    int result = -1;
    do {
#if defined(__APPLE__)
        result = ::fcntl(descriptor, F_FULLFSYNC);
#else
        result = ::fsync(descriptor);
#endif
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return true;
    }
    error = posix_error(errno);
    return false;
}

[[nodiscard]] int fstat_retrying_eintr(int descriptor, struct stat& metadata) noexcept {
    int result = -1;
    do {
        result = ::fstat(descriptor, &metadata);
    } while (result != 0 && errno == EINTR);
    return result;
}

[[nodiscard]] int fstatat_retrying_eintr(int parent, const char* leaf,
                                         struct stat& metadata) noexcept {
    int result = -1;
    do {
        result = ::fstatat(parent, leaf, &metadata, AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    return result;
}

[[nodiscard]] bool valid_root_metadata(const struct stat& metadata) noexcept {
    return S_ISDIR(metadata.st_mode) &&
           static_cast<std::uint64_t>(metadata.st_uid) == static_cast<std::uint64_t>(::geteuid()) &&
           exact_mode(metadata, 0700);
}

[[nodiscard]] bool valid_parent_metadata(const struct stat& metadata) noexcept {
    return S_ISDIR(metadata.st_mode) &&
           static_cast<std::uint64_t>(metadata.st_uid) == static_cast<std::uint64_t>(::geteuid()) &&
           (metadata.st_mode & static_cast<mode_t>(S_IWGRP | S_IWOTH)) == 0;
}

[[nodiscard]] bool valid_lock_metadata(const struct stat& metadata) noexcept {
    return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 && metadata.st_size == 0 &&
           static_cast<std::uint64_t>(metadata.st_uid) == static_cast<std::uint64_t>(::geteuid()) &&
           exact_mode(metadata, 0600);
}

[[nodiscard]] bool valid_manifest_metadata(const struct stat& metadata) noexcept {
    return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 && metadata.st_size > 0 &&
           static_cast<std::uint64_t>(metadata.st_size) <=
               DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES &&
           static_cast<std::uint64_t>(metadata.st_uid) == static_cast<std::uint64_t>(::geteuid()) &&
           exact_mode(metadata, 0600);
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
validate_parent_binding(int parent_fd, const std::vector<std::string>& parent_components,
                        std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    struct stat held_before{};
    if (fstat_retrying_eintr(parent_fd, held_before) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno));
    }
    if (auto acl = acl_rejection(parent_fd, true, DistributedSieveWaveStoreStatus::root_invalid);
        acl.has_value()) {
        return *acl;
    }
    auto named_parent = walk_parent_no_follow(parent_components, creator_process_id);
    if (!named_parent) {
        return named_parent.diagnostic;
    }
    struct stat named{};
    if (fstat_retrying_eintr(named_parent.parent.get(), named) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno));
    }
    if (auto acl = acl_rejection(named_parent.parent.get(), true,
                                 DistributedSieveWaveStoreStatus::root_invalid);
        acl.has_value()) {
        return *acl;
    }
    struct stat held_after{};
    if (fstat_retrying_eintr(parent_fd, held_after) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno));
    }
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    if (auto acl = acl_rejection(parent_fd, true, DistributedSieveWaveStoreStatus::root_invalid);
        acl.has_value()) {
        return *acl;
    }
    if (!valid_parent_metadata(held_before) || !valid_parent_metadata(named) ||
        !valid_parent_metadata(held_after) || !stable_metadata(held_before, held_after) ||
        !stable_metadata(held_after, named)) {
        return diagnostic(DistributedSieveWaveStoreStatus::root_invalid, protocol_error());
    }
    return {};
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
validate_root_binding(int parent_fd, const std::vector<std::string>& parent_components, int root_fd,
                      const std::filesystem::path& root_leaf, std::uint64_t creator_process_id,
                      std::optional<NativeIdentityV1> expected = std::nullopt) noexcept {
    if (const auto parent =
            validate_parent_binding(parent_fd, parent_components, creator_process_id);
        parent.status != DistributedSieveWaveStoreStatus::ready) {
        return parent;
    }

    struct stat held_before{};
    if (fstat_retrying_eintr(root_fd, held_before) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno));
    }
    if (auto acl = acl_rejection(root_fd, true, DistributedSieveWaveStoreStatus::root_invalid);
        acl.has_value()) {
        return *acl;
    }
    struct stat named{};
    if (fstatat_retrying_eintr(parent_fd, root_leaf.c_str(), named) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno));
    }
    struct stat held_after{};
    if (fstat_retrying_eintr(root_fd, held_after) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno));
    }
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    if (auto acl = acl_rejection(root_fd, true, DistributedSieveWaveStoreStatus::root_invalid);
        acl.has_value()) {
        return *acl;
    }
    if (!valid_root_metadata(held_before) || !valid_root_metadata(named) ||
        !valid_root_metadata(held_after) || !stable_metadata(held_before, held_after) ||
        !stable_metadata(held_after, named)) {
        return diagnostic(DistributedSieveWaveStoreStatus::root_invalid, protocol_error());
    }
    if (expected.has_value() && protocol_identity(held_after) != *expected) {
        return diagnostic(DistributedSieveWaveStoreStatus::root_invalid, protocol_error());
    }
    return {};
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
validate_lock_binding(int root_fd, int lock_fd, std::uint64_t creator_process_id,
                      std::optional<NativeIdentityV1> expected = std::nullopt) noexcept {
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    struct stat held_before{};
    if (fstat_retrying_eintr(lock_fd, held_before) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::lock_invalid, posix_error(errno));
    }
    if (auto acl = acl_rejection(lock_fd, false, DistributedSieveWaveStoreStatus::lock_invalid);
        acl.has_value()) {
        return *acl;
    }
    struct stat named{};
    if (fstatat_retrying_eintr(root_fd, LOCK_LEAF, named) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::lock_invalid, posix_error(errno));
    }
    struct stat held_after{};
    if (fstat_retrying_eintr(lock_fd, held_after) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::lock_invalid, posix_error(errno));
    }
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    if (auto acl = acl_rejection(lock_fd, false, DistributedSieveWaveStoreStatus::lock_invalid);
        acl.has_value()) {
        return *acl;
    }
    if (!valid_lock_metadata(held_before) || !valid_lock_metadata(named) ||
        !valid_lock_metadata(held_after) || !stable_metadata(held_before, held_after) ||
        !stable_metadata(held_after, named)) {
        return diagnostic(DistributedSieveWaveStoreStatus::lock_invalid, protocol_error());
    }
    if (expected.has_value() && protocol_identity(held_after) != *expected) {
        return diagnostic(DistributedSieveWaveStoreStatus::lock_invalid, protocol_error());
    }
    return {};
}

struct NamespaceInventory final {
    bool lock = false;
    bool manifest = false;
    bool pending = false;
    std::vector<std::string> private_lease_base_lock_leaves;

    [[nodiscard]] friend constexpr bool operator==(const NamespaceInventory&,
                                                   const NamespaceInventory&) noexcept = default;
};

struct NamespaceInventoryResult final {
    std::optional<NamespaceInventory> inventory;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return inventory.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] NamespaceInventoryResult inspect_namespace(int root_fd) noexcept {
    const int scan_fd =
        openat_retrying_eintr(root_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (scan_fd < 0) {
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno))};
    }
    DIR* raw_directory = ::fdopendir(scan_fd);
    if (raw_directory == nullptr) {
        const int saved_errno = errno;
        (void)::close(scan_fd);
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(saved_errno))};
    }
    UniqueDirectory directory(raw_directory);

    NamespaceInventory inventory;
    errno = 0;
    for (;;) {
        dirent* entry = ::readdir(raw_directory);
        if (entry == nullptr) {
            if (errno != 0) {
                return {std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno))};
            }
            break;
        }
        const std::string_view leaf(entry->d_name);
        if (leaf == "." || leaf == "..") {
            continue;
        }
        if (leaf == LOCK_LEAF) {
            if (inventory.lock) {
                return {std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                   protocol_error())};
            }
            inventory.lock = true;
            continue;
        }
        if (leaf == MANIFEST_LEAF) {
            if (inventory.manifest) {
                return {std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                   protocol_error())};
            }
            inventory.manifest = true;
            continue;
        }
        if (leaf == MANIFEST_PENDING_LEAF) {
            if (inventory.pending) {
                return {std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                   protocol_error())};
            }
            inventory.pending = true;
            continue;
        }
        if (leaf.ends_with(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX)) {
            const std::size_t stem_size =
                leaf.size() - DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX.size();
            constexpr std::size_t max_base_lock_count =
                static_cast<std::size_t>(DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) *
                DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
            if (stem_size == 0 || stem_size > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES ||
                inventory.private_lease_base_lock_leaves.size() >= max_base_lock_count) {
                return {std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                   protocol_error())};
            }
            try {
                inventory.private_lease_base_lock_leaves.emplace_back(leaf);
            } catch (const std::bad_alloc&) {
                return {std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                   std::make_error_code(std::errc::not_enough_memory))};
            } catch (...) {
                return {std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                   std::make_error_code(std::errc::io_error))};
            }
            continue;
        }
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    std::sort(inventory.private_lease_base_lock_leaves.begin(),
              inventory.private_lease_base_lock_leaves.end());
    return {std::move(inventory), {}};
}

[[nodiscard]] bool
manifest_attempt_matches_private_lease_base_lock(const WaveManifestV1& manifest,
                                                 std::string_view leaf) noexcept {
    if (!leaf.ends_with(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX)) {
        return false;
    }
    leaf.remove_suffix(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX.size());
    constexpr std::size_t attempt_suffix_size =
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
    if (leaf.size() <= attempt_suffix_size) {
        return false;
    }
    const std::size_t attempt_tag_offset = leaf.size() - attempt_suffix_size;
    if (leaf.substr(attempt_tag_offset, DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size()) !=
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1) {
        return false;
    }
    const std::size_t digits_offset =
        attempt_tag_offset + DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size();
    if (leaf[digits_offset] < '0' || leaf[digits_offset] > '9' || leaf[digits_offset + 1U] < '0' ||
        leaf[digits_offset + 1U] > '9') {
        return false;
    }
    const auto attempt_ordinal = static_cast<std::uint32_t>(leaf[digits_offset] - '0') * 10U +
                                 static_cast<std::uint32_t>(leaf[digits_offset + 1U] - '0');
    if (attempt_ordinal >= manifest.max_worker_attempts) {
        return false;
    }

    const std::string_view chunk_stem = leaf.substr(0, attempt_tag_offset);
    const ChunkPlanV1* matched_chunk = nullptr;
    for (const auto& chunk : manifest.chunks) {
        if (chunk.relative_artifact_stem == chunk_stem) {
            if (matched_chunk != nullptr || chunk.sq_begin >= chunk.sq_end) {
                return false;
            }
            matched_chunk = &chunk;
        }
    }
    return matched_chunk != nullptr &&
           distributed_sieve_worker_attempt_relative_stem_matches(
               matched_chunk->relative_artifact_stem, attempt_ordinal, leaf);
}

struct PrivateLeaseBaseLockLeafValidationResult final {
    std::optional<NativeIdentityV1> identity;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return identity.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] PrivateLeaseBaseLockLeafValidationResult validate_private_lease_base_lock_binding(
    int root_fd, int lock_fd, const std::string& leaf, std::uint64_t creator_process_id,
    std::optional<NativeIdentityV1> expected_identity = std::nullopt) noexcept {
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, process_mismatch()};
    }

    struct stat held_before{};
    if (fstat_retrying_eintr(lock_fd, held_before) != 0) {
        return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                         posix_error(errno))};
    }
    if (auto acl =
            acl_rejection(lock_fd, false, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return {std::nullopt, *acl};
    }
    struct stat named{};
    if (fstatat_retrying_eintr(root_fd, leaf.c_str(), named) != 0) {
        return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                         posix_error(errno))};
    }
    struct stat held_after{};
    if (fstat_retrying_eintr(lock_fd, held_after) != 0) {
        return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                         posix_error(errno))};
    }
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, process_mismatch()};
    }
    if (auto acl =
            acl_rejection(lock_fd, false, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return {std::nullopt, *acl};
    }
    if (!valid_lock_metadata(held_before) || !valid_lock_metadata(named) ||
        !valid_lock_metadata(held_after) || !stable_metadata(held_before, held_after) ||
        !stable_metadata(held_after, named)) {
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    const NativeIdentityV1 identity = protocol_identity(held_after);
    if (expected_identity.has_value() && identity != *expected_identity) {
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    return {identity, {}};
}

[[nodiscard]] PrivateLeaseBaseLockLeafValidationResult
validate_private_lease_base_lock_leaf(int root_fd, const std::string& leaf,
                                      std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, process_mismatch()};
    }
    const int descriptor = openat_retrying_eintr(root_fd, leaf.c_str(),
                                                 O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                         posix_error(errno))};
    }
    UniqueFd held(descriptor);
    return validate_private_lease_base_lock_binding(root_fd, held.get(), leaf, creator_process_id);
}

struct PrivateLeaseBaseLockInventoryValidationResult final {
    std::optional<std::vector<NativeIdentityV1>> identities;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return identities.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] PrivateLeaseBaseLockInventoryValidationResult
validate_private_lease_base_lock_inventory(int root_fd, const NamespaceInventory& inventory,
                                           const WaveManifestV1& manifest,
                                           std::uint64_t creator_process_id) noexcept {
    const std::size_t maximum_base_locks =
        manifest.chunks.size() * static_cast<std::size_t>(manifest.max_worker_attempts);
    if (inventory.private_lease_base_lock_leaves.size() > maximum_base_locks) {
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    std::vector<NativeIdentityV1> identities;
    try {
        identities.resize(inventory.private_lease_base_lock_leaves.size());
    } catch (const std::bad_alloc&) {
        return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                         std::make_error_code(std::errc::not_enough_memory))};
    } catch (...) {
        return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                         std::make_error_code(std::errc::io_error))};
    }
    for (std::size_t index = 0; index < inventory.private_lease_base_lock_leaves.size(); ++index) {
        const auto& leaf = inventory.private_lease_base_lock_leaves[index];
        if (!manifest_attempt_matches_private_lease_base_lock(manifest, leaf)) {
            return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                             protocol_error())};
        }
        const auto validated =
            validate_private_lease_base_lock_leaf(root_fd, leaf, creator_process_id);
        if (!validated) {
            return {std::nullopt, validated.diagnostic};
        }
        identities[index] = *validated.identity;
    }
    return {std::move(identities), {}};
}

[[nodiscard]] PrivateLeaseBaseLockInventoryValidationResult
validate_manifest_bound_inventory(int root_fd, const NamespaceInventory& inventory,
                                  const WaveManifestV1& manifest,
                                  std::uint64_t creator_process_id) noexcept {
    if (!inventory.lock || !inventory.manifest || inventory.pending) {
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    return validate_private_lease_base_lock_inventory(root_fd, inventory, manifest,
                                                      creator_process_id);
}

struct ManifestBoundInventoryWitnessResult final {
    std::optional<NamespaceInventory> inventory;
    std::optional<std::vector<NativeIdentityV1>> base_lock_identities;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return inventory.has_value() && base_lock_identities.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] ManifestBoundInventoryWitnessResult
capture_manifest_bound_inventory_witness(int root_fd, const WaveManifestV1& manifest,
                                         std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, std::nullopt, process_mismatch()};
    }
    auto inventory = inspect_namespace(root_fd);
    if (!inventory) {
        return {std::nullopt, std::nullopt, std::move(inventory.diagnostic)};
    }
    auto validated = validate_manifest_bound_inventory(root_fd, *inventory.inventory, manifest,
                                                       creator_process_id);
    if (!validated) {
        return {std::nullopt, std::nullopt, std::move(validated.diagnostic)};
    }
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, std::nullopt, process_mismatch()};
    }
    return {std::move(inventory.inventory), std::move(validated.identities), {}};
}

struct RootOpenResult final {
    UniqueFd parent;
    UniqueFd root;
    NativeIdentityV1 root_identity;
    bool created = false;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(parent) && static_cast<bool>(root) &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] RootOpenResult open_root(const FrozenRoot& frozen, bool create,
                                       std::uint64_t creator_process_id) noexcept {
    RootOpenResult result;
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }
    auto parent = walk_parent_no_follow(frozen.parent_components, creator_process_id);
    if (!parent) {
        result.diagnostic = parent.diagnostic;
        return result;
    }
    result.parent = std::move(parent.parent);
    if (const auto parent_validated = validate_parent_binding(
            result.parent.get(), frozen.parent_components, creator_process_id);
        parent_validated.status != DistributedSieveWaveStoreStatus::ready) {
        result.diagnostic = parent_validated;
        return result;
    }
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }

    if (create) {
        // Parent trust, ACL support, and stable no-follow reachability have all
        // been proven before this first namespace mutation.
        int created = -1;
        do {
            created = ::mkdirat(result.parent.get(), frozen.leaf.c_str(), 0700);
        } while (created != 0 && errno == EINTR);
        if (created == 0) {
            result.created = true;
        } else if (errno != EEXIST) {
            result.diagnostic =
                diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno));
            return result;
        }
        if (!process_matches(creator_process_id)) {
            result.diagnostic = process_mismatch();
            return result;
        }
    }

    const int root_fd = openat_retrying_eintr(result.parent.get(), frozen.leaf.c_str(),
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) {
        const int saved_errno = errno;
        result.diagnostic =
            diagnostic(saved_errno == ENOENT ? DistributedSieveWaveStoreStatus::root_missing
                                             : DistributedSieveWaveStoreStatus::root_invalid,
                       posix_error(saved_errno));
        return result;
    }
    result.root = UniqueFd(root_fd);
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }

    if (result.created) {
        int mode_result = -1;
        do {
            mode_result = ::fchmod(result.root.get(), 0700);
        } while (mode_result != 0 && errno == EINTR);
        if (mode_result != 0) {
            result.diagnostic =
                diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno));
            return result;
        }
    }

    if (const auto validated =
            validate_root_binding(result.parent.get(), frozen.parent_components, result.root.get(),
                                  frozen.leaf, creator_process_id);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        result.diagnostic = validated;
        return result;
    }
    struct stat metadata{};
    if (fstat_retrying_eintr(result.root.get(), metadata) != 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::root_invalid, posix_error(errno));
        return result;
    }
    result.root_identity = protocol_identity(metadata);

    if (create) {
        if (!process_matches(creator_process_id)) {
            result.diagnostic = process_mismatch();
            return result;
        }
        std::error_code sync_error;
        if (!sync_descriptor(result.root.get(), sync_error) ||
            !sync_descriptor(result.parent.get(), sync_error)) {
            result.diagnostic =
                diagnostic(DistributedSieveWaveStoreStatus::durability_failed, sync_error);
            return result;
        }
        if (!process_matches(creator_process_id)) {
            result.diagnostic = process_mismatch();
            return result;
        }
        if (const auto validated = validate_root_binding(
                result.parent.get(), frozen.parent_components, result.root.get(), frozen.leaf,
                creator_process_id, result.root_identity);
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            result.diagnostic = validated;
            return result;
        }
    }
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }
    return result;
}

struct LockOpenResult final {
    UniqueFd lock;
    NativeIdentityV1 lock_identity;
    bool created = false;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(lock) &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] LockOpenResult open_lock(int root_fd, const NamespaceInventory& before, bool create,
                                       std::uint64_t creator_process_id) noexcept {
    LockOpenResult result;
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }
    if (auto acl = acl_rejection(root_fd, true, DistributedSieveWaveStoreStatus::root_invalid);
        acl.has_value()) {
        result.diagnostic = *acl;
        return result;
    }
    if (!before.lock && !create) {
        result.diagnostic = diagnostic(DistributedSieveWaveStoreStatus::lock_missing);
        return result;
    }
    if (!before.lock &&
        (before.manifest || before.pending || !before.private_lease_base_lock_leaves.empty())) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
        return result;
    }

    constexpr int common_flags = O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    int lock_fd = -1;
    if (create && !before.lock) {
        if (!process_matches(creator_process_id)) {
            result.diagnostic = process_mismatch();
            return result;
        }
        lock_fd = openat_retrying_eintr(root_fd, LOCK_LEAF, common_flags | O_CREAT | O_EXCL, 0600);
        if (lock_fd >= 0) {
            result.created = true;
        }
    }
    if (lock_fd < 0 && ((!create && before.lock) || (create && before.lock) ||
                        (create && !before.lock && errno == EEXIST))) {
        lock_fd = openat_retrying_eintr(root_fd, LOCK_LEAF, common_flags);
    }
    if (lock_fd < 0) {
        const int saved_errno = errno;
        result.diagnostic =
            diagnostic(saved_errno == ENOENT ? DistributedSieveWaveStoreStatus::lock_missing
                                             : DistributedSieveWaveStoreStatus::lock_invalid,
                       posix_error(saved_errno));
        return result;
    }
    result.lock = UniqueFd(lock_fd);
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }

    if (result.created) {
        int mode_result = -1;
        do {
            mode_result = ::fchmod(result.lock.get(), 0600);
        } while (mode_result != 0 && errno == EINTR);
        if (mode_result != 0) {
            result.diagnostic =
                diagnostic(DistributedSieveWaveStoreStatus::lock_invalid, posix_error(errno));
            return result;
        }
    }

    if (const auto validated =
            validate_lock_binding(root_fd, result.lock.get(), creator_process_id);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        result.diagnostic = validated;
        return result;
    }

    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }
    int locked = -1;
    do {
        locked = ::flock(result.lock.get(), LOCK_EX | LOCK_NB);
    } while (locked != 0 && errno == EINTR);
    if (locked != 0) {
        const int saved_errno = errno;
        if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
            result.diagnostic =
                diagnostic(DistributedSieveWaveStoreStatus::lock_busy, posix_error(saved_errno));
        } else if (saved_errno == ENOTSUP || saved_errno == EOPNOTSUPP || saved_errno == ENOSYS) {
            result.diagnostic = diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                                           posix_error(saved_errno));
        } else {
            result.diagnostic =
                diagnostic(DistributedSieveWaveStoreStatus::lock_invalid, posix_error(saved_errno));
        }
        return result;
    }
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }

    if (const auto validated =
            validate_lock_binding(root_fd, result.lock.get(), creator_process_id);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        result.diagnostic = validated;
        return result;
    }
    struct stat metadata{};
    if (fstat_retrying_eintr(result.lock.get(), metadata) != 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::lock_invalid, posix_error(errno));
        return result;
    }
    result.lock_identity = protocol_identity(metadata);

    if (create) {
        if (!process_matches(creator_process_id)) {
            result.diagnostic = process_mismatch();
            return result;
        }
        std::error_code sync_error;
        if (!sync_descriptor(result.lock.get(), sync_error) ||
            !sync_descriptor(root_fd, sync_error)) {
            result.diagnostic =
                diagnostic(DistributedSieveWaveStoreStatus::durability_failed, sync_error);
            return result;
        }
        if (!process_matches(creator_process_id)) {
            result.diagnostic = process_mismatch();
            return result;
        }
        if (const auto validated = validate_lock_binding(root_fd, result.lock.get(),
                                                         creator_process_id, result.lock_identity);
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            result.diagnostic = validated;
            return result;
        }
    }
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }
    return result;
}

struct ManifestReadResult final {
    std::optional<std::vector<std::byte>> bytes;
    std::optional<durable_record::RecordSnapshot> snapshot;
    bool missing = false;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return bytes.has_value() && snapshot.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] ManifestReadResult read_manifest_leaf(int root_fd, const char* leaf,
                                                    std::uint64_t creator_process_id) noexcept {
    ManifestReadResult result;
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }
    const int manifest_fd =
        openat_retrying_eintr(root_fd, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (manifest_fd < 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            result.missing = true;
            result.diagnostic = diagnostic(DistributedSieveWaveStoreStatus::manifest_missing,
                                           posix_error(saved_errno));
        } else {
            result.diagnostic = diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid,
                                           posix_error(saved_errno));
        }
        return result;
    }
    UniqueFd manifest(manifest_fd);
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }

    struct stat held_before{};
    if (fstat_retrying_eintr(manifest.get(), held_before) != 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
        return result;
    }
    struct stat named_before{};
    if (fstatat_retrying_eintr(root_fd, leaf, named_before) != 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid, posix_error(errno));
        return result;
    }
    if (!valid_manifest_metadata(held_before) || !valid_manifest_metadata(named_before) ||
        !stable_metadata(held_before, named_before)) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid, protocol_error());
        return result;
    }
    if (auto acl =
            acl_rejection(manifest.get(), false, DistributedSieveWaveStoreStatus::manifest_invalid);
        acl.has_value()) {
        result.diagnostic = *acl;
        return result;
    }

    const std::size_t exact_size = static_cast<std::size_t>(held_before.st_size);
    std::vector<std::byte> bytes;
    try {
        bytes.resize(exact_size);
    } catch (const std::bad_alloc&) {
        result.diagnostic = diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                       std::make_error_code(std::errc::not_enough_memory));
        return result;
    } catch (...) {
        result.diagnostic = diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                       std::make_error_code(std::errc::io_error));
        return result;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (!process_matches(creator_process_id)) {
            result.diagnostic = process_mismatch();
            return result;
        }
        const std::size_t request = std::min(
            bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t count =
            ::pread(manifest.get(), bytes.data() + offset, request, static_cast<off_t>(offset));
        if (count < 0) {
            const int saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            result.diagnostic =
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(saved_errno));
            return result;
        }
        if (count == 0) {
            result.diagnostic =
                diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid, protocol_error());
            return result;
        }
        offset += static_cast<std::size_t>(count);
    }

    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }
    std::byte trailing{};
    ssize_t trailing_count = -1;
    do {
        trailing_count = ::pread(manifest.get(), &trailing, 1, static_cast<off_t>(bytes.size()));
    } while (trailing_count < 0 && errno == EINTR);
    if (trailing_count < 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
        return result;
    }
    if (trailing_count != 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid, protocol_error());
        return result;
    }

    struct stat held_after{};
    if (fstat_retrying_eintr(manifest.get(), held_after) != 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
        return result;
    }
    struct stat named_after{};
    if (fstatat_retrying_eintr(root_fd, leaf, named_after) != 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid, posix_error(errno));
        return result;
    }
    if (!valid_manifest_metadata(held_after) || !valid_manifest_metadata(named_after) ||
        !stable_metadata(held_before, held_after) || !stable_metadata(held_after, named_after)) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid, protocol_error());
        return result;
    }
    if (auto acl =
            acl_rejection(manifest.get(), false, DistributedSieveWaveStoreStatus::manifest_invalid);
        acl.has_value()) {
        result.diagnostic = *acl;
        return result;
    }
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }

    result.snapshot = durable_record::RecordSnapshot{
        .identity = record_identity(held_after),
        .size = static_cast<std::uint64_t>(held_after.st_size),
    };
    result.bytes = std::move(bytes);
    return result;
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
require_manifest_pending_missing(int root_fd, std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    struct stat named{};
    if (fstatat_retrying_eintr(root_fd, MANIFEST_PENDING_LEAF, named) == 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    const int saved_errno = errno;
    if (saved_errno != ENOENT) {
        return diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid,
                          posix_error(saved_errno));
    }
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    return {};
}

struct DecodedManifestResult final {
    std::optional<WaveManifestV1> manifest;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return manifest.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] DecodedManifestResult decode_manifest(
    std::span<const std::byte> bytes, const util::Sha256Digest& expected_manifest_digest,
    const NativeIdentityV1& root_identity, const NativeIdentityV1& lock_identity) noexcept {
    auto decoded = decode_distributed_sieve_record(bytes);
    if (!decoded) {
        auto failure =
            diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid, protocol_error());
        failure.protocol_status = decoded.status;
        return {std::nullopt, std::move(failure)};
    }
    WaveManifestV1* manifest = std::get_if<WaveManifestV1>(&*decoded.value);
    if (manifest == nullptr) {
        auto failure =
            diagnostic(DistributedSieveWaveStoreStatus::manifest_invalid, protocol_error());
        failure.protocol_status = DistributedSieveProtocolStatus{
            .error = DistributedSieveProtocolError::record_type_mismatch,
        };
        return {std::nullopt, std::move(failure)};
    }
    if (manifest->self_digest != expected_manifest_digest ||
        manifest->wave_root_identity != root_identity ||
        manifest->permanent_lock_identity != lock_identity ||
        manifest->lock_semantics_version != DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1) {
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::manifest_conflict, protocol_error())};
    }
    return {std::move(*manifest), {}};
}

struct ExistingManifestResult final {
    std::optional<std::vector<std::byte>> bytes;
    std::optional<WaveManifestV1> manifest;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return bytes.has_value() && manifest.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] ExistingManifestResult
read_existing_manifest(int root_fd, const NamespaceInventory& inventory,
                       const util::Sha256Digest& expected_manifest_digest,
                       const NativeIdentityV1& root_identity, const NativeIdentityV1& lock_identity,
                       std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, std::nullopt, process_mismatch()};
    }
    if (!inventory.manifest && !inventory.pending) {
        return {std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::manifest_missing)};
    }

    std::optional<ManifestReadResult> canonical;
    std::optional<DecodedManifestResult> decoded_canonical;
    if (inventory.manifest) {
        canonical.emplace(read_manifest_leaf(root_fd, MANIFEST_LEAF, creator_process_id));
        if (!*canonical) {
            return {std::nullopt, std::nullopt, canonical->diagnostic};
        }
        decoded_canonical.emplace(decode_manifest(*canonical->bytes, expected_manifest_digest,
                                                  root_identity, lock_identity));
        if (!*decoded_canonical) {
            return {std::nullopt, std::nullopt, decoded_canonical->diagnostic};
        }
    }

    std::optional<ManifestReadResult> pending;
    std::optional<DecodedManifestResult> decoded_pending;
    if (inventory.pending) {
        pending.emplace(read_manifest_leaf(root_fd, MANIFEST_PENDING_LEAF, creator_process_id));
        if (!*pending) {
            return {std::nullopt, std::nullopt, pending->diagnostic};
        }
        decoded_pending.emplace(decode_manifest(*pending->bytes, expected_manifest_digest,
                                                root_identity, lock_identity));
        if (!*decoded_pending) {
            return {std::nullopt, std::nullopt, decoded_pending->diagnostic};
        }
    }

    if (canonical.has_value() && pending.has_value() && *canonical->bytes != *pending->bytes) {
        return {std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::manifest_conflict, protocol_error())};
    }
    if (canonical.has_value()) {
        return {std::move(canonical->bytes), std::move(decoded_canonical->manifest), {}};
    }
    return {std::move(pending->bytes), std::move(decoded_pending->manifest), {}};
}

struct ManifestHookBridge final {
    DistributedSieveWaveStoreTestHooks hooks;
    std::uint64_t creator_process_id = 0;
    bool process_changed = false;
    std::optional<DistributedSieveWaveStoreFaultPoint> last_durable_fault_point;
};

[[nodiscard]] bool bridge_manifest_hook(durable_record::RecordFaultPoint point,
                                        void* raw_context) noexcept {
    auto& context = *static_cast<ManifestHookBridge*>(raw_context);
    if (!process_matches(context.creator_process_id)) {
        context.process_changed = true;
        return true;
    }
    DistributedSieveWaveStoreFaultPoint mapped = DistributedSieveWaveStoreFaultPoint::Count;
    switch (point) {
    case durable_record::RecordFaultPoint::PendingDurable:
        mapped = DistributedSieveWaveStoreFaultPoint::ManifestPendingDurable;
        break;
    case durable_record::RecordFaultPoint::CanonicalPromoted:
        mapped = DistributedSieveWaveStoreFaultPoint::ManifestCanonicalPromoted;
        break;
    case durable_record::RecordFaultPoint::CanonicalDurable:
        mapped = DistributedSieveWaveStoreFaultPoint::ManifestCanonicalDurable;
        break;
    }
    if (mapped == DistributedSieveWaveStoreFaultPoint::Count) {
        return true;
    }
    context.last_durable_fault_point = mapped;
    const bool interrupted = context.hooks.stop_after != nullptr &&
                             context.hooks.stop_after(mapped, context.hooks.context);
    if (!process_matches(context.creator_process_id)) {
        context.process_changed = true;
        return true;
    }
    return interrupted;
}

struct ManifestPublishResult final {
    std::optional<durable_record::RecordSnapshot> snapshot;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return snapshot.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] ManifestPublishResult publish_manifest(int root_fd, std::span<const std::byte> bytes,
                                                     DistributedSieveWaveStoreTestHooks hooks,
                                                     std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, process_mismatch()};
    }
    ManifestHookBridge bridge{
        .hooks = hooks,
        .creator_process_id = creator_process_id,
        .process_changed = false,
        .last_durable_fault_point = std::nullopt,
    };
    const auto published =
        durable_record::publish_at(static_cast<durable_record::NativeHandle>(root_fd),
                                   MANIFEST_PENDING_LEAF, MANIFEST_LEAF, bytes,
                                   durable_record::RecordTestHooks{
                                       .stop_after = bridge_manifest_hook,
                                       .context = &bridge,
                                   });

    DistributedSieveWaveStoreDiagnostic outcome;
    outcome.publication_status = published.status();
    outcome.native_error = published.native_error();
    outcome.last_durable_fault_point = bridge.last_durable_fault_point;
    if (bridge.process_changed || !process_matches(creator_process_id)) {
        outcome.status = DistributedSieveWaveStoreStatus::invalid_request;
        outcome.native_error = invalid_argument_error();
        return {std::nullopt, std::move(outcome)};
    }

    switch (published.status()) {
    case durable_record::RecordPublishStatus::durable:
        if (!published.canonical_snapshot().has_value()) {
            outcome.status = DistributedSieveWaveStoreStatus::publication_failed;
            outcome.native_error = protocol_error();
            return {std::nullopt, std::move(outcome)};
        }
        outcome.status = DistributedSieveWaveStoreStatus::ready;
        return {*published.canonical_snapshot(), std::move(outcome)};
    case durable_record::RecordPublishStatus::interrupted:
        outcome.status = DistributedSieveWaveStoreStatus::interrupted;
        break;
    case durable_record::RecordPublishStatus::invalid_request:
    case durable_record::RecordPublishStatus::input_too_large:
        outcome.status = DistributedSieveWaveStoreStatus::invalid_request;
        break;
    case durable_record::RecordPublishStatus::platform_unsupported:
        outcome.status = DistributedSieveWaveStoreStatus::platform_unsupported;
        break;
    case durable_record::RecordPublishStatus::pending_conflict:
    case durable_record::RecordPublishStatus::canonical_conflict:
        outcome.status = DistributedSieveWaveStoreStatus::manifest_conflict;
        break;
    case durable_record::RecordPublishStatus::parent_sync_failed:
        outcome.status = DistributedSieveWaveStoreStatus::durability_failed;
        break;
    case durable_record::RecordPublishStatus::pending_publish_failed:
    case durable_record::RecordPublishStatus::promotion_failed:
    case durable_record::RecordPublishStatus::canonical_confirm_failed:
    case durable_record::RecordPublishStatus::pending_cleanup_failed:
    case durable_record::RecordPublishStatus::ops_contract_violation:
        outcome.status = DistributedSieveWaveStoreStatus::publication_failed;
        break;
    case durable_record::RecordPublishStatus::unexpected_failure:
        outcome.status =
            published.native_error() == std::make_error_code(std::errc::not_enough_memory)
                ? DistributedSieveWaveStoreStatus::resource_exhausted
                : DistributedSieveWaveStoreStatus::unexpected_failure;
        break;
    }
    return {std::nullopt, std::move(outcome)};
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
invoke_store_hook(DistributedSieveWaveStoreTestHooks hooks,
                  DistributedSieveWaveStoreFaultPoint point,
                  std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    const bool interrupted = hooks.stop_after != nullptr && hooks.stop_after(point, hooks.context);
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    if (!interrupted) {
        return {};
    }
    auto outcome = diagnostic(DistributedSieveWaveStoreStatus::interrupted);
    outcome.last_durable_fault_point = point;
    return outcome;
}

struct FinalManifestResult final {
    std::optional<std::vector<std::byte>> bytes;
    std::optional<WaveManifestV1> manifest;
    std::optional<durable_record::RecordSnapshot> snapshot;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return bytes.has_value() && manifest.has_value() && snapshot.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] FinalManifestResult
confirm_final_manifest(int root_fd, std::span<const std::byte> expected_bytes,
                       const util::Sha256Digest& expected_manifest_digest,
                       const NativeIdentityV1& root_identity, const NativeIdentityV1& lock_identity,
                       const durable_record::RecordSnapshot& published_snapshot,
                       std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, std::nullopt, std::nullopt, process_mismatch()};
    }
    auto canonical = read_manifest_leaf(root_fd, MANIFEST_LEAF, creator_process_id);
    if (!canonical) {
        return {std::nullopt, std::nullopt, std::nullopt, canonical.diagnostic};
    }
    if (!std::equal(canonical.bytes->begin(), canonical.bytes->end(), expected_bytes.begin(),
                    expected_bytes.end()) ||
        *canonical.snapshot != published_snapshot) {
        return {std::nullopt, std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::manifest_conflict, protocol_error())};
    }
    auto decoded =
        decode_manifest(*canonical.bytes, expected_manifest_digest, root_identity, lock_identity);
    if (!decoded) {
        return {std::nullopt, std::nullopt, std::nullopt, decoded.diagnostic};
    }
    const auto inventory = inspect_namespace(root_fd);
    if (!inventory) {
        return {std::nullopt, std::nullopt, std::nullopt, inventory.diagnostic};
    }
    if (!inventory.inventory->lock || !inventory.inventory->manifest ||
        inventory.inventory->pending) {
        return {std::nullopt, std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, std::nullopt, std::nullopt, process_mismatch()};
    }
    return {
        std::move(canonical.bytes), std::move(decoded.manifest), std::move(canonical.snapshot), {}};
}

[[nodiscard]] bool valid_private_lease_base_lock_leaf(std::string_view leaf) noexcept {
    return !leaf.empty() && leaf != "." && leaf != ".." &&
           leaf.find('/') == std::string_view::npos && leaf.find('\0') == std::string_view::npos &&
           leaf.ends_with(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX);
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
private_lease_base_lock_open_failure(int error) noexcept {
    switch (error) {
    case ENOMEM:
    case EMFILE:
    case ENFILE:
    case ENOSPC:
#if defined(EDQUOT)
    case EDQUOT:
#endif
        return diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted, posix_error(error));
    case EACCES:
    case EEXIST:
    case EISDIR:
    case ELOOP:
    case ENOENT:
    case ENOTDIR:
    case EPERM:
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(error));
    default:
        return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(error));
    }
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
lock_private_lease_base_lock(int descriptor) noexcept {
    int result = -1;
    do {
        result = ::flock(descriptor, LOCK_EX | LOCK_NB);
    } while (result != 0 && errno == EINTR);
    if (result == 0) {
        return {};
    }
    const int saved_errno = errno;
    if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
        return diagnostic(DistributedSieveWaveStoreStatus::private_lease_lock_busy,
                          posix_error(saved_errno));
    }
    if (saved_errno == ENOTSUP || saved_errno == EOPNOTSUPP || saved_errno == ENOSYS) {
        return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                          posix_error(saved_errno));
    }
    return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(saved_errno));
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
invoke_private_lease_base_lock_hook(DistributedSievePrivateLeaseBaseLockTestHooks::Boundary hook,
                                    void* context, std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    if (hook != nullptr) {
        hook(context);
    }
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    return {};
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
invoke_private_lease_base_lock_sync_hook(DistributedSievePrivateLeaseBaseLockTestHooks hooks,
                                         DistributedSievePrivateLeaseBaseLockSyncPoint point,
                                         std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    const bool fail =
        hooks.fail_before_sync != nullptr && hooks.fail_before_sync(point, hooks.context);
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    if (!fail) {
        return {};
    }
    auto outcome = diagnostic(DistributedSieveWaveStoreStatus::durability_failed,
                              std::make_error_code(std::errc::io_error));
    outcome.failed_private_lease_base_lock_sync_point = point;
    return outcome;
}

#endif

} // namespace

std::optional<DistributedSieveWorkerAttemptNamesV1>
distributed_sieve_worker_attempt_names_v1(std::string_view chunk_relative_artifact_stem,
                                          std::uint32_t chunk_id, std::uint32_t attempt_ordinal) {
    if (chunk_id >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS ||
        attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        chunk_relative_artifact_stem.size() >
            DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES -
                DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() -
                DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1) {
        return std::nullopt;
    }

    DistributedSieveWorkerAttemptNamesV1 names;
    names.relative_lease_stem.reserve(chunk_relative_artifact_stem.size() +
                                      DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() +
                                      DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1);
    names.relative_lease_stem.append(chunk_relative_artifact_stem);
    names.relative_lease_stem.append(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1);
    names.relative_lease_stem.push_back(decimal_digit(attempt_ordinal / 10U));
    names.relative_lease_stem.push_back(decimal_digit(attempt_ordinal % 10U));
    if (!distributed_sieve_worker_attempt_relative_stem_matches(
            chunk_relative_artifact_stem, attempt_ordinal, names.relative_lease_stem)) {
        return std::nullopt;
    }

    names.private_directory_leaf = names.relative_lease_stem;
    names.private_directory_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_DIRECTORY_SUFFIX);
    names.base_lock_leaf = names.relative_lease_stem;
    names.base_lock_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX);

    names.canonical_record_leaf.reserve(
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1 +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1);
    names.canonical_record_leaf.append(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX);
    names.canonical_record_leaf.push_back(decimal_digit(chunk_id / 10U));
    names.canonical_record_leaf.push_back(decimal_digit(chunk_id % 10U));
    names.canonical_record_leaf.append(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR);
    names.canonical_record_leaf.push_back(decimal_digit(attempt_ordinal / 10U));
    names.canonical_record_leaf.push_back(decimal_digit(attempt_ordinal % 10U));
    names.pending_record_leaf = names.canonical_record_leaf;
    names.pending_record_leaf.append(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PENDING_SUFFIX);
    return names;
}

std::optional<DistributedSieveParsedWorkerAttemptLeafV1>
parse_distributed_sieve_worker_attempt_leaf_v1(std::string_view leaf) noexcept {
    bool pending = false;
    if (leaf.ends_with(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PENDING_SUFFIX)) {
        pending = true;
        leaf.remove_suffix(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PENDING_SUFFIX.size());
    }
    const std::size_t expected_size =
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1 +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
    if (leaf.size() != expected_size ||
        !leaf.starts_with(DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX)) {
        return std::nullopt;
    }
    std::size_t cursor = DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX.size();
    const auto parse_two_digits = [&](std::uint32_t& output) noexcept {
        if (leaf[cursor] < '0' || leaf[cursor] > '9' || leaf[cursor + 1U] < '0' ||
            leaf[cursor + 1U] > '9') {
            return false;
        }
        output = static_cast<std::uint32_t>(leaf[cursor] - '0') * 10U +
                 static_cast<std::uint32_t>(leaf[cursor + 1U] - '0');
        cursor += DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
        return true;
    };

    DistributedSieveParsedWorkerAttemptLeafV1 parsed{.pending = pending};
    if (!parse_two_digits(parsed.chunk_id) ||
        leaf.substr(cursor, DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR.size()) !=
            DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR) {
        return std::nullopt;
    }
    cursor += DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR.size();
    if (!parse_two_digits(parsed.attempt_ordinal) || cursor != leaf.size() ||
        parsed.chunk_id >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS ||
        parsed.attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS) {
        return std::nullopt;
    }
    return parsed;
}

struct DistributedSieveWaveStore::State final {
    std::filesystem::path absolute_root;
    std::filesystem::path root_leaf;
    std::vector<std::string> parent_components;
    WaveManifestV1 manifest;
    util::durable_immutable_record::RecordSnapshot manifest_snapshot;
    NativeIdentityV1 root_identity;
    NativeIdentityV1 lock_identity;
    std::vector<std::byte> manifest_bytes;
    std::uint64_t creator_process_id = 0;
    mutable std::atomic_flag private_lease_root_action_claimed = ATOMIC_FLAG_INIT;
#if !defined(_WIN32)
    int parent_fd = -1;
    int root_fd = -1;
    int lock_fd = -1;
#endif

    ~State();
};

DistributedSieveWaveStore::State::~State() {
#if !defined(_WIN32)
    if (lock_fd >= 0) {
        (void)::close(lock_fd);
    }
    if (root_fd >= 0) {
        (void)::close(root_fd);
    }
    if (parent_fd >= 0) {
        (void)::close(parent_fd);
    }
#endif
}

DistributedSievePrivateLeaseBaseLockAt::DistributedSievePrivateLeaseBaseLockAt(
    int root_fd, std::string leaf, std::uint64_t creator_process_id) noexcept
    : root_fd_(root_fd), leaf_(std::move(leaf)), creator_process_id_(creator_process_id) {}

DistributedSievePrivateLeaseBaseLockAt::~DistributedSievePrivateLeaseBaseLockAt() noexcept {
#if !defined(_WIN32)
    if (lock_fd_ >= 0) {
        // Never issue LOCK_UN: after fork the parent and child descriptors
        // refer to the same open-file description. Close-only preserves the
        // parent's flock until its final descriptor is gone.
        (void)::close(lock_fd_);
        lock_fd_ = -1;
    }
#endif
}

std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>
DistributedSievePrivateLeaseBaseLockAt::create_new_locked(
    int root_fd, std::string leaf, std::uint64_t creator_process_id,
    DistributedSievePrivateLeaseBaseLockTestHooks hooks,
    DistributedSieveWaveStoreDiagnostic& outcome) noexcept {
    outcome = {};
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    (void)root_fd;
    (void)leaf;
    (void)creator_process_id;
    (void)hooks;
    outcome =
        diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
    return nullptr;
#else
    if (root_fd < 0 || !process_matches(creator_process_id) ||
        !valid_private_lease_base_lock_leaf(leaf)) {
        outcome = process_matches(creator_process_id)
                      ? diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                   invalid_argument_error())
                      : process_mismatch();
        return nullptr;
    }

    std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> lock;
    try {
        lock.reset(new DistributedSievePrivateLeaseBaseLockAt(root_fd, std::move(leaf),
                                                              creator_process_id));
    } catch (const std::bad_alloc&) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                             std::make_error_code(std::errc::not_enough_memory));
        return nullptr;
    } catch (...) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                             std::make_error_code(std::errc::io_error));
        return nullptr;
    }

    struct stat existing{};
    if (fstatat_retrying_eintr(root_fd, lock->leaf_.c_str(), existing) == 0) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
        return nullptr;
    }
    const int absence_errno = errno;
    if (absence_errno != ENOENT) {
        outcome = private_lease_base_lock_open_failure(absence_errno);
        return nullptr;
    }
    if (!process_matches(creator_process_id)) {
        outcome = process_mismatch();
        return nullptr;
    }

    const int descriptor = openat_retrying_eintr(
        root_fd, lock->leaf_.c_str(),
        O_RDWR | O_NONBLOCK | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        outcome = private_lease_base_lock_open_failure(errno);
        return nullptr;
    }
    UniqueFd held(descriptor);

    int chmod_result = -1;
    do {
        chmod_result = ::fchmod(held.get(), 0600);
    } while (chmod_result != 0 && errno == EINTR);
    if (chmod_result != 0) {
        outcome =
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
        return nullptr;
    }

    const auto initially_bound = validate_private_lease_base_lock_binding(
        root_fd, held.get(), lock->leaf_, creator_process_id);
    if (!initially_bound) {
        outcome = initially_bound.diagnostic;
        return nullptr;
    }
    if (const auto locked = lock_private_lease_base_lock(held.get());
        locked.status != DistributedSieveWaveStoreStatus::ready) {
        outcome = locked;
        return nullptr;
    }
    if (const auto hooked = invoke_private_lease_base_lock_hook(hooks.after_target_lock_acquired,
                                                                hooks.context, creator_process_id);
        hooked.status != DistributedSieveWaveStoreStatus::ready) {
        outcome = hooked;
        return nullptr;
    }
    const auto finally_bound = validate_private_lease_base_lock_binding(
        root_fd, held.get(), lock->leaf_, creator_process_id, *initially_bound.identity);
    if (!finally_bound) {
        outcome = finally_bound.diagnostic;
        return nullptr;
    }

    lock->identity_ = *finally_bound.identity;
    lock->lock_fd_ = held.release();
    return lock;
#endif
}

std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>
DistributedSievePrivateLeaseBaseLockAt::open_existing_locked(
    int root_fd, std::string leaf, const NativeIdentityV1& expected_identity,
    std::uint64_t creator_process_id, DistributedSievePrivateLeaseBaseLockTestHooks hooks,
    DistributedSieveWaveStoreDiagnostic& outcome) noexcept {
    outcome = {};
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    (void)root_fd;
    (void)leaf;
    (void)expected_identity;
    (void)creator_process_id;
    (void)hooks;
    outcome =
        diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
    return nullptr;
#else
    if (root_fd < 0 || !process_matches(creator_process_id) ||
        !valid_private_lease_base_lock_leaf(leaf)) {
        outcome = process_matches(creator_process_id)
                      ? diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                   invalid_argument_error())
                      : process_mismatch();
        return nullptr;
    }

    std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> lock;
    try {
        lock.reset(new DistributedSievePrivateLeaseBaseLockAt(root_fd, std::move(leaf),
                                                              creator_process_id));
    } catch (const std::bad_alloc&) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                             std::make_error_code(std::errc::not_enough_memory));
        return nullptr;
    } catch (...) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                             std::make_error_code(std::errc::io_error));
        return nullptr;
    }

    if (!process_matches(creator_process_id)) {
        outcome = process_mismatch();
        return nullptr;
    }
    const int descriptor = openat_retrying_eintr(root_fd, lock->leaf_.c_str(),
                                                 O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        outcome = private_lease_base_lock_open_failure(errno);
        return nullptr;
    }
    UniqueFd held(descriptor);

    const auto initially_bound = validate_private_lease_base_lock_binding(
        root_fd, held.get(), lock->leaf_, creator_process_id, expected_identity);
    if (!initially_bound) {
        outcome = initially_bound.diagnostic;
        return nullptr;
    }
    if (const auto locked = lock_private_lease_base_lock(held.get());
        locked.status != DistributedSieveWaveStoreStatus::ready) {
        outcome = locked;
        return nullptr;
    }
    if (const auto hooked = invoke_private_lease_base_lock_hook(hooks.after_target_lock_acquired,
                                                                hooks.context, creator_process_id);
        hooked.status != DistributedSieveWaveStoreStatus::ready) {
        outcome = hooked;
        return nullptr;
    }
    const auto finally_bound = validate_private_lease_base_lock_binding(
        root_fd, held.get(), lock->leaf_, creator_process_id, expected_identity);
    if (!finally_bound) {
        outcome = finally_bound.diagnostic;
        return nullptr;
    }

    lock->identity_ = *finally_bound.identity;
    lock->lock_fd_ = held.release();
    return lock;
#endif
}

DistributedSieveWaveStoreDiagnostic
DistributedSievePrivateLeaseBaseLockAt::revalidate() const noexcept {
    if (!owned_by_current_process()) {
        return invalidated_.load(std::memory_order_acquire)
                   ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                protocol_error())
                   : process_mismatch();
    }
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
#else
    const auto validated = validate_private_lease_base_lock_binding(root_fd_, lock_fd_, leaf_,
                                                                    creator_process_id_, identity_);
    if (!validated) {
        invalidated_.store(true, std::memory_order_release);
        return validated.diagnostic;
    }
    return {};
#endif
}

DistributedSieveWaveStoreDiagnostic DistributedSievePrivateLeaseBaseLockAt::synchronize(
    DistributedSievePrivateLeaseBaseLockTestHooks hooks) const noexcept {
    if (const auto before = revalidate(); before.status != DistributedSieveWaveStoreStatus::ready) {
        return before;
    }
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
#else
    std::error_code sync_error;
    if (const auto injected = invoke_private_lease_base_lock_sync_hook(
            hooks, DistributedSievePrivateLeaseBaseLockSyncPoint::TargetInitial,
            creator_process_id_);
        injected.status != DistributedSieveWaveStoreStatus::ready) {
        return injected;
    }
    if (!sync_descriptor(lock_fd_, sync_error)) {
        return diagnostic(DistributedSieveWaveStoreStatus::durability_failed, sync_error);
    }
    if (const auto after_lock_sync = revalidate();
        after_lock_sync.status != DistributedSieveWaveStoreStatus::ready) {
        return after_lock_sync;
    }
    if (const auto injected = invoke_private_lease_base_lock_sync_hook(
            hooks, DistributedSievePrivateLeaseBaseLockSyncPoint::RootDirectory,
            creator_process_id_);
        injected.status != DistributedSieveWaveStoreStatus::ready) {
        return injected;
    }
    if (!sync_descriptor(root_fd_, sync_error)) {
        return diagnostic(DistributedSieveWaveStoreStatus::durability_failed, sync_error);
    }
    if (const auto after_root_sync = revalidate();
        after_root_sync.status != DistributedSieveWaveStoreStatus::ready) {
        return after_root_sync;
    }
    if (const auto injected = invoke_private_lease_base_lock_sync_hook(
            hooks, DistributedSievePrivateLeaseBaseLockSyncPoint::TargetFinal, creator_process_id_);
        injected.status != DistributedSieveWaveStoreStatus::ready) {
        return injected;
    }
    if (!sync_descriptor(lock_fd_, sync_error)) {
        return diagnostic(DistributedSieveWaveStoreStatus::durability_failed, sync_error);
    }
    return revalidate();
#endif
}

bool DistributedSievePrivateLeaseBaseLockAt::owned_by_current_process() const noexcept {
    return root_fd_ >= 0 && lock_fd_ >= 0 && creator_process_id_ != 0 && !leaf_.empty() &&
           !invalidated_.load(std::memory_order_acquire) &&
           creator_process_id_ == static_cast<std::uint64_t>(gnfs::util::process_id());
}

const NativeIdentityV1& DistributedSievePrivateLeaseBaseLockAt::identity() const noexcept {
    return identity_;
}

void DistributedSievePrivateLeaseBaseLockAt::invalidate() const noexcept {
    invalidated_.store(true, std::memory_order_release);
}

DistributedSieveExternalCleanupAuthorizationState::
    DistributedSieveExternalCleanupAuthorizationState(
        std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state) noexcept
    : wave_store_state_(std::move(wave_store_state)),
      creator_process_id_(wave_store_state_ != nullptr ? wave_store_state_->creator_process_id
                                                       : 0) {}

bool distributed_sieve_external_cleanup_authorization_state_owned_by_current_process(
    const DistributedSieveExternalCleanupAuthorizationState& state) noexcept {
    return state.owned_by_current_process();
}

DistributedSieveWaveStore::DistributedSieveWaveStore(std::shared_ptr<const State> state) noexcept
    : state_(std::move(state)) {}

DistributedSieveWaveStore::~DistributedSieveWaveStore() = default;

DistributedSievePrivateLeaseRootClaim::DistributedSievePrivateLeaseRootClaim(
    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state) noexcept
    : wave_store_state_(std::move(wave_store_state)),
      creator_process_id_(wave_store_state_ != nullptr ? wave_store_state_->creator_process_id
                                                       : 0) {}

DistributedSievePrivateLeaseRootClaim::~DistributedSievePrivateLeaseRootClaim() noexcept {
    base_lock_at_.reset();
    if (wave_store_state_ != nullptr && creator_process_id_ != 0 &&
        creator_process_id_ == static_cast<std::uint64_t>(gnfs::util::process_id())) {
        wave_store_state_->private_lease_root_action_claimed.clear(std::memory_order_release);
    }
}

bool DistributedSievePrivateLeaseRootClaim::owned_by_current_process() const noexcept {
    return wave_store_state_ != nullptr && creator_process_id_ != 0 &&
           creator_process_id_ == static_cast<std::uint64_t>(gnfs::util::process_id()) &&
           wave_store_state_->private_lease_root_action_claimed.test(std::memory_order_acquire);
}

DistributedSieveWaveStoreDiagnostic DistributedSievePrivateLeaseRootClaim::revalidate(
    DistributedSievePrivateLeaseRootClaimTestHooks hooks) const noexcept {
    if (!owned_by_current_process()) {
        return process_mismatch();
    }
    DistributedSieveWaveStore view(wave_store_state_);
    if (base_lock_at_ == nullptr) {
        auto validated = view.revalidate();
        if (validated.status != DistributedSieveWaveStoreStatus::ready) {
            return validated;
        }
        return owned_by_current_process() ? DistributedSieveWaveStoreDiagnostic{}
                                          : process_mismatch();
    }

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    (void)hooks;
    base_lock_at_->invalidate();
    return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
#else
    const auto invalidate_with = [&](DistributedSieveWaveStoreDiagnostic outcome) noexcept
        -> DistributedSieveWaveStoreDiagnostic {
        base_lock_at_->invalidate();
        return outcome;
    };
    if (!worker_attempt_names_.has_value() ||
        !expected_private_lease_base_lock_leaves_.has_value() ||
        !expected_private_lease_base_lock_identities_.has_value() ||
        expected_private_lease_base_lock_leaves_->size() !=
            expected_private_lease_base_lock_identities_->size()) {
        return invalidate_with(
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error()));
    }
    const auto target_position = std::lower_bound(expected_private_lease_base_lock_leaves_->begin(),
                                                  expected_private_lease_base_lock_leaves_->end(),
                                                  worker_attempt_names_->base_lock_leaf);
    const auto target_index = static_cast<std::size_t>(
        std::distance(expected_private_lease_base_lock_leaves_->begin(), target_position));
    if (target_position == expected_private_lease_base_lock_leaves_->end() ||
        *target_position != worker_attempt_names_->base_lock_leaf ||
        expected_private_lease_base_lock_identities_->at(target_index) !=
            base_lock_at_->identity()) {
        return invalidate_with(
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error()));
    }

    bool claim_boundary_offered = false;
    const auto revalidate_higher_priority_bindings =
        [&]() noexcept -> DistributedSieveWaveStoreDiagnostic {
        if (const auto authority = view.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        if (!claim_boundary_offered) {
            claim_boundary_offered = true;
            if (const auto hooked = invoke_private_lease_base_lock_hook(
                    hooks.after_first_authority_validation, hooks.context, creator_process_id_);
                hooked.status != DistributedSieveWaveStoreStatus::ready) {
                return hooked;
            }
        }
        if (const auto target = base_lock_at_->revalidate();
            target.status != DistributedSieveWaveStoreStatus::ready) {
            if (const auto authority = view.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return authority;
            }
            return target;
        }
        return view.revalidate_authority();
    };
    const auto reject_lower_priority = [&](DistributedSieveWaveStoreDiagnostic outcome) noexcept
        -> DistributedSieveWaveStoreDiagnostic {
        if (const auto bindings = revalidate_higher_priority_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return invalidate_with(bindings);
        }
        return invalidate_with(std::move(outcome));
    };

    if (const auto bindings = revalidate_higher_priority_bindings();
        bindings.status != DistributedSieveWaveStoreStatus::ready) {
        return invalidate_with(bindings);
    }
    auto first = capture_manifest_bound_inventory_witness(
        wave_store_state_->root_fd, wave_store_state_->manifest, creator_process_id_);
    if (!first) {
        return reject_lower_priority(std::move(first.diagnostic));
    }
    if (first.inventory->private_lease_base_lock_leaves !=
            *expected_private_lease_base_lock_leaves_ ||
        *first.base_lock_identities != *expected_private_lease_base_lock_identities_) {
        return reject_lower_priority(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (const auto bindings = revalidate_higher_priority_bindings();
        bindings.status != DistributedSieveWaveStoreStatus::ready) {
        return invalidate_with(bindings);
    }

    auto confirmed = capture_manifest_bound_inventory_witness(
        wave_store_state_->root_fd, wave_store_state_->manifest, creator_process_id_);
    if (!confirmed) {
        return reject_lower_priority(std::move(confirmed.diagnostic));
    }
    if (*confirmed.inventory != *first.inventory ||
        *confirmed.base_lock_identities != *first.base_lock_identities ||
        confirmed.inventory->private_lease_base_lock_leaves !=
            *expected_private_lease_base_lock_leaves_ ||
        *confirmed.base_lock_identities != *expected_private_lease_base_lock_identities_) {
        return reject_lower_priority(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (const auto bindings = revalidate_higher_priority_bindings();
        bindings.status != DistributedSieveWaveStoreStatus::ready) {
        return invalidate_with(bindings);
    }
    if (!owned_by_current_process()) {
        return invalidate_with(process_mismatch());
    }
    return {};
#endif
}

DistributedSieveWaveStoreDiagnostic
DistributedSievePrivateLeaseRootClaim::revalidate_authority() const noexcept {
    if (!owned_by_current_process()) {
        return process_mismatch();
    }
    DistributedSieveWaveStore view(wave_store_state_);
    auto validated = view.revalidate_authority();
    if (validated.status != DistributedSieveWaveStoreStatus::ready) {
        if (base_lock_at_ != nullptr) {
            base_lock_at_->invalidate();
        }
        return validated;
    }
    if (base_lock_at_ != nullptr) {
        if (const auto target = base_lock_at_->revalidate();
            target.status != DistributedSieveWaveStoreStatus::ready) {
            if (const auto authority = view.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                base_lock_at_->invalidate();
                return authority;
            }
            return target;
        }
    }
    validated = view.revalidate_authority();
    if (validated.status != DistributedSieveWaveStoreStatus::ready) {
        if (base_lock_at_ != nullptr) {
            base_lock_at_->invalidate();
        }
        return validated;
    }
    if (!owned_by_current_process()) {
        if (base_lock_at_ != nullptr) {
            base_lock_at_->invalidate();
        }
        return process_mismatch();
    }
    return {};
}

DistributedSieveWaveStoreOpenResult
DistributedSieveWaveStore::create(const std::filesystem::path& absolute_root,
                                  WaveManifestV1 manifest_draft,
                                  DistributedSieveWaveStoreTestHooks hooks) noexcept {
    try {
        auto frozen = freeze_absolute_root(absolute_root);
        if (!frozen.has_value() || !valid_manifest_draft(manifest_draft)) {
            return open_failure(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                           invalid_argument_error()));
        }
        const auto preflight = validate_manifest_draft_semantics(manifest_draft);
        if (!preflight) {
            auto failure =
                diagnostic(DistributedSieveWaveStoreStatus::invalid_request, protocol_error());
            failure.protocol_status = preflight;
            return open_failure(std::move(failure));
        }
        const std::uint64_t creator_process_id = current_process_id();
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)hooks;
        return open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
        auto root = open_root(*frozen, true, creator_process_id);
        if (!root) {
            return open_failure(std::move(root.diagnostic));
        }
        auto inventory = inspect_namespace(root.root.get());
        if (!inventory) {
            return open_failure(std::move(inventory.diagnostic));
        }
        const NamespaceInventory root_inventory = *inventory.inventory;
        const auto root_hook = invoke_store_hook(
            hooks, DistributedSieveWaveStoreFaultPoint::RootDurable, creator_process_id);
        if (root_hook.status == DistributedSieveWaveStoreStatus::invalid_request) {
            return open_failure(root_hook);
        }
        if (const auto root_validated =
                validate_root_binding(root.parent.get(), frozen->parent_components, root.root.get(),
                                      frozen->leaf, creator_process_id, root.root_identity);
            root_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(root_validated);
        }
        const auto after_root_hook = inspect_namespace(root.root.get());
        if (!after_root_hook) {
            return open_failure(after_root_hook.diagnostic);
        }
        if (*after_root_hook.inventory != root_inventory) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (root_hook.status == DistributedSieveWaveStoreStatus::interrupted) {
            return open_failure(root_hook);
        }

        auto lock = open_lock(root.root.get(), root_inventory, true, creator_process_id);
        if (!lock) {
            return open_failure(std::move(lock.diagnostic));
        }
        if (const auto root_validated =
                validate_root_binding(root.parent.get(), frozen->parent_components, root.root.get(),
                                      frozen->leaf, creator_process_id, root.root_identity);
            root_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(root_validated);
        }
        if (const auto lock_validated = validate_lock_binding(
                root.root.get(), lock.lock.get(), creator_process_id, lock.lock_identity);
            lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(lock_validated);
        }
        inventory = inspect_namespace(root.root.get());
        if (!inventory) {
            return open_failure(std::move(inventory.diagnostic));
        }
        if (!inventory.inventory->lock) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        const NamespaceInventory lock_inventory = *inventory.inventory;
        const auto lock_hook = invoke_store_hook(
            hooks, DistributedSieveWaveStoreFaultPoint::LockDurable, creator_process_id);
        if (lock_hook.status == DistributedSieveWaveStoreStatus::invalid_request) {
            return open_failure(lock_hook);
        }
        if (const auto root_validated =
                validate_root_binding(root.parent.get(), frozen->parent_components, root.root.get(),
                                      frozen->leaf, creator_process_id, root.root_identity);
            root_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(root_validated);
        }
        if (const auto lock_validated = validate_lock_binding(
                root.root.get(), lock.lock.get(), creator_process_id, lock.lock_identity);
            lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(lock_validated);
        }
        const auto after_lock_hook = inspect_namespace(root.root.get());
        if (!after_lock_hook) {
            return open_failure(after_lock_hook.diagnostic);
        }
        if (*after_lock_hook.inventory != lock_inventory) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (lock_hook.status == DistributedSieveWaveStoreStatus::interrupted) {
            return open_failure(lock_hook);
        }

        manifest_draft.wave_root_identity = root.root_identity;
        manifest_draft.permanent_lock_identity = lock.lock_identity;
        manifest_draft.lock_semantics_version = DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1;
        DistributedSieveProtocolRecordV1 record(std::move(manifest_draft));
        const auto sealed = seal_distributed_sieve_record(record);
        if (!sealed) {
            auto failure =
                diagnostic(DistributedSieveWaveStoreStatus::invalid_request, protocol_error());
            failure.protocol_status = sealed;
            return open_failure(std::move(failure));
        }
        const auto& sealed_manifest = std::get<WaveManifestV1>(record);
        const util::Sha256Digest expected_manifest_digest = sealed_manifest.self_digest;
        auto encoded = encode_distributed_sieve_record(record);
        if (!encoded) {
            auto failure =
                diagnostic(DistributedSieveWaveStoreStatus::invalid_request, protocol_error());
            failure.protocol_status = encoded.status;
            return open_failure(std::move(failure));
        }
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }
        if (const auto root_validated =
                validate_root_binding(root.parent.get(), frozen->parent_components, root.root.get(),
                                      frozen->leaf, creator_process_id, root.root_identity);
            root_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(root_validated);
        }
        if (const auto lock_validated = validate_lock_binding(
                root.root.get(), lock.lock.get(), creator_process_id, lock.lock_identity);
            lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(lock_validated);
        }
        const auto before_publish = inspect_namespace(root.root.get());
        if (!before_publish) {
            return open_failure(before_publish.diagnostic);
        }
        if (*before_publish.inventory != lock_inventory) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        PrivateLeaseBaseLockInventoryValidationResult before_publish_private_inventory;
        if (!before_publish.inventory->private_lease_base_lock_leaves.empty()) {
            auto existing = read_existing_manifest(root.root.get(), *before_publish.inventory,
                                                   expected_manifest_digest, root.root_identity,
                                                   lock.lock_identity, creator_process_id);
            if (!existing) {
                return open_failure(std::move(existing.diagnostic));
            }
            before_publish_private_inventory = validate_private_lease_base_lock_inventory(
                root.root.get(), *before_publish.inventory, *existing.manifest, creator_process_id);
        } else {
            before_publish_private_inventory = validate_private_lease_base_lock_inventory(
                root.root.get(), *before_publish.inventory, sealed_manifest, creator_process_id);
        }
        if (!before_publish_private_inventory) {
            return open_failure(before_publish_private_inventory.diagnostic);
        }

        auto published =
            publish_manifest(root.root.get(), *encoded.bytes, hooks, creator_process_id);
        if (!published) {
            return open_failure(std::move(published.diagnostic));
        }
        if (const auto root_validated =
                validate_root_binding(root.parent.get(), frozen->parent_components, root.root.get(),
                                      frozen->leaf, creator_process_id, root.root_identity);
            root_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(root_validated);
        }
        if (const auto lock_validated = validate_lock_binding(
                root.root.get(), lock.lock.get(), creator_process_id, lock.lock_identity);
            lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(lock_validated);
        }

        auto final_manifest = confirm_final_manifest(
            root.root.get(), *encoded.bytes, expected_manifest_digest, root.root_identity,
            lock.lock_identity, *published.snapshot, creator_process_id);
        if (!final_manifest) {
            return open_failure(std::move(final_manifest.diagnostic));
        }
        if (const auto root_validated =
                validate_root_binding(root.parent.get(), frozen->parent_components, root.root.get(),
                                      frozen->leaf, creator_process_id, root.root_identity);
            root_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(root_validated);
        }
        if (const auto lock_validated = validate_lock_binding(
                root.root.get(), lock.lock.get(), creator_process_id, lock.lock_identity);
            lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(lock_validated);
        }
        const auto final_inventory = inspect_namespace(root.root.get());
        if (!final_inventory) {
            return open_failure(final_inventory.diagnostic);
        }
        const auto final_inventory_validated = validate_manifest_bound_inventory(
            root.root.get(), *final_inventory.inventory, sealed_manifest, creator_process_id);
        if (!final_inventory_validated) {
            return open_failure(final_inventory_validated.diagnostic);
        }
        if (final_inventory.inventory->private_lease_base_lock_leaves !=
                before_publish.inventory->private_lease_base_lock_leaves ||
            *final_inventory_validated.identities != *before_publish_private_inventory.identities) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }

        auto state = std::make_shared<State>();
        state->absolute_root = std::move(frozen->absolute);
        state->root_leaf = std::move(frozen->leaf);
        state->parent_components = std::move(frozen->parent_components);
        state->manifest = std::move(*final_manifest.manifest);
        state->manifest_snapshot = *final_manifest.snapshot;
        state->root_identity = root.root_identity;
        state->lock_identity = lock.lock_identity;
        state->manifest_bytes = std::move(*final_manifest.bytes);
        state->creator_process_id = creator_process_id;
        state->parent_fd = root.parent.release();
        state->root_fd = root.root.release();
        state->lock_fd = lock.lock.release();

        auto store = std::unique_ptr<DistributedSieveWaveStore>(
            new DistributedSieveWaveStore(std::move(state)));
        if (!process_matches(creator_process_id)) {
            store.reset();
            return open_failure(process_mismatch());
        }
        return {std::move(store), std::move(published.diagnostic)};
#endif
    } catch (const std::bad_alloc&) {
        return open_failure(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                       std::make_error_code(std::errc::not_enough_memory)));
    } catch (const std::filesystem::filesystem_error& error) {
        return open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, error.code()));
    } catch (...) {
        return open_failure(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                       std::make_error_code(std::errc::io_error)));
    }
}

DistributedSieveWaveStoreOpenResult
DistributedSieveWaveStore::open(const std::filesystem::path& absolute_root,
                                const util::Sha256Digest& expected_manifest_digest,
                                DistributedSieveWaveStoreTestHooks hooks) noexcept {
    try {
        auto frozen = freeze_absolute_root(absolute_root);
        if (!frozen.has_value() || nil_digest(expected_manifest_digest)) {
            return open_failure(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                           invalid_argument_error()));
        }
        const std::uint64_t creator_process_id = current_process_id();
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)expected_manifest_digest;
        (void)hooks;
        return open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
        auto root = open_root(*frozen, false, creator_process_id);
        if (!root) {
            return open_failure(std::move(root.diagnostic));
        }
        auto inventory = inspect_namespace(root.root.get());
        if (!inventory) {
            return open_failure(std::move(inventory.diagnostic));
        }

        const NamespaceInventory initial_inventory = *inventory.inventory;
        auto lock = open_lock(root.root.get(), initial_inventory, false, creator_process_id);
        if (!lock) {
            return open_failure(std::move(lock.diagnostic));
        }
        if (const auto root_validated =
                validate_root_binding(root.parent.get(), frozen->parent_components, root.root.get(),
                                      frozen->leaf, creator_process_id, root.root_identity);
            root_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(root_validated);
        }
        if (const auto lock_validated = validate_lock_binding(
                root.root.get(), lock.lock.get(), creator_process_id, lock.lock_identity);
            lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(lock_validated);
        }

        inventory = inspect_namespace(root.root.get());
        if (!inventory) {
            return open_failure(std::move(inventory.diagnostic));
        }
        if (!inventory.inventory->lock) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (*inventory.inventory != initial_inventory) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        auto existing =
            read_existing_manifest(root.root.get(), *inventory.inventory, expected_manifest_digest,
                                   root.root_identity, lock.lock_identity, creator_process_id);
        if (!existing) {
            return open_failure(std::move(existing.diagnostic));
        }
        const auto inventory_validated = validate_private_lease_base_lock_inventory(
            root.root.get(), *inventory.inventory, *existing.manifest, creator_process_id);
        if (!inventory_validated) {
            return open_failure(inventory_validated.diagnostic);
        }

        const auto after_read = inspect_namespace(root.root.get());
        if (!after_read) {
            return open_failure(std::move(after_read.diagnostic));
        }
        if (*after_read.inventory != *inventory.inventory) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        const auto after_read_inventory_validated = validate_private_lease_base_lock_inventory(
            root.root.get(), *after_read.inventory, *existing.manifest, creator_process_id);
        if (!after_read_inventory_validated) {
            return open_failure(after_read_inventory_validated.diagnostic);
        }
        if (*after_read_inventory_validated.identities != *inventory_validated.identities) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (const auto root_validated =
                validate_root_binding(root.parent.get(), frozen->parent_components, root.root.get(),
                                      frozen->leaf, creator_process_id, root.root_identity);
            root_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(root_validated);
        }
        if (const auto lock_validated = validate_lock_binding(
                root.root.get(), lock.lock.get(), creator_process_id, lock.lock_identity);
            lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(lock_validated);
        }

        auto published =
            publish_manifest(root.root.get(), *existing.bytes, hooks, creator_process_id);
        if (!published) {
            return open_failure(std::move(published.diagnostic));
        }
        auto final_manifest = confirm_final_manifest(
            root.root.get(), *existing.bytes, expected_manifest_digest, root.root_identity,
            lock.lock_identity, *published.snapshot, creator_process_id);
        if (!final_manifest) {
            return open_failure(std::move(final_manifest.diagnostic));
        }
        if (const auto root_validated =
                validate_root_binding(root.parent.get(), frozen->parent_components, root.root.get(),
                                      frozen->leaf, creator_process_id, root.root_identity);
            root_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(root_validated);
        }
        if (const auto lock_validated = validate_lock_binding(
                root.root.get(), lock.lock.get(), creator_process_id, lock.lock_identity);
            lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return open_failure(lock_validated);
        }
        const auto final_inventory = inspect_namespace(root.root.get());
        if (!final_inventory) {
            return open_failure(final_inventory.diagnostic);
        }
        const auto final_inventory_validated =
            validate_manifest_bound_inventory(root.root.get(), *final_inventory.inventory,
                                              *final_manifest.manifest, creator_process_id);
        if (!final_inventory_validated) {
            return open_failure(final_inventory_validated.diagnostic);
        }
        if (final_inventory.inventory->private_lease_base_lock_leaves !=
                after_read.inventory->private_lease_base_lock_leaves ||
            *final_inventory_validated.identities != *after_read_inventory_validated.identities) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }

        auto state = std::make_shared<State>();
        state->absolute_root = std::move(frozen->absolute);
        state->root_leaf = std::move(frozen->leaf);
        state->parent_components = std::move(frozen->parent_components);
        state->manifest = std::move(*final_manifest.manifest);
        state->manifest_snapshot = *final_manifest.snapshot;
        state->root_identity = root.root_identity;
        state->lock_identity = lock.lock_identity;
        state->manifest_bytes = std::move(*final_manifest.bytes);
        state->creator_process_id = creator_process_id;
        state->parent_fd = root.parent.release();
        state->root_fd = root.root.release();
        state->lock_fd = lock.lock.release();

        auto store = std::unique_ptr<DistributedSieveWaveStore>(
            new DistributedSieveWaveStore(std::move(state)));
        if (!process_matches(creator_process_id)) {
            store.reset();
            return open_failure(process_mismatch());
        }
        return {std::move(store), std::move(published.diagnostic)};
#endif
    } catch (const std::bad_alloc&) {
        return open_failure(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                       std::make_error_code(std::errc::not_enough_memory)));
    } catch (const std::filesystem::filesystem_error& error) {
        return open_failure(
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, error.code()));
    } catch (...) {
        return open_failure(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                       std::make_error_code(std::errc::io_error)));
    }
}

const std::filesystem::path& DistributedSieveWaveStore::absolute_root() const noexcept {
    return state_->absolute_root;
}

const WaveManifestV1& DistributedSieveWaveStore::manifest() const noexcept {
    return state_->manifest;
}

const util::Sha256Digest& DistributedSieveWaveStore::manifest_digest() const noexcept {
    return state_->manifest.self_digest;
}

const NativeIdentityV1& DistributedSieveWaveStore::wave_root_identity() const noexcept {
    return state_->root_identity;
}

const NativeIdentityV1& DistributedSieveWaveStore::permanent_lock_identity() const noexcept {
    return state_->lock_identity;
}

const durable_record::RecordSnapshot&
DistributedSieveWaveStore::manifest_snapshot() const noexcept {
    return state_->manifest_snapshot;
}

DistributedSieveWaveStoreDiagnostic
DistributedSieveWaveStore::revalidate_authority() const noexcept {
    if (state_ == nullptr) {
        return diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                          invalid_argument_error());
    }

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
#else
    if (state_->creator_process_id == 0 || !process_matches(state_->creator_process_id)) {
        return process_mismatch();
    }
    if (const auto root_validated = validate_root_binding(
            state_->parent_fd, state_->parent_components, state_->root_fd, state_->root_leaf,
            state_->creator_process_id, state_->root_identity);
        root_validated.status != DistributedSieveWaveStoreStatus::ready) {
        return root_validated;
    }
    if (const auto lock_validated = validate_lock_binding(
            state_->root_fd, state_->lock_fd, state_->creator_process_id, state_->lock_identity);
        lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
        return lock_validated;
    }
    if (const auto pending =
            require_manifest_pending_missing(state_->root_fd, state_->creator_process_id);
        pending.status != DistributedSieveWaveStoreStatus::ready) {
        return pending;
    }

    auto canonical = read_manifest_leaf(state_->root_fd, MANIFEST_LEAF, state_->creator_process_id);
    if (!canonical) {
        return canonical.diagnostic;
    }
    if (*canonical.bytes != state_->manifest_bytes ||
        *canonical.snapshot != state_->manifest_snapshot) {
        return diagnostic(DistributedSieveWaveStoreStatus::manifest_conflict, protocol_error());
    }
    auto decoded = decode_manifest(*canonical.bytes, state_->manifest.self_digest,
                                   state_->root_identity, state_->lock_identity);
    if (!decoded) {
        return decoded.diagnostic;
    }

    if (const auto root_validated = validate_root_binding(
            state_->parent_fd, state_->parent_components, state_->root_fd, state_->root_leaf,
            state_->creator_process_id, state_->root_identity);
        root_validated.status != DistributedSieveWaveStoreStatus::ready) {
        return root_validated;
    }
    if (const auto lock_validated = validate_lock_binding(
            state_->root_fd, state_->lock_fd, state_->creator_process_id, state_->lock_identity);
        lock_validated.status != DistributedSieveWaveStoreStatus::ready) {
        return lock_validated;
    }
    if (const auto pending =
            require_manifest_pending_missing(state_->root_fd, state_->creator_process_id);
        pending.status != DistributedSieveWaveStoreStatus::ready) {
        return pending;
    }
    if (!process_matches(state_->creator_process_id)) {
        return process_mismatch();
    }
    return {};
#endif
}

DistributedSieveWaveStoreDiagnostic DistributedSieveWaveStore::revalidate(
    DistributedSieveWaveStoreInventoryTestHooks hooks) const noexcept {
    if (const auto authority = revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
#else
    const auto inventory = inspect_namespace(state_->root_fd);
    if (!inventory) {
        return inventory.diagnostic;
    }
    const auto validated = validate_manifest_bound_inventory(
        state_->root_fd, *inventory.inventory, state_->manifest, state_->creator_process_id);
    if (!validated) {
        return validated.diagnostic;
    }
    if (hooks.after_first_validation != nullptr) {
        hooks.after_first_validation(hooks.context);
    }
    if (const auto authority = revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    const auto final_inventory = inspect_namespace(state_->root_fd);
    if (!final_inventory) {
        return final_inventory.diagnostic;
    }
    if (*final_inventory.inventory != *inventory.inventory) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    const auto final_validated = validate_manifest_bound_inventory(
        state_->root_fd, *final_inventory.inventory, state_->manifest, state_->creator_process_id);
    if (!final_validated) {
        return final_validated.diagnostic;
    }
    if (*final_validated.identities != *validated.identities) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    if (!process_matches(state_->creator_process_id)) {
        return process_mismatch();
    }
    return {};
#endif
}

DistributedSievePrivateLeaseRootClaimResult
DistributedSieveWaveStore::claim_private_lease_root() const noexcept {
    if (state_ == nullptr) {
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                    invalid_argument_error())};
    }
    if (state_->creator_process_id == 0 || !process_matches(state_->creator_process_id)) {
        return {nullptr, process_mismatch()};
    }

    if (state_->private_lease_root_action_claimed.test_and_set(std::memory_order_acq_rel)) {
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::private_lease_root_busy,
                                    std::make_error_code(std::errc::device_or_resource_busy))};
    }

    std::unique_ptr<DistributedSievePrivateLeaseRootClaim> claim;
    try {
        claim.reset(new DistributedSievePrivateLeaseRootClaim(state_));
    } catch (const std::bad_alloc&) {
        state_->private_lease_root_action_claimed.clear(std::memory_order_release);
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                    std::make_error_code(std::errc::not_enough_memory))};
    } catch (...) {
        state_->private_lease_root_action_claimed.clear(std::memory_order_release);
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                    std::make_error_code(std::errc::io_error))};
    }

    auto validated = claim->revalidate();
    if (validated.status != DistributedSieveWaveStoreStatus::ready) {
        return {nullptr, std::move(validated)};
    }
    return {std::move(claim), {}};
}

DistributedSievePrivateLeaseRootClaimResult
DistributedSieveWaveStore::create_worker_attempt_private_lease_root(
    std::uint32_t chunk_id, std::uint32_t attempt_ordinal,
    DistributedSievePrivateLeaseBaseLockTestHooks hooks) const noexcept {
    return claim_worker_attempt_private_lease_root(chunk_id, attempt_ordinal,
                                                   AttemptBaseLockExpectation::absent, hooks);
}

DistributedSievePrivateLeaseRootClaimResult
DistributedSieveWaveStore::open_worker_attempt_private_lease_root(
    std::uint32_t chunk_id, std::uint32_t attempt_ordinal,
    DistributedSievePrivateLeaseBaseLockTestHooks hooks) const noexcept {
    return claim_worker_attempt_private_lease_root(chunk_id, attempt_ordinal,
                                                   AttemptBaseLockExpectation::present, hooks);
}

DistributedSievePrivateLeaseRootClaimResult
DistributedSieveWaveStore::claim_worker_attempt_private_lease_root(
    std::uint32_t chunk_id, std::uint32_t attempt_ordinal, AttemptBaseLockExpectation expectation,
    DistributedSievePrivateLeaseBaseLockTestHooks hooks) const noexcept {
    if (state_ == nullptr) {
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                    invalid_argument_error())};
    }
    if (state_->creator_process_id == 0 || !process_matches(state_->creator_process_id)) {
        return {nullptr, process_mismatch()};
    }

    try {
        const ChunkPlanV1* matched_chunk = nullptr;
        if (attempt_ordinal < state_->manifest.max_worker_attempts) {
            for (const auto& chunk : state_->manifest.chunks) {
                if (chunk.chunk_id == chunk_id) {
                    matched_chunk = &chunk;
                    break;
                }
            }
        }
        if (matched_chunk == nullptr || matched_chunk->sq_begin >= matched_chunk->sq_end) {
            return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                        invalid_argument_error())};
        }
        auto names = distributed_sieve_worker_attempt_names_v1(
            matched_chunk->relative_artifact_stem, chunk_id, attempt_ordinal);
        if (!names.has_value()) {
            return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                        invalid_argument_error())};
        }
        std::string target_leaf = names->base_lock_leaf;

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)expectation;
        (void)hooks;
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                                    unsupported_error())};
#else
        auto claimed = claim_private_lease_root();
        if (!claimed || claimed.claim == nullptr) {
            return claimed;
        }
        auto& claim = *claimed.claim;

        if (const auto authority = claim.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return {nullptr, authority};
        }
        auto before = capture_manifest_bound_inventory_witness(state_->root_fd, state_->manifest,
                                                               state_->creator_process_id);
        if (!before) {
            if (const auto authority = claim.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return {nullptr, authority};
            }
            return {nullptr, std::move(before.diagnostic)};
        }
        if (const auto authority = claim.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return {nullptr, authority};
        }

        const auto target_position =
            std::lower_bound(before.inventory->private_lease_base_lock_leaves.begin(),
                             before.inventory->private_lease_base_lock_leaves.end(), target_leaf);
        const std::size_t target_index = static_cast<std::size_t>(std::distance(
            before.inventory->private_lease_base_lock_leaves.begin(), target_position));
        const bool target_present =
            target_position != before.inventory->private_lease_base_lock_leaves.end() &&
            *target_position == target_leaf;
        if ((expectation == AttemptBaseLockExpectation::absent && target_present) ||
            (expectation == AttemptBaseLockExpectation::present && !target_present)) {
            if (const auto authority = claim.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return {nullptr, authority};
            }
            return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                        protocol_error())};
        }

        NamespaceInventory expected_successor_inventory = *before.inventory;
        std::vector<NativeIdentityV1> expected_successor_identities = *before.base_lock_identities;
        std::optional<NativeIdentityV1> expected_existing_identity;
        if (expectation == AttemptBaseLockExpectation::absent) {
            expected_successor_inventory.private_lease_base_lock_leaves.insert(
                expected_successor_inventory.private_lease_base_lock_leaves.begin() +
                    static_cast<std::ptrdiff_t>(target_index),
                target_leaf);
            expected_successor_identities.insert(expected_successor_identities.begin() +
                                                     static_cast<std::ptrdiff_t>(target_index),
                                                 NativeIdentityV1{});
        } else {
            expected_existing_identity = before.base_lock_identities->at(target_index);
        }
        claim.worker_attempt_names_.emplace(std::move(*names));

        if (const auto hooked = invoke_private_lease_base_lock_hook(
                hooks.after_initial_phase_validation, hooks.context, state_->creator_process_id);
            hooked.status != DistributedSieveWaveStoreStatus::ready) {
            return {nullptr, hooked};
        }
        if (const auto authority = claim.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return {nullptr, authority};
        }
        auto immediately_before = capture_manifest_bound_inventory_witness(
            state_->root_fd, state_->manifest, state_->creator_process_id);
        if (!immediately_before) {
            if (const auto authority = claim.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return {nullptr, authority};
            }
            return {nullptr, std::move(immediately_before.diagnostic)};
        }
        if (*immediately_before.inventory != *before.inventory ||
            *immediately_before.base_lock_identities != *before.base_lock_identities) {
            if (const auto authority = claim.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return {nullptr, authority};
            }
            return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                        protocol_error())};
        }
        if (const auto authority = claim.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return {nullptr, authority};
        }

        DistributedSieveWaveStoreDiagnostic target_outcome;
        std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> target;
        if (expectation == AttemptBaseLockExpectation::absent) {
            target = DistributedSievePrivateLeaseBaseLockAt::create_new_locked(
                state_->root_fd, std::move(target_leaf), state_->creator_process_id, hooks,
                target_outcome);
        } else {
            target = DistributedSievePrivateLeaseBaseLockAt::open_existing_locked(
                state_->root_fd, std::move(target_leaf), *expected_existing_identity,
                state_->creator_process_id, hooks, target_outcome);
        }
        if (target == nullptr || target_outcome.status != DistributedSieveWaveStoreStatus::ready) {
            if (const auto authority = claim.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return {nullptr, authority};
            }
            if (target_outcome.status == DistributedSieveWaveStoreStatus::ready) {
                target_outcome = diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                            std::make_error_code(std::errc::io_error));
            }
            return {nullptr, std::move(target_outcome)};
        }

        if (expectation == AttemptBaseLockExpectation::absent) {
            expected_successor_identities[target_index] = target->identity();
        }

        bool target_revalidation_boundary_offered = false;
        const auto revalidate_higher_priority_bindings =
            [&]() noexcept -> DistributedSieveWaveStoreDiagnostic {
            if (const auto authority = claim.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return authority;
            }
            if (const auto target_validated = target->revalidate();
                target_validated.status != DistributedSieveWaveStoreStatus::ready) {
                if (const auto authority = claim.revalidate_authority();
                    authority.status != DistributedSieveWaveStoreStatus::ready) {
                    return authority;
                }
                return target_validated;
            }
            if (!target_revalidation_boundary_offered) {
                target_revalidation_boundary_offered = true;
                if (const auto hooked = invoke_private_lease_base_lock_hook(
                        hooks.after_target_revalidation, hooks.context, state_->creator_process_id);
                    hooked.status != DistributedSieveWaveStoreStatus::ready) {
                    return hooked;
                }
            }
            return claim.revalidate_authority();
        };
        const auto revalidate_closed_successor =
            [&]() noexcept -> DistributedSieveWaveStoreDiagnostic {
            if (const auto bindings = revalidate_higher_priority_bindings();
                bindings.status != DistributedSieveWaveStoreStatus::ready) {
                return bindings;
            }

            auto first = capture_manifest_bound_inventory_witness(state_->root_fd, state_->manifest,
                                                                  state_->creator_process_id);
            if (!first) {
                if (const auto bindings = revalidate_higher_priority_bindings();
                    bindings.status != DistributedSieveWaveStoreStatus::ready) {
                    return bindings;
                }
                return std::move(first.diagnostic);
            }
            if (*first.inventory != expected_successor_inventory ||
                *first.base_lock_identities != expected_successor_identities) {
                if (const auto bindings = revalidate_higher_priority_bindings();
                    bindings.status != DistributedSieveWaveStoreStatus::ready) {
                    return bindings;
                }
                return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                  protocol_error());
            }
            if (const auto bindings = revalidate_higher_priority_bindings();
                bindings.status != DistributedSieveWaveStoreStatus::ready) {
                return bindings;
            }

            auto confirmed = capture_manifest_bound_inventory_witness(
                state_->root_fd, state_->manifest, state_->creator_process_id);
            if (!confirmed) {
                if (const auto bindings = revalidate_higher_priority_bindings();
                    bindings.status != DistributedSieveWaveStoreStatus::ready) {
                    return bindings;
                }
                return std::move(confirmed.diagnostic);
            }
            if (*confirmed.inventory != *first.inventory ||
                *confirmed.base_lock_identities != *first.base_lock_identities) {
                if (const auto bindings = revalidate_higher_priority_bindings();
                    bindings.status != DistributedSieveWaveStoreStatus::ready) {
                    return bindings;
                }
                return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                  protocol_error());
            }
            return revalidate_higher_priority_bindings();
        };

        if (const auto validated = revalidate_closed_successor();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            return {nullptr, validated};
        }
        if (const auto synchronized = target->synchronize(hooks);
            synchronized.status != DistributedSieveWaveStoreStatus::ready) {
            if (const auto validated = revalidate_closed_successor();
                validated.status != DistributedSieveWaveStoreStatus::ready) {
                return {nullptr, validated};
            }
            return {nullptr, synchronized};
        }
        if (const auto validated = revalidate_closed_successor();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            return {nullptr, validated};
        }

        claim.expected_private_lease_base_lock_leaves_.emplace(
            std::move(expected_successor_inventory.private_lease_base_lock_leaves));
        claim.expected_private_lease_base_lock_identities_.emplace(
            std::move(expected_successor_identities));
        claim.base_lock_at_ = std::move(target);
        if (const auto fully_validated = claim.revalidate();
            fully_validated.status != DistributedSieveWaveStoreStatus::ready) {
            return {nullptr, fully_validated};
        }
        return claimed;
#endif
    } catch (const std::bad_alloc&) {
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                    std::make_error_code(std::errc::not_enough_memory))};
    } catch (...) {
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                    std::make_error_code(std::errc::io_error))};
    }
}

} // namespace gnfs::sieve::distributed_sieve_resume_detail
