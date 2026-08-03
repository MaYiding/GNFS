#include "distributed_sieve_worker_cleanup_codec_internal.hpp"

#include <gnfs/relation/ooc_relation_format.hpp>
#include <gnfs/util/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace codec = gnfs::sieve::distributed_sieve_worker_cleanup_codec_detail;
namespace cleanup = gnfs::relation::ooc_cleanup_detail;
namespace durable = gnfs::util::durable_immutable_record;
namespace relation = gnfs::relation;
namespace resume = gnfs::sieve::distributed_sieve_resume_detail;
namespace sieve = gnfs::sieve;

using AuthorizationResult = codec::DistributedSieveWorkerCleanupAuthorizationBuildResultV1;
using BindingResult = codec::DistributedSieveWorkerCleanupRelationBindingBuildResultV1;
using BuildPhase = codec::DistributedSieveWorkerCleanupCodecPhaseV1;
using BuildStatus = codec::DistributedSieveWorkerCleanupCodecStatusV1;
using CompletionResult = codec::DistributedSieveWorkerCleanupCompletionBuildResultV1;
using Digest = gnfs::util::Sha256Digest;
using ProjectionResult = codec::DistributedSieveWorkerCleanupHandoffProjectionBuildResultV1;
using Record = sieve::DistributedSieveProtocolRecordV1;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error(std::string("CHECK failed: " #condition " at ") + __FILE__ +  \
                                     ":" + std::to_string(__LINE__));                              \
        }                                                                                          \
    } while (false)

[[nodiscard]] Digest digest_with_seed(std::uint8_t seed) noexcept {
    Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index + 1U));
    }
    return digest;
}

void perturb_digest(Digest& digest) noexcept {
    digest.bytes.front() ^= std::byte{1};
}

[[nodiscard]] sieve::WaveIdV1 wave_id_with_seed(std::uint8_t seed) noexcept {
    sieve::WaveIdV1 wave_id;
    for (std::size_t index = 0; index < wave_id.bytes.size(); ++index) {
        wave_id.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index + 1U));
    }
    return wave_id;
}

[[nodiscard]] constexpr sieve::NativeIdentityV1 native_identity(std::uint64_t seed) noexcept {
    return {
        .volume = seed,
        .object = seed + 1U,
        .generation = seed + 2U,
    };
}

[[nodiscard]] constexpr durable::NativeIdentity
relation_identity(const sieve::NativeIdentityV1& identity) noexcept {
    return {
        .first = identity.volume,
        .second = identity.object,
        .third = identity.generation,
    };
}

[[nodiscard]] constexpr relation::OOCPrivateHandoffArtifactBindingV1
relation_artifact(const sieve::NativeFileExtentV1& artifact) noexcept {
    return {
        .identity = relation_identity(artifact.identity),
        .extent = artifact.extent,
    };
}

[[nodiscard]] constexpr relation::OOCPrivateHandoffPairDescriptorV1
relation_pair(const sieve::CorpusArtifactV1& artifact) noexcept {
    return {
        .format_version = artifact.descriptor.format_version,
        .store_id = artifact.descriptor.store_id,
        .generation = artifact.descriptor.generation,
        .count = artifact.descriptor.relation_count,
        .index_extent = artifact.index_file.extent,
        .data_extent = artifact.data_file.extent,
    };
}

[[nodiscard]] constexpr sieve::LeaseIdV1 lease_id(std::uint64_t seed) noexcept {
    return {{seed + 1U, seed + 2U}};
}

[[nodiscard]] sieve::LeaseIdentityV1 lease_identity(std::uint64_t seed, std::string stem) {
    return {
        .lease_id = lease_id(seed),
        .owner_marker = native_identity(seed + 10U),
        .directory = native_identity(seed + 20U),
        .relative_stem = std::move(stem),
    };
}

template <typename Value> [[nodiscard]] Value seal_value(Value value) {
    Record record(std::move(value));
    const auto status = sieve::seal_distributed_sieve_record(record);
    CHECK(status);
    return std::get<Value>(std::move(record));
}

template <typename Value> [[nodiscard]] Value reseal(Value value) {
    value.self_digest = {};
    return seal_value(std::move(value));
}

[[nodiscard]] std::vector<std::byte> encode_record(const Record& record) {
    const auto encoded = sieve::encode_distributed_sieve_record(record);
    CHECK(encoded);
    CHECK(encoded.bytes.has_value());
    return *encoded.bytes;
}

[[nodiscard]] relation::OOCPrivateHandoffRecordV1
reseal_private_handoff(relation::OOCPrivateHandoffRecordV1 record) {
    record.payload_digest = {};
    record.self_digest = {};
    CHECK(relation::seal_ooc_private_handoff_record(record));
    return record;
}

