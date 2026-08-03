#pragma once

// Test seam for the source-private worker work-package file carrier. These
// operations report observations only; satisfying them cannot mint authority.

#include "distributed_sieve_work_package_codec_internal.hpp"

#include <cstddef>
#include <cstdint>

namespace gnfs::sieve::distributed_sieve_worker_work_package_file_detail {

using DistributedSieveWorkerWorkPackageNativeHandle = std::intptr_t;

inline constexpr DistributedSieveWorkerWorkPackageNativeHandle
    DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE = -1;

enum class DistributedSieveWorkerWorkPackageOperationState : std::uint8_t {
    succeeded,
    interrupted,
    missing,
    already_exists,
    unsupported,
    failed,
};

struct DistributedSieveWorkerWorkPackageOperationResult final {
    DistributedSieveWorkerWorkPackageOperationState state =
        DistributedSieveWorkerWorkPackageOperationState::failed;
    int native_error = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return state == DistributedSieveWorkerWorkPackageOperationState::succeeded;
    }
};

struct DistributedSieveWorkerWorkPackageOpenResult final {
    DistributedSieveWorkerWorkPackageOperationResult operation;
    DistributedSieveWorkerWorkPackageNativeHandle handle =
        DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
};

struct DistributedSieveWorkerWorkPackageWriteResult final {
    DistributedSieveWorkerWorkPackageOperationResult operation;
    std::size_t bytes_written = 0;
};

enum class DistributedSieveWorkerWorkPackageObjectKind : std::uint8_t {
    regular_file,
    directory,
    other,
};

struct DistributedSieveWorkerWorkPackageMetadataV1 final {
    DistributedSieveWorkerWorkPackageObjectKind kind =
        DistributedSieveWorkerWorkPackageObjectKind::other;
    NativeIdentityV1 identity;
    std::uint64_t owner_user_id = 0;
    std::uint32_t mode = 0;
    std::uint64_t link_count = 0;
    std::uint64_t size = 0;
};

struct DistributedSieveWorkerWorkPackageMetadataResultV1 final {
    DistributedSieveWorkerWorkPackageOperationResult operation;
    DistributedSieveWorkerWorkPackageMetadataV1 metadata;
};

struct DistributedSieveWorkerWorkPackageAclResultV1 final {
    DistributedSieveWorkerWorkPackageOperationResult operation;
    bool has_extended_acl = false;
};

struct DistributedSieveWorkerWorkPackageDescriptorPolicyResultV1 final {
    DistributedSieveWorkerWorkPackageOperationResult operation;
    bool read_only = false;
    bool close_on_exec = false;
};

struct DistributedSieveWorkerWorkPackageDecodeResultV1 final {
    DistributedSieveWorkerWorkPackageOperationResult operation;
    distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageDecodeResultV1 decoded;
};

/// Narrow, non-authoritative POSIX-shaped seam used to exhaustively test the
/// state machine. Only the production wrapper may turn a retained descriptor
/// into a move-only carrier token.
class DistributedSieveWorkerWorkPackageFileOpsV1 {
public:
    virtual ~DistributedSieveWorkerWorkPackageFileOpsV1() = default;

    [[nodiscard]] virtual std::uint64_t current_process_id() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t effective_user_id() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t maximum_file_offset() const noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageMetadataResultV1
    stat_handle(DistributedSieveWorkerWorkPackageNativeHandle handle) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageMetadataResultV1
    stat_at_nofollow(DistributedSieveWorkerWorkPackageNativeHandle directory_handle) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageAclResultV1
    inspect_acl(DistributedSieveWorkerWorkPackageNativeHandle handle, bool directory) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageOpenResult
    open_exclusive_at(DistributedSieveWorkerWorkPackageNativeHandle directory_handle) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageOperationResult
    set_mode(DistributedSieveWorkerWorkPackageNativeHandle handle, std::uint32_t mode) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageWriteResult
    pwrite_some(DistributedSieveWorkerWorkPackageNativeHandle handle, const std::byte* data,
                std::size_t size, std::uint64_t offset) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageOperationResult
    sync_handle(DistributedSieveWorkerWorkPackageNativeHandle handle) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageOpenResult
    open_readonly_at(DistributedSieveWorkerWorkPackageNativeHandle directory_handle) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageDescriptorPolicyResultV1
    inspect_read_descriptor(DistributedSieveWorkerWorkPackageNativeHandle handle) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageDecodeResultV1
    decode_exact(DistributedSieveWorkerWorkPackageNativeHandle handle,
                 std::uint64_t total_bytes) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageOperationResult
    close_handle(DistributedSieveWorkerWorkPackageNativeHandle handle) noexcept = 0;

    [[nodiscard]] virtual DistributedSieveWorkerWorkPackageOperationResult
    unlink_at(DistributedSieveWorkerWorkPackageNativeHandle directory_handle) noexcept = 0;
};

} // namespace gnfs::sieve::distributed_sieve_worker_work_package_file_detail
