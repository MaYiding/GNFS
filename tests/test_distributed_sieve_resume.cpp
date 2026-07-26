#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include "distributed_sieve_wave_store_internal.hpp"
#include "ooc_private_handoff_cleanup_authorization_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <membership.h>
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

namespace {

namespace sieve = gnfs::sieve;
namespace cleanup_detail = gnfs::relation::ooc_cleanup_detail;
namespace wave_detail = gnfs::sieve::distributed_sieve_resume_detail;
using Digest = gnfs::util::Sha256Digest;
using Record = sieve::DistributedSieveProtocolRecordV1;
using Status = sieve::DistributedSieveProtocolStatus;
using ExternalCleanupAuthorizationState =
    wave_detail::DistributedSieveExternalCleanupAuthorizationState;

static_assert(!noexcept(sieve::distributed_sieve_record_kind(std::declval<const Record&>())));
static_assert(
    std::is_default_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding>);
static_assert(
    std::is_copy_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding>);
static_assert(
    !std::is_default_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey>);
static_assert(
    !std::is_copy_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey>);
static_assert(
    !std::is_move_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey>);
static_assert(
    !std::is_move_assignable_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey>);
static_assert(
    !std::is_default_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(
    !std::is_copy_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(
    !std::is_copy_assignable_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(std::is_nothrow_move_constructible_v<
              cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(
    !std::is_move_assignable_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>);
static_assert(
    !std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                             cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       std::shared_ptr<const ExternalCleanupAuthorizationState>>);
static_assert(
    !std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                             cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey&&,
                             cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding,
                             std::shared_ptr<const ExternalCleanupAuthorizationState>>);
static_assert(!std::is_constructible_v<
              cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
              cleanup_detail::OOCPrivateHandoffCleanupAuthorizationMintKey&&,
              cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding,
              std::shared_ptr<const ExternalCleanupAuthorizationState>, std::uint64_t>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       sieve::ArtifactCleanupAuthorizedV1>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       gnfs::relation::OOCPrivateHandoffRecordV1>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       gnfs::relation::OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt,
                                       std::filesystem::path>);
static_assert(
    !std::is_constructible_v<cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt, Digest>);
static_assert(!std::is_default_constructible_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_copy_constructible_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_copy_assignable_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_move_constructible_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_move_assignable_v<ExternalCleanupAuthorizationState>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState,
                                       sieve::ArtifactCleanupAuthorizedV1>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState,
                                       gnfs::relation::OOCPrivateHandoffRecordV1>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState,
                                       gnfs::relation::OOCPrivateHandoffAdoptionReceipt>);
static_assert(
    !std::is_constructible_v<ExternalCleanupAuthorizationState,
                             cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState, std::filesystem::path>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState, Digest>);
static_assert(
    !std::is_constructible_v<ExternalCleanupAuthorizationState, std::shared_ptr<const void>>);
static_assert(!std::is_constructible_v<ExternalCleanupAuthorizationState,
                                       std::shared_ptr<const void>, std::uint64_t>);
static_assert(!std::is_default_constructible_v<wave_detail::DistributedSieveWaveStore>);
static_assert(!std::is_copy_constructible_v<wave_detail::DistributedSieveWaveStore>);
static_assert(!std::is_copy_assignable_v<wave_detail::DistributedSieveWaveStore>);
static_assert(!std::is_move_constructible_v<wave_detail::DistributedSieveWaveStore>);
static_assert(!std::is_move_assignable_v<wave_detail::DistributedSieveWaveStore>);

constexpr std::array WAVE_STORE_FAULT_POINTS{
    wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable,
    wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable,
    wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestPendingDurable,
    wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestCanonicalPromoted,
    wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestCanonicalDurable,
};
static_assert(WAVE_STORE_FAULT_POINTS.size() ==
              static_cast<std::size_t>(wave_detail::DistributedSieveWaveStoreFaultPoint::Count));
static_assert([] {
    for (std::size_t index = 0; index < WAVE_STORE_FAULT_POINTS.size(); ++index) {
        if (static_cast<std::size_t>(WAVE_STORE_FAULT_POINTS[index]) != index) {
            return false;
        }
    }
    return true;
}());

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view expression, int line, std::string_view detail = {}) {
    std::string message = "CHECK failed at line " + std::to_string(line) + ": ";
    message.append(expression);
    if (!detail.empty()) {
        message.append(" (");
        message.append(detail);
        message.push_back(')');
    }
    throw TestFailure(message);
}

void check(bool condition, std::string_view expression, int line) {
    if (!condition) {
        fail(expression, line);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void require_ok(const Status& status, std::string_view context) {
    if (!status) {
        fail(context, __LINE__, sieve::distributed_sieve_protocol_error_name(status.error));
    }
}

void require_failed(const Status& status, std::string_view context) {
    if (status || status.error == sieve::DistributedSieveProtocolError::none) {
        fail(context, __LINE__, "operation unexpectedly succeeded");
    }
}

[[nodiscard]] Digest digest_with_seed(uint8_t seed) {
    Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        const auto value = static_cast<uint8_t>(seed + static_cast<uint8_t>(index));
        digest.bytes[index] = static_cast<std::byte>(value);
    }
    return digest;
}

void perturb_digest(Digest& digest) {
    digest.bytes.front() ^= std::byte{0x80};
}

[[nodiscard]] sieve::WaveIdV1 wave_id_with_seed(uint8_t seed) {
    sieve::WaveIdV1 wave_id;
    for (std::size_t index = 0; index < wave_id.bytes.size(); ++index) {
        const auto value = static_cast<uint8_t>(seed + static_cast<uint8_t>(index));
        wave_id.bytes[index] = static_cast<std::byte>(value);
    }
    return wave_id;
}

[[nodiscard]] sieve::NativeIdentityV1 native_identity(uint64_t seed) {
    sieve::NativeIdentityV1 identity;
    identity.volume = seed;
    identity.object = seed + 1;
    identity.generation = seed + 2;
    return identity;
}

[[nodiscard]] constexpr sieve::LeaseIdV1 lease_id_with_seed(uint64_t seed) noexcept {
    return sieve::LeaseIdV1{{seed + 1, seed + 2}};
}

[[nodiscard]] sieve::LeaseIdentityV1 lease_identity(uint64_t seed, std::string stem) {
    sieve::LeaseIdentityV1 lease;
    lease.lease_id = lease_id_with_seed(seed);
    lease.owner_marker = native_identity(seed + 10);
    lease.directory = native_identity(seed + 20);
    lease.relative_stem = std::move(stem);
    return lease;
}

[[nodiscard]] sieve::CorpusArtifactV1 corpus_artifact(uint64_t seed, uint64_t relation_count) {
    sieve::CorpusArtifactV1 artifact;
    artifact.descriptor.format_version = 1;
    artifact.descriptor.store_id = seed + 1;
    artifact.descriptor.generation = seed + 2;
    artifact.descriptor.relation_count = relation_count;
    artifact.descriptor.data_end = 4096 + relation_count;
    artifact.index_file.identity = native_identity(seed + 30);
    artifact.index_file.extent = 128 + relation_count * 16;
    artifact.data_file.identity = native_identity(seed + 40);
    artifact.data_file.extent = artifact.descriptor.data_end;
    artifact.sequence_receipt.relation_count = relation_count;
    artifact.sequence_receipt.low = seed + 50;
    artifact.sequence_receipt.high = seed + 51;
    artifact.corpus_sha256 = digest_with_seed(static_cast<uint8_t>(seed));
    return artifact;
}

template <typename T> [[nodiscard]] T seal_value(T value) {
    Record record = std::move(value);
    require_ok(sieve::seal_distributed_sieve_record(record), "seal record");
    return std::get<T>(std::move(record));
}

template <typename T>
[[nodiscard]] Status validate_value(const T& value, bool verify_self_digest = true) {
    return sieve::validate_distributed_sieve_record(Record{value}, verify_self_digest);
}

template <typename T> [[nodiscard]] T reseal(T value) {
    value.self_digest = {};
    return seal_value(std::move(value));
}

template <typename T> void require_reseal_failed(T value, std::string_view context) {
    value.self_digest = {};
    Record record = std::move(value);
    require_failed(sieve::seal_distributed_sieve_record(record), context);
}

[[nodiscard]] std::vector<std::byte> encode_or_fail(const Record& record) {
    const auto encoded = sieve::encode_distributed_sieve_record(record);
    if (!encoded) {
        fail("encode_distributed_sieve_record", __LINE__,
             sieve::distributed_sieve_protocol_error_name(encoded.status.error));
    }
    return *encoded.bytes;
}

[[nodiscard]] Digest record_digest_or_fail(const Record& record) {
    const auto digest = sieve::distributed_sieve_record_digest(record);
    if (!digest) {
        fail("distributed_sieve_record_digest", __LINE__,
             sieve::distributed_sieve_protocol_error_name(digest.status.error));
    }
    return *digest.digest;
}

[[nodiscard]] const Digest& self_digest(const Record& record) {
    return std::visit([](const auto& value) -> const Digest& { return value.self_digest; }, record);
}

[[nodiscard]] constexpr uint64_t binary64_bits(double value) noexcept {
    return std::bit_cast<uint64_t>(value);
}

struct PolicySettingFixture final {
    sieve::ExecutionPolicyKeyV1 key;
    sieve::ExecutionPolicyScalarKindV1 kind;
    uint64_t baseline;
    uint64_t alternate;
};

constexpr std::array<PolicySettingFixture,
                     sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
    POLICY_SETTINGS = {{
        {sieve::ExecutionPolicyKeyV1::lattice_lll, sieve::ExecutionPolicyScalarKindV1::closed_mode,
         1, 2},
        {sieve::ExecutionPolicyKeyV1::lattice_skew, sieve::ExecutionPolicyScalarKindV1::boolean, 1,
         0},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice_threshold,
         sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.5),
         binary64_bits(0.75)},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice_max_retries,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::adaptive_lattice_seed,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 123, 124},
        {sieve::ExecutionPolicyKeyV1::survival_filter, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0},
        {sieve::ExecutionPolicyKeyV1::survival_threshold,
         sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.125),
         binary64_bits(0.25)},
        {sieve::ExecutionPolicyKeyV1::cofactor_brent, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0},
        {sieve::ExecutionPolicyKeyV1::ecm_brent_suyama, sieve::ExecutionPolicyScalarKindV1::boolean,
         1, 0},
        {sieve::ExecutionPolicyKeyV1::ecm_bs_degree,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 12, 30},
        {sieve::ExecutionPolicyKeyV1::ecm_sigma_pool_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16, 32},
        {sieve::ExecutionPolicyKeyV1::ecm_curve_pool,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 8, 16},
        {sieve::ExecutionPolicyKeyV1::ecm_batch_inv, sieve::ExecutionPolicyScalarKindV1::boolean, 1,
         0},
        {sieve::ExecutionPolicyKeyV1::cofactor_batch_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 64, 128},
        {sieve::ExecutionPolicyKeyV1::brent_pollard_rho_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::ecm_b1_cache_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16, 32},
        {sieve::ExecutionPolicyKeyV1::ecm_stage1_parallel_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::ecm_stage2_parallel,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 1, 2},
        {sieve::ExecutionPolicyKeyV1::cofactor_result_cache_size,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 128, 256},
        {sieve::ExecutionPolicyKeyV1::trial_div_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
        {sieve::ExecutionPolicyKeyV1::lattice_basis_parallel_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::lattice_coords_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
        {sieve::ExecutionPolicyKeyV1::sieve_apply_tile_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::bucket_prefetch,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
        {sieve::ExecutionPolicyKeyV1::sieve_ecore_threads,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2, 3},
        {sieve::ExecutionPolicyKeyV1::sieve_no_tiny_simd,
         sieve::ExecutionPolicyScalarKindV1::boolean, 1, 0},
        {sieve::ExecutionPolicyKeyV1::sieve_norm_tile_bits,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4, 5},
        {sieve::ExecutionPolicyKeyV1::sieve_region_tile_bits,
         sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4, 5},
        {sieve::ExecutionPolicyKeyV1::sieve_saturated_sub_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
        {sieve::ExecutionPolicyKeyV1::sieve_count_above_threshold_simd,
         sieve::ExecutionPolicyScalarKindV1::closed_mode, 1, 2},
    }};

constexpr std::array<uint64_t, sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
    ALL_UNSET_POLICY_BITS = {
        2, 0, 0, binary64_bits(0.5),
        2, 0, 0, binary64_bits(0.0),
        0, 0, 0, 0,
        0, 0, 1, 1,
        0, 1, 1, 0,
        1, 1, 1, 1,
        1, 0, 0, 0,
        0, 1, 1,
};

static_assert(POLICY_SETTINGS.size() == sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1);
static_assert(ALL_UNSET_POLICY_BITS.size() == POLICY_SETTINGS.size());

[[nodiscard]] sieve::DistributedSieveExecutionPolicyV1 make_execution_policy() {
    sieve::DistributedSieveExecutionPolicyV1 policy;
    policy.settings.reserve(POLICY_SETTINGS.size());
    for (std::size_t index = 0; index < POLICY_SETTINGS.size(); ++index) {
        const auto& spec = POLICY_SETTINGS[index];
        CHECK(static_cast<uint16_t>(spec.key) == index + 1);
        policy.settings.push_back({spec.key, spec.kind, spec.baseline});
    }
    require_ok(sieve::validate_distributed_sieve_execution_policy(policy),
               "baseline execution policy");
    return policy;
}

[[nodiscard]] std::vector<sieve::ChunkPlanV1> make_work_chunks() {
    return {
        sieve::ChunkPlanV1{0, 2, 3, "chunk_0"},
        sieve::ChunkPlanV1{1, 3, 5, "chunk_1"},
    };
}

[[nodiscard]] sieve::DistributedSieveWorkIdentityV1 make_work_identity() {
    sieve::DistributedSieveWorkIdentityV1 identity;
    identity.polynomial.n.decimal = "1000036000099";
    identity.polynomial.m.decimal = "10001";
    identity.polynomial.degree = 2;
    identity.polynomial.coefficients = {
        sieve::CanonicalIntegerV1{"-5"},
        sieve::CanonicalIntegerV1{"3"},
        sieve::CanonicalIntegerV1{"1"},
    };
    identity.polynomial.skewness_ieee754_bits = binary64_bits(1.25);

    identity.factor_base.rational_bound = 100;
    identity.factor_base.algebraic_bound = 200;
    identity.factor_base.large_prime_bound = 10'000;
    identity.factor_base.log_scale = 16;
    identity.factor_base.rational = {{2, 16}, {5, 25}};
    identity.factor_base.algebraic = {
        {7, 1, 37, 1},   {11, 4, 55, 2},
        {211, 3, 61, 1}, {223, std::numeric_limits<uint32_t>::max(), 67, 1},
        {227, 5, 71, 1},
    };
    identity.factor_base.sieve_algebraic_count = 2;

    identity.sieve.log_scale = 16;
    identity.sieve.rational_threshold = 50;
    identity.sieve.algebraic_threshold = 51;
    identity.sieve.large_prime_bound = 10'000;
    identity.sieve.allow_2lp = true;
    identity.sieve.allow_3lp = false;

    identity.region.i_min = -100;
    identity.region.i_max = 100;
    identity.region.j_min = 1;
    identity.region.j_max = 50;

    identity.cofactor.large_prime_bound = 10'000;
    identity.cofactor.allow_1lp = true;
    identity.cofactor.allow_2lp = true;
    identity.cofactor.allow_3lp = true;
    identity.cofactor.max_factorization_attempts = 20;

    identity.original_sq_bounds = {0, 5, 100, 1000};
    identity.effective_sq_bounds = {2, 5, 0, std::numeric_limits<uint32_t>::max()};

    identity.distributed.worker_count = 2;
    identity.distributed.chunks = make_work_chunks();
    identity.distributed.sq_cap_per_worker = 10;
    identity.distributed.relation_cap_per_worker = 100;
    identity.distributed.max_worker_attempts = 2;
    identity.distributed.max_merge_build_attempts = 2;
    identity.distributed.max_consumption_attempts = 2;
    identity.execution_policy = make_execution_policy();

    identity.semantic_versions.relation_serialization_version = 1;
    identity.semantic_versions.ooc_format_version = 1;
    identity.semantic_versions.digest_version = 1;
    identity.semantic_versions.handoff_version = 1;
    identity.semantic_versions.retry_policy_version = 1;
    identity.semantic_versions.chunking_version = 1;
    identity.semantic_versions.completion_version = 1;
    identity.semantic_versions.deduplication_version = 1;
    identity.semantic_versions.merge_policy_version = 1;

    require_ok(sieve::validate_distributed_sieve_work_identity(identity), "baseline work identity");
    return identity;
}

[[nodiscard]] Digest work_digest_or_fail(const sieve::DistributedSieveWorkIdentityV1& identity) {
    const auto result = sieve::distributed_sieve_work_digest(identity);
    if (!result) {
        fail("distributed_sieve_work_digest", __LINE__,
             sieve::distributed_sieve_protocol_error_name(result.status.error));
    }
    return *result.digest;
}

[[nodiscard]] sieve::TerminalChunkInputV1 terminal_input(uint32_t chunk_id, uint32_t begin,
                                                         uint32_t end, uint8_t seed,
                                                         uint64_t relation_count) {
    sieve::TerminalChunkInputV1 input;
    input.chunk_id = chunk_id;
    input.disposition = sieve::ChunkDispositionV1::handoff;
    input.sq_begin = begin;
    input.sq_end = end;
    input.next_sq_index = end;
    input.processed_sq_count = static_cast<uint64_t>(end - begin);
    input.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
    input.durable_attempt_count = 1;
    input.last_attempt_digest = digest_with_seed(seed);
    input.lease_id = lease_id_with_seed(static_cast<uint64_t>(seed) + 100);
    input.handoff_digest = digest_with_seed(static_cast<uint8_t>(seed + 1));
    input.raw_relation_count = relation_count;
    input.sequence_receipt.relation_count = relation_count;
    input.sequence_receipt.low = static_cast<uint64_t>(seed) + 200;
    input.sequence_receipt.high = static_cast<uint64_t>(seed) + 201;
    input.corpus_sha256 = digest_with_seed(static_cast<uint8_t>(seed + 2));
    return input;
}

struct ProtocolFixture final {
    sieve::DistributedSieveWorkIdentityV1 work_identity = make_work_identity();
    Digest work_digest = work_digest_or_fail(work_identity);
    sieve::WaveManifestV1 manifest;
    sieve::AttemptStartedV1 attempt;
    sieve::AttemptStartedV1 failure_attempt_0;
    sieve::AttemptStartedV1 failure_attempt_1;
    sieve::ChunkTerminalFailureV1 terminal_failure;
    sieve::WorkerHandoffV1 handoff;
    sieve::WorkerHandoffV1 handoff_1;
    sieve::MergeStartedV1 merge_started;
    sieve::MergePreparedV1 merge_prepared;
    sieve::WaveMergeCommitV1 merge_commit;
    std::array<sieve::ArtifactCleanupAuthorizedV1, 3> cleanup_authorizations;
    std::array<sieve::ArtifactCleanupCompletedV1, 3> cleanup_completions;
    sieve::ConsumptionStartedV1 consumption_started;
    sieve::SuccessorPreparedV1 successor_prepared;
    sieve::WaveConsumptionAckV1 consumption_ack;
    sieve::WaveCompletedV1 completed;

    ProtocolFixture() {
        manifest.wave_id = wave_id_with_seed(1);
        manifest.execution_contract_version = 1;
        manifest.executable_sha256 = digest_with_seed(2);
        manifest.work_sha256 = work_digest;
        manifest.wave_root_identity = native_identity(10);
        manifest.permanent_lock_identity = native_identity(20);
        manifest.lock_semantics_version = 1;
        manifest.effective_sq_begin = 2;
        manifest.effective_sq_end = 5;
        manifest.worker_count = 2;
        manifest.chunks = {
            sieve::ChunkPlanV1{0, 2, 3, "chunk_0"},
            sieve::ChunkPlanV1{1, 3, 5, "chunk_1"},
        };
        manifest.sq_cap_per_worker = 10;
        manifest.relation_cap_per_worker = 100;
        manifest.max_worker_attempts = 2;
        manifest.max_merge_build_attempts = 2;
        manifest.max_consumption_attempts = 2;
        manifest.canonical_naming_version = 1;
        manifest.retry_policy_version = 1;
        manifest.durable_start_consumes_ordinal = true;
        manifest.ooc_format_version = 1;
        manifest.relation_serialization_version = 1;
        manifest.handoff_version = 1;
        manifest.receipt_version = 1;
        manifest.digest_version = 1;
        manifest.merge_policy_version = 1;
        manifest = seal_value(std::move(manifest));

        attempt.manifest_digest = manifest.self_digest;
        attempt.chunk_id = 0;
        attempt.sq_begin = 2;
        attempt.sq_end = 3;
        attempt.attempt_ordinal = 0;
        attempt.predecessor_digest = manifest.self_digest;
        attempt.lease = lease_identity(100, "chunk_0_attempt_00");
        attempt.retry_policy_version = 1;
        attempt = seal_value(std::move(attempt));

        failure_attempt_0.manifest_digest = manifest.self_digest;
        failure_attempt_0.chunk_id = 1;
        failure_attempt_0.sq_begin = 3;
        failure_attempt_0.sq_end = 5;
        failure_attempt_0.attempt_ordinal = 0;
        failure_attempt_0.predecessor_digest = manifest.self_digest;
        failure_attempt_0.lease = lease_identity(130, "chunk_1_attempt_00");
        failure_attempt_0.retry_policy_version = 1;
        failure_attempt_0 = seal_value(std::move(failure_attempt_0));

        failure_attempt_1 = failure_attempt_0;
        failure_attempt_1.attempt_ordinal = 1;
        failure_attempt_1.predecessor_digest = failure_attempt_0.self_digest;
        failure_attempt_1.lease = lease_identity(160, "chunk_1_attempt_01");
        failure_attempt_1 = reseal(std::move(failure_attempt_1));

        terminal_failure.manifest_digest = manifest.self_digest;
        terminal_failure.chunk_id = 1;
        terminal_failure.sq_begin = 3;
        terminal_failure.sq_end = 5;
        terminal_failure.exhausted_attempt_count = 2;
        terminal_failure.last_attempt_digest = failure_attempt_1.self_digest;
        const std::array failed_attempt_chain = {failure_attempt_0, failure_attempt_1};
        const auto failed_chain_digest = sieve::distributed_sieve_attempt_chain_digest(
            manifest.self_digest, failed_attempt_chain);
        if (!failed_chain_digest) {
            fail("distributed_sieve_attempt_chain_digest", __LINE__,
                 sieve::distributed_sieve_protocol_error_name(failed_chain_digest.status.error));
        }
        terminal_failure.predecessor_chain_digest = *failed_chain_digest.digest;
        terminal_failure.reason = sieve::ChunkTerminalFailureReasonV1::attempt_budget_exhausted;
        terminal_failure.wait_facts.kind = sieve::WorkerWaitFactKindV1::unavailable;
        terminal_failure.no_canonical_handoff_confirmed = true;
        terminal_failure.exact_attempt_lease_absent_confirmed = true;
        terminal_failure.next_sq_index = 3;
        terminal_failure.processed_sq_count = 0;
        terminal_failure = seal_value(std::move(terminal_failure));

        handoff.manifest_digest = manifest.self_digest;
        handoff.work_digest = work_digest;
        handoff.wave_id = manifest.wave_id;
        handoff.chunk_id = 0;
        handoff.sq_begin = 2;
        handoff.sq_end = 3;
        handoff.attempt_ordinal = 0;
        handoff.attempt_started_digest = attempt.self_digest;
        handoff.lease = attempt.lease;
        handoff.artifact = corpus_artifact(10, 3);
        handoff.processed_sq_count = 1;
        handoff.next_sq_index = 3;
        handoff.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
        handoff.relation_count = 3;
        handoff.cleanup_intent_absent = true;
        handoff = seal_value(std::move(handoff));

        handoff_1.manifest_digest = manifest.self_digest;
        handoff_1.work_digest = work_digest;
        handoff_1.wave_id = manifest.wave_id;
        handoff_1.chunk_id = 1;
        handoff_1.sq_begin = 3;
        handoff_1.sq_end = 5;
        handoff_1.attempt_ordinal = 0;
        handoff_1.attempt_started_digest = failure_attempt_0.self_digest;
        handoff_1.lease = failure_attempt_0.lease;
        handoff_1.artifact = corpus_artifact(1000, 4);
        handoff_1.processed_sq_count = 1;
        handoff_1.next_sq_index = 5;
        handoff_1.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
        handoff_1.relation_count = 4;
        handoff_1.cleanup_intent_absent = true;
        handoff_1 = seal_value(std::move(handoff_1));

        std::vector<sieve::TerminalChunkInputV1> inputs = {
            terminal_input(0, 2, 3, 30, 3),
            terminal_input(1, 3, 5, 40, 4),
        };
        inputs[0].last_attempt_digest = attempt.self_digest;
        inputs[0].lease_id = attempt.lease.lease_id;
        inputs[0].handoff_digest = handoff.self_digest;
        inputs[0].sequence_receipt = handoff.artifact.sequence_receipt;
        inputs[0].corpus_sha256 = handoff.artifact.corpus_sha256;
        inputs[1].last_attempt_digest = failure_attempt_0.self_digest;
        inputs[1].lease_id = failure_attempt_0.lease.lease_id;
        inputs[1].handoff_digest = handoff_1.self_digest;
        inputs[1].processed_sq_count = handoff_1.processed_sq_count;
        inputs[1].sequence_receipt = handoff_1.artifact.sequence_receipt;
        inputs[1].corpus_sha256 = handoff_1.artifact.corpus_sha256;

        merge_started.manifest_digest = manifest.self_digest;
        merge_started.work_digest = work_digest;
        merge_started.ordered_inputs = inputs;
        merge_started.merge_policy_version = 1;
        merge_started.merged_lease = lease_identity(200, "merged_attempt_0");
        merge_started.merge_attempt_ordinal = 0;
        merge_started.predecessor_digest = manifest.self_digest;
        merge_started = seal_value(std::move(merge_started));

        merge_prepared.manifest_digest = manifest.self_digest;
        merge_prepared.work_digest = work_digest;
        merge_prepared.merge_policy_version = 1;
        merge_prepared.merge_started_digest = merge_started.self_digest;
        merge_prepared.ordered_inputs = inputs;
        merge_prepared.input_relation_count = 7;
        merge_prepared.duplicate_relation_count = 1;
        merge_prepared.output_relation_count = 6;
        merge_prepared.per_chunk_retained_counts = {{0, 3}, {1, 3}};
        merge_prepared.merged_artifact = corpus_artifact(2000, 6);
        merge_prepared.merged_lease = merge_started.merged_lease;
        merge_prepared = seal_value(std::move(merge_prepared));

        merge_commit.manifest_digest = manifest.self_digest;
        merge_commit.work_digest = work_digest;
        merge_commit.chunks = {
            sieve::ChunkCommitSummaryV1{inputs[0], 3, {}},
            sieve::ChunkCommitSummaryV1{inputs[1], 3, {}},
        };
        merge_commit.merge_policy_version = 1;
        merge_commit.input_relation_count = 7;
        merge_commit.duplicate_relation_count = 1;
        merge_commit.output_relation_count = 6;
        merge_commit.merge_prepared_digest = merge_prepared.self_digest;
        merge_commit.merged_lease = merge_started.merged_lease;
        merge_commit.merged_artifact = merge_prepared.merged_artifact;
        merge_commit = seal_value(std::move(merge_commit));

        cleanup_authorizations[0].authorizer = sieve::CleanupAuthorizerKindV1::merge_commit_worker;
        cleanup_authorizations[0].manifest_digest = manifest.self_digest;
        cleanup_authorizations[0].authorizer_record_digest = merge_commit.self_digest;
        cleanup_authorizations[0].artifact_kind = sieve::CleanupArtifactKindV1::worker;
        cleanup_authorizations[0].manifest_order_ordinal = 0;
        cleanup_authorizations[0].lease = handoff.lease;
        cleanup_authorizations[0].handoff_digest = handoff.self_digest;
        cleanup_authorizations[0].artifact = handoff.artifact;
        cleanup_authorizations[0] = seal_value(std::move(cleanup_authorizations[0]));

        cleanup_authorizations[1] = cleanup_authorizations[0];
        cleanup_authorizations[1].manifest_order_ordinal = 1;
        cleanup_authorizations[1].lease = handoff_1.lease;
        cleanup_authorizations[1].handoff_digest = handoff_1.self_digest;
        cleanup_authorizations[1].artifact = handoff_1.artifact;
        cleanup_authorizations[1] = reseal(std::move(cleanup_authorizations[1]));

        cleanup_completions[0].authorization_digest = cleanup_authorizations[0].self_digest;
        cleanup_completions[0].cleanup_intent_identity = native_identity(300);
        cleanup_completions[0].parent_directory_durability_confirmed = true;
        cleanup_completions[0].expected_namespace_absent = true;
        cleanup_completions[0] = seal_value(std::move(cleanup_completions[0]));

        cleanup_completions[1] = cleanup_completions[0];
        cleanup_completions[1].authorization_digest = cleanup_authorizations[1].self_digest;
        cleanup_completions[1].cleanup_intent_identity = native_identity(310);
        cleanup_completions[1] = reseal(std::move(cleanup_completions[1]));

        consumption_started.merge_commit_digest = merge_commit.self_digest;
        consumption_started.manifest_digest = manifest.self_digest;
        consumption_started.consumer_kind =
            sieve::ConsumerKindV1::structured_reduction_relation_corpus;
        consumption_started.execution_contract_version = 1;
        consumption_started.successor_lease = lease_identity(400, "successor_attempt_0");
        consumption_started.successor_format_version = 1;
        consumption_started.consumption_attempt_ordinal = 0;
        consumption_started.predecessor_digest = manifest.self_digest;
        consumption_started = seal_value(std::move(consumption_started));

        successor_prepared.consumption_started_digest = consumption_started.self_digest;
        successor_prepared.successor_lease = consumption_started.successor_lease;
        successor_prepared.successor_artifact = corpus_artifact(3000, 6);
        successor_prepared.successor_semantic_digest = digest_with_seed(61);
        successor_prepared.input_relation_count = 6;
        successor_prepared.output_relation_count = 6;
        successor_prepared = seal_value(std::move(successor_prepared));

        consumption_ack.merge_commit_digest = merge_commit.self_digest;
        consumption_ack.consumer_kind = sieve::ConsumerKindV1::structured_reduction_relation_corpus;
        consumption_ack.consumption_started_digest = consumption_started.self_digest;
        consumption_ack.successor_prepared_digest = successor_prepared.self_digest;
        consumption_ack.successor_artifact = successor_prepared.successor_artifact;
        consumption_ack.successor_semantic_digest = successor_prepared.successor_semantic_digest;
        consumption_ack.successor_cleanup_authority_identity =
            consumption_started.successor_lease.owner_marker;
        consumption_ack = seal_value(std::move(consumption_ack));

        cleanup_authorizations[2].authorizer =
            sieve::CleanupAuthorizerKindV1::consumption_ack_merged;
        cleanup_authorizations[2].manifest_digest = manifest.self_digest;
        cleanup_authorizations[2].authorizer_record_digest = consumption_ack.self_digest;
        cleanup_authorizations[2].artifact_kind = sieve::CleanupArtifactKindV1::merged;
        cleanup_authorizations[2].manifest_order_ordinal = 0;
        cleanup_authorizations[2].lease = merge_commit.merged_lease;
        cleanup_authorizations[2].handoff_digest = merge_prepared.self_digest;
        cleanup_authorizations[2].artifact = merge_commit.merged_artifact;
        cleanup_authorizations[2] = seal_value(std::move(cleanup_authorizations[2]));

        cleanup_completions[2] = cleanup_completions[0];
        cleanup_completions[2].authorization_digest = cleanup_authorizations[2].self_digest;
        cleanup_completions[2].cleanup_intent_identity = native_identity(320);
        cleanup_completions[2] = reseal(std::move(cleanup_completions[2]));

        completed.wave_root_identity = manifest.wave_root_identity;
        completed.permanent_lock_identity = manifest.permanent_lock_identity;
        completed.manifest_digest = manifest.self_digest;
        completed.merge_commit_digest = merge_commit.self_digest;
        completed.consumption_ack_digest = consumption_ack.self_digest;
        completed.successor_prepared_digest = successor_prepared.self_digest;
        completed.chunks = merge_commit.chunks;
        completed.cleanup_confirmations = {
            {sieve::CleanupArtifactKindV1::worker, 0, cleanup_authorizations[0].self_digest,
             cleanup_completions[0].self_digest},
            {sieve::CleanupArtifactKindV1::worker, 1, cleanup_authorizations[1].self_digest,
             cleanup_completions[1].self_digest},
            {sieve::CleanupArtifactKindV1::merged, 0, cleanup_authorizations[2].self_digest,
             cleanup_completions[2].self_digest},
        };
        completed.successor_artifact = successor_prepared.successor_artifact;
        completed.successor_semantic_digest = successor_prepared.successor_semantic_digest;
        completed = seal_value(std::move(completed));
    }

    [[nodiscard]] std::vector<Record> all_records() const {
        return {
            manifest,
            attempt,
            terminal_failure,
            handoff,
            merge_started,
            merge_prepared,
            merge_commit,
            cleanup_authorizations[0],
            cleanup_completions[0],
            consumption_started,
            successor_prepared,
            consumption_ack,
            completed,
        };
    }
};

void mutate_digest_bound_field(Record& record) {
    std::visit(
        [](auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, sieve::WaveManifestV1>) {
                ++value.execution_contract_version;
            } else if constexpr (std::is_same_v<T, sieve::AttemptStartedV1>) {
                ++value.retry_policy_version;
            } else if constexpr (std::is_same_v<T, sieve::ChunkTerminalFailureV1>) {
                perturb_digest(value.predecessor_chain_digest);
            } else if constexpr (std::is_same_v<T, sieve::WorkerHandoffV1>) {
                perturb_digest(value.work_digest);
            } else if constexpr (std::is_same_v<T, sieve::MergeStartedV1>) {
                ++value.merge_policy_version;
            } else if constexpr (std::is_same_v<T, sieve::MergePreparedV1>) {
                ++value.merge_policy_version;
            } else if constexpr (std::is_same_v<T, sieve::WaveMergeCommitV1>) {
                ++value.merge_policy_version;
            } else if constexpr (std::is_same_v<T, sieve::ArtifactCleanupAuthorizedV1>) {
                perturb_digest(value.handoff_digest);
            } else if constexpr (std::is_same_v<T, sieve::ArtifactCleanupCompletedV1>) {
                perturb_digest(value.authorization_digest);
            } else if constexpr (std::is_same_v<T, sieve::ConsumptionStartedV1>) {
                ++value.execution_contract_version;
            } else if constexpr (std::is_same_v<T, sieve::SuccessorPreparedV1>) {
                perturb_digest(value.successor_semantic_digest);
            } else if constexpr (std::is_same_v<T, sieve::WaveConsumptionAckV1>) {
                perturb_digest(value.successor_semantic_digest);
            } else if constexpr (std::is_same_v<T, sieve::WaveCompletedV1>) {
                perturb_digest(value.successor_semantic_digest);
            }
        },
        record);
}

void test_closed_names_and_record_kinds() {
    constexpr std::array ERRORS = {
        sieve::DistributedSieveProtocolError::none,
        sieve::DistributedSieveProtocolError::input_too_large,
        sieve::DistributedSieveProtocolError::output_too_large,
        sieve::DistributedSieveProtocolError::truncated,
        sieve::DistributedSieveProtocolError::trailing_bytes,
        sieve::DistributedSieveProtocolError::invalid_magic,
        sieve::DistributedSieveProtocolError::unsupported_wire_version,
        sieve::DistributedSieveProtocolError::unsupported_schema_version,
        sieve::DistributedSieveProtocolError::declared_size_mismatch,
        sieve::DistributedSieveProtocolError::unknown_record_kind,
        sieve::DistributedSieveProtocolError::unknown_enum,
        sieve::DistributedSieveProtocolError::invalid_boolean,
        sieve::DistributedSieveProtocolError::invalid_value,
        sieve::DistributedSieveProtocolError::invalid_string,
        sieve::DistributedSieveProtocolError::collection_too_large,
        sieve::DistributedSieveProtocolError::duplicate_entry,
        sieve::DistributedSieveProtocolError::noncanonical_order,
        sieve::DistributedSieveProtocolError::range_gap,
        sieve::DistributedSieveProtocolError::range_overlap,
        sieve::DistributedSieveProtocolError::integer_out_of_range,
        sieve::DistributedSieveProtocolError::digest_mismatch,
        sieve::DistributedSieveProtocolError::digest_unavailable,
        sieve::DistributedSieveProtocolError::resource_exhausted,
        sieve::DistributedSieveProtocolError::record_type_mismatch,
    };
    static_assert(static_cast<uint8_t>(sieve::DistributedSieveProtocolError::record_type_mismatch) +
                      1U ==
                  ERRORS.size());
    for (std::size_t left = 0; left < ERRORS.size(); ++left) {
        const auto left_name = sieve::distributed_sieve_protocol_error_name(ERRORS[left]);
        CHECK(!left_name.empty());
        for (std::size_t right = left + 1; right < ERRORS.size(); ++right) {
            CHECK(left_name != sieve::distributed_sieve_protocol_error_name(ERRORS[right]));
        }
    }

    constexpr std::array KINDS = {
        sieve::DistributedSieveRecordKindV1::wave_manifest,
        sieve::DistributedSieveRecordKindV1::attempt_started,
        sieve::DistributedSieveRecordKindV1::chunk_terminal_failure,
        sieve::DistributedSieveRecordKindV1::worker_handoff,
        sieve::DistributedSieveRecordKindV1::merge_started,
        sieve::DistributedSieveRecordKindV1::merge_prepared,
        sieve::DistributedSieveRecordKindV1::wave_merge_commit,
        sieve::DistributedSieveRecordKindV1::artifact_cleanup_authorized,
        sieve::DistributedSieveRecordKindV1::artifact_cleanup_completed,
        sieve::DistributedSieveRecordKindV1::consumption_started,
        sieve::DistributedSieveRecordKindV1::successor_prepared,
        sieve::DistributedSieveRecordKindV1::wave_consumption_ack,
        sieve::DistributedSieveRecordKindV1::wave_completed,
    };
    static_assert(static_cast<uint16_t>(sieve::DistributedSieveRecordKindV1::wave_completed) ==
                  KINDS.size());
    ProtocolFixture fixture;
    const auto records = fixture.all_records();
    CHECK(records.size() == KINDS.size());
    for (std::size_t index = 0; index < KINDS.size(); ++index) {
        CHECK(static_cast<uint16_t>(KINDS[index]) == index + 1);
        CHECK(!sieve::distributed_sieve_record_kind_name(KINDS[index]).empty());
        CHECK(sieve::distributed_sieve_record_kind(records[index]) == KINDS[index]);
    }

    auto invalid_terminal = fixture.terminal_failure;
    invalid_terminal.reason = static_cast<sieve::ChunkTerminalFailureReasonV1>(0);
    require_failed(validate_value(invalid_terminal, false), "unknown terminal reason");
    invalid_terminal = fixture.terminal_failure;
    invalid_terminal.wait_facts.kind = static_cast<sieve::WorkerWaitFactKindV1>(0);
    require_failed(validate_value(invalid_terminal, false), "unknown wait fact");

    auto invalid_handoff = fixture.handoff;
    invalid_handoff.completion_reason = static_cast<sieve::WorkerCompletionReasonV1>(0);
    require_failed(validate_value(invalid_handoff, false), "unknown completion reason");

    auto invalid_merge = fixture.merge_started;
    invalid_merge.ordered_inputs.front().disposition = static_cast<sieve::ChunkDispositionV1>(0);
    require_failed(validate_value(invalid_merge, false), "unknown chunk disposition");

    auto invalid_commit = fixture.merge_commit;
    invalid_commit.chunks.front().diagnostic.kind =
        static_cast<sieve::NormalizedDiagnosticKindV1>(0);
    require_failed(validate_value(invalid_commit, false), "unknown diagnostic kind");
    constexpr std::array DIAGNOSTIC_KINDS = {
        sieve::NormalizedDiagnosticKindV1::none,
        sieve::NormalizedDiagnosticKindV1::recovered_handoff,
        sieve::NormalizedDiagnosticKindV1::retried_after_exit,
        sieve::NormalizedDiagnosticKindV1::retried_after_signal,
        sieve::NormalizedDiagnosticKindV1::retried_after_invalid_handoff,
    };
    for (const auto kind : DIAGNOSTIC_KINDS) {
        auto valid_commit = fixture.merge_commit;
        valid_commit.chunks.front().diagnostic.kind = kind;
        valid_commit.chunks.front().diagnostic.code =
            kind == sieve::NormalizedDiagnosticKindV1::retried_after_exit ||
                    kind == sieve::NormalizedDiagnosticKindV1::retried_after_signal
                ? 1U
                : 0U;
        require_ok(validate_value(valid_commit, false), "closed diagnostic kind");
    }

    auto invalid_cleanup = fixture.cleanup_authorizations[0];
    invalid_cleanup.authorizer = static_cast<sieve::CleanupAuthorizerKindV1>(0);
    require_failed(validate_value(invalid_cleanup, false), "unknown cleanup authorizer");
    invalid_cleanup = fixture.cleanup_authorizations[0];
    invalid_cleanup.artifact_kind = static_cast<sieve::CleanupArtifactKindV1>(0);
    require_failed(validate_value(invalid_cleanup, false), "unknown cleanup artifact kind");

    auto invalid_consumer = fixture.consumption_started;
    invalid_consumer.consumer_kind = static_cast<sieve::ConsumerKindV1>(0);
    require_failed(validate_value(invalid_consumer, false), "unknown consumer kind");
}

void test_all_record_round_trips_and_self_digests() {
    ProtocolFixture fixture;
    const auto records = fixture.all_records();

    for (const auto& record : records) {
        require_ok(sieve::validate_distributed_sieve_record(record), "sealed record validation");
        CHECK(record_digest_or_fail(record) == self_digest(record));

        const auto encoded = encode_or_fail(record);
        CHECK(!encoded.empty());
        CHECK(encoded.size() <= sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES);

        const auto decoded = sieve::decode_distributed_sieve_record(encoded);
        CHECK(decoded);
        CHECK(decoded.value.has_value());
        CHECK(sieve::distributed_sieve_record_kind(*decoded.value) ==
              sieve::distributed_sieve_record_kind(record));
        CHECK(encode_or_fail(*decoded.value) == encoded);

        Record changed = record;
        mutate_digest_bound_field(changed);
        require_ok(sieve::validate_distributed_sieve_record(changed, false),
                   "semantic mutation without digest verification");
        const auto stale_digest_status = sieve::validate_distributed_sieve_record(changed, true);
        CHECK(!stale_digest_status);
        CHECK(stale_digest_status.error == sieve::DistributedSieveProtocolError::digest_mismatch);

        require_ok(sieve::seal_distributed_sieve_record(changed), "reseal semantic mutation");
        CHECK(self_digest(changed) != self_digest(record));
        require_ok(sieve::validate_distributed_sieve_record(changed), "resealed semantic mutation");
    }

    auto high_limb_drift = fixture.attempt;
    ++high_limb_drift.lease.lease_id.limbs[1];
    high_limb_drift = reseal(std::move(high_limb_drift));
    CHECK(high_limb_drift.self_digest != fixture.attempt.self_digest);
    const auto high_limb_round_trip =
        sieve::decode_distributed_sieve_record(encode_or_fail(Record{high_limb_drift}));
    CHECK(high_limb_round_trip);
    CHECK(std::get<sieve::AttemptStartedV1>(*high_limb_round_trip.value).lease.lease_id ==
          high_limb_drift.lease.lease_id);

    const std::array baseline_attempts = {fixture.attempt};
    const std::array high_limb_attempts = {high_limb_drift};
    const auto baseline_chain = sieve::distributed_sieve_attempt_chain_digest(
        fixture.manifest.self_digest, baseline_attempts);
    const auto high_limb_chain = sieve::distributed_sieve_attempt_chain_digest(
        fixture.manifest.self_digest, high_limb_attempts);
    CHECK(baseline_chain);
    CHECK(high_limb_chain);
    CHECK(*baseline_chain.digest != *high_limb_chain.digest);

    auto nil_lease = fixture.attempt;
    nil_lease.lease.lease_id = {};
    require_reseal_failed(std::move(nil_lease), "all-zero lease id is reserved");
}

void test_exact_framing_and_wire_tamper() {
    ProtocolFixture fixture;
    const auto records = fixture.all_records();

    for (const auto& record : records) {
        const auto encoded = encode_or_fail(record);
        for (std::size_t prefix_size = 0; prefix_size < encoded.size(); ++prefix_size) {
            const auto decoded = sieve::decode_distributed_sieve_record(
                std::span<const std::byte>(encoded).first(prefix_size));
            CHECK(!decoded);
            CHECK(decoded.status.error != sieve::DistributedSieveProtocolError::none);
        }

        auto trailing = encoded;
        trailing.push_back(std::byte{0x42});
        const auto trailing_result = sieve::decode_distributed_sieve_record(trailing);
        CHECK(!trailing_result);

        auto digest_tamper = encoded;
        digest_tamper.back() ^= std::byte{0x01};
        const auto digest_result = sieve::decode_distributed_sieve_record(digest_tamper);
        CHECK(!digest_result);
        CHECK(digest_result.status.error == sieve::DistributedSieveProtocolError::digest_mismatch);
    }

    auto lease_wire_order = fixture.attempt;
    lease_wire_order.lease.lease_id.limbs = {
        UINT64_C(0x0807060504030201),
        UINT64_C(0x1817161514131211),
    };
    lease_wire_order = reseal(std::move(lease_wire_order));
    const auto lease_wire = encode_or_fail(Record{lease_wire_order});
    constexpr std::size_t ATTEMPT_LEASE_ID_OFFSET = 24 + 32 + 4 + 4 + 4 + 4 + 32;
    constexpr std::array<std::byte, 16> EXPECTED_LEASE_ID_BYTES = {
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
        std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
        std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18},
    };
    CHECK(lease_wire.size() >= ATTEMPT_LEASE_ID_OFFSET + EXPECTED_LEASE_ID_BYTES.size());
    CHECK(std::equal(EXPECTED_LEASE_ID_BYTES.begin(), EXPECTED_LEASE_ID_BYTES.end(),
                     lease_wire.begin() + static_cast<std::ptrdiff_t>(ATTEMPT_LEASE_ID_OFFSET)));

    auto bad_magic = encode_or_fail(Record{fixture.manifest});
    bad_magic.front() ^= std::byte{0x80};
    const auto bad_magic_result = sieve::decode_distributed_sieve_record(bad_magic);
    CHECK(!bad_magic_result);
    CHECK(bad_magic_result.status.error == sieve::DistributedSieveProtocolError::invalid_magic);

    std::vector<std::byte> oversized(
        static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES) + 1,
        std::byte{0});
    const auto oversized_result = sieve::decode_distributed_sieve_record(oversized);
    CHECK(!oversized_result);
    CHECK(oversized_result.status.error == sieve::DistributedSieveProtocolError::input_too_large);
}

void test_manifest_canonical_order_and_limits() {
    ProtocolFixture fixture;
    require_ok(validate_value(fixture.manifest, false), "baseline manifest");
    require_ok(sieve::validate_manifest_executable_identity(fixture.manifest,
                                                            fixture.manifest.executable_sha256),
               "manifest executable identity exact match");
    auto wrong_executable_digest = fixture.manifest.executable_sha256;
    perturb_digest(wrong_executable_digest);
    require_failed(
        sieve::validate_manifest_executable_identity(fixture.manifest, wrong_executable_digest),
        "manifest executable identity drift");

    auto zero_executable = fixture.manifest;
    zero_executable.executable_sha256 = {};
    zero_executable = reseal(std::move(zero_executable));
    require_ok(sieve::validate_manifest_executable_identity(zero_executable, {}),
               "all-zero executable digest remains an exact present identity");

    auto nil_wave_id = fixture.manifest;
    nil_wave_id.wave_id = {};
    require_failed(validate_value(nil_wave_id, false), "nil wave id rejected");

    auto maximum = fixture.manifest;
    maximum.effective_sq_begin = 100;
    maximum.effective_sq_end = 100 + sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS;
    maximum.worker_count = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS;
    maximum.chunks.clear();
    for (uint32_t index = 0; index < sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS; ++index) {
        maximum.chunks.push_back(
            {index, 100 + index, 101 + index, "worker_" + std::to_string(index)});
    }
    maximum.max_worker_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
    maximum.max_merge_build_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
    maximum.max_consumption_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
    require_ok(validate_value(maximum, false), "maximum manifest limits");

    auto too_many = maximum;
    too_many.worker_count = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS + 1;
    too_many.effective_sq_end += 1;
    too_many.chunks.push_back({sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS,
                               maximum.effective_sq_end, too_many.effective_sq_end,
                               "worker_over_limit"});
    require_failed(validate_value(too_many, false), "manifest chunk limit");

    auto duplicate = fixture.manifest;
    duplicate.chunks[1].chunk_id = duplicate.chunks[0].chunk_id;
    require_failed(validate_value(duplicate, false), "duplicate chunk id");

    auto unordered = fixture.manifest;
    std::swap(unordered.chunks[0], unordered.chunks[1]);
    require_failed(validate_value(unordered, false), "unordered chunks");

    auto gap = fixture.manifest;
    ++gap.chunks[1].sq_begin;
    require_failed(validate_value(gap, false), "chunk range gap");

    auto overlap = fixture.manifest;
    --overlap.chunks[1].sq_begin;
    require_failed(validate_value(overlap, false), "chunk range overlap");

    auto wrong_count = fixture.manifest;
    ++wrong_count.worker_count;
    require_failed(validate_value(wrong_count, false), "worker/chunk count mismatch");

    auto bad_range = fixture.manifest;
    bad_range.chunks[0].sq_end = bad_range.chunks[0].sq_begin - 1;
    require_failed(validate_value(bad_range, false), "reversed chunk range");

    auto bad_stem = fixture.manifest;
    bad_stem.chunks[0].relative_artifact_stem.clear();
    require_failed(validate_value(bad_stem, false), "empty artifact stem");
    bad_stem = fixture.manifest;
    bad_stem.chunks[0].relative_artifact_stem.assign(
        static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES) + 1,
        'x');
    require_failed(validate_value(bad_stem, false), "artifact stem length limit");

    constexpr std::array<std::string_view, 8> NONPORTABLE_STEMS = {
        ".", "foo.", ".foo", "foo.bar", "CON", "nul", "COM1", "lPt9",
    };
    for (const auto stem : NONPORTABLE_STEMS) {
        auto bad_chunk_stem = fixture.manifest;
        bad_chunk_stem.chunks[0].relative_artifact_stem = stem;
        require_failed(validate_value(bad_chunk_stem, false),
                       "manifest rejects nonportable artifact stem");

        auto bad_lease_stem = fixture.attempt;
        bad_lease_stem.lease.relative_stem = stem;
        require_failed(validate_value(bad_lease_stem, false),
                       "lease rejects nonportable artifact stem");
    }

    constexpr std::array<std::string_view, 6> PORTABLE_STEMS = {
        "worker_0", "corpus", "foo", "foo-bar", "COM0", "COM10",
    };
    for (const auto stem : PORTABLE_STEMS) {
        auto valid_chunk_stem = fixture.manifest;
        valid_chunk_stem.chunks[0].relative_artifact_stem = stem;
        require_ok(validate_value(valid_chunk_stem, false), "manifest accepts portable stem");

        auto valid_lease_stem = fixture.attempt;
        valid_lease_stem.lease.relative_stem = stem;
        require_ok(validate_value(valid_lease_stem, false), "lease accepts portable stem");
    }

    auto case_folded_stems = fixture.manifest;
    case_folded_stems.chunks[0].relative_artifact_stem = "foo";
    case_folded_stems.chunks[1].relative_artifact_stem = "Foo";
    require_failed(validate_value(case_folded_stems, false),
                   "manifest chunk stems conflict under conservative ASCII case fold");

    auto zero_budget = fixture.manifest;
    zero_budget.max_worker_attempts = 0;
    require_failed(validate_value(zero_budget, false), "zero worker attempt budget");
    zero_budget = fixture.manifest;
    zero_budget.max_merge_build_attempts = 0;
    require_failed(validate_value(zero_budget, false), "zero merge attempt budget");
    zero_budget = fixture.manifest;
    zero_budget.max_consumption_attempts = 0;
    require_failed(validate_value(zero_budget, false), "zero consumption attempt budget");

    auto over_budget = fixture.manifest;
    over_budget.max_worker_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS + 1;
    require_failed(validate_value(over_budget, false), "worker attempt budget limit");
    over_budget = fixture.manifest;
    over_budget.max_merge_build_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS + 1;
    require_failed(validate_value(over_budget, false), "merge attempt budget limit");
    over_budget = fixture.manifest;
    over_budget.max_consumption_attempts = sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS + 1;
    require_failed(validate_value(over_budget, false), "consumption attempt budget limit");
}

void test_worker_attempt_naming_contract() {
    const auto two_digits = [](uint32_t value) {
        std::string digits(2, '0');
        digits[0] = static_cast<char>('0' + value / 10U);
        digits[1] = static_cast<char>('0' + value % 10U);
        return digits;
    };
    const auto ascii_casefold = [](std::string value) {
        for (char& character : value) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character + ('a' - 'A'));
            }
        }
        return value;
    };
    const auto require_unique = [](std::vector<std::string> values) {
        std::sort(values.begin(), values.end());
        CHECK(std::adjacent_find(values.begin(), values.end()) == values.end());
    };

    std::vector<std::string> stems;
    std::vector<std::string> record_leaves = {
        std::string(wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF),
        std::string(wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF),
        std::string(wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF),
    };
    stems.reserve(static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) *
                  sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS);
    record_leaves.reserve(
        3U + 2U * static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) *
                 sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS);

    for (uint32_t chunk_id = 0; chunk_id < sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS;
         ++chunk_id) {
        const std::string chunk_digits = two_digits(chunk_id);
        const std::string chunk_stem = "S" + chunk_digits;
        for (uint32_t ordinal = 0; ordinal < sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
             ++ordinal) {
            const std::string ordinal_digits = two_digits(ordinal);
            const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1(
                chunk_stem, chunk_id, ordinal);
            CHECK(names.has_value());
            CHECK(names->relative_lease_stem ==
                  chunk_stem + std::string(sieve::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1) +
                      ordinal_digits);
            CHECK(names->canonical_record_leaf ==
                  std::string(wave_detail::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PREFIX) +
                      chunk_digits +
                      std::string(
                          wave_detail::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_ORDINAL_SEPARATOR) +
                      ordinal_digits);
            CHECK(names->pending_record_leaf ==
                  names->canonical_record_leaf +
                      std::string(
                          wave_detail::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_RECORD_PENDING_SUFFIX));
            CHECK(names->canonical_record_leaf != names->pending_record_leaf);
            CHECK(names->relative_lease_stem.size() <=
                  sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES);
            CHECK(sieve::distributed_sieve_worker_attempt_relative_stem_matches(
                chunk_stem, ordinal, names->relative_lease_stem));

            const auto canonical = wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
                names->canonical_record_leaf);
            const auto pending = wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
                names->pending_record_leaf);
            CHECK(canonical.has_value());
            CHECK(pending.has_value());
            CHECK(canonical->chunk_id == chunk_id);
            CHECK(canonical->attempt_ordinal == ordinal);
            CHECK(!canonical->pending);
            CHECK(pending->chunk_id == chunk_id);
            CHECK(pending->attempt_ordinal == ordinal);
            CHECK(pending->pending);

            stems.push_back(names->relative_lease_stem);
            record_leaves.push_back(names->canonical_record_leaf);
            record_leaves.push_back(names->pending_record_leaf);
        }
    }

    require_unique(stems);
    require_unique(record_leaves);
    for (std::string& stem : stems) {
        stem = ascii_casefold(std::move(stem));
    }
    for (std::string& leaf : record_leaves) {
        leaf = ascii_casefold(std::move(leaf));
    }
    require_unique(std::move(stems));
    require_unique(std::move(record_leaves));

    for (uint32_t ordinal = 0; ordinal < sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS;
         ++ordinal) {
        const auto names = wave_detail::distributed_sieve_worker_attempt_names_v1("S", 0, ordinal);
        CHECK(names.has_value());
        CHECK(names->relative_lease_stem == "S_attempt_" + two_digits(ordinal));
    }

    const auto lower_bound = wave_detail::distributed_sieve_worker_attempt_names_v1("S", 0, 0);
    const auto upper_bound = wave_detail::distributed_sieve_worker_attempt_names_v1("S", 63, 63);
    CHECK(lower_bound.has_value());
    CHECK(upper_bound.has_value());
    CHECK(lower_bound->canonical_record_leaf == ".gnfs-wave-v1.attempt-c00-a00");
    CHECK(upper_bound->canonical_record_leaf == ".gnfs-wave-v1.attempt-c63-a63");
    CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(
        "S", sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS, 0));
    CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(
        "S", 0, sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS));
    CHECK(!sieve::distributed_sieve_worker_attempt_relative_stem_matches(
        "S", sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS, "S_attempt_64"));

    constexpr std::size_t ATTEMPT_STEM_SUFFIX_BYTES =
        sieve::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() +
        sieve::DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
    constexpr std::size_t MAX_CHUNK_STEM_BYTES =
        sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES - ATTEMPT_STEM_SUFFIX_BYTES;
    const std::string maximum_chunk_stem(MAX_CHUNK_STEM_BYTES, 's');
    const std::string too_long_chunk_stem(MAX_CHUNK_STEM_BYTES + 1U, 't');
    const auto maximum_names =
        wave_detail::distributed_sieve_worker_attempt_names_v1(maximum_chunk_stem, 63, 63);
    CHECK(maximum_names.has_value());
    CHECK(maximum_names->relative_lease_stem.size() ==
          sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES);
    CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(too_long_chunk_stem, 0, 0));
    CHECK(!sieve::distributed_sieve_worker_attempt_relative_stem_matches(
        too_long_chunk_stem, 0, too_long_chunk_stem + "_attempt_00"));

    ProtocolFixture fixture;
    auto maximum_manifest = fixture.manifest;
    maximum_manifest.chunks[0].relative_artifact_stem = maximum_chunk_stem;
    require_ok(validate_value(maximum_manifest, false),
               "maximum representable nonempty worker-attempt stem");

    auto too_long_nonempty_manifest = fixture.manifest;
    too_long_nonempty_manifest.chunks[0].relative_artifact_stem = too_long_chunk_stem;
    require_failed(validate_value(too_long_nonempty_manifest, false),
                   "nonempty chunk stem must leave room for attempt suffix");

    auto long_empty_manifest = fixture.manifest;
    long_empty_manifest.effective_sq_end = long_empty_manifest.chunks[0].sq_end;
    long_empty_manifest.chunks[1].sq_begin = long_empty_manifest.chunks[0].sq_end;
    long_empty_manifest.chunks[1].sq_end = long_empty_manifest.chunks[0].sq_end;
    long_empty_manifest.chunks[1].relative_artifact_stem.assign(
        sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES, 'e');
    require_ok(validate_value(long_empty_manifest, false),
               "empty chunk may retain a maximum-length manifest stem");
    CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(
        long_empty_manifest.chunks[1].relative_artifact_stem, 1, 0));
    CHECK(!sieve::distributed_sieve_worker_attempt_relative_stem_matches(
        long_empty_manifest.chunks[1].relative_artifact_stem, 0,
        long_empty_manifest.chunks[1].relative_artifact_stem + "_attempt_00"));

    auto empty_manifest = fixture.manifest;
    empty_manifest.effective_sq_end = empty_manifest.chunks[0].sq_end;
    empty_manifest.chunks[1].sq_begin = empty_manifest.chunks[0].sq_end;
    empty_manifest.chunks[1].sq_end = empty_manifest.chunks[0].sq_end;
    empty_manifest = reseal(std::move(empty_manifest));
    const auto empty_chunk_names = wave_detail::distributed_sieve_worker_attempt_names_v1(
        empty_manifest.chunks[1].relative_artifact_stem, 1, 0);
    CHECK(empty_chunk_names.has_value());
    auto empty_chunk_attempt = fixture.failure_attempt_0;
    empty_chunk_attempt.manifest_digest = empty_manifest.self_digest;
    empty_chunk_attempt.sq_begin = empty_manifest.chunks[1].sq_begin;
    empty_chunk_attempt.sq_end = empty_manifest.chunks[1].sq_end;
    empty_chunk_attempt.predecessor_digest = empty_manifest.self_digest;
    empty_chunk_attempt.lease.relative_stem = empty_chunk_names->relative_lease_stem;
    require_failed(validate_value(empty_chunk_attempt, false),
                   "record semantics reject a syntactically valid empty-chunk attempt");
    const std::span<const sieve::AttemptStartedV1> empty_chunk_attempts;
    require_failed(sieve::validate_worker_attempt_chain(empty_manifest, 1, empty_chunk_attempts,
                                                        nullptr, nullptr),
                   "empty chunk has no worker-attempt chain");

    constexpr std::array<std::string_view, 12> INVALID_BASE_STEMS = {
        "",        ".",        "..",  ".foo", "foo.", "foo.bar",
        "foo/bar", "foo\\bar", "CON", "nul",  "COM1", "lPt9",
    };
    for (const auto stem : INVALID_BASE_STEMS) {
        CHECK(!wave_detail::distributed_sieve_worker_attempt_names_v1(stem, 0, 0));
        CHECK(!sieve::distributed_sieve_worker_attempt_relative_stem_matches(
            stem, 0, std::string(stem) + "_attempt_00"));
    }

    constexpr std::array<std::string_view, 25> INVALID_RECORD_LEAVES = {
        "",
        ".gnfs-wave-v1.attempt-c7-a09",
        ".gnfs-wave-v1.attempt-c007-a09",
        ".gnfs-wave-v1.attempt-c07-a9",
        ".gnfs-wave-v1.attempt-c07-a009",
        ".GNFS-wave-v1.attempt-c07-a09",
        ".gnfs-WAVE-v1.attempt-c07-a09",
        ".gnfs-wave-v1.ATTEMPT-c07-a09",
        ".gnfs-wave-v1.attempt-C07-a09",
        ".gnfs-wave-v1.attempt-c07-A09",
        ".gnfs-wave-v1.attempt-c07_a09",
        ".gnfs-wave-v1.attempt-c07.a09",
        ".gnfs-wave-v1.attempt-c07-b09",
        ".gnfs-wave-v1.attempt-c+7-a09",
        ".gnfs-wave-v1.attempt-c-7-a09",
        ".gnfs-wave-v1.attempt-c07-a+9",
        ".gnfs-wave-v1.attempt-c07-a-9",
        ".gnfs-wave-v1.attempt-c07-a09.PENDING",
        ".gnfs-wave-v1.attempt-c07-a09.pending.pending",
        ".gnfs-wave-v1.attempt-c07-a09.tmp",
        ".gnfs-wave-v1.attempt-c07-a09.trailing",
        ".gnfs-wave-v1.attempt-c64-a00",
        ".gnfs-wave-v1.attempt-c00-a64",
        ".gnfs-wave-v1.attempt-c99-a99",
        "gnfs-wave-v1.attempt-c07-a09",
    };
    for (const auto leaf : INVALID_RECORD_LEAVES) {
        CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(leaf));
    }
    CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
        wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF));
    CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
        wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF));
    CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(
        wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF));
    std::string nul_terminated_alias = ".gnfs-wave-v1.attempt-c07-a09";
    nul_terminated_alias.push_back('\0');
    CHECK(!wave_detail::parse_distributed_sieve_worker_attempt_leaf_v1(nul_terminated_alias));

    const auto expected_names =
        wave_detail::distributed_sieve_worker_attempt_names_v1("chunk_0", 0, 0);
    CHECK(expected_names.has_value());
    CHECK(fixture.attempt.lease.relative_stem == expected_names->relative_lease_stem);
    const std::array baseline_attempts = {fixture.attempt};
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 0, baseline_attempts, nullptr,
                                                    nullptr),
               "worker chain accepts exact derived lease stem");

    const auto require_wrong_stem_rejected = [&](std::string wrong_stem) {
        auto attempt = fixture.attempt;
        attempt.lease.relative_stem = std::move(wrong_stem);
        attempt = reseal(std::move(attempt));
        const std::array attempts = {attempt};
        const auto status =
            sieve::validate_worker_attempt_chain(fixture.manifest, 0, attempts, nullptr, nullptr);
        CHECK(!status);
        CHECK(status.error == sieve::DistributedSieveProtocolError::noncanonical_order);
    };
    require_wrong_stem_rejected("chunk_0_attempt_0");
    require_wrong_stem_rejected("chunk_0_attempt_000");
    require_wrong_stem_rejected("Chunk_0_attempt_00");
    require_wrong_stem_rejected("chunk_0_Attempt_00");
    require_wrong_stem_rejected("chunk_0-attempt-00");
    require_wrong_stem_rejected("chunk_1_attempt_00");
    require_wrong_stem_rejected("chunk_0_attempt_01");
    require_wrong_stem_rejected("chunk_0_attempt_00_trailing");

    const std::array valid_retry_attempts = {fixture.failure_attempt_0, fixture.failure_attempt_1};
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 1, valid_retry_attempts,
                                                    nullptr, nullptr),
               "worker retry chain accepts per-ordinal derived stems");
    auto wrong_retry = fixture.failure_attempt_1;
    wrong_retry.lease.relative_stem = fixture.failure_attempt_0.lease.relative_stem;
    wrong_retry = reseal(std::move(wrong_retry));
    const std::array invalid_retry_attempts = {fixture.failure_attempt_0, wrong_retry};
    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 1, invalid_retry_attempts,
                                                        nullptr, nullptr),
                   "worker retry chain rejects reused ordinal stem");

    auto wrong_naming_version = fixture.manifest;
    ++wrong_naming_version.canonical_naming_version;
    require_failed(validate_value(wrong_naming_version, false),
                   "manifest rejects unknown canonical naming version");
    wrong_naming_version.canonical_naming_version = 0;
    require_failed(validate_value(wrong_naming_version, false),
                   "manifest rejects zero canonical naming version");
}