[[nodiscard]] std::vector<std::byte>
encode_private_handoff(const relation::OOCPrivateHandoffRecordV1& record) {
    const auto encoded = relation::encode_ooc_private_handoff_record(record);
    CHECK(encoded);
    CHECK(encoded.bytes.has_value());
    return *encoded.bytes;
}

[[nodiscard]] sieve::CorpusArtifactV1 make_artifact(std::uint64_t seed,
                                                    std::uint64_t relation_count) noexcept {
    constexpr std::uint64_t OFFSET_BYTES = sizeof(std::uint64_t);
    const std::uint64_t index_extent = relation::OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                                       relation::OOCRelationStoreFormat::INDEX_SENTINEL_BYTES +
                                       relation_count * OFFSET_BYTES;
    const std::uint64_t data_extent =
        relation::OOCRelationStoreFormat::DATA_HEADER_BYTES + (relation_count == 0 ? 0U : 64U);
    return {
        .descriptor =
            {
                .format_version = relation::OOCRelationStoreFormat::FORMAT_VERSION_V3,
                .store_id = seed + 100U,
                .generation = seed + 200U,
                .relation_count = relation_count,
                .data_end = data_extent,
            },
        .index_file =
            {
                .identity = native_identity(seed + 10U),
                .extent = index_extent,
            },
        .data_file =
            {
                .identity = native_identity(seed + 20U),
                .extent = data_extent,
            },
        .sequence_receipt =
            {
                .relation_count = relation_count,
                .low = seed + 300U,
                .high = seed + 400U,
            },
        .corpus_sha256 = digest_with_seed(static_cast<std::uint8_t>(seed)),
    };
}

[[nodiscard]] std::filesystem::path absolute_test_root() {
#if defined(_WIN32)
    return std::filesystem::path{L"C:\\gnfs-worker-cleanup-codec-root"};
#else
    return std::filesystem::path{"/gnfs-worker-cleanup-codec-root"};
#endif
}

[[nodiscard]] std::optional<Digest> frozen_path_digest(const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    const auto characters =
        std::span<const std::filesystem::path::value_type>(native.data(), native.size());
    return gnfs::util::sha256(std::as_bytes(characters));
}

struct PrivateHandoffFixture final {
    relation::OOCPrivateHandoffRecordV1 record;
    durable::RecordSnapshot snapshot;
};

[[nodiscard]] PrivateHandoffFixture
make_private_handoff(const sieve::WorkerHandoffV1& worker,
                     const sieve::NativeIdentityV1& base_lock_identity,
                     const sieve::NativeIdentityV1& owned_marker_identity,
                     durable::NativeIdentity snapshot_identity) {
    relation::OOCPrivateHandoffRecordV1 record{
        .lease_id = worker.lease.lease_id.limbs,
        .lock_identity = relation_identity(base_lock_identity),
        .directory_identity = relation_identity(worker.lease.directory),
        .owner_marker_identity = relation_identity(worker.lease.owner_marker),
        .owned_marker_identity = relation_identity(owned_marker_identity),
        .pair = relation_pair(worker.artifact),
        .index = relation_artifact(worker.artifact.index_file),
        .data = relation_artifact(worker.artifact.data_file),
        .payload_kind =
            static_cast<std::uint32_t>(sieve::DistributedSieveRecordKindV1::worker_handoff),
        .payload_version = sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1,
        .opaque_payload = encode_record(Record{worker}),
    };
    record = reseal_private_handoff(std::move(record));
    const auto bytes = encode_private_handoff(record);
    return {
        .record = std::move(record),
        .snapshot =
            {
                .identity = snapshot_identity,
                .size = static_cast<std::uint64_t>(bytes.size()),
            },
    };
}

[[nodiscard]] sieve::WaveManifestV1 make_manifest() {
    sieve::WaveManifestV1 manifest{
        .wave_id = wave_id_with_seed(1),
        .execution_contract_version = 1,
        .executable_sha256 = digest_with_seed(2),
        .work_sha256 = digest_with_seed(3),
        .wave_root_identity = native_identity(100),
        .permanent_lock_identity = native_identity(200),
        .lock_semantics_version = 1,
        .effective_sq_begin = 10,
        .effective_sq_end = 12,
        .worker_count = 1,
        .chunks = {sieve::ChunkPlanV1{0, 10, 12, "cleanup_chunk"}},
        .sq_cap_per_worker = 10,
        .relation_cap_per_worker = 100,
        .max_worker_attempts = 3,
        .max_merge_build_attempts = 2,
        .max_consumption_attempts = 2,
        .canonical_naming_version = sieve::DISTRIBUTED_SIEVE_CANONICAL_NAMING_VERSION_V1,
        .retry_policy_version = 1,
        .durable_start_consumes_ordinal = true,
        .ooc_format_version = relation::OOCRelationStoreFormat::FORMAT_VERSION_V3,
        .relation_serialization_version = 1,
        .handoff_version = sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1,
        .receipt_version = 1,
        .digest_version = 1,
        .merge_policy_version = 1,
    };
    return seal_value(std::move(manifest));
}

