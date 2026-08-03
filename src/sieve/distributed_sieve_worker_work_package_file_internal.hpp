#pragma once

// Source-private, path-free carrier for one canonical worker work package.

#include "distributed_sieve_worker_work_package_file_ops_internal.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace gnfs::sieve::distributed_sieve_resume_detail {
class DistributedSieveWaveStore;
}

namespace gnfs::sieve::distributed_sieve_worker_work_package_file_detail {

inline constexpr std::string_view DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1 =
    ".gnfs-worker-work-package-v1";

enum class DistributedSieveWorkerWorkPackageFileStatus : std::uint8_t {
    ready,
    interrupted,
    invalid_request,
    platform_unavailable,
    resource_exhausted,
    namespace_conflict,
    publication_failed,
    durability_failed,
    decode_failed,
    close_failed,
    ops_contract_violation,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_worker_work_package_file_status_name(
    DistributedSieveWorkerWorkPackageFileStatus status) noexcept {
    switch (status) {
    case DistributedSieveWorkerWorkPackageFileStatus::ready:
        return "ready";
    case DistributedSieveWorkerWorkPackageFileStatus::interrupted:
        return "interrupted";
    case DistributedSieveWorkerWorkPackageFileStatus::invalid_request:
        return "invalid_request";
    case DistributedSieveWorkerWorkPackageFileStatus::platform_unavailable:
        return "platform_unavailable";
    case DistributedSieveWorkerWorkPackageFileStatus::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict:
        return "namespace_conflict";
    case DistributedSieveWorkerWorkPackageFileStatus::publication_failed:
        return "publication_failed";
    case DistributedSieveWorkerWorkPackageFileStatus::durability_failed:
        return "durability_failed";
    case DistributedSieveWorkerWorkPackageFileStatus::decode_failed:
        return "decode_failed";
    case DistributedSieveWorkerWorkPackageFileStatus::close_failed:
        return "close_failed";
    case DistributedSieveWorkerWorkPackageFileStatus::ops_contract_violation:
        return "ops_contract_violation";
    case DistributedSieveWorkerWorkPackageFileStatus::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveWorkerWorkPackageFileDiagnostic final {
    DistributedSieveWorkerWorkPackageFileStatus status =
        DistributedSieveWorkerWorkPackageFileStatus::ready;
    int native_error = 0;
    DistributedSieveProtocolStatus protocol_status;
    int secondary_close_error = 0;
    bool named_may_remain = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == DistributedSieveWorkerWorkPackageFileStatus::ready;
    }
};

struct DistributedSieveWorkerWorkPackageFileRequestV1 final {
    DistributedSieveWorkerWorkPackageNativeHandle borrowed_attempt_directory_handle =
        DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
    NativeIdentityV1 expected_directory_identity;
    std::uint64_t creator_process_id = 0;
};

struct DistributedSieveWorkerWorkPackageFileWitnessV1 final {
    distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageWitnessV1 package;
    NativeIdentityV1 file_identity;
    std::uint64_t file_extent = 0;
    std::uint64_t creator_process_id = 0;
};

class DistributedSieveWorkerWorkPackageFileV1;
struct DistributedSieveWorkerWorkPackageFileResultV1;

[[nodiscard]] DistributedSieveWorkerWorkPackageFileResultV1
create_distributed_sieve_worker_work_package_file_v1(
    const DistributedSieveWorkerWorkPackageFileRequestV1& request,
    const DistributedSieveWorkIdentityV1& identity) noexcept;

class DistributedSieveWorkerWorkPackageFileV1 final {
public:
    ~DistributedSieveWorkerWorkPackageFileV1() noexcept;

    DistributedSieveWorkerWorkPackageFileV1(const DistributedSieveWorkerWorkPackageFileV1&) =
        delete;
    DistributedSieveWorkerWorkPackageFileV1&
    operator=(const DistributedSieveWorkerWorkPackageFileV1&) = delete;

    DistributedSieveWorkerWorkPackageFileV1(
        DistributedSieveWorkerWorkPackageFileV1&& other) noexcept;
    DistributedSieveWorkerWorkPackageFileV1&
    operator=(DistributedSieveWorkerWorkPackageFileV1&&) = delete;

    [[nodiscard]] bool owned_by_current_process() const noexcept;
    [[nodiscard]] const DistributedSieveWorkerWorkPackageFileWitnessV1& witness() const noexcept;
    [[nodiscard]] DistributedSieveWorkerWorkPackageFileDiagnostic revalidate() const noexcept;

private:
    DistributedSieveWorkerWorkPackageFileV1(
        DistributedSieveWorkerWorkPackageNativeHandle retained_reader,
        DistributedSieveWorkerWorkPackageFileWitnessV1 witness,
        std::uint64_t creator_process_id) noexcept;

    DistributedSieveWorkerWorkPackageNativeHandle retained_reader_ =
        DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
    DistributedSieveWorkerWorkPackageFileWitnessV1 witness_;
    std::uint64_t creator_process_id_ = 0;

    friend DistributedSieveWorkerWorkPackageFileResultV1
    create_distributed_sieve_worker_work_package_file_v1(
        const DistributedSieveWorkerWorkPackageFileRequestV1& request,
        const DistributedSieveWorkIdentityV1& identity) noexcept;
    friend class distributed_sieve_resume_detail::DistributedSieveWaveStore;
};

struct DistributedSieveWorkerWorkPackageFileResultV1 final {
    std::optional<DistributedSieveWorkerWorkPackageFileV1> file;
    DistributedSieveWorkerWorkPackageFileDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return file.has_value() && static_cast<bool>(diagnostic);
    }
};

struct DistributedSieveWorkerWorkPackageFileWithOpsResultV1 final {
    std::optional<DistributedSieveWorkerWorkPackageFileWitnessV1> witness;
    DistributedSieveWorkerWorkPackageFileDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return witness.has_value() && static_cast<bool>(diagnostic);
    }
};

/// Test-only state-machine entry point. Even on success the reader is closed
/// and only a data witness is returned, so fake operations cannot mint a
/// descriptor-bearing capability.
[[nodiscard]] DistributedSieveWorkerWorkPackageFileWithOpsResultV1
create_distributed_sieve_worker_work_package_file_v1_with_ops(
    const DistributedSieveWorkerWorkPackageFileRequestV1& request,
    const DistributedSieveWorkIdentityV1& identity,
    DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept;

/// Read-only cross-process witness for a still-named, sealed work-package
/// residue. The observer process id authenticates only the live inspection;
/// it is deliberately absent from the durable witness.
struct DistributedSieveWorkerWorkPackageResidueInspectionRequestV1 final {
    DistributedSieveWorkerWorkPackageNativeHandle borrowed_attempt_directory_handle =
        DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
    NativeIdentityV1 expected_directory_identity;
    std::uint64_t observer_process_id = 0;
};

struct DistributedSieveWorkerWorkPackageResidueWitnessV1 final {
    DistributedSieveWorkIdentityV1 identity;
    distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageWitnessV1 package;
    NativeIdentityV1 file_identity;
    std::uint64_t file_extent = 0;
    std::uint64_t owner_user_id = 0;

    /// Authentic residue witnesses compare their canonical work identities by
    /// the same SHA-256 binding carried by the package envelope.
    friend bool operator==(const DistributedSieveWorkerWorkPackageResidueWitnessV1& left,
                           const DistributedSieveWorkerWorkPackageResidueWitnessV1& right) noexcept;
};

struct DistributedSieveWorkerWorkPackageResidueInspectionResultV1 final {
    std::optional<DistributedSieveWorkerWorkPackageResidueWitnessV1> witness;
    DistributedSieveWorkerWorkPackageFileDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return witness.has_value() && static_cast<bool>(diagnostic);
    }
};

/// Inspect the fixed named leaf without mutating it or retaining a descriptor.
/// Success means the leaf was present and fully authenticated against the
/// exact borrowed attempt-directory capability.
[[nodiscard]] DistributedSieveWorkerWorkPackageResidueInspectionResultV1
inspect_distributed_sieve_worker_work_package_residue_v1(
    const DistributedSieveWorkerWorkPackageResidueInspectionRequestV1& request) noexcept;

/// Test-only state-machine entry point. As with the production inspector, the
/// reader is always closed and only a copy/move data witness may escape.
[[nodiscard]] DistributedSieveWorkerWorkPackageResidueInspectionResultV1
inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
    const DistributedSieveWorkerWorkPackageResidueInspectionRequestV1& request,
    DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept;

enum class DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1 : std::uint8_t {
    removed,
    confirmed_absent,
};

enum class DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1 : std::uint8_t {
    after_name_unlinked,
    after_directory_durable,
};

/// A null expected_residue requests the idempotent absence barrier. A non-null
/// witness authorizes removal only after the named file is proven to be that
/// exact sealed package. The borrowed directory descriptor remains caller
/// owned in both cases.
struct DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1 final {
    DistributedSieveWorkerWorkPackageNativeHandle borrowed_attempt_directory_handle =
        DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
    NativeIdentityV1 expected_directory_identity;
    std::uint64_t reconciler_process_id = 0;
    const DistributedSieveWorkerWorkPackageResidueWitnessV1* expected_residue = nullptr;
};

struct DistributedSieveWorkerWorkPackageResidueReconciliationResultV1 final {
    std::optional<DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1> disposition;
    DistributedSieveWorkerWorkPackageFileDiagnostic diagnostic;
    std::optional<DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1> fault_point;

