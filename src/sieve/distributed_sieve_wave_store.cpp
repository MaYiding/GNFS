#include "../relation/ooc_private_lease_recovery_protocol_internal.hpp"
#include "../relation/ooc_private_lease_reservation_protocol_internal.hpp"
#include "distributed_sieve_bound_work_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_launcher_internal.hpp"
#include "distributed_sieve_worker_work_package_file_internal.hpp"

#include <gnfs/relation/ooc_cleanup_transaction.hpp>
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
#include <stdio.h>
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <sys/xattr.h>
#endif
#include <unistd.h>
#endif

namespace gnfs::sieve::distributed_sieve_resume_detail {
namespace {

namespace durable_record = gnfs::util::durable_immutable_record;
namespace private_lease = gnfs::relation::ooc_cleanup_detail;
namespace work_package_file = gnfs::sieve::distributed_sieve_worker_work_package_file_detail;

static_assert(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES.size() ==
              private_lease::PRIVATE_LEASE_RESERVATION_BOUNDARIES.size());
static_assert(
    static_cast<std::size_t>(DistributedSievePrivateLeaseReservationBoundary::PermitAcquired) ==
    static_cast<std::size_t>(private_lease::PrivateLeaseReservationBoundary::PermitAcquired));
static_assert(static_cast<std::size_t>(
                  DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable) ==
              static_cast<std::size_t>(
                  private_lease::PrivateLeaseReservationBoundary::ReservedPendingDurable));
static_assert(static_cast<std::size_t>(
                  DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable) ==
              static_cast<std::size_t>(
                  private_lease::PrivateLeaseReservationBoundary::ReservedCanonicalDurable));
static_assert(static_cast<std::size_t>(
                  DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable) ==
              static_cast<std::size_t>(
                  private_lease::PrivateLeaseReservationBoundary::StagingDirectoryDurable));
static_assert(
    static_cast<std::size_t>(
        DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable) ==
    static_cast<std::size_t>(private_lease::PrivateLeaseReservationBoundary::OwnerPendingDurable));
static_assert(static_cast<std::size_t>(
                  DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable) ==
              static_cast<std::size_t>(
                  private_lease::PrivateLeaseReservationBoundary::OwnerCanonicalDurable));
static_assert(
    static_cast<std::size_t>(
        DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable) ==
    static_cast<std::size_t>(private_lease::PrivateLeaseReservationBoundary::OwnedPendingDurable));
static_assert(static_cast<std::size_t>(
                  DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable) ==
              static_cast<std::size_t>(
                  private_lease::PrivateLeaseReservationBoundary::OwnedCanonicalDurable));
static_assert(static_cast<std::size_t>(
                  DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable) ==
              static_cast<std::size_t>(
                  private_lease::PrivateLeaseReservationBoundary::FinalDirectoryDurable));
static_assert([] {
    for (std::size_t index = 0;
         index < DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES.size(); ++index) {
        if (static_cast<std::size_t>(
                DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVATION_BOUNDARIES[index]) !=
            static_cast<std::size_t>(private_lease::PRIVATE_LEASE_RESERVATION_BOUNDARIES[index])) {
            return false;
        }
    }
    return true;
}());
static_assert(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RECOVERY_EDGES.size() ==
              private_lease::PRIVATE_LEASE_RECOVERY_TRANSITIONS.size());
static_assert([] {
    for (std::size_t index = 0; index < DISTRIBUTED_SIEVE_PRIVATE_LEASE_RECOVERY_EDGES.size();
         ++index) {
        const auto& edge = DISTRIBUTED_SIEVE_PRIVATE_LEASE_RECOVERY_EDGES[index];
        const auto& transition = private_lease::PRIVATE_LEASE_RECOVERY_TRANSITIONS[index];
        if (static_cast<std::size_t>(edge.source) != static_cast<std::size_t>(transition.source) ||
            static_cast<std::size_t>(edge.successor) !=
                static_cast<std::size_t>(transition.successor)) {
            return false;
        }
    }
    return true;
}());

inline constexpr char LOCK_LEAF[] = ".gnfs-wave-v1.lock";
inline constexpr char MANIFEST_LEAF[] = ".gnfs-wave-v1.manifest";
inline constexpr char MANIFEST_PENDING_LEAF[] = ".gnfs-wave-v1.manifest.pending";
inline constexpr std::string_view WORKER_CORPUS_INDEX_LEAF = "corpus.relidx";
inline constexpr std::string_view WORKER_CORPUS_DATA_LEAF = "corpus.reldata";
inline constexpr std::string_view WORKER_HANDOFF_LEAF = "corpus.gnfs-ooc-private-handoff-v1";
inline constexpr std::string_view WORKER_HANDOFF_PENDING_LEAF =
    "corpus.gnfs-ooc-private-handoff-v1.pending";

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

struct ImmutableProtocolRecordReadResult final {
    std::optional<std::vector<std::byte>> bytes;
    std::optional<durable_record::RecordSnapshot> snapshot;
    bool missing = false;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return bytes.has_value() && snapshot.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] ImmutableProtocolRecordReadResult
read_immutable_protocol_record_leaf(int root_fd, const char* leaf, std::uint64_t creator_process_id,
                                    DistributedSieveWaveStoreStatus missing_status,
                                    DistributedSieveWaveStoreStatus invalid_status) noexcept {
    ImmutableProtocolRecordReadResult result;
    if (root_fd < 0 || leaf == nullptr || *leaf == '\0') {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error());
        return result;
    }
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }
    const int descriptor =
        openat_retrying_eintr(root_fd, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            result.missing = true;
            result.diagnostic = diagnostic(missing_status, posix_error(saved_errno));
        } else {
            result.diagnostic = diagnostic(invalid_status, posix_error(saved_errno));
        }
        return result;
    }
    UniqueFd held(descriptor);
    if (!process_matches(creator_process_id)) {
        result.diagnostic = process_mismatch();
        return result;
    }

    struct stat held_before{};
    struct stat named_before{};
    if (fstat_retrying_eintr(held.get(), held_before) != 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
        return result;
    }
    if (fstatat_retrying_eintr(root_fd, leaf, named_before) != 0) {
        result.diagnostic = diagnostic(invalid_status, posix_error(errno));
        return result;
    }
    if (!valid_manifest_metadata(held_before) || !valid_manifest_metadata(named_before) ||
        !stable_metadata(held_before, named_before)) {
        result.diagnostic = diagnostic(invalid_status, protocol_error());
        return result;
    }
    if (auto acl = acl_rejection(held.get(), false, invalid_status); acl.has_value()) {
        result.diagnostic = *acl;
        return result;
    }

    const auto exact_size = static_cast<std::size_t>(held_before.st_size);
    try {
        result.bytes.emplace(exact_size);
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
    while (offset < result.bytes->size()) {
        if (!process_matches(creator_process_id)) {
            result.diagnostic = process_mismatch();
            return result;
        }
        const std::size_t request =
            std::min(result.bytes->size() - offset,
                     static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t count =
            ::pread(held.get(), result.bytes->data() + offset, request, static_cast<off_t>(offset));
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
            result.diagnostic = diagnostic(invalid_status, protocol_error());
            return result;
        }
        offset += static_cast<std::size_t>(count);
    }

    std::byte trailing{};
    ssize_t trailing_count = -1;
    do {
        trailing_count =
            ::pread(held.get(), &trailing, 1, static_cast<off_t>(result.bytes->size()));
    } while (trailing_count < 0 && errno == EINTR);
    if (trailing_count < 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
        return result;
    }
    if (trailing_count != 0) {
        result.diagnostic = diagnostic(invalid_status, protocol_error());
        return result;
    }

    struct stat held_after{};
    struct stat named_after{};
    if (fstat_retrying_eintr(held.get(), held_after) != 0) {
        result.diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
        return result;
    }
    if (fstatat_retrying_eintr(root_fd, leaf, named_after) != 0) {
        result.diagnostic = diagnostic(invalid_status, posix_error(errno));
        return result;
    }
    if (!valid_manifest_metadata(held_after) || !valid_manifest_metadata(named_after) ||
        !stable_metadata(held_before, held_after) || !stable_metadata(held_after, named_after)) {
        result.diagnostic = diagnostic(invalid_status, protocol_error());
        return result;
    }
    if (auto acl = acl_rejection(held.get(), false, invalid_status); acl.has_value()) {
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
    return result;
}

[[nodiscard]] bool valid_private_lease_marker_metadata(const struct stat& metadata) noexcept {
    return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 &&
           static_cast<std::uint64_t>(metadata.st_size) ==
               private_lease::PRIVATE_LEASE_MARKER_BYTES &&
           static_cast<std::uint64_t>(metadata.st_uid) == static_cast<std::uint64_t>(::geteuid()) &&
           exact_mode(metadata, 0600);
}

[[nodiscard]] bool valid_private_lease_directory_metadata(const struct stat& metadata) noexcept {
    return S_ISDIR(metadata.st_mode) &&
           static_cast<std::uint64_t>(metadata.st_uid) == static_cast<std::uint64_t>(::geteuid()) &&
           exact_mode(metadata, 0700);
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
    std::vector<std::string> private_lease_protocol_leaves;
    std::vector<std::string> worker_attempt_record_leaves;
    std::vector<DistributedSieveWorkerAttemptRecordInventoryWitness> worker_attempt_records;

    [[nodiscard]] friend constexpr bool operator==(const NamespaceInventory&,
                                                   const NamespaceInventory&) noexcept = default;
};

[[nodiscard]] bool
has_pre_manifest_private_lease_candidate(const NamespaceInventory& inventory) noexcept {
    return !inventory.manifest && (!inventory.private_lease_protocol_leaves.empty() ||
                                   !inventory.worker_attempt_record_leaves.empty());
}

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
        if (parse_distributed_sieve_worker_attempt_leaf_v1(leaf).has_value()) {
            constexpr std::size_t max_attempt_record_leaf_count =
                static_cast<std::size_t>(DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) *
                DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS * 2U;
            if (inventory.worker_attempt_record_leaves.size() >= max_attempt_record_leaf_count) {
                return {std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                   protocol_error())};
            }
            try {
                inventory.worker_attempt_record_leaves.emplace_back(leaf);
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
        constexpr std::size_t max_protocol_leaf_count =
            static_cast<std::size_t>(DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) *
            DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS * 6U;
        if (inventory.private_lease_protocol_leaves.size() >= max_protocol_leaf_count) {
            return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                             protocol_error())};
        }
        try {
            inventory.private_lease_protocol_leaves.emplace_back(leaf);
        } catch (const std::bad_alloc&) {
            return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                             std::make_error_code(std::errc::not_enough_memory))};
        } catch (...) {
            return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                             std::make_error_code(std::errc::io_error))};
        }
    }
    std::sort(inventory.private_lease_base_lock_leaves.begin(),
              inventory.private_lease_base_lock_leaves.end());
    std::sort(inventory.private_lease_protocol_leaves.begin(),
              inventory.private_lease_protocol_leaves.end());
    std::sort(inventory.worker_attempt_record_leaves.begin(),
              inventory.worker_attempt_record_leaves.end());
    return {std::move(inventory), {}};
}

struct ManifestWorkerAttemptCoordinate final {
    std::uint32_t chunk_id = 0;
    std::uint32_t attempt_ordinal = 0;
};

[[nodiscard]] std::optional<ManifestWorkerAttemptCoordinate>
manifest_worker_attempt_coordinate_from_private_lease_base_lock(const WaveManifestV1& manifest,
                                                                std::string_view leaf) noexcept {
    if (!leaf.ends_with(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX)) {
        return std::nullopt;
    }
    leaf.remove_suffix(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX.size());
    constexpr std::size_t attempt_suffix_size =
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() +
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
    if (leaf.size() <= attempt_suffix_size) {
        return std::nullopt;
    }
    const std::size_t attempt_tag_offset = leaf.size() - attempt_suffix_size;
    if (leaf.substr(attempt_tag_offset, DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size()) !=
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1) {
        return std::nullopt;
    }
    const std::size_t digits_offset =
        attempt_tag_offset + DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size();
    if (leaf[digits_offset] < '0' || leaf[digits_offset] > '9' || leaf[digits_offset + 1U] < '0' ||
        leaf[digits_offset + 1U] > '9') {
        return std::nullopt;
    }
    const auto attempt_ordinal = static_cast<std::uint32_t>(leaf[digits_offset] - '0') * 10U +
                                 static_cast<std::uint32_t>(leaf[digits_offset + 1U] - '0');
    if (attempt_ordinal >= manifest.max_worker_attempts) {
        return std::nullopt;
    }

    const std::string_view chunk_stem = leaf.substr(0, attempt_tag_offset);
    const ChunkPlanV1* matched_chunk = nullptr;
    for (const auto& chunk : manifest.chunks) {
        if (chunk.relative_artifact_stem == chunk_stem) {
            if (matched_chunk != nullptr || chunk.sq_begin >= chunk.sq_end) {
                return std::nullopt;
            }
            matched_chunk = &chunk;
        }
    }
    if (matched_chunk == nullptr ||
        !distributed_sieve_worker_attempt_relative_stem_matches(
            matched_chunk->relative_artifact_stem, attempt_ordinal, leaf)) {
        return std::nullopt;
    }
    return ManifestWorkerAttemptCoordinate{
        .chunk_id = matched_chunk->chunk_id,
        .attempt_ordinal = attempt_ordinal,
    };
}

[[nodiscard]] bool
manifest_attempt_matches_private_lease_base_lock(const WaveManifestV1& manifest,
                                                 std::string_view leaf) noexcept {
    return manifest_worker_attempt_coordinate_from_private_lease_base_lock(manifest, leaf)
        .has_value();
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

enum class PrivateLeaseProtocolLeafRole : std::uint8_t {
    reserved,
    reserved_pending,
    owned,
    owned_pending,
    staging_directory,
    final_directory,
};

struct ParsedPrivateLeaseProtocolLeaf final {
    DistributedSieveWorkerAttemptNamesV1 names;
    PrivateLeaseProtocolLeafRole role = PrivateLeaseProtocolLeafRole::reserved;
    std::optional<std::array<std::uint64_t, 2>> staging_lease_id;
};

[[nodiscard]] std::optional<DistributedSieveWorkerAttemptNamesV1>
manifest_attempt_names_from_relative_stem(const WaveManifestV1& manifest,
                                          std::string_view relative_stem) {
    constexpr std::size_t suffix_size = DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() +
                                        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
    if (relative_stem.size() <= suffix_size) {
        return std::nullopt;
    }
    const std::size_t tag_offset = relative_stem.size() - suffix_size;
    if (relative_stem.substr(tag_offset, DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size()) !=
        DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1) {
        return std::nullopt;
    }
    const std::size_t digits_offset =
        tag_offset + DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size();
    if (relative_stem[digits_offset] < '0' || relative_stem[digits_offset] > '9' ||
        relative_stem[digits_offset + 1U] < '0' || relative_stem[digits_offset + 1U] > '9') {
        return std::nullopt;
    }
    const auto attempt_ordinal =
        static_cast<std::uint32_t>(relative_stem[digits_offset] - '0') * 10U +
        static_cast<std::uint32_t>(relative_stem[digits_offset + 1U] - '0');
    if (attempt_ordinal >= manifest.max_worker_attempts) {
        return std::nullopt;
    }

    const std::string_view chunk_stem = relative_stem.substr(0, tag_offset);
    const ChunkPlanV1* matched_chunk = nullptr;
    for (const auto& chunk : manifest.chunks) {
        if (chunk.relative_artifact_stem == chunk_stem) {
            if (matched_chunk != nullptr || chunk.sq_begin >= chunk.sq_end) {
                return std::nullopt;
            }
            matched_chunk = &chunk;
        }
    }
    if (matched_chunk == nullptr) {
        return std::nullopt;
    }
    return distributed_sieve_worker_attempt_names_v1(matched_chunk->relative_artifact_stem,
                                                     matched_chunk->chunk_id, attempt_ordinal);
}

[[nodiscard]] std::optional<std::uint8_t> hexadecimal_nibble(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::array<std::uint64_t, 2>>
parse_private_lease_id_hex(std::string_view encoded) noexcept {
    if (encoded.size() != 32U) {
        return std::nullopt;
    }
    std::array<std::uint64_t, 2> lease_id{};
    for (std::size_t word = 0; word < lease_id.size(); ++word) {
        for (std::size_t digit = 0; digit < 16U; ++digit) {
            const auto nibble = hexadecimal_nibble(encoded[word * 16U + digit]);
            if (!nibble.has_value()) {
                return std::nullopt;
            }
            lease_id[word] = (lease_id[word] << 4U) | *nibble;
        }
    }
    if (lease_id[0] == 0 && lease_id[1] == 0) {
        return std::nullopt;
    }
    return lease_id;
}

[[nodiscard]] std::optional<ParsedPrivateLeaseProtocolLeaf>
parse_manifest_bound_private_lease_protocol_leaf(const WaveManifestV1& manifest,
                                                 std::string_view leaf) {
    const auto parse_fixed = [&](std::string_view suffix, PrivateLeaseProtocolLeafRole role,
                                 const std::string DistributedSieveWorkerAttemptNamesV1::* member)
        -> std::optional<ParsedPrivateLeaseProtocolLeaf> {
        if (!leaf.ends_with(suffix)) {
            return std::nullopt;
        }
        auto names = manifest_attempt_names_from_relative_stem(
            manifest, leaf.substr(0, leaf.size() - suffix.size()));
        if (!names.has_value() || (*names).*member != leaf) {
            return std::nullopt;
        }
        return ParsedPrivateLeaseProtocolLeaf{
            .names = std::move(*names),
            .role = role,
        };
    };

    if (auto parsed = parse_fixed(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_PENDING_SUFFIX,
                                  PrivateLeaseProtocolLeafRole::reserved_pending,
                                  &DistributedSieveWorkerAttemptNamesV1::reserved_pending_leaf)) {
        return parsed;
    }
    if (auto parsed = parse_fixed(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_SUFFIX,
                                  PrivateLeaseProtocolLeafRole::reserved,
                                  &DistributedSieveWorkerAttemptNamesV1::reserved_leaf)) {
        return parsed;
    }
    if (auto parsed = parse_fixed(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_PENDING_SUFFIX,
                                  PrivateLeaseProtocolLeafRole::owned_pending,
                                  &DistributedSieveWorkerAttemptNamesV1::owned_pending_leaf)) {
        return parsed;
    }
    if (auto parsed = parse_fixed(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_SUFFIX,
                                  PrivateLeaseProtocolLeafRole::owned,
                                  &DistributedSieveWorkerAttemptNamesV1::owned_leaf)) {
        return parsed;
    }
    if (auto parsed = parse_fixed(DISTRIBUTED_SIEVE_PRIVATE_LEASE_DIRECTORY_SUFFIX,
                                  PrivateLeaseProtocolLeafRole::final_directory,
                                  &DistributedSieveWorkerAttemptNamesV1::private_directory_leaf)) {
        return parsed;
    }

    constexpr std::size_t lease_id_hex_size = 32U;
    if (leaf.size() <= DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG.size() + lease_id_hex_size) {
        return std::nullopt;
    }
    const std::size_t staging_tag_offset =
        leaf.size() - DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG.size() - lease_id_hex_size;
    if (leaf.substr(staging_tag_offset, DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG.size()) !=
        DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG) {
        return std::nullopt;
    }
    auto names =
        manifest_attempt_names_from_relative_stem(manifest, leaf.substr(0, staging_tag_offset));
    const auto lease_id = parse_private_lease_id_hex(leaf.substr(leaf.size() - lease_id_hex_size));
    if (!names.has_value() || !lease_id.has_value()) {
        return std::nullopt;
    }
    std::string expected = names->relative_lease_stem;
    expected.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG);
    expected.append(private_lease::private_lease_id_hex(*lease_id));
    if (expected != leaf) {
        return std::nullopt;
    }
    return ParsedPrivateLeaseProtocolLeaf{
        .names = std::move(*names),
        .role = PrivateLeaseProtocolLeafRole::staging_directory,
        .staging_lease_id = lease_id,
    };
}

struct PrivateLeaseMarkerAtResult final {
    std::optional<private_lease::PrivateLeaseRecord> record;
    std::optional<NativeIdentityV1> identity;
    DistributedSieveWaveStoreDiagnostic diagnostic;
    UniqueFd marker{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return record.has_value() && identity.has_value() && static_cast<bool>(marker) &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] PrivateLeaseMarkerAtResult
read_private_lease_marker_at(int parent_fd, const std::string& leaf,
                             std::uint64_t creator_process_id) noexcept {
    if (parent_fd < 0 || !process_matches(creator_process_id)) {
        return {std::nullopt, std::nullopt,
                parent_fd < 0 ? diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                           invalid_argument_error())
                              : process_mismatch()};
    }
    const int descriptor = openat_retrying_eintr(parent_fd, leaf.c_str(),
                                                 O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        return {
            std::nullopt, std::nullopt,
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno))};
    }
    UniqueFd held(descriptor);

    struct stat held_before{};
    struct stat named_before{};
    if (fstat_retrying_eintr(held.get(), held_before) != 0) {
        return {
            std::nullopt, std::nullopt,
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno))};
    }
    if (fstatat_retrying_eintr(parent_fd, leaf.c_str(), named_before) != 0) {
        return {
            std::nullopt, std::nullopt,
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno))};
    }
    if (!valid_private_lease_marker_metadata(held_before) ||
        !valid_private_lease_marker_metadata(named_before) ||
        !stable_metadata(held_before, named_before)) {
        return {std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    if (auto acl =
            acl_rejection(held.get(), false, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return {std::nullopt, std::nullopt, *acl};
    }

    std::array<std::byte, private_lease::PRIVATE_LEASE_MARKER_BYTES> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (!process_matches(creator_process_id)) {
            return {std::nullopt, std::nullopt, process_mismatch()};
        }
        const ssize_t count = ::pread(held.get(), bytes.data() + offset, bytes.size() - offset,
                                      static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {std::nullopt, std::nullopt,
                    diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno))};
        }
        if (count == 0) {
            return {
                std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
        }
        offset += static_cast<std::size_t>(count);
    }

    struct stat held_after{};
    struct stat named_after{};
    if (fstat_retrying_eintr(held.get(), held_after) != 0) {
        return {
            std::nullopt, std::nullopt,
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno))};
    }
    if (fstatat_retrying_eintr(parent_fd, leaf.c_str(), named_after) != 0) {
        return {
            std::nullopt, std::nullopt,
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno))};
    }
    if (!valid_private_lease_marker_metadata(held_after) ||
        !valid_private_lease_marker_metadata(named_after) ||
        !stable_metadata(held_before, held_after) || !stable_metadata(held_after, named_after)) {
        return {std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    if (auto acl =
            acl_rejection(held.get(), false, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return {std::nullopt, std::nullopt, *acl};
    }
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, std::nullopt, process_mismatch()};
    }

    try {
        return {
            private_lease::parse_private_lease_marker(bytes),
            protocol_identity(held_after),
            {},
            std::move(held),
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                           std::make_error_code(std::errc::not_enough_memory))};
    } catch (...) {
        return {std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic validate_private_lease_directory_binding(
    int root_fd, int directory_fd, const std::string& leaf, std::uint64_t creator_process_id,
    std::optional<NativeIdentityV1> expected_identity = std::nullopt) noexcept {
    if (root_fd < 0 || directory_fd < 0 || !process_matches(creator_process_id)) {
        return process_matches(creator_process_id)
                   ? diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                invalid_argument_error())
                   : process_mismatch();
    }
    struct stat held_before{};
    struct stat named{};
    struct stat held_after{};
    if (fstat_retrying_eintr(directory_fd, held_before) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    if (fstatat_retrying_eintr(root_fd, leaf.c_str(), named) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    if (fstat_retrying_eintr(directory_fd, held_after) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    if (!valid_private_lease_directory_metadata(held_before) ||
        !valid_private_lease_directory_metadata(named) ||
        !valid_private_lease_directory_metadata(held_after) ||
        !stable_metadata(held_before, held_after) || !stable_metadata(held_after, named)) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    if (auto acl =
            acl_rejection(directory_fd, true, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return *acl;
    }
    const NativeIdentityV1 identity = protocol_identity(held_after);
    if (expected_identity.has_value() && identity != *expected_identity) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    return process_matches(creator_process_id) ? DistributedSieveWaveStoreDiagnostic{}
                                               : process_mismatch();
}

struct PrivateLeaseDirectoryInventory final {
    bool owner = false;
    bool owner_pending = false;
    bool work_package_residue = false;
    bool corpus_index = false;
    bool corpus_data = false;
    bool worker_handoff = false;
    bool worker_handoff_pending = false;
};

struct PrivateLeaseDirectoryAtResult final {
    UniqueFd directory;
    std::optional<NativeIdentityV1> identity;
    std::optional<PrivateLeaseDirectoryInventory> inventory;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(directory) && identity.has_value() && inventory.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] PrivateLeaseDirectoryAtResult
inspect_private_lease_directory_at(int root_fd, const std::string& leaf,
                                   std::uint64_t creator_process_id) noexcept {
    const int descriptor = openat_retrying_eintr(root_fd, leaf.c_str(),
                                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        return {
            {},
            std::nullopt,
            std::nullopt,
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno))};
    }
    UniqueFd directory(descriptor);
    if (const auto validated = validate_private_lease_directory_binding(root_fd, directory.get(),
                                                                        leaf, creator_process_id);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        return {{}, std::nullopt, std::nullopt, validated};
    }
    struct stat metadata{};
    if (fstat_retrying_eintr(directory.get(), metadata) != 0) {
        return {{},
                std::nullopt,
                std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno))};
    }
    const NativeIdentityV1 identity = protocol_identity(metadata);

    const int scan_fd = openat_retrying_eintr(directory.get(), ".",
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (scan_fd < 0) {
        return {{},
                std::nullopt,
                std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno))};
    }
    DIR* raw_directory = ::fdopendir(scan_fd);
    if (raw_directory == nullptr) {
        const int saved_errno = errno;
        (void)::close(scan_fd);
        return {{},
                std::nullopt,
                std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(saved_errno))};
    }
    UniqueDirectory scan(raw_directory);
    PrivateLeaseDirectoryInventory inventory;
    errno = 0;
    for (;;) {
        dirent* entry = ::readdir(raw_directory);
        if (entry == nullptr) {
            if (errno != 0) {
                return {{},
                        std::nullopt,
                        std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno))};
            }
            break;
        }
        const std::string_view child(entry->d_name);
        if (child == "." || child == "..") {
            continue;
        }
        if (child == DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF && !inventory.owner) {
            inventory.owner = true;
            continue;
        }
        if (child == DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF &&
            !inventory.owner_pending) {
            inventory.owner_pending = true;
            continue;
        }
        if (child == work_package_file::DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1 &&
            !inventory.work_package_residue) {
            inventory.work_package_residue = true;
            continue;
        }
        if (child == WORKER_CORPUS_INDEX_LEAF && !inventory.corpus_index) {
            inventory.corpus_index = true;
            continue;
        }
        if (child == WORKER_CORPUS_DATA_LEAF && !inventory.corpus_data) {
            inventory.corpus_data = true;
            continue;
        }
        if (child == WORKER_HANDOFF_LEAF && !inventory.worker_handoff) {
            inventory.worker_handoff = true;
            continue;
        }
        if (child == WORKER_HANDOFF_PENDING_LEAF && !inventory.worker_handoff_pending) {
            inventory.worker_handoff_pending = true;
            continue;
        }
        return {{},
                std::nullopt,
                std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    if (const auto validated = validate_private_lease_directory_binding(
            root_fd, directory.get(), leaf, creator_process_id, identity);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        return {{}, std::nullopt, std::nullopt, validated};
    }
    return {std::move(directory), identity, inventory, {}};
}

struct PrivateLeaseAttemptInventory final {
    DistributedSieveWorkerAttemptNamesV1 names;
    NativeIdentityV1 base_lock_identity{};
    bool reserved = false;
    bool reserved_pending = false;
    bool owned = false;
    bool owned_pending = false;
    bool final_directory = false;
    std::optional<std::string> staging_directory_leaf;
    std::optional<std::array<std::uint64_t, 2>> staging_lease_id;
};

[[nodiscard]] std::array<std::uint64_t, 3>
relation_identity(const NativeIdentityV1& identity) noexcept {
    return {identity.volume, identity.object, identity.generation};
}

[[nodiscard]] NativeIdentityV1
protocol_identity(const durable_record::NativeIdentity& identity) noexcept {
    return {
        .volume = identity.first,
        .object = identity.second,
        .generation = identity.third,
    };
}

using PrivateLeaseReservationWitness = DistributedSievePrivateLeaseReservationInventoryWitness;

struct PrivateLeaseAttemptValidationResult final {
    std::optional<PrivateLeaseReservationWitness> witness;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return witness.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

struct WorkerHandoffInventoryValidationResult final {
    std::optional<DistributedSieveWorkerHandoffInventoryWitnessV1> witness;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return witness.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
worker_handoff_inspection_failure(const gnfs::relation::OOCCleanupResult& lower) noexcept {
    using Status = gnfs::relation::OOCCleanupStatus;
    const auto error = lower.native_error ? lower.native_error : protocol_error();
    switch (lower.status) {
    case Status::Interrupted:
        return diagnostic(DistributedSieveWaveStoreStatus::interrupted, error);
    case Status::Busy:
        return diagnostic(DistributedSieveWaveStoreStatus::private_lease_lock_busy, error);
    case Status::IoFailure:
        return diagnostic(DistributedSieveWaveStoreStatus::io_failed, error);
    case Status::DurabilityFailure:
        return diagnostic(DistributedSieveWaveStoreStatus::durability_failed, error);
    case Status::PlatformUnsupported:
        return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, error);
    case Status::UnexpectedFailure:
        return diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, error);
    case Status::Completed:
    case Status::NoTransaction:
    case Status::InvalidRequest:
    case Status::SourcePairInvalid:
    case Status::IntentCorrupt:
    case Status::IntentConflict:
    case Status::ForeignReplacementPreserved:
    case Status::NamespaceConflict:
    case Status::RecoveryRequired:
    case Status::HandoffPresent:
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, error);
    }
    return diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
}

[[nodiscard]] WorkerHandoffInventoryValidationResult validate_worker_handoff_inventory(
    const std::filesystem::path& absolute_root, const PrivateLeaseAttemptInventory& attempt,
    const WaveManifestV1& manifest, const NativeIdentityV1& directory_identity,
    std::uint64_t creator_process_id) noexcept {
    const auto fail_with = [](DistributedSieveWaveStoreDiagnostic failure) {
        return WorkerHandoffInventoryValidationResult{std::nullopt, std::move(failure)};
    };
    const auto conflict = [&] {
        return fail_with(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    };
    if (!process_matches(creator_process_id)) {
        return fail_with(process_mismatch());
    }

    try {
        const auto parsed =
            parse_distributed_sieve_worker_attempt_leaf_v1(attempt.names.canonical_record_leaf);
        if (!parsed.has_value() || parsed->pending) {
            return conflict();
        }
        const ChunkPlanV1* chunk = nullptr;
        for (const auto& candidate : manifest.chunks) {
            if (candidate.chunk_id == parsed->chunk_id) {
                chunk = &candidate;
                break;
            }
        }
        if (chunk == nullptr || chunk->sq_begin >= chunk->sq_end) {
            return conflict();
        }

        const std::filesystem::path base_path =
            absolute_root / attempt.names.private_directory_leaf / "corpus";
        auto adopted = gnfs::relation::OOCCleanupTransaction::adopt_private_handoff(base_path);
        if (!adopted.adopted() || !adopted.adoption.has_value()) {
            return fail_with(worker_handoff_inspection_failure(adopted.result));
        }
        const auto& receipt = *adopted.adoption;
        const auto& envelope = receipt.record();
        if (envelope.payload_kind !=
                static_cast<std::uint32_t>(DistributedSieveRecordKindV1::worker_handoff) ||
            envelope.payload_version != manifest.handoff_version ||
            protocol_identity(envelope.lock_identity) != attempt.base_lock_identity ||
            protocol_identity(envelope.directory_identity) != directory_identity) {
            return conflict();
        }

        auto decoded = decode_distributed_sieve_record(envelope.opaque_payload);
        if (!decoded || !decoded.value.has_value()) {
            auto failure =
                diagnostic(decoded.status.error == DistributedSieveProtocolError::resource_exhausted
                               ? DistributedSieveWaveStoreStatus::resource_exhausted
                               : DistributedSieveWaveStoreStatus::namespace_conflict,
                           protocol_error());
            failure.protocol_status = decoded.status;
            return fail_with(std::move(failure));
        }
        auto* handoff = std::get_if<WorkerHandoffV1>(&*decoded.value);
        if (handoff == nullptr || handoff->manifest_digest != manifest.self_digest ||
            handoff->work_digest != manifest.work_sha256 || handoff->wave_id != manifest.wave_id ||
            handoff->chunk_id != parsed->chunk_id ||
            handoff->attempt_ordinal != parsed->attempt_ordinal ||
            handoff->sq_begin != chunk->sq_begin || handoff->sq_end != chunk->sq_end ||
            handoff->lease.lease_id.limbs != envelope.lease_id ||
            handoff->lease.owner_marker != protocol_identity(envelope.owner_marker_identity) ||
            handoff->lease.directory != protocol_identity(envelope.directory_identity) ||
            handoff->lease.relative_stem != attempt.names.relative_lease_stem ||
            handoff->artifact.descriptor.format_version != envelope.pair.format_version ||
            handoff->artifact.descriptor.store_id != envelope.pair.store_id ||
            handoff->artifact.descriptor.generation != envelope.pair.generation ||
            handoff->artifact.descriptor.relation_count != envelope.pair.count ||
            handoff->artifact.descriptor.data_end != envelope.pair.data_extent ||
            handoff->artifact.index_file.identity != protocol_identity(envelope.index.identity) ||
            handoff->artifact.index_file.extent != envelope.index.extent ||
            handoff->artifact.data_file.identity != protocol_identity(envelope.data.identity) ||
            handoff->artifact.data_file.extent != envelope.data.extent ||
            !handoff->cleanup_intent_absent) {
            return conflict();
        }

        return {
            DistributedSieveWorkerHandoffInventoryWitnessV1{
                .handoff = std::move(*handoff),
                .envelope_digest = envelope.self_digest,
                .owned_marker_identity = protocol_identity(envelope.owned_marker_identity),
                .handoff_snapshot = receipt.handoff_snapshot(),
                .index_snapshot = receipt.index_snapshot(),
                .data_snapshot = receipt.data_snapshot(),
            },
            {},
        };
    } catch (const std::bad_alloc&) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                    std::make_error_code(std::errc::not_enough_memory)));
    } catch (...) {
        return conflict();
    }
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic work_package_residue_inspection_failure(
    const work_package_file::DistributedSieveWorkerWorkPackageFileDiagnostic& lower) noexcept {
    const auto status =
        lower.status ==
                work_package_file::DistributedSieveWorkerWorkPackageFileStatus::resource_exhausted
            ? DistributedSieveWaveStoreStatus::resource_exhausted
            : DistributedSieveWaveStoreStatus::namespace_conflict;
    auto failure = diagnostic(status, lower.native_error != 0 ? posix_error(lower.native_error)
                                                              : protocol_error());
    failure.protocol_status = lower.protocol_status;
    return failure;
}