[[nodiscard]] sieve::WorkerHandoffV1
make_worker_handoff(const sieve::WaveManifestV1& manifest,
                    const resume::DistributedSieveWorkerAttemptNamesV1& names) {
    const auto& chunk = manifest.chunks.front();
    sieve::WorkerHandoffV1 worker{
        .manifest_digest = manifest.self_digest,
        .work_digest = manifest.work_sha256,
        .wave_id = manifest.wave_id,
        .chunk_id = chunk.chunk_id,
        .sq_begin = chunk.sq_begin,
        .sq_end = chunk.sq_end,
        .attempt_ordinal = 1,
        .attempt_started_digest = digest_with_seed(20),
        .lease = lease_identity(400, names.relative_lease_stem),
        .artifact = make_artifact(1000, 1),
        .processed_sq_count = 2,
        .next_sq_index = chunk.sq_end,
        .completion_reason = sieve::WorkerCompletionReasonV1::range_exhausted,
        .relation_count = 1,
        .cleanup_intent_absent = true,
    };
    return seal_value(std::move(worker));
}

[[nodiscard]] sieve::WaveMergeCommitV1 make_commit(const sieve::WaveManifestV1& manifest,
                                                   const sieve::WorkerHandoffV1& worker) {
    const sieve::TerminalChunkInputV1 input{
        .chunk_id = worker.chunk_id,
        .disposition = sieve::ChunkDispositionV1::handoff,
        .sq_begin = worker.sq_begin,
        .sq_end = worker.sq_end,
        .next_sq_index = worker.next_sq_index,
        .processed_sq_count = worker.processed_sq_count,
        .completion_reason = worker.completion_reason,
        .durable_attempt_count = worker.attempt_ordinal + 1U,
        .last_attempt_digest = worker.attempt_started_digest,
        .lease_id = worker.lease.lease_id,
        .handoff_digest = worker.self_digest,
        .raw_relation_count = worker.relation_count,
        .sequence_receipt = worker.artifact.sequence_receipt,
        .corpus_sha256 = worker.artifact.corpus_sha256,
    };
    sieve::WaveMergeCommitV1 commit{
        .manifest_digest = manifest.self_digest,
        .work_digest = manifest.work_sha256,
        .chunks =
            {
                sieve::ChunkCommitSummaryV1{
                    .input = input,
                    .retained_relation_count = 1,
                    .diagnostic = {},
                },
            },
        .merge_policy_version = manifest.merge_policy_version,
        .input_relation_count = 1,
        .duplicate_relation_count = 0,
        .output_relation_count = 1,
        .merge_prepared_digest = digest_with_seed(30),
        .merged_lease = lease_identity(2000, "merged_attempt_0"),
        .merged_artifact = make_artifact(3000, 1),
    };
    return seal_value(std::move(commit));
}

struct Fixture final {
    std::filesystem::path root = absolute_test_root();
    sieve::WaveManifestV1 manifest = make_manifest();
    resume::DistributedSieveWorkerAttemptNamesV1 names;
    sieve::WorkerHandoffV1 worker;
    sieve::WaveMergeCommitV1 commit;
    sieve::NativeIdentityV1 base_lock_identity = native_identity(700);
    sieve::NativeIdentityV1 owned_marker_identity = native_identity(800);
    PrivateHandoffFixture private_handoff;

    Fixture() {
        const auto derived = resume::distributed_sieve_worker_attempt_names_v1(
            manifest.chunks.front().relative_artifact_stem, manifest.chunks.front().chunk_id, 1);
        CHECK(derived.has_value());
        names = *derived;
        worker = make_worker_handoff(manifest, names);
        commit = make_commit(manifest, worker);
        private_handoff = make_private_handoff(worker, base_lock_identity, owned_marker_identity,
                                               relation_identity(native_identity(900)));
    }
};

[[nodiscard]] AuthorizationResult
build_authorization(const Fixture& fixture, const sieve::WaveMergeCommitV1& commit,
                    std::uint32_t manifest_order_ordinal, const sieve::WorkerHandoffV1& worker,
                    const PrivateHandoffFixture& private_handoff,
                    const sieve::NativeIdentityV1& base_lock_identity,
                    const sieve::NativeIdentityV1& owned_marker_identity) {
    return codec::build_distributed_sieve_worker_cleanup_authorization_v1(
        fixture.manifest, commit, manifest_order_ordinal, worker, private_handoff.record,
        base_lock_identity, owned_marker_identity, private_handoff.snapshot);
}

