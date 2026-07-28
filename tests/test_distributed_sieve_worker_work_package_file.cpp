#include "distributed_sieve_work_package_codec_internal.hpp"
#include "distributed_sieve_worker_work_package_file_internal.hpp"

#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace sieve = gnfs::sieve;
namespace package_codec = gnfs::sieve::distributed_sieve_work_package_codec_detail;
namespace package_file = gnfs::sieve::distributed_sieve_worker_work_package_file_detail;

using Identity = sieve::DistributedSieveWorkIdentityV1;
using FileDiagnostic = package_file::DistributedSieveWorkerWorkPackageFileDiagnostic;
using FileMetadata = package_file::DistributedSieveWorkerWorkPackageMetadataV1;
using FileMetadataResult = package_file::DistributedSieveWorkerWorkPackageMetadataResultV1;
using FileOperationResult = package_file::DistributedSieveWorkerWorkPackageOperationResult;
using FileOperationState = package_file::DistributedSieveWorkerWorkPackageOperationState;
using NativeHandle = package_file::DistributedSieveWorkerWorkPackageNativeHandle;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error(std::string("CHECK failed: " #condition " at ") + __FILE__ +  \
                                     ":" + std::to_string(__LINE__));                              \
        }                                                                                          \
    } while (false)

[[nodiscard]] constexpr std::uint64_t binary64_bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

struct PolicySpec final {
    sieve::ExecutionPolicyScalarKindV1 kind;
    std::uint64_t bits;
};

constexpr std::array<PolicySpec, sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
    POLICY_SPECS{{
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.5)},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 123},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.125)},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 12},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 8},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 64},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 128},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
    }};

[[nodiscard]] sieve::DistributedSieveExecutionPolicyV1 make_execution_policy() {
    sieve::DistributedSieveExecutionPolicyV1 policy;
    policy.settings.reserve(POLICY_SPECS.size());
    for (std::size_t index = 0; index < POLICY_SPECS.size(); ++index) {
        policy.settings.push_back({
            static_cast<sieve::ExecutionPolicyKeyV1>(index + 1U),
            POLICY_SPECS[index].kind,
            POLICY_SPECS[index].bits,
        });
    }
    return policy;
}