void test_worker_completion_reason_closure() {
    ProtocolFixture fixture;

    auto sq_cap = fixture.handoff;
    sq_cap.artifact = corpus_artifact(5000, 0);
    sq_cap.sq_end = sq_cap.sq_begin + 3;
    sq_cap.processed_sq_count = 2;
    sq_cap.next_sq_index = sq_cap.sq_begin + 2;
    sq_cap.completion_reason = sieve::WorkerCompletionReasonV1::sq_cap;
    sq_cap.relation_count = 0;
    sq_cap = reseal(std::move(sq_cap));
    require_ok(validate_value(sq_cap), "partial SQ-cap zero-row handoff");
    const auto sq_cap_round_trip =
        sieve::decode_distributed_sieve_record(encode_or_fail(Record{sq_cap}));
    CHECK(sq_cap_round_trip);

    auto relation_cap = sq_cap;
    relation_cap.completion_reason = sieve::WorkerCompletionReasonV1::relation_cap;
    require_failed(validate_value(relation_cap, false),
                   "relation cap requires at least one relation");

    auto zero_relations = sq_cap;
    zero_relations.processed_sq_count =
        static_cast<uint64_t>(zero_relations.sq_end - zero_relations.sq_begin);
    zero_relations.next_sq_index = zero_relations.sq_end;
    zero_relations.completion_reason = sieve::WorkerCompletionReasonV1::zero_relations;
    zero_relations = reseal(std::move(zero_relations));
    require_ok(validate_value(zero_relations), "full-range zero-relation handoff");

    auto partial_zero = zero_relations;
    partial_zero.next_sq_index = partial_zero.sq_end - 1;
    partial_zero.processed_sq_count =
        static_cast<uint64_t>(partial_zero.next_sq_index - partial_zero.sq_begin);
    require_failed(validate_value(partial_zero, false),
                   "zero-relations reason requires full range");

    auto nonzero_zero_reason = zero_relations;
    nonzero_zero_reason.artifact = corpus_artifact(5001, 1);
    nonzero_zero_reason.relation_count = 1;
    require_failed(validate_value(nonzero_zero_reason, false),
                   "zero-relations reason requires zero rows");

    auto cap_manifest = fixture.manifest;
    cap_manifest.effective_sq_end = 15;
    cap_manifest.worker_count = 1;
    cap_manifest.chunks = {
        {0, cap_manifest.effective_sq_begin, cap_manifest.effective_sq_end, "cap_worker"}};
    cap_manifest = reseal(std::move(cap_manifest));

    auto cap_attempt = fixture.attempt;
    cap_attempt.manifest_digest = cap_manifest.self_digest;
    cap_attempt.sq_begin = cap_manifest.effective_sq_begin;
    cap_attempt.sq_end = cap_manifest.effective_sq_end;
    cap_attempt.predecessor_digest = cap_manifest.self_digest;
    cap_attempt.lease.relative_stem = "cap_worker_attempt_00";
    cap_attempt = reseal(std::move(cap_attempt));

    const auto projection_for = [](const sieve::WorkerHandoffV1& handoff) {
        sieve::TerminalChunkInputV1 projection;
        projection.chunk_id = handoff.chunk_id;
        projection.disposition = sieve::ChunkDispositionV1::handoff;
        projection.sq_begin = handoff.sq_begin;
        projection.sq_end = handoff.sq_end;
        projection.next_sq_index = handoff.next_sq_index;
        projection.processed_sq_count = handoff.processed_sq_count;
        projection.completion_reason = handoff.completion_reason;
        projection.durable_attempt_count = handoff.attempt_ordinal + 1;
        projection.last_attempt_digest = handoff.attempt_started_digest;
        projection.lease_id = handoff.lease.lease_id;
        projection.handoff_digest = handoff.self_digest;
        projection.raw_relation_count = handoff.relation_count;
        projection.sequence_receipt = handoff.artifact.sequence_receipt;
        projection.corpus_sha256 = handoff.artifact.corpus_sha256;
        return projection;
    };

    auto exact_sq_cap = fixture.handoff;
    exact_sq_cap.manifest_digest = cap_manifest.self_digest;
    exact_sq_cap.sq_begin = cap_manifest.effective_sq_begin;
    exact_sq_cap.sq_end = cap_manifest.effective_sq_end;
    exact_sq_cap.attempt_started_digest = cap_attempt.self_digest;
    exact_sq_cap.lease = cap_attempt.lease;
    exact_sq_cap.artifact = corpus_artifact(5100, 3);
    exact_sq_cap.processed_sq_count = cap_manifest.sq_cap_per_worker;
    exact_sq_cap.next_sq_index =
        exact_sq_cap.sq_begin + static_cast<uint32_t>(exact_sq_cap.processed_sq_count) + 1;
    exact_sq_cap.completion_reason = sieve::WorkerCompletionReasonV1::sq_cap;
    exact_sq_cap.relation_count = 3;
    exact_sq_cap = reseal(std::move(exact_sq_cap));
    const std::array cap_attempts = {cap_attempt};
    require_ok(sieve::validate_terminal_chunk_projection(
                   cap_manifest, 0, cap_attempts, &exact_sq_cap, projection_for(exact_sq_cap)),
               "SQ cap uses exact processed count despite a skipped index");

    auto short_of_sq_cap = exact_sq_cap;
    --short_of_sq_cap.processed_sq_count;
    short_of_sq_cap = reseal(std::move(short_of_sq_cap));
    require_failed(sieve::validate_terminal_chunk_projection(cap_manifest, 0, cap_attempts,
                                                             &short_of_sq_cap,
                                                             projection_for(short_of_sq_cap)),
                   "SQ-cap completion requires the exact configured processed count");

    auto relation_cap_after_sq_cap = exact_sq_cap;
    relation_cap_after_sq_cap.artifact =
        corpus_artifact(5200, cap_manifest.relation_cap_per_worker);
    relation_cap_after_sq_cap.completion_reason = sieve::WorkerCompletionReasonV1::relation_cap;
    relation_cap_after_sq_cap.relation_count = cap_manifest.relation_cap_per_worker;
    relation_cap_after_sq_cap = reseal(std::move(relation_cap_after_sq_cap));
    require_failed(sieve::validate_terminal_chunk_projection(
                       cap_manifest, 0, cap_attempts, &relation_cap_after_sq_cap,
                       projection_for(relation_cap_after_sq_cap)),
                   "SQ cap has priority when both completion caps are reached");

    auto relation_cap_before_sq_cap = relation_cap_after_sq_cap;
    --relation_cap_before_sq_cap.processed_sq_count;
    relation_cap_before_sq_cap = reseal(std::move(relation_cap_before_sq_cap));
    require_ok(sieve::validate_terminal_chunk_projection(
                   cap_manifest, 0, cap_attempts, &relation_cap_before_sq_cap,
                   projection_for(relation_cap_before_sq_cap)),
               "relation cap wins before the SQ cap is reached");
}