[[nodiscard]] AuthorizationResult build_authorization(const Fixture& fixture) {
    return build_authorization(fixture, fixture.commit, 0, fixture.worker, fixture.private_handoff,
                               fixture.base_lock_identity, fixture.owned_marker_identity);
}

void require_authorization_failure(const AuthorizationResult& result, BuildStatus status,
                                   BuildPhase phase) {
    CHECK(!result);
    CHECK(!result.authorization.has_value());
    CHECK(result.diagnostic.status == status);
    CHECK(result.diagnostic.phase == phase);
}

void require_binding_failure(const BindingResult& result, BuildStatus status, BuildPhase phase) {
    CHECK(!result);
    CHECK(!result.relation_binding.has_value());
    CHECK(result.diagnostic.status == status);
    CHECK(result.diagnostic.phase == phase);
}

void require_completion_failure(const CompletionResult& result, BuildStatus status,
                                BuildPhase phase) {
    CHECK(!result);
    CHECK(!result.completion.has_value());
    CHECK(result.diagnostic.status == status);
    CHECK(result.diagnostic.phase == phase);
}

void require_projection_failure(const ProjectionResult& result, BuildStatus status,
                                BuildPhase phase) {
    CHECK(!result);
    CHECK(!result.worker_handoff.has_value());
    CHECK(result.diagnostic.status == status);
    CHECK(result.diagnostic.phase == phase);
}

void require_authorization_round_trip(
    const codec::DistributedSieveWorkerCleanupAuthorizationV1& authorization) {
    const auto decoded = sieve::decode_distributed_sieve_record(authorization.canonical_bytes);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* typed = std::get_if<sieve::ArtifactCleanupAuthorizedV1>(&*decoded.value);
    CHECK(typed != nullptr);
    CHECK(typed->self_digest == authorization.record.self_digest);
    CHECK(typed->private_handoff_digest == authorization.record.private_handoff_digest);
    CHECK(typed->private_handoff_record == authorization.record.private_handoff_record);
    const auto reencoded = sieve::encode_distributed_sieve_record(*decoded.value);
    CHECK(reencoded);
    CHECK(reencoded.bytes.has_value());
    CHECK(*reencoded.bytes == authorization.canonical_bytes);
}

void require_completion_round_trip(
    const codec::DistributedSieveWorkerCleanupCompletionV1& completion) {
    const auto decoded = sieve::decode_distributed_sieve_record(completion.canonical_bytes);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    const auto* typed = std::get_if<sieve::ArtifactCleanupCompletedV1>(&*decoded.value);
    CHECK(typed != nullptr);
    CHECK(typed->self_digest == completion.record.self_digest);
    CHECK(typed->authorization_digest == completion.record.authorization_digest);
    CHECK(typed->cleanup_intent_identity == completion.record.cleanup_intent_identity);
    const auto reencoded = sieve::encode_distributed_sieve_record(*decoded.value);
    CHECK(reencoded);
    CHECK(reencoded.bytes.has_value());
    CHECK(*reencoded.bytes == completion.canonical_bytes);
}

[[nodiscard]] cleanup::OOCPrivateHandoffCleanupAbsenceEvidenceV2 make_absence_evidence(
    const Fixture& fixture, const sieve::ArtifactCleanupAuthorizedV1& authorization,
    std::optional<durable::RecordSnapshot> cleanup_intent_snapshot = std::nullopt) {
    const auto binding = codec::build_distributed_sieve_worker_cleanup_relation_binding_v1(
        fixture.root, fixture.manifest, authorization);
    CHECK(binding);
    CHECK(binding.relation_binding.has_value());
    const auto digest = frozen_path_digest(binding.relation_binding->binding.base_path);
    CHECK(digest.has_value());
    return {
        .base_path_digest = *digest,
        .external_authorization_digest = authorization.self_digest,
        .lease_id = authorization.lease.lease_id.limbs,
        .cleanup_intent_snapshot = std::move(cleanup_intent_snapshot),
        .parent_directory_identity = relation_identity(fixture.manifest.wave_root_identity),
        .parent_directory_durability_confirmed = true,
        .expected_namespace_absent = true,
    };
}