[[nodiscard]] Identity make_identity() {
    Identity identity;
    identity.polynomial.n.decimal = "1000036000099";
    identity.polynomial.m.decimal = "10001";
    identity.polynomial.degree = 2;
    identity.polynomial.coefficients = {{"-5"}, {"3"}, {"1"}};
    identity.polynomial.skewness_ieee754_bits = binary64_bits(1.25);

    identity.factor_base.rational_bound = 100;
    identity.factor_base.algebraic_bound = 200;
    identity.factor_base.large_prime_bound = 10'000;
    identity.factor_base.log_scale = 16;
    identity.factor_base.rational = {{2, 16}, {5, 25}};
    identity.factor_base.algebraic = {
        {7, 1, 37, 1},   {11, 4, 55, 2},
        {211, 3, 61, 1}, {223, std::numeric_limits<std::uint32_t>::max(), 67, 1},
        {227, 5, 71, 1},
    };
    identity.factor_base.sieve_algebraic_count = 2;

    identity.sieve = {16, 50, 51, 10'000, true, false};
    identity.region = {-100, 100, 1, 50};
    identity.cofactor = {10'000, true, false, true, 20};
    identity.original_sq_bounds = {0, 5, 100, 1000};
    identity.effective_sq_bounds = {
        2,
        5,
        0,
        std::numeric_limits<std::uint32_t>::max(),
    };

    identity.distributed.worker_count = 2;
    identity.distributed.chunks = {
        {0, 2, 3, "chunk_0"},
        {1, 3, 5, "chunk_1"},
    };
    identity.distributed.sq_cap_per_worker = 10;
    identity.distributed.relation_cap_per_worker = 100;
    identity.distributed.max_worker_attempts = 2;
    identity.distributed.max_merge_build_attempts = 3;
    identity.distributed.max_consumption_attempts = 4;
    identity.execution_policy = make_execution_policy();
    identity.semantic_versions = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(sieve::validate_distributed_sieve_work_identity(identity));
    return identity;
}

[[nodiscard]] Identity make_large_identity() {
    auto identity = make_identity();
    constexpr std::uint32_t coefficient_count = 40;
    identity.polynomial.degree = coefficient_count - 1U;
    identity.polynomial.coefficients.clear();
    identity.polynomial.coefficients.reserve(coefficient_count);
    for (std::uint32_t index = 0; index < coefficient_count; ++index) {
        std::string decimal(4096, static_cast<char>('1' + index % 8U));
        identity.polynomial.coefficients.push_back({std::move(decimal)});
    }
    CHECK(sieve::validate_distributed_sieve_work_identity(identity));
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);
    CHECK(encoded.package->bytes.size() > 2U * 64U * 1024U);
    return identity;
}

[[nodiscard]] constexpr FileOperationResult operation(FileOperationState state,
                                                      int native_error = 0) noexcept {
    return {.state = state, .native_error = native_error};
}

[[nodiscard]] constexpr FileOperationResult succeeded() noexcept {
    return operation(FileOperationState::succeeded);
}

[[nodiscard]] constexpr FileOperationResult interrupted(int native_error = EINTR) noexcept {
    return operation(FileOperationState::interrupted, native_error);
}

[[nodiscard]] constexpr FileOperationResult failed(int native_error = EIO) noexcept {
    return operation(FileOperationState::failed, native_error);
}

enum class CriticalFileEvent : std::uint8_t {
    writer_sync,
    named_decode,
    writer_close,
    unlink_name,
    anonymous_decode,
    directory_sync,
    final_directory_stat,
    final_directory_acl,
    final_name_check,
    final_reader_stat,
    final_reader_acl,
};

class ScriptedFileOps final : public package_file::DistributedSieveWorkerWorkPackageFileOpsV1 {
public:
    static constexpr NativeHandle DIRECTORY_HANDLE = 11;
    static constexpr NativeHandle WRITER_HANDLE = 17;
    static constexpr NativeHandle READER_HANDLE = 19;
    static constexpr sieve::NativeIdentityV1 DIRECTORY_IDENTITY{
        .volume = 101,
        .object = 202,
        .generation = 0,
    };
    static constexpr sieve::NativeIdentityV1 FILE_IDENTITY{
        .volume = 101,
        .object = 303,
        .generation = 0,
    };

    std::uint64_t process_id = 1234;
    std::uint64_t user_id = 501;
    std::uint64_t maximum_offset = std::numeric_limits<std::uint64_t>::max();

    FileMetadata directory_metadata{
        .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::directory,
        .identity = DIRECTORY_IDENTITY,
        .owner_user_id = user_id,
        .mode = 0700,
        .link_count = 2,
        .size = 0,
    };
    bool named_exists = false;
    bool writer_open = false;
    bool reader_open = false;
    bool recreate_name_after_first_missing_observation = false;
    bool recreated_foreign_name = false;
    bool directory_sync_observed = false;
    bool file_has_extended_acl = false;
    bool replace_named_identity_after_decode = false;
    bool drift_directory_identity_after_decode = false;
    bool drift_process_after_decode = false;
    bool drift_process_after_unlink_result = false;
    bool retain_link_after_unlink = false;
    bool unlink_then_interrupt = false;
    std::uint32_t file_mode = 0600;
    std::uint64_t file_link_count = 0;
    std::optional<std::uint64_t> file_owner_user_id_override;
    std::optional<sieve::NativeIdentityV1> named_identity_override;
    std::vector<std::byte> bytes;

    std::deque<FileMetadataResult> stat_handle_script;
    std::deque<FileMetadataResult> stat_at_script;
    std::deque<package_file::DistributedSieveWorkerWorkPackageAclResultV1> acl_script;
    std::deque<package_file::DistributedSieveWorkerWorkPackageOpenResult> exclusive_open_script;
    std::deque<FileOperationResult> set_mode_script;
    std::deque<package_file::DistributedSieveWorkerWorkPackageWriteResult> write_script;
    std::deque<FileOperationResult> sync_script;
    std::deque<package_file::DistributedSieveWorkerWorkPackageOpenResult> readonly_open_script;
    std::deque<package_file::DistributedSieveWorkerWorkPackageDescriptorPolicyResultV1>
        descriptor_policy_script;
    std::deque<package_file::DistributedSieveWorkerWorkPackageDecodeResultV1> decode_script;
    std::deque<FileOperationResult> writer_close_script;
    std::deque<FileOperationResult> reader_close_script;
    std::deque<FileOperationResult> unlink_script;

    std::size_t stat_handle_calls = 0;
    std::size_t stat_at_calls = 0;
    std::size_t acl_calls = 0;
    std::size_t exclusive_open_calls = 0;
    std::size_t set_mode_calls = 0;
    std::size_t write_calls = 0;
    std::size_t sync_calls = 0;
    std::size_t readonly_open_calls = 0;
    std::size_t descriptor_policy_calls = 0;
    std::size_t decode_calls = 0;
    std::size_t writer_close_calls = 0;
    std::size_t reader_close_calls = 0;
    std::size_t unlink_calls = 0;
    std::vector<std::uint64_t> write_offsets;
    std::vector<std::size_t> write_request_sizes;
    std::vector<std::uint32_t> modes_during_writes;
    std::vector<NativeHandle> synced_handles;
    std::vector<std::uint64_t> decode_link_counts;
    std::vector<bool> decode_named_states;
    std::vector<CriticalFileEvent> critical_events;
    std::size_t missing_name_observations = 0;

    [[nodiscard]] std::uint64_t current_process_id() const noexcept override {
        return process_id;
    }

    [[nodiscard]] std::uint64_t effective_user_id() const noexcept override {
        return user_id;
    }

    [[nodiscard]] std::uint64_t maximum_file_offset() const noexcept override {
        return maximum_offset;
    }

    [[nodiscard]] FileMetadataResult stat_handle(NativeHandle handle) noexcept override {
        ++stat_handle_calls;
        if (directory_sync_observed) {
            if (handle == DIRECTORY_HANDLE) {
                critical_events.push_back(CriticalFileEvent::final_directory_stat);
            } else if (handle == READER_HANDLE) {
                critical_events.push_back(CriticalFileEvent::final_reader_stat);
            }
        }
        if (!stat_handle_script.empty()) {
            return pop(stat_handle_script);
        }
        if (handle == DIRECTORY_HANDLE) {
            return {.operation = succeeded(), .metadata = directory_metadata};
        }
        if (handle != WRITER_HANDLE && handle != READER_HANDLE) {
            return {.operation = failed(EBADF), .metadata = {}};
        }
        return {.operation = succeeded(), .metadata = file_metadata(false)};
    }

    [[nodiscard]] FileMetadataResult
    stat_at_nofollow(NativeHandle directory_handle) noexcept override {
        ++stat_at_calls;
        if (directory_sync_observed) {
            critical_events.push_back(CriticalFileEvent::final_name_check);
        }
        if (!stat_at_script.empty()) {
            return pop(stat_at_script);
        }
        if (directory_handle != DIRECTORY_HANDLE) {
            return {.operation = failed(EBADF), .metadata = {}};
        }
        if (!named_exists) {
            ++missing_name_observations;
            const FileMetadataResult missing{
                .operation = operation(FileOperationState::missing, ENOENT),
                .metadata = {},
            };
            if (recreate_name_after_first_missing_observation && missing_name_observations == 1) {
                named_exists = true;
                recreated_foreign_name = true;
            }
            return missing;
        }
        if (recreated_foreign_name) {
            return {
                .operation = succeeded(),
                .metadata =
                    {
                        .kind =
                            package_file::DistributedSieveWorkerWorkPackageObjectKind::regular_file,
                        .identity = {.volume = 101, .object = 909, .generation = 0},
                        .owner_user_id = user_id,
                        .mode = 0600,
                        .link_count = 1,
                        .size = 1,
                    },
            };
        }
        return {.operation = succeeded(), .metadata = file_metadata(true)};
    }

    [[nodiscard]] package_file::DistributedSieveWorkerWorkPackageAclResultV1
    inspect_acl(NativeHandle handle, bool directory) noexcept override {
        ++acl_calls;
        if (directory_sync_observed) {
            critical_events.push_back(directory ? CriticalFileEvent::final_directory_acl
                                                : CriticalFileEvent::final_reader_acl);
        }
        if (!acl_script.empty()) {
            return pop(acl_script);
        }
        const bool valid_handle = directory ? handle == DIRECTORY_HANDLE
                                            : handle == WRITER_HANDLE || handle == READER_HANDLE;
        if (!valid_handle) {
            return {.operation = failed(EBADF), .has_extended_acl = false};
        }
        return {
            .operation = succeeded(),
            .has_extended_acl = !directory && file_has_extended_acl,
        };
    }

    [[nodiscard]] package_file::DistributedSieveWorkerWorkPackageOpenResult
    open_exclusive_at(NativeHandle directory_handle) noexcept override {
        ++exclusive_open_calls;
        if (!exclusive_open_script.empty()) {
            return pop(exclusive_open_script);
        }
        if (directory_handle != DIRECTORY_HANDLE) {
            return {.operation = failed(EBADF)};
        }
        if (named_exists) {
            return {
                .operation = operation(FileOperationState::already_exists, EEXIST),
            };
        }
        named_exists = true;
        writer_open = true;
        file_mode = 0600;
        file_link_count = 1;
        bytes.clear();
        return {.operation = succeeded(), .handle = WRITER_HANDLE};
    }

    [[nodiscard]] FileOperationResult set_mode(NativeHandle handle,
                                               std::uint32_t mode) noexcept override {
        ++set_mode_calls;
        if (!set_mode_script.empty()) {
            return pop(set_mode_script);
        }
        if (handle != WRITER_HANDLE || !writer_open) {
            return failed(EBADF);
        }
        file_mode = mode;
        return succeeded();
    }

    [[nodiscard]] package_file::DistributedSieveWorkerWorkPackageWriteResult
    pwrite_some(NativeHandle handle, const std::byte* data, std::size_t size,
                std::uint64_t offset) noexcept override {
        ++write_calls;
        write_offsets.push_back(offset);
        write_request_sizes.push_back(size);
        modes_during_writes.push_back(file_mode);
        package_file::DistributedSieveWorkerWorkPackageWriteResult result{
            .operation = succeeded(),
            .bytes_written = size,
        };
        if (!write_script.empty()) {
            result = pop(write_script);
        }
        if (handle != WRITER_HANDLE || !writer_open) {
            return {.operation = failed(EBADF), .bytes_written = 0};
        }
        if (result.operation.state != FileOperationState::succeeded ||
            result.bytes_written > size) {
            return result;
        }
        if (offset > std::numeric_limits<std::size_t>::max() ||
            result.bytes_written >
                std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(offset)) {
            return {.operation = failed(EFBIG), .bytes_written = 0};
        }
        const auto begin = static_cast<std::size_t>(offset);
        const auto end = begin + result.bytes_written;
        if (bytes.size() < end) {
            bytes.resize(end);
        }
        std::copy_n(data, result.bytes_written, bytes.begin() + static_cast<std::ptrdiff_t>(begin));
        return result;
    }

    [[nodiscard]] FileOperationResult sync_handle(NativeHandle handle) noexcept override {
        ++sync_calls;
        synced_handles.push_back(handle);
        if (handle == WRITER_HANDLE) {
            critical_events.push_back(CriticalFileEvent::writer_sync);
        } else if (handle == DIRECTORY_HANDLE) {
            critical_events.push_back(CriticalFileEvent::directory_sync);
            directory_sync_observed = true;
        }
        if (!sync_script.empty()) {
            return pop(sync_script);
        }
        if (handle != WRITER_HANDLE && handle != DIRECTORY_HANDLE) {
            return failed(EBADF);
        }
        return succeeded();
    }

    [[nodiscard]] package_file::DistributedSieveWorkerWorkPackageOpenResult
    open_readonly_at(NativeHandle directory_handle) noexcept override {
        ++readonly_open_calls;
        if (!readonly_open_script.empty()) {
            return pop(readonly_open_script);
        }
        if (directory_handle != DIRECTORY_HANDLE || !named_exists) {
            return {.operation = failed(ENOENT)};
        }
        reader_open = true;
        return {.operation = succeeded(), .handle = READER_HANDLE};
    }

    [[nodiscard]] package_file::DistributedSieveWorkerWorkPackageDescriptorPolicyResultV1
    inspect_read_descriptor(NativeHandle handle) noexcept override {
        ++descriptor_policy_calls;
        if (!descriptor_policy_script.empty()) {
            return pop(descriptor_policy_script);
        }
        if (handle != READER_HANDLE || !reader_open) {
            return {.operation = failed(EBADF)};
        }
        return {
            .operation = succeeded(),
            .read_only = true,
            .close_on_exec = true,
        };
    }

    [[nodiscard]] package_file::DistributedSieveWorkerWorkPackageDecodeResultV1
    decode_exact(NativeHandle handle, std::uint64_t total_bytes) noexcept override {
        ++decode_calls;
        if (decode_calls == 1) {
            critical_events.push_back(CriticalFileEvent::named_decode);
        } else if (decode_calls == 2) {
            critical_events.push_back(CriticalFileEvent::anonymous_decode);
        }
        decode_link_counts.push_back(file_link_count);
        decode_named_states.push_back(named_exists);
        if (!decode_script.empty()) {
            return pop(decode_script);
        }
        if (handle != READER_HANDLE || !reader_open || total_bytes != bytes.size()) {
            return {.operation = failed(EINVAL), .decoded = {}};
        }
        auto result = package_file::DistributedSieveWorkerWorkPackageDecodeResultV1{
            .operation = succeeded(),
            .decoded = package_codec::decode_distributed_sieve_work_package_v1(bytes),
        };
        if (replace_named_identity_after_decode) {
            named_identity_override =
                sieve::NativeIdentityV1{.volume = 101, .object = 909, .generation = 0};
        }
        if (drift_directory_identity_after_decode) {
            ++directory_metadata.identity.object;
        }
        if (drift_process_after_decode) {
            ++process_id;
        }
        return result;
    }

    [[nodiscard]] FileOperationResult close_handle(NativeHandle handle) noexcept override {
        if (handle == WRITER_HANDLE) {
            ++writer_close_calls;
            critical_events.push_back(CriticalFileEvent::writer_close);
            const auto result =
                writer_close_script.empty() ? succeeded() : pop(writer_close_script);
            writer_open = false;
            return result;
        }
        if (handle == READER_HANDLE) {
            ++reader_close_calls;
            const auto result =
                reader_close_script.empty() ? succeeded() : pop(reader_close_script);
            reader_open = false;
            return result;
        }
        return failed(EBADF);
    }

    [[nodiscard]] FileOperationResult unlink_at(NativeHandle directory_handle) noexcept override {
        ++unlink_calls;
        critical_events.push_back(CriticalFileEvent::unlink_name);
        if (unlink_then_interrupt) {
            unlink_then_interrupt = false;
            if (directory_handle != DIRECTORY_HANDLE || !named_exists) {
                return failed(ENOENT);
            }
            named_exists = false;
            if (!retain_link_after_unlink) {
                file_link_count = 0;
            }
            return interrupted();
        }
        if (!unlink_script.empty()) {
            const auto result = pop(unlink_script);
            if (drift_process_after_unlink_result) {
                ++process_id;
            }
            return result;
        }
        if (directory_handle != DIRECTORY_HANDLE || !named_exists) {
            return failed(ENOENT);
        }
        named_exists = false;
        if (!retain_link_after_unlink) {
            file_link_count = 0;
        }
        return succeeded();
    }

private:
    template <typename T> [[nodiscard]] static T pop(std::deque<T>& script) noexcept {
        T result = std::move(script.front());
        script.pop_front();
        return result;
    }

    [[nodiscard]] FileMetadata file_metadata(bool named) const noexcept {
        return {
            .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::regular_file,
            .identity = named && named_identity_override.has_value() ? *named_identity_override
                                                                     : FILE_IDENTITY,
            .owner_user_id =
                file_owner_user_id_override.has_value() ? *file_owner_user_id_override : user_id,
            .mode = file_mode,
            .link_count = file_link_count,
            .size = bytes.size(),
        };
    }
};

[[nodiscard]] package_file::DistributedSieveWorkerWorkPackageFileRequestV1
fake_request(const ScriptedFileOps& ops) {
    return {
        .borrowed_attempt_directory_handle = ScriptedFileOps::DIRECTORY_HANDLE,
        .expected_directory_identity = ScriptedFileOps::DIRECTORY_IDENTITY,
        .creator_process_id = ops.process_id,
    };
}

[[nodiscard]] package_file::DistributedSieveWorkerWorkPackageResidueInspectionRequestV1
fake_residue_request(const ScriptedFileOps& ops) {
    return {
        .borrowed_attempt_directory_handle = ScriptedFileOps::DIRECTORY_HANDLE,
        .expected_directory_identity = ScriptedFileOps::DIRECTORY_IDENTITY,
        .observer_process_id = ops.process_id,
    };
}

[[nodiscard]] package_file::DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1
fake_reconciliation_request(
    const ScriptedFileOps& ops,
    const package_file::DistributedSieveWorkerWorkPackageResidueWitnessV1* expected_residue) {
    return {
        .borrowed_attempt_directory_handle = ScriptedFileOps::DIRECTORY_HANDLE,
        .expected_directory_identity = ScriptedFileOps::DIRECTORY_IDENTITY,
        .reconciler_process_id = ops.process_id,
        .expected_residue = expected_residue,
    };
}

void install_fake_residue(ScriptedFileOps& ops,
                          const package_codec::DistributedSieveEncodedWorkPackageV1& encoded) {
    ops.named_exists = true;
    ops.file_mode = 0400;
    ops.file_link_count = 1;
    ops.bytes = encoded.bytes;
}

[[nodiscard]] package_file::DistributedSieveWorkerWorkPackageResidueWitnessV1
fake_residue_witness(const Identity& identity,
                     const package_codec::DistributedSieveEncodedWorkPackageV1& encoded,
                     std::uint64_t owner_user_id) {
    return {
        .identity = identity,
        .package = encoded.witness,
        .file_identity = ScriptedFileOps::FILE_IDENTITY,
        .file_extent = encoded.bytes.size(),
        .owner_user_id = owner_user_id,
    };
}

void check_package_witness(
    const package_file::DistributedSieveWorkerWorkPackageFileWitnessV1& actual,
    const package_codec::DistributedSieveEncodedWorkPackageV1& expected,
    const sieve::NativeIdentityV1& expected_file_identity, std::uint64_t process_id) {
    CHECK(actual.package.body_bytes == expected.witness.body_bytes);
    CHECK(actual.package.total_bytes == expected.witness.total_bytes);
    CHECK(actual.package.work_sha256.bytes == expected.witness.work_sha256.bytes);
    CHECK(actual.package.package_sha256.bytes == expected.witness.package_sha256.bytes);
    CHECK(actual.file_identity == expected_file_identity);
    CHECK(actual.file_extent == expected.bytes.size());
    CHECK(actual.creator_process_id == process_id);
}

void check_residue_witness(
    const package_file::DistributedSieveWorkerWorkPackageResidueWitnessV1& actual,
    const package_codec::DistributedSieveEncodedWorkPackageV1& expected,
    const sieve::NativeIdentityV1& expected_file_identity, std::uint64_t owner_user_id) {
    CHECK(actual.package.body_bytes == expected.witness.body_bytes);
    CHECK(actual.package.total_bytes == expected.witness.total_bytes);
    CHECK(actual.package.work_sha256.bytes == expected.witness.work_sha256.bytes);
    CHECK(actual.package.package_sha256.bytes == expected.witness.package_sha256.bytes);
    CHECK(actual.file_identity == expected_file_identity);
    CHECK(actual.file_extent == expected.bytes.size());
    CHECK(actual.file_extent == actual.package.total_bytes);
    CHECK(actual.owner_user_id == owner_user_id);
    const auto digest = sieve::distributed_sieve_work_digest(actual.identity);
    CHECK(digest);
    CHECK(digest.digest->bytes == actual.package.work_sha256.bytes);
}

void test_authority_boundary_and_fixed_contract() {
    using Token = package_file::DistributedSieveWorkerWorkPackageFileV1;
    using ResidueWitness = package_file::DistributedSieveWorkerWorkPackageResidueWitnessV1;
    using ReconciliationResult =
        package_file::DistributedSieveWorkerWorkPackageResidueReconciliationResultV1;
    using WithOpsResult = package_file::DistributedSieveWorkerWorkPackageFileWithOpsResultV1;
    using Witness = package_file::DistributedSieveWorkerWorkPackageFileWitnessV1;

    static_assert(!std::is_default_constructible_v<Token>);
    static_assert(!std::is_copy_constructible_v<Token>);
    static_assert(!std::is_copy_assignable_v<Token>);
    static_assert(std::is_nothrow_move_constructible_v<Token>);
    static_assert(!std::is_move_assignable_v<Token>);
    static_assert(std::is_nothrow_destructible_v<Token>);
    static_assert(!std::is_constructible_v<Token, NativeHandle, Witness, std::uint64_t>);
    static_assert(std::is_same_v<decltype(WithOpsResult::witness), std::optional<Witness>>);
    static_assert(std::is_copy_constructible_v<ResidueWitness>);
    static_assert(std::is_nothrow_move_constructible_v<ResidueWitness>);
    static_assert(
        noexcept(std::declval<const ResidueWitness&>() == std::declval<const ResidueWitness&>()));
    static_assert(std::is_same_v<
                  decltype(ReconciliationResult::disposition),
                  std::optional<
                      package_file::
                          DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1>>);

    CHECK(package_file::DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1 ==
          ".gnfs-worker-work-package-v1");
}

void test_fake_happy_path_seals_unlinks_and_closes_reader() {
    const auto identity = make_identity();
    const auto expected = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(expected);

    ScriptedFileOps ops;
    const auto result = package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
        fake_request(ops), identity, ops);
    CHECK(result);
    CHECK(result.witness.has_value());
    check_package_witness(*result.witness, *expected.package, ScriptedFileOps::FILE_IDENTITY,
                          ops.process_id);
    CHECK(ops.bytes == expected.package->bytes);
    CHECK(!ops.named_exists);
    CHECK(!ops.writer_open);
    CHECK(!ops.reader_open);
    CHECK(ops.file_mode == 0400);
    CHECK(ops.file_link_count == 0);
    CHECK(ops.set_mode_calls == 2);
    CHECK(!ops.modes_during_writes.empty());
    CHECK(std::all_of(ops.modes_during_writes.begin(), ops.modes_during_writes.end(),
                      [](std::uint32_t mode) { return mode == 0600; }));
    CHECK(ops.writer_close_calls == 1);
    CHECK(ops.reader_close_calls == 1);
    CHECK(ops.unlink_calls == 1);
    CHECK(ops.decode_calls == 2);
    CHECK(ops.decode_named_states == std::vector<bool>({true, false}));
    CHECK(ops.decode_link_counts == std::vector<std::uint64_t>({1, 0}));
    CHECK(ops.synced_handles == std::vector<NativeHandle>({ScriptedFileOps::WRITER_HANDLE,
                                                           ScriptedFileOps::DIRECTORY_HANDLE}));
    CHECK(std::all_of(ops.write_request_sizes.begin(), ops.write_request_sizes.end(),
                      [](std::size_t size) { return size <= 64U * 1024U; }));
}