    [[nodiscard]] explicit operator bool() const noexcept {
        return disposition.has_value() && static_cast<bool>(diagnostic) && !fault_point.has_value();
    }
};

using DistributedSieveWorkerWorkPackageResidueReconciliationHookV1 = void (*)(void*) noexcept;
using DistributedSieveWorkerWorkPackageResidueReconciliationFailBeforeDirectorySyncV1 =
    bool (*)(void*) noexcept;

/// Trusted internal test bridge for scheduling, injected durability failure,
/// and crash prefixes. Ordinary production behavior leaves it empty; the
/// WaveStore may pass only its static source-private bridge. Hooks convey no
/// filesystem authority and every observation they can invalidate is
/// re-proven before success.
struct DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 final {
    DistributedSieveWorkerWorkPackageResidueReconciliationHookV1 before_unlink = nullptr;
    DistributedSieveWorkerWorkPackageResidueReconciliationFailBeforeDirectorySyncV1
        fail_before_directory_sync = nullptr;
    DistributedSieveWorkerWorkPackageResidueReconciliationHookV1 after_directory_durable = nullptr;
    std::optional<DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1> stop_after;
    void* context = nullptr;
};

/// Authenticates and durably removes the exact fixed-leaf residue, or performs
/// an idempotent durable absence barrier when expected_residue is null.
[[nodiscard]] DistributedSieveWorkerWorkPackageResidueReconciliationResultV1
reconcile_distributed_sieve_worker_work_package_residue_v1(
    const DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1& request,
    const DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1& hooks = {}) noexcept;

/// Test-only state-machine entry point using caller-supplied fake operations.
[[nodiscard]] DistributedSieveWorkerWorkPackageResidueReconciliationResultV1
reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
    const DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1& request,
    DistributedSieveWorkerWorkPackageFileOpsV1& ops,
    const DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1& hooks = {}) noexcept;

// This standalone mechanism owns only the fixed-leaf transaction relative to
// the borrowed attempt-directory descriptor. It neither closes nor stores that
// descriptor and grants no WaveStore/manifest/launch authority. A launcher
// must first root-relatively bind the held directory to the expected identity;
// its receipt and manifest checks belong to that higher authority boundary.

} // namespace gnfs::sieve::distributed_sieve_worker_work_package_file_detail
