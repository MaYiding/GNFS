#include "distributed_sieve_merge_writer_codec_internal.hpp"

#include <gnfs/relation/ooc_relation_store.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace gnfs::sieve::distributed_sieve_merge_writer_codec_detail {
namespace {

using BuildPhase = DistributedSieveMergePreparedPayloadBuildPhaseV1;
using BuildResult = DistributedSieveMergePreparedPayloadBuildResultV1;
using BuildStatus = DistributedSieveMergePreparedPayloadBuildStatusV1;

[[nodiscard]] constexpr DistributedSieveProtocolStatus
protocol_failure(DistributedSieveProtocolError error,
                 std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {
        .error = error,
        .byte_offset = DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
        .element_index = element_index,
    };
}

[[nodiscard]] BuildResult failure(BuildPhase phase, BuildStatus status,
                                  DistributedSieveProtocolStatus protocol) noexcept {
    return {
        .prepared = std::nullopt,
        .diagnostic =
            {
                .phase = phase,
                .status = status,
                .protocol = protocol,
            },
    };
}

[[nodiscard]] BuildResult resource_failure(BuildPhase phase) noexcept {
    return failure(phase, BuildStatus::resource_exhausted,
                   protocol_failure(DistributedSieveProtocolError::resource_exhausted));
}

[[nodiscard]] constexpr bool checked_add(std::uint64_t& accumulator, std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - accumulator) {
        return false;
    }
    accumulator += value;
    return true;
}

[[nodiscard]] constexpr NativeIdentityV1
protocol_native_identity(const std::array<std::uint64_t, 3>& identity) noexcept {
    return {
        .volume = identity[0],
        .object = identity[1],
        .generation = identity[2],
    };
}

[[nodiscard]] CorpusArtifactV1
project_corpus_artifact(const relation::OOCFinalizedCorpusEvidenceV1& evidence) noexcept {
    return {
        .descriptor =
            {
                .format_version = evidence.descriptor.format_version,
                .store_id = evidence.descriptor.store_id,
                .generation = evidence.descriptor.generation,
                .relation_count = evidence.descriptor.count,
                .data_end = evidence.descriptor.data_end,
            },
        .index_file =
            {
                .identity = protocol_native_identity(evidence.index_file.identity),
                .extent = evidence.index_file.extent,
            },
        .data_file =
            {
                .identity = protocol_native_identity(evidence.data_file.identity),
                .extent = evidence.data_file.extent,
            },
        .sequence_receipt =
            {
                .relation_count = evidence.sequence_receipt.relation_count,
                .low = evidence.sequence_receipt.low,
                .high = evidence.sequence_receipt.high,
            },
        .corpus_sha256 = evidence.corpus_sha256,
    };
}

[[nodiscard]] constexpr bool native_identity_is_valid(const NativeIdentityV1& identity) noexcept {
    return identity.object != 0;
}

[[nodiscard]] bool
corpus_evidence_is_admissible(const WaveManifestV1& manifest, std::uint64_t output_relation_count,
                              const relation::OOCFinalizedCorpusEvidenceV1& evidence,
                              const CorpusArtifactV1& artifact) noexcept {
    std::uint64_t expected_index_extent = 0;
    try {
        expected_index_extent =
            relation::OOCRelationWriter::index_size_for_count(artifact.descriptor.relation_count);
    } catch (...) {
        return false;
    }
    const bool canonical_data_extent =
        artifact.descriptor.relation_count == 0
            ? artifact.descriptor.data_end == relation::OOCRelationWriter::DATA_HEADER_BYTES
            : artifact.descriptor.data_end > relation::OOCRelationWriter::DATA_HEADER_BYTES;
    return artifact.descriptor.format_version == manifest.ooc_format_version &&
           artifact.descriptor.format_version != 0 && artifact.descriptor.store_id != 0 &&
           artifact.descriptor.generation != 0 &&
           artifact.descriptor.relation_count == output_relation_count &&
           artifact.descriptor.relation_count == artifact.sequence_receipt.relation_count &&
           evidence.descriptor.count == evidence.sequence_receipt.relation_count &&
           native_identity_is_valid(artifact.index_file.identity) &&
           native_identity_is_valid(artifact.data_file.identity) &&
           artifact.index_file.identity != artifact.data_file.identity &&
           artifact.index_file.extent != 0 && artifact.data_file.extent != 0 &&
           artifact.index_file.extent == expected_index_extent &&
           artifact.descriptor.data_end == artifact.data_file.extent && canonical_data_extent;
}

struct ValidatedCountsV1 final {
    std::vector<PerChunkRetainedCountV1> per_chunk_retained_counts;
};