void test_fake_critical_event_order_is_frozen() {
    ScriptedFileOps ops;
    const auto result = package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
        fake_request(ops), make_identity(), ops);
    CHECK(result);
    CHECK(ops.critical_events == std::vector<CriticalFileEvent>({
                                     CriticalFileEvent::writer_sync,
                                     CriticalFileEvent::named_decode,
                                     CriticalFileEvent::writer_close,
                                     CriticalFileEvent::unlink_name,
                                     CriticalFileEvent::anonymous_decode,
                                     CriticalFileEvent::directory_sync,
                                     CriticalFileEvent::final_directory_stat,
                                     CriticalFileEvent::final_directory_acl,
                                     CriticalFileEvent::final_name_check,
                                     CriticalFileEvent::final_reader_stat,
                                     CriticalFileEvent::final_reader_acl,
                                 }));
}

void test_fake_recreated_name_preserves_named_may_remain_or_semantics() {
    ScriptedFileOps ops;
    ops.recreate_name_after_first_missing_observation = true;
    const auto result = package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
        fake_request(ops), make_identity(), ops);
    CHECK(!result);
    CHECK(!result.witness.has_value());
    CHECK(result.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
    CHECK(result.diagnostic.native_error == EEXIST);
    CHECK(result.diagnostic.named_may_remain);
    CHECK(ops.missing_name_observations == 1);
    CHECK(ops.recreated_foreign_name);
    CHECK(ops.named_exists);
    CHECK(ops.writer_close_calls == 1);
    CHECK(ops.reader_close_calls == 1);
    CHECK(ops.unlink_calls == 1);
    CHECK(ops.decode_calls == 2);
    CHECK(ops.synced_handles == std::vector<NativeHandle>({ScriptedFileOps::WRITER_HANDLE,
                                                           ScriptedFileOps::DIRECTORY_HANDLE}));
}

void test_fake_reader_policy_rejects_writable_or_inheritable_descriptors() {
    const auto identity = make_identity();
    constexpr std::array invalid_policies{
        std::pair{false, true},
        std::pair{true, false},
    };

    for (const auto [read_only, close_on_exec] : invalid_policies) {
        ScriptedFileOps ops;
        ops.descriptor_policy_script.push_back({
            .operation = succeeded(),
            .read_only = read_only,
            .close_on_exec = close_on_exec,
        });
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(!result);
        CHECK(!result.witness.has_value());
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(result.diagnostic.named_may_remain);
        CHECK(ops.descriptor_policy_calls == 1);
        CHECK(ops.decode_calls == 0);
        CHECK(ops.writer_close_calls == 1);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 0);
        CHECK(ops.named_exists);
    }
}

void test_fake_valid_decode_with_mismatched_witness_fails_before_unlink() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);
    auto decoded = package_codec::decode_distributed_sieve_work_package_v1(encoded.package->bytes);
    CHECK(decoded);
    decoded.package->witness.package_sha256.bytes.front() ^= std::byte{0x01};

    ScriptedFileOps ops;
    ops.decode_script.push_back({
        .operation = succeeded(),
        .decoded = std::move(decoded),
    });
    const auto result = package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
        fake_request(ops), identity, ops);
    CHECK(!result);
    CHECK(!result.witness.has_value());
    CHECK(result.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::decode_failed);
    CHECK(result.diagnostic.named_may_remain);
    CHECK(ops.decode_calls == 1);
    CHECK(ops.writer_close_calls == 1);
    CHECK(ops.reader_close_calls == 1);
    CHECK(ops.unlink_calls == 0);
    CHECK(ops.named_exists);
}