[[nodiscard]] DistributedSieveWorkerWorkPackageResidueInventoryWitnessV1
compact_work_package_residue_witness(
    const work_package_file::DistributedSieveWorkerWorkPackageResidueWitnessV1& witness) noexcept {
    return {
        .body_bytes = witness.package.body_bytes,
        .total_bytes = witness.package.total_bytes,
        .work_sha256 = witness.package.work_sha256,
        .package_sha256 = witness.package.package_sha256,
        .file_identity = witness.file_identity,
        .file_extent = witness.file_extent,
        .owner_user_id = witness.owner_user_id,
    };
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
work_package_residue_reconciliation_inspection_failure(
    const work_package_file::DistributedSieveWorkerWorkPackageFileDiagnostic& lower) noexcept {
    using FileStatus = work_package_file::DistributedSieveWorkerWorkPackageFileStatus;
    DistributedSieveWaveStoreStatus status = DistributedSieveWaveStoreStatus::unexpected_failure;
    switch (lower.status) {
    case FileStatus::ready:
        status = DistributedSieveWaveStoreStatus::unexpected_failure;
        break;
    case FileStatus::interrupted:
        status = DistributedSieveWaveStoreStatus::interrupted;
        break;
    case FileStatus::invalid_request:
        status = DistributedSieveWaveStoreStatus::invalid_request;
        break;
    case FileStatus::platform_unavailable:
        status = DistributedSieveWaveStoreStatus::platform_unsupported;
        break;
    case FileStatus::resource_exhausted:
        status = DistributedSieveWaveStoreStatus::resource_exhausted;
        break;
    case FileStatus::namespace_conflict:
    case FileStatus::decode_failed:
        status = DistributedSieveWaveStoreStatus::namespace_conflict;
        break;
    case FileStatus::publication_failed:
        status = DistributedSieveWaveStoreStatus::publication_failed;
        break;
    case FileStatus::durability_failed:
        status = DistributedSieveWaveStoreStatus::durability_failed;
        break;
    case FileStatus::close_failed:
        status = DistributedSieveWaveStoreStatus::io_failed;
        break;
    case FileStatus::ops_contract_violation:
    case FileStatus::unexpected_failure:
        status = DistributedSieveWaveStoreStatus::unexpected_failure;
        break;
    }
    auto failure = diagnostic(status, lower.native_error != 0 ? posix_error(lower.native_error)
                                                              : protocol_error());
    if (lower.protocol_status.error != DistributedSieveProtocolError::none) {
        failure.protocol_status = lower.protocol_status;
    }
    return failure;
}

[[nodiscard]] std::optional<DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint>
wave_work_package_residue_reconciliation_fault_point(
    std::optional<
        work_package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1>
        point) noexcept {
    if (!point.has_value()) {
        return std::nullopt;
    }
    switch (*point) {
    case work_package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
        after_name_unlinked:
        return DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint::AfterNameUnlinked;
    case work_package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
        after_directory_durable:
        return DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint::
            AfterDirectoryDurable;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<
    work_package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1>
carrier_work_package_residue_reconciliation_fault_point(
    std::optional<DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint>
        point) noexcept {
    if (!point.has_value()) {
        return std::nullopt;
    }
    switch (*point) {
    case DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint::AfterNameUnlinked:
        return work_package_file::
            DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::after_name_unlinked;
    case DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint::AfterDirectoryDurable:
        return work_package_file::
            DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
                after_directory_durable;
    case DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint::Count:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic work_package_residue_reconciliation_failure(
    const work_package_file::DistributedSieveWorkerWorkPackageResidueReconciliationResultV1&
        lower) noexcept {
    using FileStatus = work_package_file::DistributedSieveWorkerWorkPackageFileStatus;
    DistributedSieveWaveStoreStatus status = DistributedSieveWaveStoreStatus::unexpected_failure;
    switch (lower.diagnostic.status) {
    case FileStatus::ready:
        status = DistributedSieveWaveStoreStatus::unexpected_failure;
        break;
    case FileStatus::interrupted:
        status = DistributedSieveWaveStoreStatus::interrupted;
        break;
    case FileStatus::invalid_request:
        status = DistributedSieveWaveStoreStatus::invalid_request;
        break;
    case FileStatus::platform_unavailable:
        status = DistributedSieveWaveStoreStatus::platform_unsupported;
        break;
    case FileStatus::resource_exhausted:
        status = DistributedSieveWaveStoreStatus::resource_exhausted;
        break;
    case FileStatus::namespace_conflict:
    case FileStatus::decode_failed:
        status = DistributedSieveWaveStoreStatus::namespace_conflict;
        break;
    case FileStatus::publication_failed:
        status = DistributedSieveWaveStoreStatus::publication_failed;
        break;
    case FileStatus::durability_failed:
        status = DistributedSieveWaveStoreStatus::durability_failed;
        break;
    case FileStatus::close_failed:
        status = DistributedSieveWaveStoreStatus::io_failed;
        break;
    case FileStatus::ops_contract_violation:
    case FileStatus::unexpected_failure:
        status = DistributedSieveWaveStoreStatus::unexpected_failure;
        break;
    }
    auto failure = diagnostic(status, lower.diagnostic.native_error != 0
                                          ? posix_error(lower.diagnostic.native_error)
                                          : protocol_error());
    if (lower.diagnostic.protocol_status.error != DistributedSieveProtocolError::none) {
        failure.protocol_status = lower.diagnostic.protocol_status;
    }
    failure.last_worker_work_package_residue_reconciliation_fault_point =
        wave_work_package_residue_reconciliation_fault_point(lower.fault_point);
    return failure;
}

[[nodiscard]] bool preselected_work_package_directory_sync_failure(void* context) noexcept {
    return context != nullptr && *static_cast<const bool*>(context);
}

[[nodiscard]] PrivateLeaseAttemptValidationResult
validate_private_lease_attempt_inventory(int root_fd, const std::filesystem::path& absolute_root,
                                         const PrivateLeaseAttemptInventory& attempt,
                                         const WaveManifestV1& manifest,
                                         std::uint64_t creator_process_id) noexcept {
    const auto fail_with = [](DistributedSieveWaveStoreDiagnostic failure) {
        return PrivateLeaseAttemptValidationResult{std::nullopt, std::move(failure)};
    };
    const auto conflict = [&] {
        return fail_with(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    };
    try {
        PrivateLeaseReservationWitness witness{
            .base_lock_leaf = attempt.names.base_lock_leaf,
        };
        const bool any_root_protocol = attempt.reserved || attempt.reserved_pending ||
                                       attempt.owned || attempt.owned_pending ||
                                       attempt.final_directory ||
                                       attempt.staging_directory_leaf.has_value();
        if (!any_root_protocol) {
            return {std::move(witness), {}};
        }

        const bool canonical_handoff_root_shape = !attempt.reserved && !attempt.reserved_pending &&
                                                  attempt.owned && !attempt.owned_pending &&
                                                  attempt.final_directory &&
                                                  !attempt.staging_directory_leaf.has_value();
        if (canonical_handoff_root_shape) {
            auto directory = inspect_private_lease_directory_at(
                root_fd, attempt.names.private_directory_leaf, creator_process_id);
            if (!directory) {
                return fail_with(directory.diagnostic);
            }
            const auto& entries = *directory.inventory;
            const bool exact_directory_shape =
                entries.owner && !entries.owner_pending && !entries.work_package_residue &&
                entries.corpus_index && entries.corpus_data && entries.worker_handoff &&
                !entries.worker_handoff_pending;
            if (!exact_directory_shape) {
                return conflict();
            }

            auto handoff = validate_worker_handoff_inventory(
                absolute_root, attempt, manifest, *directory.identity, creator_process_id);
            if (!handoff) {
                return fail_with(std::move(handoff.diagnostic));
            }
            witness.boundary =
                DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable;
            witness.lease_id = handoff.witness->handoff.lease.lease_id.limbs;
            witness.directory_identity = *directory.identity;
            witness.owner_marker_identity = handoff.witness->handoff.lease.owner_marker;
            witness.owned_marker_identity = handoff.witness->owned_marker_identity;
            witness.worker_handoff = std::move(*handoff.witness);
            if (const auto rebound = validate_private_lease_directory_binding(
                    root_fd, directory.directory.get(), attempt.names.private_directory_leaf,
                    creator_process_id, *directory.identity);
                rebound.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(rebound);
            }
            return {std::move(witness), {}};
        }

        if (attempt.reserved == attempt.reserved_pending) {
            return conflict();
        }

        const auto read_if_present = [&](bool present,
                                         const std::string& leaf) -> PrivateLeaseMarkerAtResult {
            if (!present) {
                return {std::nullopt, std::nullopt,
                        diagnostic(DistributedSieveWaveStoreStatus::ready)};
            }
            return read_private_lease_marker_at(root_fd, leaf, creator_process_id);
        };
        const auto reserved = read_if_present(attempt.reserved, attempt.names.reserved_leaf);
        if (attempt.reserved && !reserved) {
            return fail_with(reserved.diagnostic);
        }
        const auto reserved_pending =
            read_if_present(attempt.reserved_pending, attempt.names.reserved_pending_leaf);
        if (attempt.reserved_pending && !reserved_pending) {
            return fail_with(reserved_pending.diagnostic);
        }
        const private_lease::PrivateLeaseRecord& reserved_record =
            attempt.reserved ? *reserved.record : *reserved_pending.record;

        const std::filesystem::path base_path =
            absolute_root / attempt.names.private_directory_leaf / "corpus";
        struct stat root_metadata{};
        if (fstat_retrying_eintr(root_fd, root_metadata) != 0) {
            return fail_with(
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno)));
        }
        const private_lease::PrivateLeaseRecord exact_reserved{
            .platform_id = private_lease::PLATFORM_ID,
            .phase = private_lease::PrivateLeasePhase::Reserved,
            .capability = private_lease::PrivateLeaseCapability::RollbackStagingOnly,
            .lease_id = reserved_record.lease_id,
            .base_path_digest = private_lease::frozen_path_digest(base_path),
            .parent_identity = relation_identity(protocol_identity(root_metadata)),
            .lock_identity = relation_identity(attempt.base_lock_identity),
        };
        if (reserved_record != exact_reserved) {
            return conflict();
        }
        witness.lease_id = reserved_record.lease_id;
        witness.reserved_marker_identity =
            attempt.reserved ? reserved.identity : reserved_pending.identity;

        if (attempt.staging_directory_leaf.has_value() &&
            (!attempt.staging_lease_id.has_value() ||
             *attempt.staging_lease_id != reserved_record.lease_id)) {
            return conflict();
        }
        if (attempt.final_directory && attempt.staging_directory_leaf.has_value()) {
            return conflict();
        }
        if (attempt.reserved_pending &&
            (attempt.final_directory || attempt.staging_directory_leaf.has_value() ||
             attempt.owned || attempt.owned_pending)) {
            return conflict();
        }

        const bool directory_present =
            attempt.final_directory || attempt.staging_directory_leaf.has_value();
        if (!directory_present) {
            if (attempt.owned || attempt.owned_pending) {
                return conflict();
            }
            witness.boundary =
                attempt.reserved
                    ? DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable
                    : DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable;
            return {std::move(witness), {}};
        }

        const std::string& directory_leaf = attempt.final_directory
                                                ? attempt.names.private_directory_leaf
                                                : *attempt.staging_directory_leaf;
        auto directory =
            inspect_private_lease_directory_at(root_fd, directory_leaf, creator_process_id);
        if (!directory) {
            return fail_with(directory.diagnostic);
        }
        witness.directory_identity = directory.identity;
        const auto& directory_inventory = *directory.inventory;
        const bool any_handoff_artifact =
            directory_inventory.corpus_index || directory_inventory.corpus_data ||
            directory_inventory.worker_handoff || directory_inventory.worker_handoff_pending;
        if (any_handoff_artifact) {
            return conflict();
        }
        if (directory_inventory.work_package_residue && !attempt.final_directory) {
            return conflict();
        }
        if (directory_inventory.owner && directory_inventory.owner_pending) {
            return conflict();
        }
        if (!directory_inventory.owner && !directory_inventory.owner_pending) {
            if (attempt.final_directory || attempt.owned || attempt.owned_pending) {
                return conflict();
            }
            witness.boundary =
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable;
            return {std::move(witness), {}};
        }

        const auto owner =
            directory_inventory.owner
                ? read_private_lease_marker_at(
                      directory.directory.get(),
                      std::string(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF), creator_process_id)
                : PrivateLeaseMarkerAtResult{
                      std::nullopt,
                      std::nullopt,
                      diagnostic(DistributedSieveWaveStoreStatus::ready),
                  };
        if (directory_inventory.owner && !owner) {
            return fail_with(owner.diagnostic);
        }
        const auto owner_pending =
            directory_inventory.owner_pending
                ? read_private_lease_marker_at(
                      directory.directory.get(),
                      std::string(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF),
                      creator_process_id)
                : PrivateLeaseMarkerAtResult{
                      std::nullopt,
                      std::nullopt,
                      diagnostic(DistributedSieveWaveStoreStatus::ready),
                  };
        if (directory_inventory.owner_pending && !owner_pending) {
            return fail_with(owner_pending.diagnostic);
        }
        const auto expected_owner = private_lease::make_private_lease_owner_record(
            reserved_record, relation_identity(*directory.identity));
        if ((directory_inventory.owner && *owner.record != expected_owner) ||
            (directory_inventory.owner_pending && *owner_pending.record != expected_owner)) {
            return conflict();
        }
        witness.owner_marker_identity =
            directory_inventory.owner ? owner.identity : owner_pending.identity;

        if (!directory_inventory.owner) {
            if (attempt.final_directory || attempt.owned || attempt.owned_pending) {
                return conflict();
            }
            witness.boundary = DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable;
            return {std::move(witness), {}};
        }

        if (attempt.owned && attempt.owned_pending) {
            return conflict();
        }
        const auto owned = read_if_present(attempt.owned, attempt.names.owned_leaf);
        if (attempt.owned && !owned) {
            return fail_with(owned.diagnostic);
        }
        const auto owned_pending =
            read_if_present(attempt.owned_pending, attempt.names.owned_pending_leaf);
        if (attempt.owned_pending && !owned_pending) {
            return fail_with(owned_pending.diagnostic);
        }
        if (!attempt.owned && !attempt.owned_pending) {
            if (attempt.final_directory) {
                return conflict();
            }
            witness.boundary =
                DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable;
            return {std::move(witness), {}};
        }
        const auto expected_owned = private_lease::make_private_lease_owned_record(
            expected_owner, relation_identity(*owner.identity));
        if ((attempt.owned && *owned.record != expected_owned) ||
            (attempt.owned_pending && *owned_pending.record != expected_owned)) {
            return conflict();
        }
        witness.owned_marker_identity = attempt.owned ? owned.identity : owned_pending.identity;
        if (attempt.final_directory && !attempt.owned) {
            return conflict();
        }
        if (attempt.final_directory) {
            witness.boundary =
                DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable;
            if (directory_inventory.work_package_residue) {
                auto inspected =
                    work_package_file::inspect_distributed_sieve_worker_work_package_residue_v1({
                        .borrowed_attempt_directory_handle = static_cast<
                            work_package_file::DistributedSieveWorkerWorkPackageNativeHandle>(
                            directory.directory.get()),
                        .expected_directory_identity = *directory.identity,
                        .observer_process_id = creator_process_id,
                    });
                if (!inspected) {
                    return fail_with(work_package_residue_inspection_failure(inspected.diagnostic));
                }
                const auto work_digest = distributed_sieve_work_digest(inspected.witness->identity);
                const auto manifest_status =
                    validate_manifest_work_identity(manifest, inspected.witness->identity);
                if (!work_digest || *work_digest.digest != manifest.work_sha256 ||
                    !manifest_status ||
                    inspected.witness->package.work_sha256 != manifest.work_sha256 ||
                    inspected.witness->package.total_bytes != inspected.witness->file_extent) {
                    const auto protocol_status =
                        !work_digest
                            ? work_digest.status
                            : (!manifest_status
                                   ? manifest_status
                                   : DistributedSieveProtocolStatus{
                                         .error = DistributedSieveProtocolError::digest_mismatch,
                                     });
                    const auto status =
                        protocol_status.error == DistributedSieveProtocolError::resource_exhausted
                            ? DistributedSieveWaveStoreStatus::resource_exhausted
                            : DistributedSieveWaveStoreStatus::namespace_conflict;
                    auto failure = diagnostic(status, protocol_error());
                    failure.protocol_status = protocol_status;
                    return fail_with(std::move(failure));
                }
                witness.work_package_residue =
                    compact_work_package_residue_witness(*inspected.witness);
            }
        } else if (attempt.owned) {
            witness.boundary =
                DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable;
        } else {
            witness.boundary = DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable;
        }
        if (const auto rebound = validate_private_lease_directory_binding(
                root_fd, directory.directory.get(), directory_leaf, creator_process_id,
                *directory.identity);
            rebound.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(rebound);
        }
        return {std::move(witness), {}};
    } catch (const std::bad_alloc&) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                    std::make_error_code(std::errc::not_enough_memory)));
    } catch (...) {
        return conflict();
    }
}

struct PrivateLeaseProtocolInventoryValidationResult final {
    std::optional<std::vector<PrivateLeaseReservationWitness>> witnesses;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return witnesses.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] PrivateLeaseProtocolInventoryValidationResult
validate_private_lease_protocol_inventory(int root_fd, const std::filesystem::path& absolute_root,
                                          const NamespaceInventory& inventory,
                                          const WaveManifestV1& manifest,
                                          std::span<const NativeIdentityV1> base_lock_identities,
                                          std::uint64_t creator_process_id) noexcept {
    if (inventory.private_lease_base_lock_leaves.size() != base_lock_identities.size()) {
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error())};
    }
    const auto fail_with = [](DistributedSieveWaveStoreDiagnostic failure) {
        return PrivateLeaseProtocolInventoryValidationResult{std::nullopt, std::move(failure)};
    };
    const auto conflict = [&] {
        return fail_with(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    };
    std::vector<PrivateLeaseAttemptInventory> attempts;
    try {
        attempts.reserve(inventory.private_lease_base_lock_leaves.size());
        for (std::size_t index = 0; index < inventory.private_lease_base_lock_leaves.size();
             ++index) {
            const auto& base_lock_leaf = inventory.private_lease_base_lock_leaves[index];
            if (!base_lock_leaf.ends_with(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX)) {
                return conflict();
            }
            const auto relative_stem =
                std::string_view(base_lock_leaf)
                    .substr(0, base_lock_leaf.size() -
                                   DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX.size());
            auto names = manifest_attempt_names_from_relative_stem(manifest, relative_stem);
            if (!names.has_value() || names->base_lock_leaf != base_lock_leaf) {
                return conflict();
            }
            attempts.push_back(PrivateLeaseAttemptInventory{
                .names = std::move(*names),
                .base_lock_identity = base_lock_identities[index],
            });
        }

        for (const auto& leaf : inventory.private_lease_protocol_leaves) {
            auto parsed = parse_manifest_bound_private_lease_protocol_leaf(manifest, leaf);
            if (!parsed.has_value()) {
                return conflict();
            }
            const auto position = std::lower_bound(
                attempts.begin(), attempts.end(), parsed->names.base_lock_leaf,
                [](const PrivateLeaseAttemptInventory& candidate, const std::string& base_lock) {
                    return candidate.names.base_lock_leaf < base_lock;
                });
            if (position == attempts.end() ||
                position->names.base_lock_leaf != parsed->names.base_lock_leaf) {
                return conflict();
            }
            switch (parsed->role) {
            case PrivateLeaseProtocolLeafRole::reserved:
                if (position->reserved) {
                    return conflict();
                }
                position->reserved = true;
                break;
            case PrivateLeaseProtocolLeafRole::reserved_pending:
                if (position->reserved_pending) {
                    return conflict();
                }
                position->reserved_pending = true;
                break;
            case PrivateLeaseProtocolLeafRole::owned:
                if (position->owned) {
                    return conflict();
                }
                position->owned = true;
                break;
            case PrivateLeaseProtocolLeafRole::owned_pending:
                if (position->owned_pending) {
                    return conflict();
                }
                position->owned_pending = true;
                break;
            case PrivateLeaseProtocolLeafRole::staging_directory:
                if (position->staging_directory_leaf.has_value() ||
                    !parsed->staging_lease_id.has_value()) {
                    return conflict();
                }
                position->staging_directory_leaf = leaf;
                position->staging_lease_id = parsed->staging_lease_id;
                break;
            case PrivateLeaseProtocolLeafRole::final_directory:
                if (position->final_directory) {
                    return conflict();
                }
                position->final_directory = true;
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                    std::make_error_code(std::errc::not_enough_memory)));
    } catch (...) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                    std::make_error_code(std::errc::io_error)));
    }

    std::vector<PrivateLeaseReservationWitness> witnesses;
    try {
        witnesses.reserve(attempts.size());
    } catch (const std::bad_alloc&) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                    std::make_error_code(std::errc::not_enough_memory)));
    } catch (...) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                    std::make_error_code(std::errc::io_error)));
    }
    for (const auto& attempt : attempts) {
        auto validated = validate_private_lease_attempt_inventory(root_fd, absolute_root, attempt,
                                                                  manifest, creator_process_id);
        if (!validated) {
            return fail_with(std::move(validated.diagnostic));
        }
        try {
            witnesses.push_back(std::move(*validated.witness));
        } catch (const std::bad_alloc&) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                        std::make_error_code(std::errc::not_enough_memory)));
        } catch (...) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                        std::make_error_code(std::errc::io_error)));
        }
    }
    return {std::move(witnesses), {}};
}

struct WorkerAttemptRecordInventoryValidationResult final {
    std::optional<std::vector<DistributedSieveWorkerAttemptRecordInventoryWitness>> witnesses;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return witnesses.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] bool attempt_lease_identities_conflict(const LeaseIdentityV1& left,
                                                     const LeaseIdentityV1& right) noexcept {
    return left.lease_id == right.lease_id || left.owner_marker == right.owner_marker ||
           left.owner_marker == right.directory || left.directory == right.owner_marker ||
           left.directory == right.directory;
}

[[nodiscard]] bool attempt_record_matches_live_lease_projection(
    const AttemptStartedV1& record,
    const DistributedSievePrivateLeaseReservationInventoryWitness& lease) noexcept {
    if (lease.worker_handoff.has_value()) {
        return lease.worker_handoff->handoff.attempt_started_digest == record.self_digest &&
               lease.worker_handoff->handoff.lease == record.lease &&
               lease.lease_id == record.lease.lease_id.limbs &&
               lease.directory_identity == record.lease.directory &&
               lease.owner_marker_identity == record.lease.owner_marker;
    }
    if (lease.boundary == DistributedSievePrivateLeaseReservationBoundary::PermitAcquired) {
        return true;
    }
    if (lease.lease_id != record.lease.lease_id.limbs) {
        return false;
    }
    if (lease.directory_identity.has_value() &&
        *lease.directory_identity != record.lease.directory) {
        return false;
    }
    return !lease.owner_marker_identity.has_value() ||
           *lease.owner_marker_identity == record.lease.owner_marker;
}

[[nodiscard]] WorkerAttemptRecordInventoryValidationResult validate_worker_attempt_record_inventory(
    int root_fd, const NamespaceInventory& inventory, const WaveManifestV1& manifest,
    std::span<const NativeIdentityV1> base_lock_identities,
    std::span<const DistributedSievePrivateLeaseReservationInventoryWitness> private_leases,
    std::uint64_t creator_process_id) noexcept {
    const auto fail_with = [](DistributedSieveWaveStoreDiagnostic failure) {
        return WorkerAttemptRecordInventoryValidationResult{std::nullopt, std::move(failure)};
    };
    const auto conflict = [&] {
        return fail_with(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    };
    const auto protocol_conflict = [&](DistributedSieveProtocolStatus status) {
        auto failure =
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
        failure.protocol_status = status;
        return fail_with(std::move(failure));
    };

    if (inventory.private_lease_base_lock_leaves.size() != base_lock_identities.size() ||
        inventory.private_lease_base_lock_leaves.size() != private_leases.size()) {
        return fail_with(
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error()));
    }

    std::vector<DistributedSieveWorkerAttemptRecordInventoryWitness> records;
    try {
        records.reserve(inventory.worker_attempt_record_leaves.size());
        for (const auto& leaf : inventory.worker_attempt_record_leaves) {
            const auto parsed = parse_distributed_sieve_worker_attempt_leaf_v1(leaf);
            if (!parsed.has_value() || parsed->attempt_ordinal >= manifest.max_worker_attempts) {
                return conflict();
            }
            const ChunkPlanV1* chunk = nullptr;
            for (const auto& candidate : manifest.chunks) {
                if (candidate.chunk_id == parsed->chunk_id) {
                    chunk = &candidate;
                    break;
                }
            }
            if (chunk == nullptr || chunk->sq_begin >= chunk->sq_end) {
                return conflict();
            }
            const auto names = distributed_sieve_worker_attempt_names_v1(
                chunk->relative_artifact_stem, parsed->chunk_id, parsed->attempt_ordinal);
            if (!names.has_value() || leaf != (parsed->pending ? names->pending_record_leaf
                                                               : names->canonical_record_leaf)) {
                return conflict();
            }

            auto loaded = read_immutable_protocol_record_leaf(
                root_fd, leaf.c_str(), creator_process_id,
                DistributedSieveWaveStoreStatus::namespace_conflict,
                DistributedSieveWaveStoreStatus::namespace_conflict);
            if (!loaded) {
                return fail_with(std::move(loaded.diagnostic));
            }
            auto decoded = decode_distributed_sieve_record(*loaded.bytes);
            if (!decoded) {
                return protocol_conflict(decoded.status);
            }
            auto* attempt = std::get_if<AttemptStartedV1>(&*decoded.value);
            if (attempt == nullptr || attempt->chunk_id != parsed->chunk_id ||
                attempt->attempt_ordinal != parsed->attempt_ordinal ||
                attempt->manifest_digest != manifest.self_digest ||
                attempt->sq_begin != chunk->sq_begin || attempt->sq_end != chunk->sq_end ||
                attempt->retry_policy_version != manifest.retry_policy_version ||
                attempt->lease.relative_stem != names->relative_lease_stem) {
                return conflict();
            }

            const auto position = std::lower_bound(
                records.begin(), records.end(),
                std::pair{parsed->chunk_id, parsed->attempt_ordinal},
                [](const DistributedSieveWorkerAttemptRecordInventoryWitness& candidate,
                   const std::pair<std::uint32_t, std::uint32_t>& coordinate) {
                    return std::pair{candidate.chunk_id, candidate.attempt_ordinal} < coordinate;
                });
            if (position == records.end() || position->chunk_id != parsed->chunk_id ||
                position->attempt_ordinal != parsed->attempt_ordinal) {
                DistributedSieveWorkerAttemptRecordInventoryWitness witness{
                    .chunk_id = parsed->chunk_id,
                    .attempt_ordinal = parsed->attempt_ordinal,
                    .record = std::move(*attempt),
                    .bytes = std::move(*loaded.bytes),
                };
                if (parsed->pending) {
                    witness.pending_snapshot = *loaded.snapshot;
                } else {
                    witness.canonical_snapshot = *loaded.snapshot;
                }
                records.insert(position, std::move(witness));
                continue;
            }
            if (position->bytes != *loaded.bytes ||
                (parsed->pending ? position->pending_snapshot.has_value()
                                 : position->canonical_snapshot.has_value())) {
                return conflict();
            }
            if (parsed->pending) {
                position->pending_snapshot = *loaded.snapshot;
            } else {
                position->canonical_snapshot = *loaded.snapshot;
            }
        }
    } catch (const std::bad_alloc&) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                    std::make_error_code(std::errc::not_enough_memory)));
    } catch (...) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                    std::make_error_code(std::errc::io_error)));
    }

    for (std::size_t left = 0; left < records.size(); ++left) {
        for (const auto& base_lock_identity : base_lock_identities) {
            if (records[left].record.lease.owner_marker == base_lock_identity ||
                records[left].record.lease.directory == base_lock_identity) {
                return conflict();
            }
        }
        for (std::size_t right = left + 1U; right < records.size(); ++right) {
            if (attempt_lease_identities_conflict(records[left].record.lease,
                                                  records[right].record.lease)) {
                return conflict();
            }
        }
    }

    for (const auto& chunk : manifest.chunks) {
        std::vector<AttemptStartedV1> canonical_chain;
        const DistributedSieveWorkerAttemptRecordInventoryWitness* pending = nullptr;
        try {
            canonical_chain.reserve(manifest.max_worker_attempts);
        } catch (const std::bad_alloc&) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                        std::make_error_code(std::errc::not_enough_memory)));
        } catch (...) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                        std::make_error_code(std::errc::io_error)));
        }

        for (const auto& record : records) {
            if (record.chunk_id != chunk.chunk_id) {
                continue;
            }
            if (record.canonical_snapshot.has_value()) {
                if (record.attempt_ordinal != canonical_chain.size()) {
                    return conflict();
                }
                try {
                    canonical_chain.push_back(record.record);
                } catch (const std::bad_alloc&) {
                    return fail_with(
                        diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                   std::make_error_code(std::errc::not_enough_memory)));
                } catch (...) {
                    return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                                std::make_error_code(std::errc::io_error)));
                }
            }
            if (record.pending_snapshot.has_value()) {
                if (pending != nullptr) {
                    return conflict();
                }
                pending = &record;
            }
        }

        const WorkerHandoffV1* worker_handoff = nullptr;
        for (const auto& lease : private_leases) {
            if (!lease.worker_handoff.has_value() ||
                lease.worker_handoff->handoff.chunk_id != chunk.chunk_id) {
                continue;
            }
            if (worker_handoff != nullptr) {
                return conflict();
            }
            worker_handoff = &lease.worker_handoff->handoff;
        }
        if (worker_handoff != nullptr && pending != nullptr) {
            return conflict();
        }
        if (worker_handoff != nullptr) {
            for (const auto& lease : private_leases) {
                const auto coordinate =
                    manifest_worker_attempt_coordinate_from_private_lease_base_lock(
                        manifest, lease.base_lock_leaf);
                if (!coordinate.has_value()) {
                    return conflict();
                }
                if (coordinate->chunk_id == chunk.chunk_id &&
                    coordinate->attempt_ordinal > worker_handoff->attempt_ordinal) {
                    return conflict();
                }
            }
        }

        if (!canonical_chain.empty() || worker_handoff != nullptr) {
            const auto status = validate_worker_attempt_chain(
                manifest, chunk.chunk_id, canonical_chain, worker_handoff, nullptr);
            if (!status) {
                return protocol_conflict(status);
            }
        }
        if (pending != nullptr) {
            if (pending->canonical_snapshot.has_value()) {
                if (canonical_chain.empty() ||
                    pending->attempt_ordinal + 1U != canonical_chain.size()) {
                    return conflict();
                }
            } else {
                if (pending->attempt_ordinal != canonical_chain.size()) {
                    return conflict();
                }
                try {
                    canonical_chain.push_back(pending->record);
                } catch (const std::bad_alloc&) {
                    return fail_with(
                        diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                   std::make_error_code(std::errc::not_enough_memory)));
                } catch (...) {
                    return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                                std::make_error_code(std::errc::io_error)));
                }
                const auto status = validate_worker_attempt_chain(
                    manifest, chunk.chunk_id, canonical_chain, nullptr, nullptr);
                if (!status) {
                    return protocol_conflict(status);
                }
            }
        }

        const std::size_t canonical_count =
            canonical_chain.size() -
            static_cast<std::size_t>(pending != nullptr &&
                                     !pending->canonical_snapshot.has_value());
        const bool next_attempt_is_pending =
            pending != nullptr && !pending->canonical_snapshot.has_value();
        for (const auto& record : records) {
            if (record.chunk_id != chunk.chunk_id) {
                continue;
            }
            const auto names = distributed_sieve_worker_attempt_names_v1(
                chunk.relative_artifact_stem, chunk.chunk_id, record.attempt_ordinal);
            if (!names.has_value()) {
                return conflict();
            }
            const auto lock_position = std::lower_bound(
                inventory.private_lease_base_lock_leaves.begin(),
                inventory.private_lease_base_lock_leaves.end(), names->base_lock_leaf);
            if (lock_position == inventory.private_lease_base_lock_leaves.end() ||
                *lock_position != names->base_lock_leaf) {
                return conflict();
            }
            const auto lock_index = static_cast<std::size_t>(
                std::distance(inventory.private_lease_base_lock_leaves.begin(), lock_position));
            const auto& lease = private_leases[lock_index];
            if (!attempt_record_matches_live_lease_projection(record.record, lease)) {
                return conflict();
            }

            const bool pending_only =
                record.pending_snapshot.has_value() && !record.canonical_snapshot.has_value();
            if (pending_only &&
                lease.boundary !=
                    DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable) {
                return conflict();
            }
            const bool historical_canonical =
                record.canonical_snapshot.has_value() &&
                (next_attempt_is_pending ? record.attempt_ordinal < canonical_count
                                         : record.attempt_ordinal + 1U < canonical_count);
            if (historical_canonical &&
                lease.boundary != DistributedSievePrivateLeaseReservationBoundary::PermitAcquired) {
                return conflict();
            }
        }
    }

    for (const auto& lease : private_leases) {
        if (!lease.work_package_residue.has_value()) {
            continue;
        }
        if (lease.boundary !=
                DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable ||
            !lease.directory_identity.has_value() || !lease.owner_marker_identity.has_value() ||
            !lease.owned_marker_identity.has_value() ||
            lease.work_package_residue->work_sha256 != manifest.work_sha256 ||
            !lease.base_lock_leaf.ends_with(DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX)) {
            return conflict();
        }
        const auto relative_stem =
            std::string_view(lease.base_lock_leaf)
                .substr(0, lease.base_lock_leaf.size() -
                               DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX.size());
        const auto names = manifest_attempt_names_from_relative_stem(manifest, relative_stem);
        if (!names.has_value() || names->base_lock_leaf != lease.base_lock_leaf) {
            return conflict();
        }
        const auto parsed =
            parse_distributed_sieve_worker_attempt_leaf_v1(names->canonical_record_leaf);
        if (!parsed.has_value() || parsed->pending) {
            return conflict();
        }
        const auto record = std::lower_bound(
            records.begin(), records.end(), std::pair{parsed->chunk_id, parsed->attempt_ordinal},
            [](const DistributedSieveWorkerAttemptRecordInventoryWitness& candidate,
               const std::pair<std::uint32_t, std::uint32_t>& coordinate) {
                return std::pair{candidate.chunk_id, candidate.attempt_ordinal} < coordinate;
            });
        if (record == records.end() || record->chunk_id != parsed->chunk_id ||
            record->attempt_ordinal != parsed->attempt_ordinal ||
            !record->canonical_snapshot.has_value() || record->pending_snapshot.has_value() ||
            record->record.chunk_id != parsed->chunk_id ||
            record->record.attempt_ordinal != parsed->attempt_ordinal ||
            record->record.manifest_digest != manifest.self_digest ||
            record->record.lease.lease_id.limbs != lease.lease_id ||
            record->record.lease.owner_marker != *lease.owner_marker_identity ||
            record->record.lease.directory != *lease.directory_identity ||
            record->record.lease.relative_stem != names->relative_lease_stem) {
            return conflict();
        }
        const auto later =
            std::find_if(std::next(record), records.end(),
                         [&](const DistributedSieveWorkerAttemptRecordInventoryWitness& candidate) {
                             return candidate.chunk_id == parsed->chunk_id;
                         });
        if (later != records.end()) {
            return conflict();
        }
        for (const auto& candidate_lease : private_leases) {
            if (!candidate_lease.base_lock_leaf.ends_with(
                    DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX)) {
                return conflict();
            }
            const auto candidate_relative_stem =
                std::string_view(candidate_lease.base_lock_leaf)
                    .substr(0, candidate_lease.base_lock_leaf.size() -
                                   DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX.size());
            const auto candidate_names =
                manifest_attempt_names_from_relative_stem(manifest, candidate_relative_stem);
            if (!candidate_names.has_value()) {
                return conflict();
            }
            const auto candidate = parse_distributed_sieve_worker_attempt_leaf_v1(
                candidate_names->canonical_record_leaf);
            if (!candidate.has_value() || candidate->pending) {
                return conflict();
            }
            if (candidate->chunk_id == parsed->chunk_id &&
                candidate->attempt_ordinal > parsed->attempt_ordinal) {
                return conflict();
            }
        }
    }
    return {std::move(records), {}};
}