void test_terminal_failure_reason_normalization() {
    ProtocolFixture fixture;

    auto spawn_failed = fixture.terminal_failure;
    spawn_failed.reason = sieve::ChunkTerminalFailureReasonV1::spawn_failed;
    spawn_failed.wait_facts = {};
    spawn_failed = reseal(std::move(spawn_failed));
    require_ok(validate_value(spawn_failed), "spawn failure uses unavailable wait facts");

    require_ok(validate_value(fixture.terminal_failure),
               "attempt-budget exhaustion uses unavailable wait facts");

    auto invalid_handoff_without_wait = fixture.terminal_failure;
    invalid_handoff_without_wait.reason = sieve::ChunkTerminalFailureReasonV1::invalid_handoff;
    invalid_handoff_without_wait = reseal(std::move(invalid_handoff_without_wait));
    require_ok(validate_value(invalid_handoff_without_wait),
               "invalid handoff permits unavailable wait facts");

    auto invalid_handoff_after_clean_exit = invalid_handoff_without_wait;
    invalid_handoff_after_clean_exit.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    invalid_handoff_after_clean_exit.wait_facts.exit_code = 0;
    invalid_handoff_after_clean_exit = reseal(std::move(invalid_handoff_after_clean_exit));
    require_ok(validate_value(invalid_handoff_after_clean_exit),
               "invalid handoff permits clean worker exit");

    auto exited_unsuccessfully = fixture.terminal_failure;
    exited_unsuccessfully.reason = sieve::ChunkTerminalFailureReasonV1::exited_unsuccessfully;
    exited_unsuccessfully.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    exited_unsuccessfully.wait_facts.exit_code = 7;
    exited_unsuccessfully = reseal(std::move(exited_unsuccessfully));
    require_ok(validate_value(exited_unsuccessfully),
               "unsuccessful exit binds nonzero normalized exit code");

    auto clean_exit_is_not_unsuccessful = exited_unsuccessfully;
    clean_exit_is_not_unsuccessful.wait_facts.exit_code = 0;
    require_reseal_failed(std::move(clean_exit_is_not_unsuccessful),
                          "clean exit cannot be resealed as unsuccessful exit");

    auto spawn_with_exit = spawn_failed;
    spawn_with_exit.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    spawn_with_exit.wait_facts.exit_code = 0;
    require_reseal_failed(std::move(spawn_with_exit),
                          "spawn failure cannot be resealed with exit facts");

    auto budget_with_exit = fixture.terminal_failure;
    budget_with_exit.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    budget_with_exit.wait_facts.exit_code = 0;
    require_reseal_failed(std::move(budget_with_exit),
                          "attempt-budget exhaustion cannot be resealed with exit facts");

    auto invalid_handoff_with_failed_exit = invalid_handoff_without_wait;
    invalid_handoff_with_failed_exit.wait_facts.kind = sieve::WorkerWaitFactKindV1::exited;
    invalid_handoff_with_failed_exit.wait_facts.exit_code = 1;
    require_reseal_failed(std::move(invalid_handoff_with_failed_exit),
                          "invalid handoff cannot absorb unsuccessful exit");

    auto out_of_range_exit = exited_unsuccessfully;
    out_of_range_exit.wait_facts.exit_code = 256;
    require_reseal_failed(std::move(out_of_range_exit), "exit facts reject nonportable exit code");

    auto signaled = fixture.terminal_failure;
    signaled.reason = sieve::ChunkTerminalFailureReasonV1::signaled;
    signaled.wait_facts.kind = sieve::WorkerWaitFactKindV1::signaled;
    signaled.wait_facts.signal = 1;
    signaled = reseal(std::move(signaled));
    require_ok(validate_value(signaled), "signal one is portable");

    auto maximum_signal = signaled;
    maximum_signal.wait_facts.signal = 255;
    maximum_signal = reseal(std::move(maximum_signal));
    require_ok(validate_value(maximum_signal), "signal 255 is portable");

    auto zero_signal = signaled;
    zero_signal.wait_facts.signal = 0;
    require_reseal_failed(std::move(zero_signal), "signal zero is not a signaled wait fact");

    auto out_of_range_signal = signaled;
    out_of_range_signal.wait_facts.signal = 256;
    require_reseal_failed(std::move(out_of_range_signal),
                          "signal facts reject nonportable signal code");

    const auto require_diagnostic_ok = [&](sieve::NormalizedDiagnosticKindV1 kind, uint32_t code,
                                           std::string_view context) {
        auto commit = fixture.merge_commit;
        commit.chunks.front().diagnostic = {kind, code};
        commit = reseal(std::move(commit));
        require_ok(validate_value(commit), context);
    };
    const auto require_diagnostic_bad = [&](sieve::NormalizedDiagnosticKindV1 kind, uint32_t code,
                                            std::string_view context) {
        auto commit = fixture.merge_commit;
        commit.chunks.front().diagnostic = {kind, code};
        require_reseal_failed(std::move(commit), context);
    };

    for (const auto kind : {sieve::NormalizedDiagnosticKindV1::none,
                            sieve::NormalizedDiagnosticKindV1::recovered_handoff,
                            sieve::NormalizedDiagnosticKindV1::retried_after_invalid_handoff}) {
        require_diagnostic_ok(kind, 0, "non-process diagnostic has canonical zero code");
        require_diagnostic_bad(kind, 1, "non-process diagnostic rejects nonzero code");
    }
    for (const auto kind : {sieve::NormalizedDiagnosticKindV1::retried_after_exit,
                            sieve::NormalizedDiagnosticKindV1::retried_after_signal}) {
        require_diagnostic_ok(kind, 1, "process diagnostic accepts code one");
        require_diagnostic_ok(kind, 255, "process diagnostic accepts code 255");
        require_diagnostic_bad(kind, 0, "process diagnostic rejects zero code");
        require_diagnostic_bad(kind, 256, "process diagnostic rejects code above 255");
        require_diagnostic_bad(kind, std::numeric_limits<uint32_t>::max(),
                               "process diagnostic rejects uint32 maximum");
    }
}

void test_execution_policy_closed_inventory_and_field_drift() {
    auto identity = make_work_identity();
    const Digest baseline = work_digest_or_fail(identity);
    CHECK(identity.execution_policy.settings.size() == POLICY_SETTINGS.size());

    for (std::size_t index = 0; index < POLICY_SETTINGS.size(); ++index) {
        auto changed = identity;
        changed.execution_policy.settings[index].canonical_bits = POLICY_SETTINGS[index].alternate;
        require_ok(sieve::validate_distributed_sieve_execution_policy(changed.execution_policy),
                   "policy field alternate");
        require_ok(sieve::validate_distributed_sieve_work_identity(changed),
                   "work identity after policy field drift");
        CHECK(work_digest_or_fail(changed) != baseline);
    }

    auto disabled_degree = identity;
    disabled_degree.execution_policy.settings[10].canonical_bits = 0;
    require_ok(sieve::validate_distributed_sieve_execution_policy(disabled_degree.execution_policy),
               "ECM Brent-Suyama degree disabled boundary");
    CHECK(work_digest_or_fail(disabled_degree) != baseline);

    auto disabled_ecore = identity;
    disabled_ecore.execution_policy.settings[25].canonical_bits = 0;
    require_ok(sieve::validate_distributed_sieve_execution_policy(disabled_ecore.execution_policy),
               "sieve E-core threads disabled boundary");
    CHECK(work_digest_or_fail(disabled_ecore) != baseline);

    auto all_unset = identity;
    for (std::size_t index = 0; index < ALL_UNSET_POLICY_BITS.size(); ++index) {
        all_unset.execution_policy.settings[index].canonical_bits = ALL_UNSET_POLICY_BITS[index];
    }
    require_ok(sieve::validate_distributed_sieve_execution_policy(all_unset.execution_policy),
               "all-unset normalized execution policy");
    require_ok(sieve::validate_distributed_sieve_work_identity(all_unset),
               "work identity with all-unset policy");
    CHECK(work_digest_or_fail(all_unset) != baseline);

    auto invalid = identity.execution_policy;
    invalid.settings.pop_back();
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "missing policy field");

    invalid = identity.execution_policy;
    invalid.settings.push_back(invalid.settings.back());
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "extra policy field");

    invalid = identity.execution_policy;
    invalid.settings[1].key = invalid.settings[0].key;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "duplicate policy key");

    invalid = identity.execution_policy;
    std::swap(invalid.settings[0], invalid.settings[1]);
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "noncanonical policy order");

    invalid = identity.execution_policy;
    invalid.settings[0].kind = sieve::ExecutionPolicyScalarKindV1::unsigned_integer;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "wrong policy scalar kind");

    invalid = identity.execution_policy;
    invalid.settings[1].canonical_bits = 2;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "noncanonical policy boolean");

    invalid = identity.execution_policy;
    invalid.settings[0].canonical_bits = 0;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "noncanonical closed mode");

    invalid = identity.execution_policy;
    invalid.settings[3].canonical_bits = binary64_bits(std::numeric_limits<double>::infinity());
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "non-finite policy binary64");

    invalid = identity.execution_policy;
    invalid.schema_version = sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1 + 1;
    require_failed(sieve::validate_distributed_sieve_execution_policy(invalid),
                   "unsupported policy schema");
}