[[nodiscard]] sieve::ArtifactCleanupAuthorizedV1
test_successful_round_trips_and_binding(const Fixture& fixture) {
    const auto authorization = build_authorization(fixture);
    CHECK(authorization);
    CHECK(authorization.authorization.has_value());
    CHECK(authorization.diagnostic);
    require_authorization_round_trip(*authorization.authorization);

    const auto& record = authorization.authorization->record;
    CHECK(record.manifest_digest == fixture.manifest.self_digest);
    CHECK(record.authorizer_record_digest == fixture.commit.self_digest);
    CHECK(record.manifest_order_ordinal == 0);
    CHECK(record.lease == fixture.worker.lease);
    CHECK(record.base_lock_identity == fixture.base_lock_identity);
    CHECK(record.owned_marker_identity == fixture.owned_marker_identity);
    CHECK(record.handoff_digest == fixture.worker.self_digest);
    CHECK(record.private_handoff_digest == fixture.private_handoff.record.self_digest);
    const sieve::NativeIdentityV1 expected_private_handoff_identity{
        fixture.private_handoff.snapshot.identity.first,
        fixture.private_handoff.snapshot.identity.second,
        fixture.private_handoff.snapshot.identity.third,
    };
    CHECK(record.private_handoff_record.identity == expected_private_handoff_identity);
    CHECK(record.private_handoff_record.extent == fixture.private_handoff.snapshot.size);

    const auto projected = codec::project_distributed_sieve_worker_cleanup_handoff_v1(
        fixture.manifest, fixture.commit, record);
    CHECK(projected);
    CHECK(projected.worker_handoff.has_value());
    CHECK(projected.diagnostic);
    const auto& worker = *projected.worker_handoff;
    CHECK(worker.manifest_digest == fixture.worker.manifest_digest);
    CHECK(worker.work_digest == fixture.worker.work_digest);
    CHECK(worker.wave_id == fixture.worker.wave_id);
    CHECK(worker.chunk_id == fixture.worker.chunk_id);
    CHECK(worker.sq_begin == fixture.worker.sq_begin);
    CHECK(worker.sq_end == fixture.worker.sq_end);
    CHECK(worker.attempt_ordinal == fixture.worker.attempt_ordinal);
    CHECK(worker.attempt_started_digest == fixture.worker.attempt_started_digest);
    CHECK(worker.lease == fixture.worker.lease);
    CHECK(worker.artifact == fixture.worker.artifact);
    CHECK(worker.processed_sq_count == fixture.worker.processed_sq_count);
    CHECK(worker.next_sq_index == fixture.worker.next_sq_index);
    CHECK(worker.completion_reason == fixture.worker.completion_reason);
    CHECK(worker.relation_count == fixture.worker.relation_count);
    CHECK(worker.cleanup_intent_absent == fixture.worker.cleanup_intent_absent);
    CHECK(worker.self_digest == fixture.worker.self_digest);
    CHECK(sieve::validate_distributed_sieve_record(Record{worker}, true));

    const auto binding = codec::build_distributed_sieve_worker_cleanup_relation_binding_v1(
        fixture.root, fixture.manifest, record);
    CHECK(binding);
    CHECK(binding.relation_binding.has_value());
    CHECK(binding.diagnostic);
    CHECK(binding.relation_binding->attempt_names == fixture.names);
    const auto expected_base_path = fixture.root / fixture.names.private_directory_leaf / "corpus";
    const auto& relation_binding = binding.relation_binding->binding;
    CHECK(relation_binding.base_path == expected_base_path);
    CHECK(relation_binding.external_authorization_digest == record.self_digest);
    CHECK(relation_binding.generic_handoff_self_digest == record.private_handoff_digest);
    CHECK(relation_binding.lease_id == record.lease.lease_id.limbs);
    CHECK(relation_binding.parent_directory_identity ==
          relation_identity(fixture.manifest.wave_root_identity));
    CHECK(relation_binding.lock_identity == relation_identity(record.base_lock_identity));
    CHECK(relation_binding.directory_identity == relation_identity(record.lease.directory));
    CHECK(relation_binding.owner_marker_identity == relation_identity(record.lease.owner_marker));
    CHECK(relation_binding.owned_marker_identity ==
          relation_identity(record.owned_marker_identity));
    CHECK(relation_binding.pair == relation_pair(record.artifact));
    CHECK(relation_binding.handoff == relation_artifact(record.private_handoff_record));
    CHECK(relation_binding.index == relation_artifact(record.artifact.index_file));
    CHECK(relation_binding.data == relation_artifact(record.artifact.data_file));

    const auto markerless_evidence = make_absence_evidence(fixture, record);
    const auto markerless_completion = codec::build_distributed_sieve_worker_cleanup_completion_v1(
        fixture.root, fixture.manifest, record, markerless_evidence);
    CHECK(markerless_completion);
    CHECK(markerless_completion.completion.has_value());
    CHECK(markerless_completion.diagnostic);
    CHECK(!markerless_completion.completion->record.cleanup_intent_identity.has_value());
    require_completion_round_trip(*markerless_completion.completion);

    const durable::RecordSnapshot intent_snapshot{
        .identity = relation_identity(native_identity(5000)),
        .size = relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2,
    };
    const auto intent_evidence = make_absence_evidence(fixture, record, intent_snapshot);
    const auto intent_completion = codec::build_distributed_sieve_worker_cleanup_completion_v1(
        fixture.root, fixture.manifest, record, intent_evidence);
    CHECK(intent_completion);
    CHECK(intent_completion.completion.has_value());
    CHECK(intent_completion.diagnostic);
    CHECK(intent_completion.completion->record.cleanup_intent_identity ==
          std::optional<sieve::NativeIdentityV1>(native_identity(5000)));
    require_completion_round_trip(*intent_completion.completion);

    return record;
}