void test_fake_buffered_pwrite_retries_short_writes_and_rejects_zero() {
    const auto identity = make_large_identity();
    const auto expected = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(expected);

    {
        ScriptedFileOps ops;
        ops.write_script.push_back({.operation = interrupted(), .bytes_written = 0});
        ops.write_script.push_back({.operation = succeeded(), .bytes_written = 7});
        ops.write_script.push_back({.operation = succeeded(), .bytes_written = 13});
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(result);
        CHECK(ops.bytes == expected.package->bytes);
        CHECK(ops.write_calls >= 5);
        CHECK(ops.write_offsets.size() >= 3);
        CHECK(ops.write_offsets[0] == 0);
        CHECK(ops.write_offsets[1] == 0);
        CHECK(ops.write_offsets[2] == 7);
        CHECK(std::all_of(ops.write_request_sizes.begin(), ops.write_request_sizes.end(),
                          [](std::size_t size) { return size <= 64U * 1024U; }));
        CHECK(ops.writer_close_calls == 1);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 1);
    }

    {
        ScriptedFileOps ops;
        ops.write_script.push_back({.operation = succeeded(), .bytes_written = 0});
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::publication_failed);
        CHECK(ops.writer_close_calls == 1);
        CHECK(ops.reader_close_calls == 0);
        CHECK(ops.unlink_calls == 0);
        CHECK(ops.named_exists);
        CHECK(result.diagnostic.named_may_remain);
    }
}

void test_fake_request_directory_pid_and_acl_rejection() {
    const auto identity = make_identity();

    {
        ScriptedFileOps ops;
        auto request = fake_request(ops);
        request.borrowed_attempt_directory_handle =
            package_file::DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_INVALID_HANDLE;
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                request, identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
        CHECK(ops.stat_handle_calls == 0);
        CHECK(ops.exclusive_open_calls == 0);
    }

    {
        ScriptedFileOps ops;
        auto request = fake_request(ops);
        request.creator_process_id += 1;
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                request, identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
        CHECK(ops.stat_handle_calls == 0);
        CHECK(ops.exclusive_open_calls == 0);
    }

    {
        ScriptedFileOps ops;
        auto request = fake_request(ops);
        request.expected_directory_identity.object += 1;
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                request, identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
        CHECK(ops.exclusive_open_calls == 0);
    }

    {
        ScriptedFileOps ops;
        ops.directory_metadata.mode = 0755;
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
        CHECK(ops.exclusive_open_calls == 0);
    }

    {
        ScriptedFileOps ops;
        ops.acl_script.push_back({.operation = succeeded(), .has_extended_acl = true});
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
        CHECK(ops.exclusive_open_calls == 0);
    }

    {
        ScriptedFileOps ops;
        ops.acl_script.push_back({.operation = operation(FileOperationState::unsupported, ENOTSUP),
                                  .has_extended_acl = false});
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::platform_unavailable);
        CHECK(ops.exclusive_open_calls == 0);
    }
}

void test_fake_exclusive_create_identity_drift_and_foreign_preservation() {
    const auto identity = make_identity();

    {
        ScriptedFileOps ops;
        ops.named_exists = true;
        ops.file_link_count = 1;
        ops.bytes = {std::byte{0xde}, std::byte{0xad}};
        const auto before = ops.bytes;
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(ops.bytes == before);
        CHECK(ops.named_exists);
        CHECK(ops.writer_close_calls == 0);
        CHECK(ops.reader_close_calls == 0);
        CHECK(ops.unlink_calls == 0);
    }

    {
        ScriptedFileOps ops;
        auto drifted = FileMetadata{
            .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::regular_file,
            .identity = {.volume = 101, .object = 999, .generation = 0},
            .owner_user_id = ops.user_id,
            .mode = 0600,
            .link_count = 1,
            .size = 0,
        };
        ops.stat_at_script.push_back({.operation = succeeded(), .metadata = drifted});
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(!result);
        CHECK(ops.writer_close_calls == 1);
        CHECK(ops.unlink_calls == 0);
        CHECK(ops.named_exists);
        CHECK(result.diagnostic.named_may_remain);
    }
}

void test_fake_close_is_single_shot_and_preserves_primary_failure() {
    const auto identity = make_identity();

    {
        ScriptedFileOps ops;
        ops.writer_close_script.push_back(interrupted());
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::close_failed);
        CHECK(result.diagnostic.native_error == EINTR);
        CHECK(ops.writer_close_calls == 1);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 0);
        CHECK(ops.named_exists);
    }

    {
        ScriptedFileOps ops;
        ops.write_script.push_back({.operation = failed(EIO), .bytes_written = 0});
        ops.writer_close_script.push_back(failed(EBADF));
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1_with_ops(
                fake_request(ops), identity, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::publication_failed);
        CHECK(result.diagnostic.native_error == EIO);
        CHECK(result.diagnostic.secondary_close_error == EBADF);
        CHECK(ops.writer_close_calls == 1);
        CHECK(ops.reader_close_calls == 0);
        CHECK(ops.unlink_calls == 0);
    }
}

void test_fake_residue_inspector_happy_path_and_equality() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    ScriptedFileOps ops;
    install_fake_residue(ops, *encoded.package);
    const auto result =
        package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
            fake_residue_request(ops), ops);
    CHECK(result);
    CHECK(result.witness.has_value());
    check_residue_witness(*result.witness, *encoded.package, ScriptedFileOps::FILE_IDENTITY,
                          ops.user_id);
    CHECK(!ops.reader_open);
    CHECK(ops.reader_close_calls == 1);
    CHECK(ops.readonly_open_calls == 1);
    CHECK(ops.decode_calls == 1);
    CHECK(ops.unlink_calls == 0);
    CHECK(ops.named_exists);

    auto copy = *result.witness;
    CHECK(copy == *result.witness);
    ++copy.owner_user_id;
    CHECK(!(copy == *result.witness));
    copy = *result.witness;
    ++copy.identity.distributed.relation_cap_per_worker;
    CHECK(!(copy == *result.witness));

    {
        ScriptedFileOps closing_ops;
        install_fake_residue(closing_ops, *encoded.package);
        closing_ops.reader_close_script.push_back(interrupted());
        const auto close_failed =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_residue_request(closing_ops), closing_ops);
        CHECK(!close_failed);
        CHECK(!close_failed.witness.has_value());
        CHECK(close_failed.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::close_failed);
        CHECK(close_failed.diagnostic.native_error == EINTR);
        CHECK(closing_ops.reader_close_calls == 1);
        CHECK(closing_ops.unlink_calls == 0);
        CHECK(closing_ops.named_exists);
    }
}

void test_fake_residue_inspector_rejects_missing_metadata_acl_and_policy() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    {
        ScriptedFileOps ops;
        const auto result =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_residue_request(ops), ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(result.diagnostic.native_error == ENOENT);
        CHECK(!result.diagnostic.named_may_remain);
        CHECK(ops.readonly_open_calls == 0);
        CHECK(ops.unlink_calls == 0);
    }

    constexpr std::array invalid_metadata{
        FileMetadata{
            .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::directory,
            .identity = ScriptedFileOps::FILE_IDENTITY,
            .owner_user_id = 501,
            .mode = 0400,
            .link_count = 1,
            .size = 128,
        },
        FileMetadata{
            .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::other,
            .identity = ScriptedFileOps::FILE_IDENTITY,
            .owner_user_id = 501,
            .mode = 0400,
            .link_count = 1,
            .size = 128,
        },
        FileMetadata{
            .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::regular_file,
            .identity = ScriptedFileOps::FILE_IDENTITY,
            .owner_user_id = 502,
            .mode = 0400,
            .link_count = 1,
            .size = 128,
        },
        FileMetadata{
            .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::regular_file,
            .identity = ScriptedFileOps::FILE_IDENTITY,
            .owner_user_id = 501,
            .mode = 0600,
            .link_count = 1,
            .size = 128,
        },
        FileMetadata{
            .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::regular_file,
            .identity = ScriptedFileOps::FILE_IDENTITY,
            .owner_user_id = 501,
            .mode = 0400,
            .link_count = 2,
            .size = 128,
        },
        FileMetadata{
            .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::regular_file,
            .identity = ScriptedFileOps::FILE_IDENTITY,
            .owner_user_id = 501,
            .mode = 0400,
            .link_count = 1,
            .size = package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1 + UINT64_C(1),
        },
    };
    for (const auto& metadata : invalid_metadata) {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        ops.stat_at_script.push_back({.operation = succeeded(), .metadata = metadata});
        const auto result =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_residue_request(ops), ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(ops.readonly_open_calls == 0);
        CHECK(ops.unlink_calls == 0);
    }

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        ops.file_has_extended_acl = true;
        const auto result =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_residue_request(ops), ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(result.diagnostic.native_error == EACCES);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 0);
    }

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        ops.descriptor_policy_script.push_back({
            .operation = succeeded(),
            .read_only = false,
            .close_on_exec = true,
        });
        const auto result =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_residue_request(ops), ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 0);
    }
}

void test_fake_residue_inspector_revalidates_decode_boundary() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    const auto run_mutation = [&](auto mutate) {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        mutate(ops);
        const auto result =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_residue_request(ops), ops);
        CHECK(!result);
        CHECK(!result.witness.has_value());
        CHECK(ops.decode_calls == 1);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 0);
        CHECK(ops.named_exists);
        return result.diagnostic;
    };

    const auto name_replaced =
        run_mutation([](ScriptedFileOps& ops) { ops.replace_named_identity_after_decode = true; });
    CHECK(name_replaced.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);

    const auto directory_replaced = run_mutation(
        [](ScriptedFileOps& ops) { ops.drift_directory_identity_after_decode = true; });
    CHECK(directory_replaced.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);

    const auto process_changed =
        run_mutation([](ScriptedFileOps& ops) { ops.drift_process_after_decode = true; });
    CHECK(process_changed.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        ops.bytes.back() ^= std::byte{0x01};
        const auto result =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_residue_request(ops), ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::decode_failed);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 0);
    }

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        ops.decode_script.push_back({
            .operation = succeeded(),
            .decoded =
                {
                    .package = std::nullopt,
                    .status =
                        {
                            .error = sieve::DistributedSieveProtocolError::resource_exhausted,
                        },
                },
        });
        const auto result =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_residue_request(ops), ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::resource_exhausted);
        CHECK(result.diagnostic.protocol_status.error ==
              sieve::DistributedSieveProtocolError::resource_exhausted);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 0);
        CHECK(ops.named_exists);
    }

    for (const int extent_delta : {-1, 1}) {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        if (extent_delta < 0) {
            ops.bytes.pop_back();
        } else {
            ops.bytes.push_back(std::byte{0});
        }
        const auto result =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_residue_request(ops), ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::decode_failed);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 0);
    }
}

struct FakeReconciliationHookContext final {
    ScriptedFileOps* ops = nullptr;
    std::size_t calls = 0;
    bool select_failure = true;
};