struct WorkMutation final {
    std::string_view name;
    std::function<void(sieve::DistributedSieveWorkIdentityV1&)> apply;
    bool remains_valid = true;
};

void test_work_identity_field_drift_and_canonical_integers() {
    const auto baseline_identity = make_work_identity();
    const Digest baseline_digest = work_digest_or_fail(baseline_identity);

    const std::vector<WorkMutation> mutations = {
        {"polynomial n", [](auto& value) { value.polynomial.n.decimal = "1000036000101"; }},
        {"polynomial m", [](auto& value) { value.polynomial.m.decimal = "10003"; }},
        {"polynomial degree/count",
         [](auto& value) {
             value.polynomial.degree = 3;
             value.polynomial.coefficients.push_back({"2"});
         }},
        {"polynomial coefficient",
         [](auto& value) { value.polynomial.coefficients[0].decimal = "-7"; }},
        {"polynomial skew",
         [](auto& value) { value.polynomial.skewness_ieee754_bits = binary64_bits(1.5); }},
        {"rational bound", [](auto& value) { ++value.factor_base.rational_bound; }},
        {"algebraic bound", [](auto& value) { ++value.factor_base.algebraic_bound; }},
        {"factor-base LP bound", [](auto& value) { ++value.factor_base.large_prime_bound; }},
        {"factor-base log scale", [](auto& value) { ++value.factor_base.log_scale; }},
        {"rational prime", [](auto& value) { value.factor_base.rational[0].p = 3; }},
        {"rational log", [](auto& value) { ++value.factor_base.rational[0].log_p; }},
        {"rational entry count",
         [](auto& value) { value.factor_base.rational.push_back({7, 31}); }},
        {"algebraic prime", [](auto& value) { value.factor_base.algebraic[0].p = 5; }},
        {"algebraic root", [](auto& value) { ++value.factor_base.algebraic[0].r; }},
        {"algebraic log", [](auto& value) { ++value.factor_base.algebraic[0].log_p; }},
        {"algebraic degree", [](auto& value) { ++value.factor_base.algebraic[0].degree; }},
        {"algebraic entry count",
         [](auto& value) { value.factor_base.algebraic.push_back({229, 7, 73, 1}); }},
        {"sieve algebraic count",
         [](auto& value) {
             value.factor_base.sieve_algebraic_count = 1;
             value.factor_base.algebraic = {
                 {7, 1, 37, 1}, {211, 4, 55, 2}, {223, 3, 61, 1}, {227, 2, 67, 1}, {229, 5, 71, 1},
             };
         },
         false},
        {"sieve log scale", [](auto& value) { ++value.sieve.log_scale; }},
        {"rational threshold", [](auto& value) { ++value.sieve.rational_threshold; }},
        {"algebraic threshold", [](auto& value) { ++value.sieve.algebraic_threshold; }},
        {"sieve LP bound", [](auto& value) { ++value.sieve.large_prime_bound; }},
        {"allow 2LP", [](auto& value) { value.sieve.allow_2lp = false; }},
        {"allow 3LP", [](auto& value) { value.sieve.allow_3lp = true; }},
        {"region i min", [](auto& value) { --value.region.i_min; }},
        {"region i max", [](auto& value) { ++value.region.i_max; }},
        {"region j min", [](auto& value) { ++value.region.j_min; }},
        {"region j max", [](auto& value) { ++value.region.j_max; }},
        {"cofactor LP bound", [](auto& value) { ++value.cofactor.large_prime_bound; }},
        {"cofactor allow 1LP", [](auto& value) { value.cofactor.allow_1lp = false; }},
        {"cofactor allow 2LP", [](auto& value) { value.cofactor.allow_2lp = false; }},
        {"cofactor allow 3LP", [](auto& value) { value.cofactor.allow_3lp = false; }},
        {"cofactor attempts", [](auto& value) { ++value.cofactor.max_factorization_attempts; }},
        {"original SQ start", [](auto& value) { ++value.original_sq_bounds.start_index; }},
        {"original SQ end", [](auto& value) { ++value.original_sq_bounds.end_index; }},
        {"original min q", [](auto& value) { --value.original_sq_bounds.min_q; }},
        {"original max q", [](auto& value) { ++value.original_sq_bounds.max_q; }},
        {"effective SQ start",
         [](auto& value) {
             --value.effective_sq_bounds.start_index;
             --value.distributed.chunks.front().sq_begin;
         },
         false},
        {"effective SQ end",
         [](auto& value) {
             ++value.effective_sq_bounds.end_index;
             ++value.distributed.chunks.back().sq_end;
         },
         false},
        {"effective min q", [](auto& value) { ++value.effective_sq_bounds.min_q; }, false},
        {"effective max q", [](auto& value) { ++value.effective_sq_bounds.max_q; }, false},
        {"worker count/chunk table",
         [](auto& value) {
             value.distributed.worker_count = 3;
             value.distributed.chunks = {
                 {0, 2, 3, "chunk_0"},
                 {1, 3, 4, "chunk_1"},
                 {2, 4, 5, "chunk_2"},
             };
         }},
        {"chunk split",
         [](auto& value) {
             value.distributed.chunks[0].sq_end = 4;
             value.distributed.chunks[1].sq_begin = 4;
         }},
        {"chunk artifact stem",
         [](auto& value) { value.distributed.chunks[0].relative_artifact_stem = "chunk_zero"; }},
        {"SQ cap", [](auto& value) { ++value.distributed.sq_cap_per_worker; }},
        {"relation cap", [](auto& value) { ++value.distributed.relation_cap_per_worker; }},
        {"worker budget", [](auto& value) { ++value.distributed.max_worker_attempts; }},
        {"merge budget", [](auto& value) { ++value.distributed.max_merge_build_attempts; }},
        {"consumption budget", [](auto& value) { ++value.distributed.max_consumption_attempts; }},
        {"relation serialization version",
         [](auto& value) { ++value.semantic_versions.relation_serialization_version; }},
        {"OOC version", [](auto& value) { ++value.semantic_versions.ooc_format_version; }},
        {"digest version", [](auto& value) { ++value.semantic_versions.digest_version; }},
        {"handoff version", [](auto& value) { ++value.semantic_versions.handoff_version; }},
        {"retry version", [](auto& value) { ++value.semantic_versions.retry_policy_version; }},
        {"chunking version", [](auto& value) { ++value.semantic_versions.chunking_version; }},
        {"completion version", [](auto& value) { ++value.semantic_versions.completion_version; }},
        {"deduplication version",
         [](auto& value) { ++value.semantic_versions.deduplication_version; }},
        {"merge policy version",
         [](auto& value) { ++value.semantic_versions.merge_policy_version; }},
    };

    for (const auto& mutation : mutations) {
        auto changed = baseline_identity;
        mutation.apply(changed);
        const auto status = sieve::validate_distributed_sieve_work_identity(changed);
        if (!mutation.remains_valid) {
            require_failed(status, mutation.name);
            continue;
        }
        if (!status) {
            fail(mutation.name, __LINE__,
                 sieve::distributed_sieve_protocol_error_name(status.error));
        }
        CHECK(work_digest_or_fail(changed) != baseline_digest);
    }

    CHECK(baseline_identity.original_sq_bounds.start_index == 0);
    CHECK(baseline_identity.original_sq_bounds.end_index == 5);
    CHECK(baseline_identity.original_sq_bounds.min_q == 100);
    CHECK(baseline_identity.original_sq_bounds.max_q == 1000);
    CHECK(baseline_identity.effective_sq_bounds.start_index == 2);
    CHECK(baseline_identity.effective_sq_bounds.end_index == 5);
    CHECK(baseline_identity.effective_sq_bounds.min_q == 0);
    CHECK(baseline_identity.effective_sq_bounds.max_q == std::numeric_limits<uint32_t>::max());

    auto containment_only_narrowing = baseline_identity;
    containment_only_narrowing.effective_sq_bounds.start_index = 3;
    containment_only_narrowing.effective_sq_bounds.end_index = 4;
    containment_only_narrowing.distributed.chunks = {
        {0, 3, 4, "chunk_0"},
        {1, 4, 4, "chunk_1"},
    };
    require_failed(sieve::validate_distributed_sieve_work_identity(containment_only_narrowing),
                   "effective SQ range cannot be an arbitrary contained subset");

    auto wrong_effective_begin = baseline_identity;
    wrong_effective_begin.effective_sq_bounds.start_index = 1;
    wrong_effective_begin.distributed.chunks.front().sq_begin = 1;
    require_failed(sieve::validate_distributed_sieve_work_identity(wrong_effective_begin),
                   "effective SQ begin must be exactly recomputed");

    auto wrong_effective_end = baseline_identity;
    wrong_effective_end.effective_sq_bounds.end_index = 6;
    wrong_effective_end.distributed.chunks.back().sq_end = 6;
    require_failed(sieve::validate_distributed_sieve_work_identity(wrong_effective_end),
                   "effective SQ end must be exactly recomputed");

    auto effective_min_is_not_index_only = baseline_identity;
    effective_min_is_not_index_only.effective_sq_bounds.min_q = 100;
    require_failed(sieve::validate_distributed_sieve_work_identity(effective_min_is_not_index_only),
                   "effective SQ minimum must use the index-only zero sentinel");

    auto effective_max_is_not_index_only = baseline_identity;
    effective_max_is_not_index_only.effective_sq_bounds.max_q = 1000;
    require_failed(sieve::validate_distributed_sieve_work_identity(effective_max_is_not_index_only),
                   "effective SQ maximum must use the index-only uint32 sentinel");

    for (const std::string invalid_integer : {"", "+1", "01", "-0", " 1", "1 ", "--1", "1x"}) {
        auto invalid = baseline_identity;
        invalid.polynomial.n.decimal = invalid_integer;
        require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                       "noncanonical integer");
    }

    auto invalid = baseline_identity;
    invalid.polynomial.n.decimal.assign(
        static_cast<std::size_t>(sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES) + 1,
        '1');
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "canonical integer length limit");

    invalid = baseline_identity;
    invalid.polynomial.degree = 3;
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "polynomial degree/coefficient mismatch");

    auto zero_padded_polynomial = baseline_identity;
    zero_padded_polynomial.polynomial.coefficients.push_back({"0"});
    zero_padded_polynomial.polynomial.coefficients.push_back({"0"});
    require_ok(sieve::validate_distributed_sieve_work_identity(zero_padded_polynomial),
               "canonical zero-padded polynomial live shape");
    CHECK(work_digest_or_fail(zero_padded_polynomial) != baseline_digest);

    auto maximum_zero_padding = baseline_identity;
    maximum_zero_padding.polynomial.coefficients.resize(
        sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_COEFFICIENTS, sieve::CanonicalIntegerV1{"0"});
    require_ok(sieve::validate_distributed_sieve_work_identity(maximum_zero_padding),
               "maximum polynomial coefficient count");
    CHECK(work_digest_or_fail(maximum_zero_padding) != baseline_digest);

    auto nonzero_padding = zero_padded_polynomial;
    nonzero_padding.polynomial.coefficients.back().decimal = "1";
    require_failed(sieve::validate_distributed_sieve_work_identity(nonzero_padding),
                   "nonzero polynomial tail rejected");

    auto noncanonical_padding = zero_padded_polynomial;
    noncanonical_padding.polynomial.coefficients.back().decimal = "00";
    require_failed(sieve::validate_distributed_sieve_work_identity(noncanonical_padding),
                   "noncanonical polynomial zero tail rejected");

    auto insufficient_live_coefficients = baseline_identity;
    insufficient_live_coefficients.polynomial.coefficients.resize(
        insufficient_live_coefficients.polynomial.degree);
    require_failed(sieve::validate_distributed_sieve_work_identity(insufficient_live_coefficients),
                   "polynomial requires degree plus one live coefficients");

    auto zero_leading_coefficient = baseline_identity;
    zero_leading_coefficient.polynomial.coefficients[zero_leading_coefficient.polynomial.degree]
        .decimal = "0";
    require_failed(sieve::validate_distributed_sieve_work_identity(zero_leading_coefficient),
                   "polynomial leading live coefficient is nonzero");

    auto too_many_coefficients = maximum_zero_padding;
    too_many_coefficients.polynomial.coefficients.push_back({"0"});
    require_failed(sieve::validate_distributed_sieve_work_identity(too_many_coefficients),
                   "polynomial coefficient count limit");

    auto degree_zero_constant = baseline_identity;
    degree_zero_constant.polynomial.degree = 0;
    degree_zero_constant.polynomial.coefficients = {{"7"}, {"0"}};
    require_ok(sieve::validate_distributed_sieve_work_identity(degree_zero_constant),
               "nonzero degree-zero polynomial with zero padding");
    auto all_zero_constant = degree_zero_constant;
    all_zero_constant.polynomial.coefficients.front().decimal = "0";
    require_failed(sieve::validate_distributed_sieve_work_identity(all_zero_constant),
                   "all-zero degree-zero polynomial rejected");

    invalid = baseline_identity;
    std::swap(invalid.distributed.chunks[0], invalid.distributed.chunks[1]);
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "work chunk noncanonical order");

    invalid = baseline_identity;
    invalid.factor_base.rational[0].p = 1;
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "rational factor-base p=1 rejected");

    invalid = baseline_identity;
    invalid.factor_base.algebraic[0].p = 1;
    invalid.factor_base.algebraic[0].r = 0;
    require_failed(sieve::validate_distributed_sieve_work_identity(invalid),
                   "algebraic factor-base p=1 rejected");

    auto actual_factor_base_shape = baseline_identity;
    actual_factor_base_shape.factor_base.algebraic = {
        {7, 1, 37, 1},   {7, std::numeric_limits<uint32_t>::max(), 38, 1},
        {211, 4, 55, 2}, {223, 5, 61, 1},
        {227, 6, 67, 1},
    };
    actual_factor_base_shape.factor_base.sieve_algebraic_count = 2;
    require_ok(sieve::validate_distributed_sieve_work_identity(actual_factor_base_shape),
               "projective root and special-Q extension factor-base shape");
    CHECK(work_digest_or_fail(actual_factor_base_shape) != baseline_digest);

    auto out_of_bound_sieve_prefix = actual_factor_base_shape;
    out_of_bound_sieve_prefix.factor_base.algebraic = {
        {211, 1, 37, 1}, {211, std::numeric_limits<uint32_t>::max(), 38, 1},
        {223, 4, 55, 2}, {227, 5, 61, 1},
        {229, 6, 67, 1},
    };
    require_failed(sieve::validate_distributed_sieve_work_identity(out_of_bound_sieve_prefix),
                   "sieve algebraic prefix must stay within bound");

    auto noncanonical_factor_base_order = actual_factor_base_shape;
    std::swap(noncanonical_factor_base_order.factor_base.algebraic[0],
              noncanonical_factor_base_order.factor_base.algebraic[1]);
    require_failed(sieve::validate_distributed_sieve_work_identity(noncanonical_factor_base_order),
                   "full algebraic factor-base order remains canonical");

    auto sieve_bound_fallback = baseline_identity;
    sieve_bound_fallback.sieve.large_prime_bound = 0;
    require_ok(sieve::validate_distributed_sieve_work_identity(sieve_bound_fallback),
               "zero sieve large-prime bound fallback");
    CHECK(work_digest_or_fail(sieve_bound_fallback) != baseline_digest);

    auto cofactor_bound_fallback = baseline_identity;
    cofactor_bound_fallback.cofactor.large_prime_bound = 0;
    require_ok(sieve::validate_distributed_sieve_work_identity(cofactor_bound_fallback),
               "zero cofactor large-prime bound fallback");
    CHECK(work_digest_or_fail(cofactor_bound_fallback) != baseline_digest);

    auto default_factorization_attempts = baseline_identity;
    default_factorization_attempts.cofactor.max_factorization_attempts = 0;
    require_ok(sieve::validate_distributed_sieve_work_identity(default_factorization_attempts),
               "zero factorization attempts sentinel");
    CHECK(work_digest_or_fail(default_factorization_attempts) != baseline_digest);

    std::vector<Digest> sieve_flag_digests;
    for (uint32_t mask = 0; mask < 4; ++mask) {
        auto combination = baseline_identity;
        combination.sieve.allow_2lp = (mask & 1U) != 0;
        combination.sieve.allow_3lp = (mask & 2U) != 0;
        require_ok(sieve::validate_distributed_sieve_work_identity(combination),
                   "independent sieve LP flags");
        const Digest digest = work_digest_or_fail(combination);
        CHECK(std::find(sieve_flag_digests.begin(), sieve_flag_digests.end(), digest) ==
              sieve_flag_digests.end());
        sieve_flag_digests.push_back(digest);
    }

    std::vector<Digest> cofactor_flag_digests;
    for (uint32_t mask = 0; mask < 8; ++mask) {
        auto combination = baseline_identity;
        combination.cofactor.allow_1lp = (mask & 1U) != 0;
        combination.cofactor.allow_2lp = (mask & 2U) != 0;
        combination.cofactor.allow_3lp = (mask & 4U) != 0;
        require_ok(sieve::validate_distributed_sieve_work_identity(combination),
                   "independent cofactor LP flags");
        const Digest digest = work_digest_or_fail(combination);
        CHECK(std::find(cofactor_flag_digests.begin(), cofactor_flag_digests.end(), digest) ==
              cofactor_flag_digests.end());
        cofactor_flag_digests.push_back(digest);
    }
}

void test_manifest_work_identity_binding() {
    ProtocolFixture fixture;
    require_ok(sieve::validate_manifest_work_identity(fixture.manifest, fixture.work_identity),
               "manifest binds canonical work identity");

    auto wrong_work_digest = fixture.manifest;
    perturb_digest(wrong_work_digest.work_sha256);
    wrong_work_digest = reseal(std::move(wrong_work_digest));
    require_failed(sieve::validate_manifest_work_identity(wrong_work_digest, fixture.work_identity),
                   "manifest rejects wrong work digest");

    const std::vector<WorkMutation> denormalized_field_mutations = {
        {"manifest effective SQ begin",
         [](auto& value) {
             --value.effective_sq_bounds.start_index;
             --value.distributed.chunks.front().sq_begin;
         },
         false},
        {"manifest effective SQ end",
         [](auto& value) {
             ++value.effective_sq_bounds.end_index;
             ++value.distributed.chunks.back().sq_end;
         },
         false},
        {"manifest worker count and chunks",
         [](auto& value) {
             value.distributed.worker_count = 3;
             value.distributed.chunks = {
                 {0, 2, 3, "chunk_0"},
                 {1, 3, 4, "chunk_1"},
                 {2, 4, 5, "chunk_2"},
             };
         }},
        {"manifest chunk split",
         [](auto& value) {
             value.distributed.chunks[0].sq_end = 4;
             value.distributed.chunks[1].sq_begin = 4;
         }},
        {"manifest chunk artifact stem",
         [](auto& value) { value.distributed.chunks[0].relative_artifact_stem = "chunk_zero"; }},
        {"manifest SQ cap", [](auto& value) { ++value.distributed.sq_cap_per_worker; }},
        {"manifest relation cap", [](auto& value) { ++value.distributed.relation_cap_per_worker; }},
        {"manifest worker budget", [](auto& value) { ++value.distributed.max_worker_attempts; }},
        {"manifest merge budget",
         [](auto& value) { ++value.distributed.max_merge_build_attempts; }},
        {"manifest consumption budget",
         [](auto& value) { ++value.distributed.max_consumption_attempts; }},
        {"manifest OOC version", [](auto& value) { ++value.semantic_versions.ooc_format_version; }},
        {"manifest relation serialization version",
         [](auto& value) { ++value.semantic_versions.relation_serialization_version; }},
        {"manifest handoff version",
         [](auto& value) { ++value.semantic_versions.handoff_version; }},
        {"manifest digest version", [](auto& value) { ++value.semantic_versions.digest_version; }},
        {"manifest retry version",
         [](auto& value) { ++value.semantic_versions.retry_policy_version; }},
        {"manifest merge policy version",
         [](auto& value) { ++value.semantic_versions.merge_policy_version; }},
    };

    for (const auto& mutation : denormalized_field_mutations) {
        auto changed_identity = fixture.work_identity;
        mutation.apply(changed_identity);
        if (!mutation.remains_valid) {
            require_failed(sieve::validate_distributed_sieve_work_identity(changed_identity),
                           mutation.name);
            continue;
        }
        require_ok(sieve::validate_distributed_sieve_work_identity(changed_identity),
                   mutation.name);

        auto stale_manifest = fixture.manifest;
        stale_manifest.work_sha256 = work_digest_or_fail(changed_identity);
        stale_manifest = reseal(std::move(stale_manifest));
        const auto status =
            sieve::validate_manifest_work_identity(stale_manifest, changed_identity);
        if (status) {
            fail(mutation.name, __LINE__, "stale denormalized manifest field accepted");
        }
    }

    const std::vector<WorkMutation> digest_only_version_mutations = {
        {"chunking version", [](auto& value) { ++value.semantic_versions.chunking_version; }},
        {"completion version", [](auto& value) { ++value.semantic_versions.completion_version; }},
        {"deduplication version",
         [](auto& value) { ++value.semantic_versions.deduplication_version; }},
    };
    for (const auto& mutation : digest_only_version_mutations) {
        auto changed_identity = fixture.work_identity;
        mutation.apply(changed_identity);
        require_ok(sieve::validate_distributed_sieve_work_identity(changed_identity),
                   mutation.name);
        require_failed(sieve::validate_manifest_work_identity(fixture.manifest, changed_identity),
                       mutation.name);
    }
}

[[nodiscard]] Digest seed_or_fail(const sieve::DeterministicRandomSeedRequestV1& request) {
    const auto result = sieve::derive_distributed_sieve_deterministic_seed(request);
    if (!result) {
        fail("derive_distributed_sieve_deterministic_seed", __LINE__,
             sieve::distributed_sieve_protocol_error_name(result.status.error));
    }
    return *result.digest;
}

void test_deterministic_seed_domain_and_input_drift() {
    sieve::DeterministicRandomSeedRequestV1 request;
    request.work_digest = work_digest_or_fail(make_work_identity());
    request.domain = sieve::DeterministicRandomDomainV1::adaptive_lattice;
    request.chunk_id = 3;
    request.sq_index = 7;
    request.candidate_ordinal = 11;
    request.algorithm_identity = 13;
    request.cofactor_input_digest = digest_with_seed(90);

    const Digest baseline = seed_or_fail(request);
    CHECK(seed_or_fail(request) == baseline);

    constexpr std::array DOMAINS = {
        sieve::DeterministicRandomDomainV1::adaptive_lattice,
        sieve::DeterministicRandomDomainV1::ecm_sigma,
        sieve::DeterministicRandomDomainV1::ecm_curve,
        sieve::DeterministicRandomDomainV1::pollard_rho,
        sieve::DeterministicRandomDomainV1::cofactor_choice,
    };
    std::vector<Digest> domain_digests;
    for (const auto domain : DOMAINS) {
        auto changed = request;
        changed.domain = domain;
        const auto digest = seed_or_fail(changed);
        CHECK(std::find(domain_digests.begin(), domain_digests.end(), digest) ==
              domain_digests.end());
        domain_digests.push_back(digest);
    }

    auto expect_input_drift = [&](const auto& mutate) {
        auto changed = request;
        mutate(changed);
        CHECK(seed_or_fail(changed) != baseline);
    };
    expect_input_drift([](auto& value) { perturb_digest(value.work_digest); });
    expect_input_drift([](auto& value) { ++value.chunk_id; });
    expect_input_drift([](auto& value) { ++value.sq_index; });
    expect_input_drift([](auto& value) { ++value.candidate_ordinal; });
    expect_input_drift([](auto& value) { ++value.algorithm_identity; });
    expect_input_drift([](auto& value) { perturb_digest(value.cofactor_input_digest); });

    auto invalid = request;
    invalid.domain = static_cast<sieve::DeterministicRandomDomainV1>(0);
    const auto invalid_result = sieve::derive_distributed_sieve_deterministic_seed(invalid);
    CHECK(!invalid_result);
    CHECK(invalid_result.status.error == sieve::DistributedSieveProtocolError::unknown_enum);
}