void test_handoff_projection_failures(const Fixture& fixture,
                                      const sieve::ArtifactCleanupAuthorizedV1& authorization) {
    auto durable_zero = fixture.commit;
    auto& zero_input = durable_zero.chunks.front().input;
    zero_input.disposition = sieve::ChunkDispositionV1::empty;
    zero_input.sq_end = zero_input.sq_begin;
    zero_input.next_sq_index = zero_input.sq_begin;
    zero_input.processed_sq_count = 0;
    zero_input.completion_reason = sieve::WorkerCompletionReasonV1::zero_relations;
    zero_input.durable_attempt_count = 0;
    zero_input.last_attempt_digest = {};
    zero_input.lease_id = {};
    zero_input.handoff_digest = {};
    zero_input.raw_relation_count = 0;
    zero_input.sequence_receipt = {};
    zero_input.corpus_sha256 = {};
    durable_zero.chunks.front().retained_relation_count = 0;
    durable_zero.input_relation_count = 0;
    durable_zero.duplicate_relation_count = 0;
    durable_zero.output_relation_count = 0;
    durable_zero.merged_artifact = make_artifact(3000, 0);
    durable_zero = reseal(std::move(durable_zero));
    auto zero_authorization = authorization;
    zero_authorization.authorizer_record_digest = durable_zero.self_digest;
    zero_authorization = reseal(std::move(zero_authorization));
    require_projection_failure(codec::project_distributed_sieve_worker_cleanup_handoff_v1(
                                   fixture.manifest, durable_zero, zero_authorization),
                               BuildStatus::worker_handoff_projection_failed,
                               BuildPhase::worker_handoff_projection);

    auto digest_drift = authorization;
    perturb_digest(digest_drift.handoff_digest);
    digest_drift = reseal(std::move(digest_drift));
    require_projection_failure(codec::project_distributed_sieve_worker_cleanup_handoff_v1(
                                   fixture.manifest, fixture.commit, digest_drift),
                               BuildStatus::protocol_validation_failed,
                               BuildPhase::worker_handoff_validation);

    auto lease_drift = authorization;
    ++lease_drift.lease.lease_id.limbs[1];
    lease_drift = reseal(std::move(lease_drift));
    require_projection_failure(codec::project_distributed_sieve_worker_cleanup_handoff_v1(
                                   fixture.manifest, fixture.commit, lease_drift),
                               BuildStatus::protocol_validation_failed,
                               BuildPhase::worker_handoff_validation);

    auto artifact_drift = authorization;
    ++artifact_drift.artifact.descriptor.store_id;
    artifact_drift = reseal(std::move(artifact_drift));
    require_projection_failure(codec::project_distributed_sieve_worker_cleanup_handoff_v1(
                                   fixture.manifest, fixture.commit, artifact_drift),
                               BuildStatus::protocol_validation_failed,
                               BuildPhase::worker_handoff_validation);

    auto ordinal_drift = authorization;
    ordinal_drift.manifest_order_ordinal = 1;
    ordinal_drift = reseal(std::move(ordinal_drift));
    require_projection_failure(codec::project_distributed_sieve_worker_cleanup_handoff_v1(
                                   fixture.manifest, fixture.commit, ordinal_drift),
                               BuildStatus::worker_handoff_projection_failed,
                               BuildPhase::worker_handoff_projection);
}