void replace_named_residue_with_same_bytes(void* opaque) noexcept {
    auto& context = *static_cast<FakeReconciliationHookContext*>(opaque);
    ++context.calls;
    context.ops->named_identity_override =
        sieve::NativeIdentityV1{.volume = 101, .object = 909, .generation = 0};
}

void recreate_foreign_name(void* opaque) noexcept {
    auto& context = *static_cast<FakeReconciliationHookContext*>(opaque);
    ++context.calls;
    context.ops->named_exists = true;
    context.ops->recreated_foreign_name = true;
}

[[nodiscard]] bool select_directory_sync_failure(void* opaque) noexcept {
    auto& context = *static_cast<FakeReconciliationHookContext*>(opaque);
    ++context.calls;
    return context.select_failure;
}

[[nodiscard]] bool drift_process_while_selecting_sync_failure(void* opaque) noexcept {
    auto& context = *static_cast<FakeReconciliationHookContext*>(opaque);
    ++context.calls;
    ++context.ops->process_id;
    return true;
}

void test_fake_residue_reconciliation_present_absent_and_idempotent() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    ScriptedFileOps ops;
    install_fake_residue(ops, *encoded.package);
    const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
    const auto present =
        package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
            fake_reconciliation_request(ops, &witness), ops);
    CHECK(present);
    CHECK(
        present.disposition ==
        package_file::DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1::removed);
    CHECK(!present.fault_point.has_value());
    CHECK(!ops.named_exists);
    CHECK(ops.file_link_count == 0);
    CHECK(ops.readonly_open_calls == 1);
    CHECK(ops.unlink_calls == 1);
    CHECK(ops.sync_calls == 1);
    CHECK(ops.reader_close_calls == 1);
    CHECK(!ops.reader_open);
    CHECK(ops.decode_calls >= 6);

    const auto absent =
        package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
            fake_reconciliation_request(ops, nullptr), ops);
    CHECK(absent);
    CHECK(absent.disposition ==
          package_file::DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1::
              confirmed_absent);
    CHECK(ops.readonly_open_calls == 1);
    CHECK(ops.decode_calls >= 6);
    CHECK(ops.unlink_calls == 1);
    CHECK(ops.sync_calls == 2);
    CHECK(ops.reader_close_calls == 1);

    ScriptedFileOps initially_absent;
    const auto absence_barrier =
        package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
            fake_reconciliation_request(initially_absent, nullptr), initially_absent);
    CHECK(absence_barrier);
    CHECK(absence_barrier.disposition ==
          package_file::DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1::
              confirmed_absent);
    CHECK(initially_absent.readonly_open_calls == 0);
    CHECK(initially_absent.decode_calls == 0);
    CHECK(initially_absent.unlink_calls == 0);
    CHECK(initially_absent.reader_close_calls == 0);
    CHECK(initially_absent.sync_calls == 1);

    ScriptedFileOps unexpected_name;
    install_fake_residue(unexpected_name, *encoded.package);
    const auto rejected_absence =
        package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
            fake_reconciliation_request(unexpected_name, nullptr), unexpected_name);
    CHECK(!rejected_absence);
    CHECK(rejected_absence.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
    CHECK(rejected_absence.diagnostic.native_error == EEXIST);
    CHECK(rejected_absence.diagnostic.named_may_remain);
    CHECK(unexpected_name.named_exists);
    CHECK(unexpected_name.readonly_open_calls == 0);
    CHECK(unexpected_name.decode_calls == 0);
    CHECK(unexpected_name.unlink_calls == 0);
    CHECK(unexpected_name.reader_close_calls == 0);
    CHECK(unexpected_name.sync_calls == 0);

    ScriptedFileOps invalid_absent_fault;
    const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1
        invalid_hooks{
            .stop_after =
                package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
                    after_name_unlinked,
        };
    const auto invalid_fault =
        package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
            fake_reconciliation_request(invalid_absent_fault, nullptr), invalid_absent_fault,
            invalid_hooks);
    CHECK(!invalid_fault);
    CHECK(invalid_fault.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
    CHECK(invalid_absent_fault.readonly_open_calls == 0);
    CHECK(invalid_absent_fault.decode_calls == 0);
    CHECK(invalid_absent_fault.unlink_calls == 0);
    CHECK(invalid_absent_fault.reader_close_calls == 0);
    CHECK(invalid_absent_fault.sync_calls == 0);
}

void test_fake_residue_reconciliation_crash_prefixes_retry_to_absence() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    for (const auto fault :
         {package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
              after_name_unlinked,
          package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
              after_directory_durable}) {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .stop_after = fault};
        const auto interrupted =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops, hooks);
        CHECK(!interrupted);
        CHECK(!interrupted.disposition.has_value());
        CHECK(interrupted.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::interrupted);
        CHECK(interrupted.fault_point == fault);
        CHECK(!ops.named_exists);
        CHECK(ops.file_link_count == 0);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.sync_calls ==
              (fault == package_file::
                            DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
                                after_name_unlinked
                   ? 0U
                   : 1U));

        const auto retried =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, nullptr), ops);
        CHECK(retried);
        CHECK(retried.disposition ==
              package_file::DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1::
                  confirmed_absent);
    }

    ScriptedFileOps absent_ops;
    const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1
        absent_hooks{
            .stop_after =
                package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
                    after_directory_durable,
        };
    const auto interrupted_absence =
        package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
            fake_reconciliation_request(absent_ops, nullptr), absent_ops, absent_hooks);
    CHECK(!interrupted_absence);
    CHECK(interrupted_absence.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::interrupted);
    CHECK(interrupted_absence.fault_point ==
          package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
              after_directory_durable);
    CHECK(absent_ops.readonly_open_calls == 0);
    CHECK(absent_ops.decode_calls == 0);
    CHECK(absent_ops.unlink_calls == 0);
    CHECK(absent_ops.sync_calls == 1);
    const auto absent_retry =
        package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
            fake_reconciliation_request(absent_ops, nullptr), absent_ops);
    CHECK(absent_retry);

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        ops.unlink_then_interrupt = true;
        const auto ambiguous =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops);
        CHECK(!ambiguous);
        CHECK(ambiguous.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::interrupted);
        CHECK(!ambiguous.fault_point.has_value());
        CHECK(ops.unlink_calls == 1);
        CHECK(!ops.named_exists);
        CHECK(ops.file_link_count == 0);
        CHECK(ops.sync_calls == 0);
        const auto retry =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, nullptr), ops);
        CHECK(retry);
    }

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        ops.unlink_script.push_back(interrupted());
        const auto interrupted_without_effect =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops);
        CHECK(!interrupted_without_effect);
        CHECK(interrupted_without_effect.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::interrupted);
        CHECK(ops.unlink_calls == 1);
        CHECK(ops.named_exists);
        CHECK(ops.file_link_count == 1);
        CHECK(ops.sync_calls == 0);
    }

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        ops.unlink_script.push_back(interrupted());
        ops.drift_process_after_unlink_result = true;
        const auto unproven =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops);
        CHECK(!unproven);
        CHECK(unproven.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
        CHECK(unproven.diagnostic.named_may_remain);
        CHECK(ops.unlink_calls == 1);
        CHECK(ops.named_exists);
        CHECK(ops.file_link_count == 1);
        CHECK(ops.sync_calls == 0);
    }
}

void test_fake_residue_reconciliation_sync_failures_are_not_success() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        ops.sync_script.push_back(failed(EIO));
        const auto failed_sync =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops);
        CHECK(!failed_sync);
        CHECK(failed_sync.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::durability_failed);
        CHECK(failed_sync.diagnostic.native_error == EIO);
        CHECK(!ops.named_exists);
        CHECK(ops.sync_calls == 1);
        const auto retried =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, nullptr), ops);
        CHECK(retried);
    }

    for (const bool present : {false, true}) {
        ScriptedFileOps ops;
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        if (present) {
            install_fake_residue(ops, *encoded.package);
        }
        FakeReconciliationHookContext context{.ops = &ops};
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .fail_before_directory_sync = select_directory_sync_failure,
            .context = &context,
        };
        const auto injected =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, present ? &witness : nullptr), ops, hooks);
        CHECK(!injected);
        CHECK(injected.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::durability_failed);
        CHECK(injected.diagnostic.native_error == EIO);
        CHECK(context.calls == 1);
        CHECK(ops.sync_calls == 1);
        CHECK(!ops.named_exists);

        const auto retried =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, nullptr), ops);
        CHECK(retried);
    }

    {
        ScriptedFileOps ops;
        ops.sync_script.push_back(interrupted());
        const auto retried_sync =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, nullptr), ops);
        CHECK(retried_sync);
        CHECK(ops.sync_calls == 2);
        CHECK(ops.unlink_calls == 0);
    }

    {
        ScriptedFileOps ops;
        const auto request = fake_reconciliation_request(ops, nullptr);
        FakeReconciliationHookContext context{.ops = &ops};
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .fail_before_directory_sync = drift_process_while_selecting_sync_failure,
            .context = &context,
        };
        const auto drifted =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                request, ops, hooks);
        CHECK(!drifted);
        CHECK(drifted.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
        CHECK(context.calls == 1);
        CHECK(ops.sync_calls == 0);
        CHECK(ops.unlink_calls == 0);
    }
}

