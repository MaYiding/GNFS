#include "distributed_sieve_worker_work_package_file_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#endif

namespace gnfs::sieve::distributed_sieve_worker_work_package_file_detail {
namespace {

using OperationResult = DistributedSieveWorkerWorkPackageOperationResult;
using OperationState = DistributedSieveWorkerWorkPackageOperationState;
using NativeHandle = DistributedSieveWorkerWorkPackageNativeHandle;
using Metadata = DistributedSieveWorkerWorkPackageMetadataV1;
using MetadataResult = DistributedSieveWorkerWorkPackageMetadataResultV1;
using FileStatus = DistributedSieveWorkerWorkPackageFileStatus;
using Diagnostic = DistributedSieveWorkerWorkPackageFileDiagnostic;
using PackageWitness =
    distributed_sieve_work_package_codec_detail::DistributedSieveWorkPackageWitnessV1;
using ResidueInspectionRequest = DistributedSieveWorkerWorkPackageResidueInspectionRequestV1;
using ResidueWitness = DistributedSieveWorkerWorkPackageResidueWitnessV1;
using ResidueReconciliationRequest =
    DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1;
using ResidueReconciliationResult = DistributedSieveWorkerWorkPackageResidueReconciliationResultV1;
using ResidueReconciliationDisposition =
    DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1;
using ResidueReconciliationFaultPoint =
    DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1;
using ResidueReconciliationTestHooks =
    DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1;

constexpr std::uint32_t PRIVATE_DIRECTORY_MODE = 0700;
constexpr std::uint32_t WRITABLE_FILE_MODE = 0600;
constexpr std::uint32_t SEALED_FILE_MODE = 0400;
constexpr std::size_t WRITE_BUFFER_BYTES = 64U * 1024U;

#if defined(__APPLE__) || defined(__linux__)
constexpr int PROCESS_ID_MISMATCH_ERROR = ECHILD;
constexpr int PROTOCOL_CONTRACT_ERROR = EPROTO;
constexpr int FILE_EXTENT_OVERFLOW_ERROR = EOVERFLOW;
#else
// The carrier is deliberately unavailable on other platforms, but its common
// state machine must still compile where the POSIX errno extensions above are
// absent (notably MSVC).
constexpr int PROCESS_ID_MISMATCH_ERROR = EINVAL;
constexpr int PROTOCOL_CONTRACT_ERROR = EINVAL;
constexpr int FILE_EXTENT_OVERFLOW_ERROR = ERANGE;
constexpr int PLATFORM_UNAVAILABLE_ERROR = EINVAL;
#endif

[[nodiscard]] constexpr OperationResult operation_success() noexcept {
    return {OperationState::succeeded, 0};
}

[[nodiscard]] constexpr OperationResult operation_failure(OperationState state,
                                                          int native_error) noexcept {
    return {state, native_error};
}

[[nodiscard]] constexpr Diagnostic make_diagnostic(FileStatus status, int native_error = 0,
                                                   DistributedSieveProtocolStatus protocol = {},
                                                   bool named_may_remain = false) noexcept {
    return {
        .status = status,
        .native_error = native_error,
        .protocol_status = protocol,
        .secondary_close_error = 0,
        .named_may_remain = named_may_remain,
    };
}

[[nodiscard]] constexpr bool same_package_witness(const PackageWitness& left,
                                                  const PackageWitness& right) noexcept {
    return left.body_bytes == right.body_bytes && left.total_bytes == right.total_bytes &&
           left.work_sha256 == right.work_sha256 && left.package_sha256 == right.package_sha256;
}

[[nodiscard]] constexpr bool is_resource_error(int native_error) noexcept {
    return native_error == EMFILE || native_error == ENFILE || native_error == ENOMEM ||
           native_error == ENOSPC;
}

template <typename Callable>
[[nodiscard]] auto retry_interrupted(Callable&& callable) noexcept(noexcept(callable())) {
    auto result = callable();
    while (result.operation.state == OperationState::interrupted) {
        result = callable();
    }
    return result;
}

template <typename Callable>
[[nodiscard]] OperationResult
retry_interrupted_operation(Callable&& callable) noexcept(noexcept(callable())) {
    auto result = callable();
    while (result.state == OperationState::interrupted) {
        result = callable();
    }
    return result;
}

[[nodiscard]] constexpr bool valid_success(const OperationResult& operation) noexcept {
    return operation.state == OperationState::succeeded && operation.native_error == 0;
}

[[nodiscard]] constexpr bool valid_failure(const OperationResult& operation) noexcept {
    return operation.state != OperationState::succeeded;
}

class BufferedPwriteSink final {
public:
    BufferedPwriteSink(DistributedSieveWorkerWorkPackageFileOpsV1& ops, NativeHandle writer,
                       std::uint64_t maximum_offset, std::uint64_t creator_process_id) noexcept
        : ops_(ops), writer_(writer), maximum_offset_(maximum_offset),
          creator_process_id_(creator_process_id) {}

    void put_bytes(std::span<const std::byte> bytes) noexcept {
        while (good_ && !bytes.empty()) {
            const std::size_t available = buffer_.size() - buffered_;
            const std::size_t count = std::min(available, bytes.size());
            std::copy_n(bytes.data(), count, buffer_.data() + buffered_);
            buffered_ += count;
            bytes = bytes.subspan(count);
            if (buffered_ == buffer_.size()) {
                flush();
            }
        }
    }

    [[nodiscard]] bool good() const noexcept {
        return good_;
    }

    [[nodiscard]] bool finish() noexcept {
        flush();
        return good_;
    }

    [[nodiscard]] std::uint64_t bytes_written() const noexcept {
        return offset_;
    }

    [[nodiscard]] const Diagnostic& diagnostic() const noexcept {
        return diagnostic_;
    }

private:
    void fail(FileStatus status, int native_error = 0) noexcept {
        if (!good_) {
            return;
        }
        good_ = false;
        diagnostic_ = make_diagnostic(status, native_error, {}, true);
    }

    void flush() noexcept {
        if (!good_ || buffered_ == 0) {
            return;
        }
        if (ops_.current_process_id() != creator_process_id_) {
            fail(FileStatus::invalid_request, PROCESS_ID_MISMATCH_ERROR);
            return;
        }
        if (offset_ > maximum_offset_ ||
            static_cast<std::uint64_t>(buffered_) > maximum_offset_ - offset_ ||
            buffered_ > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
            fail(FileStatus::publication_failed, FILE_EXTENT_OVERFLOW_ERROR);
            return;
        }

        std::size_t consumed = 0;
        while (good_ && consumed < buffered_) {
            const std::size_t remaining = buffered_ - consumed;
            const auto written = retry_interrupted([&]() noexcept {
                return ops_.pwrite_some(writer_, buffer_.data() + consumed, remaining,
                                        offset_ + consumed);
            });
            if (!valid_failure(written.operation) && !valid_success(written.operation)) {
                fail(FileStatus::ops_contract_violation, PROTOCOL_CONTRACT_ERROR);
                return;
            }
            if (!written.operation) {
                fail(FileStatus::publication_failed, written.operation.native_error);
                return;
            }
            if (written.bytes_written == 0) {
                fail(FileStatus::publication_failed, EIO);
                return;
            }
            if (written.bytes_written > remaining) {
                fail(FileStatus::ops_contract_violation, PROTOCOL_CONTRACT_ERROR);
                return;
            }
            consumed += written.bytes_written;
        }
        if (!good_) {
            return;
        }
        offset_ += buffered_;
        buffered_ = 0;
    }