void test_authorization_bridge_failures(const Fixture& fixture) {
    auto payload_drift = fixture.private_handoff;
    CHECK(!payload_drift.record.opaque_payload.empty());
    payload_drift.record.opaque_payload[payload_drift.record.opaque_payload.size() / 2U] ^=
        std::byte{1};
    payload_drift.record = reseal_private_handoff(std::move(payload_drift.record));
    payload_drift.snapshot.size =
        static_cast<std::uint64_t>(encode_private_handoff(payload_drift.record).size());
    require_authorization_failure(build_authorization(fixture, fixture.commit, 0, fixture.worker,
                                                      payload_drift, fixture.base_lock_identity,
                                                      fixture.owned_marker_identity),
                                  BuildStatus::private_handoff_bridge_mismatch,
                                  BuildPhase::private_handoff_bridge_validation);

    auto payload_version_drift = fixture.private_handoff;
    ++payload_version_drift.record.payload_version;
    payload_version_drift.record = reseal_private_handoff(std::move(payload_version_drift.record));
    payload_version_drift.snapshot.size =
        static_cast<std::uint64_t>(encode_private_handoff(payload_version_drift.record).size());
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, fixture.worker, payload_version_drift,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::private_handoff_bridge_mismatch,
        BuildPhase::private_handoff_bridge_validation);

    auto short_snapshot = fixture.private_handoff;
    --short_snapshot.snapshot.size;
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, fixture.worker, short_snapshot,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::private_handoff_snapshot_mismatch, BuildPhase::private_handoff_validation);

    auto long_snapshot = fixture.private_handoff;
    ++long_snapshot.snapshot.size;
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, fixture.worker, long_snapshot,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::private_handoff_snapshot_mismatch, BuildPhase::private_handoff_validation);

    auto zero_snapshot_identity = fixture.private_handoff;
    zero_snapshot_identity.snapshot.identity = {};
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, fixture.worker, zero_snapshot_identity,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::private_handoff_snapshot_mismatch, BuildPhase::private_handoff_validation);

    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, fixture.worker, fixture.private_handoff,
                            native_identity(6000), fixture.owned_marker_identity),
        BuildStatus::private_handoff_bridge_mismatch,
        BuildPhase::private_handoff_bridge_validation);
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, fixture.worker, fixture.private_handoff,
                            fixture.base_lock_identity, native_identity(6100)),
        BuildStatus::private_handoff_bridge_mismatch,
        BuildPhase::private_handoff_bridge_validation);

    auto snapshot_alias = fixture.private_handoff;
    snapshot_alias.snapshot.identity = relation_identity(fixture.base_lock_identity);
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, fixture.worker, snapshot_alias,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::record_sealing_failed, BuildPhase::authorization_sealing);

    const auto root_lock_handoff = make_private_handoff(
        fixture.worker, fixture.manifest.wave_root_identity, fixture.owned_marker_identity,
        fixture.private_handoff.snapshot.identity);
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, fixture.worker, root_lock_handoff,
                            fixture.manifest.wave_root_identity, fixture.owned_marker_identity),
        BuildStatus::dependency_validation_failed, BuildPhase::authorization_dependency_validation);
}

void test_authorization_source_failures(const Fixture& fixture) {
    auto lease_drift = fixture.worker;
    ++lease_drift.lease.lease_id.limbs[1];
    lease_drift = reseal(std::move(lease_drift));
    const auto lease_handoff =
        make_private_handoff(lease_drift, fixture.base_lock_identity, fixture.owned_marker_identity,
                             fixture.private_handoff.snapshot.identity);
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, lease_drift, lease_handoff,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::dependency_validation_failed, BuildPhase::authorization_dependency_validation);

    auto artifact_drift = fixture.worker;
    ++artifact_drift.artifact.descriptor.store_id;
    artifact_drift = reseal(std::move(artifact_drift));
    const auto artifact_handoff = make_private_handoff(artifact_drift, fixture.base_lock_identity,
                                                       fixture.owned_marker_identity,
                                                       fixture.private_handoff.snapshot.identity);
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, artifact_drift, artifact_handoff,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::dependency_validation_failed, BuildPhase::authorization_dependency_validation);

    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 1, fixture.worker, fixture.private_handoff,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::worker_attempt_naming_failed, BuildPhase::worker_attempt_naming);

    auto commit_drift = fixture.commit;
    perturb_digest(commit_drift.chunks.front().input.handoff_digest);
    commit_drift = reseal(std::move(commit_drift));
    require_authorization_failure(
        build_authorization(fixture, commit_drift, 0, fixture.worker, fixture.private_handoff,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::dependency_validation_failed, BuildPhase::authorization_dependency_validation);

    const auto attempt_zero = resume::distributed_sieve_worker_attempt_names_v1(
        fixture.manifest.chunks.front().relative_artifact_stem,
        fixture.manifest.chunks.front().chunk_id, 0);
    CHECK(attempt_zero.has_value());
    auto attempt_stem_drift = fixture.worker;
    attempt_stem_drift.lease.relative_stem = attempt_zero->relative_lease_stem;
    attempt_stem_drift = reseal(std::move(attempt_stem_drift));
    const auto attempt_stem_handoff = make_private_handoff(
        attempt_stem_drift, fixture.base_lock_identity, fixture.owned_marker_identity,
        fixture.private_handoff.snapshot.identity);
    require_authorization_failure(
        build_authorization(fixture, fixture.commit, 0, attempt_stem_drift, attempt_stem_handoff,
                            fixture.base_lock_identity, fixture.owned_marker_identity),
        BuildStatus::worker_attempt_naming_failed, BuildPhase::worker_attempt_naming);
}