void test_predecessor_and_dependency_closure() {
    ProtocolFixture fixture;

    const std::array worker_attempts = {fixture.attempt};
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 0, worker_attempts,
                                                    &fixture.handoff, nullptr),
               "valid worker handoff chain");

    const std::array<std::pair<std::string_view, sieve::NativeIdentityV1>, 2>
        manifest_control_identities = {{
            {"wave root", fixture.manifest.wave_root_identity},
            {"permanent lock", fixture.manifest.permanent_lock_identity},
        }};
    for (const auto& [control_name, control_identity] : manifest_control_identities) {
        for (const bool mutate_owner : {true, false}) {
            auto aliased_attempt = fixture.attempt;
            if (mutate_owner) {
                aliased_attempt.lease.owner_marker = control_identity;
            } else {
                aliased_attempt.lease.directory = control_identity;
            }
            aliased_attempt = reseal(std::move(aliased_attempt));
            const std::array aliased_attempts = {aliased_attempt};
            const std::string attempt_context = std::string("attempt lease ") +
                                                (mutate_owner ? "owner" : "directory") +
                                                " aliases " + std::string(control_name);
            require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 0,
                                                                aliased_attempts, nullptr, nullptr),
                           attempt_context);

            auto aliased_handoff = fixture.handoff;
            aliased_handoff.attempt_started_digest = aliased_attempt.self_digest;
            aliased_handoff.lease = aliased_attempt.lease;
            aliased_handoff = reseal(std::move(aliased_handoff));
            const std::string handoff_context = std::string("worker handoff lease ") +
                                                (mutate_owner ? "owner" : "directory") +
                                                " aliases " + std::string(control_name);
            require_failed(sieve::validate_worker_attempt_chain(
                               fixture.manifest, 0, aliased_attempts, &aliased_handoff, nullptr),
                           handoff_context);
        }
        for (const bool mutate_index : {true, false}) {
            auto aliased_handoff = fixture.handoff;
            if (mutate_index) {
                aliased_handoff.artifact.index_file.identity = control_identity;
            } else {
                aliased_handoff.artifact.data_file.identity = control_identity;
            }
            aliased_handoff = reseal(std::move(aliased_handoff));
            const std::string context = std::string("worker handoff artifact ") +
                                        (mutate_index ? "index" : "data") + " aliases " +
                                        std::string(control_name);
            require_failed(sieve::validate_worker_attempt_chain(
                               fixture.manifest, 0, worker_attempts, &aliased_handoff, nullptr),
                           context);
        }
    }

    auto wrong_worker_format = fixture.handoff;
    ++wrong_worker_format.artifact.descriptor.format_version;
    wrong_worker_format = reseal(std::move(wrong_worker_format));
    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 0, worker_attempts,
                                                        &wrong_worker_format, nullptr),
                   "worker handoff format binds manifest OOC version");

    const std::array failed_attempts = {fixture.failure_attempt_0, fixture.failure_attempt_1};
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 1, failed_attempts, nullptr,
                                                    nullptr),
               "max-attempt crash prefix remains recoverable before lease cleanup");
    require_ok(sieve::validate_worker_attempt_chain(fixture.manifest, 1, failed_attempts, nullptr,
                                                    &fixture.terminal_failure),
               "terminalization is valid after exact lease cleanup");

    auto forbidden_attempt = fixture.failure_attempt_1;
    forbidden_attempt.attempt_ordinal = 2;
    forbidden_attempt.predecessor_digest = fixture.failure_attempt_1.self_digest;
    forbidden_attempt.lease = lease_identity(112, "chunk_1_attempt_02");
    forbidden_attempt = reseal(std::move(forbidden_attempt));
    const std::array attempts_beyond_budget = {
        fixture.failure_attempt_0,
        fixture.failure_attempt_1,
        forbidden_attempt,
    };
    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 1, attempts_beyond_budget,
                                                        nullptr, nullptr),
                   "max-attempt crash prefix cannot start another attempt");

    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 0, worker_attempts,
                                                        &fixture.handoff,
                                                        &fixture.terminal_failure),
                   "mutually exclusive worker terminals");

    auto wrong_attempt = fixture.attempt;
    wrong_attempt.attempt_ordinal = 1;
    wrong_attempt.predecessor_digest = fixture.attempt.self_digest;
    wrong_attempt = reseal(std::move(wrong_attempt));
    const std::array wrong_attempts = {wrong_attempt};
    require_failed(
        sieve::validate_worker_attempt_chain(fixture.manifest, 0, wrong_attempts, nullptr, nullptr),
        "worker ordinal gap");

    auto extra_attempt = fixture.attempt;
    extra_attempt.attempt_ordinal = 1;
    extra_attempt.predecessor_digest = fixture.attempt.self_digest;
    extra_attempt.lease = lease_identity(101, "chunk_0_attempt_01");
    extra_attempt = reseal(std::move(extra_attempt));
    const std::array attempts_after_terminal = {fixture.attempt, extra_attempt};
    require_failed(sieve::validate_worker_attempt_chain(
                       fixture.manifest, 0, attempts_after_terminal, &fixture.handoff, nullptr),
                   "worker handoff absorbs later attempt");

    const std::array merge_starts = {fixture.merge_started};
    require_ok(sieve::validate_merge_predecessor_chain(
                   fixture.manifest, merge_starts, &fixture.merge_prepared, &fixture.merge_commit),
               "valid merge chain");

    const std::array successful_attempts_1 = {fixture.failure_attempt_0};
    CHECK(fixture.work_identity.factor_base.algebraic[3].r == std::numeric_limits<uint32_t>::max());
    CHECK(fixture.handoff_1.processed_sq_count <
          static_cast<uint64_t>(fixture.handoff_1.next_sq_index - fixture.handoff_1.sq_begin));
    auto processed_beyond_index_span = fixture.handoff_1;
    processed_beyond_index_span.processed_sq_count =
        static_cast<uint64_t>(processed_beyond_index_span.next_sq_index -
                              processed_beyond_index_span.sq_begin) +
        1;
    require_reseal_failed(std::move(processed_beyond_index_span),
                          "processed special-Q count cannot exceed index advance");

    require_ok(sieve::validate_terminal_chunk_projection(fixture.manifest, 0, worker_attempts,
                                                         &fixture.handoff,
                                                         fixture.merge_started.ordered_inputs[0]),
               "chunk zero terminal projection binds exact handoff evidence");
    require_ok(sieve::validate_terminal_chunk_projection(fixture.manifest, 1, successful_attempts_1,
                                                         &fixture.handoff_1,
                                                         fixture.merge_started.ordered_inputs[1]),
               "chunk one terminal projection binds exact handoff evidence");

    auto high_limb_projection = fixture.merge_started.ordered_inputs[0];
    ++high_limb_projection.lease_id.limbs[1];
    require_failed(sieve::validate_terminal_chunk_projection(fixture.manifest, 0, worker_attempts,
                                                             &fixture.handoff,
                                                             high_limb_projection),
                   "terminal projection rejects lease high-limb drift");

    auto nil_handoff_projection = fixture.merge_started.ordered_inputs[0];
    nil_handoff_projection.lease_id = {};
    require_failed(sieve::validate_terminal_chunk_projection(fixture.manifest, 0, worker_attempts,
                                                             &fixture.handoff,
                                                             nil_handoff_projection),
                   "handoff projection rejects nil lease id");

    const std::array<sieve::ChunkTerminalEvidenceViewV1, 2> terminal_evidence = {{
        {std::span<const sieve::AttemptStartedV1>{worker_attempts}, &fixture.handoff},
        {std::span<const sieve::AttemptStartedV1>{successful_attempts_1}, &fixture.handoff_1},
    }};
    const std::span<const sieve::ConsumptionStartedV1> no_consumption_starts;
    require_ok(sieve::validate_merge_dependency_chain(fixture.manifest, terminal_evidence,
                                                      merge_starts, &fixture.merge_prepared,
                                                      &fixture.merge_commit),
               "valid merge dependency closure");
    require_ok(sieve::validate_artifact_cleanup_dependencies(
                   fixture.manifest, fixture.merge_commit, no_consumption_starts, nullptr, nullptr,
                   fixture.cleanup_authorizations[0], &fixture.handoff, nullptr),
               "validated merge evidence composes with worker cleanup mint");

    struct MergeAliasCase final {
        sieve::AttemptStartedV1 attempt_1;
        sieve::WorkerHandoffV1 handoff_1;
        sieve::MergeStartedV1 started;
        sieve::MergePreparedV1 prepared;
        sieve::WaveMergeCommitV1 commit;
    };
    using WorkerAliasMutation =
        std::function<void(sieve::AttemptStartedV1&, sieve::WorkerHandoffV1&)>;
    using MergeStartAliasMutation = std::function<void(sieve::MergeStartedV1&)>;
    using MergePreparedAliasMutation = std::function<void(sieve::MergePreparedV1&)>;
    const auto make_merge_alias_case = [&](const WorkerAliasMutation& mutate_worker,
                                           const MergeStartAliasMutation& mutate_start,
                                           const MergePreparedAliasMutation& mutate_prepared) {
        MergeAliasCase value;
        value.attempt_1 = fixture.failure_attempt_0;
        value.handoff_1 = fixture.handoff_1;
        if (mutate_worker) {
            mutate_worker(value.attempt_1, value.handoff_1);
        }
        value.attempt_1 = reseal(std::move(value.attempt_1));
        value.handoff_1.attempt_started_digest = value.attempt_1.self_digest;
        value.handoff_1.lease = value.attempt_1.lease;
        value.handoff_1 = reseal(std::move(value.handoff_1));

        auto projection = fixture.merge_started.ordered_inputs[1];
        projection.last_attempt_digest = value.attempt_1.self_digest;
        projection.lease_id = value.handoff_1.lease.lease_id;
        projection.handoff_digest = value.handoff_1.self_digest;
        projection.raw_relation_count = value.handoff_1.relation_count;
        projection.sequence_receipt = value.handoff_1.artifact.sequence_receipt;
        projection.corpus_sha256 = value.handoff_1.artifact.corpus_sha256;

        value.started = fixture.merge_started;
        value.started.ordered_inputs[1] = projection;
        if (mutate_start) {
            mutate_start(value.started);
        }
        value.started = reseal(std::move(value.started));

        value.prepared = fixture.merge_prepared;
        value.prepared.merge_started_digest = value.started.self_digest;
        value.prepared.ordered_inputs = value.started.ordered_inputs;
        value.prepared.merged_lease = value.started.merged_lease;
        if (mutate_prepared) {
            mutate_prepared(value.prepared);
        }
        value.prepared = reseal(std::move(value.prepared));

        value.commit = fixture.merge_commit;
        value.commit.chunks[1].input = projection;
        value.commit.merge_prepared_digest = value.prepared.self_digest;
        value.commit.merged_lease = value.prepared.merged_lease;
        value.commit.merged_artifact = value.prepared.merged_artifact;
        value.commit = reseal(std::move(value.commit));
        return value;
    };
    const auto validate_merge_alias_case = [&](const MergeAliasCase& value) {
        const std::array changed_attempts_1 = {value.attempt_1};
        const std::array<sieve::ChunkTerminalEvidenceViewV1, 2> changed_evidence = {{
            {std::span<const sieve::AttemptStartedV1>{worker_attempts}, &fixture.handoff},
            {std::span<const sieve::AttemptStartedV1>{changed_attempts_1}, &value.handoff_1},
        }};
        const std::array changed_starts = {value.started};
        return sieve::validate_merge_dependency_chain(
            fixture.manifest, changed_evidence, changed_starts, &value.prepared, &value.commit);
    };
    const auto require_merge_alias_rejected = [&](std::string_view name,
                                                  const MergeAliasCase& value) {
        const auto status = validate_merge_alias_case(value);
        if (status) {
            fail(name, __LINE__, "legal sequence accepted aliased identities");
        }
    };
    const auto require_merge_alias_accepted = [&](std::string_view name,
                                                  const MergeAliasCase& value) {
        const auto status = validate_merge_alias_case(value);
        if (!status) {
            fail(name, __LINE__, sieve::distributed_sieve_protocol_error_name(status.error));
        }
    };

    require_merge_alias_accepted("worker lease low limb reuse with distinct high limb",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.lease_id.limbs[0] =
                                             fixture.attempt.lease.lease_id.limbs[0];
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker exact 128-bit lease id reuse",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.lease_id = fixture.attempt.lease.lease_id;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease owner alias",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.owner_marker =
                                             fixture.attempt.lease.owner_marker;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease directory alias with distinct stem",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.directory =
                                             fixture.attempt.lease.directory;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease stem must match its chunk and ordinal",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.relative_stem =
                                             fixture.attempt.lease.relative_stem;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease owner-to-directory alias",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.owner_marker =
                                             fixture.attempt.lease.directory;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker lease directory-to-owner alias",
                                 make_merge_alias_case(
                                     [&](auto& attempt_1, auto&) {
                                         attempt_1.lease.directory =
                                             fixture.attempt.lease.owner_marker;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC same store with different generation",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.descriptor.store_id =
                                             fixture.handoff.artifact.descriptor.store_id;
                                     },
                                     {}, {}));
    require_merge_alias_accepted("worker OOC generation reuse",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.descriptor.generation =
                                             fixture.handoff.artifact.descriptor.generation;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC index identity alias",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.index_file.identity =
                                             fixture.handoff.artifact.index_file.identity;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC data identity alias",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.data_file.identity =
                                             fixture.handoff.artifact.data_file.identity;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC index-to-data identity alias",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.index_file.identity =
                                             fixture.handoff.artifact.data_file.identity;
                                     },
                                     {}, {}));
    require_merge_alias_rejected("worker OOC data-to-index identity alias",
                                 make_merge_alias_case(
                                     [&](auto&, auto& handoff_1) {
                                         handoff_1.artifact.data_file.identity =
                                             fixture.handoff.artifact.index_file.identity;
                                     },
                                     {}, {}));
    require_merge_alias_accepted(
        "worker-to-merge lease low limb reuse",
        make_merge_alias_case({},
                              [&](auto& started) {
                                  started.merged_lease.lease_id.limbs[0] =
                                      fixture.handoff.lease.lease_id.limbs[0];
                              },
                              {}));
    require_merge_alias_rejected("worker-to-merge exact 128-bit lease id reuse",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.lease_id =
                                                               fixture.handoff.lease.lease_id;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge lease owner alias",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.owner_marker =
                                                               fixture.handoff.lease.owner_marker;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge directory alias with distinct stem",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.directory =
                                                               fixture.handoff.lease.directory;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge owner-to-directory alias",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.owner_marker =
                                                               fixture.handoff.lease.directory;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge directory-to-owner alias",
                                 make_merge_alias_case({},
                                                       [&](auto& started) {
                                                           started.merged_lease.directory =
                                                               fixture.handoff.lease.owner_marker;
                                                       },
                                                       {}));
    require_merge_alias_rejected("worker-to-merge same store with different generation",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.descriptor.store_id =
                                         fixture.handoff.artifact.descriptor.store_id;
                                 }));
    require_merge_alias_accepted("worker-to-merge OOC generation reuse",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.descriptor.generation =
                                         fixture.handoff.artifact.descriptor.generation;
                                 }));
    require_merge_alias_rejected("worker-to-merge OOC index identity alias",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.index_file.identity =
                                         fixture.handoff.artifact.index_file.identity;
                                 }));
    require_merge_alias_rejected("worker-to-merge OOC data identity alias",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.data_file.identity =
                                         fixture.handoff.artifact.data_file.identity;
                                 }));
    require_merge_alias_rejected("worker-to-merge OOC index-to-data identity alias",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.index_file.identity =
                                         fixture.handoff.artifact.data_file.identity;
                                 }));
    require_merge_alias_rejected("worker-to-merge OOC data-to-index identity alias",
                                 make_merge_alias_case({}, {}, [&](auto& prepared) {
                                     prepared.merged_artifact.data_file.identity =
                                         fixture.handoff.artifact.index_file.identity;
                                 }));

    for (const auto& [control_name, control_identity] : manifest_control_identities) {
        for (const bool mutate_owner : {true, false}) {
            const auto value =
                make_merge_alias_case({},
                                      [&](auto& started) {
                                          if (mutate_owner) {
                                              started.merged_lease.owner_marker = control_identity;
                                          } else {
                                              started.merged_lease.directory = control_identity;
                                          }
                                      },
                                      {});
            const std::string context = std::string("merged lease ") +
                                        (mutate_owner ? "owner" : "directory") + " aliases " +
                                        std::string(control_name);
            require_merge_alias_rejected(context, value);
        }
        for (const bool mutate_index : {true, false}) {
            const auto value = make_merge_alias_case({}, {}, [&](auto& prepared) {
                if (mutate_index) {
                    prepared.merged_artifact.index_file.identity = control_identity;
                } else {
                    prepared.merged_artifact.data_file.identity = control_identity;
                }
            });
            const std::string context = std::string("merged artifact ") +
                                        (mutate_index ? "index" : "data") + " aliases " +
                                        std::string(control_name);
            require_merge_alias_rejected(context, value);
        }
    }

    const auto worker_format_drift_case = make_merge_alias_case(
        [&](auto&, auto& handoff_1) { ++handoff_1.artifact.descriptor.format_version; }, {}, {});
    require_merge_alias_rejected("merge dependency rejects worker OOC format drift",
                                 worker_format_drift_case);

    const auto merged_format_drift_case = make_merge_alias_case(
        {}, {}, [&](auto& prepared) { ++prepared.merged_artifact.descriptor.format_version; });
    const std::array merged_format_starts = {merged_format_drift_case.started};
    require_failed(sieve::validate_merge_predecessor_chain(fixture.manifest, merged_format_starts,
                                                           &merged_format_drift_case.prepared,
                                                           &merged_format_drift_case.commit),
                   "merge prepared and commit format bind manifest OOC version");

    auto bundle_owner_as_index = fixture.handoff;
    bundle_owner_as_index.artifact.index_file.identity = bundle_owner_as_index.lease.owner_marker;
    require_reseal_failed(std::move(bundle_owner_as_index),
                          "worker bundle rejects lease owner as index identity");
    auto bundle_directory_as_data = fixture.handoff;
    bundle_directory_as_data.artifact.data_file.identity = bundle_directory_as_data.lease.directory;
    require_reseal_failed(std::move(bundle_directory_as_data),
                          "worker bundle rejects lease directory as data identity");
    auto bundle_owner_as_data = fixture.handoff;
    bundle_owner_as_data.artifact.data_file.identity = bundle_owner_as_data.lease.owner_marker;
    require_reseal_failed(std::move(bundle_owner_as_data),
                          "worker bundle rejects lease owner as data identity");
    auto bundle_directory_as_index = fixture.handoff;
    bundle_directory_as_index.artifact.index_file.identity =
        bundle_directory_as_index.lease.directory;
    require_reseal_failed(std::move(bundle_directory_as_index),
                          "worker bundle rejects lease directory as index identity");

    auto case_fold_attempt_0 = fixture.failure_attempt_0;
    case_fold_attempt_0.lease.lease_id = lease_id_with_seed(500);
    case_fold_attempt_0.lease.owner_marker = native_identity(500);
    case_fold_attempt_0.lease.directory = native_identity(550);
    case_fold_attempt_0.lease.relative_stem = "chunk_1_attempt_00";
    case_fold_attempt_0 = reseal(std::move(case_fold_attempt_0));
    auto case_fold_attempt_1 = fixture.failure_attempt_1;
    case_fold_attempt_1.predecessor_digest = case_fold_attempt_0.self_digest;
    case_fold_attempt_1.lease.lease_id = lease_id_with_seed(501);
    case_fold_attempt_1.lease.owner_marker = native_identity(510);
    case_fold_attempt_1.lease.directory = case_fold_attempt_0.lease.directory;
    case_fold_attempt_1.lease.relative_stem = "chunk_1_attempt_01";
    case_fold_attempt_1 = reseal(std::move(case_fold_attempt_1));
    const std::array case_fold_attempts = {case_fold_attempt_0, case_fold_attempt_1};
    require_failed(sieve::validate_worker_attempt_chain(fixture.manifest, 1, case_fold_attempts,
                                                        nullptr, nullptr),
                   "lease directory reuse conflicts even with distinct stems");

    auto fake_merge_started = fixture.merge_started;
    perturb_digest(fake_merge_started.ordered_inputs[0].handoff_digest);
    fake_merge_started = reseal(std::move(fake_merge_started));
    auto fake_merge_prepared = fixture.merge_prepared;
    fake_merge_prepared.merge_started_digest = fake_merge_started.self_digest;
    fake_merge_prepared.ordered_inputs = fake_merge_started.ordered_inputs;
    fake_merge_prepared = reseal(std::move(fake_merge_prepared));
    auto fake_merge_commit = fixture.merge_commit;
    fake_merge_commit.chunks[0].input = fake_merge_started.ordered_inputs[0];
    fake_merge_commit.merge_prepared_digest = fake_merge_prepared.self_digest;
    fake_merge_commit = reseal(std::move(fake_merge_commit));
    const std::array fake_merge_starts = {fake_merge_started};
    require_ok(sieve::validate_merge_predecessor_chain(fixture.manifest, fake_merge_starts,
                                                       &fake_merge_prepared, &fake_merge_commit),
               "internally resealed merge chain remains structurally valid");
    require_failed(sieve::validate_merge_dependency_chain(fixture.manifest, terminal_evidence,
                                                          fake_merge_starts, &fake_merge_prepared,
                                                          &fake_merge_commit),
                   "fully resealed fake merge projection lacks worker evidence");

    require_failed(sieve::validate_merge_predecessor_chain(fixture.manifest, merge_starts, nullptr,
                                                           &fixture.merge_commit),
                   "merge commit without prepared");

    auto wrong_prepared = fixture.merge_prepared;
    perturb_digest(wrong_prepared.merge_started_digest);
    wrong_prepared = reseal(std::move(wrong_prepared));
    require_failed(sieve::validate_merge_predecessor_chain(fixture.manifest, merge_starts,
                                                           &wrong_prepared, nullptr),
                   "prepared merge references wrong start");

    auto extra_merge_start = fixture.merge_started;
    extra_merge_start.merge_attempt_ordinal = 1;
    extra_merge_start.predecessor_digest = fixture.merge_started.self_digest;
    extra_merge_start.merged_lease = lease_identity(201, "merged_attempt_1");
    extra_merge_start = reseal(std::move(extra_merge_start));
    const std::array starts_after_prepared = {fixture.merge_started, extra_merge_start};
    require_failed(sieve::validate_merge_predecessor_chain(fixture.manifest, starts_after_prepared,
                                                           &fixture.merge_prepared,
                                                           &fixture.merge_commit),
                   "prepared merge must reference tail start");

    const std::array consumption_starts = {fixture.consumption_started};
    require_ok(sieve::validate_consumption_predecessor_chain(
                   fixture.manifest, fixture.merge_commit, consumption_starts,
                   &fixture.successor_prepared, &fixture.consumption_ack),
               "valid consumption chain");

    struct ConsumptionAliasCase final {
        sieve::ConsumptionStartedV1 started;
        sieve::SuccessorPreparedV1 successor;
        sieve::WaveConsumptionAckV1 ack;
    };
    const auto make_consumption_alias_case = [&](bool alias_lease, bool alias_artifact) {
        ConsumptionAliasCase value;
        value.started = fixture.consumption_started;
        if (alias_lease) {
            value.started.successor_lease = fixture.merge_commit.merged_lease;
        }
        value.started = reseal(std::move(value.started));

        value.successor = fixture.successor_prepared;
        value.successor.consumption_started_digest = value.started.self_digest;
        value.successor.successor_lease = value.started.successor_lease;
        if (alias_artifact) {
            value.successor.successor_artifact = fixture.merge_commit.merged_artifact;
        }
        value.successor = reseal(std::move(value.successor));

        value.ack = fixture.consumption_ack;
        value.ack.consumption_started_digest = value.started.self_digest;
        value.ack.successor_prepared_digest = value.successor.self_digest;
        value.ack.successor_artifact = value.successor.successor_artifact;
        value.ack.successor_semantic_digest = value.successor.successor_semantic_digest;
        value.ack.successor_cleanup_authority_identity = value.started.successor_lease.owner_marker;
        value.ack = reseal(std::move(value.ack));
        return value;
    };
    using ConsumptionStartMutation = std::function<void(sieve::ConsumptionStartedV1&)>;
    using SuccessorMutation = std::function<void(sieve::SuccessorPreparedV1&)>;
    const auto make_consumption_identity_case = [&](const ConsumptionStartMutation& mutate_start,
                                                    const SuccessorMutation& mutate_successor) {
        ConsumptionAliasCase value;
        value.started = fixture.consumption_started;
        if (mutate_start) {
            mutate_start(value.started);
        }
        value.started = reseal(std::move(value.started));

        value.successor = fixture.successor_prepared;
        value.successor.consumption_started_digest = value.started.self_digest;
        value.successor.successor_lease = value.started.successor_lease;
        if (mutate_successor) {
            mutate_successor(value.successor);
        }
        value.successor = reseal(std::move(value.successor));

        value.ack = fixture.consumption_ack;
        value.ack.consumption_started_digest = value.started.self_digest;
        value.ack.successor_prepared_digest = value.successor.self_digest;
        value.ack.successor_artifact = value.successor.successor_artifact;
        value.ack.successor_semantic_digest = value.successor.successor_semantic_digest;
        value.ack.successor_cleanup_authority_identity = value.started.successor_lease.owner_marker;
        value.ack = reseal(std::move(value.ack));
        return value;
    };
    const auto require_consumption_alias_rejected = [&](std::string_view name,
                                                        const ConsumptionAliasCase& value) {
        const std::array starts = {value.started};
        const auto status = sieve::validate_consumption_predecessor_chain(
            fixture.manifest, fixture.merge_commit, starts, &value.successor, &value.ack);
        if (status) {
            fail(name, __LINE__, "consumption chain accepted merged identity alias");
        }
    };
    require_consumption_alias_rejected("successor exact lease alias",
                                       make_consumption_alias_case(true, false));
    require_consumption_alias_rejected("successor exact artifact alias",
                                       make_consumption_alias_case(false, true));
    require_consumption_alias_rejected("successor exact lease and artifact alias",
                                       make_consumption_alias_case(true, true));

    auto successor_format_mismatch = fixture.successor_prepared;
    ++successor_format_mismatch.successor_artifact.descriptor.format_version;
    successor_format_mismatch = reseal(std::move(successor_format_mismatch));
    require_failed(sieve::validate_consumption_predecessor_chain(
                       fixture.manifest, fixture.merge_commit, consumption_starts,
                       &successor_format_mismatch, nullptr),
                   "successor artifact format binds consumption start");

    const auto matching_successor_format = make_consumption_identity_case(
        [](auto& started) { ++started.successor_format_version; },
        [](auto& successor) { ++successor.successor_artifact.descriptor.format_version; });
    const std::array matching_successor_starts = {matching_successor_format.started};
    require_ok(sieve::validate_consumption_predecessor_chain(
                   fixture.manifest, fixture.merge_commit, matching_successor_starts,
                   &matching_successor_format.successor, &matching_successor_format.ack),
               "successor format may advance when start binds the same version");

    for (const auto& [control_name, control_identity] : manifest_control_identities) {
        for (const bool mutate_owner : {true, false}) {
            const auto value = make_consumption_identity_case(
                [&](auto& started) {
                    if (mutate_owner) {
                        started.successor_lease.owner_marker = control_identity;
                    } else {
                        started.successor_lease.directory = control_identity;
                    }
                },
                {});
            const std::string context = std::string("successor lease ") +
                                        (mutate_owner ? "owner" : "directory") + " aliases " +
                                        std::string(control_name);
            require_consumption_alias_rejected(context, value);
        }
        for (const bool mutate_index : {true, false}) {
            const auto value = make_consumption_identity_case({}, [&](auto& successor) {
                if (mutate_index) {
                    successor.successor_artifact.index_file.identity = control_identity;
                } else {
                    successor.successor_artifact.data_file.identity = control_identity;
                }
            });
            const std::string context = std::string("successor artifact ") +
                                        (mutate_index ? "index" : "data") + " aliases " +
                                        std::string(control_name);
            require_consumption_alias_rejected(context, value);
        }
    }

    require_failed(sieve::validate_consumption_predecessor_chain(
                       fixture.manifest, fixture.merge_commit, consumption_starts, nullptr,
                       &fixture.consumption_ack),
                   "consumption ACK without prepared successor");

    auto wrong_successor = fixture.successor_prepared;
    perturb_digest(wrong_successor.consumption_started_digest);
    wrong_successor = reseal(std::move(wrong_successor));
    require_failed(
        sieve::validate_consumption_predecessor_chain(
            fixture.manifest, fixture.merge_commit, consumption_starts, &wrong_successor, nullptr),
        "successor references wrong consumption start");

    const auto validate_worker_authorization =
        [&](const sieve::WaveMergeCommitV1& commit,
            const sieve::ArtifactCleanupAuthorizedV1& authorization,
            const sieve::WorkerHandoffV1* worker_handoff) {
            return sieve::validate_artifact_cleanup_dependencies(
                fixture.manifest, commit, no_consumption_starts, nullptr, nullptr, authorization,
                worker_handoff, nullptr);
        };
    const auto validate_merged_authorization =
        [&](const sieve::WaveMergeCommitV1& commit,
            std::span<const sieve::ConsumptionStartedV1> starts,
            const sieve::SuccessorPreparedV1* successor, const sieve::WaveConsumptionAckV1* ack,
            const sieve::ArtifactCleanupAuthorizedV1& authorization,
            const sieve::MergePreparedV1* merge_prepared) {
            return sieve::validate_artifact_cleanup_dependencies(fixture.manifest, commit, starts,
                                                                 successor, ack, authorization,
                                                                 nullptr, merge_prepared);
        };

    require_ok(validate_worker_authorization(fixture.merge_commit,
                                             fixture.cleanup_authorizations[0], &fixture.handoff),
               "valid worker cleanup authorization closure");
    require_ok(validate_worker_authorization(fixture.merge_commit,
                                             fixture.cleanup_authorizations[1], &fixture.handoff_1),
               "valid second worker cleanup authorization closure");
    require_ok(validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                             &fixture.successor_prepared, &fixture.consumption_ack,
                                             fixture.cleanup_authorizations[2],
                                             &fixture.merge_prepared),
               "valid merged cleanup authorization closure");
    require_failed(validate_worker_authorization(fixture.merge_commit,
                                                 fixture.cleanup_authorizations[0], nullptr),
                   "worker cleanup mint requires exact handoff");
    require_failed(validate_merged_authorization(
                       fixture.merge_commit, consumption_starts, &fixture.successor_prepared,
                       &fixture.consumption_ack, fixture.cleanup_authorizations[2], nullptr),
                   "merged cleanup mint requires exact prepared evidence");
    for (std::size_t index = 0; index < fixture.cleanup_authorizations.size(); ++index) {
        require_ok(sieve::validate_artifact_cleanup_completion_dependency(
                       fixture.cleanup_authorizations[index], fixture.cleanup_completions[index]),
                   "canonical cleanup recovery closure");
    }

    auto cleanup_bundle_alias = fixture.cleanup_authorizations[0];
    cleanup_bundle_alias.artifact.index_file.identity = cleanup_bundle_alias.lease.owner_marker;
    require_reseal_failed(std::move(cleanup_bundle_alias),
                          "cleanup authorization rejects bundle-native alias");

    auto cleanup_intent_alias = fixture.cleanup_completions[0];
    cleanup_intent_alias.cleanup_intent_identity =
        fixture.cleanup_authorizations[0].lease.owner_marker;
    cleanup_intent_alias = reseal(std::move(cleanup_intent_alias));
    require_failed(sieve::validate_artifact_cleanup_completion_dependency(
                       fixture.cleanup_authorizations[0], cleanup_intent_alias),
                   "cleanup recovery rejects intent alias with authorized lease");
    cleanup_intent_alias = fixture.cleanup_completions[0];
    cleanup_intent_alias.cleanup_intent_identity =
        fixture.cleanup_authorizations[0].artifact.index_file.identity;
    cleanup_intent_alias = reseal(std::move(cleanup_intent_alias));
    require_failed(sieve::validate_artifact_cleanup_completion_dependency(
                       fixture.cleanup_authorizations[0], cleanup_intent_alias),
                   "cleanup recovery rejects intent alias with authorized artifact");

    const auto make_worker_authorization_for_case = [&](const MergeAliasCase& value) {
        auto authorization = fixture.cleanup_authorizations[1];
        authorization.authorizer_record_digest = value.commit.self_digest;
        authorization.lease = value.handoff_1.lease;
        authorization.handoff_digest = value.handoff_1.self_digest;
        authorization.artifact = value.handoff_1.artifact;
        return reseal(std::move(authorization));
    };
    const auto root_aliased_worker_case = make_merge_alias_case(
        [&](auto& attempt_1, auto&) {
            attempt_1.lease.owner_marker = fixture.manifest.wave_root_identity;
        },
        {}, {});
    const auto root_aliased_worker_authorization =
        make_worker_authorization_for_case(root_aliased_worker_case);
    require_failed(validate_worker_authorization(root_aliased_worker_case.commit,
                                                 root_aliased_worker_authorization,
                                                 &root_aliased_worker_case.handoff_1),
                   "cleanup mint rejects worker lease alias with wave root");

    const auto lock_aliased_worker_case = make_merge_alias_case(
        [&](auto&, auto& handoff_1) {
            handoff_1.artifact.data_file.identity = fixture.manifest.permanent_lock_identity;
        },
        {}, {});
    const auto lock_aliased_worker_authorization =
        make_worker_authorization_for_case(lock_aliased_worker_case);
    require_failed(validate_worker_authorization(lock_aliased_worker_case.commit,
                                                 lock_aliased_worker_authorization,
                                                 &lock_aliased_worker_case.handoff_1),
                   "cleanup mint rejects worker artifact alias with permanent lock");

    const auto worker_format_drift_authorization =
        make_worker_authorization_for_case(worker_format_drift_case);
    require_failed(validate_worker_authorization(worker_format_drift_case.commit,
                                                 worker_format_drift_authorization,
                                                 &worker_format_drift_case.handoff_1),
                   "cleanup mint rejects fully resealed worker OOC format drift");

    auto wrong_worker_authorization = fixture.cleanup_authorizations[0];
    perturb_digest(wrong_worker_authorization.manifest_digest);
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds manifest");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    perturb_digest(wrong_worker_authorization.authorizer_record_digest);
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds commit authorizer");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    wrong_worker_authorization.authorizer = sieve::CleanupAuthorizerKindV1::consumption_ack_merged;
    wrong_worker_authorization.artifact_kind = sieve::CleanupArtifactKindV1::merged;
    wrong_worker_authorization.manifest_order_ordinal = 0;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup rejects wrong authorizer kind");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    wrong_worker_authorization.manifest_order_ordinal = 1;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds manifest ordinal summary");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    ++wrong_worker_authorization.lease.lease_id.limbs[1];
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds summary lease high limb");
    require_failed(sieve::validate_artifact_cleanup_completion_dependency(
                       wrong_worker_authorization, fixture.cleanup_completions[0]),
                   "cleanup recovery rejects lease high-limb authorization drift");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    ++wrong_worker_authorization.lease.owner_marker.generation;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds full handoff lease");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    perturb_digest(wrong_worker_authorization.handoff_digest);
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds summary handoff");

    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    ++wrong_worker_authorization.artifact.descriptor.store_id;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds full handoff artifact");

    using CleanupAuthorizationMutation =
        std::pair<std::string_view, std::function<void(sieve::ArtifactCleanupAuthorizedV1&)>>;
    const std::vector<CleanupAuthorizationMutation> exact_target_mutations = {
        {"lease owner", [](auto& value) { value.lease.owner_marker = native_identity(900); }},
        {"lease directory", [](auto& value) { value.lease.directory = native_identity(910); }},
        {"lease stem", [](auto& value) { value.lease.relative_stem = "alternate_worker"; }},
        {"OOC format version", [](auto& value) { ++value.artifact.descriptor.format_version; }},
        {"OOC store id", [](auto& value) { ++value.artifact.descriptor.store_id; }},
        {"OOC generation", [](auto& value) { ++value.artifact.descriptor.generation; }},
        {"OOC relation count",
         [](auto& value) {
             ++value.artifact.descriptor.relation_count;
             ++value.artifact.sequence_receipt.relation_count;
         }},
        {"OOC data end",
         [](auto& value) {
             ++value.artifact.descriptor.data_end;
             ++value.artifact.data_file.extent;
         }},
        {"index file identity",
         [](auto& value) { value.artifact.index_file.identity = native_identity(920); }},
        {"index file extent", [](auto& value) { ++value.artifact.index_file.extent; }},
        {"data file identity",
         [](auto& value) { value.artifact.data_file.identity = native_identity(930); }},
        {"data file extent",
         [](auto& value) {
             value.artifact.data_file.extent += 2;
             value.artifact.descriptor.data_end += 2;
         }},
        {"sequence receipt low", [](auto& value) { ++value.artifact.sequence_receipt.low; }},
        {"sequence receipt high", [](auto& value) { ++value.artifact.sequence_receipt.high; }},
        {"corpus digest", [](auto& value) { perturb_digest(value.artifact.corpus_sha256); }},
    };
    for (const auto& [name, mutate] : exact_target_mutations) {
        auto changed = fixture.cleanup_authorizations[0];
        mutate(changed);
        changed = reseal(std::move(changed));
        const auto status =
            validate_worker_authorization(fixture.merge_commit, changed, &fixture.handoff);
        if (status) {
            fail(name, __LINE__, "resealed worker cleanup target drift accepted");
        }
    }

    auto laundered_worker_authorization = fixture.cleanup_authorizations[0];
    laundered_worker_authorization.lease.owner_marker = native_identity(700);
    laundered_worker_authorization.lease.directory = native_identity(710);
    laundered_worker_authorization.lease.relative_stem = "laundered_worker";
    laundered_worker_authorization.artifact.descriptor.format_version = 2;
    laundered_worker_authorization.artifact.descriptor.store_id = 701;
    laundered_worker_authorization.artifact.descriptor.generation = 702;
    laundered_worker_authorization.artifact.descriptor.data_end = 8192;
    laundered_worker_authorization.artifact.index_file.identity = native_identity(720);
    laundered_worker_authorization.artifact.index_file.extent = 1024;
    laundered_worker_authorization.artifact.data_file.identity = native_identity(730);
    laundered_worker_authorization.artifact.data_file.extent = 8192;
    laundered_worker_authorization = reseal(std::move(laundered_worker_authorization));
    require_failed(validate_worker_authorization(fixture.merge_commit,
                                                 laundered_worker_authorization, nullptr),
                   "worker cleanup cannot launder unprojected lease and OOC identity");
    require_failed(validate_worker_authorization(fixture.merge_commit,
                                                 laundered_worker_authorization, &fixture.handoff),
                   "worker cleanup exact handoff rejects laundered lease and OOC identity");

    auto wrong_worker_commit = fixture.merge_commit;
    ++wrong_worker_commit.chunks[0].input.sequence_receipt.low;
    wrong_worker_commit = reseal(std::move(wrong_worker_commit));
    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    wrong_worker_authorization.authorizer_record_digest = wrong_worker_commit.self_digest;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(wrong_worker_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup authorization binds commit summary");

    wrong_worker_commit = fixture.merge_commit;
    perturb_digest(wrong_worker_commit.work_digest);
    wrong_worker_commit = reseal(std::move(wrong_worker_commit));
    wrong_worker_authorization = fixture.cleanup_authorizations[0];
    wrong_worker_authorization.authorizer_record_digest = wrong_worker_commit.self_digest;
    wrong_worker_authorization = reseal(std::move(wrong_worker_authorization));
    require_failed(validate_worker_authorization(wrong_worker_commit, wrong_worker_authorization,
                                                 &fixture.handoff),
                   "worker cleanup rejects fully resealed commit work drift");

    auto wrong_merged_authorization = fixture.cleanup_authorizations[2];
    perturb_digest(wrong_merged_authorization.manifest_digest);
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds manifest");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    wrong_merged_authorization.authorizer = sieve::CleanupAuthorizerKindV1::merge_commit_worker;
    wrong_merged_authorization.artifact_kind = sieve::CleanupArtifactKindV1::worker;
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup rejects wrong authorizer kind");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    perturb_digest(wrong_merged_authorization.authorizer_record_digest);
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds ACK");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    wrong_merged_authorization.manifest_order_ordinal = 1;
    require_failed(validate_value(wrong_merged_authorization, false),
                   "merged cleanup authorization requires canonical ordinal");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    ++wrong_merged_authorization.lease.lease_id.limbs[1];
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds commit lease high limb");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    perturb_digest(wrong_merged_authorization.handoff_digest);
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds prepared digest");

    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    ++wrong_merged_authorization.artifact.descriptor.store_id;
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      wrong_merged_authorization, &fixture.merge_prepared),
        "merged cleanup authorization binds commit artifact");

    auto wrong_ack = fixture.consumption_ack;
    perturb_digest(wrong_ack.merge_commit_digest);
    wrong_ack = reseal(std::move(wrong_ack));
    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    wrong_merged_authorization.authorizer_record_digest = wrong_ack.self_digest;
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(validate_merged_authorization(
                       fixture.merge_commit, consumption_starts, &fixture.successor_prepared,
                       &wrong_ack, wrong_merged_authorization, &fixture.merge_prepared),
                   "merged cleanup authorization binds ACK to commit");

    auto wrong_merged_commit = fixture.merge_commit;
    perturb_digest(wrong_merged_commit.work_digest);
    wrong_merged_commit = reseal(std::move(wrong_merged_commit));
    auto wrong_consumption_started = fixture.consumption_started;
    wrong_consumption_started.merge_commit_digest = wrong_merged_commit.self_digest;
    wrong_consumption_started = reseal(std::move(wrong_consumption_started));
    const std::array wrong_consumption_starts = {wrong_consumption_started};
    auto wrong_successor_for_commit = fixture.successor_prepared;
    wrong_successor_for_commit.consumption_started_digest = wrong_consumption_started.self_digest;
    wrong_successor_for_commit = reseal(std::move(wrong_successor_for_commit));
    wrong_ack = fixture.consumption_ack;
    wrong_ack.merge_commit_digest = wrong_merged_commit.self_digest;
    wrong_ack.consumption_started_digest = wrong_consumption_started.self_digest;
    wrong_ack.successor_prepared_digest = wrong_successor_for_commit.self_digest;
    wrong_ack = reseal(std::move(wrong_ack));
    wrong_merged_authorization = fixture.cleanup_authorizations[2];
    wrong_merged_authorization.authorizer_record_digest = wrong_ack.self_digest;
    wrong_merged_authorization = reseal(std::move(wrong_merged_authorization));
    require_failed(validate_merged_authorization(
                       wrong_merged_commit, wrong_consumption_starts, &wrong_successor_for_commit,
                       &wrong_ack, wrong_merged_authorization, &fixture.merge_prepared),
                   "merged cleanup rejects fully resealed commit work drift");

    auto wrong_merge_prepared = fixture.merge_prepared;
    ++wrong_merge_prepared.merged_artifact.descriptor.store_id;
    wrong_merge_prepared = reseal(std::move(wrong_merge_prepared));
    require_failed(
        validate_merged_authorization(fixture.merge_commit, consumption_starts,
                                      &fixture.successor_prepared, &fixture.consumption_ack,
                                      fixture.cleanup_authorizations[2], &wrong_merge_prepared),
        "merged cleanup authorization binds prepared artifact");

    auto wrong_cleanup_completion = fixture.cleanup_completions[0];
    perturb_digest(wrong_cleanup_completion.authorization_digest);
    wrong_cleanup_completion = reseal(std::move(wrong_cleanup_completion));
    require_failed(sieve::validate_artifact_cleanup_completion_dependency(
                       fixture.cleanup_authorizations[0], wrong_cleanup_completion),
                   "cleanup completion binds authorization");

    require_ok(sieve::validate_wave_completion_dependencies(
                   fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                   fixture.consumption_ack, consumption_starts, fixture.cleanup_authorizations,
                   fixture.cleanup_completions, fixture.completed),
               "valid completion dependency closure");

    auto final_high_limb_authorizations = fixture.cleanup_authorizations;
    ++final_high_limb_authorizations[0].lease.lease_id.limbs[1];
    final_high_limb_authorizations[0] = reseal(std::move(final_high_limb_authorizations[0]));
    auto final_high_limb_completions = fixture.cleanup_completions;
    final_high_limb_completions[0].authorization_digest =
        final_high_limb_authorizations[0].self_digest;
    final_high_limb_completions[0] = reseal(std::move(final_high_limb_completions[0]));
    require_ok(sieve::validate_artifact_cleanup_completion_dependency(
                   final_high_limb_authorizations[0], final_high_limb_completions[0]),
               "rebound recovery record remains internally canonical");
    auto final_high_limb_completed = fixture.completed;
    final_high_limb_completed.cleanup_confirmations[0].authorization_digest =
        final_high_limb_authorizations[0].self_digest;
    final_high_limb_completed.cleanup_confirmations[0].completion_digest =
        final_high_limb_completions[0].self_digest;
    final_high_limb_completed = reseal(std::move(final_high_limb_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, final_high_limb_authorizations,
                       final_high_limb_completions, final_high_limb_completed),
                   "WaveCompleted rejects fully resealed lease high-limb drift");

    auto control_aliased_authorizations = fixture.cleanup_authorizations;
    control_aliased_authorizations[0].lease.owner_marker = fixture.manifest.wave_root_identity;
    control_aliased_authorizations[0] = reseal(std::move(control_aliased_authorizations[0]));
    auto control_aliased_completions = fixture.cleanup_completions;
    control_aliased_completions[0].authorization_digest =
        control_aliased_authorizations[0].self_digest;
    control_aliased_completions[0] = reseal(std::move(control_aliased_completions[0]));
    auto control_aliased_completed = fixture.completed;
    control_aliased_completed.cleanup_confirmations[0].authorization_digest =
        control_aliased_authorizations[0].self_digest;
    control_aliased_completed.cleanup_confirmations[0].completion_digest =
        control_aliased_completions[0].self_digest;
    control_aliased_completed = reseal(std::move(control_aliased_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, control_aliased_authorizations,
                       control_aliased_completions, control_aliased_completed),
                   "final closure rejects cleanup lease alias with wave root");

    auto control_aliased_intent_completions = fixture.cleanup_completions;
    control_aliased_intent_completions[0].cleanup_intent_identity =
        fixture.manifest.permanent_lock_identity;
    control_aliased_intent_completions[0] =
        reseal(std::move(control_aliased_intent_completions[0]));
    auto control_aliased_intent_completed = fixture.completed;
    control_aliased_intent_completed.cleanup_confirmations[0].completion_digest =
        control_aliased_intent_completions[0].self_digest;
    control_aliased_intent_completed = reseal(std::move(control_aliased_intent_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, fixture.cleanup_authorizations,
                       control_aliased_intent_completions, control_aliased_intent_completed),
                   "final closure rejects cleanup intent alias with permanent lock");

    auto format_drift_authorizations = fixture.cleanup_authorizations;
    ++format_drift_authorizations[0].artifact.descriptor.format_version;
    format_drift_authorizations[0] = reseal(std::move(format_drift_authorizations[0]));
    auto format_drift_completions = fixture.cleanup_completions;
    format_drift_completions[0].authorization_digest = format_drift_authorizations[0].self_digest;
    format_drift_completions[0] = reseal(std::move(format_drift_completions[0]));
    auto format_drift_completed = fixture.completed;
    format_drift_completed.cleanup_confirmations[0].authorization_digest =
        format_drift_authorizations[0].self_digest;
    format_drift_completed.cleanup_confirmations[0].completion_digest =
        format_drift_completions[0].self_digest;
    format_drift_completed = reseal(std::move(format_drift_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, format_drift_authorizations,
                       format_drift_completions, format_drift_completed),
                   "final closure rejects cleanup authorization OOC format laundering");

    auto wrong_completed = fixture.completed;
    perturb_digest(wrong_completed.consumption_ack_digest);
    wrong_completed = reseal(std::move(wrong_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, fixture.cleanup_authorizations,
                       fixture.cleanup_completions, wrong_completed),
                   "completed record references wrong ACK");

    const std::span<const sieve::ArtifactCleanupAuthorizedV1> missing_authorization(
        fixture.cleanup_authorizations.data(), fixture.cleanup_authorizations.size() - 1);
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, missing_authorization,
                       fixture.cleanup_completions, fixture.completed),
                   "completed record requires all cleanup authorizations");

    const std::span<const sieve::ArtifactCleanupCompletedV1> missing_cleanup(
        fixture.cleanup_completions.data(), fixture.cleanup_completions.size() - 1);
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, fixture.cleanup_authorizations,
                       missing_cleanup, fixture.completed),
                   "completed record requires all cleanup confirmations");

    auto forged_authorizations = fixture.cleanup_authorizations;
    perturb_digest(forged_authorizations[0].authorizer_record_digest);
    forged_authorizations[0] = reseal(std::move(forged_authorizations[0]));
    auto forged_completions = fixture.cleanup_completions;
    forged_completions[0].authorization_digest = forged_authorizations[0].self_digest;
    forged_completions[0] = reseal(std::move(forged_completions[0]));
    auto forged_completed = fixture.completed;
    forged_completed.cleanup_confirmations[0].authorization_digest =
        forged_authorizations[0].self_digest;
    forged_completed.cleanup_confirmations[0].completion_digest = forged_completions[0].self_digest;
    forged_completed = reseal(std::move(forged_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, forged_authorizations,
                       forged_completions, forged_completed),
                   "matching arbitrary cleanup digests do not establish authority");

    auto duplicate_authorizations = fixture.cleanup_authorizations;
    duplicate_authorizations[1] = duplicate_authorizations[0];
    auto duplicate_completions = fixture.cleanup_completions;
    duplicate_completions[1].authorization_digest = duplicate_authorizations[1].self_digest;
    duplicate_completions[1] = reseal(std::move(duplicate_completions[1]));
    auto duplicate_completed = fixture.completed;
    duplicate_completed.cleanup_confirmations[1].authorization_digest =
        duplicate_authorizations[1].self_digest;
    duplicate_completed.cleanup_confirmations[1].completion_digest =
        duplicate_completions[1].self_digest;
    duplicate_completed = reseal(std::move(duplicate_completed));
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, duplicate_authorizations,
                       duplicate_completions, duplicate_completed),
                   "duplicate worker zero authorization cannot cover worker one");

    auto swapped_authorizations = fixture.cleanup_authorizations;
    auto swapped_completions = fixture.cleanup_completions;
    std::swap(swapped_authorizations[0], swapped_authorizations[1]);
    std::swap(swapped_completions[0], swapped_completions[1]);
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, fixture.successor_prepared,
                       fixture.consumption_ack, consumption_starts, swapped_authorizations,
                       swapped_completions, fixture.completed),
                   "swapped cleanup arrays violate manifest-order coverage");

    auto laundered_successor = fixture.successor_prepared;
    laundered_successor.consumption_started_digest = digest_with_seed(180);
    laundered_successor.successor_lease = lease_identity(800, "laundered_successor");
    laundered_successor.successor_artifact = corpus_artifact(810, 6);
    laundered_successor.successor_semantic_digest = digest_with_seed(181);
    laundered_successor = reseal(std::move(laundered_successor));
    auto laundered_ack = fixture.consumption_ack;
    laundered_ack.consumption_started_digest = laundered_successor.consumption_started_digest;
    laundered_ack.successor_prepared_digest = laundered_successor.self_digest;
    laundered_ack.successor_artifact = laundered_successor.successor_artifact;
    laundered_ack.successor_semantic_digest = laundered_successor.successor_semantic_digest;
    laundered_ack.successor_cleanup_authority_identity =
        laundered_successor.successor_lease.owner_marker;
    laundered_ack = reseal(std::move(laundered_ack));
    auto laundered_authorizations = fixture.cleanup_authorizations;
    laundered_authorizations[2].authorizer_record_digest = laundered_ack.self_digest;
    laundered_authorizations[2] = reseal(std::move(laundered_authorizations[2]));
    auto laundered_completions = fixture.cleanup_completions;
    laundered_completions[2].authorization_digest = laundered_authorizations[2].self_digest;
    laundered_completions[2] = reseal(std::move(laundered_completions[2]));
    auto laundered_completed = fixture.completed;
    laundered_completed.consumption_ack_digest = laundered_ack.self_digest;
    laundered_completed.successor_prepared_digest = laundered_successor.self_digest;
    laundered_completed.cleanup_confirmations[2].authorization_digest =
        laundered_authorizations[2].self_digest;
    laundered_completed.cleanup_confirmations[2].completion_digest =
        laundered_completions[2].self_digest;
    laundered_completed.successor_artifact = laundered_successor.successor_artifact;
    laundered_completed.successor_semantic_digest = laundered_successor.successor_semantic_digest;
    laundered_completed = reseal(std::move(laundered_completed));
    require_failed(validate_merged_authorization(
                       fixture.merge_commit, consumption_starts, &laundered_successor,
                       &laundered_ack, laundered_authorizations[2], &fixture.merge_prepared),
                   "merged cleanup mint rejects ACK without canonical consumption start");
    require_failed(sieve::validate_wave_completion_dependencies(
                       fixture.manifest, fixture.merge_commit, laundered_successor, laundered_ack,
                       consumption_starts, laundered_authorizations, laundered_completions,
                       laundered_completed),
                   "final closure rejects fully resealed ACK and successor laundering");
}

