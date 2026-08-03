#include "distributed_sieve_worker_entry_internal.hpp"

#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_process_internal.hpp"

#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/util/process.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#include <sys/param.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#include <unistd.h>
#endif

namespace gnfs::sieve::distributed_sieve_worker_entry_detail {

namespace private_lease = gnfs::relation::ooc_cleanup_detail;
namespace process = gnfs::sieve::distributed_sieve_worker_process_detail;
namespace wave = gnfs::sieve::distributed_sieve_resume_detail;
namespace work_package = gnfs::sieve::distributed_sieve_work_package_codec_detail;

struct DistributedSieveWorkerAttemptChainWitnessV1 final {
    wave::DistributedSieveWorkerAttemptNamesV1 names;
    AttemptStartedV1 record;
    std::vector<std::byte> bytes;
    NativeIdentityV1 identity;
};

struct DistributedSieveWorkerEntryV1::State final {
    int root_descriptor = -1;
    int permanent_lock_descriptor = -1;
    int attempt_lock_descriptor = -1;
    int attempt_directory_descriptor = -1;
    int package_descriptor = -1;
    std::uint64_t creator_process_id = 0;
    mutable std::atomic_bool invalidated = false;
    std::string absolute_root_path;
    std::array<std::uint64_t, 4> base_path_digest{};

    AttemptStartedV1 record;
    WaveManifestV1 manifest;
    DistributedSieveWorkIdentityV1 identity;
    ChunkPlanV1 chunk;
    work_package::DistributedSieveWorkPackageWitnessV1 package_witness;
    wave::DistributedSieveWorkerAttemptNamesV1 names;

    NativeIdentityV1 root_identity;
    NativeIdentityV1 permanent_lock_identity;
    NativeIdentityV1 attempt_lock_identity;
    NativeIdentityV1 attempt_directory_identity;
    NativeIdentityV1 package_identity;
    NativeIdentityV1 manifest_file_identity;
    NativeIdentityV1 attempt_record_identity;
    NativeIdentityV1 reserved_marker_identity;
    NativeIdentityV1 owner_marker_identity;
    NativeIdentityV1 owned_marker_identity;

    std::vector<std::byte> bootstrap_bytes;
    std::vector<std::byte> manifest_bytes;
    std::vector<std::byte> reserved_marker_bytes;
    std::vector<std::byte> owner_marker_bytes;
    std::vector<std::byte> owned_marker_bytes;
    std::vector<DistributedSieveWorkerAttemptChainWitnessV1> attempt_chain;