struct ManifestBoundInventoryValidationResult final {
    std::optional<std::vector<NativeIdentityV1>> base_lock_identities;
    std::optional<std::vector<PrivateLeaseReservationWitness>> private_lease_witnesses;
    std::optional<std::vector<DistributedSieveWorkerAttemptRecordInventoryWitness>>
        worker_attempt_records;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return base_lock_identities.has_value() && private_lease_witnesses.has_value() &&
               worker_attempt_records.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] ManifestBoundInventoryValidationResult validate_manifest_bound_inventory(
    int root_fd, const std::filesystem::path& absolute_root, const NamespaceInventory& inventory,
    const WaveManifestV1& manifest, std::uint64_t creator_process_id) noexcept {
    if (!inventory.lock || !inventory.manifest || inventory.pending) {
        return {std::nullopt, std::nullopt, std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    auto base_locks = validate_private_lease_base_lock_inventory(root_fd, inventory, manifest,
                                                                 creator_process_id);
    if (!base_locks) {
        return {std::nullopt, std::nullopt, std::nullopt, std::move(base_locks.diagnostic)};
    }
    auto private_leases = validate_private_lease_protocol_inventory(
        root_fd, absolute_root, inventory, manifest, *base_locks.identities, creator_process_id);
    if (!private_leases) {
        return {std::nullopt, std::nullopt, std::nullopt, std::move(private_leases.diagnostic)};
    }
    auto worker_attempts = validate_worker_attempt_record_inventory(
        root_fd, inventory, manifest, *base_locks.identities, *private_leases.witnesses,
        creator_process_id);
    if (!worker_attempts) {
        return {std::nullopt, std::nullopt, std::nullopt, std::move(worker_attempts.diagnostic)};
    }
    return {std::move(base_locks.identities),
            std::move(private_leases.witnesses),
            std::move(worker_attempts.witnesses),
            {}};
}

struct ManifestBoundInventoryWitnessResult final {
    std::optional<NamespaceInventory> inventory;
    std::optional<std::vector<NativeIdentityV1>> base_lock_identities;
    std::optional<std::vector<PrivateLeaseReservationWitness>> private_lease_witnesses;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return inventory.has_value() && base_lock_identities.has_value() &&
               private_lease_witnesses.has_value() &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] ManifestBoundInventoryWitnessResult
capture_manifest_bound_inventory_witness(int root_fd, const WaveManifestV1& manifest,
                                         const std::filesystem::path& absolute_root,
                                         std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, std::nullopt, std::nullopt, process_mismatch()};
    }
    auto inventory = inspect_namespace(root_fd);
    if (!inventory) {
        return {std::nullopt, std::nullopt, std::nullopt, std::move(inventory.diagnostic)};
    }
    auto validated = validate_manifest_bound_inventory(root_fd, absolute_root, *inventory.inventory,
                                                       manifest, creator_process_id);
    if (!validated) {
        return {std::nullopt, std::nullopt, std::nullopt, std::move(validated.diagnostic)};
    }
    if (!process_matches(creator_process_id)) {
        return {std::nullopt, std::nullopt, std::nullopt, process_mismatch()};
    }
    inventory.inventory->worker_attempt_records = std::move(*validated.worker_attempt_records);
    return {
        std::move(inventory.inventory),
        std::move(validated.base_lock_identities),
        std::move(validated.private_lease_witnesses),
        {},
    };
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
        (before.manifest || before.pending || !before.private_lease_base_lock_leaves.empty() ||
         !before.private_lease_protocol_leaves.empty() ||
         !before.worker_attempt_record_leaves.empty())) {
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

struct WorkerAttemptStartHookBridge final {
    DistributedSieveWorkerAttemptStartTestHooks hooks;
    std::uint64_t creator_process_id = 0;
    bool process_changed = false;
    std::optional<DistributedSieveWorkerAttemptStartFaultPoint> last_fault_point;
};

struct WorkerAttemptReconcileHookBridge final {
    DistributedSieveWorkerAttemptReconcileTestHooks hooks;
    std::uint64_t creator_process_id = 0;
    bool process_changed = false;
    std::optional<DistributedSieveWorkerAttemptReconcileFaultPoint> last_fault_point;
};

[[nodiscard]] bool bridge_worker_attempt_start_hook(durable_record::RecordFaultPoint point,
                                                    void* raw_context) noexcept {
    auto& context = *static_cast<WorkerAttemptStartHookBridge*>(raw_context);
    if (!process_matches(context.creator_process_id)) {
        context.process_changed = true;
        return true;
    }
    DistributedSieveWorkerAttemptStartFaultPoint mapped =
        DistributedSieveWorkerAttemptStartFaultPoint::Count;
    switch (point) {
    case durable_record::RecordFaultPoint::PendingDurable:
        mapped = DistributedSieveWorkerAttemptStartFaultPoint::PendingDurable;
        break;
    case durable_record::RecordFaultPoint::CanonicalPromoted:
        mapped = DistributedSieveWorkerAttemptStartFaultPoint::CanonicalPromoted;
        break;
    case durable_record::RecordFaultPoint::CanonicalDurable:
        mapped = DistributedSieveWorkerAttemptStartFaultPoint::CanonicalDurable;
        break;
    }
    if (mapped == DistributedSieveWorkerAttemptStartFaultPoint::Count) {
        return true;
    }
    context.last_fault_point = mapped;
    const bool interrupted = context.hooks.stop_after != nullptr &&
                             context.hooks.stop_after(mapped, context.hooks.context);
    if (!process_matches(context.creator_process_id)) {
        context.process_changed = true;
        return true;
    }
    return interrupted;
}

[[nodiscard]] bool bridge_worker_attempt_reconcile_hook(durable_record::RecordFaultPoint point,
                                                        void* raw_context) noexcept {
    auto& context = *static_cast<WorkerAttemptReconcileHookBridge*>(raw_context);
    if (!process_matches(context.creator_process_id)) {
        context.process_changed = true;
        return true;
    }
    DistributedSieveWorkerAttemptReconcileFaultPoint mapped =
        DistributedSieveWorkerAttemptReconcileFaultPoint::Count;
    switch (point) {
    case durable_record::RecordFaultPoint::PendingDurable:
        mapped = DistributedSieveWorkerAttemptReconcileFaultPoint::PendingDurable;
        break;
    case durable_record::RecordFaultPoint::CanonicalPromoted:
        mapped = DistributedSieveWorkerAttemptReconcileFaultPoint::CanonicalPromoted;
        break;
    case durable_record::RecordFaultPoint::CanonicalDurable:
        mapped = DistributedSieveWorkerAttemptReconcileFaultPoint::CanonicalDurable;
        break;
    }
    if (mapped == DistributedSieveWorkerAttemptReconcileFaultPoint::Count) {
        return true;
    }
    context.last_fault_point = mapped;
    const bool interrupted = context.hooks.stop_after != nullptr &&
                             context.hooks.stop_after(mapped, context.hooks.context);
    if (!process_matches(context.creator_process_id)) {
        context.process_changed = true;
        return true;
    }
    return interrupted;
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
worker_attempt_start_publication_diagnostic(const durable_record::RecordPublishResult& published,
                                            const WorkerAttemptStartHookBridge& bridge) noexcept {
    DistributedSieveWaveStoreDiagnostic outcome;
    outcome.publication_status = published.status();
    outcome.publication_disposition = published.disposition();
    outcome.native_error = published.native_error();
    outcome.last_worker_attempt_start_fault_point = bridge.last_fault_point;
    if (bridge.process_changed || !process_matches(bridge.creator_process_id)) {
        outcome.status = DistributedSieveWaveStoreStatus::invalid_request;
        outcome.native_error = invalid_argument_error();
        return outcome;
    }
    switch (published.status()) {
    case durable_record::RecordPublishStatus::durable:
        outcome.status = DistributedSieveWaveStoreStatus::ready;
        break;
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
        outcome.status = DistributedSieveWaveStoreStatus::namespace_conflict;
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
    return outcome;
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic worker_attempt_reconcile_publication_diagnostic(
    const durable_record::RecordPublishResult& published,
    const WorkerAttemptReconcileHookBridge& bridge) noexcept {
    DistributedSieveWaveStoreDiagnostic outcome;
    outcome.publication_status = published.status();
    outcome.publication_disposition = published.disposition();
    outcome.native_error = published.native_error();
    outcome.last_worker_attempt_reconcile_fault_point = bridge.last_fault_point;
    if (bridge.process_changed || !process_matches(bridge.creator_process_id)) {
        outcome.status = DistributedSieveWaveStoreStatus::invalid_request;
        outcome.native_error = invalid_argument_error();
        return outcome;
    }
    switch (published.status()) {
    case durable_record::RecordPublishStatus::durable:
        outcome.status = DistributedSieveWaveStoreStatus::ready;
        break;
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
        outcome.status = DistributedSieveWaveStoreStatus::namespace_conflict;
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
    return outcome;
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

struct PrivateLeaseMarkerIdentityResult final {
    std::optional<NativeIdentityV1> identity;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return identity.has_value() && diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] PrivateLeaseMarkerIdentityResult
inspect_published_private_lease_marker(int parent_fd, const char* leaf, int marker_fd) noexcept {
    struct stat held{};
    struct stat named{};
    if (fstat_retrying_eintr(marker_fd, held) != 0 ||
        fstatat_retrying_eintr(parent_fd, leaf, named) != 0) {
        return {std::nullopt, diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                         posix_error(errno))};
    }
    if (!valid_private_lease_marker_metadata(held) || !valid_private_lease_marker_metadata(named) ||
        !stable_metadata(held, named)) {
        return {std::nullopt,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    if (auto acl =
            acl_rejection(marker_fd, false, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return {std::nullopt, *acl};
    }
    return {protocol_identity(held), {}};
}

struct PrivateLeaseStrictMarkerPublishResult final {
    UniqueFd marker;
    std::optional<NativeIdentityV1> identity;
    DistributedSieveWaveStoreDiagnostic outcome;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(marker) && identity.has_value() &&
               outcome.status == DistributedSieveWaveStoreStatus::ready;
    }

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic close_checked() noexcept {
        const int descriptor = marker.release();
        if (descriptor < 0) {
            return diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                              protocol_error());
        }
        if (::close(descriptor) == 0) {
            return {};
        }
        return diagnostic(DistributedSieveWaveStoreStatus::publication_failed, posix_error(errno));
    }
};

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
private_lease_sync_handle(int descriptor) noexcept;

[[nodiscard]] DistributedSieveWaveStoreDiagnostic private_lease_reservation_sync_handle(
    int descriptor, DistributedSievePrivateLeaseReservationBoundary boundary,
    DistributedSievePrivateLeaseReservationSyncPoint point,
    std::optional<DistributedSievePrivateLeaseReservationSyncPoint> injected_failure) noexcept {
    if (const auto synchronized = private_lease_sync_handle(descriptor);
        synchronized.status != DistributedSieveWaveStoreStatus::ready) {
        return synchronized;
    }
    if (!injected_failure.has_value() || *injected_failure != point) {
        return {};
    }
    auto outcome = diagnostic(DistributedSieveWaveStoreStatus::durability_failed,
                              std::make_error_code(std::errc::io_error));
    outcome.failed_private_lease_reservation_sync_site =
        DistributedSievePrivateLeaseReservationSyncFailureSite{
            .boundary = boundary,
            .point = point,
        };
    return outcome;
}

[[nodiscard]] PrivateLeaseStrictMarkerPublishResult publish_private_lease_marker_strict_at(
    int parent_fd, const std::string& leaf, std::span<const std::byte> bytes,
    DistributedSievePrivateLeaseReservationBoundary boundary,
    std::optional<DistributedSievePrivateLeaseReservationSyncPoint>
        injected_sync_failure) noexcept {
    if (parent_fd < 0 || leaf.empty() || leaf == "." || leaf == ".." ||
        leaf.find('/') != std::string::npos || leaf.find('\0') != std::string::npos ||
        bytes.size() != private_lease::PRIVATE_LEASE_MARKER_BYTES) {
        return {
            {},
            std::nullopt,
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()),
        };
    }

    UniqueFd marker(openat_retrying_eintr(
        parent_fd, leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!marker) {
        const int saved_errno = errno;
        const auto status =
            saved_errno == EEXIST
                ? DistributedSieveWaveStoreStatus::namespace_conflict
                : (saved_errno == EMFILE || saved_errno == ENFILE || saved_errno == ENOMEM
                       ? DistributedSieveWaveStoreStatus::resource_exhausted
                       : DistributedSieveWaveStoreStatus::publication_failed);
        return {{}, std::nullopt, diagnostic(status, posix_error(saved_errno))};
    }
    auto fail_with = [&](DistributedSieveWaveStoreDiagnostic failure,
                         std::optional<NativeIdentityV1> identity = std::nullopt) mutable {
        return PrivateLeaseStrictMarkerPublishResult{std::move(marker), identity,
                                                     std::move(failure)};
    };

    int chmod_result = -1;
    do {
        chmod_result = ::fchmod(marker.get(), 0600);
    } while (chmod_result != 0 && errno == EINTR);
    if (chmod_result != 0) {
        return fail_with(
            diagnostic(DistributedSieveWaveStoreStatus::publication_failed, posix_error(errno)));
    }
    struct stat initial{};
    if (fstat_retrying_eintr(marker.get(), initial) != 0) {
        return fail_with(
            diagnostic(DistributedSieveWaveStoreStatus::publication_failed, posix_error(errno)));
    }
    if (!S_ISREG(initial.st_mode) || initial.st_nlink != 1 ||
        static_cast<std::uint64_t>(initial.st_uid) != static_cast<std::uint64_t>(::geteuid()) ||
        !exact_mode(initial, 0600) || initial.st_size != 0) {
        return fail_with(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (auto acl =
            acl_rejection(marker.get(), false, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return fail_with(*acl);
    }
    const NativeIdentityV1 created_identity = protocol_identity(initial);

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t request = std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        ssize_t written = -1;
        do {
            written = ::write(marker.get(), bytes.data() + offset, request);
        } while (written < 0 && errno == EINTR);
        if (written < 0) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::publication_failed,
                                        posix_error(errno)));
        }
        if (written == 0) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::publication_failed,
                                        std::make_error_code(std::errc::io_error)));
        }
        offset += static_cast<std::size_t>(written);
    }

    auto inspected = inspect_published_private_lease_marker(parent_fd, leaf.c_str(), marker.get());
    if (!inspected || *inspected.identity != created_identity) {
        return fail_with(inspected ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                                protocol_error())
                                   : std::move(inspected.diagnostic));
    }
    if (const auto synchronized = private_lease_reservation_sync_handle(
            marker.get(), boundary,
            DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileInitial,
            injected_sync_failure);
        synchronized.status != DistributedSieveWaveStoreStatus::ready) {
        return fail_with(synchronized, created_identity);
    }
    inspected = inspect_published_private_lease_marker(parent_fd, leaf.c_str(), marker.get());
    if (!inspected || *inspected.identity != created_identity) {
        return fail_with(inspected ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                                protocol_error())
                                   : std::move(inspected.diagnostic));
    }
    if (const auto synchronized = private_lease_reservation_sync_handle(
            parent_fd, boundary, DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
            injected_sync_failure);
        synchronized.status != DistributedSieveWaveStoreStatus::ready) {
        return fail_with(synchronized, created_identity);
    }
    if (const auto synchronized = private_lease_reservation_sync_handle(
            marker.get(), boundary,
            DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileFinal,
            injected_sync_failure);
        synchronized.status != DistributedSieveWaveStoreStatus::ready) {
        return fail_with(synchronized, created_identity);
    }
    inspected = inspect_published_private_lease_marker(parent_fd, leaf.c_str(), marker.get());
    if (!inspected || *inspected.identity != created_identity) {
        return fail_with(inspected ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                                protocol_error())
                                   : std::move(inspected.diagnostic));
    }
    return {std::move(marker), created_identity, {}};
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
private_lease_rename_no_replace_at(int parent_fd, const char* source,
                                   const char* destination) noexcept {
    for (;;) {
#if defined(__APPLE__)
        if (::renameatx_np(parent_fd, source, parent_fd, destination, RENAME_EXCL) == 0) {
            return {};
        }
#elif defined(__linux__) && defined(SYS_renameat2)
        constexpr unsigned int rename_noreplace = 1U;
        if (::syscall(SYS_renameat2, parent_fd, source, parent_fd, destination, rename_noreplace) ==
            0) {
            return {};
        }
#else
        (void)parent_fd;
        (void)source;
        (void)destination;
        return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                          unsupported_error());
#endif
        const int saved_errno = errno;
        if (saved_errno == EINTR) {
            continue;
        }
        switch (saved_errno) {
        case EEXIST:
        case ENOENT:
        case ENOTDIR:
        case EISDIR:
        case ENOTEMPTY:
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              posix_error(saved_errno));
        case ENOSYS:
        case EINVAL:
        case EOPNOTSUPP:
            return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                              posix_error(saved_errno));
        default:
            return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(saved_errno));
        }
    }
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
private_lease_sync_handle(int descriptor) noexcept {
    std::error_code error;
    if (sync_descriptor(descriptor, error)) {
        return {};
    }
    return diagnostic(DistributedSieveWaveStoreStatus::durability_failed,
                      error ? error : std::make_error_code(std::errc::io_error));
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic private_lease_recovery_sync_handle(
    int descriptor, DistributedSievePrivateLeaseRecoveryEdge edge,
    const std::optional<DistributedSievePrivateLeaseRecoveryEdge>& injected_failure) noexcept {
    auto outcome = private_lease_sync_handle(descriptor);
    if (outcome.status != DistributedSieveWaveStoreStatus::ready) {
        outcome.failed_private_lease_recovery_sync_edge = edge;
        return outcome;
    }
    if (!injected_failure.has_value() || *injected_failure != edge) {
        return outcome;
    }
    outcome = diagnostic(DistributedSieveWaveStoreStatus::durability_failed,
                         std::make_error_code(std::errc::io_error));
    outcome.failed_private_lease_recovery_sync_edge = edge;
    return outcome;
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
read_exact_private_lease_marker_handle(int marker_fd,
                                       const private_lease::PrivateLeaseRecord& expected,
                                       std::uint64_t creator_process_id) noexcept {
    if (marker_fd < 0 || !process_matches(creator_process_id)) {
        return marker_fd < 0 ? diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                          invalid_argument_error())
                             : process_mismatch();
    }
    std::array<std::byte, private_lease::PRIVATE_LEASE_MARKER_BYTES> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (!process_matches(creator_process_id)) {
            return process_mismatch();
        }
        const ssize_t count = ::pread(marker_fd, bytes.data() + offset, bytes.size() - offset,
                                      static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
        }
        if (count == 0) {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              protocol_error());
        }
        offset += static_cast<std::size_t>(count);
    }
    try {
        if (private_lease::parse_private_lease_marker(bytes) != expected) {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              protocol_error());
        }
    } catch (const std::bad_alloc&) {
        return diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                          std::make_error_code(std::errc::not_enough_memory));
    } catch (...) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    return process_matches(creator_process_id) ? DistributedSieveWaveStoreDiagnostic{}
                                               : process_mismatch();
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
validate_named_private_lease_marker_handle(int parent_fd, const char* leaf, int marker_fd,
                                           const private_lease::PrivateLeaseRecord& expected_record,
                                           const NativeIdentityV1& expected_identity,
                                           std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    auto first = inspect_published_private_lease_marker(parent_fd, leaf, marker_fd);
    if (!first || *first.identity != expected_identity) {
        return first ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                  protocol_error())
                     : std::move(first.diagnostic);
    }
    if (const auto record =
            read_exact_private_lease_marker_handle(marker_fd, expected_record, creator_process_id);
        record.status != DistributedSieveWaveStoreStatus::ready) {
        return record;
    }
    auto confirmed = inspect_published_private_lease_marker(parent_fd, leaf, marker_fd);
    if (!confirmed || *confirmed.identity != expected_identity ||
        *confirmed.identity != *first.identity) {
        return confirmed ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                      protocol_error())
                         : std::move(confirmed.diagnostic);
    }
    return process_matches(creator_process_id) ? DistributedSieveWaveStoreDiagnostic{}
                                               : process_mismatch();
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic validate_unlinked_private_lease_marker_handle(
    int parent_fd, const char* former_leaf, int marker_fd,
    const private_lease::PrivateLeaseRecord& expected_record,
    const NativeIdentityV1& expected_identity, std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    struct stat named{};
    if (fstatat_retrying_eintr(parent_fd, former_leaf, named) == 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    if (const int saved_errno = errno; saved_errno != ENOENT) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                          posix_error(saved_errno));
    }
    struct stat held{};
    if (fstat_retrying_eintr(marker_fd, held) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    if (!S_ISREG(held.st_mode) || held.st_nlink != 0 ||
        static_cast<std::uint64_t>(held.st_size) != private_lease::PRIVATE_LEASE_MARKER_BYTES ||
        static_cast<std::uint64_t>(held.st_uid) != static_cast<std::uint64_t>(::geteuid()) ||
        !exact_mode(held, 0600) || protocol_identity(held) != expected_identity) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    if (auto acl =
            acl_rejection(marker_fd, false, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return *acl;
    }
    return read_exact_private_lease_marker_handle(marker_fd, expected_record, creator_process_id);
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
validate_exact_empty_private_lease_directory_handle(int directory_fd,
                                                    std::uint64_t creator_process_id) noexcept {
    if (directory_fd < 0 || !process_matches(creator_process_id)) {
        return directory_fd < 0 ? diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                             invalid_argument_error())
                                : process_mismatch();
    }
    struct stat before{};
    if (fstat_retrying_eintr(directory_fd, before) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    if (!valid_private_lease_directory_metadata(before)) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    if (auto acl =
            acl_rejection(directory_fd, true, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return *acl;
    }

    const int scan_fd =
        openat_retrying_eintr(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (scan_fd < 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
    }
    DIR* raw_directory = ::fdopendir(scan_fd);
    if (raw_directory == nullptr) {
        const int saved_errno = errno;
        (void)::close(scan_fd);
        return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(saved_errno));
    }
    UniqueDirectory scan(raw_directory);
    errno = 0;
    for (;;) {
        dirent* entry = ::readdir(raw_directory);
        if (entry == nullptr) {
            if (errno != 0) {
                return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
            }
            break;
        }
        const std::string_view child(entry->d_name);
        if (child != "." && child != "..") {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              protocol_error());
        }
    }
    struct stat after{};
    if (fstat_retrying_eintr(directory_fd, after) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    if (!valid_private_lease_directory_metadata(after) || !stable_metadata(before, after)) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    return process_matches(creator_process_id) ? DistributedSieveWaveStoreDiagnostic{}
                                               : process_mismatch();
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic validate_unlinked_private_lease_directory_handle(
    int root_fd, const char* former_leaf, int directory_fd,
    const NativeIdentityV1& expected_identity, std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }
    struct stat named{};
    if (fstatat_retrying_eintr(root_fd, former_leaf, named) == 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    if (const int saved_errno = errno; saved_errno != ENOENT) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                          posix_error(saved_errno));
    }
    struct stat held{};
    if (fstat_retrying_eintr(directory_fd, held) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    if (!valid_private_lease_directory_metadata(held) ||
        protocol_identity(held) != expected_identity) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
#if defined(__linux__)
    if (held.st_nlink != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
#elif defined(__APPLE__)
    // Darwin retains st_nlink == 2 for an open directory after rmdir. The
    // disconnected handle keeps its former ".." binding, so prove that it
    // still names the held WaveStore root. The surrounding exact closed-root
    // observation proves that no alternate name in that parent survived.
    const int parent_descriptor =
        openat_retrying_eintr(directory_fd, "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent_descriptor < 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    UniqueFd parent(parent_descriptor);
    struct stat parent_metadata{};
    struct stat root_metadata{};
    if (fstat_retrying_eintr(parent.get(), parent_metadata) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    if (fstat_retrying_eintr(root_fd, root_metadata) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    if (!valid_private_lease_directory_metadata(parent_metadata) ||
        !valid_private_lease_directory_metadata(root_metadata) ||
        !stable_metadata(parent_metadata, root_metadata)) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    if (auto acl =
            acl_rejection(parent.get(), true, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return *acl;
    }
#else
    return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
#endif
    if (auto acl =
            acl_rejection(directory_fd, true, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return *acl;
    }
    return validate_exact_empty_private_lease_directory_handle(directory_fd, creator_process_id);
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
private_lease_unlink_at(int parent_fd, const char* leaf, int flags) noexcept {
    for (;;) {
        if (::unlinkat(parent_fd, leaf, flags) == 0) {
            return {};
        }
        const int saved_errno = errno;
        if (saved_errno == EINTR) {
            continue;
        }
        switch (saved_errno) {
        case EEXIST:
        case ENOENT:
        case ENOTDIR:
        case EISDIR:
        case ENOTEMPTY:
        case ELOOP:
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              posix_error(saved_errno));
        default:
            return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(saved_errno));
        }
    }
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
    names.reserved_leaf = names.relative_lease_stem;
    names.reserved_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_SUFFIX);
    names.reserved_pending_leaf = names.relative_lease_stem;
    names.reserved_pending_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_RESERVED_PENDING_SUFFIX);
    names.owned_leaf = names.relative_lease_stem;
    names.owned_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_SUFFIX);
    names.owned_pending_leaf = names.relative_lease_stem;
    names.owned_pending_leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNED_PENDING_SUFFIX);

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
        !expected_private_lease_reservation_witnesses_.has_value() ||
        !expected_worker_attempt_record_witnesses_.has_value() ||
        expected_private_lease_base_lock_leaves_->size() !=
            expected_private_lease_base_lock_identities_->size() ||
        expected_private_lease_base_lock_leaves_->size() !=
            expected_private_lease_reservation_witnesses_->size()) {
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
        wave_store_state_->root_fd, wave_store_state_->manifest, wave_store_state_->absolute_root,
        creator_process_id_);
    if (!first) {
        return reject_lower_priority(std::move(first.diagnostic));
    }
    if (first.inventory->private_lease_base_lock_leaves !=
            *expected_private_lease_base_lock_leaves_ ||
        *first.base_lock_identities != *expected_private_lease_base_lock_identities_ ||
        *first.private_lease_witnesses != *expected_private_lease_reservation_witnesses_ ||
        first.inventory->worker_attempt_records != *expected_worker_attempt_record_witnesses_) {
        return reject_lower_priority(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (const auto bindings = revalidate_higher_priority_bindings();
        bindings.status != DistributedSieveWaveStoreStatus::ready) {
        return invalidate_with(bindings);
    }

    auto confirmed = capture_manifest_bound_inventory_witness(
        wave_store_state_->root_fd, wave_store_state_->manifest, wave_store_state_->absolute_root,
        creator_process_id_);
    if (!confirmed) {
        return reject_lower_priority(std::move(confirmed.diagnostic));
    }
    if (*confirmed.inventory != *first.inventory ||
        *confirmed.base_lock_identities != *first.base_lock_identities ||
        *confirmed.private_lease_witnesses != *first.private_lease_witnesses ||
        confirmed.inventory->private_lease_base_lock_leaves !=
            *expected_private_lease_base_lock_leaves_ ||
        *confirmed.base_lock_identities != *expected_private_lease_base_lock_identities_ ||
        *confirmed.private_lease_witnesses != *expected_private_lease_reservation_witnesses_ ||
        confirmed.inventory->worker_attempt_records != *expected_worker_attempt_record_witnesses_) {
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

namespace {

#if !defined(_WIN32)

struct DistributedSievePrivateLeaseClosedSnapshot final {
    NamespaceInventory inventory;
    std::vector<NativeIdentityV1> base_lock_identities;
    std::vector<PrivateLeaseReservationWitness> reservation_witnesses;
};

[[nodiscard]] DistributedSievePrivateLeaseClosedSnapshot
private_lease_closed_snapshot(ManifestBoundInventoryWitnessResult observed) {
    return {
        .inventory = std::move(*observed.inventory),
        .base_lock_identities = std::move(*observed.base_lock_identities),
        .reservation_witnesses = std::move(*observed.private_lease_witnesses),
    };
}

[[nodiscard]] bool private_lease_boundary_has_reserved(
    DistributedSievePrivateLeaseReservationBoundary boundary) noexcept {
    return boundary != DistributedSievePrivateLeaseReservationBoundary::PermitAcquired &&
           boundary != DistributedSievePrivateLeaseReservationBoundary::Count;
}

[[nodiscard]] bool private_lease_boundary_has_directory(
    DistributedSievePrivateLeaseReservationBoundary boundary) noexcept {
    switch (boundary) {
    case DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable:
    case DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable:
        return true;
    case DistributedSievePrivateLeaseReservationBoundary::PermitAcquired:
    case DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable:
    case DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable:
    case DistributedSievePrivateLeaseReservationBoundary::Count:
        return false;
    }
    return false;
}

[[nodiscard]] bool private_lease_boundary_has_owner(
    DistributedSievePrivateLeaseReservationBoundary boundary) noexcept {
    switch (boundary) {
    case DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable:
    case DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable:
        return true;
    case DistributedSievePrivateLeaseReservationBoundary::PermitAcquired:
    case DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable:
    case DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable:
    case DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable:
    case DistributedSievePrivateLeaseReservationBoundary::Count:
        return false;
    }
    return false;
}

[[nodiscard]] bool private_lease_boundary_has_owned(
    DistributedSievePrivateLeaseReservationBoundary boundary) noexcept {
    switch (boundary) {
    case DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable:
    case DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable:
        return true;
    case DistributedSievePrivateLeaseReservationBoundary::PermitAcquired:
    case DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable:
    case DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable:
    case DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable:
    case DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable:
    case DistributedSievePrivateLeaseReservationBoundary::Count:
        return false;
    }
    return false;
}

[[nodiscard]] DistributedSievePrivateLeaseReservationBoundary
wave_private_lease_boundary(private_lease::PrivateLeaseReservationBoundary boundary) noexcept {
    return static_cast<DistributedSievePrivateLeaseReservationBoundary>(
        static_cast<std::uint8_t>(boundary));
}

[[nodiscard]] std::string
private_lease_staging_directory_leaf(const DistributedSieveWorkerAttemptNamesV1& names,
                                     const std::array<std::uint64_t, 2>& lease_id) {
    std::string leaf;
    leaf.reserve(names.relative_lease_stem.size() +
                 DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG.size() + 32U);
    leaf.append(names.relative_lease_stem);
    leaf.append(DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG);
    leaf.append(private_lease::private_lease_id_hex(lease_id));
    return leaf;
}

#endif

} // namespace

#if defined(_WIN32)

class DistributedSieveFdPrivateLeaseReservationTarget final {
public:
    struct Failure final {
        DistributedSieveWaveStoreDiagnostic diagnostic;
    };

    struct Completion final {
        DistributedSieveWorkerAttemptNamesV1 worker_attempt_names;
        NativeIdentityV1 base_lock_identity;
        DistributedSievePrivateLeaseReservationInventoryWitness final_witness;
    };

    DistributedSieveFdPrivateLeaseReservationTarget(DistributedSievePrivateLeaseRootClaim&,
                                                    DistributedSievePrivateLeaseProtocolTestHooks) {
        throw Failure{
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error())};
    }

    [[nodiscard]] bool checkpoint(private_lease::PrivateLeaseReservationBoundary) {
        return false;
    }

    [[nodiscard]] bool publish_marker(private_lease::PrivateLeaseReservationMarkerRole,
                                      private_lease::PrivateLeaseReservationBoundary) {
        return false;
    }

    void create_staging_directory() {}
    void promote_final_directory() {}
    void complete() {}

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic interruption_diagnostic() const noexcept {
        return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                          unsupported_error());
    }

    [[nodiscard]] Completion take_completion() {
        throw Failure{
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error())};
    }
};

#else

class DistributedSieveFdPrivateLeaseReservationTarget final {
public:
    struct Failure final {
        DistributedSieveWaveStoreDiagnostic diagnostic;
    };

    struct Completion final {
        DistributedSieveWorkerAttemptNamesV1 worker_attempt_names;
        NativeIdentityV1 base_lock_identity;
        DistributedSievePrivateLeaseReservationInventoryWitness final_witness;
    };

    DistributedSieveFdPrivateLeaseReservationTarget(
        DistributedSievePrivateLeaseRootClaim& claim,
        DistributedSievePrivateLeaseProtocolTestHooks hooks)
        : claim_(claim), hooks_(hooks) {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
        if (!claim_.owned_by_current_process() || claim_.wave_store_state_ == nullptr ||
            claim_.base_lock_at_ == nullptr || !claim_.worker_attempt_names_.has_value() ||
            !claim_.expected_private_lease_base_lock_leaves_.has_value() ||
            !claim_.expected_private_lease_base_lock_identities_.has_value() ||
            !claim_.expected_private_lease_reservation_witnesses_.has_value() ||
            !claim_.expected_worker_attempt_record_witnesses_.has_value()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
        if (const auto validated = claim_.revalidate();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            fail(validated);
        }

        const auto& state = *claim_.wave_store_state_;
        auto observed = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!observed) {
            fail(adjudicate_observation_failure(std::move(observed.diagnostic)));
        }
        if (observed.inventory->private_lease_base_lock_leaves !=
                *claim_.expected_private_lease_base_lock_leaves_ ||
            *observed.base_lock_identities !=
                *claim_.expected_private_lease_base_lock_identities_ ||
            *observed.private_lease_witnesses !=
                *claim_.expected_private_lease_reservation_witnesses_ ||
            observed.inventory->worker_attempt_records !=
                *claim_.expected_worker_attempt_record_witnesses_) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        current_ = private_lease_closed_snapshot(std::move(observed));
        worker_attempt_names_ = *claim_.worker_attempt_names_;
        if (std::binary_search(current_.inventory.worker_attempt_record_leaves.begin(),
                               current_.inventory.worker_attempt_record_leaves.end(),
                               worker_attempt_names_.canonical_record_leaf) ||
            std::binary_search(current_.inventory.worker_attempt_record_leaves.begin(),
                               current_.inventory.worker_attempt_record_leaves.end(),
                               worker_attempt_names_.pending_record_leaf)) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }

        const auto target =
            std::lower_bound(current_.inventory.private_lease_base_lock_leaves.begin(),
                             current_.inventory.private_lease_base_lock_leaves.end(),
                             worker_attempt_names_.base_lock_leaf);
        if (target == current_.inventory.private_lease_base_lock_leaves.end() ||
            *target != worker_attempt_names_.base_lock_leaf) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        target_index_ = static_cast<std::size_t>(
            std::distance(current_.inventory.private_lease_base_lock_leaves.begin(), target));
        if (target_index_ >= current_.base_lock_identities.size() ||
            target_index_ >= current_.reservation_witnesses.size() ||
            current_.base_lock_identities[target_index_] != claim_.base_lock_at_->identity()) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        const auto& initial = current_.reservation_witnesses[target_index_];
        if (initial.base_lock_leaf != worker_attempt_names_.base_lock_leaf ||
            initial.boundary != DistributedSievePrivateLeaseReservationBoundary::PermitAcquired ||
            initial.lease_id != std::array<std::uint64_t, 2>{} ||
            initial.reserved_marker_identity.has_value() ||
            initial.directory_identity.has_value() || initial.owner_marker_identity.has_value() ||
            initial.owned_marker_identity.has_value() || initial.work_package_residue.has_value()) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }

        lease_id_ = private_lease::allocate_private_lease_id();
        if (lease_id_ == std::array<std::uint64_t, 2>{}) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error()));
        }
        staging_directory_leaf_ =
            private_lease_staging_directory_leaf(worker_attempt_names_, lease_id_);

        const std::filesystem::path base_path =
            state.absolute_root / worker_attempt_names_.private_directory_leaf / "corpus";
        reserved_record_ = private_lease::PrivateLeaseRecord{
            .platform_id = private_lease::PLATFORM_ID,
            .phase = private_lease::PrivateLeasePhase::Reserved,
            .capability = private_lease::PrivateLeaseCapability::RollbackStagingOnly,
            .lease_id = lease_id_,
            .base_path_digest = private_lease::frozen_path_digest(base_path),
            .parent_identity = relation_identity(state.root_identity),
            .lock_identity = relation_identity(claim_.base_lock_at_->identity()),
        };
        reserved_bytes_ = private_lease::serialize_private_lease_marker(reserved_record_);
        require_current();
#endif
    }

    [[nodiscard]] bool checkpoint(private_lease::PrivateLeaseReservationBoundary boundary) {
        const auto wave_boundary = wave_private_lease_boundary(boundary);
        if (wave_boundary == DistributedSievePrivateLeaseReservationBoundary::Count ||
            current_witness().boundary != wave_boundary) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
        return offer_boundary(wave_boundary);
    }

    [[nodiscard]] bool
    publish_marker(private_lease::PrivateLeaseReservationMarkerRole role,
                   private_lease::PrivateLeaseReservationBoundary pending_boundary) {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)role;
        (void)pending_boundary;
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
        switch (role) {
        case private_lease::PrivateLeaseReservationMarkerRole::Reserved:
            require_pending_boundary(
                pending_boundary,
                private_lease::PrivateLeaseReservationBoundary::ReservedPendingDurable);
            require_phase(DistributedSievePrivateLeaseReservationBoundary::PermitAcquired);
            return publish_marker_pair(
                claim_.wave_store_state_->root_fd, worker_attempt_names_.reserved_pending_leaf,
                worker_attempt_names_.reserved_leaf, reserved_bytes_,
                DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable,
                DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable, true);
        case private_lease::PrivateLeaseReservationMarkerRole::Owner: {
            require_pending_boundary(
                pending_boundary,
                private_lease::PrivateLeaseReservationBoundary::OwnerPendingDurable);
            require_phase(DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable);
            if (!current_witness().directory_identity.has_value()) {
                fail(adjudicate_observation_failure(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
            const auto owner = private_lease::make_private_lease_owner_record(
                reserved_record_, relation_identity(*current_witness().directory_identity));
            auto bytes = private_lease::serialize_private_lease_marker(owner);
            return publish_marker_pair(
                staging_directory_.get(),
                std::string(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF),
                std::string(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF), bytes,
                DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable,
                DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable, false);
        }
        case private_lease::PrivateLeaseReservationMarkerRole::Owned: {
            require_pending_boundary(
                pending_boundary,
                private_lease::PrivateLeaseReservationBoundary::OwnedPendingDurable);
            require_phase(DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable);
            if (!current_witness().directory_identity.has_value() ||
                !current_witness().owner_marker_identity.has_value()) {
                fail(adjudicate_observation_failure(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
            const auto owner = private_lease::make_private_lease_owner_record(
                reserved_record_, relation_identity(*current_witness().directory_identity));
            const auto owned = private_lease::make_private_lease_owned_record(
                owner, relation_identity(*current_witness().owner_marker_identity));
            auto bytes = private_lease::serialize_private_lease_marker(owned);
            return publish_marker_pair(
                claim_.wave_store_state_->root_fd, worker_attempt_names_.owned_pending_leaf,
                worker_attempt_names_.owned_leaf, bytes,
                DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable,
                DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable, true);
        }
        case private_lease::PrivateLeaseReservationMarkerRole::Count:
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
        fail(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error()));
#endif
    }

    void create_staging_directory() {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
        require_phase(DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable);
        NamespaceInventory successor_inventory = current_.inventory;
        insert_protocol_leaf(successor_inventory, staging_directory_leaf_);
        before_mutation(DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable);

        int result = -1;
        do {
            result =
                ::mkdirat(claim_.wave_store_state_->root_fd, staging_directory_leaf_.c_str(), 0700);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            fail(adjudicate_operation_failure(
                diagnostic(errno == EEXIST ? DistributedSieveWaveStoreStatus::namespace_conflict
                                           : DistributedSieveWaveStoreStatus::io_failed,
                           posix_error(errno)),
                &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable));
        }

        UniqueFd created(openat_retrying_eintr(claim_.wave_store_state_->root_fd,
                                               staging_directory_leaf_.c_str(),
                                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!created) {
            fail(adjudicate_operation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno)),
                &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable));
        }
        int chmod_result = -1;
        do {
            chmod_result = ::fchmod(created.get(), 0700);
        } while (chmod_result != 0 && errno == EINTR);
        if (chmod_result != 0) {
            fail(adjudicate_operation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno)),
                &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable));
        }
        struct stat metadata{};
        if (fstat_retrying_eintr(created.get(), metadata) != 0) {
            fail(adjudicate_operation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno)),
                &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable));
        }
        if (!valid_private_lease_directory_metadata(metadata)) {
            fail(adjudicate_operation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()),
                &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable));
        }
        if (auto acl = acl_rejection(created.get(), true,
                                     DistributedSieveWaveStoreStatus::namespace_conflict);
            acl.has_value()) {
            fail(adjudicate_operation_failure(
                *acl, &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable));
        }
        const NativeIdentityV1 created_identity = protocol_identity(metadata);
        staging_directory_ = std::move(created);
        if (const auto synchronized = private_lease_reservation_sync_handle(
                staging_directory_.get(),
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
                DistributedSievePrivateLeaseReservationSyncPoint::StagingDirectory,
                injected_sync_failure_);
            synchronized.status != DistributedSieveWaveStoreStatus::ready) {
            fail(adjudicate_operation_failure(
                synchronized, &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
                created_identity));
        }
        if (const auto synchronized = private_lease_reservation_sync_handle(
                claim_.wave_store_state_->root_fd,
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
                DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
                injected_sync_failure_);
            synchronized.status != DistributedSieveWaveStoreStatus::ready) {
            fail(adjudicate_operation_failure(
                synchronized, &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
                created_identity));
        }
        accept_successor(std::move(successor_inventory),
                         DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable,
                         created_identity);
