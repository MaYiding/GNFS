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
    std::uint32_t file_mode = 0600;
    std::uint64_t file_link_count = 0;
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
        return {.operation = succeeded(), .metadata = file_metadata()};
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
        return {.operation = succeeded(), .metadata = file_metadata()};
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
        return {.operation = succeeded(), .has_extended_acl = false};
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
        return {
            .operation = succeeded(),
            .decoded = package_codec::decode_distributed_sieve_work_package_v1(bytes),
        };
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
        if (!unlink_script.empty()) {
            return pop(unlink_script);
        }
        if (directory_handle != DIRECTORY_HANDLE || !named_exists) {
            return failed(ENOENT);
        }
        named_exists = false;
        file_link_count = 0;
        return succeeded();
    }

private:
    template <typename T> [[nodiscard]] static T pop(std::deque<T>& script) noexcept {
        T result = std::move(script.front());
        script.pop_front();
        return result;
    }

    [[nodiscard]] FileMetadata file_metadata() const noexcept {
        return {
            .kind = package_file::DistributedSieveWorkerWorkPackageObjectKind::regular_file,
            .identity = FILE_IDENTITY,
            .owner_user_id = user_id,
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

void test_authority_boundary_and_fixed_contract() {
    using Token = package_file::DistributedSieveWorkerWorkPackageFileV1;
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
#if !defined(_WIN32)
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