    DistributedSieveWorkerWorkPackageFileOpsV1& ops_;
    NativeHandle writer_;
    std::uint64_t maximum_offset_ = 0;
    std::uint64_t creator_process_id_ = 0;
    std::array<std::byte, WRITE_BUFFER_BYTES> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t offset_ = 0;
    bool good_ = true;
    Diagnostic diagnostic_;
};

struct FileExecutionResult final {
    std::optional<DistributedSieveWorkerWorkPackageFileWitnessV1> witness;
    NativeHandle retained_reader = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
    Diagnostic diagnostic;
};

class FileExecution final {
public:
    explicit FileExecution(DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept : ops_(ops) {}

    [[nodiscard]] FileExecutionResult fail(Diagnostic failure) noexcept {
        failure.named_may_remain = failure.named_may_remain || named_may_remain_;
        close_for_cleanup(writer_, failure);
        close_for_cleanup(reader_, failure);
        return {std::nullopt, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE, failure};
    }

    [[nodiscard]] Diagnostic close_writer_at_boundary() noexcept {
        if (writer_ == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE) {
            return make_diagnostic(FileStatus::ops_contract_violation, PROTOCOL_CONTRACT_ERROR, {},
                                   named_may_remain_);
        }
        const NativeHandle released =
            std::exchange(writer_, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE);
        const auto closed = ops_.close_handle(released);
        if (!valid_success(closed)) {
            return make_diagnostic(FileStatus::close_failed,
                                   closed.native_error != 0 ? closed.native_error : EIO, {},
                                   named_may_remain_);
        }
        return make_diagnostic(FileStatus::ready, 0, {}, named_may_remain_);
    }

    [[nodiscard]] FileExecutionResult
    succeed(DistributedSieveWorkerWorkPackageFileWitnessV1 witness, bool retain_reader) noexcept {
        if (!retain_reader) {
            Diagnostic closed = make_diagnostic(FileStatus::ready, 0, {}, named_may_remain_);
            close_for_cleanup(reader_, closed);
            if (closed.secondary_close_error != 0) {
                closed.status = FileStatus::close_failed;
                closed.native_error = closed.secondary_close_error;
                closed.secondary_close_error = 0;
                return {std::nullopt, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE, closed};
            }
            return {std::move(witness), DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE, {}};
        }
        const NativeHandle retained =
            std::exchange(reader_, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE);
        return {std::move(witness), retained, {}};
    }

    [[nodiscard]] NativeHandle writer() const noexcept {
        return writer_;
    }

    [[nodiscard]] NativeHandle reader() const noexcept {
        return reader_;
    }

    void set_writer(NativeHandle writer) noexcept {
        writer_ = writer;
        named_may_remain_ = true;
    }

    void mark_name_may_remain() noexcept {
        named_may_remain_ = true;
    }

    void set_reader(NativeHandle reader) noexcept {
        reader_ = reader;
    }

    void prove_name_absent() noexcept {
        named_may_remain_ = false;
    }

private:
    void close_for_cleanup(NativeHandle& handle, Diagnostic& primary) noexcept {
        if (handle == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE) {
            return;
        }
        const NativeHandle released =
            std::exchange(handle, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE);
        // Never retry close: after EINTR POSIX leaves ownership unspecified.
        const auto closed = ops_.close_handle(released);
        if (!valid_success(closed) && primary.secondary_close_error == 0) {
            primary.secondary_close_error = closed.native_error != 0 ? closed.native_error : EIO;
        }
    }

    DistributedSieveWorkerWorkPackageFileOpsV1& ops_;
    NativeHandle writer_ = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
    NativeHandle reader_ = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
    bool named_may_remain_ = false;
};

[[nodiscard]] Diagnostic
process_check(const DistributedSieveWorkerWorkPackageFileRequestV1& request,
              const DistributedSieveWorkerWorkPackageFileOpsV1& ops,
              bool named_may_remain = false) noexcept {
    if (request.creator_process_id == 0 || ops.current_process_id() != request.creator_process_id) {
        return make_diagnostic(FileStatus::invalid_request, PROCESS_ID_MISMATCH_ERROR, {},
                               named_may_remain);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, named_may_remain);
}

[[nodiscard]] Diagnostic process_check(const ResidueInspectionRequest& request,
                                       const DistributedSieveWorkerWorkPackageFileOpsV1& ops,
                                       bool named_may_remain = false) noexcept {
    if (request.observer_process_id == 0 ||
        ops.current_process_id() != request.observer_process_id) {
        return make_diagnostic(FileStatus::invalid_request, PROCESS_ID_MISMATCH_ERROR, {},
                               named_may_remain);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, named_may_remain);
}

[[nodiscard]] Diagnostic process_check(const ResidueReconciliationRequest& request,
                                       const DistributedSieveWorkerWorkPackageFileOpsV1& ops,
                                       bool named_may_remain = false) noexcept {
    if (request.reconciler_process_id == 0 ||
        ops.current_process_id() != request.reconciler_process_id) {
        return make_diagnostic(FileStatus::invalid_request, PROCESS_ID_MISMATCH_ERROR, {},
                               named_may_remain);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, named_may_remain);
}

[[nodiscard]] Diagnostic operation_diagnostic(const OperationResult& operation,
                                              FileStatus failure_status,
                                              bool named_may_remain = true) noexcept {
    if (valid_success(operation)) {
        return make_diagnostic(FileStatus::ready, 0, {}, named_may_remain);
    }
    if (operation.state == OperationState::succeeded ||
        operation.state == OperationState::interrupted) {
        return make_diagnostic(FileStatus::ops_contract_violation, PROTOCOL_CONTRACT_ERROR, {},
                               named_may_remain);
    }
    if (operation.state == OperationState::unsupported) {
        return make_diagnostic(FileStatus::platform_unavailable, operation.native_error, {},
                               named_may_remain);
    }
    return make_diagnostic(failure_status, operation.native_error, {}, named_may_remain);
}

template <typename Request>
[[nodiscard]] Diagnostic validate_directory(const Request& request,
                                            DistributedSieveWorkerWorkPackageFileOpsV1& ops,
                                            bool named_may_remain = false) noexcept {
    if (auto process = process_check(request, ops, named_may_remain); !process) {
        return process;
    }
    const auto observed = retry_interrupted(
        [&]() noexcept { return ops.stat_handle(request.borrowed_attempt_directory_handle); });
    if (!valid_success(observed.operation)) {
        return operation_diagnostic(observed.operation, FileStatus::invalid_request,
                                    named_may_remain);
    }
    const Metadata& metadata = observed.metadata;
    if (metadata.kind != DistributedSieveWorkerWorkPackageObjectKind::directory ||
        metadata.identity != request.expected_directory_identity ||
        metadata.owner_user_id != ops.effective_user_id() ||
        metadata.mode != PRIVATE_DIRECTORY_MODE) {
        return make_diagnostic(FileStatus::invalid_request, PROTOCOL_CONTRACT_ERROR, {},
                               named_may_remain);
    }
    const auto acl = retry_interrupted([&]() noexcept {
        return ops.inspect_acl(request.borrowed_attempt_directory_handle, true);
    });
    if (!valid_success(acl.operation)) {
        return operation_diagnostic(acl.operation, FileStatus::invalid_request, named_may_remain);
    }
    if (acl.has_extended_acl) {
        return make_diagnostic(FileStatus::invalid_request, EACCES, {}, named_may_remain);
    }
    return process_check(request, ops, named_may_remain);
}

[[nodiscard]] constexpr bool
valid_file_metadata(const Metadata& metadata, const NativeIdentityV1& expected_identity,
                    std::uint32_t expected_mode, std::uint64_t expected_links,
                    std::uint64_t expected_size, std::uint64_t expected_user) noexcept {
    return metadata.kind == DistributedSieveWorkerWorkPackageObjectKind::regular_file &&
           metadata.identity == expected_identity && metadata.owner_user_id == expected_user &&
           metadata.mode == expected_mode && metadata.link_count == expected_links &&
           metadata.size == expected_size;
}

[[nodiscard]] Diagnostic
validate_handle_acl(NativeHandle handle, DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto acl = retry_interrupted([&]() noexcept { return ops.inspect_acl(handle, false); });
    if (!valid_success(acl.operation)) {
        return operation_diagnostic(acl.operation, FileStatus::namespace_conflict);
    }
    if (acl.has_extended_acl) {
        return make_diagnostic(FileStatus::namespace_conflict, EACCES, {}, true);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, true);
}

[[nodiscard]] std::pair<std::optional<Metadata>, Diagnostic>
stat_handle_checked(NativeHandle handle, DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto observed = retry_interrupted([&]() noexcept { return ops.stat_handle(handle); });
    if (!valid_success(observed.operation)) {
        return {std::nullopt,
                operation_diagnostic(observed.operation, FileStatus::namespace_conflict)};
    }
    return {observed.metadata, make_diagnostic(FileStatus::ready, 0, {}, true)};
}

[[nodiscard]] std::pair<std::optional<Metadata>, Diagnostic>
stat_named_checked(NativeHandle directory,
                   DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto observed =
        retry_interrupted([&]() noexcept { return ops.stat_at_nofollow(directory); });
    if (!valid_success(observed.operation)) {
        return {std::nullopt,
                operation_diagnostic(observed.operation, FileStatus::namespace_conflict)};
    }
    return {observed.metadata, make_diagnostic(FileStatus::ready, 0, {}, true)};
}

[[nodiscard]] std::pair<std::optional<NativeIdentityV1>, Diagnostic>
validate_writer_and_name(const DistributedSieveWorkerWorkPackageFileRequestV1& request,
                         NativeHandle writer, std::uint32_t expected_mode,
                         std::uint64_t expected_size,
                         DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto [writer_metadata, writer_diagnostic] = stat_handle_checked(writer, ops);
    if (!writer_metadata.has_value()) {
        return {std::nullopt, writer_diagnostic};
    }
    const auto [named_metadata, named_diagnostic] =
        stat_named_checked(request.borrowed_attempt_directory_handle, ops);
    if (!named_metadata.has_value()) {
        return {std::nullopt, named_diagnostic};
    }
    const NativeIdentityV1 identity = writer_metadata->identity;
    const std::uint64_t expected_user = ops.effective_user_id();
    if (!valid_file_metadata(*writer_metadata, identity, expected_mode, 1, expected_size,
                             expected_user) ||
        !valid_file_metadata(*named_metadata, identity, expected_mode, 1, expected_size,
                             expected_user)) {
        return {std::nullopt,
                make_diagnostic(FileStatus::namespace_conflict, PROTOCOL_CONTRACT_ERROR, {}, true)};
    }
    if (auto acl = validate_handle_acl(writer, ops); !acl) {
        return {std::nullopt, acl};
    }
    return {identity, make_diagnostic(FileStatus::ready, 0, {}, true)};
}

[[nodiscard]] Diagnostic
validate_triple_file_binding(const DistributedSieveWorkerWorkPackageFileRequestV1& request,
                             NativeHandle writer, NativeHandle reader,
                             const NativeIdentityV1& expected_identity, std::uint64_t expected_size,
                             DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto [writer_metadata, writer_diagnostic] = stat_handle_checked(writer, ops);
    if (!writer_metadata.has_value()) {
        return writer_diagnostic;
    }
    const auto [reader_metadata, reader_diagnostic] = stat_handle_checked(reader, ops);
    if (!reader_metadata.has_value()) {
        return reader_diagnostic;
    }
    const auto [named_metadata, named_diagnostic] =
        stat_named_checked(request.borrowed_attempt_directory_handle, ops);
    if (!named_metadata.has_value()) {
        return named_diagnostic;
    }
    const std::uint64_t expected_user = ops.effective_user_id();
    if (!valid_file_metadata(*writer_metadata, expected_identity, SEALED_FILE_MODE, 1,
                             expected_size, expected_user) ||
        !valid_file_metadata(*reader_metadata, expected_identity, SEALED_FILE_MODE, 1,
                             expected_size, expected_user) ||
        !valid_file_metadata(*named_metadata, expected_identity, SEALED_FILE_MODE, 1, expected_size,
                             expected_user)) {
        return make_diagnostic(FileStatus::namespace_conflict, PROTOCOL_CONTRACT_ERROR, {}, true);
    }
    if (auto acl = validate_handle_acl(writer, ops); !acl) {
        return acl;
    }
    return validate_handle_acl(reader, ops);
}

template <typename Request>
[[nodiscard]] Diagnostic
validate_reader_and_name(const Request& request, NativeHandle reader,
                         const NativeIdentityV1& expected_identity, std::uint64_t expected_size,
                         DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto [reader_metadata, reader_diagnostic] = stat_handle_checked(reader, ops);
    if (!reader_metadata.has_value()) {
        return reader_diagnostic;
    }
    const auto [named_metadata, named_diagnostic] =
        stat_named_checked(request.borrowed_attempt_directory_handle, ops);
    if (!named_metadata.has_value()) {
        return named_diagnostic;
    }
    const std::uint64_t expected_user = ops.effective_user_id();
    if (!valid_file_metadata(*reader_metadata, expected_identity, SEALED_FILE_MODE, 1,
                             expected_size, expected_user) ||
        !valid_file_metadata(*named_metadata, expected_identity, SEALED_FILE_MODE, 1, expected_size,
                             expected_user)) {
        return make_diagnostic(FileStatus::namespace_conflict, PROTOCOL_CONTRACT_ERROR, {}, true);
    }
    return validate_handle_acl(reader, ops);
}

[[nodiscard]] Diagnostic
verify_name_missing(NativeHandle directory,
                    DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto observed =
        retry_interrupted([&]() noexcept { return ops.stat_at_nofollow(directory); });
    if (observed.operation.state == OperationState::missing) {
        return {};
    }
    if (valid_success(observed.operation)) {
        return make_diagnostic(FileStatus::namespace_conflict, EEXIST, {}, true);
    }
    return operation_diagnostic(observed.operation, FileStatus::namespace_conflict, true);
}

[[nodiscard]] Diagnostic
validate_unlinked_reader(NativeHandle reader, const NativeIdentityV1& expected_identity,
                         std::uint64_t expected_size,
                         DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto [metadata, observed] = stat_handle_checked(reader, ops);
    if (!metadata.has_value()) {
        return observed;
    }
    if (!valid_file_metadata(*metadata, expected_identity, SEALED_FILE_MODE, 0, expected_size,
                             ops.effective_user_id())) {
        return make_diagnostic(FileStatus::namespace_conflict, PROTOCOL_CONTRACT_ERROR);
    }
    auto acl = validate_handle_acl(reader, ops);
    acl.named_may_remain = false;
    return acl;
}

[[nodiscard]] Diagnostic
validate_reader_policy(NativeHandle reader,
                       DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto policy =
        retry_interrupted([&]() noexcept { return ops.inspect_read_descriptor(reader); });
    if (!valid_success(policy.operation)) {
        return operation_diagnostic(policy.operation, FileStatus::namespace_conflict);
    }
    if (!policy.read_only || !policy.close_on_exec) {
        return make_diagnostic(FileStatus::namespace_conflict, PROTOCOL_CONTRACT_ERROR, {}, true);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, true);
}

[[nodiscard]] Diagnostic validate_decoded_package(NativeHandle reader, std::uint64_t total_bytes,
                                                  const PackageWitness& expected_witness,
                                                  DistributedSieveWorkerWorkPackageFileOpsV1& ops,
                                                  bool named_may_remain) noexcept {
    auto decoded =
        retry_interrupted([&]() noexcept { return ops.decode_exact(reader, total_bytes); });
    if (!valid_success(decoded.operation)) {
        return operation_diagnostic(decoded.operation, FileStatus::decode_failed, named_may_remain);
    }
    if (!decoded.decoded) {
        return make_diagnostic(FileStatus::decode_failed, 0, decoded.decoded.status,
                               named_may_remain);
    }
    if (!same_package_witness(decoded.decoded.package->witness, expected_witness)) {
        return make_diagnostic(FileStatus::decode_failed, PROTOCOL_CONTRACT_ERROR, {},
                               named_may_remain);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, named_may_remain);
}

[[nodiscard]] Diagnostic
set_mode_checked(NativeHandle handle, std::uint32_t mode,
                 DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    const auto changed =
        retry_interrupted_operation([&]() noexcept { return ops.set_mode(handle, mode); });
    return operation_diagnostic(changed, FileStatus::publication_failed);
}

[[nodiscard]] Diagnostic sync_checked(NativeHandle handle,
                                      DistributedSieveWorkerWorkPackageFileOpsV1& ops,
                                      bool named_may_remain) noexcept {
    const auto synchronized =
        retry_interrupted_operation([&]() noexcept { return ops.sync_handle(handle); });
    return operation_diagnostic(synchronized, FileStatus::durability_failed, named_may_remain);
}

#if defined(__APPLE__) || defined(__linux__)

static_assert(std::is_integral_v<uid_t>);
static_assert(!std::numeric_limits<uid_t>::is_signed);
static_assert(std::numeric_limits<uid_t>::digits <= std::numeric_limits<std::uint64_t>::digits);

[[nodiscard]] bool to_descriptor(NativeHandle handle, int& descriptor) noexcept {
    if (handle < 0 || handle > static_cast<NativeHandle>(std::numeric_limits<int>::max())) {
        return false;
    }
    descriptor = static_cast<int>(handle);
    return true;
}

[[nodiscard]] OperationResult posix_operation_failure(int error) noexcept {
    if (error == EINTR) {
        return operation_failure(OperationState::interrupted, error);
    }
    if (error == ENOENT) {
        return operation_failure(OperationState::missing, error);
    }
    if (error == EEXIST) {
        return operation_failure(OperationState::already_exists, error);
    }
    if (error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS) {
        return operation_failure(OperationState::unsupported, error);
    }
    return operation_failure(OperationState::failed, error);
}

[[nodiscard]] Metadata metadata_from_stat(const struct stat& value) noexcept {
    DistributedSieveWorkerWorkPackageObjectKind kind =
        DistributedSieveWorkerWorkPackageObjectKind::other;
    if (S_ISREG(value.st_mode)) {
        kind = DistributedSieveWorkerWorkPackageObjectKind::regular_file;
    } else if (S_ISDIR(value.st_mode)) {
        kind = DistributedSieveWorkerWorkPackageObjectKind::directory;
    }
    return {
        .kind = kind,
        .identity =
            {
                .volume = static_cast<std::uint64_t>(value.st_dev),
                .object = static_cast<std::uint64_t>(value.st_ino),
                .generation = 0,
            },
        .owner_user_id = static_cast<std::uint64_t>(value.st_uid),
        .mode = static_cast<std::uint32_t>(value.st_mode & static_cast<mode_t>(07777)),
        .link_count = static_cast<std::uint64_t>(value.st_nlink),
        .size = value.st_size < 0 ? 0 : static_cast<std::uint64_t>(value.st_size),
    };
}

class ProductionDistributedSieveWorkerWorkPackageFileOps final
    : public DistributedSieveWorkerWorkPackageFileOpsV1 {
public:
    [[nodiscard]] std::uint64_t current_process_id() const noexcept override {
        return static_cast<std::uint64_t>(::getpid());
    }

    [[nodiscard]] std::uint64_t effective_user_id() const noexcept override {
        return static_cast<std::uint64_t>(::geteuid());
    }

    [[nodiscard]] std::uint64_t maximum_file_offset() const noexcept override {
        return static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    }

    [[nodiscard]] MetadataResult stat_handle(NativeHandle handle) noexcept override {
        int descriptor = -1;
        if (!to_descriptor(handle, descriptor)) {
            return {operation_failure(OperationState::failed, EBADF), {}};
        }
        struct stat value {};
        if (::fstat(descriptor, &value) != 0) {
            return {posix_operation_failure(errno), {}};
        }
        if (value.st_size < 0) {
            return {operation_failure(OperationState::failed, FILE_EXTENT_OVERFLOW_ERROR), {}};
        }
        return {operation_success(), metadata_from_stat(value)};
    }

    [[nodiscard]] MetadataResult stat_at_nofollow(NativeHandle directory_handle) noexcept override {
        int directory = -1;
        if (!to_descriptor(directory_handle, directory)) {
            return {operation_failure(OperationState::failed, EBADF), {}};
        }
        struct stat value {};
        constexpr auto leaf = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1;
        if (::fstatat(directory, leaf.data(), &value, AT_SYMLINK_NOFOLLOW) != 0) {
            return {posix_operation_failure(errno), {}};
        }
        if (value.st_size < 0) {
            return {operation_failure(OperationState::failed, FILE_EXTENT_OVERFLOW_ERROR), {}};
        }
        return {operation_success(), metadata_from_stat(value)};
    }

    [[nodiscard]] DistributedSieveWorkerWorkPackageAclResultV1
    inspect_acl(NativeHandle handle, bool directory) noexcept override {
        int descriptor = -1;
        if (!to_descriptor(handle, descriptor)) {
            return {operation_failure(OperationState::failed, EBADF), false};
        }
#if defined(__APPLE__)
        (void)directory;
        errno = 0;
        acl_t acl = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
        if (acl == nullptr) {
            const int saved_error = errno;
            if (saved_error == ENOENT) {
                return {operation_success(), false};
            }
            return {posix_operation_failure(saved_error), false};
        }
        (void)::acl_free(acl);
        return {operation_success(), true};
#elif defined(__linux__)
        const auto inspect_name = [descriptor](const char* name) noexcept {
            const ssize_t size = ::fgetxattr(descriptor, name, nullptr, 0);
            if (size >= 0) {
                return DistributedSieveWorkerWorkPackageAclResultV1{operation_success(), true};
            }
            const int saved_error = errno;
            if (saved_error == ENODATA) {
                return DistributedSieveWorkerWorkPackageAclResultV1{operation_success(), false};
            }
            return DistributedSieveWorkerWorkPackageAclResultV1{
                posix_operation_failure(saved_error), false};
        };
        auto access = inspect_name("system.posix_acl_access");
        if (!access.operation || access.has_extended_acl || !directory) {
            return access;
        }
        return inspect_name("system.posix_acl_default");
#endif
    }

    [[nodiscard]] DistributedSieveWorkerWorkPackageOpenResult
    open_exclusive_at(NativeHandle directory_handle) noexcept override {
        int directory = -1;
        if (!to_descriptor(directory_handle, directory)) {
            return {operation_failure(OperationState::failed, EBADF),
                    DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE};
        }
        constexpr auto leaf = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1;
        const int descriptor =
            ::openat(directory, leaf.data(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                     static_cast<mode_t>(WRITABLE_FILE_MODE));
        if (descriptor < 0) {
            return {posix_operation_failure(errno),
                    DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE};
        }
        return {operation_success(), static_cast<NativeHandle>(descriptor)};
    }

    [[nodiscard]] OperationResult set_mode(NativeHandle handle,
                                           std::uint32_t mode) noexcept override {
        int descriptor = -1;
        if (!to_descriptor(handle, descriptor)) {
            return operation_failure(OperationState::failed, EBADF);
        }
        if (::fchmod(descriptor, static_cast<mode_t>(mode)) != 0) {
            return posix_operation_failure(errno);
        }
        return operation_success();
    }

    [[nodiscard]] DistributedSieveWorkerWorkPackageWriteResult
    pwrite_some(NativeHandle handle, const std::byte* data, std::size_t size,
                std::uint64_t offset) noexcept override {
        int descriptor = -1;
        if (!to_descriptor(handle, descriptor)) {
            return {operation_failure(OperationState::failed, EBADF), 0};
        }
        if (data == nullptr || size == 0 ||
            size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()) ||
            offset > maximum_file_offset()) {
            return {operation_failure(OperationState::failed, EINVAL), 0};
        }
        const ssize_t written = ::pwrite(descriptor, data, size, static_cast<off_t>(offset));
        if (written < 0) {
            return {posix_operation_failure(errno), 0};
        }
        return {operation_success(), static_cast<std::size_t>(written)};
    }

    [[nodiscard]] OperationResult sync_handle(NativeHandle handle) noexcept override {
        int descriptor = -1;
        if (!to_descriptor(handle, descriptor)) {
            return operation_failure(OperationState::failed, EBADF);
        }
#if defined(__APPLE__)
        if (::fcntl(descriptor, F_FULLFSYNC) != 0) {
#else
        if (::fsync(descriptor) != 0) {
#endif
            return posix_operation_failure(errno);
        }
        return operation_success();
    }

    [[nodiscard]] DistributedSieveWorkerWorkPackageOpenResult
    open_readonly_at(NativeHandle directory_handle) noexcept override {
        int directory = -1;
        if (!to_descriptor(directory_handle, directory)) {
            return {operation_failure(OperationState::failed, EBADF),
                    DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE};
        }
        constexpr auto leaf = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1;
        const int descriptor =
            ::openat(directory, leaf.data(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor < 0) {
            return {posix_operation_failure(errno),
                    DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE};
        }
        return {operation_success(), static_cast<NativeHandle>(descriptor)};
    }

    [[nodiscard]] DistributedSieveWorkerWorkPackageDescriptorPolicyResultV1
    inspect_read_descriptor(NativeHandle handle) noexcept override {
        int descriptor = -1;
        if (!to_descriptor(handle, descriptor)) {
            return {operation_failure(OperationState::failed, EBADF), false, false};
        }
        const int status_flags = ::fcntl(descriptor, F_GETFL);
        if (status_flags < 0) {
            return {posix_operation_failure(errno), false, false};
        }
        const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
        if (descriptor_flags < 0) {
            return {posix_operation_failure(errno), false, false};
        }
        return {
            operation_success(),
            (status_flags & O_ACCMODE) == O_RDONLY,
            (descriptor_flags & FD_CLOEXEC) != 0,
        };
    }

    [[nodiscard]] DistributedSieveWorkerWorkPackageDecodeResultV1
    decode_exact(NativeHandle handle, std::uint64_t total_bytes) noexcept override {
        int descriptor = -1;
        if (!to_descriptor(handle, descriptor)) {
            return {operation_failure(OperationState::failed, EBADF), {}};
        }
        if (total_bytes == 0 ||
            total_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            total_bytes > maximum_file_offset()) {
            return {operation_failure(OperationState::failed, FILE_EXTENT_OVERFLOW_ERROR), {}};
        }
        const auto length = static_cast<std::size_t>(total_bytes);
        std::vector<std::byte> bytes;
        try {
            bytes.resize(length);
        } catch (const std::bad_alloc&) {
            return {operation_failure(OperationState::failed, ENOMEM), {}};
        } catch (...) {
            return {operation_failure(OperationState::failed, ENOMEM), {}};
        }

        std::size_t offset = 0;
        while (offset < length) {
            const std::size_t request = std::min(
                length - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t count =
                ::pread(descriptor, bytes.data() + static_cast<std::ptrdiff_t>(offset), request,
                        static_cast<off_t>(offset));
            if (count < 0) {
                const auto failure = posix_operation_failure(errno);
                if (failure.state == OperationState::interrupted) {
                    continue;
                }
                return {failure, {}};
            }
            if (count == 0) {
                return {operation_failure(OperationState::failed, EIO), {}};
            }
            offset += static_cast<std::size_t>(count);
        }

        std::byte extra{};
        ssize_t extra_count = -1;
        do {
            extra_count = ::pread(descriptor, &extra, 1, static_cast<off_t>(total_bytes));
        } while (extra_count < 0 && errno == EINTR);
        if (extra_count < 0) {
            return {posix_operation_failure(errno), {}};
        }
        if (extra_count != 0) {
            return {operation_failure(OperationState::failed, EFBIG), {}};
        }

        return {
            operation_success(),
            distributed_sieve_work_package_codec_detail::decode_distributed_sieve_work_package_v1(
                bytes),
        };
    }

    [[nodiscard]] OperationResult close_handle(NativeHandle handle) noexcept override {
        int descriptor = -1;
        if (!to_descriptor(handle, descriptor)) {
            return operation_failure(OperationState::failed, EBADF);
        }
        if (::close(descriptor) != 0) {
            return posix_operation_failure(errno);
        }
        return operation_success();
    }

    [[nodiscard]] OperationResult unlink_at(NativeHandle directory_handle) noexcept override {
        int directory = -1;
        if (!to_descriptor(directory_handle, directory)) {
            return operation_failure(OperationState::failed, EBADF);
        }
        constexpr auto leaf = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1;
        if (::unlinkat(directory, leaf.data(), 0) != 0) {
            return posix_operation_failure(errno);
        }
        return operation_success();
    }
};

#endif

[[nodiscard]] Diagnostic classify_open_failure(const OperationResult& operation) noexcept {
    if (operation.state == OperationState::already_exists) {
        return make_diagnostic(FileStatus::namespace_conflict, operation.native_error);
    }
    if (operation.state == OperationState::unsupported) {
        return make_diagnostic(FileStatus::platform_unavailable, operation.native_error);
    }
    if (is_resource_error(operation.native_error)) {
        return make_diagnostic(FileStatus::resource_exhausted, operation.native_error);
    }
    return operation_diagnostic(operation, FileStatus::publication_failed, false);
}

[[nodiscard]] Diagnostic
classify_protocol_failure(DistributedSieveProtocolStatus protocol,
                          FileStatus fallback = FileStatus::invalid_request,
                          bool named_may_remain = false) noexcept {
    const FileStatus status = protocol.error == DistributedSieveProtocolError::resource_exhausted
                                  ? FileStatus::resource_exhausted
                                  : fallback;
    return make_diagnostic(status, 0, protocol, named_may_remain);
}

[[nodiscard]] Diagnostic classify_residue_open_failure(const OperationResult& operation) noexcept {
    if (operation.state == OperationState::unsupported) {
        return make_diagnostic(FileStatus::platform_unavailable, operation.native_error);
    }
    if (is_resource_error(operation.native_error)) {
        return make_diagnostic(FileStatus::resource_exhausted, operation.native_error);
    }
    return operation_diagnostic(operation, FileStatus::namespace_conflict, true);
}

[[nodiscard]] Diagnostic validate_residue_metadata(const Metadata& metadata,
                                                   const NativeIdentityV1& expected_identity,
                                                   std::uint64_t expected_extent,
                                                   std::uint64_t expected_user) noexcept {
    if (!valid_file_metadata(metadata, expected_identity, SEALED_FILE_MODE, 1, expected_extent,
                             expected_user)) {
        return make_diagnostic(FileStatus::namespace_conflict, PROTOCOL_CONTRACT_ERROR, {}, true);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, true);
}

template <typename Request>
[[nodiscard]] Diagnostic
validate_residue_reader_and_name(const Request& request, NativeHandle reader,
                                 const NativeIdentityV1& expected_identity,
                                 std::uint64_t expected_extent, std::uint64_t expected_user,
                                 DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    if (ops.effective_user_id() != expected_user) {
        return make_diagnostic(FileStatus::invalid_request, EACCES, {}, true);
    }
    const auto [reader_metadata, reader_diagnostic] = stat_handle_checked(reader, ops);
    if (!reader_metadata.has_value()) {
        return reader_diagnostic;
    }
    const auto [named_metadata, named_diagnostic] =
        stat_named_checked(request.borrowed_attempt_directory_handle, ops);
    if (!named_metadata.has_value()) {
        return named_diagnostic;
    }
    if (auto held = validate_residue_metadata(*reader_metadata, expected_identity, expected_extent,
                                              expected_user);
        !held) {
        return held;
    }
    if (auto named = validate_residue_metadata(*named_metadata, expected_identity, expected_extent,
                                               expected_user);
        !named) {
        return named;
    }
    return validate_handle_acl(reader, ops);
}

class ResidueInspectionExecution final {
public:
    explicit ResidueInspectionExecution(DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept
        : ops_(ops) {}

    void set_reader(NativeHandle reader) noexcept {
        reader_ = reader;
    }

    [[nodiscard]] NativeHandle reader() const noexcept {
        return reader_;
    }

    [[nodiscard]] DistributedSieveWorkerWorkPackageResidueInspectionResultV1
    fail(Diagnostic failure) noexcept {
        close_reader(failure);
        return {std::nullopt, failure};
    }

    [[nodiscard]] DistributedSieveWorkerWorkPackageResidueInspectionResultV1
    succeed(ResidueWitness witness) noexcept {
        Diagnostic closed = make_diagnostic(FileStatus::ready, 0, {}, true);
        close_reader(closed);
        if (!closed) {
            return {std::nullopt, closed};
        }
        return {std::move(witness), {}};
    }

private:
    void close_reader(Diagnostic& primary) noexcept {
        if (reader_ == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE) {
            return;
        }
        const NativeHandle released =
            std::exchange(reader_, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE);
        // Never retry close: after EINTR POSIX leaves ownership unspecified.
        const auto closed = ops_.close_handle(released);
        if (valid_success(closed)) {
            return;
        }
        const int native_error = closed.native_error != 0 ? closed.native_error : EIO;
        if (primary.status == FileStatus::ready) {
            primary = make_diagnostic(FileStatus::close_failed, native_error, {}, true);
        } else if (primary.secondary_close_error == 0) {
            primary.secondary_close_error = native_error;
        }
    }

    DistributedSieveWorkerWorkPackageFileOpsV1& ops_;
    NativeHandle reader_ = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
};

[[nodiscard]] DistributedSieveWorkerWorkPackageResidueInspectionResultV1
run_residue_inspection(const ResidueInspectionRequest& request,
                       DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    ResidueInspectionExecution execution(ops);
    if (request.borrowed_attempt_directory_handle ==
            DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE ||
        request.borrowed_attempt_directory_handle < 0) {
        return execution.fail(make_diagnostic(FileStatus::invalid_request, EBADF));
    }
    if (auto directory = validate_directory(request, ops); !directory) {
        return execution.fail(directory);
    }

    const std::uint64_t owner_user_id = ops.effective_user_id();
    const auto [initial_metadata, initial_diagnostic] =
        stat_named_checked(request.borrowed_attempt_directory_handle, ops);
    if (!initial_metadata.has_value()) {
        Diagnostic failure = initial_diagnostic;
        if (failure.native_error == ENOENT) {
            failure.named_may_remain = false;
        }
        return execution.fail(failure);
    }
    const std::uint64_t extent = initial_metadata->size;
    if (initial_metadata->kind != DistributedSieveWorkerWorkPackageObjectKind::regular_file ||
        initial_metadata->owner_user_id != owner_user_id ||
        initial_metadata->mode != SEALED_FILE_MODE || initial_metadata->link_count != 1 ||
        extent == 0 ||
        extent > distributed_sieve_work_package_codec_detail::
                     DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1 ||
        extent > ops.maximum_file_offset() ||
        extent > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return execution.fail(
            make_diagnostic(FileStatus::namespace_conflict, PROTOCOL_CONTRACT_ERROR, {}, true));
    }
    const NativeIdentityV1 file_identity = initial_metadata->identity;

    if (auto directory = validate_directory(request, ops, true); !directory) {
        return execution.fail(directory);
    }
    if (ops.effective_user_id() != owner_user_id) {
        return execution.fail(make_diagnostic(FileStatus::invalid_request, EACCES, {}, true));
    }
    const auto opened = retry_interrupted(
        [&]() noexcept { return ops.open_readonly_at(request.borrowed_attempt_directory_handle); });
    if (!valid_success(opened.operation)) {
        return execution.fail(classify_residue_open_failure(opened.operation));
    }
    if (opened.handle == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE ||
        opened.handle < 0 || opened.handle == request.borrowed_attempt_directory_handle) {
        return execution.fail(
            make_diagnostic(FileStatus::ops_contract_violation, PROTOCOL_CONTRACT_ERROR, {}, true));
    }
    execution.set_reader(opened.handle);

    if (auto policy = validate_reader_policy(execution.reader(), ops); !policy) {
        return execution.fail(policy);
    }
    if (auto directory = validate_directory(request, ops, true); !directory) {
        return execution.fail(directory);
    }
    if (auto binding = validate_residue_reader_and_name(request, execution.reader(), file_identity,
                                                        extent, owner_user_id, ops);
        !binding) {
        return execution.fail(binding);
    }
    if (auto process = process_check(request, ops, true); !process) {
        return execution.fail(process);
    }

    auto decoded =
        retry_interrupted([&]() noexcept { return ops.decode_exact(execution.reader(), extent); });
    if (!valid_success(decoded.operation)) {
        const FileStatus status = is_resource_error(decoded.operation.native_error)
                                      ? FileStatus::resource_exhausted
                                      : FileStatus::decode_failed;
        return execution.fail(operation_diagnostic(decoded.operation, status, true));
    }
    if (!decoded.decoded) {
        return execution.fail(
            classify_protocol_failure(decoded.decoded.status, FileStatus::decode_failed, true));
    }
    if (decoded.decoded.package->witness.total_bytes != extent ||
        decoded.decoded.package->witness.total_bytes >
            distributed_sieve_work_package_codec_detail::
                DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1) {
        return execution.fail(
            make_diagnostic(FileStatus::decode_failed, PROTOCOL_CONTRACT_ERROR, {}, true));
    }
    const auto work_digest = distributed_sieve_work_digest(decoded.decoded.package->identity);
    if (!work_digest) {
        return execution.fail(
            classify_protocol_failure(work_digest.status, FileStatus::decode_failed, true));
    }
    if (work_digest.digest->bytes != decoded.decoded.package->witness.work_sha256.bytes) {
        return execution.fail(
            make_diagnostic(FileStatus::decode_failed, PROTOCOL_CONTRACT_ERROR, {}, true));
    }

    // The decode seam is a hostile callback boundary. Recheck the process,
    // exact held directory, descriptor policy, held inode, and named inode
    // before allowing any data witness to escape.
    if (auto process = process_check(request, ops, true); !process) {
        return execution.fail(process);
    }
    if (auto directory = validate_directory(request, ops, true); !directory) {
        return execution.fail(directory);
    }
    if (auto policy = validate_reader_policy(execution.reader(), ops); !policy) {
        return execution.fail(policy);
    }
    if (auto binding = validate_residue_reader_and_name(request, execution.reader(), file_identity,
                                                        extent, owner_user_id, ops);
        !binding) {
        return execution.fail(binding);
    }
    if (auto process = process_check(request, ops, true); !process) {
        return execution.fail(process);
    }

    ResidueWitness witness{
        .identity = std::move(decoded.decoded.package->identity),
        .package = decoded.decoded.package->witness,
        .file_identity = file_identity,
        .file_extent = extent,
        .owner_user_id = owner_user_id,
    };
    return execution.succeed(std::move(witness));
}

[[nodiscard]] Diagnostic validate_reconciliation_process_and_user(
    const ResidueReconciliationRequest& request, std::uint64_t expected_user,
    DistributedSieveWorkerWorkPackageFileOpsV1& ops, bool named_may_remain) noexcept {
    if (auto process = process_check(request, ops, named_may_remain); !process) {
        return process;
    }
    if (ops.effective_user_id() != expected_user) {
        return make_diagnostic(FileStatus::invalid_request, EACCES, {}, named_may_remain);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, named_may_remain);
}

[[nodiscard]] Diagnostic
validate_expected_residue_witness(const ResidueWitness& expected,
                                  DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    if (expected.owner_user_id != ops.effective_user_id() || expected.file_extent == 0 ||
        expected.file_identity == NativeIdentityV1{} ||
        expected.file_extent != expected.package.total_bytes ||
        expected.file_extent > distributed_sieve_work_package_codec_detail::
                                   DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1 ||
        expected.file_extent > ops.maximum_file_offset() ||
        expected.file_extent >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return make_diagnostic(FileStatus::invalid_request, PROTOCOL_CONTRACT_ERROR, {}, true);
    }

    const auto prepared =
        distributed_sieve_work_package_codec_detail::prepare_distributed_sieve_work_package_v1(
            expected.identity);
    if (!prepared) {
        return classify_protocol_failure(prepared.status, FileStatus::invalid_request, true);
    }
    if (prepared.prepared->body_bytes != expected.package.body_bytes ||
        prepared.prepared->total_bytes != expected.package.total_bytes ||
        prepared.prepared->work_sha256.bytes != expected.package.work_sha256.bytes) {
        return make_diagnostic(FileStatus::invalid_request, PROTOCOL_CONTRACT_ERROR, {}, true);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, true);
}

[[nodiscard]] Diagnostic
decode_and_validate_exact_residue(NativeHandle reader, const ResidueWitness& expected,
                                  DistributedSieveWorkerWorkPackageFileOpsV1& ops,
                                  bool named_may_remain) noexcept {
    auto decoded = retry_interrupted(
        [&]() noexcept { return ops.decode_exact(reader, expected.file_extent); });
    if (!valid_success(decoded.operation)) {
        const FileStatus status = is_resource_error(decoded.operation.native_error)
                                      ? FileStatus::resource_exhausted
                                      : FileStatus::decode_failed;
        return operation_diagnostic(decoded.operation, status, named_may_remain);
    }
    if (!decoded.decoded) {
        return classify_protocol_failure(decoded.decoded.status, FileStatus::decode_failed,
                                         named_may_remain);
    }
    if (decoded.decoded.package->witness.total_bytes != expected.file_extent) {
        return make_diagnostic(FileStatus::decode_failed, PROTOCOL_CONTRACT_ERROR, {},
                               named_may_remain);
    }

    ResidueWitness observed{
        .identity = std::move(decoded.decoded.package->identity),
        .package = decoded.decoded.package->witness,
        .file_identity = expected.file_identity,
        .file_extent = expected.file_extent,
        .owner_user_id = expected.owner_user_id,
    };
    if (!(observed == expected)) {
        return make_diagnostic(FileStatus::decode_failed, PROTOCOL_CONTRACT_ERROR, {},
                               named_may_remain);
    }
    return make_diagnostic(FileStatus::ready, 0, {}, named_may_remain);
}

[[nodiscard]] Diagnostic
validate_named_residue_state(const ResidueReconciliationRequest& request, NativeHandle reader,
                             const ResidueWitness& expected, std::uint64_t expected_user,
                             DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    if (auto process = validate_reconciliation_process_and_user(request, expected_user, ops, true);
        !process) {
        return process;
    }
    if (auto directory = validate_directory(request, ops, true); !directory) {
        return directory;
    }
    if (auto policy = validate_reader_policy(reader, ops); !policy) {
        return policy;
    }
    if (auto binding = validate_residue_reader_and_name(request, reader, expected.file_identity,
                                                        expected.file_extent, expected_user, ops);
        !binding) {
        return binding;
    }
    if (auto decoded = decode_and_validate_exact_residue(reader, expected, ops, true); !decoded) {
        return decoded;
    }
    if (auto process = validate_reconciliation_process_and_user(request, expected_user, ops, true);
        !process) {
        return process;
    }
    if (auto directory = validate_directory(request, ops, true); !directory) {
        return directory;
    }
    if (auto policy = validate_reader_policy(reader, ops); !policy) {
        return policy;
    }
    return validate_residue_reader_and_name(request, reader, expected.file_identity,
                                            expected.file_extent, expected_user, ops);
}

[[nodiscard]] Diagnostic
validate_unlinked_residue_state(const ResidueReconciliationRequest& request, NativeHandle reader,
                                const ResidueWitness& expected, std::uint64_t expected_user,
                                DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    if (auto process = validate_reconciliation_process_and_user(request, expected_user, ops, false);
        !process) {
        return process;
    }
    if (auto directory = validate_directory(request, ops, false); !directory) {
        return directory;
    }
    if (auto absent = verify_name_missing(request.borrowed_attempt_directory_handle, ops);
        !absent) {
        return absent;
    }
    if (auto policy = validate_reader_policy(reader, ops); !policy) {
        return policy;
    }
    if (auto retained =
            validate_unlinked_reader(reader, expected.file_identity, expected.file_extent, ops);
        !retained) {
        return retained;
    }
    if (auto decoded = decode_and_validate_exact_residue(reader, expected, ops, false); !decoded) {
        return decoded;
    }
    if (auto process = validate_reconciliation_process_and_user(request, expected_user, ops, false);
        !process) {
        return process;
    }
    if (auto directory = validate_directory(request, ops, false); !directory) {
        return directory;
    }
    if (auto absent = verify_name_missing(request.borrowed_attempt_directory_handle, ops);
        !absent) {
        return absent;
    }
    if (auto policy = validate_reader_policy(reader, ops); !policy) {
        policy.named_may_remain = false;
        return policy;
    }
    auto retained =
        validate_unlinked_reader(reader, expected.file_identity, expected.file_extent, ops);
    retained.named_may_remain = false;
    return retained;
}

[[nodiscard]] Diagnostic
validate_absent_residue_state(const ResidueReconciliationRequest& request,
                              std::uint64_t expected_user,
                              DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    if (auto process = validate_reconciliation_process_and_user(request, expected_user, ops, false);
        !process) {
        return process;
    }
    if (auto directory = validate_directory(request, ops, false); !directory) {
        return directory;
    }
    if (auto absent = verify_name_missing(request.borrowed_attempt_directory_handle, ops);
        !absent) {
        return absent;
    }
    if (auto process = validate_reconciliation_process_and_user(request, expected_user, ops, false);
        !process) {
        return process;
    }
    if (auto directory = validate_directory(request, ops, false); !directory) {
        return directory;
    }
    auto absent = verify_name_missing(request.borrowed_attempt_directory_handle, ops);
    if (absent) {
        absent.named_may_remain = false;
    }
    return absent;
}

class ResidueReconciliationExecution final {
public:
    explicit ResidueReconciliationExecution(
        DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept
        : ops_(ops) {}

    void set_reader(NativeHandle reader) noexcept {
        reader_ = reader;
    }

    [[nodiscard]] NativeHandle reader() const noexcept {
        return reader_;
    }

    [[nodiscard]] ResidueReconciliationResult
    fail(Diagnostic failure,
         std::optional<ResidueReconciliationFaultPoint> fault_point = std::nullopt) noexcept {
        close_reader(failure);
        return {std::nullopt, failure, fault_point};
    }

    [[nodiscard]] ResidueReconciliationResult interrupt(ResidueReconciliationFaultPoint fault_point,
                                                        bool named_may_remain) noexcept {
        return fail(make_diagnostic(FileStatus::interrupted, EINTR, {}, named_may_remain),
                    fault_point);
    }

    [[nodiscard]] ResidueReconciliationResult
    succeed(ResidueReconciliationDisposition disposition) noexcept {
        Diagnostic closed = make_diagnostic(FileStatus::ready);
        close_reader(closed);
        if (!closed) {
            return {std::nullopt, closed, std::nullopt};
        }
        return {disposition, {}, std::nullopt};
    }

private:
    void close_reader(Diagnostic& primary) noexcept {
        if (reader_ == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE) {
            return;
        }
        const NativeHandle released =
            std::exchange(reader_, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE);
        // Never retry close: after EINTR POSIX leaves ownership unspecified.
        const auto closed = ops_.close_handle(released);
        if (valid_success(closed)) {
            return;
        }
        const int native_error = closed.native_error != 0 ? closed.native_error : EIO;
        if (primary.status == FileStatus::ready) {
            primary = make_diagnostic(FileStatus::close_failed, native_error, {},
                                      primary.named_may_remain);
        } else if (primary.secondary_close_error == 0) {
            primary.secondary_close_error = native_error;
        }
    }

    DistributedSieveWorkerWorkPackageFileOpsV1& ops_;
    NativeHandle reader_ = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
};

template <typename ValidateCurrentState>
[[nodiscard]] std::pair<Diagnostic, bool>
synchronize_reconciled_directory(const ResidueReconciliationRequest& request,
                                 const ResidueReconciliationTestHooks& hooks,
                                 DistributedSieveWorkerWorkPackageFileOpsV1& ops,
                                 ValidateCurrentState&& validate_current_state) noexcept {
    if (auto predecessor = validate_current_state(); !predecessor) {
        return {predecessor, false};
    }
    const bool inject_failure = hooks.fail_before_directory_sync != nullptr &&
                                hooks.fail_before_directory_sync(hooks.context);
    if (auto confirmed = validate_current_state(); !confirmed) {
        return {confirmed, false};
    }
    if (auto process = process_check(request, ops, false); !process) {
        return {process, false};
    }

    const auto synchronized = retry_interrupted_operation(
        [&]() noexcept { return ops.sync_handle(request.borrowed_attempt_directory_handle); });
    const Diagnostic synchronization =
        operation_diagnostic(synchronized, FileStatus::durability_failed, false);

    // A sync result is not authoritative by itself. Always adjudicate the
    // visible successor through the same complete proof, including when a
    // selected successful sync is deliberately surfaced as a failure.
    if (auto successor = validate_current_state(); !successor) {
        return {successor, false};
    }
    if (!synchronization) {
        return {synchronization, false};
    }
    if (inject_failure) {
        return {make_diagnostic(FileStatus::durability_failed, EIO, {}, false), false};
    }
    return {make_diagnostic(FileStatus::ready, 0, {}, false), true};
}

[[nodiscard]] ResidueReconciliationResult
run_residue_reconciliation(const ResidueReconciliationRequest& request,
                           DistributedSieveWorkerWorkPackageFileOpsV1& ops,
                           const ResidueReconciliationTestHooks& hooks) noexcept {
    ResidueReconciliationExecution execution(ops);
    if (request.borrowed_attempt_directory_handle ==
            DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE ||
        request.borrowed_attempt_directory_handle < 0) {
        return execution.fail(make_diagnostic(FileStatus::invalid_request, EBADF));
    }
    if (hooks.stop_after.has_value() &&
        *hooks.stop_after != ResidueReconciliationFaultPoint::after_name_unlinked &&
        *hooks.stop_after != ResidueReconciliationFaultPoint::after_directory_durable) {
        return execution.fail(
            make_diagnostic(FileStatus::invalid_request, PROTOCOL_CONTRACT_ERROR));
    }
    if (auto process = process_check(request, ops); !process) {
        return execution.fail(process);
    }

    const std::uint64_t expected_user = ops.effective_user_id();
    std::optional<ResidueWitness> expected;
    if (request.expected_residue != nullptr) {
        try {
            expected.emplace(*request.expected_residue);
        } catch (const std::bad_alloc&) {
            return execution.fail(
                make_diagnostic(FileStatus::resource_exhausted, ENOMEM, {}, true));
        } catch (...) {
            return execution.fail(
                make_diagnostic(FileStatus::unexpected_failure, ENOMEM, {}, true));
        }
        if (auto witness = validate_expected_residue_witness(*expected, ops); !witness) {
            return execution.fail(witness);
        }
    }
    if (auto directory = validate_directory(request, ops, expected.has_value()); !directory) {
        return execution.fail(directory);
    }
    if (auto process = validate_reconciliation_process_and_user(request, expected_user, ops,
                                                                expected.has_value());
        !process) {
        return execution.fail(process);
    }

    if (!expected.has_value()) {
        if (hooks.stop_after == ResidueReconciliationFaultPoint::after_name_unlinked) {
            return execution.fail(
                make_diagnostic(FileStatus::invalid_request, PROTOCOL_CONTRACT_ERROR));
        }
        const auto validate_absence = [&]() noexcept {
            return validate_absent_residue_state(request, expected_user, ops);
        };
        if (auto first = validate_absence(); !first) {
            return execution.fail(first);
        }

        const auto [synchronization, durable] =
            synchronize_reconciled_directory(request, hooks, ops, validate_absence);
        if (!durable) {
            return execution.fail(synchronization);
        }
        if (hooks.stop_after == ResidueReconciliationFaultPoint::after_directory_durable) {
            return execution.interrupt(ResidueReconciliationFaultPoint::after_directory_durable,
                                       false);
        }
        if (hooks.after_directory_durable != nullptr) {
            hooks.after_directory_durable(hooks.context);
        }
        if (auto successor = validate_absence(); !successor) {
            return execution.fail(successor);
        }
        return execution.succeed(ResidueReconciliationDisposition::confirmed_absent);
    }

    const auto opened = retry_interrupted(
        [&]() noexcept { return ops.open_readonly_at(request.borrowed_attempt_directory_handle); });
    if (!valid_success(opened.operation)) {
        return execution.fail(classify_residue_open_failure(opened.operation));
    }
    if (opened.handle == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE ||
        opened.handle < 0 || opened.handle == request.borrowed_attempt_directory_handle) {
        return execution.fail(
            make_diagnostic(FileStatus::ops_contract_violation, PROTOCOL_CONTRACT_ERROR, {}, true));
    }
    execution.set_reader(opened.handle);

    const auto validate_named = [&]() noexcept {
        return validate_named_residue_state(request, execution.reader(), *expected, expected_user,
                                            ops);
    };
    if (auto predecessor = validate_named(); !predecessor) {
        return execution.fail(predecessor);
    }
    if (hooks.before_unlink != nullptr) {
        hooks.before_unlink(hooks.context);
    }
    if (auto predecessor = validate_named(); !predecessor) {
        return execution.fail(predecessor);
    }

    const auto validate_unlinked = [&]() noexcept {
        return validate_unlinked_residue_state(request, execution.reader(), *expected,
                                               expected_user, ops);
    };

    // unlinkat is destructive and therefore strictly single-shot. In
    // particular, an EINTR report may hide a completed unlink; retrying could
    // delete a successor installed at the same fixed name.
    const auto unlinked = ops.unlink_at(request.borrowed_attempt_directory_handle);
    if (!valid_success(unlinked)) {
        Diagnostic failure =
            unlinked.state == OperationState::interrupted
                ? make_diagnostic(FileStatus::interrupted,
                                  unlinked.native_error != 0 ? unlinked.native_error : EINTR, {},
                                  true)
                : operation_diagnostic(unlinked, FileStatus::publication_failed, true);
        auto successor = validate_unlinked();
        if (successor) {
            failure.named_may_remain = false;
            return execution.fail(failure);
        }
        if (const auto predecessor = validate_named(); predecessor) {
            failure.named_may_remain = true;
            return execution.fail(failure);
        }
        // Neither authenticated state survived the ambiguous destructive
        // boundary. Preserve the namespace and return the successor proof's
        // stronger diagnostic without attempting another unlink.
        successor.named_may_remain = true;
        return execution.fail(successor);
    }

    if (auto successor = validate_unlinked(); !successor) {
        return execution.fail(successor);
    }
    if (hooks.stop_after == ResidueReconciliationFaultPoint::after_name_unlinked) {
        return execution.interrupt(ResidueReconciliationFaultPoint::after_name_unlinked, false);
    }

    const auto [synchronization, durable] =
        synchronize_reconciled_directory(request, hooks, ops, validate_unlinked);
    if (!durable) {
        return execution.fail(synchronization);
    }
    if (hooks.stop_after == ResidueReconciliationFaultPoint::after_directory_durable) {
        return execution.interrupt(ResidueReconciliationFaultPoint::after_directory_durable, false);
    }
    if (hooks.after_directory_durable != nullptr) {
        hooks.after_directory_durable(hooks.context);
    }
    if (auto successor = validate_unlinked(); !successor) {
        return execution.fail(successor);
    }
    return execution.succeed(ResidueReconciliationDisposition::removed);
}

[[nodiscard]] FileExecutionResult
run_file_creation(const DistributedSieveWorkerWorkPackageFileRequestV1& request,
                  const DistributedSieveWorkIdentityV1& identity,
                  DistributedSieveWorkerWorkPackageFileOpsV1& ops, bool retain_reader) noexcept {
    FileExecution execution(ops);

    if (request.borrowed_attempt_directory_handle ==
            DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE ||
        request.borrowed_attempt_directory_handle < 0) {
        return execution.fail(make_diagnostic(FileStatus::invalid_request, EBADF));
    }
    if (auto directory = validate_directory(request, ops); !directory) {
        return execution.fail(directory);
    }

    const auto prepared =
        distributed_sieve_work_package_codec_detail::prepare_distributed_sieve_work_package_v1(
            identity);
    if (!prepared) {
        return execution.fail(classify_protocol_failure(prepared.status));
    }
    if (prepared.prepared->total_bytes == 0 ||
        prepared.prepared->total_bytes > distributed_sieve_work_package_codec_detail::
                                             DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1 ||
        prepared.prepared->total_bytes > ops.maximum_file_offset()) {
        return execution.fail(
            make_diagnostic(FileStatus::invalid_request, FILE_EXTENT_OVERFLOW_ERROR));
    }
    if (auto process = process_check(request, ops); !process) {
        return execution.fail(process);
    }

    const auto opened_writer = retry_interrupted([&]() noexcept {
        return ops.open_exclusive_at(request.borrowed_attempt_directory_handle);
    });
    if (!valid_success(opened_writer.operation)) {
        return execution.fail(classify_open_failure(opened_writer.operation));
    }
    execution.mark_name_may_remain();
    if (opened_writer.handle == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE ||
        opened_writer.handle < 0 ||
        opened_writer.handle == request.borrowed_attempt_directory_handle) {
        return execution.fail(
            make_diagnostic(FileStatus::ops_contract_violation, PROTOCOL_CONTRACT_ERROR, {}, true));
    }
    execution.set_writer(opened_writer.handle);

    // Normalize away any process umask before accepting the created inode.
    if (auto changed = set_mode_checked(execution.writer(), WRITABLE_FILE_MODE, ops); !changed) {
        return execution.fail(changed);
    }
    const auto [initial_identity, initial_diagnostic] =
        validate_writer_and_name(request, execution.writer(), WRITABLE_FILE_MODE, 0, ops);
    if (!initial_identity.has_value()) {
        return execution.fail(initial_diagnostic);
    }

    BufferedPwriteSink sink(ops, execution.writer(), ops.maximum_file_offset(),
                            request.creator_process_id);
    const auto emitted =
        distributed_sieve_work_package_codec_detail::emit_distributed_sieve_work_package_v1(
            *prepared.prepared, identity, sink);
    if (!emitted) {
        if (!sink.good()) {
            return execution.fail(sink.diagnostic());
        }
        return execution.fail(
            classify_protocol_failure(emitted.status, FileStatus::publication_failed, true));
    }
    if (!sink.finish()) {
        return execution.fail(sink.diagnostic());
    }
    if (!emitted.witness.has_value() || emitted.bytes_emitted != prepared.prepared->total_bytes ||
        sink.bytes_written() != prepared.prepared->total_bytes ||
        emitted.witness->total_bytes != prepared.prepared->total_bytes) {
        return execution.fail(
            make_diagnostic(FileStatus::ops_contract_violation, PROTOCOL_CONTRACT_ERROR, {}, true));
    }

    if (auto changed = set_mode_checked(execution.writer(), SEALED_FILE_MODE, ops); !changed) {
        return execution.fail(changed);
    }
    if (auto synchronized = sync_checked(execution.writer(), ops, true); !synchronized) {
        return execution.fail(synchronized);
    }
    if (auto process = process_check(request, ops, true); !process) {
        return execution.fail(process);
    }

    const auto opened_reader = retry_interrupted(
        [&]() noexcept { return ops.open_readonly_at(request.borrowed_attempt_directory_handle); });
    if (!valid_success(opened_reader.operation)) {
        return execution.fail(
            operation_diagnostic(opened_reader.operation, FileStatus::namespace_conflict, true));
    }
    if (opened_reader.handle == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE ||
        opened_reader.handle < 0 ||
        opened_reader.handle == request.borrowed_attempt_directory_handle ||
        opened_reader.handle == execution.writer()) {
        return execution.fail(
            make_diagnostic(FileStatus::ops_contract_violation, PROTOCOL_CONTRACT_ERROR, {}, true));
    }
    execution.set_reader(opened_reader.handle);

    if (auto policy = validate_reader_policy(execution.reader(), ops); !policy) {
        return execution.fail(policy);
    }
    if (auto binding =
            validate_triple_file_binding(request, execution.writer(), execution.reader(),
                                         *initial_identity, prepared.prepared->total_bytes, ops);
        !binding) {
        return execution.fail(binding);
    }
    if (auto decoded = validate_decoded_package(execution.reader(), prepared.prepared->total_bytes,
                                                *emitted.witness, ops, true);
        !decoded) {
        return execution.fail(decoded);
    }

    if (auto closed = execution.close_writer_at_boundary(); !closed) {
        return execution.fail(closed);
    }

    if (auto directory = validate_directory(request, ops, true); !directory) {
        return execution.fail(directory);
    }
    if (auto binding = validate_reader_and_name(request, execution.reader(), *initial_identity,
                                                prepared.prepared->total_bytes, ops);
        !binding) {
        return execution.fail(binding);
    }

    const auto unlinked = retry_interrupted_operation(
        [&]() noexcept { return ops.unlink_at(request.borrowed_attempt_directory_handle); });
    if (!valid_success(unlinked)) {
        return execution.fail(operation_diagnostic(unlinked, FileStatus::publication_failed, true));
    }
    if (auto absent = verify_name_missing(request.borrowed_attempt_directory_handle, ops);
        !absent) {
        return execution.fail(absent);
    }
    execution.prove_name_absent();

    if (auto retained = validate_unlinked_reader(execution.reader(), *initial_identity,
                                                 prepared.prepared->total_bytes, ops);
        !retained) {
        return execution.fail(retained);
    }
    if (auto decoded = validate_decoded_package(execution.reader(), prepared.prepared->total_bytes,
                                                *emitted.witness, ops, false);
        !decoded) {
        return execution.fail(decoded);
    }

    if (auto synchronized = sync_checked(request.borrowed_attempt_directory_handle, ops, false);
        !synchronized) {
        return execution.fail(synchronized);
    }
    if (auto directory = validate_directory(request, ops, false); !directory) {
        return execution.fail(directory);
    }
    if (auto absent = verify_name_missing(request.borrowed_attempt_directory_handle, ops);
        !absent) {
        return execution.fail(absent);
    }
    execution.prove_name_absent();
    if (auto retained = validate_unlinked_reader(execution.reader(), *initial_identity,
                                                 prepared.prepared->total_bytes, ops);
        !retained) {
        return execution.fail(retained);
    }
    if (auto process = process_check(request, ops, false); !process) {
        return execution.fail(process);
    }

    DistributedSieveWorkerWorkPackageFileWitnessV1 witness{
        .package = *emitted.witness,
        .file_identity = *initial_identity,
        .file_extent = prepared.prepared->total_bytes,
        .creator_process_id = request.creator_process_id,
    };
    return execution.succeed(std::move(witness), retain_reader);
}

[[nodiscard]] Diagnostic revalidate_retained_reader(
    NativeHandle reader, const DistributedSieveWorkerWorkPackageFileWitnessV1& witness,
    std::uint64_t creator_process_id, DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    if (reader == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE || reader < 0 ||
        creator_process_id == 0 || ops.current_process_id() != creator_process_id ||
        witness.creator_process_id != creator_process_id ||
        witness.file_extent != witness.package.total_bytes) {
        return make_diagnostic(FileStatus::invalid_request, PROCESS_ID_MISMATCH_ERROR);
    }
    if (auto policy = validate_reader_policy(reader, ops); !policy) {
        policy.named_may_remain = false;
        return policy;
    }
    if (auto retained =
            validate_unlinked_reader(reader, witness.file_identity, witness.file_extent, ops);
        !retained) {
        return retained;
    }
    if (auto decoded =
            validate_decoded_package(reader, witness.file_extent, witness.package, ops, false);
        !decoded) {
        return decoded;
    }
    if (ops.current_process_id() != creator_process_id) {
        return make_diagnostic(FileStatus::invalid_request, PROCESS_ID_MISMATCH_ERROR);
    }
    return {};
}

} // namespace

bool operator==(const DistributedSieveWorkerWorkPackageResidueWitnessV1& left,
                const DistributedSieveWorkerWorkPackageResidueWitnessV1& right) noexcept {
    if (!same_package_witness(left.package, right.package) ||
        left.file_identity != right.file_identity || left.file_extent != right.file_extent ||
        left.owner_user_id != right.owner_user_id) {
        return false;
    }
    const auto left_digest = distributed_sieve_work_digest(left.identity);
    const auto right_digest = distributed_sieve_work_digest(right.identity);
    return left_digest && right_digest && left_digest.digest->bytes == right_digest.digest->bytes &&
           left_digest.digest->bytes == left.package.work_sha256.bytes &&
           right_digest.digest->bytes == right.package.work_sha256.bytes;
}

DistributedSieveWorkerWorkPackageFileV1::DistributedSieveWorkerWorkPackageFileV1(
    NativeHandle retained_reader, DistributedSieveWorkerWorkPackageFileWitnessV1 witness,
    std::uint64_t creator_process_id) noexcept
    : retained_reader_(retained_reader), witness_(std::move(witness)),
      creator_process_id_(creator_process_id) {}

DistributedSieveWorkerWorkPackageFileV1::~DistributedSieveWorkerWorkPackageFileV1() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    if (retained_reader_ != DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE) {
        ProductionDistributedSieveWorkerWorkPackageFileOps ops;
        const NativeHandle released =
            std::exchange(retained_reader_, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE);
        // A descriptor close is always single-shot, including EINTR.
        (void)ops.close_handle(released);
    }
#else
    retained_reader_ = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
#endif
}

DistributedSieveWorkerWorkPackageFileV1::DistributedSieveWorkerWorkPackageFileV1(
    DistributedSieveWorkerWorkPackageFileV1&& other) noexcept
    : retained_reader_(std::exchange(other.retained_reader_,
                                     DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE)),
      witness_(std::move(other.witness_)),
      creator_process_id_(std::exchange(other.creator_process_id_, 0)) {}

bool DistributedSieveWorkerWorkPackageFileV1::owned_by_current_process() const noexcept {
#if defined(__APPLE__) || defined(__linux__)
    return retained_reader_ != DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE &&
           creator_process_id_ != 0 &&
           creator_process_id_ == static_cast<std::uint64_t>(::getpid());
#else
    return false;
#endif
}

const DistributedSieveWorkerWorkPackageFileWitnessV1&
DistributedSieveWorkerWorkPackageFileV1::witness() const noexcept {
    return witness_;
}

DistributedSieveWorkerWorkPackageFileDiagnostic
DistributedSieveWorkerWorkPackageFileV1::revalidate() const noexcept {
#if defined(__APPLE__) || defined(__linux__)
    ProductionDistributedSieveWorkerWorkPackageFileOps ops;
    return revalidate_retained_reader(retained_reader_, witness_, creator_process_id_, ops);
#else
    return make_diagnostic(FileStatus::platform_unavailable, PLATFORM_UNAVAILABLE_ERROR);
#endif
}

DistributedSieveWorkerWorkPackageFileResultV1 create_distributed_sieve_worker_work_package_file_v1(
    const DistributedSieveWorkerWorkPackageFileRequestV1& request,
    const DistributedSieveWorkIdentityV1& identity) noexcept {
#if defined(__APPLE__) || defined(__linux__)
    ProductionDistributedSieveWorkerWorkPackageFileOps ops;
    auto executed = run_file_creation(request, identity, ops, true);
    if (!executed.witness.has_value() ||
        executed.retained_reader == DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE ||
        !executed.diagnostic) {
        return {std::nullopt, executed.diagnostic};
    }
    DistributedSieveWorkerWorkPackageFileV1 file(
        executed.retained_reader, std::move(*executed.witness), request.creator_process_id);
    return {
        std::optional<DistributedSieveWorkerWorkPackageFileV1>(std::move(file)),
        {},
    };
#else
    (void)request;
    (void)identity;
    return {
        std::nullopt,
        make_diagnostic(FileStatus::platform_unavailable, PLATFORM_UNAVAILABLE_ERROR),
    };
#endif
}

DistributedSieveWorkerWorkPackageFileWithOpsResultV1
create_distributed_sieve_worker_work_package_file_v1_with_ops(
    const DistributedSieveWorkerWorkPackageFileRequestV1& request,
    const DistributedSieveWorkIdentityV1& identity,
    DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    auto executed = run_file_creation(request, identity, ops, false);
    return {std::move(executed.witness), executed.diagnostic};
}

DistributedSieveWorkerWorkPackageResidueInspectionResultV1
inspect_distributed_sieve_worker_work_package_residue_v1(
    const DistributedSieveWorkerWorkPackageResidueInspectionRequestV1& request) noexcept {
#if defined(__APPLE__) || defined(__linux__)
    ProductionDistributedSieveWorkerWorkPackageFileOps ops;
    return run_residue_inspection(request, ops);
#else
    (void)request;
    return {
        std::nullopt,
        make_diagnostic(FileStatus::platform_unavailable, PLATFORM_UNAVAILABLE_ERROR),
    };
#endif
}

DistributedSieveWorkerWorkPackageResidueInspectionResultV1
inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
    const DistributedSieveWorkerWorkPackageResidueInspectionRequestV1& request,
    DistributedSieveWorkerWorkPackageFileOpsV1& ops) noexcept {
    return run_residue_inspection(request, ops);
}

DistributedSieveWorkerWorkPackageResidueReconciliationResultV1
reconcile_distributed_sieve_worker_work_package_residue_v1(
    const DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1& request,
    const DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1& hooks) noexcept {
#if defined(__APPLE__) || defined(__linux__)
    ProductionDistributedSieveWorkerWorkPackageFileOps ops;
    return run_residue_reconciliation(request, ops, hooks);
#else
    (void)request;
    (void)hooks;
    return {
        std::nullopt,
        make_diagnostic(FileStatus::platform_unavailable, PLATFORM_UNAVAILABLE_ERROR),
        std::nullopt,
    };
#endif
}

DistributedSieveWorkerWorkPackageResidueReconciliationResultV1
reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
    const DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1& request,
    DistributedSieveWorkerWorkPackageFileOpsV1& ops,
    const DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1& hooks) noexcept {
    return run_residue_reconciliation(request, ops, hooks);
}

} // namespace gnfs::sieve::distributed_sieve_worker_work_package_file_detail