#endif
    }

    void promote_final_directory() {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
        require_phase(DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable);
        NamespaceInventory successor_inventory = current_.inventory;
        erase_protocol_leaf(successor_inventory, staging_directory_leaf_);
        insert_protocol_leaf(successor_inventory, worker_attempt_names_.private_directory_leaf);
        before_mutation(DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable);
        if (const auto renamed = private_lease_rename_no_replace_at(
                claim_.wave_store_state_->root_fd, staging_directory_leaf_.c_str(),
                worker_attempt_names_.private_directory_leaf.c_str());
            renamed.status != DistributedSieveWaveStoreStatus::ready) {
            fail(adjudicate_operation_failure(
                renamed, &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable));
        }
        if (const auto synchronized = private_lease_reservation_sync_handle(
                claim_.wave_store_state_->root_fd,
                DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable,
                DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
                injected_sync_failure_);
            synchronized.status != DistributedSieveWaveStoreStatus::ready) {
            fail(adjudicate_operation_failure(
                synchronized, &successor_inventory,
                DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable));
        }
        accept_successor(std::move(successor_inventory),
                         DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable);
#endif
    }

    void complete() {
        require_phase(DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable);
        require_current();
        completed_ = true;
    }

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic interruption_diagnostic() const noexcept {
        auto outcome = diagnostic(DistributedSieveWaveStoreStatus::interrupted,
                                  std::make_error_code(std::errc::operation_canceled));
        outcome.last_private_lease_reservation_boundary = interrupted_boundary_;
        return outcome;
    }

    [[nodiscard]] Completion take_completion() {
        if (!completed_ ||
            current_witness().boundary !=
                DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable ||
            !current_witness().directory_identity.has_value() ||
            !current_witness().owner_marker_identity.has_value() ||
            !current_witness().owned_marker_identity.has_value() ||
            current_witness().work_package_residue.has_value()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error()));
        }
        require_current();
        return {
            .worker_attempt_names = std::move(worker_attempt_names_),
            .base_lock_identity = current_.base_lock_identities[target_index_],
            .final_witness = std::move(current_.reservation_witnesses[target_index_]),
        };
    }

private:
    [[noreturn]] static void fail(DistributedSieveWaveStoreDiagnostic outcome) {
        throw Failure{std::move(outcome)};
    }

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    adjudicate_observation_failure(DistributedSieveWaveStoreDiagnostic lower) const noexcept {
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        return lower;
    }

    [[nodiscard]] const PrivateLeaseReservationWitness& current_witness() const {
        if (target_index_ >= current_.reservation_witnesses.size()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error()));
        }
        return current_.reservation_witnesses[target_index_];
    }

    void require_phase(DistributedSievePrivateLeaseReservationBoundary expected) const {
        if (current_witness().boundary != expected) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
    }

    static void require_pending_boundary(private_lease::PrivateLeaseReservationBoundary actual,
                                         private_lease::PrivateLeaseReservationBoundary expected) {
        if (actual != expected) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
    }

    static void insert_protocol_leaf(NamespaceInventory& inventory, const std::string& leaf) {
        const auto position = std::lower_bound(inventory.private_lease_protocol_leaves.begin(),
                                               inventory.private_lease_protocol_leaves.end(), leaf);
        if (position != inventory.private_lease_protocol_leaves.end() && *position == leaf) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        inventory.private_lease_protocol_leaves.insert(position, leaf);
    }

    static void erase_protocol_leaf(NamespaceInventory& inventory, const std::string& leaf) {
        const auto position = std::lower_bound(inventory.private_lease_protocol_leaves.begin(),
                                               inventory.private_lease_protocol_leaves.end(), leaf);
        if (position == inventory.private_lease_protocol_leaves.end() || *position != leaf) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        inventory.private_lease_protocol_leaves.erase(position);
    }

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    validate_staging_handle(const PrivateLeaseReservationWitness& witness) const noexcept {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)witness;
        return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                          unsupported_error());
#else
        if (!private_lease_boundary_has_directory(witness.boundary)) {
            return staging_directory_
                       ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                    protocol_error())
                       : DistributedSieveWaveStoreDiagnostic{};
        }
        if (!staging_directory_ || !witness.directory_identity.has_value()) {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              protocol_error());
        }
        struct stat metadata{};
        if (fstat_retrying_eintr(staging_directory_.get(), metadata) != 0) {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              posix_error(errno));
        }
        if (!valid_private_lease_directory_metadata(metadata) ||
            protocol_identity(metadata) != *witness.directory_identity) {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              protocol_error());
        }
        if (auto acl = acl_rejection(staging_directory_.get(), true,
                                     DistributedSieveWaveStoreStatus::namespace_conflict);
            acl.has_value()) {
            return *acl;
        }
        return {};
#endif
    }

    [[nodiscard]] bool successor_witness_valid(
        const PrivateLeaseReservationWitness& candidate,
        DistributedSievePrivateLeaseReservationBoundary boundary,
        std::optional<NativeIdentityV1> expected_introduced_identity) const noexcept {
        const auto& prior = current_witness();
        if (candidate.base_lock_leaf != prior.base_lock_leaf || candidate.boundary != boundary ||
            candidate.lease_id != lease_id_ || prior.work_package_residue.has_value() ||
            candidate.work_package_residue.has_value()) {
            return false;
        }
        const auto optional_identity_valid = [](const std::optional<NativeIdentityV1>& previous,
                                                const std::optional<NativeIdentityV1>& next,
                                                bool required) noexcept {
            if (next.has_value() != required || (next.has_value() && nil_identity(*next))) {
                return false;
            }
            return !previous.has_value() || (next.has_value() && *previous == *next);
        };
        if (!optional_identity_valid(prior.reserved_marker_identity,
                                     candidate.reserved_marker_identity,
                                     private_lease_boundary_has_reserved(boundary)) ||
            !optional_identity_valid(prior.directory_identity, candidate.directory_identity,
                                     private_lease_boundary_has_directory(boundary)) ||
            !optional_identity_valid(prior.owner_marker_identity, candidate.owner_marker_identity,
                                     private_lease_boundary_has_owner(boundary)) ||
            !optional_identity_valid(prior.owned_marker_identity, candidate.owned_marker_identity,
                                     private_lease_boundary_has_owned(boundary))) {
            return false;
        }

        const std::optional<NativeIdentityV1>* introduced = nullptr;
        switch (boundary) {
        case DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable:
            introduced = &candidate.reserved_marker_identity;
            break;
        case DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable:
            introduced = &candidate.directory_identity;
            break;
        case DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable:
            introduced = &candidate.owner_marker_identity;
            break;
        case DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable:
            introduced = &candidate.owned_marker_identity;
            break;
        case DistributedSievePrivateLeaseReservationBoundary::PermitAcquired:
        case DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable:
        case DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable:
        case DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable:
        case DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable:
        case DistributedSievePrivateLeaseReservationBoundary::Count:
            break;
        }
        if (introduced == nullptr) {
            return !expected_introduced_identity.has_value();
        }
        return expected_introduced_identity.has_value() && introduced->has_value() &&
               **introduced == *expected_introduced_identity;
    }

    [[nodiscard]] bool
    semantic_successor_valid(const ManifestBoundInventoryWitnessResult& observed,
                             const NamespaceInventory& expected_inventory,
                             DistributedSievePrivateLeaseReservationBoundary boundary,
                             std::optional<NativeIdentityV1> expected_introduced_identity =
                                 std::nullopt) const noexcept {
        if (!observed || *observed.inventory != expected_inventory ||
            *observed.base_lock_identities != current_.base_lock_identities ||
            observed.private_lease_witnesses->size() != current_.reservation_witnesses.size() ||
            target_index_ >= observed.private_lease_witnesses->size()) {
            return false;
        }
        for (std::size_t index = 0; index < observed.private_lease_witnesses->size(); ++index) {
            if (index != target_index_ && observed.private_lease_witnesses->at(index) !=
                                              current_.reservation_witnesses[index]) {
                return false;
            }
        }
        return successor_witness_valid(observed.private_lease_witnesses->at(target_index_),
                                       boundary, expected_introduced_identity);
    }

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic validate_exact_snapshot(
        const DistributedSievePrivateLeaseClosedSnapshot& expected) const noexcept {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)expected;
        return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                          unsupported_error());
#else
        const auto& state = *claim_.wave_store_state_;
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        auto first = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!first) {
            return adjudicate_observation_failure(std::move(first.diagnostic));
        }
        if (*first.inventory != expected.inventory ||
            *first.base_lock_identities != expected.base_lock_identities ||
            *first.private_lease_witnesses != expected.reservation_witnesses) {
            return adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (const auto staging =
                validate_staging_handle(expected.reservation_witnesses[target_index_]);
            staging.status != DistributedSieveWaveStoreStatus::ready) {
            return adjudicate_observation_failure(staging);
        }
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        auto confirmed = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!confirmed) {
            return adjudicate_observation_failure(std::move(confirmed.diagnostic));
        }
        if (*confirmed.inventory != *first.inventory ||
            *confirmed.base_lock_identities != *first.base_lock_identities ||
            *confirmed.private_lease_witnesses != *first.private_lease_witnesses ||
            *confirmed.inventory != expected.inventory ||
            *confirmed.base_lock_identities != expected.base_lock_identities ||
            *confirmed.private_lease_witnesses != expected.reservation_witnesses) {
            return adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (const auto staging =
                validate_staging_handle(expected.reservation_witnesses[target_index_]);
            staging.status != DistributedSieveWaveStoreStatus::ready) {
            return adjudicate_observation_failure(staging);
        }
        return claim_.revalidate_authority();
#endif
    }

    void require_current() const {
        if (const auto validated = validate_exact_snapshot(current_);
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            fail(validated);
        }
    }

    void select_injected_sync_failure(DistributedSievePrivateLeaseReservationBoundary successor) {
        injected_sync_failure_.reset();
        if (hooks_.fail_before_sync == nullptr) {
            return;
        }
        const auto offer = [&](DistributedSievePrivateLeaseReservationSyncPoint point) -> bool {
            if (!process_matches(claim_.creator_process_id_)) {
                fail(process_mismatch());
            }
            const bool selected = hooks_.fail_before_sync(successor, point, hooks_.context);
            if (!process_matches(claim_.creator_process_id_)) {
                fail(process_mismatch());
            }
            require_current();
            if (selected) {
                injected_sync_failure_ = point;
            }
            return selected;
        };

        switch (successor) {
        case DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable:
        case DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable:
        case DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable:
            if (offer(DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileInitial) ||
                offer(DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory)) {
                return;
            }
            (void)offer(DistributedSievePrivateLeaseReservationSyncPoint::MarkerFileFinal);
            return;
        case DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable:
            if (offer(DistributedSievePrivateLeaseReservationSyncPoint::StagingDirectory)) {
                return;
            }
            (void)offer(DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory);
            return;
        case DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable:
        case DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable:
        case DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable:
        case DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable:
            (void)offer(DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory);
            return;
        case DistributedSievePrivateLeaseReservationBoundary::PermitAcquired:
        case DistributedSievePrivateLeaseReservationBoundary::Count:
            return;
        }
    }

    void before_mutation(DistributedSievePrivateLeaseReservationBoundary successor) {
        require_current();
        if (current_witness().work_package_residue.has_value()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (!process_matches(claim_.creator_process_id_)) {
            fail(process_mismatch());
        }
        if (hooks_.after_predecessor_validation != nullptr) {
            hooks_.after_predecessor_validation(successor, hooks_.context);
        }
        if (!process_matches(claim_.creator_process_id_)) {
            fail(process_mismatch());
        }
        require_current();
        select_injected_sync_failure(successor);
    }

    [[nodiscard]] bool offer_boundary(DistributedSievePrivateLeaseReservationBoundary boundary) {
        require_current();
        if (!process_matches(claim_.creator_process_id_)) {
            fail(process_mismatch());
        }
        const bool stop =
            hooks_.stop_after != nullptr && hooks_.stop_after(boundary, hooks_.context);
        if (!process_matches(claim_.creator_process_id_)) {
            fail(process_mismatch());
        }
        require_current();
        if (stop) {
            interrupted_boundary_ = boundary;
        }
        return stop;
    }

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic adjudicate_operation_failure(
        DistributedSieveWaveStoreDiagnostic lower,
        const NamespaceInventory* possible_successor_inventory,
        DistributedSievePrivateLeaseReservationBoundary possible_successor_boundary,
        std::optional<NativeIdentityV1> expected_introduced_identity =
            std::nullopt) const noexcept {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)possible_successor_inventory;
        (void)possible_successor_boundary;
        (void)expected_introduced_identity;
        return lower;
#else
        const auto& state = *claim_.wave_store_state_;
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        auto first = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!first) {
            return adjudicate_observation_failure(std::move(first.diagnostic));
        }
        const bool predecessor = *first.inventory == current_.inventory &&
                                 *first.base_lock_identities == current_.base_lock_identities &&
                                 *first.private_lease_witnesses == current_.reservation_witnesses;
        const bool successor =
            possible_successor_inventory != nullptr &&
            semantic_successor_valid(first, *possible_successor_inventory,
                                     possible_successor_boundary, expected_introduced_identity);
        if (!predecessor && !successor) {
            return adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        const auto& first_witness = predecessor ? current_.reservation_witnesses[target_index_]
                                                : first.private_lease_witnesses->at(target_index_);
        if (const auto staging = validate_staging_handle(first_witness);
            staging.status != DistributedSieveWaveStoreStatus::ready) {
            return adjudicate_observation_failure(staging);
        }
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        if (lower.failed_private_lease_reservation_sync_site.has_value() &&
            hooks_.after_injected_sync_failure != nullptr) {
            if (!process_matches(claim_.creator_process_id_)) {
                return process_mismatch();
            }
            hooks_.after_injected_sync_failure(*lower.failed_private_lease_reservation_sync_site,
                                               hooks_.context);
            if (!process_matches(claim_.creator_process_id_)) {
                return process_mismatch();
            }
            if (const auto authority = claim_.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return authority;
            }
        }
        auto confirmed = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!confirmed) {
            return adjudicate_observation_failure(std::move(confirmed.diagnostic));
        }
        if (*confirmed.inventory != *first.inventory ||
            *confirmed.base_lock_identities != *first.base_lock_identities ||
            *confirmed.private_lease_witnesses != *first.private_lease_witnesses) {
            return adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        const auto& confirmed_witness = predecessor
                                            ? current_.reservation_witnesses[target_index_]
                                            : confirmed.private_lease_witnesses->at(target_index_);
        if (const auto staging = validate_staging_handle(confirmed_witness);
            staging.status != DistributedSieveWaveStoreStatus::ready) {
            return adjudicate_observation_failure(staging);
        }
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        return lower;
#endif
    }

    void
    accept_successor(NamespaceInventory expected_inventory,
                     DistributedSievePrivateLeaseReservationBoundary boundary,
                     std::optional<NativeIdentityV1> expected_introduced_identity = std::nullopt) {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)expected_inventory;
        (void)boundary;
        (void)expected_introduced_identity;
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
        const auto& state = *claim_.wave_store_state_;
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            fail(authority);
        }
        auto first = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!first) {
            fail(adjudicate_observation_failure(std::move(first.diagnostic)));
        }
        if (!semantic_successor_valid(first, expected_inventory, boundary,
                                      expected_introduced_identity)) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        if (const auto staging =
                validate_staging_handle(first.private_lease_witnesses->at(target_index_));
            staging.status != DistributedSieveWaveStoreStatus::ready) {
            fail(adjudicate_observation_failure(staging));
        }
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            fail(authority);
        }
        if (!process_matches(claim_.creator_process_id_)) {
            fail(process_mismatch());
        }
        if (hooks_.after_first_successor_validation != nullptr) {
            hooks_.after_first_successor_validation(boundary, hooks_.context);
        }
        if (!process_matches(claim_.creator_process_id_)) {
            fail(process_mismatch());
        }
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            fail(authority);
        }
        auto confirmed = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!confirmed) {
            fail(adjudicate_observation_failure(std::move(confirmed.diagnostic)));
        }
        if (*confirmed.inventory != *first.inventory ||
            *confirmed.base_lock_identities != *first.base_lock_identities ||
            *confirmed.private_lease_witnesses != *first.private_lease_witnesses ||
            !semantic_successor_valid(confirmed, expected_inventory, boundary,
                                      expected_introduced_identity)) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        if (const auto authority = claim_.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            fail(authority);
        }
        current_ = private_lease_closed_snapshot(std::move(first));
        if (const auto staging = validate_staging_handle(current_witness());
            staging.status != DistributedSieveWaveStoreStatus::ready) {
            fail(adjudicate_observation_failure(staging));
        }
#endif
    }

    [[nodiscard]] bool
    publish_marker_pair(int parent_fd, const std::string& pending_leaf,
                        const std::string& canonical_leaf, std::span<const std::byte> bytes,
                        DistributedSievePrivateLeaseReservationBoundary pending_boundary,
                        DistributedSievePrivateLeaseReservationBoundary canonical_boundary,
                        bool root_inventory_leaf) {
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)parent_fd;
        (void)pending_leaf;
        (void)canonical_leaf;
        (void)bytes;
        (void)pending_boundary;
        (void)canonical_boundary;
        (void)root_inventory_leaf;
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
        NamespaceInventory pending_inventory = current_.inventory;
        NamespaceInventory canonical_inventory = current_.inventory;
        if (root_inventory_leaf) {
            insert_protocol_leaf(pending_inventory, pending_leaf);
            insert_protocol_leaf(canonical_inventory, canonical_leaf);
        }
        before_mutation(pending_boundary);
        auto published = publish_private_lease_marker_strict_at(
            parent_fd, pending_leaf, bytes, pending_boundary, injected_sync_failure_);
        if (!published) {
            fail(adjudicate_operation_failure(std::move(published.outcome), &pending_inventory,
                                              pending_boundary, published.identity));
        }
        accept_successor(std::move(pending_inventory), pending_boundary, published.identity);
        if (const auto closed = published.close_checked();
            closed.status != DistributedSieveWaveStoreStatus::ready) {
            fail(adjudicate_operation_failure(
                closed, nullptr, DistributedSievePrivateLeaseReservationBoundary::Count));
        }
        if (offer_boundary(pending_boundary)) {
            return true;
        }

        before_mutation(canonical_boundary);
        if (const auto renamed = private_lease_rename_no_replace_at(parent_fd, pending_leaf.c_str(),
                                                                    canonical_leaf.c_str());
            renamed.status != DistributedSieveWaveStoreStatus::ready) {
            fail(adjudicate_operation_failure(renamed, &canonical_inventory, canonical_boundary));
        }
        if (const auto synchronized = private_lease_reservation_sync_handle(
                parent_fd, canonical_boundary,
                DistributedSievePrivateLeaseReservationSyncPoint::ParentDirectory,
                injected_sync_failure_);
            synchronized.status != DistributedSieveWaveStoreStatus::ready) {
            fail(adjudicate_operation_failure(synchronized, &canonical_inventory,
                                              canonical_boundary));
        }
        accept_successor(std::move(canonical_inventory), canonical_boundary);
        return false;
#endif
    }

    DistributedSievePrivateLeaseRootClaim& claim_;
    DistributedSievePrivateLeaseProtocolTestHooks hooks_;
    DistributedSievePrivateLeaseClosedSnapshot current_;
    DistributedSieveWorkerAttemptNamesV1 worker_attempt_names_;
    std::size_t target_index_ = 0;
    std::array<std::uint64_t, 2> lease_id_{};
    std::string staging_directory_leaf_;
    private_lease::PrivateLeaseRecord reserved_record_;
    std::vector<std::byte> reserved_bytes_;
    std::optional<DistributedSievePrivateLeaseReservationSyncPoint> injected_sync_failure_;
#if !defined(_WIN32)
    UniqueFd staging_directory_;
#endif
    std::optional<DistributedSievePrivateLeaseReservationBoundary> interrupted_boundary_;
    bool completed_ = false;
};

#endif

class DistributedSieveStartedAttemptCleanupAdmission final {
public:
    DistributedSieveStartedAttemptCleanupAdmission() = delete;
    DistributedSieveStartedAttemptCleanupAdmission(
        const DistributedSieveStartedAttemptCleanupAdmission&) = delete;
    DistributedSieveStartedAttemptCleanupAdmission&
    operator=(const DistributedSieveStartedAttemptCleanupAdmission&) = delete;
    DistributedSieveStartedAttemptCleanupAdmission(
        DistributedSieveStartedAttemptCleanupAdmission&&) noexcept = default;
    DistributedSieveStartedAttemptCleanupAdmission&
    operator=(DistributedSieveStartedAttemptCleanupAdmission&&) = delete;
    ~DistributedSieveStartedAttemptCleanupAdmission() = default;

private:
#if !defined(_WIN32)
    DistributedSieveStartedAttemptCleanupAdmission(
        UniqueFd canonical_record, DistributedSieveWorkerAttemptNamesV1 worker_attempt_names,
        DistributedSieveWorkerAttemptRecordInventoryWitness record_witness,
        std::uint64_t creator_process_id) noexcept
        : canonical_record_(std::move(canonical_record)),
          worker_attempt_names_(std::move(worker_attempt_names)),
          record_witness_(std::move(record_witness)), creator_process_id_(creator_process_id) {}

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic revalidate(int root_fd) const noexcept;

    UniqueFd canonical_record_;
    DistributedSieveWorkerAttemptNamesV1 worker_attempt_names_;
    DistributedSieveWorkerAttemptRecordInventoryWitness record_witness_;
    std::uint64_t creator_process_id_ = 0;
#endif

    friend class DistributedSieveFdPrivateLeaseRecoveryTarget;
    friend DistributedSieveWorkerAttemptReconcileResult reconcile_worker_attempt_started(
        DistributedSievePrivateLeaseRootClaimResult&& claimed,
        DistributedSieveWorkerAttemptReconcileTestHooks hooks) noexcept;
};

#if !defined(_WIN32)

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
validate_exact_worker_attempt_record_handle(int root_fd, int record_fd, const std::string& leaf,
                                            const durable_record::RecordSnapshot& expected_snapshot,
                                            std::span<const std::byte> expected_bytes,
                                            std::uint64_t creator_process_id) noexcept {
    if (root_fd < 0 || record_fd < 0 || leaf.empty() || expected_bytes.empty()) {
        return diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                          invalid_argument_error());
    }
    if (!process_matches(creator_process_id)) {
        return process_mismatch();
    }

    struct stat held_before{};
    struct stat named_before{};
    if (fstat_retrying_eintr(record_fd, held_before) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
    }
    if (fstatat_retrying_eintr(root_fd, leaf.c_str(), named_before) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    const auto held_snapshot = durable_record::RecordSnapshot{
        .identity = record_identity(held_before),
        .size = static_cast<std::uint64_t>(held_before.st_size),
    };
    if (!valid_manifest_metadata(held_before) || !valid_manifest_metadata(named_before) ||
        !stable_metadata(held_before, named_before) || held_snapshot != expected_snapshot ||
        static_cast<std::uint64_t>(expected_bytes.size()) != expected_snapshot.size) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    if (auto acl =
            acl_rejection(record_fd, false, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return *acl;
    }

    std::size_t offset = 0;
    while (offset < expected_bytes.size()) {
        if (!process_matches(creator_process_id)) {
            return process_mismatch();
        }
        const std::size_t request =
            std::min(expected_bytes.size() - offset,
                     static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        std::array<std::byte, 4096> buffer{};
        const std::size_t bounded_request = std::min(request, buffer.size());
        const ssize_t count =
            ::pread(record_fd, buffer.data(), bounded_request, static_cast<off_t>(offset));
        if (count < 0) {
            const int saved_errno = errno;
            if (saved_errno == EINTR) {
                continue;
            }
            return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(saved_errno));
        }
        if (count == 0 ||
            !std::equal(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count),
                        expected_bytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              protocol_error());
        }
        offset += static_cast<std::size_t>(count);
    }

    std::byte trailing{};
    ssize_t trailing_count = -1;
    do {
        trailing_count =
            ::pread(record_fd, &trailing, 1, static_cast<off_t>(expected_bytes.size()));
    } while (trailing_count < 0 && errno == EINTR);
    if (trailing_count < 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
    }
    if (trailing_count != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }

    struct stat held_after{};
    struct stat named_after{};
    if (fstat_retrying_eintr(record_fd, held_after) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::io_failed, posix_error(errno));
    }
    if (fstatat_retrying_eintr(root_fd, leaf.c_str(), named_after) != 0) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
    }
    const auto final_snapshot = durable_record::RecordSnapshot{
        .identity = record_identity(held_after),
        .size = static_cast<std::uint64_t>(held_after.st_size),
    };
    if (!valid_manifest_metadata(held_after) || !valid_manifest_metadata(named_after) ||
        !stable_metadata(held_before, held_after) || !stable_metadata(held_after, named_after) ||
        final_snapshot != expected_snapshot) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    if (auto acl =
            acl_rejection(record_fd, false, DistributedSieveWaveStoreStatus::namespace_conflict);
        acl.has_value()) {
        return *acl;
    }
    return process_matches(creator_process_id) ? DistributedSieveWaveStoreDiagnostic{}
                                               : process_mismatch();
}

struct WorkerAttemptCanonicalRecordOpenResult final {
    UniqueFd canonical_record;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(canonical_record) &&
               diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }
};

[[nodiscard]] WorkerAttemptCanonicalRecordOpenResult open_worker_attempt_canonical_record(
    int root_fd, const std::string& leaf, const durable_record::RecordSnapshot& expected_snapshot,
    std::span<const std::byte> expected_bytes, std::uint64_t creator_process_id) noexcept {
    if (!process_matches(creator_process_id)) {
        return {{}, process_mismatch()};
    }
    const int descriptor = openat_retrying_eintr(root_fd, leaf.c_str(),
                                                 O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        return {
            {},
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno))};
    }
    UniqueFd held(descriptor);
    auto validated = validate_exact_worker_attempt_record_handle(
        root_fd, held.get(), leaf, expected_snapshot, expected_bytes, creator_process_id);
    if (validated.status != DistributedSieveWaveStoreStatus::ready) {
        return {{}, std::move(validated)};
    }
    return {std::move(held), {}};
}

DistributedSieveWaveStoreDiagnostic
DistributedSieveStartedAttemptCleanupAdmission::revalidate(int root_fd) const noexcept {
    if (!canonical_record_ || creator_process_id_ == 0 ||
        !record_witness_.canonical_snapshot.has_value() ||
        record_witness_.pending_snapshot.has_value()) {
        return diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                          invalid_argument_error());
    }
    return validate_exact_worker_attempt_record_handle(
        root_fd, canonical_record_.get(), worker_attempt_names_.canonical_record_leaf,
        *record_witness_.canonical_snapshot, record_witness_.bytes, creator_process_id_);
}

#endif

#if defined(_WIN32)

class DistributedSieveFdPrivateLeaseRecoveryTarget final {
public:
    struct Failure final {
        DistributedSieveWaveStoreDiagnostic diagnostic;
    };

    DistributedSieveFdPrivateLeaseRecoveryTarget(DistributedSievePrivateLeaseRootClaim&,
                                                 DistributedSievePrivateLeaseRecoveryTestHooks) {
        throw Failure{
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error())};
    }

    [[nodiscard]] private_lease::PrivateLeaseReservationBoundary boundary() const {
        return private_lease::PrivateLeaseReservationBoundary::Count;
    }

    void apply(private_lease::PrivateLeaseRecoveryTransition) {}

    [[nodiscard]] bool checkpoint(private_lease::PrivateLeaseReservationBoundary) {
        return false;
    }

    void complete() {}
    void reject_invalid_boundary() {}
    [[nodiscard]] bool completed() const noexcept {
        return false;
    }

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic interruption_diagnostic() const noexcept {
        return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported,
                          unsupported_error());
    }
};

#else

class DistributedSieveFdPrivateLeaseRecoveryTarget final {
public:
    struct Failure final {
        DistributedSieveWaveStoreDiagnostic diagnostic;
    };

    DistributedSieveFdPrivateLeaseRecoveryTarget(
        DistributedSievePrivateLeaseRootClaim& claim,
        DistributedSievePrivateLeaseRecoveryTestHooks hooks);
    DistributedSieveFdPrivateLeaseRecoveryTarget(
        DistributedSievePrivateLeaseRootClaim& claim,
        DistributedSieveStartedAttemptCleanupAdmission&& admission,
        DistributedSievePrivateLeaseRecoveryTestHooks hooks);

    [[nodiscard]] private_lease::PrivateLeaseReservationBoundary boundary() const;
    void apply(private_lease::PrivateLeaseRecoveryTransition transition);
    [[nodiscard]] bool checkpoint(private_lease::PrivateLeaseReservationBoundary boundary);
    void complete();
    void reject_invalid_boundary() noexcept;
    [[nodiscard]] bool completed() const noexcept;
    [[nodiscard]] std::optional<DistributedSieveReconciledWorkerAttemptV1>
    started_completion() const;

    [[nodiscard]] DistributedSieveWaveStoreDiagnostic interruption_diagnostic() const noexcept;

private:
    [[noreturn]] static void fail(DistributedSieveWaveStoreDiagnostic outcome);

    void initialize_common();
    void require_prestart_record_absent() const;
    void require_started_record_exact() const;
    [[nodiscard]] const PrivateLeaseReservationWitness& current_witness() const;
    void require_phase(DistributedSievePrivateLeaseReservationBoundary expected) const;
    void require_current() const;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic validate_started_record_pin() const noexcept;
    void before_mutation();
    void select_injected_sync_failure(DistributedSievePrivateLeaseRecoveryEdge edge);

    [[nodiscard]] private_lease::PrivateLeaseRecord expected_reserved_record() const;
    [[nodiscard]] private_lease::PrivateLeaseRecord expected_owner_record() const;
    [[nodiscard]] private_lease::PrivateLeaseRecord expected_owned_record() const;

    [[nodiscard]] PrivateLeaseMarkerAtResult
    hold_exact_marker(int parent_fd, const std::string& leaf,
                      const private_lease::PrivateLeaseRecord& expected_record,
                      const NativeIdentityV1& expected_identity) const;

    [[nodiscard]] DistributedSievePrivateLeaseClosedSnapshot
    expected_successor(private_lease::PrivateLeaseRecoveryTransition transition) const;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic validate_exact_snapshot(
        const DistributedSievePrivateLeaseClosedSnapshot& expected) const noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    validate_directory_handle(const PrivateLeaseReservationWitness& witness) const noexcept;
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic
    adjudicate_observation_failure(DistributedSieveWaveStoreDiagnostic lower) const noexcept;
    template <typename LocalValidator>
    [[nodiscard]] DistributedSieveWaveStoreDiagnostic adjudicate_operation_failure(
        DistributedSieveWaveStoreDiagnostic lower,
        const DistributedSievePrivateLeaseClosedSnapshot& possible_successor,
        DistributedSievePrivateLeaseRecoveryEdge edge,
        LocalValidator&& validate_local) const noexcept;
    template <typename LocalValidator>
    void accept_successor(const DistributedSievePrivateLeaseClosedSnapshot& expected,
                          DistributedSievePrivateLeaseRecoveryEdge edge,
                          LocalValidator&& validate_local);

    static void insert_protocol_leaf(NamespaceInventory& inventory, const std::string& leaf);
    static void erase_protocol_leaf(NamespaceInventory& inventory, const std::string& leaf);
    static void replace_protocol_leaf(NamespaceInventory& inventory, const std::string& source,
                                      const std::string& destination);

    void rename_marker_no_replace(int parent_fd, const std::string& source,
                                  const std::string& destination,
                                  const private_lease::PrivateLeaseRecord& expected_record,
                                  NativeIdentityV1 expected_identity,
                                  const DistributedSievePrivateLeaseClosedSnapshot& successor,
                                  DistributedSievePrivateLeaseRecoveryEdge edge);
    void unlink_exact_marker(int parent_fd, const std::string& leaf,
                             const private_lease::PrivateLeaseRecord& expected_record,
                             NativeIdentityV1 expected_identity,
                             const DistributedSievePrivateLeaseClosedSnapshot& successor,
                             DistributedSievePrivateLeaseRecoveryEdge edge);
    void
    rename_final_directory_to_staging(const DistributedSievePrivateLeaseClosedSnapshot& successor,
                                      DistributedSievePrivateLeaseRecoveryEdge edge);
    void remove_exact_empty_staging_directory(
        const DistributedSievePrivateLeaseClosedSnapshot& successor,
        DistributedSievePrivateLeaseRecoveryEdge edge);
    [[nodiscard]] bool offer_boundary(DistributedSievePrivateLeaseReservationBoundary boundary);

    DistributedSievePrivateLeaseRootClaim& claim_;
    DistributedSievePrivateLeaseRecoveryTestHooks hooks_;
    DistributedSievePrivateLeaseClosedSnapshot current_;
    DistributedSieveWorkerAttemptNamesV1 worker_attempt_names_;
    std::size_t target_index_ = 0;
    std::string staging_directory_leaf_;
    std::optional<DistributedSieveStartedAttemptCleanupAdmission> started_admission_;
    UniqueFd directory_;
    std::optional<DistributedSievePrivateLeaseRecoveryEdge> injected_sync_failure_;
    std::optional<DistributedSievePrivateLeaseReservationBoundary> interrupted_boundary_;
    bool completed_ = false;
    bool rejected_ = false;
};

#endif

#if !defined(_WIN32)

DistributedSieveFdPrivateLeaseRecoveryTarget::DistributedSieveFdPrivateLeaseRecoveryTarget(
    DistributedSievePrivateLeaseRootClaim& claim,
    DistributedSievePrivateLeaseRecoveryTestHooks hooks)
    : claim_(claim), hooks_(hooks) {
    initialize_common();
    require_prestart_record_absent();
    require_current();
}