void test_fake_residue_reconciliation_rejects_drift_without_blind_unlink() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    const auto run_preserved = [&](auto mutate_ops, auto mutate_witness) {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        mutate_ops(ops);
        mutate_witness(witness);
        const auto result =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops);
        CHECK(!result);
        CHECK(!result.disposition.has_value());
        CHECK(ops.unlink_calls == 0);
        CHECK(ops.named_exists);
    };
    const auto no_ops = [](ScriptedFileOps&) {};
    const auto no_witness = [](auto&) {};

    run_preserved([](ScriptedFileOps& ops) { ops.directory_metadata.mode = 0755; }, no_witness);
    run_preserved([](ScriptedFileOps& ops) { ++ops.directory_metadata.identity.object; },
                  no_witness);
    run_preserved([](ScriptedFileOps& ops) { ++ops.directory_metadata.owner_user_id; }, no_witness);
    run_preserved(
        [](ScriptedFileOps& ops) {
            ops.acl_script.push_back({
                .operation = succeeded(),
                .has_extended_acl = true,
            });
        },
        no_witness);
    run_preserved([](ScriptedFileOps& ops) { ops.file_has_extended_acl = true; }, no_witness);
    run_preserved([](ScriptedFileOps& ops) { ops.file_owner_user_id_override = 502; }, no_witness);
    run_preserved([](ScriptedFileOps& ops) { ops.file_mode = 0600; }, no_witness);
    run_preserved([](ScriptedFileOps& ops) { ops.file_link_count = 2; }, no_witness);
    run_preserved([](ScriptedFileOps& ops) { ops.bytes.push_back(std::byte{0}); }, no_witness);
    run_preserved([](ScriptedFileOps& ops) { ops.bytes.back() ^= std::byte{1}; }, no_witness);
    run_preserved(
        [](ScriptedFileOps& ops) {
            ops.descriptor_policy_script.push_back({
                .operation = succeeded(),
                .read_only = false,
                .close_on_exec = true,
            });
        },
        no_witness);
    run_preserved(no_ops, [](auto& witness) { ++witness.owner_user_id; });
    run_preserved(no_ops, [](auto& witness) { witness.file_identity = {}; });
    run_preserved(no_ops, [](auto& witness) { ++witness.file_identity.object; });
    run_preserved(no_ops, [](auto& witness) { ++witness.file_extent; });
    run_preserved(no_ops, [](auto& witness) { ++witness.package.body_bytes; });
    run_preserved(no_ops,
                  [](auto& witness) { witness.package.package_sha256.bytes[0] ^= std::byte{1}; });
    run_preserved(no_ops,
                  [](auto& witness) { ++witness.identity.distributed.relation_cap_per_worker; });

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        auto request = fake_reconciliation_request(ops, &witness);
        ++request.reconciler_process_id;
        const auto result =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                request, ops);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
        CHECK(ops.readonly_open_calls == 0);
        CHECK(ops.unlink_calls == 0);
    }

    {
        ScriptedFileOps ops;
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .stop_after = static_cast<
                package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1>(
                255),
        };
        const auto result =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, nullptr), ops, hooks);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
        CHECK(ops.stat_handle_calls == 0);
        CHECK(ops.sync_calls == 0);
    }
}

void test_fake_residue_reconciliation_closes_all_hook_and_post_unlink_seams() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        FakeReconciliationHookContext context{.ops = &ops};
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .before_unlink = replace_named_residue_with_same_bytes,
            .context = &context,
        };
        const auto replaced =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops, hooks);
        CHECK(!replaced);
        CHECK(context.calls == 1);
        CHECK(ops.unlink_calls == 0);
        CHECK(ops.named_exists);
        CHECK(ops.reader_close_calls == 1);
    }

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        ops.recreate_name_after_first_missing_observation = true;
        const auto recreated =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops);
        CHECK(!recreated);
        CHECK(recreated.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(ops.unlink_calls == 1);
        CHECK(ops.named_exists);
        CHECK(ops.recreated_foreign_name);
    }

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        ops.retain_link_after_unlink = true;
        const auto retained_link =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops);
        CHECK(!retained_link);
        CHECK(retained_link.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(ops.unlink_calls == 1);
        CHECK(!ops.named_exists);
        CHECK(ops.file_link_count == 1);
    }

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        FakeReconciliationHookContext context{.ops = &ops};
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .after_directory_durable = recreate_foreign_name,
            .context = &context,
        };
        const auto recreated =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops, hooks);
        CHECK(!recreated);
        CHECK(context.calls == 1);
        CHECK(ops.sync_calls == 1);
        CHECK(ops.named_exists);
        CHECK(ops.reader_close_calls == 1);
    }

    {
        ScriptedFileOps ops;
        install_fake_residue(ops, *encoded.package);
        const auto witness = fake_residue_witness(identity, *encoded.package, ops.user_id);
        ops.reader_close_script.push_back(interrupted());
        const auto close_failed =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
                fake_reconciliation_request(ops, &witness), ops);
        CHECK(!close_failed);
        CHECK(close_failed.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::close_failed);
        CHECK(close_failed.diagnostic.native_error == EINTR);
        CHECK(ops.reader_close_calls == 1);
        CHECK(ops.unlink_calls == 1);
        CHECK(ops.sync_calls == 1);
        CHECK(!ops.named_exists);
    }
}

#if !defined(_WIN32)

class ScopedFd final {
public:
    ScopedFd() noexcept = default;
    explicit ScopedFd(int descriptor) noexcept : descriptor_(descriptor) {}

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}

    ScopedFd& operator=(ScopedFd&&) = delete;

    ~ScopedFd() {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_ = -1;
};

class TempDirectory final {
public:
    TempDirectory() {
        path_ = gnfs::util::temp_path("gnfs_work_package_file_" +
                                      std::to_string(gnfs::util::process_id()) + "_" +
                                      std::to_string(++sequence_));
        std::error_code error;
        const bool created = std::filesystem::create_directory(path_, error);
        CHECK(created);
        CHECK(!error);
        CHECK(::chmod(path_.c_str(), 0700) == 0);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] ScopedFd open_directory() const {
        int descriptor = -1;
        do {
            descriptor = ::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        } while (descriptor < 0 && errno == EINTR);
        CHECK(descriptor >= 0);
        return ScopedFd(descriptor);
    }

private:
    inline static unsigned sequence_ = 0;
    std::filesystem::path path_;
};

[[nodiscard]] bool descriptor_is_open(int descriptor) noexcept {
    int result = -1;
    do {
        result = ::fcntl(descriptor, F_GETFD);
    } while (result < 0 && errno == EINTR);
    return result >= 0;
}

[[nodiscard]] std::vector<int> snapshot_open_descriptors() {
    long configured_limit = ::sysconf(_SC_OPEN_MAX);
    if (configured_limit < 0) {
        configured_limit = 1024;
    }
    const int scan_limit =
        static_cast<int>(std::min<long>(configured_limit, static_cast<long>(4096)));

    std::vector<int> descriptors;
    for (int descriptor = 0; descriptor < scan_limit; ++descriptor) {
        if (descriptor_is_open(descriptor)) {
            descriptors.push_back(descriptor);
        }
    }
    return descriptors;
}

[[nodiscard]] std::vector<int> added_descriptors(const std::vector<int>& before,
                                                 const std::vector<int>& after) {
    std::vector<int> added;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(added));
    return added;
}

[[nodiscard]] sieve::NativeIdentityV1 descriptor_identity(int descriptor) {
    struct stat metadata{};
    CHECK(::fstat(descriptor, &metadata) == 0);
    return {
        .volume = static_cast<std::uint64_t>(metadata.st_dev),
        .object = static_cast<std::uint64_t>(metadata.st_ino),
        .generation = 0,
    };
}

[[nodiscard]] package_file::DistributedSieveWorkerWorkPackageFileRequestV1
native_request(int directory_descriptor) {
    return {
        .borrowed_attempt_directory_handle = directory_descriptor,
        .expected_directory_identity = descriptor_identity(directory_descriptor),
        .creator_process_id = static_cast<std::uint64_t>(gnfs::util::process_id()),
    };
}

[[nodiscard]] package_file::DistributedSieveWorkerWorkPackageResidueInspectionRequestV1
native_residue_request(int directory_descriptor) {
    return {
        .borrowed_attempt_directory_handle = directory_descriptor,
        .expected_directory_identity = descriptor_identity(directory_descriptor),
        .observer_process_id = static_cast<std::uint64_t>(gnfs::util::process_id()),
    };
}

[[nodiscard]] package_file::DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1
native_reconciliation_request(
    int directory_descriptor,
    const package_file::DistributedSieveWorkerWorkPackageResidueWitnessV1* expected_residue) {
    return {
        .borrowed_attempt_directory_handle = directory_descriptor,
        .expected_directory_identity = descriptor_identity(directory_descriptor),
        .reconciler_process_id = static_cast<std::uint64_t>(gnfs::util::process_id()),
        .expected_residue = expected_residue,
    };
}

[[nodiscard]] std::string fixed_leaf() {
    return std::string(package_file::DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1);
}

void require_fixed_leaf_missing(int directory_descriptor) {
    struct stat metadata{};
    int result = -1;
    do {
        result =
            ::fstatat(directory_descriptor, fixed_leaf().c_str(), &metadata, AT_SYMLINK_NOFOLLOW);
    } while (result != 0 && errno == EINTR);
    CHECK(result != 0);
    CHECK(errno == ENOENT);
}

void write_all(int descriptor, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        CHECK(count > 0);
        offset += static_cast<std::size_t>(count);
    }
}