[[nodiscard]] std::optional<BuildResult>
validate_and_copy_counts(const MergeStartedV1& latest, std::uint64_t input_relation_count,
                         std::uint64_t duplicate_relation_count,
                         std::uint64_t output_relation_count,
                         std::span<const PerChunkRetainedCountV1> per_chunk_retained_counts,
                         ValidatedCountsV1& validated) {
    if (per_chunk_retained_counts.size() != latest.ordered_inputs.size()) {
        return failure(BuildPhase::count_validation, BuildStatus::per_chunk_counts_invalid,
                       protocol_failure(DistributedSieveProtocolError::invalid_value));
    }
    if (duplicate_relation_count > input_relation_count ||
        output_relation_count > input_relation_count) {
        return failure(BuildPhase::count_validation, BuildStatus::count_conservation_invalid,
                       protocol_failure(DistributedSieveProtocolError::invalid_value));
    }
    std::uint64_t conserved_count = duplicate_relation_count;
    if (!checked_add(conserved_count, output_relation_count)) {
        return failure(BuildPhase::count_validation, BuildStatus::count_overflow,
                       protocol_failure(DistributedSieveProtocolError::integer_out_of_range));
    }
    if (conserved_count != input_relation_count) {
        return failure(BuildPhase::count_validation, BuildStatus::count_conservation_invalid,
                       protocol_failure(DistributedSieveProtocolError::invalid_value));
    }

    std::uint64_t observed_input = 0;
    std::uint64_t observed_output = 0;
    for (std::size_t index = 0; index < latest.ordered_inputs.size(); ++index) {
        const auto protocol_index = static_cast<std::uint32_t>(index);
        const auto& input = latest.ordered_inputs[index];
        const auto& retained = per_chunk_retained_counts[index];
        if (retained.chunk_id != input.chunk_id) {
            return failure(BuildPhase::count_validation, BuildStatus::per_chunk_counts_invalid,
                           protocol_failure(DistributedSieveProtocolError::noncanonical_order,
                                            protocol_index));
        }
        if (retained.retained_relation_count > input.raw_relation_count) {
            return failure(
                BuildPhase::count_validation, BuildStatus::per_chunk_counts_invalid,
                protocol_failure(DistributedSieveProtocolError::invalid_value, protocol_index));
        }
        if (!checked_add(observed_input, input.raw_relation_count) ||
            !checked_add(observed_output, retained.retained_relation_count)) {
            return failure(BuildPhase::count_validation, BuildStatus::count_overflow,
                           protocol_failure(DistributedSieveProtocolError::integer_out_of_range,
                                            protocol_index));
        }
    }
    if (observed_input != input_relation_count || observed_output != output_relation_count) {
        return failure(BuildPhase::count_validation, BuildStatus::count_conservation_invalid,
                       protocol_failure(DistributedSieveProtocolError::invalid_value));
    }

    validated.per_chunk_retained_counts.assign(per_chunk_retained_counts.begin(),
                                               per_chunk_retained_counts.end());
    return std::nullopt;
}

[[nodiscard]] bool
prepared_matches_inputs(const MergePreparedV1& prepared, const WaveManifestV1& manifest,
                        const MergeStartedV1& latest, std::uint64_t input_relation_count,
                        std::uint64_t duplicate_relation_count, std::uint64_t output_relation_count,
                        std::span<const PerChunkRetainedCountV1> per_chunk_retained_counts,
                        const CorpusArtifactV1& artifact) noexcept {
    return prepared.manifest_digest == manifest.self_digest &&
           prepared.work_digest == manifest.work_sha256 &&
           prepared.merge_policy_version == manifest.merge_policy_version &&
           prepared.merge_started_digest == latest.self_digest &&
           prepared.ordered_inputs == latest.ordered_inputs &&
           prepared.input_relation_count == input_relation_count &&
           prepared.duplicate_relation_count == duplicate_relation_count &&
           prepared.output_relation_count == output_relation_count &&
           prepared.per_chunk_retained_counts.size() == per_chunk_retained_counts.size() &&
           std::equal(prepared.per_chunk_retained_counts.begin(),
                      prepared.per_chunk_retained_counts.end(), per_chunk_retained_counts.begin(),
                      per_chunk_retained_counts.end()) &&
           prepared.merged_artifact == artifact && prepared.merged_lease == latest.merged_lease;
}

} // namespace