DistributedSieveFdPrivateLeaseRecoveryTarget::DistributedSieveFdPrivateLeaseRecoveryTarget(
    DistributedSievePrivateLeaseRootClaim& claim,
    DistributedSieveStartedAttemptCleanupAdmission&& admission,
    DistributedSievePrivateLeaseRecoveryTestHooks hooks)
    : claim_(claim), hooks_(hooks), started_admission_(std::move(admission)) {
    initialize_common();
    require_started_record_exact();
    require_current();
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::initialize_common() {
    if (!claim_.owned_by_current_process() || claim_.wave_store_state_ == nullptr ||
        claim_.base_lock_at_ == nullptr || !claim_.worker_attempt_names_.has_value() ||
        !claim_.expected_private_lease_base_lock_leaves_.has_value() ||
        !claim_.expected_private_lease_base_lock_identities_.has_value() ||
        !claim_.expected_private_lease_reservation_witnesses_.has_value() ||
        !claim_.expected_worker_attempt_record_witnesses_.has_value() ||
        claim_.base_lock_acquisition_ !=
            DistributedSievePrivateLeaseRootClaim::BaseLockAcquisition::OpenedExisting) {
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
    }
    if (const auto validated = claim_.revalidate();
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(validated);
    }

    const auto& state = *claim_.wave_store_state_;
    auto observed = capture_manifest_bound_inventory_witness(
        state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
    if (!observed) {
        fail(adjudicate_observation_failure(std::move(observed.diagnostic)));
    }
    if (observed.inventory->private_lease_base_lock_leaves !=
            *claim_.expected_private_lease_base_lock_leaves_ ||
        *observed.base_lock_identities != *claim_.expected_private_lease_base_lock_identities_ ||
        *observed.private_lease_witnesses !=
            *claim_.expected_private_lease_reservation_witnesses_ ||
        observed.inventory->worker_attempt_records !=
            *claim_.expected_worker_attempt_record_witnesses_) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    current_ = private_lease_closed_snapshot(std::move(observed));
    worker_attempt_names_ = *claim_.worker_attempt_names_;

    const auto target = std::lower_bound(current_.inventory.private_lease_base_lock_leaves.begin(),
                                         current_.inventory.private_lease_base_lock_leaves.end(),
                                         worker_attempt_names_.base_lock_leaf);
    if (target == current_.inventory.private_lease_base_lock_leaves.end() ||
        *target != worker_attempt_names_.base_lock_leaf) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    target_index_ = static_cast<std::size_t>(
        std::distance(current_.inventory.private_lease_base_lock_leaves.begin(), target));
    if (target_index_ >= current_.base_lock_identities.size() ||
        target_index_ >= current_.reservation_witnesses.size() ||
        current_.base_lock_identities[target_index_] != claim_.base_lock_at_->identity()) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }

    const auto& initial = current_witness();
    if (initial.base_lock_leaf != worker_attempt_names_.base_lock_leaf ||
        initial.boundary == DistributedSievePrivateLeaseReservationBoundary::Count ||
        initial.work_package_residue.has_value()) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    if (initial.boundary == DistributedSievePrivateLeaseReservationBoundary::PermitAcquired) {
        if (initial.lease_id != std::array<std::uint64_t, 2>{} ||
            initial.reserved_marker_identity.has_value() ||
            initial.directory_identity.has_value() || initial.owner_marker_identity.has_value() ||
            initial.owned_marker_identity.has_value()) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
    } else {
        if (initial.lease_id == std::array<std::uint64_t, 2>{}) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        staging_directory_leaf_ =
            private_lease_staging_directory_leaf(worker_attempt_names_, initial.lease_id);
    }

    if (private_lease_boundary_has_directory(initial.boundary)) {
        if (!initial.directory_identity.has_value()) {
            fail(adjudicate_observation_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        const std::string& named_leaf =
            initial.boundary ==
                    DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable
                ? worker_attempt_names_.private_directory_leaf
                : staging_directory_leaf_;
        auto inspected =
            inspect_private_lease_directory_at(state.root_fd, named_leaf, state.creator_process_id);
        if (!inspected || inspected.identity != initial.directory_identity) {
            fail(adjudicate_observation_failure(
                inspected ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                       protocol_error())
                          : std::move(inspected.diagnostic)));
        }
        directory_ = std::move(inspected.directory);
    }
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::require_prestart_record_absent() const {
    if (started_admission_.has_value() ||
        std::binary_search(current_.inventory.worker_attempt_record_leaves.begin(),
                           current_.inventory.worker_attempt_record_leaves.end(),
                           worker_attempt_names_.canonical_record_leaf) ||
        std::binary_search(current_.inventory.worker_attempt_record_leaves.begin(),
                           current_.inventory.worker_attempt_record_leaves.end(),
                           worker_attempt_names_.pending_record_leaf)) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::require_started_record_exact() const {
    if (!started_admission_.has_value() ||
        started_admission_->worker_attempt_names_ != worker_attempt_names_ ||
        started_admission_->creator_process_id_ != claim_.creator_process_id_ ||
        !started_admission_->record_witness_.canonical_snapshot.has_value() ||
        started_admission_->record_witness_.pending_snapshot.has_value() ||
        !std::binary_search(current_.inventory.worker_attempt_record_leaves.begin(),
                            current_.inventory.worker_attempt_record_leaves.end(),
                            worker_attempt_names_.canonical_record_leaf) ||
        std::binary_search(current_.inventory.worker_attempt_record_leaves.begin(),
                           current_.inventory.worker_attempt_record_leaves.end(),
                           worker_attempt_names_.pending_record_leaf)) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    const auto& expected = started_admission_->record_witness_;
    const auto target = std::lower_bound(
        current_.inventory.worker_attempt_records.begin(),
        current_.inventory.worker_attempt_records.end(),
        std::pair{expected.chunk_id, expected.attempt_ordinal},
        [](const DistributedSieveWorkerAttemptRecordInventoryWitness& candidate,
           const std::pair<std::uint32_t, std::uint32_t>& coordinate) {
            return std::pair{candidate.chunk_id, candidate.attempt_ordinal} < coordinate;
        });
    if (target == current_.inventory.worker_attempt_records.end() || !(*target == expected)) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    if (const auto pinned = validate_started_record_pin();
        pinned.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(pinned));
    }
}

[[noreturn]] void
DistributedSieveFdPrivateLeaseRecoveryTarget::fail(DistributedSieveWaveStoreDiagnostic outcome) {
    throw Failure{std::move(outcome)};
}

const PrivateLeaseReservationWitness&
DistributedSieveFdPrivateLeaseRecoveryTarget::current_witness() const {
    if (target_index_ >= current_.reservation_witnesses.size()) {
        fail(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error()));
    }
    return current_.reservation_witnesses[target_index_];
}

private_lease::PrivateLeaseReservationBoundary
DistributedSieveFdPrivateLeaseRecoveryTarget::boundary() const {
    return static_cast<private_lease::PrivateLeaseReservationBoundary>(
        static_cast<std::uint8_t>(current_witness().boundary));
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::require_phase(
    DistributedSievePrivateLeaseReservationBoundary expected) const {
    if (current_witness().boundary != expected) {
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
    }
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::before_mutation() {
    require_current();
    if (current_witness().work_package_residue.has_value()) {
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::reconciliation_required, protocol_error()));
    }
    if (!process_matches(claim_.creator_process_id_)) {
        fail(process_mismatch());
    }
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::select_injected_sync_failure(
    DistributedSievePrivateLeaseRecoveryEdge edge) {
    injected_sync_failure_.reset();
    require_current();
    if (hooks_.fail_before_sync == nullptr) {
        return;
    }
    if (!process_matches(claim_.creator_process_id_)) {
        fail(process_mismatch());
    }
    const bool selected = hooks_.fail_before_sync(edge, hooks_.context);
    if (!process_matches(claim_.creator_process_id_)) {
        fail(process_mismatch());
    }
    require_current();
    if (selected) {
        injected_sync_failure_ = edge;
    }
}

private_lease::PrivateLeaseRecord
DistributedSieveFdPrivateLeaseRecoveryTarget::expected_reserved_record() const {
    const auto& state = *claim_.wave_store_state_;
    const auto& witness = current_witness();
    if (witness.lease_id == std::array<std::uint64_t, 2>{}) {
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
    }
    const std::filesystem::path base_path =
        state.absolute_root / worker_attempt_names_.private_directory_leaf / "corpus";
    return {
        .platform_id = private_lease::PLATFORM_ID,
        .phase = private_lease::PrivateLeasePhase::Reserved,
        .capability = private_lease::PrivateLeaseCapability::RollbackStagingOnly,
        .lease_id = witness.lease_id,
        .base_path_digest = private_lease::frozen_path_digest(base_path),
        .parent_identity = relation_identity(state.root_identity),
        .lock_identity = relation_identity(claim_.base_lock_at_->identity()),
    };
}

private_lease::PrivateLeaseRecord
DistributedSieveFdPrivateLeaseRecoveryTarget::expected_owner_record() const {
    if (!current_witness().directory_identity.has_value()) {
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
    }
    return private_lease::make_private_lease_owner_record(
        expected_reserved_record(), relation_identity(*current_witness().directory_identity));
}

private_lease::PrivateLeaseRecord
DistributedSieveFdPrivateLeaseRecoveryTarget::expected_owned_record() const {
    if (!current_witness().owner_marker_identity.has_value()) {
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
    }
    return private_lease::make_private_lease_owned_record(
        expected_owner_record(), relation_identity(*current_witness().owner_marker_identity));
}

PrivateLeaseMarkerAtResult DistributedSieveFdPrivateLeaseRecoveryTarget::hold_exact_marker(
    int parent_fd, const std::string& leaf,
    const private_lease::PrivateLeaseRecord& expected_record,
    const NativeIdentityV1& expected_identity) const {
    auto held = read_private_lease_marker_at(parent_fd, leaf, claim_.creator_process_id_);
    if (!held) {
        fail(adjudicate_observation_failure(std::move(held.diagnostic)));
    }
    if (*held.record != expected_record || *held.identity != expected_identity) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    if (const auto validated = validate_named_private_lease_marker_handle(
            parent_fd, leaf.c_str(), held.marker.get(), expected_record, expected_identity,
            claim_.creator_process_id_);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(validated));
    }
    return held;
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::insert_protocol_leaf(
    NamespaceInventory& inventory, const std::string& leaf) {
    const auto position = std::lower_bound(inventory.private_lease_protocol_leaves.begin(),
                                           inventory.private_lease_protocol_leaves.end(), leaf);
    if (position != inventory.private_lease_protocol_leaves.end() && *position == leaf) {
        fail(diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    inventory.private_lease_protocol_leaves.insert(position, leaf);
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::erase_protocol_leaf(
    NamespaceInventory& inventory, const std::string& leaf) {
    const auto position = std::lower_bound(inventory.private_lease_protocol_leaves.begin(),
                                           inventory.private_lease_protocol_leaves.end(), leaf);
    if (position == inventory.private_lease_protocol_leaves.end() || *position != leaf) {
        fail(diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    inventory.private_lease_protocol_leaves.erase(position);
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::replace_protocol_leaf(
    NamespaceInventory& inventory, const std::string& source, const std::string& destination) {
    erase_protocol_leaf(inventory, source);
    insert_protocol_leaf(inventory, destination);
}

DistributedSievePrivateLeaseClosedSnapshot
DistributedSieveFdPrivateLeaseRecoveryTarget::expected_successor(
    private_lease::PrivateLeaseRecoveryTransition transition) const {
    const auto source = wave_private_lease_boundary(transition.source);
    const auto successor = wave_private_lease_boundary(transition.successor);
    const auto transition_index =
        source == DistributedSievePrivateLeaseReservationBoundary::PermitAcquired
            ? private_lease::PRIVATE_LEASE_RECOVERY_TRANSITIONS.size()
            : static_cast<std::size_t>(source) - 1U;
    if (source == DistributedSievePrivateLeaseReservationBoundary::Count ||
        successor == DistributedSievePrivateLeaseReservationBoundary::Count ||
        source != current_witness().boundary ||
        static_cast<std::size_t>(successor) + 1U != static_cast<std::size_t>(source) ||
        transition_index >= private_lease::PRIVATE_LEASE_RECOVERY_TRANSITIONS.size() ||
        private_lease::PRIVATE_LEASE_RECOVERY_TRANSITIONS[transition_index] != transition) {
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
    }

    DistributedSievePrivateLeaseClosedSnapshot expected = current_;
    auto& witness = expected.reservation_witnesses[target_index_];
    witness.boundary = successor;
    if (successor == DistributedSievePrivateLeaseReservationBoundary::PermitAcquired) {
        witness.lease_id = {};
    }
    if (!private_lease_boundary_has_reserved(successor)) {
        witness.reserved_marker_identity.reset();
    }
    if (!private_lease_boundary_has_directory(successor)) {
        witness.directory_identity.reset();
    }
    if (!private_lease_boundary_has_owner(successor)) {
        witness.owner_marker_identity.reset();
    }
    if (!private_lease_boundary_has_owned(successor)) {
        witness.owned_marker_identity.reset();
    }

    switch (transition.action) {
    case private_lease::PrivateLeaseRecoveryAction::UnlinkExactReservedPending:
        erase_protocol_leaf(expected.inventory, worker_attempt_names_.reserved_pending_leaf);
        break;
    case private_lease::PrivateLeaseRecoveryAction::RenameReservedCanonicalToPendingNoReplace:
        replace_protocol_leaf(expected.inventory, worker_attempt_names_.reserved_leaf,
                              worker_attempt_names_.reserved_pending_leaf);
        break;
    case private_lease::PrivateLeaseRecoveryAction::RemoveExactEmptyStagingDirectory:
        erase_protocol_leaf(expected.inventory, staging_directory_leaf_);
        break;
    case private_lease::PrivateLeaseRecoveryAction::UnlinkExactOwnerPending:
    case private_lease::PrivateLeaseRecoveryAction::RenameOwnerCanonicalToPendingNoReplace:
        break;
    case private_lease::PrivateLeaseRecoveryAction::UnlinkExactOwnedPending:
        erase_protocol_leaf(expected.inventory, worker_attempt_names_.owned_pending_leaf);
        break;
    case private_lease::PrivateLeaseRecoveryAction::RenameOwnedCanonicalToPendingNoReplace:
        replace_protocol_leaf(expected.inventory, worker_attempt_names_.owned_leaf,
                              worker_attempt_names_.owned_pending_leaf);
        break;
    case private_lease::PrivateLeaseRecoveryAction::RenameFinalDirectoryToStagingNoReplace:
        replace_protocol_leaf(expected.inventory, worker_attempt_names_.private_directory_leaf,
                              staging_directory_leaf_);
        break;
    case private_lease::PrivateLeaseRecoveryAction::Count:
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
    }
    return expected;
}

DistributedSieveWaveStoreDiagnostic
DistributedSieveFdPrivateLeaseRecoveryTarget::adjudicate_observation_failure(
    DistributedSieveWaveStoreDiagnostic lower) const noexcept {
    if (const auto authority = claim_.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    if (const auto pinned = validate_started_record_pin();
        pinned.status != DistributedSieveWaveStoreStatus::ready) {
        return pinned;
    }
    return lower;
}

DistributedSieveWaveStoreDiagnostic
DistributedSieveFdPrivateLeaseRecoveryTarget::validate_started_record_pin() const noexcept {
    if (!started_admission_.has_value()) {
        return {};
    }
    if (claim_.wave_store_state_ == nullptr) {
        return diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                          invalid_argument_error());
    }
    return started_admission_->revalidate(claim_.wave_store_state_->root_fd);
}

DistributedSieveWaveStoreDiagnostic
DistributedSieveFdPrivateLeaseRecoveryTarget::validate_directory_handle(
    const PrivateLeaseReservationWitness& witness) const noexcept {
    if (!private_lease_boundary_has_directory(witness.boundary)) {
        return directory_ ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                       protocol_error())
                          : DistributedSieveWaveStoreDiagnostic{};
    }
    if (!directory_ || !witness.directory_identity.has_value()) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    }
    const std::string& named_leaf =
        witness.boundary == DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable
            ? worker_attempt_names_.private_directory_leaf
            : staging_directory_leaf_;
    return validate_private_lease_directory_binding(
        claim_.wave_store_state_->root_fd, directory_.get(), named_leaf, claim_.creator_process_id_,
        *witness.directory_identity);
}

DistributedSieveWaveStoreDiagnostic
DistributedSieveFdPrivateLeaseRecoveryTarget::validate_exact_snapshot(
    const DistributedSievePrivateLeaseClosedSnapshot& expected) const noexcept {
    const auto& state = *claim_.wave_store_state_;
    if (const auto authority = claim_.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    auto first = capture_manifest_bound_inventory_witness(
        state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
    if (!first) {
        return adjudicate_observation_failure(std::move(first.diagnostic));
    }
    if (*first.inventory != expected.inventory ||
        *first.base_lock_identities != expected.base_lock_identities ||
        *first.private_lease_witnesses != expected.reservation_witnesses) {
        return adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (const auto pinned = validate_started_record_pin();
        pinned.status != DistributedSieveWaveStoreStatus::ready) {
        return adjudicate_observation_failure(pinned);
    }
    if (const auto directory =
            validate_directory_handle(expected.reservation_witnesses[target_index_]);
        directory.status != DistributedSieveWaveStoreStatus::ready) {
        return adjudicate_observation_failure(directory);
    }
    if (const auto authority = claim_.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    auto confirmed = capture_manifest_bound_inventory_witness(
        state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
    if (!confirmed) {
        return adjudicate_observation_failure(std::move(confirmed.diagnostic));
    }
    if (*confirmed.inventory != *first.inventory ||
        *confirmed.base_lock_identities != *first.base_lock_identities ||
        *confirmed.private_lease_witnesses != *first.private_lease_witnesses ||
        *confirmed.inventory != expected.inventory ||
        *confirmed.base_lock_identities != expected.base_lock_identities ||
        *confirmed.private_lease_witnesses != expected.reservation_witnesses) {
        return adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (const auto pinned = validate_started_record_pin();
        pinned.status != DistributedSieveWaveStoreStatus::ready) {
        return adjudicate_observation_failure(pinned);
    }
    if (const auto directory =
            validate_directory_handle(expected.reservation_witnesses[target_index_]);
        directory.status != DistributedSieveWaveStoreStatus::ready) {
        return adjudicate_observation_failure(directory);
    }
    return claim_.revalidate_authority();
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::require_current() const {
    if (const auto validated = validate_exact_snapshot(current_);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(validated);
    }
}

template <typename LocalValidator>
DistributedSieveWaveStoreDiagnostic
DistributedSieveFdPrivateLeaseRecoveryTarget::adjudicate_operation_failure(
    DistributedSieveWaveStoreDiagnostic lower,
    const DistributedSievePrivateLeaseClosedSnapshot& possible_successor,
    DistributedSievePrivateLeaseRecoveryEdge edge, LocalValidator&& validate_local) const noexcept {
    const auto& state = *claim_.wave_store_state_;
    if (const auto authority = claim_.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    auto first = capture_manifest_bound_inventory_witness(
        state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
    if (!first) {
        return adjudicate_observation_failure(std::move(first.diagnostic));
    }
    const bool predecessor = *first.inventory == current_.inventory &&
                             *first.base_lock_identities == current_.base_lock_identities &&
                             *first.private_lease_witnesses == current_.reservation_witnesses;
    const bool successor =
        *first.inventory == possible_successor.inventory &&
        *first.base_lock_identities == possible_successor.base_lock_identities &&
        *first.private_lease_witnesses == possible_successor.reservation_witnesses;
    if (predecessor == successor) {
        return adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (const auto pinned = validate_started_record_pin();
        pinned.status != DistributedSieveWaveStoreStatus::ready) {
        return adjudicate_observation_failure(pinned);
    }
    if (const auto local = validate_local(successor);
        local.status != DistributedSieveWaveStoreStatus::ready) {
        return adjudicate_observation_failure(local);
    }
    if (successor && hooks_.after_first_successor_validation != nullptr) {
        if (!process_matches(claim_.creator_process_id_)) {
            return process_mismatch();
        }
        hooks_.after_first_successor_validation(edge, hooks_.context);
        if (!process_matches(claim_.creator_process_id_)) {
            return process_mismatch();
        }
    }
    if (const auto authority = claim_.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    auto confirmed = capture_manifest_bound_inventory_witness(
        state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
    if (!confirmed) {
        return adjudicate_observation_failure(std::move(confirmed.diagnostic));
    }
    if (*confirmed.inventory != *first.inventory ||
        *confirmed.base_lock_identities != *first.base_lock_identities ||
        *confirmed.private_lease_witnesses != *first.private_lease_witnesses) {
        return adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (const auto pinned = validate_started_record_pin();
        pinned.status != DistributedSieveWaveStoreStatus::ready) {
        return adjudicate_observation_failure(pinned);
    }
    if (const auto local = validate_local(successor);
        local.status != DistributedSieveWaveStoreStatus::ready) {
        return adjudicate_observation_failure(local);
    }
    if (const auto authority = claim_.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    return lower;
}

template <typename LocalValidator>
void DistributedSieveFdPrivateLeaseRecoveryTarget::accept_successor(
    const DistributedSievePrivateLeaseClosedSnapshot& expected,
    DistributedSievePrivateLeaseRecoveryEdge edge, LocalValidator&& validate_local) {
    const auto& state = *claim_.wave_store_state_;
    if (const auto authority = claim_.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        fail(authority);
    }
    auto first = capture_manifest_bound_inventory_witness(
        state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
    if (!first) {
        fail(adjudicate_observation_failure(std::move(first.diagnostic)));
    }
    if (*first.inventory != expected.inventory ||
        *first.base_lock_identities != expected.base_lock_identities ||
        *first.private_lease_witnesses != expected.reservation_witnesses) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    if (const auto pinned = validate_started_record_pin();
        pinned.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(pinned));
    }
    if (const auto directory =
            validate_directory_handle(expected.reservation_witnesses[target_index_]);
        directory.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(directory));
    }
    if (const auto local = validate_local(true);
        local.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(local));
    }
    if (!process_matches(claim_.creator_process_id_)) {
        fail(process_mismatch());
    }
    if (hooks_.after_first_successor_validation != nullptr) {
        hooks_.after_first_successor_validation(edge, hooks_.context);
    }
    if (!process_matches(claim_.creator_process_id_)) {
        fail(process_mismatch());
    }
    if (const auto authority = claim_.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        fail(authority);
    }
    auto confirmed = capture_manifest_bound_inventory_witness(
        state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
    if (!confirmed) {
        fail(adjudicate_observation_failure(std::move(confirmed.diagnostic)));
    }
    if (*confirmed.inventory != *first.inventory ||
        *confirmed.base_lock_identities != *first.base_lock_identities ||
        *confirmed.private_lease_witnesses != *first.private_lease_witnesses ||
        *confirmed.inventory != expected.inventory ||
        *confirmed.base_lock_identities != expected.base_lock_identities ||
        *confirmed.private_lease_witnesses != expected.reservation_witnesses) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    if (const auto pinned = validate_started_record_pin();
        pinned.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(pinned));
    }
    if (const auto directory =
            validate_directory_handle(expected.reservation_witnesses[target_index_]);
        directory.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(directory));
    }
    if (const auto local = validate_local(true);
        local.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(local));
    }
    if (const auto authority = claim_.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        fail(authority);
    }
    current_ = private_lease_closed_snapshot(std::move(first));
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::rename_marker_no_replace(
    int parent_fd, const std::string& source, const std::string& destination,
    const private_lease::PrivateLeaseRecord& expected_record, NativeIdentityV1 expected_identity,
    const DistributedSievePrivateLeaseClosedSnapshot& successor,
    DistributedSievePrivateLeaseRecoveryEdge edge) {
    before_mutation();
    auto held = hold_exact_marker(parent_fd, source, expected_record, expected_identity);
    const auto validate_local = [&](bool visible_successor) noexcept {
        const std::string& leaf = visible_successor ? destination : source;
        return validate_named_private_lease_marker_handle(
            parent_fd, leaf.c_str(), held.marker.get(), expected_record, expected_identity,
            claim_.creator_process_id_);
    };
    require_current();
    if (const auto renamed =
            private_lease_rename_no_replace_at(parent_fd, source.c_str(), destination.c_str());
        renamed.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(renamed, successor, edge, validate_local));
    }
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(validated, successor, edge, validate_local));
    }
    if (const auto synchronized =
            private_lease_recovery_sync_handle(parent_fd, edge, injected_sync_failure_);
        synchronized.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(synchronized, successor, edge, validate_local));
    }
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(validated, successor, edge, validate_local));
    }
    accept_successor(successor, edge, validate_local);
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(validated));
    }
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::unlink_exact_marker(
    int parent_fd, const std::string& leaf,
    const private_lease::PrivateLeaseRecord& expected_record, NativeIdentityV1 expected_identity,
    const DistributedSievePrivateLeaseClosedSnapshot& successor,
    DistributedSievePrivateLeaseRecoveryEdge edge) {
    before_mutation();
    auto held = hold_exact_marker(parent_fd, leaf, expected_record, expected_identity);
    const auto validate_local = [&](bool visible_successor) noexcept {
        return visible_successor ? validate_unlinked_private_lease_marker_handle(
                                       parent_fd, leaf.c_str(), held.marker.get(), expected_record,
                                       expected_identity, claim_.creator_process_id_)
                                 : validate_named_private_lease_marker_handle(
                                       parent_fd, leaf.c_str(), held.marker.get(), expected_record,
                                       expected_identity, claim_.creator_process_id_);
    };
    require_current();
    if (const auto removed = private_lease_unlink_at(parent_fd, leaf.c_str(), 0);
        removed.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(removed, successor, edge, validate_local));
    }
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(validated, successor, edge, validate_local));
    }
    if (const auto synchronized =
            private_lease_recovery_sync_handle(parent_fd, edge, injected_sync_failure_);
        synchronized.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(synchronized, successor, edge, validate_local));
    }
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(validated, successor, edge, validate_local));
    }
    accept_successor(successor, edge, validate_local);
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(validated));
    }
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::rename_final_directory_to_staging(
    const DistributedSievePrivateLeaseClosedSnapshot& successor,
    DistributedSievePrivateLeaseRecoveryEdge edge) {
    require_phase(DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable);
    before_mutation();
    if (!directory_ || !current_witness().directory_identity.has_value()) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    const auto identity = *current_witness().directory_identity;
    const auto& state = *claim_.wave_store_state_;
    const auto validate_local = [&](bool visible_successor) noexcept {
        const std::string& leaf = visible_successor ? staging_directory_leaf_
                                                    : worker_attempt_names_.private_directory_leaf;
        return validate_private_lease_directory_binding(state.root_fd, directory_.get(), leaf,
                                                        claim_.creator_process_id_, identity);
    };
    if (const auto validated = validate_local(false);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(validated));
    }
    require_current();
    if (const auto renamed = private_lease_rename_no_replace_at(
            state.root_fd, worker_attempt_names_.private_directory_leaf.c_str(),
            staging_directory_leaf_.c_str());
        renamed.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(renamed, successor, edge, validate_local));
    }
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(validated, successor, edge, validate_local));
    }
    if (const auto synchronized =
            private_lease_recovery_sync_handle(state.root_fd, edge, injected_sync_failure_);
        synchronized.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(synchronized, successor, edge, validate_local));
    }
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(validated, successor, edge, validate_local));
    }
    accept_successor(successor, edge, validate_local);
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(validated));
    }
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::remove_exact_empty_staging_directory(
    const DistributedSievePrivateLeaseClosedSnapshot& successor,
    DistributedSievePrivateLeaseRecoveryEdge edge) {
    require_phase(DistributedSievePrivateLeaseReservationBoundary::StagingDirectoryDurable);
    before_mutation();
    if (!directory_ || !current_witness().directory_identity.has_value()) {
        fail(adjudicate_observation_failure(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
    }
    const auto identity = *current_witness().directory_identity;
    const auto& state = *claim_.wave_store_state_;
    if (const auto validated = validate_private_lease_directory_binding(
            state.root_fd, directory_.get(), staging_directory_leaf_, claim_.creator_process_id_,
            identity);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(validated));
    }
    if (const auto empty = validate_exact_empty_private_lease_directory_handle(
            directory_.get(), claim_.creator_process_id_);
        empty.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(empty));
    }
    require_current();

    if (hooks_.before_staging_directory_remove != nullptr) {
        if (!process_matches(claim_.creator_process_id_)) {
            fail(process_mismatch());
        }
        hooks_.before_staging_directory_remove(edge, hooks_.context);
        if (!process_matches(claim_.creator_process_id_)) {
            fail(process_mismatch());
        }
        require_current();
    }

    UniqueFd removed_directory = std::move(directory_);
    const auto validate_local = [&](bool visible_successor) noexcept {
        if (visible_successor) {
            return validate_unlinked_private_lease_directory_handle(
                state.root_fd, staging_directory_leaf_.c_str(), removed_directory.get(), identity,
                claim_.creator_process_id_);
        }
        if (const auto bound = validate_private_lease_directory_binding(
                state.root_fd, removed_directory.get(), staging_directory_leaf_,
                claim_.creator_process_id_, identity);
            bound.status != DistributedSieveWaveStoreStatus::ready) {
            return bound;
        }
        return validate_exact_empty_private_lease_directory_handle(removed_directory.get(),
                                                                   claim_.creator_process_id_);
    };
    if (const auto validated = validate_local(false);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(validated));
    }
    if (const auto removed =
            private_lease_unlink_at(state.root_fd, staging_directory_leaf_.c_str(), AT_REMOVEDIR);
        removed.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(removed, successor, edge, validate_local));
    }
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(validated, successor, edge, validate_local));
    }
    if (const auto synchronized =
            private_lease_recovery_sync_handle(state.root_fd, edge, injected_sync_failure_);
        synchronized.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(synchronized, successor, edge, validate_local));
    }
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_operation_failure(validated, successor, edge, validate_local));
    }
    accept_successor(successor, edge, validate_local);
    if (const auto validated = validate_local(true);
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(adjudicate_observation_failure(validated));
    }
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::apply(
    private_lease::PrivateLeaseRecoveryTransition transition) {
    const auto successor = expected_successor(transition);
    const DistributedSievePrivateLeaseRecoveryEdge edge{
        .source = wave_private_lease_boundary(transition.source),
        .successor = wave_private_lease_boundary(transition.successor),
    };
    select_injected_sync_failure(edge);
    const int root_fd = claim_.wave_store_state_->root_fd;

    switch (transition.action) {
    case private_lease::PrivateLeaseRecoveryAction::UnlinkExactReservedPending: {
        require_phase(DistributedSievePrivateLeaseReservationBoundary::ReservedPendingDurable);
        if (!current_witness().reserved_marker_identity.has_value()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
        const auto record = expected_reserved_record();
        unlink_exact_marker(root_fd, worker_attempt_names_.reserved_pending_leaf, record,
                            *current_witness().reserved_marker_identity, successor, edge);
        return;
    }
    case private_lease::PrivateLeaseRecoveryAction::RenameReservedCanonicalToPendingNoReplace: {
        require_phase(DistributedSievePrivateLeaseReservationBoundary::ReservedCanonicalDurable);
        if (!current_witness().reserved_marker_identity.has_value()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
        const auto record = expected_reserved_record();
        rename_marker_no_replace(root_fd, worker_attempt_names_.reserved_leaf,
                                 worker_attempt_names_.reserved_pending_leaf, record,
                                 *current_witness().reserved_marker_identity, successor, edge);
        return;
    }
    case private_lease::PrivateLeaseRecoveryAction::RemoveExactEmptyStagingDirectory:
        remove_exact_empty_staging_directory(successor, edge);
        return;
    case private_lease::PrivateLeaseRecoveryAction::UnlinkExactOwnerPending: {
        require_phase(DistributedSievePrivateLeaseReservationBoundary::OwnerPendingDurable);
        if (!directory_ || !current_witness().owner_marker_identity.has_value()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
        const auto record = expected_owner_record();
        unlink_exact_marker(directory_.get(),
                            std::string(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF), record,
                            *current_witness().owner_marker_identity, successor, edge);
        return;
    }
    case private_lease::PrivateLeaseRecoveryAction::RenameOwnerCanonicalToPendingNoReplace: {
        require_phase(DistributedSievePrivateLeaseReservationBoundary::OwnerCanonicalDurable);
        if (!directory_ || !current_witness().owner_marker_identity.has_value()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
        const auto record = expected_owner_record();
        rename_marker_no_replace(directory_.get(),
                                 std::string(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF),
                                 std::string(DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF),
                                 record, *current_witness().owner_marker_identity, successor, edge);
        return;
    }
    case private_lease::PrivateLeaseRecoveryAction::UnlinkExactOwnedPending: {
        require_phase(DistributedSievePrivateLeaseReservationBoundary::OwnedPendingDurable);
        if (!current_witness().owned_marker_identity.has_value()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
        const auto record = expected_owned_record();
        unlink_exact_marker(root_fd, worker_attempt_names_.owned_pending_leaf, record,
                            *current_witness().owned_marker_identity, successor, edge);
        return;
    }
    case private_lease::PrivateLeaseRecoveryAction::RenameOwnedCanonicalToPendingNoReplace: {
        require_phase(DistributedSievePrivateLeaseReservationBoundary::OwnedCanonicalDurable);
        if (!current_witness().owned_marker_identity.has_value()) {
            fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                            invalid_argument_error()));
        }
        const auto record = expected_owned_record();
        rename_marker_no_replace(root_fd, worker_attempt_names_.owned_leaf,
                                 worker_attempt_names_.owned_pending_leaf, record,
                                 *current_witness().owned_marker_identity, successor, edge);
        return;
    }
    case private_lease::PrivateLeaseRecoveryAction::RenameFinalDirectoryToStagingNoReplace:
        rename_final_directory_to_staging(successor, edge);
        return;
    case private_lease::PrivateLeaseRecoveryAction::Count:
        break;
    }
    fail(diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
}

bool DistributedSieveFdPrivateLeaseRecoveryTarget::offer_boundary(
    DistributedSievePrivateLeaseReservationBoundary boundary) {
    require_current();
    if (!process_matches(claim_.creator_process_id_)) {
        fail(process_mismatch());
    }
    const bool stop = hooks_.stop_after != nullptr && hooks_.stop_after(boundary, hooks_.context);
    if (!process_matches(claim_.creator_process_id_)) {
        fail(process_mismatch());
    }
    require_current();
    if (stop) {
        interrupted_boundary_ = boundary;
    }
    return stop;
}

bool DistributedSieveFdPrivateLeaseRecoveryTarget::checkpoint(
    private_lease::PrivateLeaseReservationBoundary boundary) {
    const auto wave_boundary = wave_private_lease_boundary(boundary);
    if (wave_boundary == DistributedSievePrivateLeaseReservationBoundary::Count ||
        current_witness().boundary != wave_boundary) {
        fail(
            diagnostic(DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error()));
    }
    return offer_boundary(wave_boundary);
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::complete() {
    require_phase(DistributedSievePrivateLeaseReservationBoundary::PermitAcquired);
    require_current();
    *claim_.expected_private_lease_base_lock_leaves_ =
        current_.inventory.private_lease_base_lock_leaves;
    *claim_.expected_private_lease_base_lock_identities_ = current_.base_lock_identities;
    *claim_.expected_private_lease_reservation_witnesses_ = current_.reservation_witnesses;
    *claim_.expected_worker_attempt_record_witnesses_ = current_.inventory.worker_attempt_records;
    if (const auto validated = claim_.revalidate();
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        fail(validated);
    }
    completed_ = true;
}

void DistributedSieveFdPrivateLeaseRecoveryTarget::reject_invalid_boundary() noexcept {
    rejected_ = true;
}

bool DistributedSieveFdPrivateLeaseRecoveryTarget::completed() const noexcept {
    return completed_ && !rejected_ && target_index_ < current_.reservation_witnesses.size() &&
           current_.reservation_witnesses[target_index_].boundary ==
               DistributedSievePrivateLeaseReservationBoundary::PermitAcquired;
}

std::optional<DistributedSieveReconciledWorkerAttemptV1>
DistributedSieveFdPrivateLeaseRecoveryTarget::started_completion() const {
    if (!completed() || !started_admission_.has_value() || claim_.wave_store_state_ == nullptr) {
        return std::nullopt;
    }
    require_current();
    require_started_record_exact();
    const auto& witness = started_admission_->record_witness_;
    std::optional<std::uint32_t> next_attempt_ordinal;
    if (witness.attempt_ordinal + 1U < claim_.wave_store_state_->manifest.max_worker_attempts) {
        next_attempt_ordinal = witness.attempt_ordinal + 1U;
    }
    return DistributedSieveReconciledWorkerAttemptV1{
        .record = witness.record,
        .canonical_snapshot = *witness.canonical_snapshot,
        .next_attempt_ordinal = next_attempt_ordinal,
    };
}

DistributedSieveWaveStoreDiagnostic
DistributedSieveFdPrivateLeaseRecoveryTarget::interruption_diagnostic() const noexcept {
    auto outcome = diagnostic(DistributedSieveWaveStoreStatus::interrupted,
                              std::make_error_code(std::errc::operation_canceled));
    outcome.last_private_lease_recovery_boundary = interrupted_boundary_;
    return outcome;
}

#endif

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
        std::optional<WaveManifestV1> existing_inventory_manifest;
        if (before_publish.inventory->manifest || before_publish.inventory->pending ||
            !before_publish.inventory->private_lease_base_lock_leaves.empty()) {
            auto existing = read_existing_manifest(root.root.get(), *before_publish.inventory,
                                                   expected_manifest_digest, root.root_identity,
                                                   lock.lock_identity, creator_process_id);
            if (!existing) {
                return open_failure(std::move(existing.diagnostic));
            }
            existing_inventory_manifest = std::move(existing.manifest);
        }
        if (has_pre_manifest_private_lease_candidate(*before_publish.inventory)) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        const WaveManifestV1& inventory_manifest = existing_inventory_manifest.has_value()
                                                       ? *existing_inventory_manifest
                                                       : sealed_manifest;
        before_publish_private_inventory = validate_private_lease_base_lock_inventory(
            root.root.get(), *before_publish.inventory, inventory_manifest, creator_process_id);
        if (!before_publish_private_inventory) {
            return open_failure(before_publish_private_inventory.diagnostic);
        }
        auto before_publish_private_leases = validate_private_lease_protocol_inventory(
            root.root.get(), frozen->absolute, *before_publish.inventory, inventory_manifest,
            *before_publish_private_inventory.identities, creator_process_id);
        if (!before_publish_private_leases) {
            return open_failure(std::move(before_publish_private_leases.diagnostic));
        }
        auto before_publish_worker_attempts = validate_worker_attempt_record_inventory(
            root.root.get(), *before_publish.inventory, inventory_manifest,
            *before_publish_private_inventory.identities, *before_publish_private_leases.witnesses,
            creator_process_id);
        if (!before_publish_worker_attempts) {
            return open_failure(std::move(before_publish_worker_attempts.diagnostic));
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
            root.root.get(), frozen->absolute, *final_inventory.inventory, sealed_manifest,
            creator_process_id);
        if (!final_inventory_validated) {
            return open_failure(final_inventory_validated.diagnostic);
        }
        if (final_inventory.inventory->private_lease_base_lock_leaves !=
                before_publish.inventory->private_lease_base_lock_leaves ||
            final_inventory.inventory->private_lease_protocol_leaves !=
                before_publish.inventory->private_lease_protocol_leaves ||
            final_inventory.inventory->worker_attempt_record_leaves !=
                before_publish.inventory->worker_attempt_record_leaves ||
            *final_inventory_validated.base_lock_identities !=
                *before_publish_private_inventory.identities ||
            *final_inventory_validated.private_lease_witnesses !=
                *before_publish_private_leases.witnesses ||
            *final_inventory_validated.worker_attempt_records !=
                *before_publish_worker_attempts.witnesses) {
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
        if (has_pre_manifest_private_lease_candidate(*inventory.inventory)) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        const auto inventory_validated = validate_private_lease_base_lock_inventory(
            root.root.get(), *inventory.inventory, *existing.manifest, creator_process_id);
        if (!inventory_validated) {
            return open_failure(inventory_validated.diagnostic);
        }
        auto private_leases = validate_private_lease_protocol_inventory(
            root.root.get(), frozen->absolute, *inventory.inventory, *existing.manifest,
            *inventory_validated.identities, creator_process_id);
        if (!private_leases) {
            return open_failure(std::move(private_leases.diagnostic));
        }
        auto worker_attempts = validate_worker_attempt_record_inventory(
            root.root.get(), *inventory.inventory, *existing.manifest,
            *inventory_validated.identities, *private_leases.witnesses, creator_process_id);
        if (!worker_attempts) {
            return open_failure(std::move(worker_attempts.diagnostic));
        }

        const auto after_read = inspect_namespace(root.root.get());
        if (!after_read) {
            return open_failure(std::move(after_read.diagnostic));
        }
        if (*after_read.inventory != *inventory.inventory) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (has_pre_manifest_private_lease_candidate(*after_read.inventory)) {
            return open_failure(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        const auto after_read_inventory_validated = validate_private_lease_base_lock_inventory(
            root.root.get(), *after_read.inventory, *existing.manifest, creator_process_id);
        if (!after_read_inventory_validated) {
            return open_failure(after_read_inventory_validated.diagnostic);
        }
        auto after_read_private_leases = validate_private_lease_protocol_inventory(
            root.root.get(), frozen->absolute, *after_read.inventory, *existing.manifest,
            *after_read_inventory_validated.identities, creator_process_id);
        if (!after_read_private_leases) {
            return open_failure(std::move(after_read_private_leases.diagnostic));
        }
        auto after_read_worker_attempts = validate_worker_attempt_record_inventory(
            root.root.get(), *after_read.inventory, *existing.manifest,
            *after_read_inventory_validated.identities, *after_read_private_leases.witnesses,
            creator_process_id);
        if (!after_read_worker_attempts) {
            return open_failure(std::move(after_read_worker_attempts.diagnostic));
        }
        if (*after_read_inventory_validated.identities != *inventory_validated.identities ||
            *after_read_private_leases.witnesses != *private_leases.witnesses ||
            *after_read_worker_attempts.witnesses != *worker_attempts.witnesses) {
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
        const auto final_inventory_validated = validate_manifest_bound_inventory(
            root.root.get(), frozen->absolute, *final_inventory.inventory, *final_manifest.manifest,
            creator_process_id);
        if (!final_inventory_validated) {
            return open_failure(final_inventory_validated.diagnostic);
        }
        if (final_inventory.inventory->private_lease_base_lock_leaves !=
                after_read.inventory->private_lease_base_lock_leaves ||
            final_inventory.inventory->private_lease_protocol_leaves !=
                after_read.inventory->private_lease_protocol_leaves ||
            final_inventory.inventory->worker_attempt_record_leaves !=
                after_read.inventory->worker_attempt_record_leaves ||
            *final_inventory_validated.base_lock_identities !=
                *after_read_inventory_validated.identities ||
            *final_inventory_validated.private_lease_witnesses !=
                *after_read_private_leases.witnesses ||
            *final_inventory_validated.worker_attempt_records !=
                *after_read_worker_attempts.witnesses) {
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
    const auto validated = validate_manifest_bound_inventory(state_->root_fd, state_->absolute_root,
                                                             *inventory.inventory, state_->manifest,
                                                             state_->creator_process_id);
    if (!validated) {
        return validated.diagnostic;
    }
    if (hooks.observe_reservation_witnesses != nullptr) {
        hooks.observe_reservation_witnesses(*validated.private_lease_witnesses, hooks.context);
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
        state_->root_fd, state_->absolute_root, *final_inventory.inventory, state_->manifest,
        state_->creator_process_id);
    if (!final_validated) {
        return final_validated.diagnostic;
    }
    if (*final_validated.base_lock_identities != *validated.base_lock_identities ||
        *final_validated.private_lease_witnesses != *validated.private_lease_witnesses ||
        *final_validated.worker_attempt_records != *validated.worker_attempt_records) {
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
        auto before = capture_manifest_bound_inventory_witness(
            state_->root_fd, state_->manifest, state_->absolute_root, state_->creator_process_id);
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
        if (expectation == AttemptBaseLockExpectation::absent) {
            for (const auto& lease : *before.private_lease_witnesses) {
                if (lease.worker_handoff.has_value() &&
                    lease.worker_handoff->handoff.chunk_id == chunk_id) {
                    return {
                        nullptr,
                        diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                   protocol_error()),
                    };
                }
                if (!lease.work_package_residue.has_value()) {
                    continue;
                }
                if (!lease.base_lock_leaf.ends_with(
                        DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX)) {
                    return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                                protocol_error())};
                }
                const auto relative_stem =
                    std::string_view(lease.base_lock_leaf)
                        .substr(0, lease.base_lock_leaf.size() -
                                       DISTRIBUTED_SIEVE_PRIVATE_LEASE_BASE_LOCK_SUFFIX.size());
                const auto residue_names =
                    manifest_attempt_names_from_relative_stem(state_->manifest, relative_stem);
                const auto residue_coordinate =
                    residue_names.has_value() ? parse_distributed_sieve_worker_attempt_leaf_v1(
                                                    residue_names->canonical_record_leaf)
                                              : std::nullopt;
                if (!residue_names.has_value() || !residue_coordinate.has_value() ||
                    residue_coordinate->pending) {
                    return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                                protocol_error())};
                }
                if (residue_coordinate->chunk_id == chunk_id) {
                    return {
                        nullptr,
                        diagnostic(DistributedSieveWaveStoreStatus::reconciliation_required,
                                   protocol_error()),
                    };
                }
            }
        }

        NamespaceInventory expected_successor_inventory = *before.inventory;
        std::vector<NativeIdentityV1> expected_successor_identities = *before.base_lock_identities;
        std::vector<PrivateLeaseReservationWitness> expected_successor_private_leases =
            *before.private_lease_witnesses;
        std::optional<NativeIdentityV1> expected_existing_identity;
        if (expectation == AttemptBaseLockExpectation::absent) {
            expected_successor_inventory.private_lease_base_lock_leaves.insert(
                expected_successor_inventory.private_lease_base_lock_leaves.begin() +
                    static_cast<std::ptrdiff_t>(target_index),
                target_leaf);
            expected_successor_identities.insert(expected_successor_identities.begin() +
                                                     static_cast<std::ptrdiff_t>(target_index),
                                                 NativeIdentityV1{});
            expected_successor_private_leases.insert(expected_successor_private_leases.begin() +
                                                         static_cast<std::ptrdiff_t>(target_index),
                                                     PrivateLeaseReservationWitness{
                                                         .base_lock_leaf = target_leaf,
                                                     });
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

        // A terminal worker handoff is published while the worker owns its
        // attempt BaseLock.  Creating attempt N must therefore join that same
        // serialization domain before its final inventory witness.  Always
        // acquire predecessor locks in increasing attempt order, and only with
        // nonblocking flock, so every creator follows the fixed order
        //
        //   same-State root claim -> attempts [0, N) -> attempt N.
        //
        // Holding the root claim while probing a busy predecessor cannot form
        // a wait cycle: the probe never waits, and a worker publishing a
        // handoff acquires neither the root claim nor another attempt lock.
        std::vector<std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt>> predecessor_base_locks;
        if (expectation == AttemptBaseLockExpectation::absent) {
            predecessor_base_locks.reserve(attempt_ordinal);
            for (std::uint32_t predecessor_ordinal = 0; predecessor_ordinal < attempt_ordinal;
                 ++predecessor_ordinal) {
                const auto predecessor_names = distributed_sieve_worker_attempt_names_v1(
                    matched_chunk->relative_artifact_stem, chunk_id, predecessor_ordinal);
                if (!predecessor_names.has_value()) {
                    return {
                        nullptr,
                        diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                   protocol_error()),
                    };
                }
                const auto predecessor_position =
                    std::lower_bound(before.inventory->private_lease_base_lock_leaves.begin(),
                                     before.inventory->private_lease_base_lock_leaves.end(),
                                     predecessor_names->base_lock_leaf);
                if (predecessor_position ==
                        before.inventory->private_lease_base_lock_leaves.end() ||
                    *predecessor_position != predecessor_names->base_lock_leaf) {
                    continue;
                }
                const std::size_t predecessor_index = static_cast<std::size_t>(
                    std::distance(before.inventory->private_lease_base_lock_leaves.begin(),
                                  predecessor_position));
                DistributedSieveWaveStoreDiagnostic predecessor_outcome;
                auto predecessor = DistributedSievePrivateLeaseBaseLockAt::open_existing_locked(
                    state_->root_fd, predecessor_names->base_lock_leaf,
                    before.base_lock_identities->at(predecessor_index), state_->creator_process_id,
                    {}, predecessor_outcome);
                if (predecessor == nullptr ||
                    predecessor_outcome.status != DistributedSieveWaveStoreStatus::ready) {
                    if (const auto authority = claim.revalidate_authority();
                        authority.status != DistributedSieveWaveStoreStatus::ready) {
                        return {nullptr, authority};
                    }
                    if (predecessor_outcome.status == DistributedSieveWaveStoreStatus::ready) {
                        predecessor_outcome =
                            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                       std::make_error_code(std::errc::io_error));
                    }
                    return {nullptr, std::move(predecessor_outcome)};
                }
                predecessor_base_locks.emplace_back(std::move(predecessor));
            }
        }

        auto immediately_before = capture_manifest_bound_inventory_witness(
            state_->root_fd, state_->manifest, state_->absolute_root, state_->creator_process_id);
        if (!immediately_before) {
            if (const auto authority = claim.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return {nullptr, authority};
            }
            return {nullptr, std::move(immediately_before.diagnostic)};
        }
        if (*immediately_before.inventory != *before.inventory ||
            *immediately_before.base_lock_identities != *before.base_lock_identities ||
            *immediately_before.private_lease_witnesses != *before.private_lease_witnesses) {
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
        for (const auto& predecessor : predecessor_base_locks) {
            if (const auto predecessor_validated = predecessor->revalidate();
                predecessor_validated.status != DistributedSieveWaveStoreStatus::ready) {
                if (const auto authority = claim.revalidate_authority();
                    authority.status != DistributedSieveWaveStoreStatus::ready) {
                    return {nullptr, authority};
                }
                return {nullptr, predecessor_validated};
            }
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
            for (const auto& predecessor : predecessor_base_locks) {
                if (const auto predecessor_validated = predecessor->revalidate();
                    predecessor_validated.status != DistributedSieveWaveStoreStatus::ready) {
                    if (const auto authority = claim.revalidate_authority();
                        authority.status != DistributedSieveWaveStoreStatus::ready) {
                        return authority;
                    }
                    return predecessor_validated;
                }
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
                                                                  state_->absolute_root,
                                                                  state_->creator_process_id);
            if (!first) {
                if (const auto bindings = revalidate_higher_priority_bindings();
                    bindings.status != DistributedSieveWaveStoreStatus::ready) {
                    return bindings;
                }
                return std::move(first.diagnostic);
            }
            if (*first.inventory != expected_successor_inventory ||
                *first.base_lock_identities != expected_successor_identities ||
                *first.private_lease_witnesses != expected_successor_private_leases) {
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
                state_->root_fd, state_->manifest, state_->absolute_root,
                state_->creator_process_id);
            if (!confirmed) {
                if (const auto bindings = revalidate_higher_priority_bindings();
                    bindings.status != DistributedSieveWaveStoreStatus::ready) {
                    return bindings;
                }
                return std::move(confirmed.diagnostic);
            }
            if (*confirmed.inventory != *first.inventory ||
                *confirmed.base_lock_identities != *first.base_lock_identities ||
                *confirmed.private_lease_witnesses != *first.private_lease_witnesses) {
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
        claim.expected_private_lease_reservation_witnesses_.emplace(
            std::move(expected_successor_private_leases));
        claim.expected_worker_attempt_record_witnesses_.emplace(
            std::move(expected_successor_inventory.worker_attempt_records));
        claim.base_lock_acquisition_ =
            expectation == AttemptBaseLockExpectation::absent
                ? DistributedSievePrivateLeaseRootClaim::BaseLockAcquisition::CreatedNew
                : DistributedSievePrivateLeaseRootClaim::BaseLockAcquisition::OpenedExisting;
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

DistributedSievePrivateLeaseReservationReceipt::DistributedSievePrivateLeaseReservationReceipt(
    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state,
    DistributedSieveWorkerAttemptNamesV1 worker_attempt_names, NativeIdentityV1 base_lock_identity,
    DistributedSievePrivateLeaseReservationInventoryWitness final_witness,
    std::uint64_t creator_process_id) noexcept
    : wave_store_state_(std::move(wave_store_state)),
      worker_attempt_names_(std::move(worker_attempt_names)),
      base_lock_identity_(base_lock_identity), final_witness_(std::move(final_witness)),
      creator_process_id_(creator_process_id) {}

bool DistributedSievePrivateLeaseReservationReceipt::owned_by_current_process() const noexcept {
    return wave_store_state_ != nullptr && creator_process_id_ != 0 &&
           process_matches(creator_process_id_);
}

DistributedSieveWaveStoreDiagnostic DistributedSievePrivateLeaseReservationReceipt::revalidate(
    DistributedSievePrivateLeaseReservationReceiptTestHooks hooks) const noexcept {
    if (!owned_by_current_process()) {
        return process_mismatch();
    }
    if (final_witness_.base_lock_leaf != worker_attempt_names_.base_lock_leaf ||
        final_witness_.boundary !=
            DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable ||
        final_witness_.lease_id == std::array<std::uint64_t, 2>{} ||
        !final_witness_.reserved_marker_identity.has_value() ||
        !final_witness_.directory_identity.has_value() ||
        !final_witness_.owner_marker_identity.has_value() ||
        !final_witness_.owned_marker_identity.has_value() ||
        final_witness_.work_package_residue.has_value() || nil_identity(base_lock_identity_)) {
        return diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                          invalid_argument_error());
    }

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
#else
    DistributedSieveWaveStore view(wave_store_state_);
    const auto reject_lower_priority = [&](DistributedSieveWaveStoreDiagnostic lower) noexcept {
        if (const auto authority = view.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        return lower;
    };
    const auto target_matches = [&](const ManifestBoundInventoryWitnessResult& observed) noexcept {
        if (!observed ||
            observed.inventory->private_lease_base_lock_leaves.size() !=
                observed.base_lock_identities->size() ||
            observed.inventory->private_lease_base_lock_leaves.size() !=
                observed.private_lease_witnesses->size()) {
            return false;
        }
        const auto position =
            std::lower_bound(observed.inventory->private_lease_base_lock_leaves.begin(),
                             observed.inventory->private_lease_base_lock_leaves.end(),
                             final_witness_.base_lock_leaf);
        if (position == observed.inventory->private_lease_base_lock_leaves.end() ||
            *position != final_witness_.base_lock_leaf) {
            return false;
        }
        const auto index = static_cast<std::size_t>(
            std::distance(observed.inventory->private_lease_base_lock_leaves.begin(), position));
        return observed.base_lock_identities->at(index) == base_lock_identity_ &&
               observed.private_lease_witnesses->at(index) == final_witness_;
    };

    if (const auto authority = view.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    auto first = capture_manifest_bound_inventory_witness(
        wave_store_state_->root_fd, wave_store_state_->manifest, wave_store_state_->absolute_root,
        creator_process_id_);
    if (!first) {
        if (const auto authority = view.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        return std::move(first.diagnostic);
    }
    if (!target_matches(first)) {
        return reject_lower_priority(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (const auto authority = view.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    if (!process_matches(creator_process_id_)) {
        return process_mismatch();
    }
    if (hooks.after_first_target_validation != nullptr) {
        hooks.after_first_target_validation(hooks.context);
    }
    if (!process_matches(creator_process_id_)) {
        return process_mismatch();
    }
    if (const auto authority = view.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    auto confirmed = capture_manifest_bound_inventory_witness(
        wave_store_state_->root_fd, wave_store_state_->manifest, wave_store_state_->absolute_root,
        creator_process_id_);
    if (!confirmed) {
        if (const auto authority = view.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return authority;
        }
        return std::move(confirmed.diagnostic);
    }
    if (!target_matches(confirmed)) {
        return reject_lower_priority(
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
    }
    if (const auto authority = view.revalidate_authority();
        authority.status != DistributedSieveWaveStoreStatus::ready) {
        return authority;
    }
    return owned_by_current_process() ? DistributedSieveWaveStoreDiagnostic{} : process_mismatch();
#endif
}

std::string_view
DistributedSievePrivateLeaseReservationReceipt::relative_lease_stem() const noexcept {
    return worker_attempt_names_.relative_lease_stem;
}

const std::array<std::uint64_t, 2>&
DistributedSievePrivateLeaseReservationReceipt::lease_id() const noexcept {
    return final_witness_.lease_id;
}

const NativeIdentityV1&
DistributedSievePrivateLeaseReservationReceipt::directory_identity() const noexcept {
    return *final_witness_.directory_identity;
}

const NativeIdentityV1&
DistributedSievePrivateLeaseReservationReceipt::owner_marker_identity() const noexcept {
    return *final_witness_.owner_marker_identity;
}

const NativeIdentityV1&
DistributedSievePrivateLeaseReservationReceipt::owned_marker_identity() const noexcept {
    return *final_witness_.owned_marker_identity;
}

DistributedSieveWorkerAttemptStartReceipt::DistributedSieveWorkerAttemptStartReceipt(
    std::shared_ptr<const DistributedSieveWaveStore::State> wave_store_state,
    DistributedSieveWorkerAttemptNamesV1 worker_attempt_names, AttemptStartedV1 record,
    durable_record::RecordSnapshot canonical_snapshot,
    DistributedSievePrivateLeaseReservationInventoryWitness final_witness,
    std::unique_ptr<DistributedSievePrivateLeaseBaseLockAt> base_lock_at,
    std::uint64_t creator_process_id) noexcept
    : wave_store_state_(std::move(wave_store_state)),
      worker_attempt_names_(std::move(worker_attempt_names)), record_(std::move(record)),
      canonical_snapshot_(canonical_snapshot), final_witness_(std::move(final_witness)),
      base_lock_at_(std::move(base_lock_at)), creator_process_id_(creator_process_id) {}

bool DistributedSieveWorkerAttemptStartReceipt::owned_by_current_process() const noexcept {
    return wave_store_state_ != nullptr && creator_process_id_ != 0 &&
           process_matches(creator_process_id_) && base_lock_at_ != nullptr &&
           base_lock_at_->owned_by_current_process();
}

namespace {

enum class WorkerAttemptRecordPrefixShape : std::uint8_t {
    absent,
    pending_only,
    canonical_only,
    identical_dual,
};

[[nodiscard]] bool
worker_attempt_start_observations_equal(const ManifestBoundInventoryWitnessResult& left,
                                        const ManifestBoundInventoryWitnessResult& right) noexcept {
    return left && right && *left.inventory == *right.inventory &&
           *left.base_lock_identities == *right.base_lock_identities &&
           *left.private_lease_witnesses == *right.private_lease_witnesses;
}

[[nodiscard]] std::optional<ManifestBoundInventoryWitnessResult>
worker_attempt_recordless_projection(const ManifestBoundInventoryWitnessResult& observed,
                                     const DistributedSieveWorkerAttemptNamesV1& names,
                                     std::uint32_t chunk_id, std::uint32_t attempt_ordinal) {
    if (!observed) {
        return std::nullopt;
    }
    ManifestBoundInventoryWitnessResult projection = observed;
    auto& leaves = projection.inventory->worker_attempt_record_leaves;
    const auto erase_leaf = [&](const std::string& leaf) {
        const auto position = std::lower_bound(leaves.begin(), leaves.end(), leaf);
        if (position != leaves.end() && *position == leaf) {
            leaves.erase(position);
        }
    };
    erase_leaf(names.canonical_record_leaf);
    erase_leaf(names.pending_record_leaf);

    auto& records = projection.inventory->worker_attempt_records;
    const auto record = std::lower_bound(
        records.begin(), records.end(), std::pair{chunk_id, attempt_ordinal},
        [](const DistributedSieveWorkerAttemptRecordInventoryWitness& candidate,
           const std::pair<std::uint32_t, std::uint32_t>& coordinate) {
            return std::pair{candidate.chunk_id, candidate.attempt_ordinal} < coordinate;
        });
    if (record == records.end() || record->chunk_id != chunk_id ||
        record->attempt_ordinal != attempt_ordinal) {
        return std::nullopt;
    }
    records.erase(record);
    return projection;
}

[[nodiscard]] std::optional<WorkerAttemptRecordPrefixShape>
worker_attempt_record_prefix_shape(const ManifestBoundInventoryWitnessResult& predecessor,
                                   const ManifestBoundInventoryWitnessResult& observed,
                                   const DistributedSieveWorkerAttemptNamesV1& names,
                                   std::uint32_t chunk_id, std::uint32_t attempt_ordinal,
                                   std::span<const std::byte> expected_bytes) noexcept {
    if (!predecessor || !observed ||
        *observed.base_lock_identities != *predecessor.base_lock_identities ||
        *observed.private_lease_witnesses != *predecessor.private_lease_witnesses ||
        observed.inventory->lock != predecessor.inventory->lock ||
        observed.inventory->manifest != predecessor.inventory->manifest ||
        observed.inventory->pending != predecessor.inventory->pending ||
        observed.inventory->private_lease_base_lock_leaves !=
            predecessor.inventory->private_lease_base_lock_leaves ||
        observed.inventory->private_lease_protocol_leaves !=
            predecessor.inventory->private_lease_protocol_leaves) {
        return std::nullopt;
    }

    bool canonical_leaf_present = false;
    bool pending_leaf_present = false;
    std::size_t predecessor_leaf_index = 0;
    for (const auto& leaf : observed.inventory->worker_attempt_record_leaves) {
        if (leaf == names.canonical_record_leaf) {
            if (canonical_leaf_present) {
                return std::nullopt;
            }
            canonical_leaf_present = true;
            continue;
        }
        if (leaf == names.pending_record_leaf) {
            if (pending_leaf_present) {
                return std::nullopt;
            }
            pending_leaf_present = true;
            continue;
        }
        if (predecessor_leaf_index >= predecessor.inventory->worker_attempt_record_leaves.size() ||
            leaf != predecessor.inventory->worker_attempt_record_leaves[predecessor_leaf_index]) {
            return std::nullopt;
        }
        ++predecessor_leaf_index;
    }
    if (predecessor_leaf_index != predecessor.inventory->worker_attempt_record_leaves.size()) {
        return std::nullopt;
    }

    const DistributedSieveWorkerAttemptRecordInventoryWitness* target = nullptr;
    std::size_t predecessor_record_index = 0;
    for (const auto& witness : observed.inventory->worker_attempt_records) {
        if (witness.chunk_id == chunk_id && witness.attempt_ordinal == attempt_ordinal) {
            if (target != nullptr) {
                return std::nullopt;
            }
            target = &witness;
            continue;
        }
        if (predecessor_record_index >= predecessor.inventory->worker_attempt_records.size() ||
            !(witness == predecessor.inventory->worker_attempt_records[predecessor_record_index])) {
            return std::nullopt;
        }
        ++predecessor_record_index;
    }
    if (predecessor_record_index != predecessor.inventory->worker_attempt_records.size()) {
        return std::nullopt;
    }

    if (target == nullptr) {
        if (canonical_leaf_present || pending_leaf_present) {
            return std::nullopt;
        }
        return WorkerAttemptRecordPrefixShape::absent;
    }
    if (!std::equal(target->bytes.begin(), target->bytes.end(), expected_bytes.begin(),
                    expected_bytes.end()) ||
        target->bytes.size() != expected_bytes.size() ||
        target->canonical_snapshot.has_value() != canonical_leaf_present ||
        target->pending_snapshot.has_value() != pending_leaf_present) {
        return std::nullopt;
    }
    if (canonical_leaf_present && pending_leaf_present) {
        return WorkerAttemptRecordPrefixShape::identical_dual;
    }
    if (canonical_leaf_present) {
        return WorkerAttemptRecordPrefixShape::canonical_only;
    }
    if (pending_leaf_present) {
        return WorkerAttemptRecordPrefixShape::pending_only;
    }
    return std::nullopt;
}

[[nodiscard]] const DistributedSieveWorkerAttemptRecordInventoryWitness*
find_worker_attempt_record_witness(const ManifestBoundInventoryWitnessResult& observed,
                                   std::uint32_t chunk_id, std::uint32_t attempt_ordinal) noexcept {
    if (!observed) {
        return nullptr;
    }
    const auto position = std::lower_bound(
        observed.inventory->worker_attempt_records.begin(),
        observed.inventory->worker_attempt_records.end(), std::pair{chunk_id, attempt_ordinal},
        [](const DistributedSieveWorkerAttemptRecordInventoryWitness& candidate,
           const std::pair<std::uint32_t, std::uint32_t>& coordinate) {
            return std::pair{candidate.chunk_id, candidate.attempt_ordinal} < coordinate;
        });
    if (position == observed.inventory->worker_attempt_records.end() ||
        position->chunk_id != chunk_id || position->attempt_ordinal != attempt_ordinal) {
        return nullptr;
    }
    return &*position;
}

[[nodiscard]] DistributedSieveWaveStoreDiagnostic
worker_attempt_protocol_conflict(DistributedSieveProtocolStatus status) noexcept {
    auto outcome =
        diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error());
    outcome.protocol_status = status;
    return outcome;
}

} // namespace

DistributedSieveWaveStoreDiagnostic DistributedSieveWorkerAttemptStartReceipt::revalidate(
    DistributedSieveWorkerAttemptStartReceiptTestHooks hooks) const noexcept {
    if (!owned_by_current_process()) {
        return process_mismatch();
    }
    const auto parsed =
        parse_distributed_sieve_worker_attempt_leaf_v1(worker_attempt_names_.canonical_record_leaf);
    if (!parsed.has_value() || parsed->pending ||
        final_witness_.base_lock_leaf != worker_attempt_names_.base_lock_leaf ||
        final_witness_.boundary !=
            DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable ||
        final_witness_.lease_id == std::array<std::uint64_t, 2>{} ||
        !final_witness_.reserved_marker_identity.has_value() ||
        !final_witness_.directory_identity.has_value() ||
        !final_witness_.owner_marker_identity.has_value() ||
        !final_witness_.owned_marker_identity.has_value() ||
        final_witness_.work_package_residue.has_value() || record_.chunk_id != parsed->chunk_id ||
        record_.attempt_ordinal != parsed->attempt_ordinal ||
        record_.manifest_digest != wave_store_state_->manifest.self_digest ||
        record_.lease.lease_id.limbs != final_witness_.lease_id ||
        record_.lease.owner_marker != *final_witness_.owner_marker_identity ||
        record_.lease.directory != *final_witness_.directory_identity ||
        record_.lease.relative_stem != worker_attempt_names_.relative_lease_stem ||
        base_lock_at_->identity() == NativeIdentityV1{}) {
        return diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                          invalid_argument_error());
    }

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    (void)hooks;
    return diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
#else
    try {
        DistributedSieveProtocolRecordV1 sealed(record_);
        const auto encoded = encode_distributed_sieve_record(sealed);
        if (!encoded) {
            auto outcome =
                diagnostic(DistributedSieveWaveStoreStatus::invalid_request, protocol_error());
            outcome.protocol_status = encoded.status;
            return outcome;
        }
        const auto regenerated = [&]() -> std::optional<DistributedSieveWorkerAttemptNamesV1> {
            for (const auto& chunk : wave_store_state_->manifest.chunks) {
                if (chunk.chunk_id == parsed->chunk_id) {
                    return distributed_sieve_worker_attempt_names_v1(
                        chunk.relative_artifact_stem, parsed->chunk_id, parsed->attempt_ordinal);
                }
            }
            return std::nullopt;
        }();
        if (!regenerated.has_value() || *regenerated != worker_attempt_names_) {
            return diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                              invalid_argument_error());
        }

        DistributedSieveWaveStore view(wave_store_state_);
        const auto revalidate_bindings = [&]() noexcept {
            if (const auto authority = view.revalidate_authority();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return authority;
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
        const auto reject_lower_priority = [&](DistributedSieveWaveStoreDiagnostic lower) noexcept {
            if (const auto bindings = revalidate_bindings();
                bindings.status != DistributedSieveWaveStoreStatus::ready) {
                return bindings;
            }
            return lower;
        };
        const auto target_matches =
            [&](const ManifestBoundInventoryWitnessResult& observed) noexcept {
                if (!observed ||
                    observed.inventory->private_lease_base_lock_leaves.size() !=
                        observed.base_lock_identities->size() ||
                    observed.inventory->private_lease_base_lock_leaves.size() !=
                        observed.private_lease_witnesses->size()) {
                    return false;
                }
                const auto lock_position =
                    std::lower_bound(observed.inventory->private_lease_base_lock_leaves.begin(),
                                     observed.inventory->private_lease_base_lock_leaves.end(),
                                     worker_attempt_names_.base_lock_leaf);
                if (lock_position == observed.inventory->private_lease_base_lock_leaves.end() ||
                    *lock_position != worker_attempt_names_.base_lock_leaf) {
                    return false;
                }
                const auto lock_index = static_cast<std::size_t>(std::distance(
                    observed.inventory->private_lease_base_lock_leaves.begin(), lock_position));
                const auto* witness = find_worker_attempt_record_witness(observed, parsed->chunk_id,
                                                                         parsed->attempt_ordinal);
                return observed.base_lock_identities->at(lock_index) == base_lock_at_->identity() &&
                       observed.private_lease_witnesses->at(lock_index) == final_witness_ &&
                       witness != nullptr && witness->bytes == *encoded.bytes &&
                       witness->canonical_snapshot == canonical_snapshot_ &&
                       !witness->pending_snapshot.has_value();
            };

        if (const auto bindings = revalidate_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return bindings;
        }
        auto first = capture_manifest_bound_inventory_witness(
            wave_store_state_->root_fd, wave_store_state_->manifest,
            wave_store_state_->absolute_root, creator_process_id_);
        if (!first) {
            return reject_lower_priority(std::move(first.diagnostic));
        }
        if (!target_matches(first)) {
            return reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (const auto bindings = revalidate_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return bindings;
        }
        if (const auto hooked = invoke_private_lease_base_lock_hook(
                hooks.after_first_validation, hooks.context, creator_process_id_);
            hooked.status != DistributedSieveWaveStoreStatus::ready) {
            return hooked;
        }
        if (const auto bindings = revalidate_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return bindings;
        }
        auto confirmed = capture_manifest_bound_inventory_witness(
            wave_store_state_->root_fd, wave_store_state_->manifest,
            wave_store_state_->absolute_root, creator_process_id_);
        if (!confirmed) {
            return reject_lower_priority(std::move(confirmed.diagnostic));
        }
        if (!target_matches(confirmed)) {
            return reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }
        if (const auto bindings = revalidate_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return bindings;
        }
        return owned_by_current_process() ? DistributedSieveWaveStoreDiagnostic{}
                                          : process_mismatch();
    } catch (const std::bad_alloc&) {
        return diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                          std::make_error_code(std::errc::not_enough_memory));
    } catch (...) {
        return diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                          std::make_error_code(std::errc::io_error));
    }
#endif
}

const AttemptStartedV1& DistributedSieveWorkerAttemptStartReceipt::record() const noexcept {
    return record_;
}

const durable_record::RecordSnapshot&
DistributedSieveWorkerAttemptStartReceipt::canonical_snapshot() const noexcept {
    return canonical_snapshot_;
}

DistributedSievePrivateLeaseRootClaimResult
recover_worker_attempt_private_lease(DistributedSievePrivateLeaseRootClaimResult&& claimed,
                                     DistributedSievePrivateLeaseRecoveryTestHooks hooks) noexcept {
    auto owned_claim = std::move(claimed);
    if (owned_claim.claim == nullptr ||
        owned_claim.diagnostic.status != DistributedSieveWaveStoreStatus::ready ||
        !owned_claim.claim->owned_by_current_process() ||
        owned_claim.claim->base_lock_acquisition_ !=
            DistributedSievePrivateLeaseRootClaim::BaseLockAcquisition::OpenedExisting) {
        auto outcome = std::move(owned_claim.diagnostic);
        if (outcome.status == DistributedSieveWaveStoreStatus::ready) {
            outcome = diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                 invalid_argument_error());
        }
        owned_claim.claim.reset();
        return {nullptr, std::move(outcome)};
    }

    if (const auto validated = owned_claim.claim->revalidate();
        validated.status != DistributedSieveWaveStoreStatus::ready) {
        owned_claim.claim.reset();
        return {nullptr, validated};
    }
    if (!owned_claim.claim->worker_attempt_names_.has_value() ||
        !owned_claim.claim->expected_private_lease_base_lock_leaves_.has_value() ||
        !owned_claim.claim->expected_private_lease_reservation_witnesses_.has_value()) {
        owned_claim.claim.reset();
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                    invalid_argument_error())};
    }
    const auto recovery_target =
        std::lower_bound(owned_claim.claim->expected_private_lease_base_lock_leaves_->begin(),
                         owned_claim.claim->expected_private_lease_base_lock_leaves_->end(),
                         owned_claim.claim->worker_attempt_names_->base_lock_leaf);
    if (recovery_target == owned_claim.claim->expected_private_lease_base_lock_leaves_->end() ||
        *recovery_target != owned_claim.claim->worker_attempt_names_->base_lock_leaf) {
        owned_claim.claim.reset();
        return {nullptr,
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())};
    }
    const auto recovery_target_index = static_cast<std::size_t>(std::distance(
        owned_claim.claim->expected_private_lease_base_lock_leaves_->begin(), recovery_target));
    if (recovery_target_index >=
        owned_claim.claim->expected_private_lease_reservation_witnesses_->size()) {
        owned_claim.claim.reset();
        return {nullptr,
                diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error())};
    }
    if (owned_claim.claim->expected_private_lease_reservation_witnesses_->at(recovery_target_index)
            .work_package_residue.has_value()) {
        owned_claim.claim.reset();
        return {nullptr, diagnostic(DistributedSieveWaveStoreStatus::reconciliation_required,
                                    protocol_error())};
    }

    bool completed = false;
    DistributedSieveWaveStoreDiagnostic outcome;
    try {
        {
            DistributedSieveFdPrivateLeaseRecoveryTarget target(*owned_claim.claim, hooks);
            const auto run = private_lease::run_private_lease_recovery_protocol(target);
            if (run == private_lease::PrivateLeaseRecoveryRunResult::Interrupted) {
                outcome = target.interruption_diagnostic();
            } else if (run == private_lease::PrivateLeaseRecoveryRunResult::Rejected) {
                outcome = diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                     invalid_argument_error());
            } else if (!target.completed()) {
                outcome = diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                     protocol_error());
            } else {
                completed = true;
            }
        }
    } catch (const DistributedSieveFdPrivateLeaseRecoveryTarget::Failure& failure) {
        outcome = failure.diagnostic;
    } catch (const std::bad_alloc&) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                             std::make_error_code(std::errc::not_enough_memory));
    } catch (...) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                             std::make_error_code(std::errc::io_error));
    }

    if (completed && outcome.status == DistributedSieveWaveStoreStatus::ready &&
        owned_claim.claim != nullptr && owned_claim.claim->owned_by_current_process()) {
        owned_claim.diagnostic = {};
        return owned_claim;
    }
    owned_claim.claim.reset();
    if (outcome.status == DistributedSieveWaveStoreStatus::ready) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
    }
    return {nullptr, std::move(outcome)};
}

DistributedSieveWorkerAttemptReconcileResult
reconcile_worker_attempt_started(DistributedSievePrivateLeaseRootClaimResult&& claimed,
                                 DistributedSieveWorkerAttemptReconcileTestHooks hooks) noexcept {
    auto owned_claim = std::move(claimed);
    const auto fail_with = [](DistributedSieveWaveStoreDiagnostic outcome) {
        if (outcome.status == DistributedSieveWaveStoreStatus::ready) {
            outcome =
                diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
        }
        return DistributedSieveWorkerAttemptReconcileResult{std::nullopt, std::move(outcome)};
    };

    if (owned_claim.claim == nullptr ||
        owned_claim.diagnostic.status != DistributedSieveWaveStoreStatus::ready ||
        !owned_claim.claim->owned_by_current_process() ||
        owned_claim.claim->wave_store_state_ == nullptr ||
        owned_claim.claim->base_lock_at_ == nullptr ||
        !owned_claim.claim->worker_attempt_names_.has_value() ||
        !owned_claim.claim->expected_private_lease_base_lock_leaves_.has_value() ||
        !owned_claim.claim->expected_private_lease_base_lock_identities_.has_value() ||
        !owned_claim.claim->expected_private_lease_reservation_witnesses_.has_value() ||
        !owned_claim.claim->expected_worker_attempt_record_witnesses_.has_value() ||
        owned_claim.claim->base_lock_acquisition_ !=
            DistributedSievePrivateLeaseRootClaim::BaseLockAcquisition::OpenedExisting) {
        auto outcome = std::move(owned_claim.diagnostic);
        if (outcome.status == DistributedSieveWaveStoreStatus::ready) {
            outcome = diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                 invalid_argument_error());
        }
        return fail_with(std::move(outcome));
    }

#if defined(_WIN32)
    (void)hooks;
    return fail_with(
        diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
    try {
        auto& claim = *owned_claim.claim;
        const auto& state = *claim.wave_store_state_;
        const auto revalidate_bindings = [&]() noexcept -> DistributedSieveWaveStoreDiagnostic {
            return claim.revalidate_authority();
        };
        const auto reject_lower_priority = [&](DistributedSieveWaveStoreDiagnostic lower) noexcept {
            if (const auto bindings = revalidate_bindings();
                bindings.status != DistributedSieveWaveStoreStatus::ready) {
                return bindings;
            }
            return lower;
        };
        const auto parsed = parse_distributed_sieve_worker_attempt_leaf_v1(
            claim.worker_attempt_names_->canonical_record_leaf);
        if (!parsed.has_value() || parsed->pending ||
            parsed->attempt_ordinal >= state.manifest.max_worker_attempts) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                        invalid_argument_error()));
        }

        const ChunkPlanV1* chunk = nullptr;
        for (const auto& candidate : state.manifest.chunks) {
            if (candidate.chunk_id == parsed->chunk_id) {
                chunk = &candidate;
                break;
            }
        }
        if (chunk == nullptr || chunk->sq_begin >= chunk->sq_end) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                        invalid_argument_error()));
        }
        const auto names = distributed_sieve_worker_attempt_names_v1(
            chunk->relative_artifact_stem, parsed->chunk_id, parsed->attempt_ordinal);
        if (!names.has_value() || *names != *claim.worker_attempt_names_) {
            return fail_with(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()));
        }

        if (const auto validated = claim.revalidate();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(validated);
        }
        auto initial = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!initial) {
            return fail_with(reject_lower_priority(std::move(initial.diagnostic)));
        }
        if (initial.inventory->private_lease_base_lock_leaves !=
                *claim.expected_private_lease_base_lock_leaves_ ||
            *initial.base_lock_identities != *claim.expected_private_lease_base_lock_identities_ ||
            *initial.private_lease_witnesses !=
                *claim.expected_private_lease_reservation_witnesses_ ||
            initial.inventory->worker_attempt_records !=
                *claim.expected_worker_attempt_record_witnesses_) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }

        const auto* initial_record =
            find_worker_attempt_record_witness(initial, parsed->chunk_id, parsed->attempt_ordinal);
        if (initial_record == nullptr) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        for (const auto& record : initial.inventory->worker_attempt_records) {
            if (record.chunk_id == parsed->chunk_id &&
                record.attempt_ordinal > parsed->attempt_ordinal) {
                return fail_with(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
        }
        for (std::uint32_t ordinal = parsed->attempt_ordinal + 1U;
             ordinal < state.manifest.max_worker_attempts; ++ordinal) {
            const auto later_names = distributed_sieve_worker_attempt_names_v1(
                chunk->relative_artifact_stem, parsed->chunk_id, ordinal);
            if (!later_names.has_value()) {
                return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                            protocol_error()));
            }
            if (std::binary_search(initial.inventory->private_lease_base_lock_leaves.begin(),
                                   initial.inventory->private_lease_base_lock_leaves.end(),
                                   later_names->base_lock_leaf)) {
                return fail_with(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
        }

        const auto target_lock = std::lower_bound(
            initial.inventory->private_lease_base_lock_leaves.begin(),
            initial.inventory->private_lease_base_lock_leaves.end(), names->base_lock_leaf);
        if (target_lock == initial.inventory->private_lease_base_lock_leaves.end() ||
            *target_lock != names->base_lock_leaf) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        const auto target_index = static_cast<std::size_t>(
            std::distance(initial.inventory->private_lease_base_lock_leaves.begin(), target_lock));
        if (target_index >= initial.private_lease_witnesses->size()) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error())));
        }
        auto recordless = worker_attempt_recordless_projection(initial, *names, parsed->chunk_id,
                                                               parsed->attempt_ordinal);
        if (!recordless.has_value()) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        const auto initial_shape =
            worker_attempt_record_prefix_shape(*recordless, initial, *names, parsed->chunk_id,
                                               parsed->attempt_ordinal, initial_record->bytes);
        if (!initial_shape.has_value() ||
            *initial_shape == WorkerAttemptRecordPrefixShape::absent) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }

        const auto initial_boundary = initial.private_lease_witnesses->at(target_index).boundary;
        if (*initial_shape == WorkerAttemptRecordPrefixShape::pending_only &&
            initial_boundary !=
                DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }

        const auto initial_record_bytes = initial_record->bytes;
        const auto initial_canonical_snapshot = initial_record->canonical_snapshot;
        const auto initial_pending_snapshot = initial_record->pending_snapshot;
        if ((*initial_shape == WorkerAttemptRecordPrefixShape::pending_only &&
             (!initial_pending_snapshot.has_value() || initial_canonical_snapshot.has_value())) ||
            ((*initial_shape == WorkerAttemptRecordPrefixShape::canonical_only ||
              *initial_shape == WorkerAttemptRecordPrefixShape::identical_dual) &&
             !initial_canonical_snapshot.has_value())) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }

        if (const auto authority = claim.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(authority);
        }
        auto confirmed_initial = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!confirmed_initial) {
            return fail_with(reject_lower_priority(std::move(confirmed_initial.diagnostic)));
        }
        const auto confirmed_initial_shape = worker_attempt_record_prefix_shape(
            *recordless, confirmed_initial, *names, parsed->chunk_id, parsed->attempt_ordinal,
            initial_record_bytes);
        if (!confirmed_initial_shape.has_value() || *confirmed_initial_shape != *initial_shape ||
            !worker_attempt_start_observations_equal(initial, confirmed_initial)) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        if (const auto authority = claim.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(authority);
        }

        if (initial_boundary ==
            DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable) {
            const auto& initial_lease = initial.private_lease_witnesses->at(target_index);
            if (!initial_lease.directory_identity.has_value() ||
                !initial_lease.reserved_marker_identity.has_value() ||
                !initial_lease.owner_marker_identity.has_value() ||
                !initial_lease.owned_marker_identity.has_value() ||
                initial_lease.lease_id == std::array<std::uint64_t, 2>{} ||
                initial.base_lock_identities->at(target_index) != claim.base_lock_at_->identity()) {
                return fail_with(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
            const NativeIdentityV1 cleanup_directory_identity = *initial_lease.directory_identity;
            const auto expected_cleanup_residue = initial_lease.work_package_residue;

            const bool pin_canonical = initial_canonical_snapshot.has_value();
            const auto& pinned_snapshot =
                pin_canonical ? *initial_canonical_snapshot : *initial_pending_snapshot;
            const std::string& pinned_leaf =
                pin_canonical ? names->canonical_record_leaf : names->pending_record_leaf;
            auto pinned_record = open_worker_attempt_canonical_record(
                state.root_fd, pinned_leaf, pinned_snapshot, initial_record_bytes,
                state.creator_process_id);
            if (!pinned_record) {
                return fail_with(reject_lower_priority(std::move(pinned_record.diagnostic)));
            }

            auto directory = inspect_private_lease_directory_at(
                state.root_fd, names->private_directory_leaf, state.creator_process_id);
            if (!directory || directory.identity != cleanup_directory_identity ||
                directory.inventory->work_package_residue != expected_cleanup_residue.has_value()) {
                return fail_with(reject_lower_priority(
                    directory ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                           protocol_error())
                              : std::move(directory.diagnostic)));
            }

            const auto validate_cleanup_bindings =
                [&]() noexcept -> DistributedSieveWaveStoreDiagnostic {
                if (const auto validated = claim.revalidate();
                    validated.status != DistributedSieveWaveStoreStatus::ready) {
                    return validated;
                }
                if (const auto pinned = validate_exact_worker_attempt_record_handle(
                        state.root_fd, pinned_record.canonical_record.get(), pinned_leaf,
                        pinned_snapshot, initial_record_bytes, state.creator_process_id);
                    pinned.status != DistributedSieveWaveStoreStatus::ready) {
                    return pinned;
                }
                if (const auto bound = validate_private_lease_directory_binding(
                        state.root_fd, directory.directory.get(), names->private_directory_leaf,
                        state.creator_process_id, cleanup_directory_identity);
                    bound.status != DistributedSieveWaveStoreStatus::ready) {
                    return bound;
                }
                return claim.revalidate_authority();
            };
            const auto validate_cleanup_successor_bindings =
                [&]() noexcept -> DistributedSieveWaveStoreDiagnostic {
                if (const auto authority = claim.revalidate_authority();
                    authority.status != DistributedSieveWaveStoreStatus::ready) {
                    return authority;
                }
                if (const auto pinned = validate_exact_worker_attempt_record_handle(
                        state.root_fd, pinned_record.canonical_record.get(), pinned_leaf,
                        pinned_snapshot, initial_record_bytes, state.creator_process_id);
                    pinned.status != DistributedSieveWaveStoreStatus::ready) {
                    return pinned;
                }
                if (const auto bound = validate_private_lease_directory_binding(
                        state.root_fd, directory.directory.get(), names->private_directory_leaf,
                        state.creator_process_id, cleanup_directory_identity);
                    bound.status != DistributedSieveWaveStoreStatus::ready) {
                    return bound;
                }
                return claim.revalidate_authority();
            };
            const auto reject_after_cleanup_mutation =
                [&](DistributedSieveWaveStoreDiagnostic lower) noexcept {
                    if (const auto bindings = validate_cleanup_successor_bindings();
                        bindings.status != DistributedSieveWaveStoreStatus::ready) {
                        return bindings;
                    }
                    return lower;
                };

            if (const auto hooked = invoke_private_lease_base_lock_hook(
                    hooks.work_package_residue.before_reconciliation,
                    hooks.work_package_residue.context, state.creator_process_id);
                hooked.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(hooked);
            }
            bool inject_directory_sync_failure = false;
            if (hooks.work_package_residue.fail_before_directory_sync != nullptr) {
                if (!process_matches(state.creator_process_id)) {
                    return fail_with(process_mismatch());
                }
                inject_directory_sync_failure =
                    hooks.work_package_residue.fail_before_directory_sync(
                        hooks.work_package_residue.context);
                if (!process_matches(state.creator_process_id)) {
                    return fail_with(process_mismatch());
                }
            }
            if (const auto validated = validate_cleanup_bindings();
                validated.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(validated);
            }

            std::optional<work_package_file::DistributedSieveWorkerWorkPackageResidueWitnessV1>
                full_residue;
            if (expected_cleanup_residue.has_value()) {
                auto inspected =
                    work_package_file::inspect_distributed_sieve_worker_work_package_residue_v1({
                        .borrowed_attempt_directory_handle = static_cast<
                            work_package_file::DistributedSieveWorkerWorkPackageNativeHandle>(
                            directory.directory.get()),
                        .expected_directory_identity = cleanup_directory_identity,
                        .observer_process_id = state.creator_process_id,
                    });
                if (!inspected) {
                    return fail_with(reject_lower_priority(
                        work_package_residue_reconciliation_inspection_failure(
                            inspected.diagnostic)));
                }
                const auto work_digest = distributed_sieve_work_digest(inspected.witness->identity);
                const auto manifest_status =
                    validate_manifest_work_identity(state.manifest, inspected.witness->identity);
                if (!work_digest || *work_digest.digest != state.manifest.work_sha256 ||
                    !manifest_status ||
                    inspected.witness->package.work_sha256 != state.manifest.work_sha256 ||
                    inspected.witness->package.total_bytes != inspected.witness->file_extent ||
                    compact_work_package_residue_witness(*inspected.witness) !=
                        *expected_cleanup_residue) {
                    const auto protocol_status =
                        !work_digest
                            ? work_digest.status
                            : (!manifest_status
                                   ? manifest_status
                                   : DistributedSieveProtocolStatus{
                                         .error = DistributedSieveProtocolError::digest_mismatch,
                                     });
                    const auto status =
                        protocol_status.error == DistributedSieveProtocolError::resource_exhausted
                            ? DistributedSieveWaveStoreStatus::resource_exhausted
                            : DistributedSieveWaveStoreStatus::namespace_conflict;
                    auto failure = diagnostic(status, protocol_error());
                    failure.protocol_status = protocol_status;
                    return fail_with(reject_lower_priority(std::move(failure)));
                }
                full_residue.emplace(std::move(*inspected.witness));
            }
            if (const auto validated = validate_cleanup_bindings();
                validated.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(validated);
            }

            const auto carrier_stop_after = carrier_work_package_residue_reconciliation_fault_point(
                hooks.work_package_residue.stop_after);
            if (hooks.work_package_residue.stop_after.has_value() &&
                (!carrier_stop_after.has_value() ||
                 (!full_residue.has_value() &&
                  hooks.work_package_residue.stop_after ==
                      DistributedSieveWorkerWorkPackageResidueReconciliationFaultPoint::
                          AfterNameUnlinked))) {
                return fail_with(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                            invalid_argument_error()));
            }
            const auto carrier_hooks = work_package_file::
                DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1{
                    .before_unlink = nullptr,
                    .fail_before_directory_sync =
                        inject_directory_sync_failure
                            ? preselected_work_package_directory_sync_failure
                            : nullptr,
                    .after_directory_durable = nullptr,
                    .stop_after = carrier_stop_after,
                    .context = &inject_directory_sync_failure,
                };
            const auto residue_reconciled =
                work_package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
                    {
                        .borrowed_attempt_directory_handle = static_cast<
                            work_package_file::DistributedSieveWorkerWorkPackageNativeHandle>(
                            directory.directory.get()),
                        .expected_directory_identity = cleanup_directory_identity,
                        .reconciler_process_id = state.creator_process_id,
                        .expected_residue =
                            full_residue.has_value() ? std::addressof(*full_residue) : nullptr,
                    },
                    carrier_hooks);
            if (!residue_reconciled) {
                return fail_with(reject_after_cleanup_mutation(
                    work_package_residue_reconciliation_failure(residue_reconciled)));
            }
            const auto expected_disposition =
                full_residue.has_value()
                    ? work_package_file::
                          DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1::
                              removed
                    : work_package_file::
                          DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1::
                              confirmed_absent;
            if (residue_reconciled.disposition != expected_disposition ||
                residue_reconciled.fault_point.has_value()) {
                return fail_with(reject_after_cleanup_mutation(diagnostic(
                    DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error())));
            }

            auto expected_cleanup_successor = std::move(confirmed_initial);
            expected_cleanup_successor.private_lease_witnesses->at(target_index)
                .work_package_residue.reset();
            const auto exact_cleanup_successor =
                [&](const ManifestBoundInventoryWitnessResult& observed) noexcept {
                    return worker_attempt_start_observations_equal(expected_cleanup_successor,
                                                                   observed);
                };

            if (const auto bindings = validate_cleanup_successor_bindings();
                bindings.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(bindings);
            }
            auto first_cleanup_successor = capture_manifest_bound_inventory_witness(
                state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
            if (!first_cleanup_successor || !exact_cleanup_successor(first_cleanup_successor)) {
                return fail_with(reject_after_cleanup_mutation(
                    first_cleanup_successor
                        ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                     protocol_error())
                        : std::move(first_cleanup_successor.diagnostic)));
            }
            if (const auto bindings = validate_cleanup_successor_bindings();
                bindings.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(bindings);
            }
            if (const auto hooked = invoke_private_lease_base_lock_hook(
                    hooks.work_package_residue.after_first_successor_validation,
                    hooks.work_package_residue.context, state.creator_process_id);
                hooked.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(hooked);
            }
            if (const auto bindings = validate_cleanup_successor_bindings();
                bindings.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(bindings);
            }
            auto confirmed_cleanup_successor = capture_manifest_bound_inventory_witness(
                state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
            if (!confirmed_cleanup_successor ||
                !exact_cleanup_successor(confirmed_cleanup_successor) ||
                !worker_attempt_start_observations_equal(first_cleanup_successor,
                                                         confirmed_cleanup_successor)) {
                return fail_with(reject_after_cleanup_mutation(
                    confirmed_cleanup_successor
                        ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                     protocol_error())
                        : std::move(confirmed_cleanup_successor.diagnostic)));
            }
            if (const auto bindings = validate_cleanup_successor_bindings();
                bindings.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(bindings);
            }

            *claim.expected_private_lease_base_lock_leaves_ =
                confirmed_cleanup_successor.inventory->private_lease_base_lock_leaves;
            *claim.expected_private_lease_base_lock_identities_ =
                *confirmed_cleanup_successor.base_lock_identities;
            *claim.expected_private_lease_reservation_witnesses_ =
                *confirmed_cleanup_successor.private_lease_witnesses;
            *claim.expected_worker_attempt_record_witnesses_ =
                confirmed_cleanup_successor.inventory->worker_attempt_records;
            initial = std::move(confirmed_cleanup_successor);
            recordless = worker_attempt_recordless_projection(initial, *names, parsed->chunk_id,
                                                              parsed->attempt_ordinal);
            if (!recordless.has_value()) {
                return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                            protocol_error()));
            }
            if (const auto validated = claim.revalidate();
                validated.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(validated);
            }
            if (const auto pinned = validate_exact_worker_attempt_record_handle(
                    state.root_fd, pinned_record.canonical_record.get(), pinned_leaf,
                    pinned_snapshot, initial_record_bytes, state.creator_process_id);
                pinned.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(pinned);
            }
            if (const auto bound = validate_private_lease_directory_binding(
                    state.root_fd, directory.directory.get(), names->private_directory_leaf,
                    state.creator_process_id, cleanup_directory_identity);
                bound.status != DistributedSieveWaveStoreStatus::ready) {
                return fail_with(bound);
            }
        }

        if (const auto hooked = invoke_private_lease_base_lock_hook(
                hooks.before_record_normalization, hooks.context, state.creator_process_id);
            hooked.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(hooked);
        }
        if (const auto validated = claim.revalidate();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(validated);
        }

        WorkerAttemptReconcileHookBridge bridge{
            .hooks = hooks,
            .creator_process_id = state.creator_process_id,
            .process_changed = false,
            .last_fault_point = std::nullopt,
        };
        const auto published = durable_record::publish_at(
            static_cast<durable_record::NativeHandle>(state.root_fd), names->pending_record_leaf,
            names->canonical_record_leaf, initial_record_bytes,
            durable_record::RecordTestHooks{
                .stop_after = bridge_worker_attempt_reconcile_hook,
                .context = &bridge,
            });
        auto publication_outcome =
            worker_attempt_reconcile_publication_diagnostic(published, bridge);
        const auto expected_disposition =
            *initial_shape == WorkerAttemptRecordPrefixShape::pending_only
                ? durable_record::RecordPublishDisposition::recovered_pending
                : durable_record::RecordPublishDisposition::confirmed_existing;
        const auto expected_source_snapshot =
            *initial_shape == WorkerAttemptRecordPrefixShape::pending_only
                ? initial_pending_snapshot
                : initial_canonical_snapshot;
        const auto attach_publication = [&](DistributedSieveWaveStoreDiagnostic higher) noexcept {
            higher.publication_status = publication_outcome.publication_status;
            higher.publication_disposition = publication_outcome.publication_disposition;
            higher.last_worker_attempt_reconcile_fault_point =
                publication_outcome.last_worker_attempt_reconcile_fault_point;
            return higher;
        };
        const auto exact_visible_prefix = [&](const ManifestBoundInventoryWitnessResult& observed,
                                              WorkerAttemptRecordPrefixShape shape) noexcept {
            const auto* witness = find_worker_attempt_record_witness(observed, parsed->chunk_id,
                                                                     parsed->attempt_ordinal);
            if (witness == nullptr) {
                return false;
            }
            const auto expected_canonical_snapshot = published.canonical_snapshot().has_value()
                                                         ? published.canonical_snapshot()
                                                         : expected_source_snapshot;
            if (shape == WorkerAttemptRecordPrefixShape::pending_only) {
                return !published.canonical_snapshot().has_value() &&
                       expected_source_snapshot.has_value() &&
                       witness->pending_snapshot == expected_source_snapshot;
            }
            if (shape != WorkerAttemptRecordPrefixShape::canonical_only &&
                shape != WorkerAttemptRecordPrefixShape::identical_dual) {
                return false;
            }
            if (!expected_canonical_snapshot.has_value() ||
                witness->canonical_snapshot != expected_canonical_snapshot) {
                return false;
            }
            return shape != WorkerAttemptRecordPrefixShape::identical_dual ||
                   (initial_pending_snapshot.has_value() &&
                    witness->pending_snapshot == initial_pending_snapshot);
        };
        const auto adjudicate_visible_prefix =
            [&](DistributedSieveWaveStoreDiagnostic lower,
                std::optional<WorkerAttemptRecordPrefixShape>& visible_shape) noexcept {
                if (const auto authority = claim.revalidate_authority();
                    authority.status != DistributedSieveWaveStoreStatus::ready) {
                    return attach_publication(authority);
                }
                auto first = capture_manifest_bound_inventory_witness(
                    state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
                if (!first) {
                    if (const auto authority = claim.revalidate_authority();
                        authority.status != DistributedSieveWaveStoreStatus::ready) {
                        return attach_publication(authority);
                    }
                    return attach_publication(std::move(first.diagnostic));
                }
                visible_shape = worker_attempt_record_prefix_shape(
                    *recordless, first, *names, parsed->chunk_id, parsed->attempt_ordinal,
                    initial_record_bytes);
                if (!visible_shape.has_value() || !exact_visible_prefix(first, *visible_shape)) {
                    return attach_publication(reject_lower_priority(diagnostic(
                        DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
                }
                if (const auto authority = claim.revalidate_authority();
                    authority.status != DistributedSieveWaveStoreStatus::ready) {
                    return attach_publication(authority);
                }
                auto confirmed = capture_manifest_bound_inventory_witness(
                    state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
                if (!confirmed) {
                    if (const auto authority = claim.revalidate_authority();
                        authority.status != DistributedSieveWaveStoreStatus::ready) {
                        return attach_publication(authority);
                    }
                    return attach_publication(std::move(confirmed.diagnostic));
                }
                const auto confirmed_shape = worker_attempt_record_prefix_shape(
                    *recordless, confirmed, *names, parsed->chunk_id, parsed->attempt_ordinal,
                    initial_record_bytes);
                if (!confirmed_shape.has_value() || *confirmed_shape != *visible_shape ||
                    !exact_visible_prefix(confirmed, *confirmed_shape) ||
                    !worker_attempt_start_observations_equal(first, confirmed)) {
                    return attach_publication(reject_lower_priority(diagnostic(
                        DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
                }
                if (const auto authority = claim.revalidate_authority();
                    authority.status != DistributedSieveWaveStoreStatus::ready) {
                    return attach_publication(authority);
                }
                return lower;
            };

        const bool accepted_publication =
            published.status() == durable_record::RecordPublishStatus::durable &&
            published.disposition() == expected_disposition &&
            published.canonical_snapshot().has_value() &&
            expected_source_snapshot == published.canonical_snapshot();
        if (!accepted_publication) {
            std::optional<WorkerAttemptRecordPrefixShape> visible_shape;
            publication_outcome = adjudicate_visible_prefix(publication_outcome, visible_shape);
            if (published.status() == durable_record::RecordPublishStatus::durable &&
                published.disposition() == durable_record::RecordPublishDisposition::created &&
                publication_outcome.status == DistributedSieveWaveStoreStatus::ready) {
                publication_outcome.status =
                    DistributedSieveWaveStoreStatus::reconciliation_required;
                publication_outcome.native_error = protocol_error();
            } else if (published.status() == durable_record::RecordPublishStatus::durable &&
                       publication_outcome.status == DistributedSieveWaveStoreStatus::ready) {
                publication_outcome.status = DistributedSieveWaveStoreStatus::namespace_conflict;
                publication_outcome.native_error = protocol_error();
            }
            return fail_with(std::move(publication_outcome));
        }

        const auto exact_normalized =
            [&](const ManifestBoundInventoryWitnessResult& observed) noexcept {
                const auto shape = worker_attempt_record_prefix_shape(
                    *recordless, observed, *names, parsed->chunk_id, parsed->attempt_ordinal,
                    initial_record_bytes);
                if (!shape.has_value() ||
                    *shape != WorkerAttemptRecordPrefixShape::canonical_only) {
                    return false;
                }
                const auto* witness = find_worker_attempt_record_witness(observed, parsed->chunk_id,
                                                                         parsed->attempt_ordinal);
                return witness != nullptr &&
                       witness->canonical_snapshot == published.canonical_snapshot() &&
                       !witness->pending_snapshot.has_value();
            };

        if (const auto authority = claim.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(authority));
        }
        auto first_normalized = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!first_normalized || !exact_normalized(first_normalized)) {
            return fail_with(attach_publication(reject_lower_priority(
                first_normalized ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                              protocol_error())
                                 : std::move(first_normalized.diagnostic))));
        }
        auto opened_record = open_worker_attempt_canonical_record(
            state.root_fd, names->canonical_record_leaf, *published.canonical_snapshot(),
            initial_record_bytes, state.creator_process_id);
        if (!opened_record) {
            return fail_with(
                attach_publication(reject_lower_priority(std::move(opened_record.diagnostic))));
        }
        const auto reject_after_pin = [&](DistributedSieveWaveStoreDiagnostic lower) noexcept {
            if (const auto authority = revalidate_bindings();
                authority.status != DistributedSieveWaveStoreStatus::ready) {
                return authority;
            }
            if (const auto pinned = validate_exact_worker_attempt_record_handle(
                    state.root_fd, opened_record.canonical_record.get(),
                    names->canonical_record_leaf, *published.canonical_snapshot(),
                    initial_record_bytes, state.creator_process_id);
                pinned.status != DistributedSieveWaveStoreStatus::ready) {
                return pinned;
            }
            return lower;
        };
        if (const auto authority = claim.revalidate_authority();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(authority));
        }
        if (const auto hooked = invoke_private_lease_base_lock_hook(
                hooks.after_first_normalized_successor_validation, hooks.context,
                state.creator_process_id);
            hooked.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(hooked));
        }
        if (const auto authority = revalidate_bindings();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(authority));
        }
        if (const auto pinned = validate_exact_worker_attempt_record_handle(
                state.root_fd, opened_record.canonical_record.get(), names->canonical_record_leaf,
                *published.canonical_snapshot(), initial_record_bytes, state.creator_process_id);
            pinned.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(reject_after_pin(pinned)));
        }
        auto confirmed_normalized = capture_manifest_bound_inventory_witness(
            state.root_fd, state.manifest, state.absolute_root, state.creator_process_id);
        if (!confirmed_normalized || !exact_normalized(confirmed_normalized) ||
            !worker_attempt_start_observations_equal(first_normalized, confirmed_normalized)) {
            return fail_with(attach_publication(reject_after_pin(
                confirmed_normalized
                    ? diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                                 protocol_error())
                    : std::move(confirmed_normalized.diagnostic))));
        }
        if (const auto pinned = validate_exact_worker_attempt_record_handle(
                state.root_fd, opened_record.canonical_record.get(), names->canonical_record_leaf,
                *published.canonical_snapshot(), initial_record_bytes, state.creator_process_id);
            pinned.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(reject_after_pin(pinned)));
        }
        if (const auto authority = revalidate_bindings();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(authority));
        }

        *claim.expected_private_lease_base_lock_leaves_ =
            confirmed_normalized.inventory->private_lease_base_lock_leaves;
        *claim.expected_private_lease_base_lock_identities_ =
            *confirmed_normalized.base_lock_identities;
        *claim.expected_private_lease_reservation_witnesses_ =
            *confirmed_normalized.private_lease_witnesses;
        *claim.expected_worker_attempt_record_witnesses_ =
            confirmed_normalized.inventory->worker_attempt_records;
        if (const auto authority = revalidate_bindings();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(authority));
        }
        if (const auto pinned = validate_exact_worker_attempt_record_handle(
                state.root_fd, opened_record.canonical_record.get(), names->canonical_record_leaf,
                *published.canonical_snapshot(), initial_record_bytes, state.creator_process_id);
            pinned.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(reject_after_pin(pinned)));
        }
        if (const auto validated = claim.revalidate();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(validated));
        }

        publication_outcome.last_worker_attempt_reconcile_fault_point =
            DistributedSieveWorkerAttemptReconcileFaultPoint::RecordNormalized;
        bool stop_after_normalized = false;
        if (!process_matches(state.creator_process_id)) {
            return fail_with(attach_publication(process_mismatch()));
        }
        if (hooks.stop_after != nullptr) {
            stop_after_normalized = hooks.stop_after(
                DistributedSieveWorkerAttemptReconcileFaultPoint::RecordNormalized, hooks.context);
        }
        if (!process_matches(state.creator_process_id)) {
            return fail_with(attach_publication(process_mismatch()));
        }
        if (const auto authority = revalidate_bindings();
            authority.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(authority));
        }
        if (const auto pinned = validate_exact_worker_attempt_record_handle(
                state.root_fd, opened_record.canonical_record.get(), names->canonical_record_leaf,
                *published.canonical_snapshot(), initial_record_bytes, state.creator_process_id);
            pinned.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(reject_after_pin(pinned)));
        }
        if (const auto validated = claim.revalidate();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(validated));
        }
        if (stop_after_normalized) {
            auto interrupted = diagnostic(DistributedSieveWaveStoreStatus::interrupted,
                                          std::make_error_code(std::errc::operation_canceled));
            return fail_with(attach_publication(reject_after_pin(std::move(interrupted))));
        }

        const auto* normalized_record = find_worker_attempt_record_witness(
            confirmed_normalized, parsed->chunk_id, parsed->attempt_ordinal);
        if (normalized_record == nullptr ||
            normalized_record->canonical_snapshot != published.canonical_snapshot() ||
            normalized_record->pending_snapshot.has_value()) {
            return fail_with(attach_publication(reject_after_pin(diagnostic(
                DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()))));
        }
        DistributedSieveStartedAttemptCleanupAdmission admission(
            std::move(opened_record.canonical_record), *names, *normalized_record,
            state.creator_process_id);

        std::optional<DistributedSieveReconciledWorkerAttemptV1> reconciled;
        DistributedSieveWaveStoreDiagnostic recovery_outcome;
        try {
            {
                DistributedSieveFdPrivateLeaseRecoveryTarget target(claim, std::move(admission),
                                                                    hooks.recovery);
                const auto run = private_lease::run_private_lease_recovery_protocol(target);
                if (run == private_lease::PrivateLeaseRecoveryRunResult::Interrupted) {
                    recovery_outcome = target.interruption_diagnostic();
                } else if (run == private_lease::PrivateLeaseRecoveryRunResult::Rejected) {
                    recovery_outcome = diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                                  invalid_argument_error());
                } else if (!target.completed()) {
                    recovery_outcome = diagnostic(
                        DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
                } else {
                    reconciled = target.started_completion();
                    if (!reconciled.has_value()) {
                        recovery_outcome = diagnostic(
                            DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
                    }
                }
            }
        } catch (const DistributedSieveFdPrivateLeaseRecoveryTarget::Failure& failure) {
            recovery_outcome = failure.diagnostic;
        }
        if (!reconciled.has_value() ||
            recovery_outcome.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(std::move(recovery_outcome)));
        }

        owned_claim.claim.reset();
        publication_outcome.status = DistributedSieveWaveStoreStatus::ready;
        publication_outcome.native_error.clear();
        return {std::move(reconciled), std::move(publication_outcome)};
    } catch (const DistributedSieveFdPrivateLeaseRecoveryTarget::Failure& failure) {
        return fail_with(failure.diagnostic);
    } catch (const std::bad_alloc&) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                    std::make_error_code(std::errc::not_enough_memory)));
    } catch (...) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                    std::make_error_code(std::errc::io_error)));
    }