[[nodiscard]] std::vector<std::byte> read_leaf_bytes(int directory_descriptor,
                                                     std::string_view leaf) {
    const std::string owned_leaf(leaf);
    int descriptor = -1;
    do {
        descriptor = ::openat(directory_descriptor, owned_leaf.c_str(),
                              O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    CHECK(descriptor >= 0);
    ScopedFd held(descriptor);

    std::vector<std::byte> bytes;
    std::array<std::byte, 128> buffer{};
    while (true) {
        const ssize_t count = ::read(held.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        CHECK(count >= 0);
        if (count == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer.begin(),
                     buffer.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return bytes;
}

void create_leaf_with_bytes(int directory_descriptor, std::string_view leaf,
                            std::span<const std::byte> bytes) {
    const std::string owned_leaf(leaf);
    int descriptor = -1;
    do {
        descriptor = ::openat(directory_descriptor, owned_leaf.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (descriptor < 0 && errno == EINTR);
    CHECK(descriptor >= 0);
    {
        ScopedFd held(descriptor);
        write_all(held.get(), bytes);
    }
}

void create_sealed_leaf_with_bytes(int directory_descriptor, std::string_view leaf,
                                   std::span<const std::byte> bytes) {
    const std::string owned_leaf(leaf);
    int descriptor = -1;
    do {
        descriptor = ::openat(directory_descriptor, owned_leaf.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (descriptor < 0 && errno == EINTR);
    CHECK(descriptor >= 0);
    {
        ScopedFd held(descriptor);
        write_all(held.get(), bytes);
        CHECK(::fchmod(held.get(), 0400) == 0);
    }
}

struct RealReplacementHookContext final {
    int directory_descriptor = -1;
    const std::vector<std::byte>* bytes = nullptr;
    bool completed = false;
    std::size_t calls = 0;
};

void replace_or_recreate_real_residue(void* opaque) noexcept {
    auto& context = *static_cast<RealReplacementHookContext*>(opaque);
    ++context.calls;
    context.completed = false;
    if (context.directory_descriptor < 0 || context.bytes == nullptr) {
        return;
    }
    constexpr auto leaf = package_file::DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1;
    if (::unlinkat(context.directory_descriptor, leaf.data(), 0) != 0 && errno != ENOENT) {
        return;
    }

    int descriptor = -1;
    do {
        descriptor = ::openat(context.directory_descriptor, leaf.data(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return;
    }

    std::size_t offset = 0;
    bool good = true;
    while (offset < context.bytes->size()) {
        const ssize_t count =
            ::write(descriptor, context.bytes->data() + static_cast<std::ptrdiff_t>(offset),
                    context.bytes->size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            good = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (good && ::fchmod(descriptor, 0400) != 0) {
        good = false;
    }
    if (::close(descriptor) != 0) {
        good = false;
    }
    context.completed = good;
}

void test_real_posix_residue_reconciliation_present_absent_and_nlink() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);
    TempDirectory temp;
    auto directory = temp.open_directory();
    create_sealed_leaf_with_bytes(directory.get(), fixed_leaf(), encoded.package->bytes);

    const auto inspected = package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
        native_residue_request(directory.get()));
    CHECK(inspected);
    CHECK(inspected.witness.has_value());

    int retained_descriptor = -1;
    do {
        retained_descriptor = ::openat(directory.get(), fixed_leaf().c_str(),
                                       O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    } while (retained_descriptor < 0 && errno == EINTR);
    CHECK(retained_descriptor >= 0);
    ScopedFd retained(retained_descriptor);
    struct stat before{};
    CHECK(::fstat(retained.get(), &before) == 0);
    CHECK(before.st_nlink == 1);
    const auto baseline = snapshot_open_descriptors();

    const auto removed = package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
        native_reconciliation_request(directory.get(), &*inspected.witness));
    CHECK(removed);
    CHECK(
        removed.disposition ==
        package_file::DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1::removed);
    require_fixed_leaf_missing(directory.get());
    struct stat after{};
    CHECK(::fstat(retained.get(), &after) == 0);
    CHECK(after.st_dev == before.st_dev);
    CHECK(after.st_ino == before.st_ino);
    CHECK(after.st_nlink == 0);
    CHECK(snapshot_open_descriptors() == baseline);

    const auto absent = package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
        native_reconciliation_request(directory.get(), nullptr));
    CHECK(absent);
    CHECK(absent.disposition ==
          package_file::DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1::
              confirmed_absent);
    require_fixed_leaf_missing(directory.get());
    CHECK(snapshot_open_descriptors() == baseline);
}

void test_real_posix_residue_reconciliation_faults_and_retry() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    for (const auto fault :
         {package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
              after_name_unlinked,
          package_file::DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1::
              after_directory_durable}) {
        TempDirectory temp;
        auto directory = temp.open_directory();
        create_sealed_leaf_with_bytes(directory.get(), fixed_leaf(), encoded.package->bytes);
        const auto inspected =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
                native_residue_request(directory.get()));
        CHECK(inspected);
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .stop_after = fault};
        const auto interrupted =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
                native_reconciliation_request(directory.get(), &*inspected.witness), hooks);
        CHECK(!interrupted);
        CHECK(interrupted.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::interrupted);
        CHECK(interrupted.fault_point == fault);
        require_fixed_leaf_missing(directory.get());
        const auto retried =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
                native_reconciliation_request(directory.get(), nullptr));
        CHECK(retried);
    }

    for (const bool present : {false, true}) {
        TempDirectory temp;
        auto directory = temp.open_directory();
        std::optional<package_file::DistributedSieveWorkerWorkPackageResidueWitnessV1> witness;
        if (present) {
            create_sealed_leaf_with_bytes(directory.get(), fixed_leaf(), encoded.package->bytes);
            const auto inspected =
                package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
                    native_residue_request(directory.get()));
            CHECK(inspected);
            witness = *inspected.witness;
        }
        FakeReconciliationHookContext context;
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .fail_before_directory_sync = select_directory_sync_failure,
            .context = &context,
        };
        const auto injected =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
                native_reconciliation_request(directory.get(),
                                              witness.has_value() ? &*witness : nullptr),
                hooks);
        CHECK(!injected);
        CHECK(injected.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::durability_failed);
        CHECK(injected.diagnostic.native_error == EIO);
        CHECK(context.calls == 1);
        require_fixed_leaf_missing(directory.get());
        const auto retried =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
                native_reconciliation_request(directory.get(), nullptr));
        CHECK(retried);
    }
}

void test_real_posix_residue_reconciliation_revalidates_hooks() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        create_sealed_leaf_with_bytes(directory.get(), fixed_leaf(), encoded.package->bytes);
        const auto inspected =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
                native_residue_request(directory.get()));
        CHECK(inspected);
        const auto original_identity = inspected.witness->file_identity;
        RealReplacementHookContext context{
            .directory_descriptor = directory.get(),
            .bytes = &encoded.package->bytes,
        };
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .before_unlink = replace_or_recreate_real_residue,
            .context = &context,
        };
        const auto replaced =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
                native_reconciliation_request(directory.get(), &*inspected.witness), hooks);
        CHECK(!replaced);
        CHECK(context.calls == 1);
        CHECK(context.completed);
        struct stat successor{};
        CHECK(::fstatat(directory.get(), fixed_leaf().c_str(), &successor, AT_SYMLINK_NOFOLLOW) ==
              0);
        CHECK(static_cast<std::uint64_t>(successor.st_ino) != original_identity.object);
        CHECK(read_leaf_bytes(directory.get(), fixed_leaf()) == encoded.package->bytes);
    }

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        create_sealed_leaf_with_bytes(directory.get(), fixed_leaf(), encoded.package->bytes);
        const auto inspected =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
                native_residue_request(directory.get()));
        CHECK(inspected);
        RealReplacementHookContext context{
            .directory_descriptor = directory.get(),
            .bytes = &encoded.package->bytes,
        };
        const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks{
            .after_directory_durable = replace_or_recreate_real_residue,
            .context = &context,
        };
        const auto recreated =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
                native_reconciliation_request(directory.get(), &*inspected.witness), hooks);
        CHECK(!recreated);
        CHECK(context.calls == 1);
        CHECK(context.completed);
        CHECK(read_leaf_bytes(directory.get(), fixed_leaf()) == encoded.package->bytes);
    }
}

void test_real_posix_residue_success_missing_and_fd_hygiene() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        create_sealed_leaf_with_bytes(directory.get(), fixed_leaf(), encoded.package->bytes);
        struct stat before_metadata{};
        CHECK(::fstatat(directory.get(), fixed_leaf().c_str(), &before_metadata,
                        AT_SYMLINK_NOFOLLOW) == 0);
        const auto before_descriptors = snapshot_open_descriptors();

        const auto result = package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
            native_residue_request(directory.get()));
        CHECK(result);
        CHECK(result.witness.has_value());
        const sieve::NativeIdentityV1 expected_file_identity{
            .volume = static_cast<std::uint64_t>(before_metadata.st_dev),
            .object = static_cast<std::uint64_t>(before_metadata.st_ino),
            .generation = 0,
        };
        check_residue_witness(*result.witness, *encoded.package, expected_file_identity,
                              static_cast<std::uint64_t>(::geteuid()));
        CHECK(snapshot_open_descriptors() == before_descriptors);
        CHECK(read_leaf_bytes(directory.get(), fixed_leaf()) == encoded.package->bytes);
        struct stat after_metadata{};
        CHECK(::fstatat(directory.get(), fixed_leaf().c_str(), &after_metadata,
                        AT_SYMLINK_NOFOLLOW) == 0);
        CHECK(before_metadata.st_dev == after_metadata.st_dev);
        CHECK(before_metadata.st_ino == after_metadata.st_ino);
        CHECK(before_metadata.st_mode == after_metadata.st_mode);
        CHECK(before_metadata.st_nlink == after_metadata.st_nlink);
    }

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        const auto before_descriptors = snapshot_open_descriptors();
        const auto result = package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
            native_residue_request(directory.get()));
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(result.diagnostic.native_error == ENOENT);
        CHECK(!result.diagnostic.named_may_remain);
        CHECK(snapshot_open_descriptors() == before_descriptors);
        require_fixed_leaf_missing(directory.get());
    }
}

void test_real_posix_residue_rejects_namespace_shapes_and_corruption() {
    const auto identity = make_identity();
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        CHECK(::mkdirat(directory.get(), fixed_leaf().c_str(), 0700) == 0);
        const auto baseline = snapshot_open_descriptors();
        const auto result = package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
            native_residue_request(directory.get()));
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(snapshot_open_descriptors() == baseline);
    }

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        constexpr std::string_view target_leaf = "sealed-target";
        create_sealed_leaf_with_bytes(directory.get(), target_leaf, encoded.package->bytes);
        CHECK(::symlinkat(std::string(target_leaf).c_str(), directory.get(),
                          fixed_leaf().c_str()) == 0);
        const auto baseline = snapshot_open_descriptors();
        const auto result = package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
            native_residue_request(directory.get()));
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(snapshot_open_descriptors() == baseline);
        CHECK(read_leaf_bytes(directory.get(), target_leaf) == encoded.package->bytes);
    }

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        constexpr std::string_view target_leaf = "hardlink-target";
        create_sealed_leaf_with_bytes(directory.get(), target_leaf, encoded.package->bytes);
        CHECK(::linkat(directory.get(), std::string(target_leaf).c_str(), directory.get(),
                       fixed_leaf().c_str(), 0) == 0);
        const auto baseline = snapshot_open_descriptors();
        const auto result = package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
            native_residue_request(directory.get()));
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(snapshot_open_descriptors() == baseline);
        CHECK(read_leaf_bytes(directory.get(), target_leaf) == encoded.package->bytes);
    }

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        create_leaf_with_bytes(directory.get(), fixed_leaf(), encoded.package->bytes);
        const auto baseline = snapshot_open_descriptors();
        const auto result = package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
            native_residue_request(directory.get()));
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(snapshot_open_descriptors() == baseline);
        CHECK(read_leaf_bytes(directory.get(), fixed_leaf()) == encoded.package->bytes);
    }

    std::array<std::vector<std::byte>, 3> corruptions{
        encoded.package->bytes,
        encoded.package->bytes,
        encoded.package->bytes,
    };
    corruptions[0].pop_back();
    corruptions[1].push_back(std::byte{0});
    corruptions[2].back() ^= std::byte{0x01};
    for (const auto& bytes : corruptions) {
        TempDirectory temp;
        auto directory = temp.open_directory();
        create_sealed_leaf_with_bytes(directory.get(), fixed_leaf(), bytes);
        const auto baseline = snapshot_open_descriptors();
        const auto result = package_file::inspect_distributed_sieve_worker_work_package_residue_v1(
            native_residue_request(directory.get()));
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::decode_failed);
        CHECK(snapshot_open_descriptors() == baseline);
        CHECK(read_leaf_bytes(directory.get(), fixed_leaf()) == bytes);
    }
}