void test_relation_binding_failures(const Fixture& fixture,
                                    const sieve::ArtifactCleanupAuthorizedV1& authorization) {
    require_binding_failure(
        codec::build_distributed_sieve_worker_cleanup_relation_binding_v1(
            std::filesystem::path{"relative-root"}, fixture.manifest, authorization),
        BuildStatus::absolute_root_invalid, BuildPhase::absolute_root_validation);

    const auto filesystem_root = fixture.root.root_path();
    CHECK(filesystem_root.is_absolute());
    CHECK(!filesystem_root.has_filename() || filesystem_root.filename().empty());
    require_binding_failure(codec::build_distributed_sieve_worker_cleanup_relation_binding_v1(
                                filesystem_root, fixture.manifest, authorization),
                            BuildStatus::absolute_root_invalid,
                            BuildPhase::absolute_root_validation);

    const auto noncanonical_root = fixture.root / ".";
    CHECK(noncanonical_root.lexically_normal() != noncanonical_root);
    require_binding_failure(codec::build_distributed_sieve_worker_cleanup_relation_binding_v1(
                                noncanonical_root, fixture.manifest, authorization),
                            BuildStatus::absolute_root_invalid,
                            BuildPhase::absolute_root_validation);

    auto unmatched_attempt = authorization;
    unmatched_attempt.lease.relative_stem = "unmatched_worker_attempt";
    unmatched_attempt = reseal(std::move(unmatched_attempt));
    require_binding_failure(codec::build_distributed_sieve_worker_cleanup_relation_binding_v1(
                                fixture.root, fixture.manifest, unmatched_attempt),
                            BuildStatus::worker_attempt_naming_failed,
                            BuildPhase::worker_attempt_naming);

    auto root_alias = authorization;
    root_alias.base_lock_identity = fixture.manifest.wave_root_identity;
    root_alias = reseal(std::move(root_alias));
    require_binding_failure(codec::build_distributed_sieve_worker_cleanup_relation_binding_v1(
                                fixture.root, fixture.manifest, root_alias),
                            BuildStatus::input_invalid, BuildPhase::authorization_validation);
}

void test_completion_evidence_failures(const Fixture& fixture,
                                       const sieve::ArtifactCleanupAuthorizedV1& authorization) {
    const auto exact = make_absence_evidence(fixture, authorization);

    auto changed = exact;
    perturb_digest(changed.base_path_digest);
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    const auto wrong_root = fixture.root.parent_path() / "gnfs-worker-cleanup-codec-wrong-root";
    CHECK(wrong_root != fixture.root);
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   wrong_root, fixture.manifest, authorization, exact),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    changed = exact;
    perturb_digest(changed.external_authorization_digest);
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    changed = exact;
    ++changed.lease_id[1];
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    changed = exact;
    changed.parent_directory_identity = relation_identity(native_identity(7000));
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    changed = exact;
    changed.parent_directory_durability_confirmed = false;
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    changed = exact;
    changed.expected_namespace_absent = false;
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    const durable::RecordSnapshot intent_snapshot{
        .identity = relation_identity(native_identity(8000)),
        .size = relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2,
    };
    changed = make_absence_evidence(fixture, authorization, intent_snapshot);
    changed.cleanup_intent_snapshot->size =
        relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 - 1U;
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    changed = make_absence_evidence(fixture, authorization, intent_snapshot);
    changed.cleanup_intent_snapshot->size =
        relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 + 1U;
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    changed = make_absence_evidence(fixture, authorization, intent_snapshot);
    changed.cleanup_intent_snapshot->identity = {};
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    changed = make_absence_evidence(fixture, authorization, intent_snapshot);
    changed.cleanup_intent_snapshot->identity =
        relation_identity(fixture.manifest.permanent_lock_identity);
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::absence_evidence_mismatch,
                               BuildPhase::absence_evidence_validation);

    changed = make_absence_evidence(fixture, authorization, intent_snapshot);
    changed.cleanup_intent_snapshot->identity = relation_identity(authorization.lease.owner_marker);
    require_completion_failure(codec::build_distributed_sieve_worker_cleanup_completion_v1(
                                   fixture.root, fixture.manifest, authorization, changed),
                               BuildStatus::dependency_validation_failed,
                               BuildPhase::completion_dependency_validation);
}

} // namespace

int main() {
    try {
        const Fixture fixture;
        const auto authorization = test_successful_round_trips_and_binding(fixture);
        test_handoff_projection_failures(fixture, authorization);
        test_authorization_bridge_failures(fixture);
        test_authorization_source_failures(fixture);
        test_relation_binding_failures(fixture, authorization);
        test_completion_evidence_failures(fixture, authorization);
        std::cout << "distributed sieve worker cleanup codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