#endif
}

DistributedSievePrivateLeaseReservationResult
reserve_worker_attempt_private_lease(DistributedSievePrivateLeaseRootClaimResult&& claimed,
                                     DistributedSievePrivateLeaseProtocolTestHooks hooks) noexcept {
    auto owned_claim = std::move(claimed);
    if (owned_claim.claim == nullptr ||
        owned_claim.diagnostic.status != DistributedSieveWaveStoreStatus::ready ||
        !owned_claim.claim->owned_by_current_process()) {
        auto outcome = std::move(owned_claim.diagnostic);
        if (outcome.status == DistributedSieveWaveStoreStatus::ready) {
            outcome = diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                 invalid_argument_error());
        }
        owned_claim.claim.reset();
        return {std::nullopt, std::move(outcome)};
    }

    std::optional<DistributedSievePrivateLeaseReservationReceipt> receipt;
    DistributedSieveWaveStoreDiagnostic outcome;
    try {
        {
            DistributedSieveFdPrivateLeaseReservationTarget target(*owned_claim.claim, hooks);
            const auto run = private_lease::run_private_lease_reservation_protocol(target);
            if (run == private_lease::PrivateLeaseReservationRunResult::Interrupted) {
                outcome = target.interruption_diagnostic();
            } else {
                auto completed = target.take_completion();
                DistributedSievePrivateLeaseReservationReceipt prepared(
                    owned_claim.claim->wave_store_state_, std::move(completed.worker_attempt_names),
                    completed.base_lock_identity, std::move(completed.final_witness),
                    owned_claim.claim->creator_process_id_);
                receipt.emplace(std::move(prepared));
            }
        }
    } catch (const DistributedSieveFdPrivateLeaseReservationTarget::Failure& failure) {
        outcome = failure.diagnostic;
    } catch (const std::bad_alloc&) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                             std::make_error_code(std::errc::not_enough_memory));
    } catch (...) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                             std::make_error_code(std::errc::io_error));
    }

    // The reservation result is deliberately not a live target capability.
    // Close the target flock, then clear the same-State root slot before the
    // caller can observe either success or interruption.
    owned_claim.claim.reset();
    if (receipt.has_value() && outcome.status == DistributedSieveWaveStoreStatus::ready) {
        return {std::move(receipt), std::move(outcome)};
    }
    receipt.reset();
    if (outcome.status == DistributedSieveWaveStoreStatus::ready) {
        outcome = diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
    }
    return {std::nullopt, std::move(outcome)};
}