void test_real_posix_residue_rejects_forked_observer() {
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(make_identity());
    CHECK(encoded);
    TempDirectory temp;
    auto directory = temp.open_directory();
    create_sealed_leaf_with_bytes(directory.get(), fixed_leaf(), encoded.package->bytes);
    const auto request = native_residue_request(directory.get());

    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const auto result =
            package_file::inspect_distributed_sieve_worker_work_package_residue_v1(request);
        const bool rejected =
            !result &&
            result.diagnostic.status ==
                package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request &&
            result.diagnostic.native_error == ECHILD;
        ::_exit(rejected ? 0 : 1);
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(read_leaf_bytes(directory.get(), fixed_leaf()) == encoded.package->bytes);

    const auto parent_result =
        package_file::inspect_distributed_sieve_worker_work_package_residue_v1(request);
    CHECK(parent_result);

    const auto reconciliation_request =
        native_reconciliation_request(directory.get(), &*parent_result.witness);
    const pid_t reconciliation_child = ::fork();
    CHECK(reconciliation_child >= 0);
    if (reconciliation_child == 0) {
        const auto result =
            package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
                reconciliation_request);
        const bool rejected =
            !result &&
            result.diagnostic.status ==
                package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request &&
            result.diagnostic.native_error == ECHILD;
        ::_exit(rejected ? 0 : 1);
    }
    status = 0;
    CHECK(::waitpid(reconciliation_child, &status, 0) == reconciliation_child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(read_leaf_bytes(directory.get(), fixed_leaf()) == encoded.package->bytes);
}

void test_real_posix_token_lifetime_revalidation_and_reuse() {
    const auto identity = make_identity();
    const auto expected = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(expected);

    TempDirectory temp;
    auto directory = temp.open_directory();
    const auto request = native_request(directory.get());
    const auto baseline = snapshot_open_descriptors();

    {
        auto first_result =
            package_file::create_distributed_sieve_worker_work_package_file_v1(request, identity);
        CHECK(first_result);
        CHECK(first_result.file.has_value());
        CHECK(descriptor_is_open(directory.get()));
        require_fixed_leaf_missing(directory.get());
        check_package_witness(first_result.file->witness(), *expected.package,
                              first_result.file->witness().file_identity,
                              request.creator_process_id);
        CHECK(first_result.file->owned_by_current_process());
        CHECK(first_result.file->revalidate());
        CHECK(added_descriptors(baseline, snapshot_open_descriptors()).size() == 1);

        auto first = std::move(*first_result.file);
        first_result.file.reset();
        CHECK(first.owned_by_current_process());
        CHECK(first.revalidate());

        auto moved = std::move(first);
        CHECK(!first.owned_by_current_process());
        CHECK(moved.owned_by_current_process());
        CHECK(moved.revalidate());

        auto second_result =
            package_file::create_distributed_sieve_worker_work_package_file_v1(request, identity);
        CHECK(second_result);
        CHECK(second_result.file.has_value());
        CHECK(second_result.file->owned_by_current_process());
        CHECK(second_result.file->revalidate());
        require_fixed_leaf_missing(directory.get());
        CHECK(second_result.file->witness().file_identity != moved.witness().file_identity);
        CHECK(added_descriptors(baseline, snapshot_open_descriptors()).size() == 2);
        CHECK(descriptor_is_open(directory.get()));
    }

    CHECK(snapshot_open_descriptors() == baseline);
    CHECK(descriptor_is_open(directory.get()));
}

void test_real_posix_foreign_leaves_are_preserved() {
    const auto identity = make_identity();
    constexpr std::array<std::byte, 4> sentinel{
        std::byte{0xde},
        std::byte{0xad},
        std::byte{0xbe},
        std::byte{0xef},
    };

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        const auto request = native_request(directory.get());
        create_leaf_with_bytes(directory.get(), fixed_leaf(), sentinel);
        struct stat before{};
        CHECK(::fstatat(directory.get(), fixed_leaf().c_str(), &before, AT_SYMLINK_NOFOLLOW) == 0);

        const auto baseline = snapshot_open_descriptors();
        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1(request, identity);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        CHECK(snapshot_open_descriptors() == baseline);
        CHECK(read_leaf_bytes(directory.get(), fixed_leaf()) ==
              std::vector<std::byte>(sentinel.begin(), sentinel.end()));
        struct stat after{};
        CHECK(::fstatat(directory.get(), fixed_leaf().c_str(), &after, AT_SYMLINK_NOFOLLOW) == 0);
        CHECK(before.st_dev == after.st_dev);
        CHECK(before.st_ino == after.st_ino);
    }

    {
        TempDirectory temp;
        auto directory = temp.open_directory();
        const auto request = native_request(directory.get());
        constexpr std::string_view target_leaf = "foreign-target.bin";
        create_leaf_with_bytes(directory.get(), target_leaf, sentinel);
        CHECK(::symlinkat(std::string(target_leaf).c_str(), directory.get(),
                          fixed_leaf().c_str()) == 0);
        struct stat before{};
        CHECK(::fstatat(directory.get(), fixed_leaf().c_str(), &before, AT_SYMLINK_NOFOLLOW) == 0);
        CHECK(S_ISLNK(before.st_mode));

        const auto result =
            package_file::create_distributed_sieve_worker_work_package_file_v1(request, identity);
        CHECK(!result);
        CHECK(result.diagnostic.status ==
              package_file::DistributedSieveWorkerWorkPackageFileStatus::namespace_conflict);
        struct stat after{};
        CHECK(::fstatat(directory.get(), fixed_leaf().c_str(), &after, AT_SYMLINK_NOFOLLOW) == 0);
        CHECK(S_ISLNK(after.st_mode));
        CHECK(before.st_dev == after.st_dev);
        CHECK(before.st_ino == after.st_ino);
        CHECK(read_leaf_bytes(directory.get(), target_leaf) ==
              std::vector<std::byte>(sentinel.begin(), sentinel.end()));
    }
}

void test_real_posix_rejects_directory_mode_and_identity_drift() {
    const auto identity = make_identity();
    TempDirectory temp;
    auto directory = temp.open_directory();
    const auto request = native_request(directory.get());

    CHECK(::chmod(temp.path().c_str(), 0755) == 0);
    const auto wrong_mode =
        package_file::create_distributed_sieve_worker_work_package_file_v1(request, identity);
    CHECK(!wrong_mode);
    CHECK(wrong_mode.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
    require_fixed_leaf_missing(directory.get());

    CHECK(::chmod(temp.path().c_str(), 0700) == 0);
    auto wrong_identity_request = request;
    wrong_identity_request.expected_directory_identity.object += 1;
    const auto wrong_identity = package_file::create_distributed_sieve_worker_work_package_file_v1(
        wrong_identity_request, identity);
    CHECK(!wrong_identity);
    CHECK(wrong_identity.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::invalid_request);
    require_fixed_leaf_missing(directory.get());
}

#else

void test_windows_reports_platform_unavailable() {
    const auto identity = make_identity();
    const package_file::DistributedSieveWorkerWorkPackageFileRequestV1 request{
        .borrowed_attempt_directory_handle = 1,
        .expected_directory_identity = {.volume = 1, .object = 1, .generation = 0},
        .creator_process_id = static_cast<std::uint64_t>(gnfs::util::process_id()),
    };
    const auto result =
        package_file::create_distributed_sieve_worker_work_package_file_v1(request, identity);
    CHECK(!result);
    CHECK(result.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::platform_unavailable);

    const package_file::DistributedSieveWorkerWorkPackageResidueInspectionRequestV1 residue_request{
        .borrowed_attempt_directory_handle = 1,
        .expected_directory_identity = {.volume = 1, .object = 1, .generation = 0},
        .observer_process_id = static_cast<std::uint64_t>(gnfs::util::process_id()),
    };
    const auto residue =
        package_file::inspect_distributed_sieve_worker_work_package_residue_v1(residue_request);
    CHECK(!residue);
    CHECK(residue.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::platform_unavailable);

    const package_file::DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1
        reconciliation_request{
            .borrowed_attempt_directory_handle = 1,
            .expected_directory_identity = {.volume = 1, .object = 1, .generation = 0},
            .reconciler_process_id = static_cast<std::uint64_t>(gnfs::util::process_id()),
            .expected_residue = nullptr,
        };
    const auto reconciliation =
        package_file::reconcile_distributed_sieve_worker_work_package_residue_v1(
            reconciliation_request);
    CHECK(!reconciliation);
    CHECK(reconciliation.diagnostic.status ==
          package_file::DistributedSieveWorkerWorkPackageFileStatus::platform_unavailable);
}

#endif

} // namespace

int main() {
    try {
        test_authority_boundary_and_fixed_contract();
        test_fake_happy_path_seals_unlinks_and_closes_reader();
        test_fake_critical_event_order_is_frozen();
        test_fake_recreated_name_preserves_named_may_remain_or_semantics();
        test_fake_reader_policy_rejects_writable_or_inheritable_descriptors();
        test_fake_valid_decode_with_mismatched_witness_fails_before_unlink();
        test_fake_buffered_pwrite_retries_short_writes_and_rejects_zero();
        test_fake_request_directory_pid_and_acl_rejection();
        test_fake_exclusive_create_identity_drift_and_foreign_preservation();
        test_fake_close_is_single_shot_and_preserves_primary_failure();
        test_fake_residue_inspector_happy_path_and_equality();
        test_fake_residue_inspector_rejects_missing_metadata_acl_and_policy();
        test_fake_residue_inspector_revalidates_decode_boundary();
        test_fake_residue_reconciliation_present_absent_and_idempotent();
        test_fake_residue_reconciliation_crash_prefixes_retry_to_absence();
        test_fake_residue_reconciliation_sync_failures_are_not_success();
        test_fake_residue_reconciliation_rejects_drift_without_blind_unlink();
        test_fake_residue_reconciliation_closes_all_hook_and_post_unlink_seams();
#if !defined(_WIN32)
        test_real_posix_residue_reconciliation_present_absent_and_nlink();
        test_real_posix_residue_reconciliation_faults_and_retry();
        test_real_posix_residue_reconciliation_revalidates_hooks();
        test_real_posix_residue_success_missing_and_fd_hygiene();
        test_real_posix_residue_rejects_namespace_shapes_and_corruption();
        test_real_posix_residue_rejects_forked_observer();
        test_real_posix_token_lifetime_revalidation_and_reuse();
        test_real_posix_foreign_leaves_are_preserved();
        test_real_posix_rejects_directory_mode_and_identity_drift();
#else
        test_windows_reports_platform_unavailable();
#endif
        std::cout << "distributed sieve worker work-package file tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