void test_empty_chunk_projection_and_merge() {
    ProtocolFixture fixture;

    auto manifest = fixture.manifest;
    manifest.effective_sq_begin = 2;
    manifest.effective_sq_end = 3;
    manifest.worker_count = 2;
    manifest.chunks = {
        {0, 2, 3, "chunk_nonempty"},
        {1, 3, 3, "chunk_empty"},
    };
    manifest = reseal(std::move(manifest));
    require_ok(validate_value(manifest), "workers may exceed nonempty special-Q ranges");

    auto attempt = fixture.attempt;
    attempt.manifest_digest = manifest.self_digest;
    attempt.sq_begin = 2;
    attempt.sq_end = 3;
    attempt.predecessor_digest = manifest.self_digest;
    attempt.lease.relative_stem = "chunk_nonempty_attempt_00";
    attempt = reseal(std::move(attempt));

    auto handoff = fixture.handoff;
    handoff.manifest_digest = manifest.self_digest;
    handoff.work_digest = manifest.work_sha256;
    handoff.wave_id = manifest.wave_id;
    handoff.sq_begin = 2;
    handoff.sq_end = 3;
    handoff.attempt_started_digest = attempt.self_digest;
    handoff.lease = attempt.lease;
    handoff.processed_sq_count = 1;
    handoff.next_sq_index = 3;
    handoff = reseal(std::move(handoff));

    sieve::TerminalChunkInputV1 nonempty_projection;
    nonempty_projection.chunk_id = 0;
    nonempty_projection.disposition = sieve::ChunkDispositionV1::handoff;
    nonempty_projection.sq_begin = 2;
    nonempty_projection.sq_end = 3;
    nonempty_projection.next_sq_index = 3;
    nonempty_projection.processed_sq_count = 1;
    nonempty_projection.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
    nonempty_projection.durable_attempt_count = 1;
    nonempty_projection.last_attempt_digest = attempt.self_digest;
    nonempty_projection.lease_id = handoff.lease.lease_id;
    nonempty_projection.handoff_digest = handoff.self_digest;
    nonempty_projection.raw_relation_count = handoff.relation_count;
    nonempty_projection.sequence_receipt = handoff.artifact.sequence_receipt;
    nonempty_projection.corpus_sha256 = handoff.artifact.corpus_sha256;

    sieve::TerminalChunkInputV1 empty_projection;
    empty_projection.chunk_id = 1;
    empty_projection.disposition = sieve::ChunkDispositionV1::empty;
    empty_projection.sq_begin = 3;
    empty_projection.sq_end = 3;
    empty_projection.next_sq_index = 3;
    empty_projection.completion_reason = sieve::WorkerCompletionReasonV1::zero_relations;

    CHECK(nonempty_projection.lease_id.limbs[0] != 0);
    CHECK(nonempty_projection.lease_id.limbs[1] != 0);
    CHECK(empty_projection.lease_id == sieve::LeaseIdV1{});

    const std::span<const sieve::AttemptStartedV1> no_attempts;
    require_ok(sieve::validate_terminal_chunk_projection(manifest, 1, no_attempts, nullptr,
                                                         empty_projection),
               "empty chunk has canonical zero-attempt projection");
    const std::array unexpected_attempts = {attempt};
    require_failed(sieve::validate_terminal_chunk_projection(manifest, 1, unexpected_attempts,
                                                             nullptr, empty_projection),
                   "empty chunk rejects attempts");
    require_failed(sieve::validate_terminal_chunk_projection(manifest, 1, no_attempts, &handoff,
                                                             empty_projection),
                   "empty chunk rejects handoff");

    using EmptyProjectionMutation =
        std::pair<std::string_view, std::function<void(sieve::TerminalChunkInputV1&)>>;
    const std::vector<EmptyProjectionMutation> invalid_empty_fields = {
        {"completion reason",
         [](auto& value) {
             value.completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted;
         }},
        {"processed count", [](auto& value) { value.processed_sq_count = 1; }},
        {"attempt count", [](auto& value) { value.durable_attempt_count = 1; }},
        {"last attempt digest",
         [](auto& value) { value.last_attempt_digest = digest_with_seed(210); }},
        {"lease id", [](auto& value) { value.lease_id = lease_id_with_seed(1); }},
        {"handoff digest", [](auto& value) { value.handoff_digest = digest_with_seed(211); }},
        {"raw relation count",
         [](auto& value) {
             value.raw_relation_count = 1;
             value.sequence_receipt.relation_count = 1;
         }},
        {"sequence receipt", [](auto& value) { value.sequence_receipt.low = 1; }},
        {"corpus digest", [](auto& value) { value.corpus_sha256 = digest_with_seed(212); }},
    };
    for (const auto& [name, mutate] : invalid_empty_fields) {
        auto changed = empty_projection;
        mutate(changed);
        const auto status =
            sieve::validate_terminal_chunk_projection(manifest, 1, no_attempts, nullptr, changed);
        if (status) {
            fail(name, __LINE__, "empty chunk accepted nonzero terminal field");
        }
    }

    auto merge_started = fixture.merge_started;
    merge_started.manifest_digest = manifest.self_digest;
    merge_started.work_digest = manifest.work_sha256;
    merge_started.ordered_inputs = {nonempty_projection, empty_projection};
    merge_started.predecessor_digest = manifest.self_digest;
    merge_started = reseal(std::move(merge_started));

    auto merge_prepared = fixture.merge_prepared;
    merge_prepared.manifest_digest = manifest.self_digest;
    merge_prepared.work_digest = manifest.work_sha256;
    merge_prepared.merge_started_digest = merge_started.self_digest;
    merge_prepared.ordered_inputs = merge_started.ordered_inputs;
    merge_prepared.input_relation_count = handoff.relation_count;
    merge_prepared.duplicate_relation_count = 0;
    merge_prepared.output_relation_count = handoff.relation_count;
    merge_prepared.per_chunk_retained_counts = {
        {0, handoff.relation_count},
        {1, 0},
    };
    merge_prepared.merged_artifact = corpus_artifact(6000, handoff.relation_count);
    merge_prepared.merged_lease = merge_started.merged_lease;
    merge_prepared = reseal(std::move(merge_prepared));

    auto merge_commit = fixture.merge_commit;
    merge_commit.manifest_digest = manifest.self_digest;
    merge_commit.work_digest = manifest.work_sha256;
    merge_commit.chunks = {
        {nonempty_projection, handoff.relation_count, {}},
        {empty_projection, 0, {}},
    };
    merge_commit.input_relation_count = merge_prepared.input_relation_count;
    merge_commit.duplicate_relation_count = 0;
    merge_commit.output_relation_count = merge_prepared.output_relation_count;
    merge_commit.merge_prepared_digest = merge_prepared.self_digest;
    merge_commit.merged_lease = merge_prepared.merged_lease;
    merge_commit.merged_artifact = merge_prepared.merged_artifact;
    merge_commit = reseal(std::move(merge_commit));

    const std::array attempts = {attempt};
    const std::array<sieve::ChunkTerminalEvidenceViewV1, 2> terminal_evidence = {{
        {std::span<const sieve::AttemptStartedV1>{attempts}, &handoff},
        {no_attempts, nullptr},
    }};
    const std::array merge_starts = {merge_started};
    require_ok(sieve::validate_merge_dependency_chain(manifest, terminal_evidence, merge_starts,
                                                      &merge_prepared, &merge_commit),
               "merge dependency chain accepts canonical empty input");

    auto consumption_started = fixture.consumption_started;
    consumption_started.merge_commit_digest = merge_commit.self_digest;
    consumption_started.manifest_digest = manifest.self_digest;
    consumption_started.predecessor_digest = manifest.self_digest;
    consumption_started = reseal(std::move(consumption_started));

    auto successor_prepared = fixture.successor_prepared;
    successor_prepared.consumption_started_digest = consumption_started.self_digest;
    successor_prepared.successor_lease = consumption_started.successor_lease;
    successor_prepared.successor_artifact =
        corpus_artifact(7000, merge_commit.output_relation_count);
    successor_prepared.input_relation_count = merge_commit.output_relation_count;
    successor_prepared.output_relation_count = merge_commit.output_relation_count;
    successor_prepared = reseal(std::move(successor_prepared));

    auto consumption_ack = fixture.consumption_ack;
    consumption_ack.merge_commit_digest = merge_commit.self_digest;
    consumption_ack.consumption_started_digest = consumption_started.self_digest;
    consumption_ack.successor_prepared_digest = successor_prepared.self_digest;
    consumption_ack.successor_artifact = successor_prepared.successor_artifact;
    consumption_ack.successor_semantic_digest = successor_prepared.successor_semantic_digest;
    consumption_ack.successor_cleanup_authority_identity =
        consumption_started.successor_lease.owner_marker;
    consumption_ack = reseal(std::move(consumption_ack));

    const std::array consumption_starts = {consumption_started};
    require_ok(sieve::validate_consumption_predecessor_chain(manifest, merge_commit,
                                                             consumption_starts,
                                                             &successor_prepared, &consumption_ack),
               "consumption chain advances beyond a trailing empty chunk");

    auto worker_cleanup_authorization = fixture.cleanup_authorizations[0];
    worker_cleanup_authorization.manifest_digest = manifest.self_digest;
    worker_cleanup_authorization.authorizer_record_digest = merge_commit.self_digest;
    worker_cleanup_authorization.manifest_order_ordinal = 0;
    worker_cleanup_authorization.lease = handoff.lease;
    worker_cleanup_authorization.handoff_digest = handoff.self_digest;
    worker_cleanup_authorization.artifact = handoff.artifact;
    worker_cleanup_authorization = reseal(std::move(worker_cleanup_authorization));

    const std::span<const sieve::ConsumptionStartedV1> no_consumption_starts;
    require_ok(sieve::validate_artifact_cleanup_dependencies(
                   manifest, merge_commit, no_consumption_starts, nullptr, nullptr,
                   worker_cleanup_authorization, &handoff, nullptr),
               "only the nonempty worker receives worker cleanup authority");

    auto merged_cleanup_authorization = fixture.cleanup_authorizations[2];
    merged_cleanup_authorization.manifest_digest = manifest.self_digest;
    merged_cleanup_authorization.authorizer_record_digest = consumption_ack.self_digest;
    merged_cleanup_authorization.lease = merge_commit.merged_lease;
    merged_cleanup_authorization.handoff_digest = merge_prepared.self_digest;
    merged_cleanup_authorization.artifact = merge_commit.merged_artifact;
    merged_cleanup_authorization = reseal(std::move(merged_cleanup_authorization));
    require_ok(sieve::validate_artifact_cleanup_dependencies(
                   manifest, merge_commit, consumption_starts, &successor_prepared,
                   &consumption_ack, merged_cleanup_authorization, nullptr, &merge_prepared),
               "merged corpus receives cleanup authority after consumption");

    auto worker_cleanup_completion = fixture.cleanup_completions[0];
    worker_cleanup_completion.authorization_digest = worker_cleanup_authorization.self_digest;
    worker_cleanup_completion.cleanup_intent_identity = native_identity(9000);
    worker_cleanup_completion = reseal(std::move(worker_cleanup_completion));
    require_ok(sieve::validate_artifact_cleanup_completion_dependency(worker_cleanup_authorization,
                                                                      worker_cleanup_completion),
               "nonempty worker cleanup completes");

    auto merged_cleanup_completion = fixture.cleanup_completions[2];
    merged_cleanup_completion.authorization_digest = merged_cleanup_authorization.self_digest;
    merged_cleanup_completion.cleanup_intent_identity = native_identity(9010);
    merged_cleanup_completion = reseal(std::move(merged_cleanup_completion));
    require_ok(sieve::validate_artifact_cleanup_completion_dependency(merged_cleanup_authorization,
                                                                      merged_cleanup_completion),
               "merged cleanup completes");

    auto completed = fixture.completed;
    completed.wave_root_identity = manifest.wave_root_identity;
    completed.permanent_lock_identity = manifest.permanent_lock_identity;
    completed.manifest_digest = manifest.self_digest;
    completed.merge_commit_digest = merge_commit.self_digest;
    completed.consumption_ack_digest = consumption_ack.self_digest;
    completed.successor_prepared_digest = successor_prepared.self_digest;
    completed.chunks = merge_commit.chunks;
    completed.cleanup_confirmations = {
        {sieve::CleanupArtifactKindV1::worker, 0, worker_cleanup_authorization.self_digest,
         worker_cleanup_completion.self_digest},
        {sieve::CleanupArtifactKindV1::merged, 0, merged_cleanup_authorization.self_digest,
         merged_cleanup_completion.self_digest},
    };
    completed.successor_artifact = successor_prepared.successor_artifact;
    completed.successor_semantic_digest = successor_prepared.successor_semantic_digest;
    completed = reseal(std::move(completed));

    const std::array cleanup_authorizations = {
        worker_cleanup_authorization,
        merged_cleanup_authorization,
    };
    const std::array cleanup_completions = {
        worker_cleanup_completion,
        merged_cleanup_completion,
    };
    require_ok(sieve::validate_wave_completion_dependencies(
                   manifest, merge_commit, successor_prepared, consumption_ack, consumption_starts,
                   cleanup_authorizations, cleanup_completions, completed),
               "trailing empty chunk reaches WaveCompleted without cleanup authority");
}