DistributedSieveWorkerAttemptStartResult
publish_worker_attempt_started(DistributedSievePrivateLeaseReservationReceipt&& reservation,
                               DistributedSieveWorkerAttemptStartTestHooks hooks) noexcept {
    auto consumed = std::move(reservation);
    const auto fail_with = [](DistributedSieveWaveStoreDiagnostic outcome,
                              DistributedSieveWorkerAttemptStartDisposition disposition =
                                  DistributedSieveWorkerAttemptStartDisposition::failed)
        -> DistributedSieveWorkerAttemptStartResult {
        return {std::nullopt, std::move(outcome), disposition};
    };

    if (!consumed.owned_by_current_process() || consumed.wave_store_state_ == nullptr ||
        consumed.creator_process_id_ != consumed.wave_store_state_->creator_process_id) {
        return fail_with(process_mismatch());
    }

    try {
        const auto parsed = parse_distributed_sieve_worker_attempt_leaf_v1(
            consumed.worker_attempt_names_.canonical_record_leaf);
        if (!parsed.has_value() || parsed->pending) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                        invalid_argument_error()));
        }
        const ChunkPlanV1* chunk = nullptr;
        for (const auto& candidate : consumed.wave_store_state_->manifest.chunks) {
            if (candidate.chunk_id == parsed->chunk_id) {
                chunk = &candidate;
                break;
            }
        }
        if (chunk == nullptr || chunk->sq_begin >= chunk->sq_end ||
            parsed->attempt_ordinal >= consumed.wave_store_state_->manifest.max_worker_attempts) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                        invalid_argument_error()));
        }
        const auto regenerated = distributed_sieve_worker_attempt_names_v1(
            chunk->relative_artifact_stem, parsed->chunk_id, parsed->attempt_ordinal);
        if (!regenerated.has_value() || *regenerated != consumed.worker_attempt_names_ ||
            consumed.final_witness_.base_lock_leaf != regenerated->base_lock_leaf ||
            consumed.final_witness_.boundary !=
                DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable ||
            consumed.final_witness_.lease_id == std::array<std::uint64_t, 2>{} ||
            !consumed.final_witness_.reserved_marker_identity.has_value() ||
            !consumed.final_witness_.directory_identity.has_value() ||
            !consumed.final_witness_.owner_marker_identity.has_value() ||
            !consumed.final_witness_.owned_marker_identity.has_value() ||
            consumed.final_witness_.work_package_residue.has_value() ||
            nil_identity(consumed.base_lock_identity_)) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                        invalid_argument_error()));
        }

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        (void)hooks;
        return fail_with(
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error()));
#else
        DistributedSieveWaveStore view(consumed.wave_store_state_);
        auto claimed = view.open_worker_attempt_private_lease_root(
            parsed->chunk_id, parsed->attempt_ordinal, hooks.base_lock);
        if (!claimed || claimed.claim == nullptr) {
            return fail_with(std::move(claimed.diagnostic));
        }
        auto& claim = *claimed.claim;
        if (claim.wave_store_state_ != consumed.wave_store_state_ ||
            claim.creator_process_id_ != consumed.creator_process_id_ ||
            claim.worker_attempt_names_ != std::optional<DistributedSieveWorkerAttemptNamesV1>(
                                               consumed.worker_attempt_names_) ||
            claim.base_lock_acquisition_ !=
                DistributedSievePrivateLeaseRootClaim::BaseLockAcquisition::OpenedExisting ||
            claim.base_lock_at_ == nullptr ||
            claim.base_lock_at_->identity() != consumed.base_lock_identity_ ||
            !claim.base_lock_at_->owned_by_current_process() ||
            !claim.expected_private_lease_base_lock_leaves_.has_value() ||
            !claim.expected_private_lease_base_lock_identities_.has_value() ||
            !claim.expected_private_lease_reservation_witnesses_.has_value() ||
            !claim.expected_worker_attempt_record_witnesses_.has_value()) {
            return fail_with(diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                        invalid_argument_error()));
        }

        const auto revalidate_bindings = [&]() noexcept -> DistributedSieveWaveStoreDiagnostic {
            return claim.revalidate_authority();
        };
        const auto reject_lower_priority = [&](DistributedSieveWaveStoreDiagnostic lower) noexcept {
            if (const auto bindings = revalidate_bindings();
                bindings.status != DistributedSieveWaveStoreStatus::ready) {
                return bindings;
            }
            return lower;
        };

        if (const auto validated = claim.revalidate();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(validated);
        }
        auto predecessor = capture_manifest_bound_inventory_witness(
            consumed.wave_store_state_->root_fd, consumed.wave_store_state_->manifest,
            consumed.wave_store_state_->absolute_root, consumed.creator_process_id_);
        if (!predecessor) {
            return fail_with(reject_lower_priority(std::move(predecessor.diagnostic)));
        }
        if (predecessor.inventory->private_lease_base_lock_leaves !=
                *claim.expected_private_lease_base_lock_leaves_ ||
            *predecessor.base_lock_identities !=
                *claim.expected_private_lease_base_lock_identities_ ||
            *predecessor.private_lease_witnesses !=
                *claim.expected_private_lease_reservation_witnesses_ ||
            predecessor.inventory->worker_attempt_records !=
                *claim.expected_worker_attempt_record_witnesses_ ||
            predecessor.inventory->private_lease_base_lock_leaves.size() !=
                predecessor.base_lock_identities->size() ||
            predecessor.inventory->private_lease_base_lock_leaves.size() !=
                predecessor.private_lease_witnesses->size()) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        const auto target_position =
            std::lower_bound(predecessor.inventory->private_lease_base_lock_leaves.begin(),
                             predecessor.inventory->private_lease_base_lock_leaves.end(),
                             regenerated->base_lock_leaf);
        if (target_position == predecessor.inventory->private_lease_base_lock_leaves.end() ||
            *target_position != regenerated->base_lock_leaf) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        const auto target_index = static_cast<std::size_t>(std::distance(
            predecessor.inventory->private_lease_base_lock_leaves.begin(), target_position));
        if (predecessor.private_lease_witnesses->at(target_index)
                .work_package_residue.has_value()) {
            return fail_with(reject_lower_priority(diagnostic(
                DistributedSieveWaveStoreStatus::reconciliation_required, protocol_error())));
        }
        if (predecessor.base_lock_identities->at(target_index) != consumed.base_lock_identity_ ||
            predecessor.private_lease_witnesses->at(target_index) != consumed.final_witness_ ||
            std::binary_search(predecessor.inventory->worker_attempt_record_leaves.begin(),
                               predecessor.inventory->worker_attempt_record_leaves.end(),
                               regenerated->canonical_record_leaf) ||
            std::binary_search(predecessor.inventory->worker_attempt_record_leaves.begin(),
                               predecessor.inventory->worker_attempt_record_leaves.end(),
                               regenerated->pending_record_leaf) ||
            find_worker_attempt_record_witness(predecessor, parsed->chunk_id,
                                               parsed->attempt_ordinal) != nullptr) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }

        std::vector<AttemptStartedV1> chain;
        chain.reserve(static_cast<std::size_t>(parsed->attempt_ordinal) + 1U);
        for (const auto& witness : predecessor.inventory->worker_attempt_records) {
            if (witness.chunk_id != parsed->chunk_id) {
                continue;
            }
            if (!witness.canonical_snapshot.has_value() || witness.pending_snapshot.has_value() ||
                witness.attempt_ordinal != chain.size() ||
                witness.attempt_ordinal >= parsed->attempt_ordinal) {
                return fail_with(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
            const auto historical_names = distributed_sieve_worker_attempt_names_v1(
                chunk->relative_artifact_stem, parsed->chunk_id, witness.attempt_ordinal);
            if (!historical_names.has_value()) {
                return fail_with(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
            const auto historical_lock =
                std::lower_bound(predecessor.inventory->private_lease_base_lock_leaves.begin(),
                                 predecessor.inventory->private_lease_base_lock_leaves.end(),
                                 historical_names->base_lock_leaf);
            if (historical_lock == predecessor.inventory->private_lease_base_lock_leaves.end() ||
                *historical_lock != historical_names->base_lock_leaf) {
                return fail_with(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
            const auto historical_index = static_cast<std::size_t>(std::distance(
                predecessor.inventory->private_lease_base_lock_leaves.begin(), historical_lock));
            if (predecessor.private_lease_witnesses->at(historical_index).boundary !=
                DistributedSievePrivateLeaseReservationBoundary::PermitAcquired) {
                return fail_with(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
            chain.push_back(witness.record);
        }
        if (chain.size() != parsed->attempt_ordinal) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }

        AttemptStartedV1 attempt{
            .manifest_digest = consumed.wave_store_state_->manifest.self_digest,
            .chunk_id = parsed->chunk_id,
            .sq_begin = chunk->sq_begin,
            .sq_end = chunk->sq_end,
            .attempt_ordinal = parsed->attempt_ordinal,
            .predecessor_digest = chain.empty() ? consumed.wave_store_state_->manifest.self_digest
                                                : chain.back().self_digest,
            .lease =
                LeaseIdentityV1{
                    .lease_id = LeaseIdV1{.limbs = consumed.final_witness_.lease_id},
                    .owner_marker = *consumed.final_witness_.owner_marker_identity,
                    .directory = *consumed.final_witness_.directory_identity,
                    .relative_stem = consumed.worker_attempt_names_.relative_lease_stem,
                },
            .retry_policy_version = consumed.wave_store_state_->manifest.retry_policy_version,
        };
        for (const auto& identity : *predecessor.base_lock_identities) {
            if (attempt.lease.owner_marker == identity || attempt.lease.directory == identity) {
                return fail_with(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
        }
        for (const auto& existing : predecessor.inventory->worker_attempt_records) {
            if (attempt_lease_identities_conflict(attempt.lease, existing.record.lease)) {
                return fail_with(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
            }
        }

        DistributedSieveProtocolRecordV1 protocol_record(std::move(attempt));
        if (const auto sealed = seal_distributed_sieve_record(protocol_record); !sealed) {
            auto failure =
                diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
            failure.protocol_status = sealed;
            return fail_with(reject_lower_priority(std::move(failure)));
        }
        auto encoded = encode_distributed_sieve_record(protocol_record);
        if (!encoded) {
            auto failure =
                diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
            failure.protocol_status = encoded.status;
            return fail_with(reject_lower_priority(std::move(failure)));
        }
        attempt = std::move(std::get<AttemptStartedV1>(protocol_record));
        chain.push_back(attempt);
        if (const auto chain_status = validate_worker_attempt_chain(
                consumed.wave_store_state_->manifest, parsed->chunk_id, chain, nullptr, nullptr);
            !chain_status) {
            return fail_with(reject_lower_priority(worker_attempt_protocol_conflict(chain_status)));
        }

        if (const auto hooked =
                invoke_private_lease_base_lock_hook(hooks.after_locked_predecessor_validation,
                                                    hooks.context, consumed.creator_process_id_);
            hooked.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(hooked);
        }
        if (const auto validated = claim.revalidate();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(validated);
        }
        auto confirmed_predecessor = capture_manifest_bound_inventory_witness(
            consumed.wave_store_state_->root_fd, consumed.wave_store_state_->manifest,
            consumed.wave_store_state_->absolute_root, consumed.creator_process_id_);
        if (!confirmed_predecessor) {
            return fail_with(reject_lower_priority(std::move(confirmed_predecessor.diagnostic)));
        }
        if (!worker_attempt_start_observations_equal(predecessor, confirmed_predecessor)) {
            return fail_with(reject_lower_priority(
                diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
        }
        if (const auto bindings = revalidate_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(bindings);
        }
        if (const auto hooked = invoke_private_lease_base_lock_hook(
                hooks.before_record_publication, hooks.context, consumed.creator_process_id_);
            hooked.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(hooked);
        }
        if (const auto validated = claim.revalidate();
            validated.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(validated);
        }

        WorkerAttemptStartHookBridge bridge{
            .hooks = hooks,
            .creator_process_id = consumed.creator_process_id_,
            .process_changed = false,
            .last_fault_point = std::nullopt,
        };
        const auto published = durable_record::publish_at(
            static_cast<durable_record::NativeHandle>(consumed.wave_store_state_->root_fd),
            regenerated->pending_record_leaf, regenerated->canonical_record_leaf, *encoded.bytes,
            durable_record::RecordTestHooks{
                .stop_after = bridge_worker_attempt_start_hook,
                .context = &bridge,
            });
        auto publication_outcome = worker_attempt_start_publication_diagnostic(published, bridge);
        const auto attach_publication = [&](DistributedSieveWaveStoreDiagnostic higher) noexcept {
            higher.publication_status = publication_outcome.publication_status;
            higher.publication_disposition = publication_outcome.publication_disposition;
            higher.last_worker_attempt_start_fault_point =
                publication_outcome.last_worker_attempt_start_fault_point;
            return higher;
        };
        const auto adjudicate_visible_prefix =
            [&](DistributedSieveWaveStoreDiagnostic lower,
                std::optional<WorkerAttemptRecordPrefixShape>& visible_shape) noexcept {
                const auto published_snapshot_matches =
                    [&](const ManifestBoundInventoryWitnessResult& observed,
                        WorkerAttemptRecordPrefixShape shape) noexcept {
                        if (!published.canonical_snapshot().has_value()) {
                            return true;
                        }
                        if (shape != WorkerAttemptRecordPrefixShape::canonical_only &&
                            shape != WorkerAttemptRecordPrefixShape::identical_dual) {
                            return false;
                        }
                        const auto* witness = find_worker_attempt_record_witness(
                            observed, parsed->chunk_id, parsed->attempt_ordinal);
                        return witness != nullptr &&
                               witness->canonical_snapshot == published.canonical_snapshot();
                    };
                if (const auto bindings = revalidate_bindings();
                    bindings.status != DistributedSieveWaveStoreStatus::ready) {
                    return attach_publication(bindings);
                }
                auto first = capture_manifest_bound_inventory_witness(
                    consumed.wave_store_state_->root_fd, consumed.wave_store_state_->manifest,
                    consumed.wave_store_state_->absolute_root, consumed.creator_process_id_);
                if (!first) {
                    if (const auto bindings = revalidate_bindings();
                        bindings.status != DistributedSieveWaveStoreStatus::ready) {
                        return attach_publication(bindings);
                    }
                    return attach_publication(std::move(first.diagnostic));
                }
                visible_shape = worker_attempt_record_prefix_shape(
                    predecessor, first, *regenerated, parsed->chunk_id, parsed->attempt_ordinal,
                    *encoded.bytes);
                if (!visible_shape.has_value() ||
                    !published_snapshot_matches(first, *visible_shape)) {
                    return attach_publication(reject_lower_priority(diagnostic(
                        DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
                }
                if (const auto bindings = revalidate_bindings();
                    bindings.status != DistributedSieveWaveStoreStatus::ready) {
                    return attach_publication(bindings);
                }
                auto confirmed = capture_manifest_bound_inventory_witness(
                    consumed.wave_store_state_->root_fd, consumed.wave_store_state_->manifest,
                    consumed.wave_store_state_->absolute_root, consumed.creator_process_id_);
                if (!confirmed) {
                    if (const auto bindings = revalidate_bindings();
                        bindings.status != DistributedSieveWaveStoreStatus::ready) {
                        return attach_publication(bindings);
                    }
                    return attach_publication(std::move(confirmed.diagnostic));
                }
                const auto confirmed_shape = worker_attempt_record_prefix_shape(
                    predecessor, confirmed, *regenerated, parsed->chunk_id, parsed->attempt_ordinal,
                    *encoded.bytes);
                if (!confirmed_shape.has_value() || *confirmed_shape != *visible_shape ||
                    !published_snapshot_matches(confirmed, *confirmed_shape) ||
                    !worker_attempt_start_observations_equal(first, confirmed)) {
                    return attach_publication(reject_lower_priority(diagnostic(
                        DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error())));
                }
                if (const auto bindings = revalidate_bindings();
                    bindings.status != DistributedSieveWaveStoreStatus::ready) {
                    return attach_publication(bindings);
                }
                return lower;
            };

        const bool fresh_canonical =
            published.status() == durable_record::RecordPublishStatus::durable &&
            published.disposition() == durable_record::RecordPublishDisposition::created &&
            published.canonical_snapshot().has_value();
        if (!fresh_canonical) {
            std::optional<WorkerAttemptRecordPrefixShape> visible_shape;
            publication_outcome =
                adjudicate_visible_prefix(std::move(publication_outcome), visible_shape);
            const bool reconciliation_needed =
                visible_shape.has_value() &&
                *visible_shape != WorkerAttemptRecordPrefixShape::absent;
            if (published.status() == durable_record::RecordPublishStatus::durable &&
                publication_outcome.status == DistributedSieveWaveStoreStatus::ready) {
                publication_outcome.status =
                    DistributedSieveWaveStoreStatus::reconciliation_required;
            }
            return fail_with(std::move(publication_outcome),
                             reconciliation_needed ||
                                     published.disposition() !=
                                         durable_record::RecordPublishDisposition::none
                                 ? DistributedSieveWorkerAttemptStartDisposition::reconcile_required
                                 : DistributedSieveWorkerAttemptStartDisposition::failed);
        }

        const auto exact_fresh_successor =
            [&](const ManifestBoundInventoryWitnessResult& observed) noexcept {
                const auto shape = worker_attempt_record_prefix_shape(
                    predecessor, observed, *regenerated, parsed->chunk_id, parsed->attempt_ordinal,
                    *encoded.bytes);
                if (!shape.has_value() ||
                    *shape != WorkerAttemptRecordPrefixShape::canonical_only) {
                    return false;
                }
                const auto* witness = find_worker_attempt_record_witness(observed, parsed->chunk_id,
                                                                         parsed->attempt_ordinal);
                return witness != nullptr &&
                       witness->canonical_snapshot == published.canonical_snapshot() &&
                       !witness->pending_snapshot.has_value();
            };

        if (const auto bindings = revalidate_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(bindings),
                             DistributedSieveWorkerAttemptStartDisposition::reconcile_required);
        }
        auto first_successor = capture_manifest_bound_inventory_witness(
            consumed.wave_store_state_->root_fd, consumed.wave_store_state_->manifest,
            consumed.wave_store_state_->absolute_root, consumed.creator_process_id_);
        if (!first_successor) {
            return fail_with(
                attach_publication(reject_lower_priority(std::move(first_successor.diagnostic))),
                DistributedSieveWorkerAttemptStartDisposition::reconcile_required);
        }
        if (!exact_fresh_successor(first_successor)) {
            return fail_with(
                attach_publication(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()))),
                DistributedSieveWorkerAttemptStartDisposition::reconcile_required);
        }
        if (const auto bindings = revalidate_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(bindings),
                             DistributedSieveWorkerAttemptStartDisposition::reconcile_required);
        }
        if (const auto hooked =
                invoke_private_lease_base_lock_hook(hooks.after_first_successor_validation,
                                                    hooks.context, consumed.creator_process_id_);
            hooked.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(hooked),
                             DistributedSieveWorkerAttemptStartDisposition::reconcile_required);
        }
        if (const auto bindings = revalidate_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(bindings),
                             DistributedSieveWorkerAttemptStartDisposition::reconcile_required);
        }
        auto confirmed_successor = capture_manifest_bound_inventory_witness(
            consumed.wave_store_state_->root_fd, consumed.wave_store_state_->manifest,
            consumed.wave_store_state_->absolute_root, consumed.creator_process_id_);
        if (!confirmed_successor) {
            return fail_with(attach_publication(
                                 reject_lower_priority(std::move(confirmed_successor.diagnostic))),
                             DistributedSieveWorkerAttemptStartDisposition::reconcile_required);
        }
        if (!exact_fresh_successor(confirmed_successor) ||
            !worker_attempt_start_observations_equal(first_successor, confirmed_successor)) {
            return fail_with(
                attach_publication(reject_lower_priority(diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, protocol_error()))),
                DistributedSieveWorkerAttemptStartDisposition::reconcile_required);
        }
        if (const auto bindings = revalidate_bindings();
            bindings.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(attach_publication(bindings),
                             DistributedSieveWorkerAttemptStartDisposition::reconcile_required);
        }

        std::optional<DistributedSieveWorkerAttemptStartReceipt> start_receipt;
        DistributedSieveWorkerAttemptStartReceipt prepared(
            std::move(consumed.wave_store_state_), std::move(consumed.worker_attempt_names_),
            std::move(attempt), *published.canonical_snapshot(), std::move(consumed.final_witness_),
            std::move(claim.base_lock_at_), consumed.creator_process_id_);
        start_receipt.emplace(std::move(prepared));
        claimed.claim.reset();
        publication_outcome.status = DistributedSieveWaveStoreStatus::ready;
        return {std::move(start_receipt), std::move(publication_outcome),
                DistributedSieveWorkerAttemptStartDisposition::fresh_start};
#endif
    } catch (const std::bad_alloc&) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                                    std::make_error_code(std::errc::not_enough_memory)));
    } catch (...) {
        return fail_with(diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                                    std::make_error_code(std::errc::io_error)));
    }
}

} // namespace gnfs::sieve::distributed_sieve_resume_detail

namespace gnfs::sieve::distributed_sieve_worker_launcher_detail {

DistributedSieveWorkerLaunchSlotV1::DistributedSieveWorkerLaunchSlotV1(
    distributed_sieve_resume_detail::DistributedSieveWorkerAttemptStartReceipt&& receipt,
    std::vector<std::string> arguments) noexcept
    : receipt_(std::move(receipt)), arguments_(std::move(arguments)) {}

DistributedSieveWorkerLaunchRequestV1::DistributedSieveWorkerLaunchRequestV1(
    std::string executable_path, std::vector<DistributedSieveWorkerLaunchSlotV1> slots,
    DistributedSieveWorkerLauncherTestHooksV1 hooks) noexcept
    : executable_path_(std::move(executable_path)), slots_(std::move(slots)), hooks_(hooks) {}

DistributedSieveLaunchedWorkerAttemptV1::DistributedSieveLaunchedWorkerAttemptV1(
    std::unique_ptr<distributed_sieve_resume_detail::DistributedSieveWorkerAttemptStartReceipt>&&
        receipt,
    distributed_sieve_worker_process_detail::DistributedSieveWorkerProcess&& process) noexcept
    : receipt_(std::move(receipt)), process_(std::move(process)) {}

DistributedSieveLaunchedWorkerAttemptV1::~DistributedSieveLaunchedWorkerAttemptV1() noexcept {
    if (!terminal_reaped_ && receipt_ != nullptr) {
        // No terminal proof means the child may still hold or use fd 5. Leak
        // this receipt intentionally until process exit instead of releasing
        // its BaseLock early. The process member still closes its report
        // endpoint during ordinary member destruction.
        (void)receipt_.release();
    }
}

const AttemptStartedV1& DistributedSieveLaunchedWorkerAttemptV1::record() const noexcept {
    return receipt_->record();
}

distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic
DistributedSieveLaunchedWorkerAttemptV1::revalidate() const noexcept {
    return receipt_->revalidate();
}

distributed_sieve_worker_process_detail::DistributedSieveWorkerProcessId
DistributedSieveLaunchedWorkerAttemptV1::process_id() const noexcept {
    return process_.process_id();
}

int DistributedSieveLaunchedWorkerAttemptV1::report_descriptor() const noexcept {
    return process_.report_descriptor();
}

distributed_sieve_worker_process_detail::DistributedSieveWorkerProcessWaitResult
DistributedSieveLaunchedWorkerAttemptV1::wait_terminal(
    distributed_sieve_worker_process_detail::DistributedSieveWorkerProcessWaitTestHooks
        hooks) noexcept {
    const auto observed = process_.wait_terminal(hooks);
    terminal_reaped_ = terminal_reaped_ || observed.reaped;
    return observed;
}

int DistributedSieveLaunchedWorkerAttemptV1::release_report_descriptor() noexcept {
    return process_.release_report_descriptor();
}

} // namespace gnfs::sieve::distributed_sieve_worker_launcher_detail

namespace gnfs::sieve::distributed_sieve_resume_detail {

distributed_sieve_worker_launcher_detail::DistributedSieveWorkerLaunchBatchResultV1
DistributedSieveWaveStore::launch_worker_process_batch_v1(
    distributed_sieve_worker_launcher_detail::DistributedSieveWorkerLaunchRequestV1&& request,
    const DistributedSieveWorkIdentityV1& identity,
    const distributed_sieve_execution_policy_detail::DistributedSieveFrozenExecutionPolicyV1&
        frozen_policy,
    const core::PolynomialContext& polynomial,
    const factor_base::FactorBase& factor_base) const noexcept {
    namespace launcher = distributed_sieve_worker_launcher_detail;
    namespace process = distributed_sieve_worker_process_detail;
    namespace carrier = distributed_sieve_worker_work_package_file_detail;
    namespace execution = distributed_sieve_execution_policy_detail;

    auto consumed = std::move(request);
    launcher::DistributedSieveWorkerLaunchBatchResultV1 result;
    bool armed = false;
    launcher::DistributedSieveWorkerLaunchPhaseV1 active_phase =
        launcher::DistributedSieveWorkerLaunchPhaseV1::request_validation;
    std::size_t active_slot = launcher::DISTRIBUTED_SIEVE_WORKER_LAUNCH_NO_SLOT;
    bool batch_absence_unproven = false;

    // Once any carrier transaction succeeds, every failure remains
    // reconciliation-required until the complete batch has repeated its
    // directory/absence checks and revalidated every receipt.
    const auto set_failure = [&](launcher::DistributedSieveWorkerLaunchPhaseV1 phase,
                                 std::size_t slot) noexcept {
        result.diagnostic.phase = phase;
        result.diagnostic.slot = slot;
        result.diagnostic.reconciliation_required =
            result.diagnostic.reconciliation_required || batch_absence_unproven;
        result.disposition =
            armed ? launcher::DistributedSieveWorkerLaunchDispositionV1::armed_no_child
                  : launcher::DistributedSieveWorkerLaunchDispositionV1::failed_before_gate;
    };

    try {
        const std::size_t slot_count = consumed.slots_.size();
        if (state_ == nullptr || !process_matches(state_->creator_process_id) || slot_count == 0U) {
            set_failure(launcher::DistributedSieveWorkerLaunchPhaseV1::request_validation,
                        launcher::DISTRIBUTED_SIEVE_WORKER_LAUNCH_NO_SLOT);
            result.diagnostic.wave_store =
                process_matches(state_ != nullptr ? state_->creator_process_id : 0U)
                    ? diagnostic(DistributedSieveWaveStoreStatus::invalid_request,
                                 invalid_argument_error())
                    : process_mismatch();
            return result;
        }

        result.children.resize(slot_count);
        std::vector<std::unique_ptr<DistributedSieveWorkerAttemptStartReceipt>> receipt_anchors(
            slot_count);
        for (std::size_t index = 0; index < slot_count; ++index) {
            receipt_anchors[index] = std::make_unique<DistributedSieveWorkerAttemptStartReceipt>(
                std::move(consumed.slots_[index].receipt_));
        }
        std::vector<std::vector<std::byte>> bootstrap_frames(slot_count);
        std::vector<std::vector<std::string_view>> argument_views(slot_count);
        std::vector<process::DistributedSieveWorkerProcessSpawnSpec> spawn_specs;
        spawn_specs.reserve(slot_count);

        std::optional<std::uint32_t> prior_chunk_id;
        for (std::size_t index = 0; index < slot_count; ++index) {
            active_slot = index;
            auto& slot = consumed.slots_[index];
            auto& receipt = *receipt_anchors[index];
            const auto& record = receipt.record_;
            auto& child = result.children[index];
            child.chunk_id = record.chunk_id;
            child.attempt_ordinal = record.attempt_ordinal;
            child.attempt_started_digest = record.self_digest;

            const ChunkPlanV1* manifest_chunk = nullptr;
            for (const auto& candidate : state_->manifest.chunks) {
                if (candidate.chunk_id == record.chunk_id) {
                    manifest_chunk = &candidate;
                    break;
                }
            }
            const bool receipt_shape_valid =
                receipt.wave_store_state_.get() == state_.get() &&
                receipt.owned_by_current_process() &&
                receipt.creator_process_id_ == state_->creator_process_id &&
                receipt.base_lock_at_ != nullptr &&
                receipt.final_witness_.directory_identity.has_value() &&
                (!prior_chunk_id.has_value() || *prior_chunk_id < record.chunk_id) &&
                manifest_chunk != nullptr && manifest_chunk->sq_begin == record.sq_begin &&
                manifest_chunk->sq_end == record.sq_end &&
                record.attempt_ordinal < state_->manifest.max_worker_attempts &&
                record.manifest_digest == state_->manifest.self_digest &&
                record.retry_policy_version == state_->manifest.retry_policy_version;
            if (!receipt_shape_valid) {
                set_failure(launcher::DistributedSieveWorkerLaunchPhaseV1::request_validation,
                            index);
                result.diagnostic.wave_store = diagnostic(
                    DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error());
                return result;
            }
            prior_chunk_id = record.chunk_id;

            DistributedSieveProtocolRecordV1 sealed(record);
            auto encoded = encode_distributed_sieve_record(sealed);
            if (!encoded) {
                set_failure(launcher::DistributedSieveWorkerLaunchPhaseV1::request_validation,
                            index);
                result.diagnostic.protocol = encoded.status;
                return result;
            }
            bootstrap_frames[index] = std::move(*encoded.bytes);

            auto& views = argument_views[index];
            views.reserve(slot.arguments_.size());
            for (const auto& argument : slot.arguments_) {
                views.emplace_back(argument);
            }
        }

        for (std::size_t index = 0; index < slot_count; ++index) {
            spawn_specs.push_back({
                .executable_path = consumed.executable_path_,
                .arguments = argument_views[index],
                .bootstrap_frame = bootstrap_frames[index],
            });
        }

        active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::process_preparation;
        active_slot = launcher::DISTRIBUTED_SIEVE_WORKER_LAUNCH_NO_SLOT;
        if (consumed.hooks_.force_fixed_capability_close_all_unavailable ||
            !process::distributed_sieve_worker_process_fixed_capability_close_all_supported()) {
            set_failure(active_phase, active_slot);
            result.diagnostic.transport = {
                .error = process::DistributedSieveWorkerProcessTransportError::platform_unavailable,
                .native_error = ENOTSUP,
            };
            return result;
        }
        auto prepared_processes =
            process::prepare_distributed_sieve_worker_process_batch(spawn_specs);
        if (!prepared_processes) {
            set_failure(active_phase, active_slot);
            result.diagnostic.transport = prepared_processes.diagnostic;
            return result;
        }

        std::vector<std::optional<carrier::DistributedSieveWorkerWorkPackageFileV1>> packages(
            slot_count);
        std::vector<process::DistributedSieveWorkerProcessFixedCapabilitySourcesV1> capabilities(
            slot_count);
#if !defined(_WIN32)
        std::vector<UniqueFd> attempt_directories(slot_count);
#endif

        active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::work_binding;
        if (const auto status = validate_manifest_work_identity(state_->manifest, identity);
            !status) {
            set_failure(active_phase, active_slot);
            result.diagnostic.protocol = status;
            return result;
        }
        auto bound = execution::bind_distributed_sieve_work_v1(identity, frozen_policy, polynomial,
                                                               factor_base);
        if (!bound) {
            set_failure(active_phase, active_slot);
            result.diagnostic.protocol = bound.status;
            return result;
        }
        if (bound.work->work_digest != state_->manifest.work_sha256) {
            set_failure(active_phase, active_slot);
            result.diagnostic.protocol = {
                .error = DistributedSieveProtocolError::invalid_value,
            };
            return result;
        }

        // No failure before this point has crossed the one-shot receipt gate.
        // From here onward every receipt remains consumed on every outcome.
        armed = true;
        result.disposition = launcher::DistributedSieveWorkerLaunchDispositionV1::armed_no_child;

        active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::initial_receipt_revalidation;
        for (std::size_t index = 0; index < slot_count; ++index) {
            active_slot = index;
            const auto checked = receipt_anchors[index]->revalidate();
            if (checked.status != DistributedSieveWaveStoreStatus::ready) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = checked;
                return result;
            }
        }
        if (consumed.hooks_.after_initial_receipts != nullptr) {
            consumed.hooks_.after_initial_receipts(consumed.hooks_.context);
        }
        // The hook represents arbitrary concurrent namespace activity. Pin
        // every receipt again before the first carrier is allowed to create
        // even a transient named package.
        for (std::size_t index = 0; index < slot_count; ++index) {
            active_slot = index;
            const auto checked = receipt_anchors[index]->revalidate();
            if (checked.status != DistributedSieveWaveStoreStatus::ready) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = checked;
                return result;
            }
        }

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
        set_failure(launcher::DistributedSieveWorkerLaunchPhaseV1::attempt_directory_binding,
                    launcher::DISTRIBUTED_SIEVE_WORKER_LAUNCH_NO_SLOT);
        result.diagnostic.wave_store =
            diagnostic(DistributedSieveWaveStoreStatus::platform_unsupported, unsupported_error());
        return result;
#else
        for (std::size_t index = 0; index < slot_count; ++index) {
            active_slot = index;
            auto& receipt = *receipt_anchors[index];
            const auto& directory_leaf = receipt.worker_attempt_names_.private_directory_leaf;
            const NativeIdentityV1 expected_directory_identity =
                *receipt.final_witness_.directory_identity;

            active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::attempt_directory_binding;
            const int directory_fd =
                openat_retrying_eintr(state_->root_fd, directory_leaf.c_str(),
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (directory_fd < 0) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
                return result;
            }
            attempt_directories[index].reset(directory_fd);
            if (const auto checked = validate_private_lease_directory_binding(
                    state_->root_fd, attempt_directories[index].get(), directory_leaf,
                    state_->creator_process_id, expected_directory_identity);
                checked.status != DistributedSieveWaveStoreStatus::ready) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = checked;
                return result;
            }

            if (consumed.hooks_.before_carrier != nullptr) {
                consumed.hooks_.before_carrier(index, consumed.hooks_.context);
            }
            // The hook stands in for activity immediately before the named
            // carrier window. Re-pin both the receipt and its exact
            // root-relative directory binding while no package name exists.
            active_phase =
                launcher::DistributedSieveWorkerLaunchPhaseV1::initial_receipt_revalidation;
            if (const auto checked = receipt.revalidate();
                checked.status != DistributedSieveWaveStoreStatus::ready) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = checked;
                return result;
            }
            active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::attempt_directory_binding;
            if (const auto checked = validate_private_lease_directory_binding(
                    state_->root_fd, attempt_directories[index].get(), directory_leaf,
                    state_->creator_process_id, expected_directory_identity);
                checked.status != DistributedSieveWaveStoreStatus::ready) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = checked;
                return result;
            }

            active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::work_package_creation;
            auto created = carrier::create_distributed_sieve_worker_work_package_file_v1(
                {
                    .borrowed_attempt_directory_handle =
                        static_cast<carrier::DistributedSieveWorkerWorkPackageNativeHandle>(
                            attempt_directories[index].get()),
                    .expected_directory_identity = expected_directory_identity,
                    .creator_process_id = state_->creator_process_id,
                },
                identity);
            if (!created) {
                set_failure(active_phase, index);
                result.diagnostic.carrier = created.diagnostic;
                if (created.diagnostic.named_may_remain) {
                    result.diagnostic.reconciliation_required = true;
                    result.diagnostic.wave_store = diagnostic(
                        DistributedSieveWaveStoreStatus::reconciliation_required, protocol_error());
                }
                return result;
            }

            batch_absence_unproven = true;
            packages[index].emplace(std::move(*created.file));

            if (consumed.hooks_.after_carrier != nullptr) {
                consumed.hooks_.after_carrier(index, consumed.hooks_.context);
            }

            active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::attempt_directory_binding;
            if (const auto checked = validate_private_lease_directory_binding(
                    state_->root_fd, attempt_directories[index].get(), directory_leaf,
                    state_->creator_process_id, expected_directory_identity);
                checked.status != DistributedSieveWaveStoreStatus::ready) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = checked;
                return result;
            }

            struct stat fixed_leaf_metadata{};
            if (fstatat_retrying_eintr(
                    attempt_directories[index].get(),
                    carrier::DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1.data(),
                    fixed_leaf_metadata) == 0) {
                set_failure(active_phase, index);
                result.diagnostic.reconciliation_required = true;
                result.diagnostic.wave_store = diagnostic(
                    DistributedSieveWaveStoreStatus::reconciliation_required, protocol_error());
                return result;
            }
            if (errno != ENOENT) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
                return result;
            }
        }

        // A later-slot hook may invalidate an earlier slot after its immediate
        // post-carrier check. Keep every directory descriptor pinned and close
        // the batch as one absence proof before assembling any capability.
        active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::attempt_directory_binding;
        for (std::size_t index = 0; index < slot_count; ++index) {
            active_slot = index;
            auto& receipt = *receipt_anchors[index];
            const auto& directory_leaf = receipt.worker_attempt_names_.private_directory_leaf;
            const NativeIdentityV1 expected_directory_identity =
                *receipt.final_witness_.directory_identity;
            if (const auto checked = validate_private_lease_directory_binding(
                    state_->root_fd, attempt_directories[index].get(), directory_leaf,
                    state_->creator_process_id, expected_directory_identity);
                checked.status != DistributedSieveWaveStoreStatus::ready) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = checked;
                return result;
            }

            struct stat fixed_leaf_metadata{};
            if (fstatat_retrying_eintr(
                    attempt_directories[index].get(),
                    carrier::DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1.data(),
                    fixed_leaf_metadata) == 0) {
                set_failure(active_phase, index);
                result.diagnostic.reconciliation_required = true;
                result.diagnostic.wave_store = diagnostic(
                    DistributedSieveWaveStoreStatus::reconciliation_required, protocol_error());
                return result;
            }
            if (errno != ENOENT) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = diagnostic(
                    DistributedSieveWaveStoreStatus::namespace_conflict, posix_error(errno));
                return result;
            }
        }

        // Receipt revalidation follows the complete absence sweep so a
        // directory or record change between those observations also keeps
        // the whole batch in reconciliation-required state.
        active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::final_receipt_revalidation;
        for (std::size_t index = 0; index < slot_count; ++index) {
            active_slot = index;
            const auto checked = receipt_anchors[index]->revalidate();
            if (checked.status != DistributedSieveWaveStoreStatus::ready) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = checked;
                return result;
            }
        }
        batch_absence_unproven = false;
        for (auto& attempt_directory : attempt_directories) {
            attempt_directory.reset();
        }

        active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::work_package_revalidation;
        for (std::size_t index = 0; index < slot_count; ++index) {
            active_slot = index;
            if (!packages[index].has_value()) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = diagnostic(
                    DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
                return result;
            }
            const auto checked = packages[index]->revalidate();
            if (!checked ||
                packages[index]->witness().package.work_sha256 != bound.work->work_digest ||
                packages[index]->witness().creator_process_id != state_->creator_process_id) {
                set_failure(active_phase, index);
                result.diagnostic.carrier = checked;
                if (checked.status == carrier::DistributedSieveWorkerWorkPackageFileStatus::ready) {
                    result.diagnostic.protocol = {
                        .error = DistributedSieveProtocolError::digest_mismatch,
                    };
                }
                return result;
            }
            if (packages[index]->retained_reader_ < 0 ||
                packages[index]->retained_reader_ >
                    static_cast<carrier::DistributedSieveWorkerWorkPackageNativeHandle>(
                        std::numeric_limits<int>::max())) {
                set_failure(active_phase, index);
                result.diagnostic.wave_store = diagnostic(
                    DistributedSieveWaveStoreStatus::invalid_request, invalid_argument_error());
                return result;
            }
            capabilities[index] = {
                .wave_root_directory_descriptor = state_->root_fd,
                .permanent_wave_store_lock_descriptor = state_->lock_fd,
                .attempt_base_lock_descriptor = receipt_anchors[index]->base_lock_at_->lock_fd_,
                .work_package_reader_descriptor =
                    static_cast<int>(packages[index]->retained_reader_),
            };
        }

        active_phase = launcher::DistributedSieveWorkerLaunchPhaseV1::process_spawn;
        active_slot = launcher::DISTRIBUTED_SIEVE_WORKER_LAUNCH_NO_SLOT;
        const process::DistributedSieveWorkerProcessSpawnTestHooks spawn_hooks{
            .before_spawn = consumed.hooks_.before_spawn,
            .context = consumed.hooks_.context,
            .force_fixed_capability_close_all_unavailable =
                consumed.hooks_.force_fixed_capability_close_all_unavailable,
        };
        // Every launcher-owned container and receipt anchor is fixed before
        // this call. The post-spawn path performs only no-throw ownership
        // moves into already-sized result slots.
        auto launched = process::spawn_distributed_sieve_worker_process_batch_with_capabilities(
            std::move(*prepared_processes.batch), capabilities, {}, spawn_hooks);
        result.diagnostic.transport = launched.diagnostic;

        const bool closed_global_pre_spawn_failure =
            !launched.spawn_loop_entered && !launched.child_set_complete &&
            launched.children.empty() &&
            launched.diagnostic.error != process::DistributedSieveWorkerProcessTransportError::none;
        if (closed_global_pre_spawn_failure) {
            set_failure(active_phase, active_slot);
            return result;
        }

        bool complete_child_set =
            launched.child_set_complete && launched.children.size() == slot_count &&
            launched.diagnostic.error == process::DistributedSieveWorkerProcessTransportError::none;
        if (complete_child_set) {
            for (const auto& child : launched.children) {
                const bool has_process = child.process.has_value();
                const bool slot_succeeded =
                    child.diagnostic.error ==
                    process::DistributedSieveWorkerProcessTransportError::none;
                if (has_process != slot_succeeded ||
                    (!launched.spawn_loop_entered && has_process)) {
                    complete_child_set = false;
                    break;
                }
            }
        }
        if (!complete_child_set) {
            result.diagnostic.phase = active_phase;
            result.diagnostic.slot = launcher::DISTRIBUTED_SIEVE_WORKER_LAUNCH_NO_SLOT;
            result.disposition = launcher::DistributedSieveWorkerLaunchDispositionV1::indeterminate;
            if (result.diagnostic.transport.error ==
                process::DistributedSieveWorkerProcessTransportError::none) {
                result.diagnostic.transport = {
                    .error = process::DistributedSieveWorkerProcessTransportError::spawn_failed,
                    .native_error = EPROTO,
                };
            }
            // An incomplete or internally inconsistent child set cannot prove
            // which slots have live children. Quarantine every receipt rather
            // than release any possibly active BaseLock.
            for (auto& receipt_anchor : receipt_anchors) {
                (void)receipt_anchor.release();
            }
            return result;
        }

        std::size_t launched_count = 0;
        for (std::size_t index = 0; index < slot_count; ++index) {
            auto& source = launched.children[index];
            auto& destination = result.children[index];
            destination.transport = source.diagnostic;
            if (!source.process.has_value()) {
                if (result.diagnostic.slot == launcher::DISTRIBUTED_SIEVE_WORKER_LAUNCH_NO_SLOT) {
                    result.diagnostic.slot = index;
                    result.diagnostic.transport = source.diagnostic;
                }
                continue;
            }
            launcher::DistributedSieveLaunchedWorkerAttemptV1 combined(
                std::move(receipt_anchors[index]), std::move(*source.process));
            destination.worker.emplace(std::move(combined));
            ++launched_count;
        }

        if (launched_count == slot_count) {
            result.disposition = launcher::DistributedSieveWorkerLaunchDispositionV1::all;
            result.diagnostic = {};
        } else if (launched_count == 0U) {
            result.disposition =
                launcher::DistributedSieveWorkerLaunchDispositionV1::armed_no_child;
            result.diagnostic.phase = active_phase;
        } else {
            result.disposition = launcher::DistributedSieveWorkerLaunchDispositionV1::partial;
            result.diagnostic.phase = active_phase;
        }
        return result;
#endif
    } catch (const std::bad_alloc&) {
        set_failure(active_phase, active_slot);
        result.diagnostic.wave_store =
            diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                       std::make_error_code(std::errc::not_enough_memory));
        return result;
    } catch (...) {
        set_failure(active_phase, active_slot);
        result.diagnostic.wave_store =
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                       std::make_error_code(std::errc::io_error));
        return result;
    }
}

} // namespace gnfs::sieve::distributed_sieve_resume_detail