DistributedSieveMergePreparedPayloadBuildResultV1 build_distributed_sieve_merge_prepared_payload_v1(
    const WaveManifestV1& manifest, std::span<const MergeStartedV1> merge_started_chain,
    std::uint64_t input_relation_count, std::uint64_t duplicate_relation_count,
    std::uint64_t output_relation_count,
    std::span<const PerChunkRetainedCountV1> per_chunk_retained_counts,
    const relation::OOCFinalizedCorpusEvidenceV1& evidence) noexcept {
    BuildPhase phase = BuildPhase::request_validation;
    try {
        if (merge_started_chain.empty()) {
            return failure(phase, BuildStatus::empty_merge_chain,
                           protocol_failure(DistributedSieveProtocolError::invalid_value));
        }

        phase = BuildPhase::merge_chain_validation;
        if (const auto status =
                validate_merge_predecessor_chain(manifest, merge_started_chain, nullptr, nullptr);
            !status) {
            return failure(phase, BuildStatus::merge_chain_invalid, status);
        }
        const auto& latest = merge_started_chain.back();

        phase = BuildPhase::count_validation;
        ValidatedCountsV1 validated_counts;
        if (auto count_failure = validate_and_copy_counts(
                latest, input_relation_count, duplicate_relation_count, output_relation_count,
                per_chunk_retained_counts, validated_counts);
            count_failure.has_value()) {
            return std::move(*count_failure);
        }

        phase = BuildPhase::evidence_projection;
        const auto artifact = project_corpus_artifact(evidence);
        if (!corpus_evidence_is_admissible(manifest, output_relation_count, evidence, artifact)) {
            return failure(phase, BuildStatus::corpus_evidence_invalid,
                           protocol_failure(DistributedSieveProtocolError::invalid_value));
        }

        DistributedSieveProtocolRecordV1 sealed = MergePreparedV1{
            .manifest_digest = manifest.self_digest,
            .work_digest = manifest.work_sha256,
            .merge_policy_version = manifest.merge_policy_version,
            .merge_started_digest = latest.self_digest,
            .ordered_inputs = latest.ordered_inputs,
            .input_relation_count = input_relation_count,
            .duplicate_relation_count = duplicate_relation_count,
            .output_relation_count = output_relation_count,
            .per_chunk_retained_counts = std::move(validated_counts.per_chunk_retained_counts),
            .merged_artifact = artifact,
            .merged_lease = latest.merged_lease,
            .self_digest = {},
        };

        phase = BuildPhase::record_sealing;
        if (const auto status = seal_distributed_sieve_record(sealed); !status) {
            return failure(phase, BuildStatus::record_sealing_failed, status);
        }

        phase = BuildPhase::record_encoding;
        auto encoded = encode_distributed_sieve_record(sealed);
        if (!encoded || !encoded.bytes.has_value()) {
            return failure(phase, BuildStatus::record_encoding_failed, encoded.status);
        }

        phase = BuildPhase::round_trip_decoding;
        auto decoded = decode_distributed_sieve_record(*encoded.bytes);
        if (!decoded || !decoded.value.has_value()) {
            return failure(phase, BuildStatus::round_trip_decoding_failed, decoded.status);
        }
        const auto* decoded_prepared = std::get_if<MergePreparedV1>(&*decoded.value);
        if (decoded_prepared == nullptr) {
            return failure(phase, BuildStatus::round_trip_type_mismatch,
                           protocol_failure(DistributedSieveProtocolError::record_type_mismatch));
        }

        phase = BuildPhase::round_trip_validation;
        if (const auto status = validate_distributed_sieve_record(*decoded.value, true); !status) {
            return failure(phase, BuildStatus::round_trip_mismatch, status);
        }
        if (!prepared_matches_inputs(*decoded_prepared, manifest, latest, input_relation_count,
                                     duplicate_relation_count, output_relation_count,
                                     per_chunk_retained_counts, artifact)) {
            return failure(phase, BuildStatus::round_trip_mismatch,
                           protocol_failure(DistributedSieveProtocolError::invalid_value));
        }
        auto reencoded = encode_distributed_sieve_record(*decoded.value);
        if (!reencoded || !reencoded.bytes.has_value()) {
            return failure(phase, BuildStatus::round_trip_mismatch, reencoded.status);
        }
        if (*reencoded.bytes != *encoded.bytes) {
            return failure(phase, BuildStatus::round_trip_mismatch,
                           protocol_failure(DistributedSieveProtocolError::digest_mismatch));
        }

        phase = BuildPhase::predecessor_chain_validation;
        if (const auto status = validate_merge_predecessor_chain(manifest, merge_started_chain,
                                                                 decoded_prepared, nullptr);
            !status) {
            return failure(phase, BuildStatus::predecessor_chain_invalid, status);
        }

        MergePreparedV1 typed_record = std::get<MergePreparedV1>(std::move(*decoded.value));
        return {
            .prepared =
                DistributedSieveMergePreparedPayloadV1{
                    .record = std::move(typed_record),
                    .opaque_payload = std::move(*encoded.bytes),
                },
            .diagnostic =
                {
                    .phase = BuildPhase::complete,
                    .status = BuildStatus::ready,
                    .protocol = {},
                },
        };
    } catch (const std::bad_alloc&) {
        return resource_failure(phase);
    } catch (const std::length_error&) {
        return resource_failure(phase);
    } catch (...) {
        return failure(phase, BuildStatus::unexpected_failure,
                       protocol_failure(DistributedSieveProtocolError::invalid_value));
    }
}

} // namespace gnfs::sieve::distributed_sieve_merge_writer_codec_detail