class WaveStoreTempDirectory final {
public:
    WaveStoreTempDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("gnfs-distributed-wave-store-" + std::to_string(tick) + "-" +
                     std::to_string(sequence.fetch_add(1)) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
#ifndef _WIN32
                if (::chmod(path_.c_str(), 0700) != 0) {
                    const int saved_errno = errno;
                    std::filesystem::remove_all(path_, error);
                    throw std::system_error(saved_errno, std::generic_category(),
                                            "chmod wave-store temp directory");
                }
#endif
                const auto canonical_path = std::filesystem::canonical(path_, error);
                if (error) {
                    std::error_code ignored;
                    (void)std::filesystem::remove_all(path_, ignored);
                    throw std::filesystem::filesystem_error(
                        "canonicalize wave-store temp directory", path_, error);
                }
                path_ = canonical_path;
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("create wave-store temp directory", path_,
                                                        error);
            }
        }
        throw TestFailure("could not reserve a wave-store temporary directory");
    }

    WaveStoreTempDirectory(const WaveStoreTempDirectory&) = delete;
    WaveStoreTempDirectory& operator=(const WaveStoreTempDirectory&) = delete;

    ~WaveStoreTempDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path wave_lock_path(const std::filesystem::path& root) {
    return root / wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF;
}

[[nodiscard]] std::filesystem::path wave_manifest_path(const std::filesystem::path& root) {
    return root / wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF;
}

[[nodiscard]] std::filesystem::path wave_manifest_pending_path(const std::filesystem::path& root) {
    return root / wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF;
}

[[nodiscard]] bool entry_exists_no_follow(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return false;
    }
    if (error) {
        throw std::filesystem::filesystem_error("inspect wave-store leaf", path, error);
    }
    return status.type() != std::filesystem::file_type::not_found;
}

[[nodiscard]] std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw TestFailure("cannot open wave-store file for reading: " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw TestFailure("cannot size wave-store file: " + path.string());
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        throw TestFailure("cannot read complete wave-store file: " + path.string());
    }
    return bytes;
}

void write_file_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw TestFailure("cannot open wave-store file for writing: " + path.string());
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    if (!output) {
        throw TestFailure("cannot write complete wave-store file: " + path.string());
    }
}

void write_foreign_leaf(const std::filesystem::path& path) {
    constexpr std::array<std::byte, 7> bytes{
        std::byte{0x66}, std::byte{0x6f}, std::byte{0x72}, std::byte{0x65},
        std::byte{0x69}, std::byte{0x67}, std::byte{0x6e},
    };
    write_file_bytes(path, bytes);
#ifndef _WIN32
    if (::chmod(path.c_str(), 0600) != 0) {
        throw std::system_error(errno, std::generic_category(), "chmod foreign wave-store leaf");
    }
#endif
}

void write_empty_foreign_leaf(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw TestFailure("cannot create empty wave-store file: " + path.string());
    }
    output.close();
    if (!output) {
        throw TestFailure("cannot close empty wave-store file: " + path.string());
    }
#ifndef _WIN32
    if (::chmod(path.c_str(), 0600) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "chmod empty foreign wave-store leaf");
    }
#endif
}

[[nodiscard]] sieve::WaveManifestV1 wave_manifest_draft() {
    auto draft = ProtocolFixture{}.manifest;
    draft.wave_root_identity = {};
    draft.permanent_lock_identity = {};
    draft.lock_semantics_version = 0;
    draft.self_digest = {};
    return draft;
}

[[nodiscard]] std::string
wave_diagnostic_detail(const wave_detail::DistributedSieveWaveStoreDiagnostic& diagnostic) {
    std::string detail(wave_detail::distributed_sieve_wave_store_status_name(diagnostic.status));
    if (diagnostic.native_error) {
        detail.append(": ");
        detail.append(diagnostic.native_error.message());
    }
    return detail;
}

void require_wave_status(const wave_detail::DistributedSieveWaveStoreDiagnostic& diagnostic,
                         wave_detail::DistributedSieveWaveStoreStatus expected,
                         std::string_view context) {
    if (diagnostic.status != expected) {
        fail(context, __LINE__, wave_diagnostic_detail(diagnostic));
    }
}

[[nodiscard]] wave_detail::DistributedSieveWaveStore&
require_wave_ready(wave_detail::DistributedSieveWaveStoreOpenResult& result,
                   std::string_view context) {
    if (!result || result.store == nullptr) {
        fail(context, __LINE__, wave_diagnostic_detail(result.diagnostic));
    }
    return *result.store;
}

[[nodiscard]] Digest create_closed_wave(const std::filesystem::path& root) {
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    auto& store = require_wave_ready(created, "create closed wave fixture");
    const Digest digest = store.manifest_digest();
    created.store.reset();
    return digest;
}

[[nodiscard]] Digest manifest_digest_from_file(const std::filesystem::path& path) {
    const auto bytes = read_file_bytes(path);
    const auto decoded = sieve::decode_distributed_sieve_record(bytes);
    if (!decoded) {
        fail("decode wave manifest prefix", __LINE__,
             sieve::distributed_sieve_protocol_error_name(decoded.status.error));
    }
    const auto* manifest = std::get_if<sieve::WaveManifestV1>(&*decoded.value);
    if (manifest == nullptr) {
        fail("wave manifest prefix record kind", __LINE__);
    }
    return manifest->self_digest;
}

struct WaveFaultStopContext final {
    wave_detail::DistributedSieveWaveStoreFaultPoint target =
        wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable;
    std::array<bool, WAVE_STORE_FAULT_POINTS.size()> observed{};
};

[[nodiscard]] bool stop_at_wave_fault(wave_detail::DistributedSieveWaveStoreFaultPoint point,
                                      void* opaque) noexcept {
    auto& context = *static_cast<WaveFaultStopContext*>(opaque);
    const auto index = static_cast<std::size_t>(point);
    if (index < context.observed.size()) {
        context.observed[index] = true;
    }
    return point == context.target;
}

#if !defined(_WIN32)

struct WaveForkFaultContext final {
    wave_detail::DistributedSieveWaveStoreFaultPoint target =
        wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable;
    pid_t original_process = -1;
    pid_t child_process = -1;
    bool invoked = false;
};

[[nodiscard]] bool fork_at_wave_fault(wave_detail::DistributedSieveWaveStoreFaultPoint point,
                                      void* opaque) noexcept {
    auto& context = *static_cast<WaveForkFaultContext*>(opaque);
    if (point != context.target || context.invoked) {
        return false;
    }
    context.invoked = true;
    const pid_t child = ::fork();
    if (child == 0) {
        return false;
    }
    context.child_process = child;
    return true;
}

struct WaveUnknownFaultContext final {
    wave_detail::DistributedSieveWaveStoreFaultPoint target =
        wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable;
    std::filesystem::path unknown_leaf;
    bool inserted = false;
};

[[nodiscard]] bool
insert_unknown_at_wave_fault(wave_detail::DistributedSieveWaveStoreFaultPoint point,
                             void* opaque) noexcept {
    auto& context = *static_cast<WaveUnknownFaultContext*>(opaque);
    if (point != context.target || context.inserted) {
        return false;
    }
    const int descriptor =
        ::open(context.unknown_leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    constexpr char byte = 'x';
    ssize_t count = -1;
    do {
        count = ::write(descriptor, &byte, 1);
    } while (count < 0 && errno == EINTR);
    const int saved_errno = errno;
    (void)::close(descriptor);
    errno = saved_errno;
    context.inserted = count == 1;
    return false;
}

#endif

void check_wave_fault_prefix(const std::filesystem::path& root,
                             wave_detail::DistributedSieveWaveStoreFaultPoint point) {
    const bool root_present = entry_exists_no_follow(root);
    const bool lock_present = root_present && entry_exists_no_follow(wave_lock_path(root));
    const bool pending_present =
        root_present && entry_exists_no_follow(wave_manifest_pending_path(root));
    const bool canonical_present = root_present && entry_exists_no_follow(wave_manifest_path(root));

    CHECK(root_present);
    switch (point) {
    case wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable:
        CHECK(!lock_present);
        CHECK(!pending_present);
        CHECK(!canonical_present);
        return;
    case wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable:
        CHECK(lock_present);
        CHECK(!pending_present);
        CHECK(!canonical_present);
        return;
    case wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestPendingDurable:
        CHECK(lock_present);
        CHECK(pending_present);
        CHECK(!canonical_present);
        return;
    case wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestCanonicalPromoted:
    case wave_detail::DistributedSieveWaveStoreFaultPoint::ManifestCanonicalDurable:
        CHECK(lock_present);
        CHECK(!pending_present);
        CHECK(canonical_present);
        return;
    case wave_detail::DistributedSieveWaveStoreFaultPoint::Count:
        break;
    }
    fail("closed wave fault point", __LINE__);
}

void test_wave_store_create_open_revalidate_and_exact_manifest() {
    CHECK(wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_LEAF == ".gnfs-wave-v1.lock");
    CHECK(wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_LEAF == ".gnfs-wave-v1.manifest");
    CHECK(wave_detail::DISTRIBUTED_SIEVE_WAVE_MANIFEST_PENDING_LEAF ==
          ".gnfs-wave-v1.manifest.pending");

    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "create-open";
    const auto draft = wave_manifest_draft();
    CHECK(draft.wave_root_identity == sieve::NativeIdentityV1{});
    CHECK(draft.permanent_lock_identity == sieve::NativeIdentityV1{});
    CHECK(draft.lock_semantics_version == 0);
    CHECK(draft.self_digest == Digest{});

    auto created = wave_detail::DistributedSieveWaveStore::create(root, draft);
    auto& store = require_wave_ready(created, "create wave store");
    CHECK(store.absolute_root() == root);
    CHECK(store.manifest().wave_root_identity == store.wave_root_identity());
    CHECK(store.manifest().permanent_lock_identity == store.permanent_lock_identity());
    CHECK(store.manifest().lock_semantics_version ==
          wave_detail::DISTRIBUTED_SIEVE_WAVE_LOCK_SEMANTICS_VERSION_V1);
    CHECK(store.manifest().self_digest == store.manifest_digest());
    CHECK(store.manifest_digest() != Digest{});

    auto independently_sealed = store.manifest();
    independently_sealed.self_digest = {};
    independently_sealed = seal_value(std::move(independently_sealed));
    CHECK(encode_or_fail(Record{independently_sealed}) == encode_or_fail(Record{store.manifest()}));
    CHECK(record_digest_or_fail(Record{store.manifest()}) == store.manifest_digest());

    const auto exact_manifest_bytes = encode_or_fail(Record{store.manifest()});
    CHECK(read_file_bytes(wave_manifest_path(root)) == exact_manifest_bytes);
    CHECK(store.manifest_snapshot().size == exact_manifest_bytes.size());
    require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "revalidate newly created wave");

    const Digest digest = store.manifest_digest();
    const auto root_identity = store.wave_root_identity();
    const auto lock_identity = store.permanent_lock_identity();
    const auto manifest_snapshot = store.manifest_snapshot();
    created.store.reset();

    auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
    auto& reopened = require_wave_ready(opened, "open exact wave store");
    CHECK(reopened.manifest_digest() == digest);
    CHECK(reopened.wave_root_identity() == root_identity);
    CHECK(reopened.permanent_lock_identity() == lock_identity);
    CHECK(reopened.manifest_snapshot() == manifest_snapshot);
    CHECK(read_file_bytes(wave_manifest_path(root)) == exact_manifest_bytes);
    require_wave_status(reopened.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                        "revalidate reopened wave");
}

void test_wave_store_rejects_non_draft_store_owned_fields() {
    WaveStoreTempDirectory temp;
    std::uint64_t index = 0;
    const auto reject = [&](auto mutate) {
        auto draft = wave_manifest_draft();
        mutate(draft);
        const auto root = temp.path() / ("forged-draft-" + std::to_string(index++));
        auto result = wave_detail::DistributedSieveWaveStore::create(root, std::move(draft));
        CHECK(!result);
        CHECK(result.store == nullptr);
        require_wave_status(result.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                            "reject invalid wave manifest draft");
        CHECK(!entry_exists_no_follow(root));
    };

    reject([](auto& draft) { draft.wave_root_identity = native_identity(8000); });
    reject([](auto& draft) { draft.permanent_lock_identity = native_identity(8010); });
    reject([](auto& draft) { draft.lock_semantics_version = 1; });
    reject([](auto& draft) { draft.self_digest = digest_with_seed(55); });
    reject([](auto& draft) { draft.worker_count = 0; });
}

void test_wave_store_rejects_zero_open_digest_without_observation() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "zero-open-digest";
    auto opened = wave_detail::DistributedSieveWaveStore::open(root, Digest{});
    CHECK(!opened);
    CHECK(opened.store == nullptr);
    require_wave_status(opened.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                        "zero manifest digest is not open authority");
    CHECK(!entry_exists_no_follow(root));

#if !defined(_WIN32)
    const auto existing_root = temp.path() / "zero-open-digest-existing";
    (void)create_closed_wave(existing_root);
    const auto manifest_before = read_file_bytes(wave_manifest_path(existing_root));
    auto existing_opened = wave_detail::DistributedSieveWaveStore::open(existing_root, Digest{});
    CHECK(!existing_opened);
    CHECK(existing_opened.store == nullptr);
    require_wave_status(existing_opened.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                        "zero manifest digest does not inspect an existing wave");
    CHECK(read_file_bytes(wave_manifest_path(existing_root)) == manifest_before);
#endif
}

void test_wave_store_all_durable_prefixes_recover_exactly() {
    for (const auto point : WAVE_STORE_FAULT_POINTS) {
        WaveStoreTempDirectory temp;
        const auto root =
            temp.path() / ("fault-" + std::to_string(static_cast<std::size_t>(point)));
        WaveFaultStopContext context{.target = point};
        auto interrupted = wave_detail::DistributedSieveWaveStore::create(
            root, wave_manifest_draft(),
            wave_detail::DistributedSieveWaveStoreTestHooks{
                .stop_after = stop_at_wave_fault,
                .context = &context,
            });
        CHECK(!interrupted);
        CHECK(interrupted.store == nullptr);
        require_wave_status(interrupted.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::interrupted,
                            "fault point interrupts create");
        CHECK(interrupted.diagnostic.last_durable_fault_point == point);
        CHECK(context.observed[static_cast<std::size_t>(point)]);
        check_wave_fault_prefix(root, point);

        wave_detail::DistributedSieveWaveStoreOpenResult recovered =
            point == wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable ||
                    point == wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable
                ? wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft())
                : wave_detail::DistributedSieveWaveStore::open(
                      root, manifest_digest_from_file(
                                point == wave_detail::DistributedSieveWaveStoreFaultPoint::
                                             ManifestPendingDurable
                                    ? wave_manifest_pending_path(root)
                                    : wave_manifest_path(root)));
        auto& store = require_wave_ready(recovered, "recover durable create prefix");
        CHECK(entry_exists_no_follow(wave_lock_path(root)));
        CHECK(entry_exists_no_follow(wave_manifest_path(root)));
        CHECK(!entry_exists_no_follow(wave_manifest_pending_path(root)));
        const auto exact_bytes = encode_or_fail(Record{store.manifest()});
        CHECK(read_file_bytes(wave_manifest_path(root)) == exact_bytes);
        const Digest digest = store.manifest_digest();
        recovered.store.reset();

        auto reopened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        auto& exact = require_wave_ready(reopened, "open recovered durable prefix");
        CHECK(exact.manifest_digest() == digest);
        require_wave_status(exact.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "revalidate recovered durable prefix");
    }
}

#if !defined(_WIN32)

void test_wave_store_revalidates_after_root_and_lock_hooks() {
    const std::array points{
        wave_detail::DistributedSieveWaveStoreFaultPoint::RootDurable,
        wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable,
    };
    for (const auto point : points) {
        WaveStoreTempDirectory temp;
        const auto root =
            temp.path() / ("hook-drift-" + std::to_string(static_cast<std::size_t>(point)));
        const auto unknown = root / "unexpected.control";
        WaveUnknownFaultContext context{
            .target = point,
            .unknown_leaf = unknown,
        };
        auto result = wave_detail::DistributedSieveWaveStore::create(
            root, wave_manifest_draft(),
            wave_detail::DistributedSieveWaveStoreTestHooks{
                .stop_after = insert_unknown_at_wave_fault,
                .context = &context,
            });
        CHECK(context.inserted);
        CHECK(!result);
        CHECK(result.store == nullptr);
        require_wave_status(result.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "hook namespace drift blocks the next mutation");
        CHECK(entry_exists_no_follow(unknown));
        CHECK(!entry_exists_no_follow(wave_manifest_pending_path(root)));
        CHECK(!entry_exists_no_follow(wave_manifest_path(root)));
        CHECK(entry_exists_no_follow(wave_lock_path(root)) ==
              (point == wave_detail::DistributedSieveWaveStoreFaultPoint::LockDurable));
    }
}

#endif