    ~State() noexcept {
#if !defined(_WIN32)
        const std::array<int, 5> descriptors{
            package_descriptor,      attempt_directory_descriptor,
            attempt_lock_descriptor, permanent_lock_descriptor,
            root_descriptor,
        };
        for (const int descriptor : descriptors) {
            if (descriptor >= 0) {
                // Ownership after EINTR is unspecified; close is never retried.
                (void)::close(descriptor);
            }
        }
#endif
    }
};

namespace {

using Diagnostic = DistributedSieveWorkerEntryDiagnosticV1;
using Phase = DistributedSieveWorkerEntryPhaseV1;
using Status = DistributedSieveWorkerEntryStatusV1;
using WriterDiagnostic = DistributedSieveWorkerWriterDiagnosticV1;
using WriterPhase = DistributedSieveWorkerWriterPhaseV1;
using WriterStatus = DistributedSieveWorkerWriterStatusV1;

std::atomic_flag ADOPTION_STARTED = ATOMIC_FLAG_INIT;

[[nodiscard]] Diagnostic failure(Phase phase, Status status, int native_error = 0,
                                 DistributedSieveProtocolStatus protocol_status = {}) noexcept {
    return {
        .phase = phase,
        .status = status,
        .native_error = native_error,
        .protocol_status = protocol_status,
    };
}

[[nodiscard]] Diagnostic protocol_failure(Phase phase, Status status,
                                          DistributedSieveProtocolStatus protocol) noexcept {
    if (protocol.error == DistributedSieveProtocolError::none) {
        protocol.error = DistributedSieveProtocolError::invalid_value;
    }
    return failure(phase, status, 0, protocol);
}

[[nodiscard]] Diagnostic namespace_failure(Phase phase, Status status) noexcept {
    return protocol_failure(phase, status, {.error = DistributedSieveProtocolError::invalid_value});
}

[[nodiscard]] WriterDiagnostic writer_failure(WriterPhase phase, WriterStatus status,
                                              int native_error = 0) noexcept {
    return {
        .phase = phase,
        .status = status,
        .native_error = native_error,
    };
}

[[nodiscard]] WriterStatus writer_status_for_entry_failure(const Diagnostic& diagnostic) noexcept {
    if (diagnostic.status == Status::platform_unsupported) {
        return WriterStatus::platform_unsupported;
    }
    if (diagnostic.status == Status::process_mismatch) {
        return WriterStatus::process_mismatch;
    }
    if (diagnostic.status == Status::resource_exhausted) {
        return WriterStatus::resource_exhausted;
    }
    if (diagnostic.phase == Phase::attempt_base_lock_validation ||
        diagnostic.status == Status::lock_invalid) {
        return WriterStatus::lock_invalid;
    }
    if (diagnostic.phase == Phase::private_lease_validation ||
        diagnostic.status == Status::private_lease_invalid) {
        return WriterStatus::private_lease_invalid;
    }
    return WriterStatus::entry_invalid;
}

[[nodiscard]] WriterDiagnostic writer_entry_failure(WriterPhase phase,
                                                    const Diagnostic& diagnostic) noexcept {
    return writer_failure(phase, writer_status_for_entry_failure(diagnostic),
                          diagnostic.native_error);
}

[[nodiscard]] std::uint64_t current_process_id() noexcept {
    const int id = gnfs::util::process_id();
    return id > 0 ? static_cast<std::uint64_t>(id) : 0;
}

[[nodiscard]] bool
same_witness(const work_package::DistributedSieveWorkPackageWitnessV1& left,
             const work_package::DistributedSieveWorkPackageWitnessV1& right) noexcept {
    return left.body_bytes == right.body_bytes && left.total_bytes == right.total_bytes &&
           left.work_sha256 == right.work_sha256 && left.package_sha256 == right.package_sha256;
}

#if !defined(_WIN32)

class UniqueFd final {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {}
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
    ~UniqueFd() noexcept {
        reset();
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
    UniqueDirectory(const UniqueDirectory&) = delete;
    UniqueDirectory& operator=(const UniqueDirectory&) = delete;
    ~UniqueDirectory() noexcept {
        if (directory_ != nullptr) {
            (void)::closedir(directory_);
        }
    }

private:
    DIR* directory_ = nullptr;
};

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

[[nodiscard]] int openat_retrying_eintr(int parent, const char* leaf, int flags) noexcept {
    int descriptor = -1;
    do {
        descriptor = ::openat(parent, leaf, flags);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

[[nodiscard]] bool exact_mode(const struct stat& metadata, mode_t expected) noexcept {
    return (metadata.st_mode & static_cast<mode_t>(07777)) == expected;
}

[[nodiscard]] NativeIdentityV1 native_identity(const struct stat& metadata) noexcept {
    return {
        .volume = static_cast<std::uint64_t>(metadata.st_dev),
        .object = static_cast<std::uint64_t>(metadata.st_ino),
        .generation = 0,
    };
}

[[nodiscard]] std::array<std::uint64_t, 3>
relation_identity(const NativeIdentityV1& identity) noexcept {
    return {identity.volume, identity.object, identity.generation};
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

[[nodiscard]] bool resource_error(int native_error) noexcept {
    return native_error == ENOMEM || native_error == EMFILE || native_error == ENFILE ||
           native_error == ENOSPC;
}

enum class AclState : std::uint8_t {
    absent,
    present,
    unsupported,
    failed,
};

struct AclResult final {
    AclState state = AclState::absent;
    int native_error = 0;
};

[[nodiscard]] bool unsupported_acl_error(int error) noexcept {
    return error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS;
}

#if defined(__linux__)
[[nodiscard]] AclResult inspect_linux_acl(int descriptor, const char* name) noexcept {
    ssize_t size = -1;
    do {
        size = ::fgetxattr(descriptor, name, nullptr, 0);
    } while (size < 0 && errno == EINTR);
    if (size >= 0) {
        return {.state = AclState::present};
    }
    const int saved_errno = errno;
    if (saved_errno == ENODATA) {
        return {};
    }
    if (unsupported_acl_error(saved_errno)) {
        return {.state = AclState::unsupported, .native_error = saved_errno};
    }
    return {.state = AclState::failed, .native_error = saved_errno};
}
#endif

[[nodiscard]] AclResult inspect_acl(int descriptor, bool directory) noexcept {
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
            return {.state = AclState::unsupported, .native_error = saved_errno};
        }
        return {.state = AclState::failed, .native_error = saved_errno};
    }
    (void)::acl_free(acl);
    return {.state = AclState::present};
#elif defined(__linux__)
    const auto access = inspect_linux_acl(descriptor, "system.posix_acl_access");
    if (access.state != AclState::absent || !directory) {
        return access;
    }
    return inspect_linux_acl(descriptor, "system.posix_acl_default");
#else
    (void)descriptor;
    (void)directory;
    return {.state = AclState::unsupported, .native_error = ENOTSUP};
#endif
}

[[nodiscard]] std::optional<Diagnostic> reject_acl(int descriptor, bool directory, Phase phase,
                                                   Status status) noexcept {
    const auto acl = inspect_acl(descriptor, directory);
    switch (acl.state) {
    case AclState::absent:
        return std::nullopt;
    case AclState::present:
        return namespace_failure(phase, status);
    case AclState::unsupported:
        return failure(phase, Status::platform_unsupported, acl.native_error);
    case AclState::failed:
        return failure(phase, status, acl.native_error);
    }
    return namespace_failure(phase, status);
}

[[nodiscard]] std::optional<Diagnostic> validate_descriptor_policy(int descriptor,
                                                                   int expected_access_mode,
                                                                   Phase phase,
                                                                   Status status) noexcept {
    int descriptor_flags = -1;
    do {
        descriptor_flags = ::fcntl(descriptor, F_GETFD);
    } while (descriptor_flags < 0 && errno == EINTR);
    if (descriptor_flags < 0) {
        return failure(phase, status, errno);
    }
    int status_flags = -1;
    do {
        status_flags = ::fcntl(descriptor, F_GETFL);
    } while (status_flags < 0 && errno == EINTR);
    if (status_flags < 0) {
        return failure(phase, status, errno);
    }
    if ((descriptor_flags & FD_CLOEXEC) == 0 ||
        (status_flags & O_ACCMODE) != expected_access_mode || (status_flags & O_APPEND) != 0) {
        return namespace_failure(phase, Status::descriptor_policy_invalid);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<Diagnostic> require_missing_at(int parent, std::string_view leaf,
                                                           Phase phase, Status status) {
    struct stat metadata {};
    std::string stable_leaf(leaf);
    if (fstatat_retrying_eintr(parent, stable_leaf.c_str(), metadata) == 0) {
        return namespace_failure(phase, status);
    }
    const int saved_errno = errno;
    if (saved_errno != ENOENT) {
        return failure(phase, status, saved_errno);
    }
    return std::nullopt;
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

[[nodiscard]] bool valid_directory_metadata(const struct stat& metadata) noexcept {
    return S_ISDIR(metadata.st_mode) &&
           static_cast<std::uint64_t>(metadata.st_uid) == static_cast<std::uint64_t>(::geteuid()) &&
           exact_mode(metadata, 0700);
}

[[nodiscard]] bool valid_immutable_metadata(const struct stat& metadata,
                                            std::uint64_t maximum_bytes,
                                            std::optional<std::uint64_t> exact_bytes) noexcept {
    return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 && metadata.st_size > 0 &&
           static_cast<std::uint64_t>(metadata.st_size) <= maximum_bytes &&
           (!exact_bytes.has_value() ||
            static_cast<std::uint64_t>(metadata.st_size) == *exact_bytes) &&
           static_cast<std::uint64_t>(metadata.st_uid) == static_cast<std::uint64_t>(::geteuid()) &&
           exact_mode(metadata, 0600);
}

struct NamedBytes final {
    std::vector<std::byte> bytes;
    NativeIdentityV1 identity;
};

struct NamedBytesResult final {
    std::optional<NamedBytes> value;
    Diagnostic diagnostic;
};

[[nodiscard]] NamedBytesResult read_named_bytes(int parent, std::string_view leaf,
                                                std::uint64_t maximum_bytes,
                                                std::optional<std::uint64_t> exact_bytes,
                                                Phase phase, Status status) noexcept {
    try {
        const std::string stable_leaf(leaf);
        const int descriptor = openat_retrying_eintr(
            parent, stable_leaf.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor < 0) {
            return {std::nullopt, failure(phase, status, errno)};
        }
        UniqueFd held(descriptor);
        if (const auto policy = validate_descriptor_policy(held.get(), O_RDONLY, phase, status);
            policy.has_value()) {
            return {std::nullopt, *policy};
        }

        struct stat held_before {};
        struct stat named_before {};
        if (fstat_retrying_eintr(held.get(), held_before) != 0) {
            return {std::nullopt, failure(phase, status, errno)};
        }
        if (fstatat_retrying_eintr(parent, stable_leaf.c_str(), named_before) != 0) {
            return {std::nullopt, failure(phase, status, errno)};
        }
        if (!valid_immutable_metadata(held_before, maximum_bytes, exact_bytes) ||
            !valid_immutable_metadata(named_before, maximum_bytes, exact_bytes) ||
            !stable_metadata(held_before, named_before)) {
            return {std::nullopt, namespace_failure(phase, status)};
        }
        if (const auto acl = reject_acl(held.get(), false, phase, status); acl.has_value()) {
            return {std::nullopt, *acl};
        }

        std::vector<std::byte> bytes(static_cast<std::size_t>(held_before.st_size));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t request =
                std::min(bytes.size() - offset,
                         static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t count =
                ::pread(held.get(), bytes.data() + offset, request, static_cast<off_t>(offset));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return {std::nullopt, failure(phase, status, errno)};
            }
            if (count == 0) {
                return {std::nullopt, namespace_failure(phase, status)};
            }
            offset += static_cast<std::size_t>(count);
        }
        std::byte trailing{};
        ssize_t trailing_count = -1;
        do {
            trailing_count = ::pread(held.get(), &trailing, 1, static_cast<off_t>(bytes.size()));
        } while (trailing_count < 0 && errno == EINTR);
        if (trailing_count < 0) {
            return {std::nullopt, failure(phase, status, errno)};
        }
        if (trailing_count != 0) {
            return {std::nullopt, namespace_failure(phase, status)};
        }

        struct stat held_after {};
        struct stat named_after {};
        if (fstat_retrying_eintr(held.get(), held_after) != 0 ||
            fstatat_retrying_eintr(parent, stable_leaf.c_str(), named_after) != 0) {
            return {std::nullopt, failure(phase, status, errno)};
        }
        if (!valid_immutable_metadata(held_after, maximum_bytes, exact_bytes) ||
            !valid_immutable_metadata(named_after, maximum_bytes, exact_bytes) ||
            !stable_metadata(held_before, held_after) ||
            !stable_metadata(held_after, named_after)) {
            return {std::nullopt, namespace_failure(phase, status)};
        }
        if (const auto acl = reject_acl(held.get(), false, phase, status); acl.has_value()) {
            return {std::nullopt, *acl};
        }
        return {
            NamedBytes{.bytes = std::move(bytes), .identity = native_identity(held_after)},
            {},
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(phase, Status::resource_exhausted, ENOMEM)};
    } catch (...) {
        return {std::nullopt, failure(phase, Status::unexpected_failure)};
    }
}

[[nodiscard]] Diagnostic validate_root(int descriptor, const NativeIdentityV1& expected) noexcept {
    constexpr Phase phase = Phase::root_validation;
    if (const auto policy =
            validate_descriptor_policy(descriptor, O_RDONLY, phase, Status::namespace_invalid);
        policy.has_value()) {
        return *policy;
    }
    struct stat before {};
    struct stat after {};
    if (fstat_retrying_eintr(descriptor, before) != 0) {
        return failure(phase, Status::namespace_invalid, errno);
    }
    if (!valid_root_metadata(before)) {
        return namespace_failure(phase, Status::namespace_invalid);
    }
    if (const auto acl = reject_acl(descriptor, true, phase, Status::namespace_invalid);
        acl.has_value()) {
        return *acl;
    }
    if (fstat_retrying_eintr(descriptor, after) != 0) {
        return failure(phase, Status::namespace_invalid, errno);
    }
    if (!valid_root_metadata(after) || !stable_metadata(before, after) ||
        native_identity(after) != expected) {
        return namespace_failure(phase, Status::namespace_invalid);
    }
    return {};
}

struct RecoveredRootPathResult final {
    std::optional<std::string> path;
    Diagnostic diagnostic;
};

[[nodiscard]] RecoveredRootPathResult recover_root_path(int descriptor) {
    constexpr Phase phase = Phase::root_validation;
#if defined(__APPLE__)
    std::array<char, MAXPATHLEN> buffer{};
    int result = -1;
    do {
        result = ::fcntl(descriptor, F_GETPATH, buffer.data());
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return {
            std::nullopt,
            failure(phase, Status::platform_unsupported, errno),
        };
    }
    const auto terminator = std::find(buffer.begin(), buffer.end(), '\0');
    if (terminator == buffer.begin() || terminator == buffer.end()) {
        return {
            std::nullopt,
            namespace_failure(phase, Status::namespace_invalid),
        };
    }
    return {std::string(buffer.begin(), terminator), {}};
#elif defined(__linux__)
    const std::string adapter_leaf = "/proc/self/fd/" + std::to_string(descriptor);
    constexpr std::size_t maximum_path_bytes = 1024U * 1024U;
    std::vector<char> buffer(4096U);
    for (;;) {
        ssize_t count = -1;
        do {
            count = ::readlink(adapter_leaf.c_str(), buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return {
                std::nullopt,
                failure(phase, Status::platform_unsupported, errno),
            };
        }
        const auto path_bytes = static_cast<std::size_t>(count);
        if (path_bytes < buffer.size()) {
            return {std::string(buffer.data(), path_bytes), {}};
        }
        if (buffer.size() >= maximum_path_bytes) {
            return {
                std::nullopt,
                namespace_failure(phase, Status::namespace_invalid),
            };
        }
        buffer.resize(std::min(buffer.size() * 2U, maximum_path_bytes));
    }
#else
    (void)descriptor;
    return {
        std::nullopt,
        failure(phase, Status::platform_unsupported, ENOTSUP),
    };
#endif
}

[[nodiscard]] std::optional<std::vector<std::string>>
strict_absolute_path_components(std::string_view absolute_path) {
    if (absolute_path.size() <= 1U || absolute_path.front() != '/' || absolute_path.back() == '/' ||
        absolute_path.find('\0') != std::string_view::npos ||
        absolute_path.ends_with(" (deleted)")) {
        return std::nullopt;
    }

    std::vector<std::string> components;
    std::size_t begin = 1U;
    while (begin < absolute_path.size()) {
        const std::size_t end = absolute_path.find('/', begin);
        const std::size_t component_end =
            end == std::string_view::npos ? absolute_path.size() : end;
        if (component_end == begin) {
            return std::nullopt;
        }
        std::string component(absolute_path.substr(begin, component_end - begin));
        if (component == "." || component == "..") {
            return std::nullopt;
        }
        components.push_back(std::move(component));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return components.empty() ? std::nullopt
                              : std::optional<std::vector<std::string>>(std::move(components));
}

[[nodiscard]] Diagnostic validate_absolute_root_binding(int descriptor,
                                                        std::string_view expected_absolute_path,
                                                        const NativeIdentityV1& expected_identity) {
    constexpr Phase phase = Phase::root_validation;
    const auto recovered_before = recover_root_path(descriptor);
    if (!recovered_before.path.has_value()) {
        return recovered_before.diagnostic;
    }
    if (*recovered_before.path != expected_absolute_path) {
        return namespace_failure(phase, Status::namespace_invalid);
    }
    auto components = strict_absolute_path_components(*recovered_before.path);
    if (!components.has_value()) {
        return namespace_failure(phase, Status::namespace_invalid);
    }

    const int direct_parent_descriptor =
        openat_retrying_eintr(AT_FDCWD, "/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (direct_parent_descriptor < 0) {
        return failure(phase, Status::namespace_invalid, errno);
    }
    UniqueFd direct_parent(direct_parent_descriptor);
    for (std::size_t index = 0; index + 1U < components->size(); ++index) {
        const int next = openat_retrying_eintr(direct_parent.get(), (*components)[index].c_str(),
                                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            return failure(phase, Status::namespace_invalid, errno);
        }
        direct_parent.reset(next);
    }

    struct stat parent_held_before {};
    if (fstat_retrying_eintr(direct_parent.get(), parent_held_before) != 0) {
        return failure(phase, Status::namespace_invalid, errno);
    }
    if (const auto acl = reject_acl(direct_parent.get(), true, phase, Status::namespace_invalid);
        acl.has_value()) {
        return *acl;
    }

    const int named_parent_descriptor =
        openat_retrying_eintr(AT_FDCWD, "/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (named_parent_descriptor < 0) {
        return failure(phase, Status::namespace_invalid, errno);
    }
    UniqueFd named_parent(named_parent_descriptor);
    for (std::size_t index = 0; index + 1U < components->size(); ++index) {
        const int next = openat_retrying_eintr(named_parent.get(), (*components)[index].c_str(),
                                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            return failure(phase, Status::namespace_invalid, errno);
        }
        named_parent.reset(next);
    }

    struct stat parent_named {};
    struct stat parent_held_after {};
    if (fstat_retrying_eintr(named_parent.get(), parent_named) != 0 ||
        fstat_retrying_eintr(direct_parent.get(), parent_held_after) != 0) {
        return failure(phase, Status::namespace_invalid, errno);
    }
    if (const auto acl = reject_acl(named_parent.get(), true, phase, Status::namespace_invalid);
        acl.has_value()) {
        return *acl;
    }
    if (const auto acl = reject_acl(direct_parent.get(), true, phase, Status::namespace_invalid);
        acl.has_value()) {
        return *acl;
    }
    if (!valid_parent_metadata(parent_held_before) || !valid_parent_metadata(parent_named) ||
        !valid_parent_metadata(parent_held_after) ||
        !stable_metadata(parent_held_before, parent_held_after) ||
        !stable_metadata(parent_held_after, parent_named)) {
        return namespace_failure(phase, Status::namespace_invalid);
    }

    const std::string& root_leaf = components->back();
    const int reopened_root_descriptor = openat_retrying_eintr(
        direct_parent.get(), root_leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (reopened_root_descriptor < 0) {
        return failure(phase, Status::namespace_invalid, errno);
    }
    UniqueFd reopened_root(reopened_root_descriptor);

    struct stat retained_before {};
    if (fstat_retrying_eintr(descriptor, retained_before) != 0) {
        return failure(phase, Status::namespace_invalid, errno);
    }
    if (const auto acl = reject_acl(descriptor, true, phase, Status::namespace_invalid);
        acl.has_value()) {
        return *acl;
    }

    struct stat root_named {};
    struct stat root_reopened {};
    struct stat retained_after {};
    if (fstatat_retrying_eintr(direct_parent.get(), root_leaf.c_str(), root_named) != 0 ||
        fstat_retrying_eintr(reopened_root.get(), root_reopened) != 0 ||
        fstat_retrying_eintr(descriptor, retained_after) != 0) {
        return failure(phase, Status::namespace_invalid, errno);
    }
    if (const auto acl = reject_acl(reopened_root.get(), true, phase, Status::namespace_invalid);
        acl.has_value()) {
        return *acl;
    }
    if (const auto acl = reject_acl(descriptor, true, phase, Status::namespace_invalid);
        acl.has_value()) {
        return *acl;
    }
    if (!valid_root_metadata(retained_before) || !valid_root_metadata(root_named) ||
        !valid_root_metadata(root_reopened) || !valid_root_metadata(retained_after) ||
        !stable_metadata(retained_before, retained_after) ||
        !stable_metadata(retained_after, root_named) ||
        !stable_metadata(root_named, root_reopened) ||
        native_identity(root_named) != expected_identity ||
        native_identity(root_reopened) != expected_identity ||
        native_identity(retained_after) != expected_identity) {
        return namespace_failure(phase, Status::namespace_invalid);
    }

    const auto recovered_after = recover_root_path(descriptor);
    if (!recovered_after.path.has_value()) {
        return recovered_after.diagnostic;
    }
    if (*recovered_after.path != expected_absolute_path) {
        return namespace_failure(phase, Status::namespace_invalid);
    }
    return {};
}

[[nodiscard]] std::array<std::uint64_t, 4>
expected_base_path_digest(std::string_view absolute_root_path,
                          const wave::DistributedSieveWorkerAttemptNamesV1& names) {
    std::filesystem::path base_path(absolute_root_path);
    base_path /= names.private_directory_leaf;
    base_path /= "corpus";
    return private_lease::frozen_path_digest(base_path);
}

[[nodiscard]] Diagnostic validate_named_lock(int root_descriptor, int lock_descriptor,
                                             std::string_view leaf,
                                             const NativeIdentityV1& expected, Phase phase) {
    if (const auto policy =
            validate_descriptor_policy(lock_descriptor, O_RDWR, phase, Status::lock_invalid);
        policy.has_value()) {
        return *policy;
    }
    struct stat held_before {};
    struct stat named {};
    struct stat held_after {};
    std::string stable_leaf(leaf);
    if (fstat_retrying_eintr(lock_descriptor, held_before) != 0 ||
        fstatat_retrying_eintr(root_descriptor, stable_leaf.c_str(), named) != 0 ||
        fstat_retrying_eintr(lock_descriptor, held_after) != 0) {
        return failure(phase, Status::lock_invalid, errno);
    }
    if (!valid_lock_metadata(held_before) || !valid_lock_metadata(named) ||
        !valid_lock_metadata(held_after) || !stable_metadata(held_before, named) ||
        !stable_metadata(held_before, held_after) || native_identity(held_after) != expected) {
        return namespace_failure(phase, Status::lock_invalid);
    }
    if (const auto acl = reject_acl(lock_descriptor, false, phase, Status::lock_invalid);
        acl.has_value()) {
        return *acl;
    }

    // A successful flock() on the retained descriptor alone is not proof of
    // inheritance: it would also acquire a previously unlocked descriptor.
    // First require an independently opened description to observe contention,
    // then require the retained description itself to succeed. Together these
    // reject both a missing lock and a reopened same-inode descriptor.
    const int contender_descriptor = openat_retrying_eintr(root_descriptor, stable_leaf.c_str(),
                                                           O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (contender_descriptor < 0) {
        return failure(phase, Status::lock_invalid, errno);
    }
    UniqueFd contender(contender_descriptor);
    struct stat contender_metadata {};
    if (fstat_retrying_eintr(contender.get(), contender_metadata) != 0) {
        return failure(phase, Status::lock_invalid, errno);
    }
    if (!valid_lock_metadata(contender_metadata) ||
        native_identity(contender_metadata) != expected) {
        return namespace_failure(phase, Status::lock_invalid);
    }
    int contender_locked = -1;
    do {
        contender_locked = ::flock(contender.get(), LOCK_EX | LOCK_NB);
    } while (contender_locked != 0 && errno == EINTR);
    if (contender_locked == 0) {
        return namespace_failure(phase, Status::lock_invalid);
    }
    const int contender_error = errno;
    if (contender_error != EWOULDBLOCK && contender_error != EAGAIN) {
        return failure(phase, Status::lock_invalid, contender_error);
    }

    int locked = -1;
    do {
        locked = ::flock(lock_descriptor, LOCK_EX | LOCK_NB);
    } while (locked != 0 && errno == EINTR);
    if (locked != 0) {
        return failure(phase, Status::lock_invalid, errno);
    }
    return {};
}

[[nodiscard]] Diagnostic validate_attempt_directory(int root_descriptor, int directory_descriptor,
                                                    std::string_view leaf,
                                                    const NativeIdentityV1& expected) {
    constexpr Phase phase = Phase::private_lease_validation;
    if (const auto policy = validate_descriptor_policy(directory_descriptor, O_RDONLY, phase,
                                                       Status::private_lease_invalid);
        policy.has_value()) {
        return *policy;
    }
    struct stat held_before {};
    struct stat named {};
    struct stat held_after {};
    std::string stable_leaf(leaf);
    if (fstat_retrying_eintr(directory_descriptor, held_before) != 0 ||
        fstatat_retrying_eintr(root_descriptor, stable_leaf.c_str(), named) != 0 ||
        fstat_retrying_eintr(directory_descriptor, held_after) != 0) {
        return failure(phase, Status::private_lease_invalid, errno);
    }
    if (!valid_directory_metadata(held_before) || !valid_directory_metadata(named) ||
        !valid_directory_metadata(held_after) || !stable_metadata(held_before, named) ||
        !stable_metadata(held_before, held_after) || native_identity(held_after) != expected) {
        return namespace_failure(phase, Status::private_lease_invalid);
    }
    if (const auto acl =
            reject_acl(directory_descriptor, true, phase, Status::private_lease_invalid);
        acl.has_value()) {
        return *acl;
    }
    return {};
}

[[nodiscard]] Diagnostic require_owner_only_directory(int descriptor) noexcept {
    constexpr Phase phase = Phase::private_lease_validation;
    const int scan_descriptor =
        openat_retrying_eintr(descriptor, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (scan_descriptor < 0) {
        return failure(phase, Status::private_lease_invalid, errno);
    }
    DIR* raw_directory = ::fdopendir(scan_descriptor);
    if (raw_directory == nullptr) {
        const int saved_errno = errno;
        (void)::close(scan_descriptor);
        return failure(phase, Status::private_lease_invalid, saved_errno);
    }
    UniqueDirectory directory(raw_directory);
    bool owner_seen = false;
    errno = 0;
    for (;;) {
        dirent* entry = ::readdir(raw_directory);
        if (entry == nullptr) {
            if (errno != 0) {
                return failure(phase, Status::private_lease_invalid, errno);
            }
            break;
        }
        const std::string_view leaf(entry->d_name);
        if (leaf == "." || leaf == "..") {
            continue;
        }
        if (leaf == wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF && !owner_seen) {
            owner_seen = true;
            continue;
        }
        return namespace_failure(phase, Status::private_lease_invalid);
    }
    return owner_seen ? Diagnostic{} : namespace_failure(phase, Status::private_lease_invalid);
}

[[nodiscard]] Diagnostic require_writer_directory_contents(int descriptor) noexcept {
    constexpr Phase phase = Phase::private_lease_validation;
    const int scan_descriptor =
        openat_retrying_eintr(descriptor, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (scan_descriptor < 0) {
        return failure(phase, Status::private_lease_invalid, errno);
    }
    DIR* raw_directory = ::fdopendir(scan_descriptor);
    if (raw_directory == nullptr) {
        const int saved_errno = errno;
        (void)::close(scan_descriptor);
        return failure(phase, Status::private_lease_invalid, saved_errno);
    }
    UniqueDirectory directory(raw_directory);
    bool owner_seen = false;
    bool index_seen = false;
    bool data_seen = false;
    errno = 0;
    for (;;) {
        dirent* entry = ::readdir(raw_directory);
        if (entry == nullptr) {
            if (errno != 0) {
                return failure(phase, Status::private_lease_invalid, errno);
            }
            break;
        }
        const std::string_view leaf(entry->d_name);
        if (leaf == "." || leaf == "..") {
            continue;
        }
        bool* seen = nullptr;
        if (leaf == wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF) {
            seen = &owner_seen;
        } else if (leaf == "corpus.relidx") {
            seen = &index_seen;
        } else if (leaf == "corpus.reldata") {
            seen = &data_seen;
        } else {
            return namespace_failure(phase, Status::private_lease_invalid);
        }
        if (*seen) {
            return namespace_failure(phase, Status::private_lease_invalid);
        }
        *seen = true;
    }
    return owner_seen ? Diagnostic{} : namespace_failure(phase, Status::private_lease_invalid);
}

struct ParsedMarker final {
    std::vector<std::byte> bytes;
    NativeIdentityV1 identity;
    private_lease::PrivateLeaseRecord record;
};

struct ParsedMarkerResult final {
    std::optional<ParsedMarker> marker;
    Diagnostic diagnostic;
};

[[nodiscard]] ParsedMarkerResult read_marker(int parent, std::string_view leaf) noexcept {
    constexpr Phase phase = Phase::private_lease_validation;
    auto loaded = read_named_bytes(parent, leaf, private_lease::PRIVATE_LEASE_MARKER_BYTES,
                                   private_lease::PRIVATE_LEASE_MARKER_BYTES, phase,
                                   Status::private_lease_invalid);
    if (!loaded.value.has_value()) {
        return {std::nullopt, loaded.diagnostic};
    }
    try {
        auto record = private_lease::parse_private_lease_marker(loaded.value->bytes);
        return {
            ParsedMarker{
                .bytes = std::move(loaded.value->bytes),
                .identity = loaded.value->identity,
                .record = std::move(record),
            },
            {},
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(phase, Status::resource_exhausted, ENOMEM)};
    } catch (...) {
        return {std::nullopt, namespace_failure(phase, Status::private_lease_invalid)};
    }
}

[[nodiscard]] Diagnostic require_no_attempt_staging_candidates(
    int root_descriptor, const wave::DistributedSieveWorkerAttemptNamesV1& names) noexcept {
    constexpr Phase phase = Phase::private_lease_validation;
    const int scan_descriptor = openat_retrying_eintr(
        root_descriptor, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (scan_descriptor < 0) {
        return failure(phase, Status::private_lease_invalid, errno);
    }
    DIR* raw_directory = ::fdopendir(scan_descriptor);
    if (raw_directory == nullptr) {
        const int saved_errno = errno;
        (void)::close(scan_descriptor);
        return failure(phase, Status::private_lease_invalid, saved_errno);
    }
    UniqueDirectory directory(raw_directory);
    errno = 0;
    for (;;) {
        dirent* entry = ::readdir(raw_directory);
        if (entry == nullptr) {
            if (errno != 0) {
                return failure(phase, Status::private_lease_invalid, errno);
            }
            break;
        }
        const std::string_view leaf(entry->d_name);
        const std::size_t tag_offset = names.relative_lease_stem.size();
        const bool staging_candidate =
            leaf.size() >= tag_offset + wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG.size() &&
            leaf.substr(0, tag_offset) == names.relative_lease_stem &&
            leaf.substr(tag_offset, wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG.size()) ==
                wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_STAGING_TAG;
        if (staging_candidate) {
            return namespace_failure(phase, Status::private_lease_invalid);
        }
    }
    return {};
}

struct AttemptChainReadResult final {
    std::optional<std::vector<DistributedSieveWorkerAttemptChainWitnessV1>> chain;
    Diagnostic diagnostic;
};

[[nodiscard]] AttemptChainReadResult
read_attempt_chain(int root_descriptor, const WaveManifestV1& manifest, const ChunkPlanV1& chunk,
                   const AttemptStartedV1& bootstrap_record,
                   std::span<const std::byte> bootstrap_bytes) {
    constexpr Phase phase = Phase::attempt_validation;
    if (bootstrap_record.chunk_id != chunk.chunk_id ||
        bootstrap_record.attempt_ordinal >= manifest.max_worker_attempts) {
        return {
            std::nullopt,
            namespace_failure(phase, Status::namespace_invalid),
        };
    }

    std::vector<DistributedSieveWorkerAttemptChainWitnessV1> witnesses;
    witnesses.reserve(static_cast<std::size_t>(bootstrap_record.attempt_ordinal) + 1U);
    for (std::uint32_t ordinal = 0; ordinal <= bootstrap_record.attempt_ordinal; ++ordinal) {
        auto names = wave::distributed_sieve_worker_attempt_names_v1(chunk.relative_artifact_stem,
                                                                     chunk.chunk_id, ordinal);
        if (!names.has_value()) {
            return {
                std::nullopt,
                namespace_failure(phase, Status::namespace_invalid),
            };
        }
        auto loaded = read_named_bytes(root_descriptor, names->canonical_record_leaf,
                                       DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES, std::nullopt,
                                       phase, Status::namespace_invalid);
        if (!loaded.value.has_value()) {
            return {std::nullopt, loaded.diagnostic};
        }
        if (const auto pending = require_missing_at(root_descriptor, names->pending_record_leaf,
                                                    phase, Status::namespace_invalid);
            pending.has_value()) {
            return {std::nullopt, *pending};
        }

        auto decoded = decode_distributed_sieve_record(loaded.value->bytes);
        if (!decoded) {
            return {
                std::nullopt,
                protocol_failure(phase, Status::protocol_invalid, decoded.status),
            };
        }
        auto* attempt = std::get_if<AttemptStartedV1>(&*decoded.value);
        if (attempt == nullptr) {
            return {
                std::nullopt,
                protocol_failure(phase, Status::protocol_invalid,
                                 {.error = DistributedSieveProtocolError::record_type_mismatch}),
            };
        }
        witnesses.push_back({
            .names = std::move(*names),
            .record = std::move(*attempt),
            .bytes = std::move(loaded.value->bytes),
            .identity = loaded.value->identity,
        });
    }

    const auto& tail = witnesses.back();
    if (tail.bytes.size() != bootstrap_bytes.size() ||
        !std::equal(tail.bytes.begin(), tail.bytes.end(), bootstrap_bytes.begin()) ||
        tail.record.self_digest != bootstrap_record.self_digest) {
        return {
            std::nullopt,
            namespace_failure(phase, Status::namespace_invalid),
        };
    }

    std::vector<AttemptStartedV1> records;
    records.reserve(witnesses.size());
    for (const auto& witness : witnesses) {
        records.push_back(witness.record);
    }
    const auto chain_status =
        validate_worker_attempt_chain(manifest, chunk.chunk_id, records, nullptr, nullptr);
    if (!chain_status) {
        return {
            std::nullopt,
            protocol_failure(phase, Status::protocol_invalid, chain_status),
        };
    }

    for (std::uint32_t ordinal = bootstrap_record.attempt_ordinal + 1U;
         ordinal < DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS; ++ordinal) {
        const auto names = wave::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, ordinal);
        if (!names.has_value()) {
            return {
                std::nullopt,
                namespace_failure(phase, Status::namespace_invalid),
            };
        }
        if (const auto canonical = require_missing_at(root_descriptor, names->canonical_record_leaf,
                                                      phase, Status::namespace_invalid);
            canonical.has_value()) {
            return {std::nullopt, *canonical};
        }
        if (const auto pending = require_missing_at(root_descriptor, names->pending_record_leaf,
                                                    phase, Status::namespace_invalid);
            pending.has_value()) {
            return {std::nullopt, *pending};
        }
    }
    return {std::move(witnesses), {}};
}

struct PackageReadResult final {
    std::optional<work_package::DistributedSieveDecodedWorkPackageV1> package;
    NativeIdentityV1 identity;
    Diagnostic diagnostic;
};

[[nodiscard]] PackageReadResult read_package(int descriptor) noexcept {
    constexpr Phase phase = Phase::work_package_validation;
    if (const auto policy =
            validate_descriptor_policy(descriptor, O_RDONLY, phase, Status::work_package_invalid);
        policy.has_value()) {
        return {std::nullopt, {}, *policy};
    }
    struct stat before {};
    if (fstat_retrying_eintr(descriptor, before) != 0) {
        return {std::nullopt, {}, failure(phase, Status::work_package_invalid, errno)};
    }
    const bool valid_metadata =
        S_ISREG(before.st_mode) && before.st_nlink == 0 && before.st_size > 0 &&
        static_cast<std::uint64_t>(before.st_size) <=
            work_package::DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1 &&
        static_cast<std::uint64_t>(before.st_uid) == static_cast<std::uint64_t>(::geteuid()) &&
        exact_mode(before, 0400);
    if (!valid_metadata) {
        return {std::nullopt, {}, namespace_failure(phase, Status::work_package_invalid)};
    }
    if (const auto acl = reject_acl(descriptor, false, phase, Status::work_package_invalid);
        acl.has_value()) {
        return {std::nullopt, {}, *acl};
    }

    try {
        std::vector<std::byte> bytes(static_cast<std::size_t>(before.st_size));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t request =
                std::min(bytes.size() - offset,
                         static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t count =
                ::pread(descriptor, bytes.data() + offset, request, static_cast<off_t>(offset));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return {std::nullopt, {}, failure(phase, Status::work_package_invalid, errno)};
            }
            if (count == 0) {
                return {std::nullopt, {}, namespace_failure(phase, Status::work_package_invalid)};
            }
            offset += static_cast<std::size_t>(count);
        }
        std::byte trailing{};
        ssize_t trailing_count = -1;
        do {
            trailing_count = ::pread(descriptor, &trailing, 1, static_cast<off_t>(bytes.size()));
        } while (trailing_count < 0 && errno == EINTR);
        if (trailing_count < 0) {
            return {std::nullopt, {}, failure(phase, Status::work_package_invalid, errno)};
        }
        if (trailing_count != 0) {
            return {std::nullopt, {}, namespace_failure(phase, Status::work_package_invalid)};
        }

        struct stat after {};
        if (fstat_retrying_eintr(descriptor, after) != 0) {
            return {std::nullopt, {}, failure(phase, Status::work_package_invalid, errno)};
        }
        if (!stable_metadata(before, after) || after.st_nlink != 0 || !exact_mode(after, 0400)) {
            return {std::nullopt, {}, namespace_failure(phase, Status::work_package_invalid)};
        }
        auto decoded = work_package::decode_distributed_sieve_work_package_v1(bytes);
        if (!decoded) {
            return {
                std::nullopt,
                {},
                protocol_failure(phase, Status::work_package_invalid, decoded.status),
            };
        }
        if (decoded.package->witness.total_bytes != static_cast<std::uint64_t>(after.st_size)) {
            return {std::nullopt, {}, namespace_failure(phase, Status::work_package_invalid)};
        }
        return {std::move(decoded.package), native_identity(after), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, {}, failure(phase, Status::resource_exhausted, ENOMEM)};
    } catch (...) {
        return {std::nullopt, {}, failure(phase, Status::unexpected_failure)};
    }
}

struct CapturedCapabilities final {
    UniqueFd root;
    UniqueFd permanent_lock;
    UniqueFd attempt_lock;
    UniqueFd package;
};

struct CaptureResult final {
    std::optional<CapturedCapabilities> capabilities;
    Diagnostic diagnostic;
};

[[nodiscard]] CaptureResult capture_fixed_capabilities() noexcept {
    constexpr Phase phase = Phase::fixed_capability_capture;
    constexpr std::array<int, 4> fixed{
        process::DISTRIBUTED_SIEVE_WORKER_CHILD_WAVE_ROOT_DESCRIPTOR,
        process::DISTRIBUTED_SIEVE_WORKER_CHILD_PERMANENT_WAVE_STORE_LOCK_DESCRIPTOR,
        process::DISTRIBUTED_SIEVE_WORKER_CHILD_ATTEMPT_BASE_LOCK_DESCRIPTOR,
        process::DISTRIBUTED_SIEVE_WORKER_CHILD_WORK_PACKAGE_READER_DESCRIPTOR,
    };
    std::array<UniqueFd, 4> duplicated;
    int first_duplicate_error = 0;
    for (std::size_t index = 0; index < fixed.size(); ++index) {
        int descriptor = -1;
        do {
            descriptor = ::fcntl(fixed[index], F_DUPFD_CLOEXEC,
                                 process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
        } while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0) {
            if (first_duplicate_error == 0) {
                first_duplicate_error = errno;
            }
            continue;
        }
        duplicated[index].reset(descriptor);
    }

    int first_close_error = 0;
    for (const int descriptor : fixed) {
        if (::close(descriptor) != 0 && first_close_error == 0) {
            first_close_error = errno;
        }
    }
    if (first_duplicate_error != 0) {
        return {
            std::nullopt,
            failure(phase,
                    resource_error(first_duplicate_error) ? Status::resource_exhausted
                                                          : Status::descriptor_unavailable,
                    first_duplicate_error),
        };
    }
    if (first_close_error != 0) {
        return {std::nullopt, failure(phase, Status::descriptor_policy_invalid, first_close_error)};
    }
    constexpr std::array<int, 4> expected_access_modes{
        O_RDONLY,
        O_RDWR,
        O_RDWR,
        O_RDONLY,
    };
    for (std::size_t index = 0; index < duplicated.size(); ++index) {
        if (const auto policy =
                validate_descriptor_policy(duplicated[index].get(), expected_access_modes[index],
                                           phase, Status::descriptor_policy_invalid);
            policy.has_value()) {
            return {std::nullopt, *policy};
        }
    }
    return {
        CapturedCapabilities{
            .root = std::move(duplicated[0]),
            .permanent_lock = std::move(duplicated[1]),
            .attempt_lock = std::move(duplicated[2]),
            .package = std::move(duplicated[3]),
        },
        {},
    };
}

struct BootstrapReadResult final {
    std::optional<std::vector<std::byte>> bytes;
    Diagnostic diagnostic;
};

[[nodiscard]] BootstrapReadResult read_bootstrap() noexcept {
    constexpr Phase phase = Phase::bootstrap_read;
    std::vector<std::byte> bytes;
    try {
        bytes.reserve(process::DISTRIBUTED_SIEVE_WORKER_BOOTSTRAP_FRAME_LIMIT);
        std::array<std::byte, 512> buffer{};
        for (;;) {
            ssize_t count = -1;
            do {
                count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            } while (count < 0 && errno == EINTR);
            if (count < 0) {
                const int saved_errno = errno;
                (void)::close(STDIN_FILENO);
                return {std::nullopt, failure(phase, Status::input_failed, saved_errno)};
            }
            if (count == 0) {
                break;
            }
            const std::size_t read_count = static_cast<std::size_t>(count);
            if (bytes.size() >
                process::DISTRIBUTED_SIEVE_WORKER_BOOTSTRAP_FRAME_LIMIT - read_count) {
                (void)::close(STDIN_FILENO);
                return {
                    std::nullopt,
                    protocol_failure(phase, Status::protocol_invalid,
                                     {.error = DistributedSieveProtocolError::input_too_large}),
                };
            }
            bytes.insert(bytes.end(), buffer.begin(),
                         buffer.begin() + static_cast<std::ptrdiff_t>(read_count));
        }
        if (::close(STDIN_FILENO) != 0) {
            return {std::nullopt, failure(phase, Status::input_failed, errno)};
        }
        if (bytes.empty()) {
            return {
                std::nullopt,
                protocol_failure(phase, Status::protocol_invalid,
                                 {.error = DistributedSieveProtocolError::truncated}),
            };
        }
        return {std::move(bytes), {}};
    } catch (const std::bad_alloc&) {
        (void)::close(STDIN_FILENO);
        return {std::nullopt, failure(phase, Status::resource_exhausted, ENOMEM)};
    } catch (...) {
        (void)::close(STDIN_FILENO);
        return {std::nullopt, failure(phase, Status::unexpected_failure)};
    }
}

template <typename State> [[nodiscard]] Diagnostic validate_state(const State& state) {
    if (state.creator_process_id == 0 || current_process_id() != state.creator_process_id) {
        return failure(Phase::final_revalidation, Status::process_mismatch, EINVAL);
    }
    if (state.invalidated.load(std::memory_order_acquire)) {
        return namespace_failure(Phase::final_revalidation, Status::namespace_invalid);
    }
    if (const auto root = validate_root(state.root_descriptor, state.root_identity); !root) {
        return root;
    }
    if (const auto binding = validate_absolute_root_binding(
            state.root_descriptor, state.absolute_root_path, state.root_identity);
        !binding) {
        return binding;
    }
    const auto base_path_digest = expected_base_path_digest(state.absolute_root_path, state.names);
    if (base_path_digest != state.base_path_digest) {
        return namespace_failure(Phase::private_lease_validation, Status::private_lease_invalid);
    }
    if (const auto locked =
            validate_named_lock(state.root_descriptor, state.permanent_lock_descriptor,
                                wave::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF,
                                state.permanent_lock_identity, Phase::permanent_lock_validation);
        !locked) {
        return locked;
    }

    auto manifest =
        read_named_bytes(state.root_descriptor, wave::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF,
                         DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES, std::nullopt,
                         Phase::manifest_validation, Status::namespace_invalid);
    if (!manifest.value.has_value()) {
        return manifest.diagnostic;
    }
    if (manifest.value->bytes != state.manifest_bytes ||
        manifest.value->identity != state.manifest_file_identity) {
        return namespace_failure(Phase::manifest_validation, Status::namespace_invalid);
    }
    if (const auto missing = require_missing_at(
            state.root_descriptor, wave::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF,
            Phase::manifest_validation, Status::namespace_invalid);
        missing.has_value()) {
        return *missing;
    }

    auto attempt_chain = read_attempt_chain(state.root_descriptor, state.manifest, state.chunk,
                                            state.record, state.bootstrap_bytes);
    if (!attempt_chain.chain.has_value()) {
        return attempt_chain.diagnostic;
    }
    if (attempt_chain.chain->size() != state.attempt_chain.size()) {
        return namespace_failure(Phase::attempt_validation, Status::namespace_invalid);
    }
    for (std::size_t index = 0; index < attempt_chain.chain->size(); ++index) {
        const auto& observed = (*attempt_chain.chain)[index];
        const auto& retained = state.attempt_chain[index];
        if (observed.names != retained.names || observed.bytes != retained.bytes ||
            observed.identity != retained.identity) {
            return namespace_failure(Phase::attempt_validation, Status::namespace_invalid);
        }
    }
    if (attempt_chain.chain->back().names != state.names ||
        attempt_chain.chain->back().identity != state.attempt_record_identity) {
        return namespace_failure(Phase::attempt_validation, Status::namespace_invalid);
    }

    if (const auto locked = validate_named_lock(
            state.root_descriptor, state.attempt_lock_descriptor, state.names.base_lock_leaf,
            state.attempt_lock_identity, Phase::attempt_base_lock_validation);
        !locked) {
        return locked;
    }
    if (const auto directory = validate_attempt_directory(
            state.root_descriptor, state.attempt_directory_descriptor,
            state.names.private_directory_leaf, state.attempt_directory_identity);
        !directory) {
        return directory;
    }
    if (const auto contents = require_owner_only_directory(state.attempt_directory_descriptor);
        !contents) {
        return contents;
    }

    const auto reserved = read_marker(state.root_descriptor, state.names.reserved_leaf);
    if (!reserved.marker.has_value()) {
        return reserved.diagnostic;
    }
    const auto owned = read_marker(state.root_descriptor, state.names.owned_leaf);
    if (!owned.marker.has_value()) {
        return owned.diagnostic;
    }
    const auto owner = read_marker(state.attempt_directory_descriptor,
                                   wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF);
    if (!owner.marker.has_value()) {
        return owner.diagnostic;
    }
    if (reserved.marker->bytes != state.reserved_marker_bytes ||
        owned.marker->bytes != state.owned_marker_bytes ||
        owner.marker->bytes != state.owner_marker_bytes ||
        reserved.marker->record.base_path_digest != base_path_digest ||
        reserved.marker->identity != state.reserved_marker_identity ||
        owned.marker->identity != state.owned_marker_identity ||
        owner.marker->identity != state.owner_marker_identity) {
        return namespace_failure(Phase::private_lease_validation, Status::private_lease_invalid);
    }
    if (const auto missing =
            require_missing_at(state.root_descriptor, state.names.reserved_pending_leaf,
                               Phase::private_lease_validation, Status::private_lease_invalid);
        missing.has_value()) {
        return *missing;
    }
    if (const auto missing =
            require_missing_at(state.root_descriptor, state.names.owned_pending_leaf,
                               Phase::private_lease_validation, Status::private_lease_invalid);
        missing.has_value()) {
        return *missing;
    }
    if (const auto staging =
            require_no_attempt_staging_candidates(state.root_descriptor, state.names);
        !staging) {
        return staging;
    }
    if (const auto missing =
            require_missing_at(state.attempt_directory_descriptor,
                               wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF,
                               Phase::private_lease_validation, Status::private_lease_invalid);
        missing.has_value()) {
        return *missing;
    }

    auto package = read_package(state.package_descriptor);
    if (!package.package.has_value()) {
        return package.diagnostic;
    }
    const auto work_digest = distributed_sieve_work_digest(package.package->identity);
    const auto manifest_binding =
        validate_manifest_work_identity(state.manifest, package.package->identity);
    if (!work_digest || !manifest_binding || *work_digest.digest != state.manifest.work_sha256 ||
        package.package->identity.distributed.chunks != state.identity.distributed.chunks ||
        !same_witness(package.package->witness, state.package_witness) ||
        package.identity != state.package_identity) {
        return !work_digest ? protocol_failure(Phase::work_package_validation,
                                               Status::work_package_invalid, work_digest.status)
               : !manifest_binding
                   ? protocol_failure(Phase::work_package_validation, Status::work_package_invalid,
                                      manifest_binding)
                   : namespace_failure(Phase::work_package_validation,
                                       Status::work_package_invalid);
    }
    return {
        .phase = Phase::final_revalidation,
        .status = Status::ready,
    };
}

template <typename State>
[[nodiscard]] Diagnostic validate_writer_lifetime_state(const State& state) {
    if (state.creator_process_id == 0 || current_process_id() != state.creator_process_id) {
        return failure(Phase::final_revalidation, Status::process_mismatch, EINVAL);
    }
    if (state.invalidated.load(std::memory_order_acquire)) {
        return namespace_failure(Phase::final_revalidation, Status::namespace_invalid);
    }
    if (const auto root = validate_root(state.root_descriptor, state.root_identity); !root) {
        return root;
    }
    if (const auto binding = validate_absolute_root_binding(
            state.root_descriptor, state.absolute_root_path, state.root_identity);
        !binding) {
        return binding;
    }
    const auto base_path_digest = expected_base_path_digest(state.absolute_root_path, state.names);
    if (base_path_digest != state.base_path_digest) {
        return namespace_failure(Phase::private_lease_validation, Status::private_lease_invalid);
    }
    if (const auto locked =
            validate_named_lock(state.root_descriptor, state.permanent_lock_descriptor,
                                wave::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF,
                                state.permanent_lock_identity, Phase::permanent_lock_validation);
        !locked) {
        return locked;
    }
    if (const auto locked = validate_named_lock(
            state.root_descriptor, state.attempt_lock_descriptor, state.names.base_lock_leaf,
            state.attempt_lock_identity, Phase::attempt_base_lock_validation);
        !locked) {
        return locked;
    }
    if (const auto directory = validate_attempt_directory(
            state.root_descriptor, state.attempt_directory_descriptor,
            state.names.private_directory_leaf, state.attempt_directory_identity);
        !directory) {
        return directory;
    }
    if (const auto contents = require_writer_directory_contents(state.attempt_directory_descriptor);
        !contents) {
        return contents;
    }

    const auto reserved = read_marker(state.root_descriptor, state.names.reserved_leaf);
    const auto owned = read_marker(state.root_descriptor, state.names.owned_leaf);
    const auto owner = read_marker(state.attempt_directory_descriptor,
                                   wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF);
    if (!reserved.marker.has_value()) {
        return reserved.diagnostic;
    }
    if (!owned.marker.has_value()) {
        return owned.diagnostic;
    }
    if (!owner.marker.has_value()) {
        return owner.diagnostic;
    }
    if (reserved.marker->bytes != state.reserved_marker_bytes ||
        owned.marker->bytes != state.owned_marker_bytes ||
        owner.marker->bytes != state.owner_marker_bytes ||
        reserved.marker->record.base_path_digest != base_path_digest ||
        reserved.marker->identity != state.reserved_marker_identity ||
        owned.marker->identity != state.owned_marker_identity ||
        owner.marker->identity != state.owner_marker_identity) {
        return namespace_failure(Phase::private_lease_validation, Status::private_lease_invalid);
    }
    if (const auto missing =
            require_missing_at(state.root_descriptor, state.names.reserved_pending_leaf,
                               Phase::private_lease_validation, Status::private_lease_invalid);
        missing.has_value()) {
        return *missing;
    }
    if (const auto missing =
            require_missing_at(state.root_descriptor, state.names.owned_pending_leaf,
                               Phase::private_lease_validation, Status::private_lease_invalid);
        missing.has_value()) {
        return *missing;
    }
    if (const auto staging =
            require_no_attempt_staging_candidates(state.root_descriptor, state.names);
        !staging) {
        return staging;
    }
    if (const auto missing =
            require_missing_at(state.attempt_directory_descriptor,
                               wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF,
                               Phase::private_lease_validation, Status::private_lease_invalid);
        missing.has_value()) {
        return *missing;
    }
    return {
        .phase = Phase::final_revalidation,
        .status = Status::ready,
    };
}

#endif

} // namespace

DistributedSieveWorkerEntryV1::DistributedSieveWorkerEntryV1(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

DistributedSieveWorkerEntryV1::DistributedSieveWorkerEntryV1(
    DistributedSieveWorkerEntryV1&&) noexcept = default;

DistributedSieveWorkerEntryV1::~DistributedSieveWorkerEntryV1() noexcept = default;

bool DistributedSieveWorkerEntryV1::valid() const noexcept {
    constexpr int first_retained_descriptor =
        process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR;
    return state_ != nullptr && state_->creator_process_id != 0 &&
           current_process_id() == state_->creator_process_id &&
           !state_->invalidated.load(std::memory_order_acquire) &&
           state_->root_descriptor >= first_retained_descriptor &&
           state_->permanent_lock_descriptor >= first_retained_descriptor &&
           state_->attempt_lock_descriptor >= first_retained_descriptor &&
           state_->attempt_directory_descriptor >= first_retained_descriptor &&
           state_->package_descriptor >= first_retained_descriptor;
}

DistributedSieveWorkerEntryDiagnosticV1 DistributedSieveWorkerEntryV1::revalidate() const noexcept {
    if (state_ == nullptr) {
        return failure(Phase::final_revalidation, Status::process_mismatch, EINVAL);
    }
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    return failure(Phase::platform_gate, Status::platform_unsupported, ENOTSUP);
#else
    try {
        const auto checked = validate_state(*state_);
        if (!checked) {
            state_->invalidated.store(true, std::memory_order_release);
        }
        return checked;
    } catch (const std::bad_alloc&) {
        state_->invalidated.store(true, std::memory_order_release);
        return failure(Phase::final_revalidation, Status::resource_exhausted, ENOMEM);
    } catch (...) {
        state_->invalidated.store(true, std::memory_order_release);
        return failure(Phase::final_revalidation, Status::unexpected_failure);
    }
#endif
}

const AttemptStartedV1& DistributedSieveWorkerEntryV1::record() const noexcept {
    return state_->record;
}

const WaveManifestV1& DistributedSieveWorkerEntryV1::manifest() const noexcept {
    return state_->manifest;
}

const DistributedSieveWorkIdentityV1& DistributedSieveWorkerEntryV1::identity() const noexcept {
    return state_->identity;
}

const ChunkPlanV1& DistributedSieveWorkerEntryV1::chunk() const noexcept {
    return state_->chunk;
}

const work_package::DistributedSieveWorkPackageWitnessV1&
DistributedSieveWorkerEntryV1::witness() const noexcept {
    return state_->package_witness;
}

DistributedSieveWorkerEntryAdoptionResultV1 adopt_distributed_sieve_worker_entry_v1() noexcept {
    return trusted_test::adopt_distributed_sieve_worker_entry_v1_with_hooks({});
}

DistributedSieveWorkerWriterAdoptionResultV1
consume_distributed_sieve_worker_writer_v1(DistributedSieveWorkerEntryV1&& entry) noexcept {
    return trusted_test::consume_distributed_sieve_worker_writer_v1_with_hooks(std::move(entry),
                                                                               {});
}

namespace trusted_test {

DistributedSieveWorkerWriterAdoptionResultV1 consume_distributed_sieve_worker_writer_v1_with_hooks(
    DistributedSieveWorkerEntryV1&& entry, DistributedSieveWorkerWriterTestHooksV1 hooks) noexcept {
    if (entry.state_ == nullptr) {
        return {
            .writer = std::nullopt,
            .diagnostic =
                writer_failure(WriterPhase::single_use_gate, WriterStatus::already_consumed),
        };
    }

    // Burn the entry at the beginning of the conversion attempt. A failed
    // authority mint cannot be retried with stale or partially transferred
    // capabilities.
    auto state = std::move(entry.state_);

#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    (void)hooks;
    return {
        .writer = std::nullopt,
        .diagnostic =
            writer_failure(WriterPhase::platform_gate, WriterStatus::platform_unsupported, ENOTSUP),
    };
#else
    if (state->creator_process_id == 0 || current_process_id() != state->creator_process_id) {
        state->invalidated.store(true, std::memory_order_release);
        return {
            .writer = std::nullopt,
            .diagnostic =
                writer_failure(WriterPhase::process_gate, WriterStatus::process_mismatch, ECHILD),
        };
    }

    try {
        std::filesystem::path absolute_root(state->absolute_root_path);
        std::filesystem::path private_directory =
            absolute_root / state->names.private_directory_leaf;
        std::filesystem::path base_path = private_directory / "corpus";
        std::filesystem::path lock_path = absolute_root / state->names.base_lock_leaf;
        const auto cleanup_paths = private_lease::freeze_paths(base_path);
        if (cleanup_paths.base_path != base_path ||
            cleanup_paths.private_directory != private_directory ||
            cleanup_paths.lock_path != lock_path ||
            cleanup_paths.index_path.filename() != "corpus.relidx" ||
            cleanup_paths.data_path.filename() != "corpus.reldata" ||
            private_lease::frozen_path_digest(cleanup_paths.base_path) != state->base_path_digest ||
            state->record.lease.lease_id.limbs == std::array<std::uint64_t, 2>{} ||
            state->record.lease.directory != state->attempt_directory_identity ||
            state->record.lease.owner_marker != state->owner_marker_identity) {
            state->invalidated.store(true, std::memory_order_release);
            return {
                .writer = std::nullopt,
                .diagnostic = writer_failure(WriterPhase::capability_transfer,
                                             WriterStatus::private_lease_invalid, EPROTO),
            };
        }

        // Allocate/copy every potentially throwing value before the final
        // validation. Descriptor transfer after that boundary is exchange-only.
        distributed_sieve_worker_writer_detail::OOCInheritedP8WriterMintV1 mint(
            -1, -1, -1, -1, -1, state->creator_process_id, std::move(absolute_root),
            std::move(base_path), std::move(private_directory), std::move(lock_path),
            state->names.private_directory_leaf, state->names.base_lock_leaf,
            relation_identity(state->root_identity),
            relation_identity(state->attempt_lock_identity),
            relation_identity(state->attempt_directory_identity),
            state->record.lease.lease_id.limbs, relation_identity(state->owner_marker_identity),
            relation_identity(state->owned_marker_identity), state->record, state->manifest,
            state->identity, state->chunk, state->package_witness, hooks.private_lease_hooks);

        const auto duplicate_for_writer = [](int descriptor) {
            int duplicate = -1;
            do {
                duplicate =
                    ::fcntl(descriptor, F_DUPFD_CLOEXEC,
                            process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
            } while (duplicate < 0 && errno == EINTR);
            if (duplicate < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "duplicate retained worker writer capability");
            }
            return UniqueFd(duplicate);
        };
        auto writer_root = duplicate_for_writer(state->root_descriptor);
        auto writer_permanent_lock = duplicate_for_writer(state->permanent_lock_descriptor);
        auto writer_attempt_lock = duplicate_for_writer(state->attempt_lock_descriptor);
        auto writer_attempt_directory = duplicate_for_writer(state->attempt_directory_descriptor);
        auto writer_package = duplicate_for_writer(state->package_descriptor);

        const auto first = validate_state(*state);
        if (!first) {
            state->invalidated.store(true, std::memory_order_release);
            return {
                .writer = std::nullopt,
                .diagnostic = writer_entry_failure(WriterPhase::entry_revalidation, first),
            };
        }
        if (hooks.after_first_validation != nullptr) {
            hooks.after_first_validation(hooks.context);
        }
        const auto confirmed = validate_state(*state);
        if (!confirmed) {
            state->invalidated.store(true, std::memory_order_release);
            return {
                .writer = std::nullopt,
                .diagnostic = writer_entry_failure(WriterPhase::final_revalidation, confirmed),
            };
        }

        mint.root_descriptor_ = writer_root.release();
        mint.permanent_lock_descriptor_ = writer_permanent_lock.release();
        mint.attempt_lock_descriptor_ = writer_attempt_lock.release();
        mint.attempt_directory_descriptor_ = writer_attempt_directory.release();
        mint.package_descriptor_ = writer_package.release();

        auto* retained_state = state.release();
        distributed_sieve_worker_writer_detail::DistributedSieveWorkerWriterLifetimeGuardV1
            lifetime_guard(
                retained_state,
                +[](const void* opaque) noexcept {
                    auto* retained =
                        static_cast<const DistributedSieveWorkerEntryV1::State*>(opaque);
                    try {
                        const auto diagnostic = validate_writer_lifetime_state(*retained);
                        if (diagnostic) {
                            return true;
                        }
                    } catch (...) {
                    }
                    const_cast<DistributedSieveWorkerEntryV1::State*>(retained)->invalidated.store(
                        true, std::memory_order_release);
                    return false;
                },
                +[](void* opaque) noexcept {
                    delete static_cast<DistributedSieveWorkerEntryV1::State*>(opaque);
                });
        mint.attach_lifetime_guard(std::move(lifetime_guard));

        return distributed_sieve_worker_writer_detail::mint_distributed_sieve_worker_writer_v1(
            std::move(mint));
    } catch (const std::bad_alloc&) {
        state->invalidated.store(true, std::memory_order_release);
        return {
            .writer = std::nullopt,
            .diagnostic = writer_failure(WriterPhase::capability_transfer,
                                         WriterStatus::resource_exhausted, ENOMEM),
        };
    } catch (const private_lease::Failure& failure) {
        state->invalidated.store(true, std::memory_order_release);
        return {
            .writer = std::nullopt,
            .diagnostic = writer_failure(WriterPhase::capability_transfer,
                                         WriterStatus::private_lease_invalid,
                                         failure.error ? failure.error.value() : EPROTO),
        };
    } catch (const std::system_error& error) {
        state->invalidated.store(true, std::memory_order_release);
        return {
            .writer = std::nullopt,
            .diagnostic = writer_failure(WriterPhase::capability_transfer,
                                         WriterStatus::private_lease_invalid, error.code().value()),
        };
    } catch (...) {
        state->invalidated.store(true, std::memory_order_release);
        return {
            .writer = std::nullopt,
            .diagnostic =
                writer_failure(WriterPhase::capability_transfer, WriterStatus::unexpected_failure),
        };
    }
#endif
}

DistributedSieveWorkerEntryAdoptionResultV1 adopt_distributed_sieve_worker_entry_v1_with_hooks(
    DistributedSieveWorkerEntryTestHooksV1 hooks) noexcept {
    if (ADOPTION_STARTED.test_and_set(std::memory_order_acq_rel)) {
        return {
            .entry = std::nullopt,
            .diagnostic = failure(Phase::single_use_gate, Status::already_adopted),
        };
    }
#if defined(_WIN32) || (!defined(__APPLE__) && !defined(__linux__))
    (void)hooks;
    return {
        .entry = std::nullopt,
        .diagnostic = failure(Phase::platform_gate, Status::platform_unsupported, ENOTSUP),
    };
#else
    try {
        auto captured = capture_fixed_capabilities();
        if (!captured.capabilities.has_value()) {
            (void)::close(STDIN_FILENO);
            return {.entry = std::nullopt, .diagnostic = captured.diagnostic};
        }
        auto capabilities = std::move(*captured.capabilities);

        auto bootstrap = read_bootstrap();
        if (!bootstrap.bytes.has_value()) {
            return {.entry = std::nullopt, .diagnostic = bootstrap.diagnostic};
        }
        auto decoded_bootstrap = decode_distributed_sieve_record(*bootstrap.bytes);
        if (!decoded_bootstrap) {
            return {
                .entry = std::nullopt,
                .diagnostic = protocol_failure(Phase::bootstrap_decode, Status::protocol_invalid,
                                               decoded_bootstrap.status),
            };
        }
        auto* attempt = std::get_if<AttemptStartedV1>(&*decoded_bootstrap.value);
        if (attempt == nullptr) {
            return {
                .entry = std::nullopt,
                .diagnostic = protocol_failure(
                    Phase::bootstrap_decode, Status::protocol_invalid,
                    {.error = DistributedSieveProtocolError::record_type_mismatch}),
            };
        }

        struct stat root_metadata {};
        if (fstat_retrying_eintr(capabilities.root.get(), root_metadata) != 0) {
            return {
                .entry = std::nullopt,
                .diagnostic = failure(Phase::root_validation, Status::namespace_invalid, errno),
            };
        }
        const NativeIdentityV1 root_identity = native_identity(root_metadata);
        if (const auto checked = validate_root(capabilities.root.get(), root_identity); !checked) {
            return {.entry = std::nullopt, .diagnostic = checked};
        }
        auto recovered_root_path = recover_root_path(capabilities.root.get());
        if (!recovered_root_path.path.has_value()) {
            return {.entry = std::nullopt, .diagnostic = recovered_root_path.diagnostic};
        }
        std::string absolute_root_path = std::move(*recovered_root_path.path);
        if (const auto checked = validate_absolute_root_binding(capabilities.root.get(),
                                                                absolute_root_path, root_identity);
            !checked) {
            return {.entry = std::nullopt, .diagnostic = checked};
        }

        struct stat permanent_lock_metadata {};
        if (fstat_retrying_eintr(capabilities.permanent_lock.get(), permanent_lock_metadata) != 0) {
            return {
                .entry = std::nullopt,
                .diagnostic =
                    failure(Phase::permanent_lock_validation, Status::lock_invalid, errno),
            };
        }
        const NativeIdentityV1 permanent_lock_identity = native_identity(permanent_lock_metadata);
        if (const auto checked =
                validate_named_lock(capabilities.root.get(), capabilities.permanent_lock.get(),
                                    wave::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF, permanent_lock_identity,
                                    Phase::permanent_lock_validation);
            !checked) {
            return {.entry = std::nullopt, .diagnostic = checked};
        }

        auto manifest_loaded =
            read_named_bytes(capabilities.root.get(), wave::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF,
                             DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES, std::nullopt,
                             Phase::manifest_validation, Status::namespace_invalid);
        if (!manifest_loaded.value.has_value()) {
            return {.entry = std::nullopt, .diagnostic = manifest_loaded.diagnostic};
        }
        if (const auto missing = require_missing_at(
                capabilities.root.get(), wave::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF,
                Phase::manifest_validation, Status::namespace_invalid);
            missing.has_value()) {
            return {.entry = std::nullopt, .diagnostic = *missing};
        }
        auto decoded_manifest = decode_distributed_sieve_record(manifest_loaded.value->bytes);
        if (!decoded_manifest) {
            return {
                .entry = std::nullopt,
                .diagnostic = protocol_failure(Phase::manifest_validation, Status::protocol_invalid,
                                               decoded_manifest.status),
            };
        }
        auto* manifest = std::get_if<WaveManifestV1>(&*decoded_manifest.value);
        if (manifest == nullptr) {
            return {
                .entry = std::nullopt,
                .diagnostic = protocol_failure(
                    Phase::manifest_validation, Status::protocol_invalid,
                    {.error = DistributedSieveProtocolError::record_type_mismatch}),
            };
        }
        if (manifest->self_digest != attempt->manifest_digest ||
            manifest->wave_root_identity != root_identity ||
            manifest->permanent_lock_identity != permanent_lock_identity ||
            manifest->lock_semantics_version !=
                wave::DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1) {
            return {
                .entry = std::nullopt,
                .diagnostic =
                    namespace_failure(Phase::manifest_validation, Status::namespace_invalid),
            };
        }

        const auto chunk_position = std::find_if(manifest->chunks.begin(), manifest->chunks.end(),
                                                 [&](const ChunkPlanV1& candidate) noexcept {
                                                     return candidate.chunk_id == attempt->chunk_id;
                                                 });
        if (chunk_position == manifest->chunks.end() ||
            chunk_position->sq_begin != attempt->sq_begin ||
            chunk_position->sq_end != attempt->sq_end ||
            attempt->attempt_ordinal >= manifest->max_worker_attempts ||
            attempt->retry_policy_version != manifest->retry_policy_version) {
            return {
                .entry = std::nullopt,
                .diagnostic =
                    namespace_failure(Phase::attempt_validation, Status::namespace_invalid),
            };
        }
        auto names = wave::distributed_sieve_worker_attempt_names_v1(
            chunk_position->relative_artifact_stem, chunk_position->chunk_id,
            attempt->attempt_ordinal);
        if (!names.has_value() || names->relative_lease_stem != attempt->lease.relative_stem) {
            return {
                .entry = std::nullopt,
                .diagnostic =
                    namespace_failure(Phase::attempt_validation, Status::namespace_invalid),
            };
        }
        auto attempt_chain = read_attempt_chain(capabilities.root.get(), *manifest, *chunk_position,
                                                *attempt, *bootstrap.bytes);
        if (!attempt_chain.chain.has_value()) {
            return {.entry = std::nullopt, .diagnostic = attempt_chain.diagnostic};
        }
        if (attempt_chain.chain->back().names != *names) {
            return {
                .entry = std::nullopt,
                .diagnostic =
                    namespace_failure(Phase::attempt_validation, Status::namespace_invalid),
            };
        }

        struct stat attempt_lock_metadata {};
        if (fstat_retrying_eintr(capabilities.attempt_lock.get(), attempt_lock_metadata) != 0) {
            return {
                .entry = std::nullopt,
                .diagnostic =
                    failure(Phase::attempt_base_lock_validation, Status::lock_invalid, errno),
            };
        }
        const NativeIdentityV1 attempt_lock_identity = native_identity(attempt_lock_metadata);
        if (const auto checked = validate_named_lock(
                capabilities.root.get(), capabilities.attempt_lock.get(), names->base_lock_leaf,
                attempt_lock_identity, Phase::attempt_base_lock_validation);
            !checked) {
            return {.entry = std::nullopt, .diagnostic = checked};
        }

        const int attempt_directory_source_descriptor =
            openat_retrying_eintr(capabilities.root.get(), names->private_directory_leaf.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (attempt_directory_source_descriptor < 0) {
            return {
                .entry = std::nullopt,
                .diagnostic =
                    failure(Phase::private_lease_validation, Status::private_lease_invalid, errno),
            };
        }
        UniqueFd attempt_directory_source(attempt_directory_source_descriptor);
        int attempt_directory_descriptor = -1;
        do {
            attempt_directory_descriptor =
                ::fcntl(attempt_directory_source.get(), F_DUPFD_CLOEXEC,
                        process::DISTRIBUTED_SIEVE_WORKER_CHILD_FIRST_UNMAPPED_DESCRIPTOR);
        } while (attempt_directory_descriptor < 0 && errno == EINTR);
        if (attempt_directory_descriptor < 0) {
            const int saved_errno = errno;
            return {
                .entry = std::nullopt,
                .diagnostic = failure(Phase::private_lease_validation,
                                      resource_error(saved_errno) ? Status::resource_exhausted
                                                                  : Status::private_lease_invalid,
                                      saved_errno),
            };
        }
        UniqueFd attempt_directory(attempt_directory_descriptor);
        attempt_directory_source.reset();
        struct stat attempt_directory_metadata {};
        if (fstat_retrying_eintr(attempt_directory.get(), attempt_directory_metadata) != 0) {
            return {
                .entry = std::nullopt,
                .diagnostic =
                    failure(Phase::private_lease_validation, Status::private_lease_invalid, errno),
            };
        }
        const NativeIdentityV1 attempt_directory_identity =
            native_identity(attempt_directory_metadata);
        if (const auto checked = validate_attempt_directory(
                capabilities.root.get(), attempt_directory.get(), names->private_directory_leaf,
                attempt_directory_identity);
            !checked) {
            return {.entry = std::nullopt, .diagnostic = checked};
        }
        if (const auto contents = require_owner_only_directory(attempt_directory.get());
            !contents) {
            return {.entry = std::nullopt, .diagnostic = contents};
        }

        auto reserved = read_marker(capabilities.root.get(), names->reserved_leaf);
        auto owned = read_marker(capabilities.root.get(), names->owned_leaf);
        auto owner =
            read_marker(attempt_directory.get(), wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_LEAF);
        if (!reserved.marker.has_value()) {
            return {.entry = std::nullopt, .diagnostic = reserved.diagnostic};
        }
        if (!owned.marker.has_value()) {
            return {.entry = std::nullopt, .diagnostic = owned.diagnostic};
        }
        if (!owner.marker.has_value()) {
            return {.entry = std::nullopt, .diagnostic = owner.diagnostic};
        }
        const auto base_path_digest = expected_base_path_digest(absolute_root_path, *names);
        if (const auto missing =
                require_missing_at(capabilities.root.get(), names->reserved_pending_leaf,
                                   Phase::private_lease_validation, Status::private_lease_invalid);
            missing.has_value()) {
            return {.entry = std::nullopt, .diagnostic = *missing};
        }
        if (const auto missing =
                require_missing_at(capabilities.root.get(), names->owned_pending_leaf,
                                   Phase::private_lease_validation, Status::private_lease_invalid);
            missing.has_value()) {
            return {.entry = std::nullopt, .diagnostic = *missing};
        }
        if (const auto staging =
                require_no_attempt_staging_candidates(capabilities.root.get(), *names);
            !staging) {
            return {.entry = std::nullopt, .diagnostic = staging};
        }
        if (const auto missing = require_missing_at(
                attempt_directory.get(), wave::DISTRIBUTED_SIEVE_PRIVATE_LEASE_OWNER_PENDING_LEAF,
                Phase::private_lease_validation, Status::private_lease_invalid);
            missing.has_value()) {
            return {.entry = std::nullopt, .diagnostic = *missing};
        }

        const auto& reserved_record = reserved.marker->record;
        const auto expected_owner = private_lease::make_private_lease_owner_record(
            reserved_record, relation_identity(attempt_directory_identity));
        const auto expected_owned = private_lease::make_private_lease_owned_record(
            expected_owner, relation_identity(owner.marker->identity));
        if (reserved_record.phase != private_lease::PrivateLeasePhase::Reserved ||
            reserved_record.capability !=
                private_lease::PrivateLeaseCapability::RollbackStagingOnly ||
            reserved_record.lease_id != attempt->lease.lease_id.limbs ||
            reserved_record.base_path_digest != base_path_digest ||
            reserved_record.parent_identity != relation_identity(root_identity) ||
            reserved_record.lock_identity != relation_identity(attempt_lock_identity) ||
            owner.marker->record != expected_owner || owned.marker->record != expected_owned ||
            attempt->lease.directory != attempt_directory_identity ||
            attempt->lease.owner_marker != owner.marker->identity) {
            return {
                .entry = std::nullopt,
                .diagnostic = namespace_failure(Phase::private_lease_validation,
                                                Status::private_lease_invalid),
            };
        }

        auto package = read_package(capabilities.package.get());
        if (!package.package.has_value()) {
            return {.entry = std::nullopt, .diagnostic = package.diagnostic};
        }
        auto work_digest = distributed_sieve_work_digest(package.package->identity);
        auto manifest_binding =
            validate_manifest_work_identity(*manifest, package.package->identity);
        if (!work_digest || !manifest_binding || *work_digest.digest != manifest->work_sha256 ||
            package.package->witness.work_sha256 != manifest->work_sha256) {
            return {
                .entry = std::nullopt,
                .diagnostic = !work_digest ? protocol_failure(Phase::work_package_validation,
                                                              Status::work_package_invalid,
                                                              work_digest.status)
                              : !manifest_binding
                                  ? protocol_failure(Phase::work_package_validation,
                                                     Status::work_package_invalid, manifest_binding)
                                  : namespace_failure(Phase::work_package_validation,
                                                      Status::work_package_invalid),
            };
        }

        ChunkPlanV1 selected_chunk = *chunk_position;
        auto state = std::make_unique<DistributedSieveWorkerEntryV1::State>();
        state->root_descriptor = capabilities.root.release();
        state->permanent_lock_descriptor = capabilities.permanent_lock.release();
        state->attempt_lock_descriptor = capabilities.attempt_lock.release();
        state->attempt_directory_descriptor = attempt_directory.release();
        state->package_descriptor = capabilities.package.release();
        state->creator_process_id = current_process_id();
        state->absolute_root_path = std::move(absolute_root_path);
        state->base_path_digest = base_path_digest;
        state->record = std::move(*attempt);
        state->manifest = std::move(*manifest);
        state->identity = std::move(package.package->identity);
        state->chunk = std::move(selected_chunk);
        state->package_witness = package.package->witness;
        state->names = std::move(*names);
        state->root_identity = root_identity;
        state->permanent_lock_identity = permanent_lock_identity;
        state->attempt_lock_identity = attempt_lock_identity;
        state->attempt_directory_identity = attempt_directory_identity;
        state->package_identity = package.identity;
        state->manifest_file_identity = manifest_loaded.value->identity;
        state->attempt_record_identity = attempt_chain.chain->back().identity;
        state->reserved_marker_identity = reserved.marker->identity;
        state->owner_marker_identity = owner.marker->identity;
        state->owned_marker_identity = owned.marker->identity;
        state->bootstrap_bytes = std::move(*bootstrap.bytes);
        state->manifest_bytes = std::move(manifest_loaded.value->bytes);
        state->reserved_marker_bytes = std::move(reserved.marker->bytes);
        state->owner_marker_bytes = std::move(owner.marker->bytes);
        state->owned_marker_bytes = std::move(owned.marker->bytes);
        state->attempt_chain = std::move(*attempt_chain.chain);

        if (const auto first = validate_state(*state); !first) {
            state->invalidated.store(true, std::memory_order_release);
            return {.entry = std::nullopt, .diagnostic = first};
        }
        if (hooks.after_first_validation != nullptr) {
            hooks.after_first_validation(hooks.context);
        }
        if (auto confirmed = validate_state(*state); !confirmed) {
            state->invalidated.store(true, std::memory_order_release);
            confirmed.phase = Phase::final_revalidation;
            return {.entry = std::nullopt, .diagnostic = confirmed};
        }

        DistributedSieveWorkerEntryV1 entry(std::move(state));
        return {
            .entry = std::move(entry),
            .diagnostic =
                {
                    .phase = Phase::final_revalidation,
                    .status = Status::ready,
                },
        };
    } catch (const std::bad_alloc&) {
        (void)::close(STDIN_FILENO);
        return {
            .entry = std::nullopt,
            .diagnostic = failure(Phase::final_revalidation, Status::resource_exhausted, ENOMEM),
        };
    } catch (...) {
        (void)::close(STDIN_FILENO);
        return {
            .entry = std::nullopt,
            .diagnostic = failure(Phase::final_revalidation, Status::unexpected_failure),
        };
    }
#endif
}

} // namespace trusted_test

} // namespace gnfs::sieve::distributed_sieve_worker_entry_detail