void test_wave_store_unknown_leaf_and_wrong_digest_are_preserved() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "unknown-leaf";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create unknown-leaf fixture");
        const Digest digest = store.manifest_digest();
        const auto manifest_before = read_file_bytes(wave_manifest_path(root));
        const auto unknown = root / "unexpected.control";
        write_foreign_leaf(unknown);
        const auto unknown_before = read_file_bytes(unknown);

        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "unknown leaf taints live wave");
        CHECK(read_file_bytes(wave_manifest_path(root)) == manifest_before);
        CHECK(read_file_bytes(unknown) == unknown_before);
        created.store.reset();

        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::namespace_conflict,
                            "unknown leaf taints reopened wave");
        CHECK(read_file_bytes(wave_manifest_path(root)) == manifest_before);
        CHECK(read_file_bytes(unknown) == unknown_before);
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "wrong-expected-digest";
        const Digest digest = create_closed_wave(root);
        const auto manifest_before = read_file_bytes(wave_manifest_path(root));
        auto wrong_digest = digest;
        perturb_digest(wrong_digest);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, wrong_digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_conflict,
                            "wrong expected manifest digest");
        CHECK(read_file_bytes(wave_manifest_path(root)) == manifest_before);
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "wrong-on-disk-digest";
        const Digest digest = create_closed_wave(root);
        auto corrupted = read_file_bytes(wave_manifest_path(root));
        CHECK(!corrupted.empty());
        corrupted.back() ^= std::byte{0x40};
        write_file_bytes(wave_manifest_path(root), corrupted);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "wrong on-disk manifest digest");
        CHECK(read_file_bytes(wave_manifest_path(root)) == corrupted);
    }
}

#if !defined(_WIN32)

void require_chmod(const std::filesystem::path& path, mode_t mode, std::string_view context) {
    if (::chmod(path.c_str(), mode) != 0) {
        throw std::system_error(errno, std::generic_category(), std::string(context));
    }
}

void require_rename(const std::filesystem::path& source, const std::filesystem::path& target,
                    std::string_view context) {
    std::error_code error;
    std::filesystem::rename(source, target, error);
    if (error) {
        throw std::filesystem::filesystem_error(std::string(context), source, target, error);
    }
}

#if defined(__APPLE__)

void install_wave_extended_read_acl(int descriptor) {
    acl_t acl = ::acl_init(1);
    if (acl == nullptr) {
        throw TestFailure("cannot allocate wave-store test ACL");
    }

    acl_entry_t entry = nullptr;
    acl_permset_t permissions = nullptr;
    uuid_t owner_uuid{};
    const int membership_error = ::mbr_uid_to_uuid(::geteuid(), owner_uuid);
    const bool configured = membership_error == 0 && ::acl_create_entry(&acl, &entry) == 0 &&
                            ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 &&
                            ::acl_set_qualifier(entry, owner_uuid) == 0 &&
                            ::acl_get_permset(entry, &permissions) == 0 &&
                            ::acl_clear_perms(permissions) == 0 &&
                            ::acl_add_perm(permissions, ACL_READ_DATA) == 0 &&
                            ::acl_set_permset(entry, permissions) == 0 && ::acl_valid(acl) == 0 &&
                            ::acl_set_fd_np(descriptor, acl, ACL_TYPE_EXTENDED) == 0;
    const int saved_errno = errno;
    (void)::acl_free(acl);
    if (!configured) {
        throw TestFailure("cannot install wave-store test ACL: " +
                          std::error_code(membership_error != 0 ? membership_error : saved_errno,
                                          std::generic_category())
                              .message());
    }
}

void install_wave_extended_read_acl(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "open wave-store ACL test target");
    }
    try {
        install_wave_extended_read_acl(descriptor);
    } catch (...) {
        (void)::close(descriptor);
        throw;
    }
    if (::close(descriptor) != 0) {
        throw std::system_error(errno, std::generic_category(), "close wave-store ACL test target");
    }
}

#elif defined(__linux__)

void install_wave_extended_read_acl(const std::filesystem::path& path) {
    constexpr std::uint16_t ACL_USER_OBJ = 0x01;
    constexpr std::uint16_t ACL_USER = 0x02;
    constexpr std::uint16_t ACL_GROUP_OBJ = 0x04;
    constexpr std::uint16_t ACL_MASK = 0x10;
    constexpr std::uint16_t ACL_OTHER = 0x20;
    constexpr std::uint32_t ACL_UNDEFINED_ID = 0xffffffffU;
    constexpr std::size_t HEADER_BYTES = 4;
    constexpr std::size_t ENTRY_BYTES = 8;
    std::array<std::byte, HEADER_BYTES + 5 * ENTRY_BYTES> acl{};
    struct stat metadata{};
    if (::lstat(path.c_str(), &metadata) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "inspect wave-store Linux ACL target");
    }
    const auto owner_permissions = static_cast<std::uint16_t>((metadata.st_mode >> 6U) & 07U);
    const auto group_permissions = static_cast<std::uint16_t>((metadata.st_mode >> 3U) & 07U);
    const auto other_permissions = static_cast<std::uint16_t>(metadata.st_mode & 07U);

    const auto put_u16_le = [&](std::size_t offset, std::uint16_t value) {
        acl[offset] = static_cast<std::byte>(value & 0xffU);
        acl[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    };
    const auto put_u32_le = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0; index < 4; ++index) {
            acl[offset + index] =
                static_cast<std::byte>((value >> static_cast<unsigned>(index * 8U)) & 0xffU);
        }
    };
    const auto put_entry = [&](std::size_t index, std::uint16_t tag, std::uint16_t permissions,
                               std::uint32_t id) {
        const std::size_t offset = HEADER_BYTES + index * ENTRY_BYTES;
        put_u16_le(offset, tag);
        put_u16_le(offset + 2, permissions);
        put_u32_le(offset + 4, id);
    };

    put_u32_le(0, 2);
    put_entry(0, ACL_USER_OBJ, owner_permissions, ACL_UNDEFINED_ID);
    put_entry(1, ACL_USER, 0, static_cast<std::uint32_t>(::geteuid()) ^ 1U);
    put_entry(2, ACL_GROUP_OBJ, group_permissions, ACL_UNDEFINED_ID);
    put_entry(3, ACL_MASK, group_permissions, ACL_UNDEFINED_ID);
    put_entry(4, ACL_OTHER, other_permissions, ACL_UNDEFINED_ID);
    if (::setxattr(path.c_str(), "system.posix_acl_access", acl.data(), acl.size(), 0) != 0) {
        throw std::system_error(errno, std::generic_category(), "install wave-store Linux ACL");
    }
}

#endif

void test_wave_store_rejects_noncanonical_and_intermediate_symlink_paths() {
    WaveStoreTempDirectory temp;
    const std::string base = temp.path().string();
    const std::array malformed_roots{
        std::filesystem::path(base + "//double-separator"),
        std::filesystem::path(base + "/./dot-component"),
        std::filesystem::path(base + "/missing/../dot-dot-component"),
        std::filesystem::path(base + "/trailing-separator/"),
    };
    for (const auto& root : malformed_roots) {
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        CHECK(!created);
        CHECK(created.store == nullptr);
        require_wave_status(created.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::invalid_request,
                            "reject noncanonical absolute wave path");
    }
    CHECK(!entry_exists_no_follow(temp.path() / "double-separator"));
    CHECK(!entry_exists_no_follow(temp.path() / "dot-component"));
    CHECK(!entry_exists_no_follow(temp.path() / "dot-dot-component"));
    CHECK(!entry_exists_no_follow(temp.path() / "trailing-separator"));

    const auto unsafe_parent = temp.path() / "unsafe-parent";
    std::error_code error;
    CHECK(std::filesystem::create_directory(unsafe_parent, error));
    CHECK(!error);
    require_chmod(unsafe_parent, 0777, "weaken wave parent mode");
    auto unsafe_create = wave_detail::DistributedSieveWaveStore::create(unsafe_parent / "wave",
                                                                        wave_manifest_draft());
    CHECK(!unsafe_create);
    require_wave_status(unsafe_create.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "reject writable direct parent before mutation");
    CHECK(!entry_exists_no_follow(unsafe_parent / "wave"));
    require_chmod(unsafe_parent, 0700, "restore wave parent mode");

    const auto real_parent = temp.path() / "real-parent";
    error.clear();
    CHECK(std::filesystem::create_directory(real_parent, error));
    CHECK(!error);
    require_chmod(real_parent, 0700, "chmod real wave parent");
    const auto alias = temp.path() / "parent-alias";
    CHECK(::symlink(real_parent.c_str(), alias.c_str()) == 0);

    auto aliased_create =
        wave_detail::DistributedSieveWaveStore::create(alias / "new-wave", wave_manifest_draft());
    CHECK(!aliased_create);
    require_wave_status(aliased_create.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "reject intermediate parent symlink on create");
    CHECK(!entry_exists_no_follow(real_parent / "new-wave"));

    const auto real_root = real_parent / "existing-wave";
    const Digest digest = create_closed_wave(real_root);
    const auto manifest_before = read_file_bytes(wave_manifest_path(real_root));
    auto aliased_open =
        wave_detail::DistributedSieveWaveStore::open(alias / "existing-wave", digest);
    CHECK(!aliased_open);
    require_wave_status(aliased_open.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "reject intermediate parent symlink on open");
    CHECK(read_file_bytes(wave_manifest_path(real_root)) == manifest_before);

    const auto stable_ancestor = temp.path() / "stable-ancestor";
    const auto held_parent = stable_ancestor / "held-parent";
    CHECK(std::filesystem::create_directories(held_parent, error));
    CHECK(!error);
    require_chmod(stable_ancestor, 0700, "chmod stable wave ancestor");
    require_chmod(held_parent, 0700, "chmod held wave parent");
    const auto held_root = held_parent / "held-wave";
    auto held = wave_detail::DistributedSieveWaveStore::create(held_root, wave_manifest_draft());
    auto& held_store = require_wave_ready(held, "create ancestor-replacement fixture");
    const auto held_manifest_before = read_file_bytes(wave_manifest_path(held_root));
    const auto original_ancestor = temp.path() / "stable-ancestor-original";
    require_rename(stable_ancestor, original_ancestor, "move held wave ancestor");
    CHECK(::symlink(original_ancestor.c_str(), stable_ancestor.c_str()) == 0);

    require_wave_status(held_store.revalidate(),
                        wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                        "reject intermediate ancestor symlink after acquisition");
    CHECK(read_file_bytes(wave_manifest_path(held_root)) == held_manifest_before);
}

void test_wave_store_root_and_lock_replacement() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "root-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create root-replacement fixture");
        const auto original_root = temp.path() / "root-replacement-original";
        const auto manifest_before = read_file_bytes(wave_manifest_path(root));
        require_rename(root, original_root, "move original wave root");
        std::error_code error;
        CHECK(std::filesystem::create_directory(root, error));
        CHECK(!error);
        require_chmod(root, 0700, "chmod replacement wave root");

        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "revalidate replaced wave root");
        CHECK(std::filesystem::is_empty(root));
        CHECK(read_file_bytes(wave_manifest_path(original_root)) == manifest_before);
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create lock-replacement fixture");
        const auto original_lock = temp.path() / "original-wave-lock";
        require_rename(wave_lock_path(root), original_lock, "move original wave lock");
        write_foreign_leaf(wave_lock_path(root));
        const auto replacement_before = read_file_bytes(wave_lock_path(root));

        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "revalidate replaced wave lock");
        CHECK(read_file_bytes(wave_lock_path(root)) == replacement_before);
        CHECK(entry_exists_no_follow(original_lock));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "live-lock-replacement";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create live-lock-replacement fixture");
        const Digest digest = store.manifest_digest();
        const auto manifest_before = read_file_bytes(wave_manifest_path(root));
        const auto original_lock = temp.path() / "live-original-wave-lock";
        require_rename(wave_lock_path(root), original_lock, "move live original wave lock");
        write_empty_foreign_leaf(wave_lock_path(root));

        auto replacement_opener = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!replacement_opener);
        require_wave_status(replacement_opener.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_conflict,
                            "replacement lock cannot satisfy manifest identity");
        CHECK(read_file_bytes(wave_manifest_path(root)) == manifest_before);
        CHECK(read_file_bytes(wave_lock_path(root)).empty());
        CHECK(!entry_exists_no_follow(wave_manifest_pending_path(root)));
        CHECK(entry_exists_no_follow(original_lock));
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "old owner rejects replaced named lock");
    }
}

void test_wave_store_mode_symlink_and_hardlink_rejections() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "root-mode";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create root-mode fixture");
        require_chmod(root, 0755, "weaken wave root mode");
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "reject weakened wave root mode");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-mode";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create lock-mode fixture");
        require_chmod(wave_lock_path(root), 0644, "weaken wave lock mode");
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "reject weakened wave lock mode");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "manifest-mode";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create manifest-mode fixture");
        require_chmod(wave_manifest_path(root), 0644, "weaken manifest mode");
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "reject weakened manifest mode");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "root-symlink";
        const Digest digest = create_closed_wave(root);
        const auto original_root = temp.path() / "root-symlink-original";
        require_rename(root, original_root, "move root before symlink replacement");
        CHECK(::symlink(original_root.c_str(), root.c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "reject wave root symlink");
        CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(root)));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-symlink";
        const Digest digest = create_closed_wave(root);
        const auto original_lock = temp.path() / "lock-symlink-original";
        require_rename(wave_lock_path(root), original_lock, "move lock before symlink replacement");
        CHECK(::symlink(original_lock.c_str(), wave_lock_path(root).c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "reject wave lock symlink");
        CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(wave_lock_path(root))));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "manifest-symlink";
        const Digest digest = create_closed_wave(root);
        const auto original_manifest = temp.path() / "manifest-symlink-original";
        require_rename(wave_manifest_path(root), original_manifest,
                       "move manifest before symlink replacement");
        CHECK(::symlink(original_manifest.c_str(), wave_manifest_path(root).c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "reject manifest symlink");
        CHECK(
            std::filesystem::is_symlink(std::filesystem::symlink_status(wave_manifest_path(root))));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-hardlink";
        const Digest digest = create_closed_wave(root);
        const auto original_lock = temp.path() / "lock-hardlink-original";
        require_rename(wave_lock_path(root), original_lock,
                       "move lock before hardlink replacement");
        CHECK(::link(original_lock.c_str(), wave_lock_path(root).c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "reject wave lock hardlink");
        CHECK(entry_exists_no_follow(original_lock));
        CHECK(entry_exists_no_follow(wave_lock_path(root)));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "manifest-hardlink";
        const Digest digest = create_closed_wave(root);
        const auto original_manifest = temp.path() / "manifest-hardlink-original";
        require_rename(wave_manifest_path(root), original_manifest,
                       "move manifest before hardlink replacement");
        CHECK(::link(original_manifest.c_str(), wave_manifest_path(root).c_str()) == 0);
        auto opened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        CHECK(!opened);
        require_wave_status(opened.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "reject manifest hardlink");
        CHECK(entry_exists_no_follow(original_manifest));
        CHECK(entry_exists_no_follow(wave_manifest_path(root)));
    }

#if defined(__APPLE__) || defined(__linux__)
    {
        WaveStoreTempDirectory temp;
        install_wave_extended_read_acl(temp.path());
        const auto root = temp.path() / "parent-acl";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        CHECK(!created);
        require_wave_status(created.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "reject extended parent ACL before mutation");
        CHECK(!entry_exists_no_follow(root));
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "root-acl";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create root-ACL fixture");
        install_wave_extended_read_acl(root);
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::root_invalid,
                            "reject extended root ACL");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "lock-acl";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create lock-ACL fixture");
        install_wave_extended_read_acl(wave_lock_path(root));
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::lock_invalid,
                            "reject extended lock ACL");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "manifest-acl";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create manifest-ACL fixture");
        install_wave_extended_read_acl(wave_manifest_path(root));
        require_wave_status(store.revalidate(),
                            wave_detail::DistributedSieveWaveStoreStatus::manifest_invalid,
                            "reject extended manifest ACL");
    }
#endif
}

[[nodiscard]] bool write_pipe_byte(int descriptor, char value) noexcept {
    ssize_t count = -1;
    do {
        count = ::write(descriptor, &value, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1;
}

[[nodiscard]] bool read_pipe_byte(int descriptor, char& value) noexcept {
    ssize_t count = -1;
    do {
        count = ::read(descriptor, &value, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1;
}

[[nodiscard]] bool wait_for_child(pid_t child, int& status) noexcept {
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child;
}

void test_wave_store_fault_hooks_cannot_fork_store_authority() {
    for (const auto point : WAVE_STORE_FAULT_POINTS) {
        WaveStoreTempDirectory temp;
        const auto root =
            temp.path() / ("fork-fault-" + std::to_string(static_cast<std::size_t>(point)));
        WaveForkFaultContext context{
            .target = point,
            .original_process = ::getpid(),
        };
        auto result = wave_detail::DistributedSieveWaveStore::create(
            root, wave_manifest_draft(),
            wave_detail::DistributedSieveWaveStoreTestHooks{
                .stop_after = fork_at_wave_fault,
                .context = &context,
            });

        if (::getpid() != context.original_process) {
            const bool rejected = !result && result.store == nullptr &&
                                  result.diagnostic.status ==
                                      wave_detail::DistributedSieveWaveStoreStatus::invalid_request;
            ::_exit(rejected ? 0 : 91);
        }

        CHECK(context.invoked);
        CHECK(context.child_process > 0);
        CHECK(!result);
        CHECK(result.store == nullptr);
        require_wave_status(result.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::interrupted,
                            "parent interrupts after forking inside fault hook");

        int child_status = 0;
        CHECK(wait_for_child(context.child_process, child_status));
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);
        check_wave_fault_prefix(root, point);
    }
}

void test_wave_store_deterministic_busy_with_fork_and_pipes() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "busy";
    const Digest digest = create_closed_wave(root);
    int ready_pipe[2]{-1, -1};
    int release_pipe[2]{-1, -1};
    CHECK(::pipe(ready_pipe) == 0);
    CHECK(::pipe(release_pipe) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        auto held = wave_detail::DistributedSieveWaveStore::open(root, digest);
        const char ready = held ? 'r' : 'f';
        const bool signalled = write_pipe_byte(ready_pipe[1], ready);
        char release = '\0';
        const bool released = read_pipe_byte(release_pipe[0], release);
        held.store.reset();
        ::_exit(signalled && released && release == 'x' && ready == 'r' ? 0 : 81);
    }

    (void)::close(ready_pipe[1]);
    (void)::close(release_pipe[0]);
    char ready = '\0';
    const bool received = read_pipe_byte(ready_pipe[0], ready);
    auto busy = wave_detail::DistributedSieveWaveStore::open(root, digest);
    const bool released = write_pipe_byte(release_pipe[1], 'x');
    (void)::close(ready_pipe[0]);
    (void)::close(release_pipe[1]);
    int child_status = 0;
    const bool waited = wait_for_child(child, child_status);

    CHECK(received);
    CHECK(ready == 'r');
    CHECK(!busy);
    require_wave_status(busy.diagnostic, wave_detail::DistributedSieveWaveStoreStatus::lock_busy,
                        "concurrent wave opener is busy");
    CHECK(released);
    CHECK(waited);
    CHECK(WIFEXITED(child_status));
    CHECK(WEXITSTATUS(child_status) == 0);

    auto reopened = wave_detail::DistributedSieveWaveStore::open(root, digest);
    (void)require_wave_ready(reopened, "wave opens after lock owner exits");
}

void test_wave_store_inherited_lock_is_process_bound_and_close_only() {
    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "child-close-only";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create child-close-only fixture");
        const Digest digest = store.manifest_digest();
        int ready_pipe[2]{-1, -1};
        int release_pipe[2]{-1, -1};
        CHECK(::pipe(ready_pipe) == 0);
        CHECK(::pipe(release_pipe) == 0);
        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            (void)::close(ready_pipe[0]);
            (void)::close(release_pipe[1]);
            const auto inherited = store.revalidate();
            const bool process_bound =
                inherited.status == wave_detail::DistributedSieveWaveStoreStatus::invalid_request;
            created.store.reset();
            const bool signalled = write_pipe_byte(ready_pipe[1], process_bound ? 'r' : 'f');
            char release = '\0';
            const bool released = read_pipe_byte(release_pipe[0], release);
            ::_exit(process_bound && signalled && released && release == 'x' ? 0 : 82);
        }

        (void)::close(ready_pipe[1]);
        (void)::close(release_pipe[0]);
        char ready = '\0';
        const bool received = read_pipe_byte(ready_pipe[0], ready);
        auto busy = wave_detail::DistributedSieveWaveStore::open(root, digest);
        const bool released = write_pipe_byte(release_pipe[1], 'x');
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        int child_status = 0;
        const bool waited = wait_for_child(child, child_status);

        CHECK(received);
        CHECK(ready == 'r');
        CHECK(!busy);
        require_wave_status(busy.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_busy,
                            "fork copy close cannot unlock parent wave");
        CHECK(released);
        CHECK(waited);
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);
        require_wave_status(store.revalidate(), wave_detail::DistributedSieveWaveStoreStatus::ready,
                            "parent wave remains valid after child close");
    }

    {
        WaveStoreTempDirectory temp;
        const auto root = temp.path() / "child-retains-inherited-lock";
        auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
        auto& store = require_wave_ready(created, "create inherited-lock fixture");
        const Digest digest = store.manifest_digest();
        int ready_pipe[2]{-1, -1};
        int release_pipe[2]{-1, -1};
        CHECK(::pipe(ready_pipe) == 0);
        CHECK(::pipe(release_pipe) == 0);
        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            (void)::close(ready_pipe[0]);
            (void)::close(release_pipe[1]);
            const auto inherited = store.revalidate();
            const bool process_bound =
                inherited.status == wave_detail::DistributedSieveWaveStoreStatus::invalid_request;
            const bool signalled = write_pipe_byte(ready_pipe[1], process_bound ? 'r' : 'f');
            char release = '\0';
            const bool released = read_pipe_byte(release_pipe[0], release);
            created.store.reset();
            ::_exit(process_bound && signalled && released && release == 'x' ? 0 : 83);
        }

        (void)::close(ready_pipe[1]);
        (void)::close(release_pipe[0]);
        char ready = '\0';
        const bool received = read_pipe_byte(ready_pipe[0], ready);
        created.store.reset();
        auto busy = wave_detail::DistributedSieveWaveStore::open(root, digest);
        const bool released = write_pipe_byte(release_pipe[1], 'x');
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        int child_status = 0;
        const bool waited = wait_for_child(child, child_status);

        CHECK(received);
        CHECK(ready == 'r');
        CHECK(!busy);
        require_wave_status(busy.diagnostic,
                            wave_detail::DistributedSieveWaveStoreStatus::lock_busy,
                            "inherited child descriptor retains wave lock");
        CHECK(released);
        CHECK(waited);
        CHECK(WIFEXITED(child_status));
        CHECK(WEXITSTATUS(child_status) == 0);

        auto reopened = wave_detail::DistributedSieveWaveStore::open(root, digest);
        (void)require_wave_ready(reopened, "wave opens after inherited descriptor closes");
    }
}

#else

void test_wave_store_platform_fail_closed() {
    WaveStoreTempDirectory temp;
    const auto root = temp.path() / "unsupported";
    auto created = wave_detail::DistributedSieveWaveStore::create(root, wave_manifest_draft());
    CHECK(!created);
    CHECK(created.store == nullptr);
    require_wave_status(created.diagnostic,
                        wave_detail::DistributedSieveWaveStoreStatus::platform_unsupported,
                        "unsupported platform rejects wave-store creation");
    CHECK(!entry_exists_no_follow(root));
}

#endif

using TestFunction = void (*)();

void run_core_suite() {
    const std::array<std::pair<std::string_view, TestFunction>, 13> tests = {{
        {"closed names and kinds", test_closed_names_and_record_kinds},
        {"record round trips and self digests", test_all_record_round_trips_and_self_digests},
        {"exact framing and tamper", test_exact_framing_and_wire_tamper},
        {"manifest canonical order and limits", test_manifest_canonical_order_and_limits},
        {"worker attempt naming contract", test_worker_attempt_naming_contract},
        {"worker completion reason closure", test_worker_completion_reason_closure},
        {"terminal failure normalization", test_terminal_failure_reason_normalization},
        {"execution-policy field drift", test_execution_policy_closed_inventory_and_field_drift},
        {"work-identity field drift", test_work_identity_field_drift_and_canonical_integers},
        {"manifest/work identity binding", test_manifest_work_identity_binding},
        {"deterministic seed drift", test_deterministic_seed_domain_and_input_drift},
        {"predecessor and dependency closure", test_predecessor_and_dependency_closure},
        {"empty chunk projection and merge", test_empty_chunk_projection_and_merge},
    }};

    std::cout << "===== Distributed Sieve Resume Core Tests =====\n";
    for (const auto& [name, function] : tests) {
        function();
        std::cout << "  " << name << ": PASS\n";
    }
    std::cout << "===== Distributed Sieve Resume Core Tests PASSED =====\n";
}

void run_wave_store_suite() {
#if !defined(_WIN32)
    const std::array<std::pair<std::string_view, TestFunction>, 12> tests = {{
        {"create, open, revalidate, and exact manifest",
         test_wave_store_create_open_revalidate_and_exact_manifest},
        {"store-owned draft fields", test_wave_store_rejects_non_draft_store_owned_fields},
        {"zero open digest", test_wave_store_rejects_zero_open_digest_without_observation},
        {"canonical no-follow paths",
         test_wave_store_rejects_noncanonical_and_intermediate_symlink_paths},
        {"all durable fault prefixes", test_wave_store_all_durable_prefixes_recover_exactly},
        {"post-hook namespace revalidation", test_wave_store_revalidates_after_root_and_lock_hooks},
        {"fault-hook fork authority", test_wave_store_fault_hooks_cannot_fork_store_authority},
        {"unknown leaf and wrong digest",
         test_wave_store_unknown_leaf_and_wrong_digest_are_preserved},
        {"root and lock replacement", test_wave_store_root_and_lock_replacement},
        {"mode, symlink, and hardlink", test_wave_store_mode_symlink_and_hardlink_rejections},
        {"deterministic busy", test_wave_store_deterministic_busy_with_fork_and_pipes},
        {"inherited lock", test_wave_store_inherited_lock_is_process_bound_and_close_only},
    }};
#else
    const std::array<std::pair<std::string_view, TestFunction>, 2> tests = {{
        {"zero open digest", test_wave_store_rejects_zero_open_digest_without_observation},
        {"unsupported platform fails closed", test_wave_store_platform_fail_closed},
    }};
#endif

    std::cout << "===== Distributed Sieve Wave Store Tests =====\n";
    for (const auto& [name, function] : tests) {
        function();
        std::cout << "  " << name << ": PASS\n";
    }
    std::cout << "===== Distributed Sieve Wave Store Tests PASSED =====\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            run_core_suite();
            run_wave_store_suite();
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--suite" &&
            std::string_view(argv[2]) == "core") {
            run_core_suite();
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--suite" &&
            std::string_view(argv[2]) == "wave-store") {
            run_wave_store_suite();
            return 0;
        }

        std::cerr << "usage: " << argv[0] << " [--suite core|wave-store]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Distributed sieve resume test failure: " << error.what() << '\n';
        return 1;
    }
}
