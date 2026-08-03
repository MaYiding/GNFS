#include "distributed_sieve_worker_cleanup_codec_internal.hpp"

#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace gnfs::sieve::distributed_sieve_worker_cleanup_codec_detail {
namespace {

using BuildDiagnostic = DistributedSieveWorkerCleanupCodecDiagnosticV1;
using BuildPhase = DistributedSieveWorkerCleanupCodecPhaseV1;
using BuildStatus = DistributedSieveWorkerCleanupCodecStatusV1;
using PrivateHandoffStatus = relation::OOCPrivateHandoffProtocolStatus;
using CleanupIntentStatus = relation::OOCAuthorizedCleanupIntentProtocolStatus;
using RelationBinding = relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding;
using AttemptNames = distributed_sieve_resume_detail::DistributedSieveWorkerAttemptNamesV1;

static_assert(static_cast<bool>(DistributedSieveProtocolStatus{}));
static_assert(static_cast<bool>(PrivateHandoffStatus{}));
static_assert(static_cast<bool>(CleanupIntentStatus{}));

[[nodiscard]] constexpr DistributedSieveProtocolStatus
protocol_failure(DistributedSieveProtocolError error,
                 std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {
        .error = error,
        .byte_offset = DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
        .element_index = element_index,
    };
}

[[nodiscard]] constexpr BuildDiagnostic
diagnostic(BuildPhase phase, BuildStatus status, DistributedSieveProtocolStatus protocol = {},
           PrivateHandoffStatus private_handoff = {},
           CleanupIntentStatus cleanup_intent = {}) noexcept {
    return {
        .phase = phase,
        .status = status,
        .protocol = protocol,
        .private_handoff = private_handoff,
        .cleanup_intent = cleanup_intent,
    };
}

[[nodiscard]] constexpr BuildDiagnostic resource_diagnostic(BuildPhase phase) noexcept {
    return diagnostic(phase, BuildStatus::resource_exhausted,
                      protocol_failure(DistributedSieveProtocolError::resource_exhausted));
}

[[nodiscard]] constexpr bool protocol_identity_is_valid(const NativeIdentityV1& identity) noexcept {
    return identity.object != 0;
}

[[nodiscard]] constexpr util::durable_immutable_record::NativeIdentity
relation_identity(const NativeIdentityV1& identity) noexcept {
    return {
        .first = identity.volume,
        .second = identity.object,
        .third = identity.generation,
    };
}

[[nodiscard]] constexpr NativeIdentityV1
protocol_identity(const util::durable_immutable_record::NativeIdentity& identity) noexcept {
    return {
        .volume = identity.first,
        .object = identity.second,
        .generation = identity.third,
    };
}

[[nodiscard]] constexpr relation::OOCPrivateHandoffPairDescriptorV1
relation_pair(const CorpusArtifactV1& artifact) noexcept {
    return {
        .format_version = artifact.descriptor.format_version,
        .store_id = artifact.descriptor.store_id,
        .generation = artifact.descriptor.generation,
        .count = artifact.descriptor.relation_count,
        .index_extent = artifact.index_file.extent,
        .data_extent = artifact.data_file.extent,
    };
}

[[nodiscard]] constexpr relation::OOCPrivateHandoffArtifactBindingV1
relation_artifact(const NativeFileExtentV1& artifact) noexcept {
    return {
        .identity = relation_identity(artifact.identity),
        .extent = artifact.extent,
    };
}

[[nodiscard]] constexpr bool
manifest_control_contains_identity(const WaveManifestV1& manifest,
                                   const NativeIdentityV1& identity) noexcept {
    return identity == manifest.wave_root_identity || identity == manifest.permanent_lock_identity;
}

[[nodiscard]] constexpr bool authorization_contains_manifest_control_identity(
    const WaveManifestV1& manifest, const ArtifactCleanupAuthorizedV1& authorization) noexcept {
    const std::array<NativeIdentityV1, 7> identities{
        authorization.base_lock_identity,
        authorization.lease.directory,
        authorization.lease.owner_marker,
        authorization.owned_marker_identity,
        authorization.private_handoff_record.identity,
        authorization.artifact.index_file.identity,
        authorization.artifact.data_file.identity,
    };
    return std::any_of(identities.begin(), identities.end(), [&](const auto& identity) noexcept {
        return manifest_control_contains_identity(manifest, identity);
    });
}

struct DerivedWorkerAttemptV1 final {
    AttemptNames names;
    std::uint32_t attempt_ordinal = 0;
};

[[nodiscard]] std::optional<DerivedWorkerAttemptV1>
derive_worker_attempt(const WaveManifestV1& manifest, std::uint32_t manifest_order_ordinal,
                      std::string_view relative_lease_stem) {
    if (manifest_order_ordinal >= manifest.chunks.size()) {
        return std::nullopt;
    }
    const auto& chunk = manifest.chunks[manifest_order_ordinal];
    if (chunk.sq_begin >= chunk.sq_end) {
        return std::nullopt;
    }

    std::optional<DerivedWorkerAttemptV1> match;
    for (std::uint32_t attempt_ordinal = 0; attempt_ordinal < manifest.max_worker_attempts;
         ++attempt_ordinal) {
        auto names = distributed_sieve_resume_detail::distributed_sieve_worker_attempt_names_v1(
            chunk.relative_artifact_stem, chunk.chunk_id, attempt_ordinal);
        if (!names.has_value() || names->relative_lease_stem != relative_lease_stem) {
            continue;
        }
        if (match.has_value()) {
            return std::nullopt;
        }
        match = DerivedWorkerAttemptV1{
            .names = std::move(*names),
            .attempt_ordinal = attempt_ordinal,
        };
    }
    return match;
}

[[nodiscard]] bool
private_handoff_matches_worker(const WaveManifestV1& manifest, const WorkerHandoffV1& handoff,
                               const relation::OOCPrivateHandoffRecordV1& private_handoff,
                               const NativeIdentityV1& base_lock_identity,
                               const NativeIdentityV1& owned_marker_identity) noexcept {
    const auto& artifact = handoff.artifact;
    return private_handoff.payload_kind ==
               static_cast<std::uint32_t>(DistributedSieveRecordKindV1::worker_handoff) &&
           private_handoff.payload_version == manifest.handoff_version &&
           private_handoff.lease_id == handoff.lease.lease_id.limbs &&
           private_handoff.lock_identity == relation_identity(base_lock_identity) &&
           private_handoff.directory_identity == relation_identity(handoff.lease.directory) &&
           private_handoff.owner_marker_identity == relation_identity(handoff.lease.owner_marker) &&
           private_handoff.owned_marker_identity == relation_identity(owned_marker_identity) &&
           private_handoff.pair == relation_pair(artifact) &&
           private_handoff.index == relation_artifact(artifact.index_file) &&
           private_handoff.data == relation_artifact(artifact.data_file);
}

[[nodiscard]] bool authorization_matches_source(
    const ArtifactCleanupAuthorizedV1& authorization, const WaveManifestV1& manifest,
    const WaveMergeCommitV1& commit, std::uint32_t manifest_order_ordinal,
    const WorkerHandoffV1& handoff, const relation::OOCPrivateHandoffRecordV1& private_handoff,
    const NativeIdentityV1& base_lock_identity, const NativeIdentityV1& owned_marker_identity,
    const util::durable_immutable_record::RecordSnapshot& private_handoff_snapshot) noexcept {
    return authorization.authorizer == CleanupAuthorizerKindV1::merge_commit_worker &&
           authorization.manifest_digest == manifest.self_digest &&
           authorization.authorizer_record_digest == commit.self_digest &&
           authorization.artifact_kind == CleanupArtifactKindV1::worker &&
           authorization.manifest_order_ordinal == manifest_order_ordinal &&
           authorization.lease == handoff.lease &&
           authorization.base_lock_identity == base_lock_identity &&
           authorization.owned_marker_identity == owned_marker_identity &&
           authorization.handoff_digest == handoff.self_digest &&
           authorization.private_handoff_digest == private_handoff.self_digest &&
           authorization.private_handoff_record.identity ==
               protocol_identity(private_handoff_snapshot.identity) &&
           authorization.private_handoff_record.extent == private_handoff_snapshot.size &&
           authorization.artifact == handoff.artifact;
}

[[nodiscard]] bool completion_matches_evidence(
    const ArtifactCleanupCompletedV1& completion, const ArtifactCleanupAuthorizedV1& authorization,
    const relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAbsenceEvidenceV2& evidence) {
    const std::optional<NativeIdentityV1> expected_intent =
        evidence.cleanup_intent_snapshot.has_value()
            ? std::optional<NativeIdentityV1>(
                  protocol_identity(evidence.cleanup_intent_snapshot->identity))
            : std::nullopt;
    return completion.authorization_digest == authorization.self_digest &&
           completion.cleanup_intent_identity == expected_intent &&
           completion.parent_directory_durability_confirmed && completion.expected_namespace_absent;
}

[[nodiscard]] bool path_is_exact_absolute_root(const std::filesystem::path& root) {
    if (root.empty() || !root.is_absolute() || !root.has_filename() || root.filename().empty()) {
        return false;
    }
    const auto& native = root.native();
    using Character = std::filesystem::path::value_type;
    if (std::find(native.begin(), native.end(), Character{}) != native.end()) {
        return false;
    }
    return root.lexically_normal() == root;
}

[[nodiscard]] std::optional<util::Sha256Digest>
frozen_path_digest(const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    const auto characters =
        std::span<const std::filesystem::path::value_type>(native.data(), native.size());
    return util::sha256(std::as_bytes(characters));
}

[[nodiscard]] bool binding_matches_authorization(
    const RelationBinding& binding, const std::filesystem::path& base_path,
    const WaveManifestV1& manifest, const ArtifactCleanupAuthorizedV1& authorization) noexcept {
    return binding.base_path == base_path &&
           binding.external_authorization_digest == authorization.self_digest &&
           binding.generic_handoff_self_digest == authorization.private_handoff_digest &&
           binding.lease_id == authorization.lease.lease_id.limbs &&
           binding.parent_directory_identity == relation_identity(manifest.wave_root_identity) &&
           binding.lock_identity == relation_identity(authorization.base_lock_identity) &&
           binding.directory_identity == relation_identity(authorization.lease.directory) &&
           binding.owner_marker_identity == relation_identity(authorization.lease.owner_marker) &&
           binding.owned_marker_identity ==
               relation_identity(authorization.owned_marker_identity) &&
           binding.pair == relation_pair(authorization.artifact) &&
           binding.handoff == relation_artifact(authorization.private_handoff_record) &&
           binding.index == relation_artifact(authorization.artifact.index_file) &&
           binding.data == relation_artifact(authorization.artifact.data_file);
}

} // namespace

DistributedSieveWorkerCleanupHandoffProjectionBuildResultV1
project_distributed_sieve_worker_cleanup_handoff_v1(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    const ArtifactCleanupAuthorizedV1& authorization) noexcept {
    BuildPhase phase = BuildPhase::manifest_validation;
    try {
        if (const auto status =
                validate_distributed_sieve_record(DistributedSieveProtocolRecordV1{manifest}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }

        phase = BuildPhase::merge_commit_validation;
        if (const auto status =
                validate_distributed_sieve_record(DistributedSieveProtocolRecordV1{commit}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }

        phase = BuildPhase::authorization_validation;
        if (const auto status = validate_distributed_sieve_record(
                DistributedSieveProtocolRecordV1{authorization}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }

        phase = BuildPhase::worker_handoff_projection;
        if (authorization.authorizer != CleanupAuthorizerKindV1::merge_commit_worker ||
            authorization.artifact_kind != CleanupArtifactKindV1::worker ||
            authorization.manifest_digest != manifest.self_digest ||
            authorization.authorizer_record_digest != commit.self_digest ||
            authorization.manifest_order_ordinal >= manifest.chunks.size() ||
            authorization.manifest_order_ordinal >= commit.chunks.size()) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::worker_handoff_projection_failed,
                               protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                authorization.manifest_order_ordinal))};
        }
        const auto& chunk = manifest.chunks[authorization.manifest_order_ordinal];
        const auto& summary = commit.chunks[authorization.manifest_order_ordinal].input;
        if (summary.disposition != ChunkDispositionV1::handoff ||
            summary.durable_attempt_count == 0 || summary.chunk_id != chunk.chunk_id ||
            summary.sq_begin != chunk.sq_begin || summary.sq_end != chunk.sq_end) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::worker_handoff_projection_failed,
                               protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                authorization.manifest_order_ordinal))};
        }
        const std::uint32_t attempt_ordinal = summary.durable_attempt_count - 1U;

        phase = BuildPhase::worker_attempt_naming;
        const auto derived = derive_worker_attempt(manifest, authorization.manifest_order_ordinal,
                                                   authorization.lease.relative_stem);
        if (!derived.has_value() || derived->attempt_ordinal != attempt_ordinal) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::worker_attempt_naming_failed,
                               protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                authorization.manifest_order_ordinal))};
        }

        WorkerHandoffV1 projected{
            .manifest_digest = manifest.self_digest,
            .work_digest = manifest.work_sha256,
            .wave_id = manifest.wave_id,
            .chunk_id = summary.chunk_id,
            .sq_begin = summary.sq_begin,
            .sq_end = summary.sq_end,
            .attempt_ordinal = attempt_ordinal,
            .attempt_started_digest = summary.last_attempt_digest,
            .lease = authorization.lease,
            .artifact = authorization.artifact,
            .processed_sq_count = summary.processed_sq_count,
            .next_sq_index = summary.next_sq_index,
            .completion_reason = summary.completion_reason,
            .relation_count = summary.raw_relation_count,
            .cleanup_intent_absent = true,
            .self_digest = authorization.handoff_digest,
        };

        phase = BuildPhase::worker_handoff_validation;
        if (const auto status = validate_distributed_sieve_record(
                DistributedSieveProtocolRecordV1{projected}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }

        phase = BuildPhase::authorization_dependency_validation;
        const std::span<const ConsumptionStartedV1> no_consumption_starts;
        if (const auto status = validate_artifact_cleanup_dependencies(
                manifest, commit, no_consumption_starts, nullptr, nullptr, authorization,
                &projected, nullptr);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::dependency_validation_failed, status)};
        }

        return {
            std::move(projected),
            diagnostic(BuildPhase::complete, BuildStatus::ready),
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, resource_diagnostic(phase)};
    } catch (const std::length_error&) {
        return {std::nullopt, resource_diagnostic(phase)};
    } catch (...) {
        return {std::nullopt,
                diagnostic(phase, BuildStatus::unexpected_failure,
                           protocol_failure(DistributedSieveProtocolError::invalid_value))};
    }
}

DistributedSieveWorkerCleanupAuthorizationBuildResultV1
build_distributed_sieve_worker_cleanup_authorization_v1(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    std::uint32_t manifest_order_ordinal, const WorkerHandoffV1& worker_handoff,
    const relation::OOCPrivateHandoffRecordV1& private_handoff,
    const NativeIdentityV1& base_lock_identity, const NativeIdentityV1& owned_marker_identity,
    const util::durable_immutable_record::RecordSnapshot& private_handoff_snapshot) noexcept {
    BuildPhase phase = BuildPhase::manifest_validation;
    try {
        if (const auto status =
                validate_distributed_sieve_record(DistributedSieveProtocolRecordV1{manifest}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }

        phase = BuildPhase::merge_commit_validation;
        if (const auto status =
                validate_distributed_sieve_record(DistributedSieveProtocolRecordV1{commit}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }

        phase = BuildPhase::worker_handoff_validation;
        if (const auto status = validate_distributed_sieve_record(
                DistributedSieveProtocolRecordV1{worker_handoff}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }

        phase = BuildPhase::worker_attempt_naming;
        const auto derived = derive_worker_attempt(manifest, manifest_order_ordinal,
                                                   worker_handoff.lease.relative_stem);
        if (!derived.has_value() || derived->attempt_ordinal != worker_handoff.attempt_ordinal ||
            manifest_order_ordinal >= commit.chunks.size() ||
            manifest_order_ordinal >= manifest.chunks.size() ||
            worker_handoff.chunk_id != manifest.chunks[manifest_order_ordinal].chunk_id ||
            worker_handoff.sq_begin != manifest.chunks[manifest_order_ordinal].sq_begin ||
            worker_handoff.sq_end != manifest.chunks[manifest_order_ordinal].sq_end ||
            manifest.handoff_version != DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::worker_attempt_naming_failed,
                               protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                manifest_order_ordinal))};
        }

        phase = BuildPhase::private_handoff_validation;
        if (const auto status =
                relation::validate_ooc_private_handoff_record(private_handoff, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::private_handoff_validation_failed, {}, status)};
        }
        auto private_handoff_bytes = relation::encode_ooc_private_handoff_record(private_handoff);
        if (!private_handoff_bytes || !private_handoff_bytes.bytes.has_value()) {
            return {std::nullopt, diagnostic(phase, BuildStatus::record_encoding_failed, {},
                                             private_handoff_bytes.status)};
        }
        const auto snapshot_identity = protocol_identity(private_handoff_snapshot.identity);
        if (!protocol_identity_is_valid(snapshot_identity) || private_handoff_snapshot.size == 0 ||
            private_handoff_snapshot.size != private_handoff_bytes.bytes->size()) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::private_handoff_snapshot_mismatch,
                               protocol_failure(DistributedSieveProtocolError::invalid_value))};
        }

        phase = BuildPhase::private_handoff_bridge_validation;
        if (!protocol_identity_is_valid(base_lock_identity) ||
            !protocol_identity_is_valid(owned_marker_identity) ||
            !private_handoff_matches_worker(manifest, worker_handoff, private_handoff,
                                            base_lock_identity, owned_marker_identity)) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::private_handoff_bridge_mismatch,
                               protocol_failure(DistributedSieveProtocolError::invalid_value))};
        }
        auto encoded_worker =
            encode_distributed_sieve_record(DistributedSieveProtocolRecordV1{worker_handoff});
        if (!encoded_worker || !encoded_worker.bytes.has_value()) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::record_encoding_failed, encoded_worker.status)};
        }
        if (*encoded_worker.bytes != private_handoff.opaque_payload) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::private_handoff_bridge_mismatch,
                               protocol_failure(DistributedSieveProtocolError::digest_mismatch))};
        }

        DistributedSieveProtocolRecordV1 sealed = ArtifactCleanupAuthorizedV1{
            .authorizer = CleanupAuthorizerKindV1::merge_commit_worker,
            .manifest_digest = manifest.self_digest,
            .authorizer_record_digest = commit.self_digest,
            .artifact_kind = CleanupArtifactKindV1::worker,
            .manifest_order_ordinal = manifest_order_ordinal,
            .lease = worker_handoff.lease,
            .base_lock_identity = base_lock_identity,
            .owned_marker_identity = owned_marker_identity,
            .handoff_digest = worker_handoff.self_digest,
            .private_handoff_digest = private_handoff.self_digest,
            .private_handoff_record =
                {
                    .identity = snapshot_identity,
                    .extent = private_handoff_snapshot.size,
                },
            .artifact = worker_handoff.artifact,
            .self_digest = {},
        };

        phase = BuildPhase::authorization_sealing;
        if (const auto status = seal_distributed_sieve_record(sealed); !status) {
            return {std::nullopt, diagnostic(phase, BuildStatus::record_sealing_failed, status)};
        }
        auto encoded_authorization = encode_distributed_sieve_record(sealed);
        if (!encoded_authorization || !encoded_authorization.bytes.has_value()) {
            return {std::nullopt, diagnostic(phase, BuildStatus::record_encoding_failed,
                                             encoded_authorization.status)};
        }

        phase = BuildPhase::authorization_round_trip;
        auto decoded_authorization = decode_distributed_sieve_record(*encoded_authorization.bytes);
        if (!decoded_authorization || !decoded_authorization.value.has_value()) {
            return {std::nullopt, diagnostic(phase, BuildStatus::round_trip_failed,
                                             decoded_authorization.status)};
        }
        const auto* authorization =
            std::get_if<ArtifactCleanupAuthorizedV1>(&*decoded_authorization.value);
        if (authorization == nullptr ||
            !authorization_matches_source(*authorization, manifest, commit, manifest_order_ordinal,
                                          worker_handoff, private_handoff, base_lock_identity,
                                          owned_marker_identity, private_handoff_snapshot)) {
            return {
                std::nullopt,
                diagnostic(phase, BuildStatus::round_trip_failed,
                           protocol_failure(DistributedSieveProtocolError::record_type_mismatch))};
        }
        auto reencoded_authorization =
            encode_distributed_sieve_record(*decoded_authorization.value);
        if (!reencoded_authorization || !reencoded_authorization.bytes.has_value() ||
            *reencoded_authorization.bytes != *encoded_authorization.bytes) {
            const auto status =
                reencoded_authorization.status
                    ? protocol_failure(DistributedSieveProtocolError::digest_mismatch)
                    : reencoded_authorization.status;
            return {std::nullopt, diagnostic(phase, BuildStatus::round_trip_failed, status)};
        }

        phase = BuildPhase::authorization_dependency_validation;
        const std::span<const ConsumptionStartedV1> no_consumption_starts;
        if (const auto status = validate_artifact_cleanup_dependencies(
                manifest, commit, no_consumption_starts, nullptr, nullptr, *authorization,
                &worker_handoff, nullptr);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::dependency_validation_failed, status)};
        }

        ArtifactCleanupAuthorizedV1 typed =
            std::get<ArtifactCleanupAuthorizedV1>(std::move(*decoded_authorization.value));
        return {
            DistributedSieveWorkerCleanupAuthorizationV1{
                .record = std::move(typed),
                .canonical_bytes = std::move(*encoded_authorization.bytes),
            },
            diagnostic(BuildPhase::complete, BuildStatus::ready),
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, resource_diagnostic(phase)};
    } catch (const std::length_error&) {
        return {std::nullopt, resource_diagnostic(phase)};
    } catch (...) {
        return {std::nullopt,
                diagnostic(phase, BuildStatus::unexpected_failure,
                           protocol_failure(DistributedSieveProtocolError::invalid_value))};
    }
}

DistributedSieveWorkerCleanupCompletionBuildResultV1
build_distributed_sieve_worker_cleanup_completion_v1(
    const std::filesystem::path& absolute_root, const WaveManifestV1& manifest,
    const ArtifactCleanupAuthorizedV1& authorization,
    const relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAbsenceEvidenceV2&
        absence_evidence) noexcept {
    BuildPhase phase = BuildPhase::manifest_validation;
    try {
        if (const auto status =
                validate_distributed_sieve_record(DistributedSieveProtocolRecordV1{manifest}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }
        phase = BuildPhase::authorization_validation;
        if (const auto status = validate_distributed_sieve_record(
                DistributedSieveProtocolRecordV1{authorization}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }

        const bool exact_worker_authorization =
            authorization.authorizer == CleanupAuthorizerKindV1::merge_commit_worker &&
            authorization.artifact_kind == CleanupArtifactKindV1::worker &&
            authorization.manifest_digest == manifest.self_digest &&
            authorization.manifest_order_ordinal < manifest.chunks.size();
        if (!exact_worker_authorization) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::absence_evidence_mismatch,
                               protocol_failure(DistributedSieveProtocolError::invalid_value))};
        }

        auto relation_binding = build_distributed_sieve_worker_cleanup_relation_binding_v1(
            absolute_root, manifest, authorization);
        if (!relation_binding || !relation_binding.relation_binding.has_value()) {
            return {std::nullopt, relation_binding.diagnostic};
        }
        const auto expected_base_path_digest =
            frozen_path_digest(relation_binding.relation_binding->binding.base_path);
        if (!expected_base_path_digest.has_value()) {
            return {std::nullopt, resource_diagnostic(BuildPhase::absence_evidence_validation)};
        }

        phase = BuildPhase::absence_evidence_validation;
        const bool exact_evidence =
            absence_evidence.base_path_digest == *expected_base_path_digest &&
            absence_evidence.external_authorization_digest == authorization.self_digest &&
            absence_evidence.lease_id == authorization.lease.lease_id.limbs &&
            absence_evidence.parent_directory_identity ==
                relation_identity(manifest.wave_root_identity) &&
            absence_evidence.parent_directory_durability_confirmed &&
            absence_evidence.expected_namespace_absent;
        if (!exact_evidence) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::absence_evidence_mismatch,
                               protocol_failure(DistributedSieveProtocolError::invalid_value))};
        }

        std::optional<NativeIdentityV1> cleanup_intent_identity;
        if (absence_evidence.cleanup_intent_snapshot.has_value()) {
            const auto& snapshot = *absence_evidence.cleanup_intent_snapshot;
            const auto identity = protocol_identity(snapshot.identity);
            if (!protocol_identity_is_valid(identity) ||
                snapshot.size != relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 ||
                manifest_control_contains_identity(manifest, identity)) {
                return {std::nullopt,
                        diagnostic(phase, BuildStatus::absence_evidence_mismatch,
                                   protocol_failure(DistributedSieveProtocolError::invalid_value))};
            }
            cleanup_intent_identity = identity;
        }

        DistributedSieveProtocolRecordV1 sealed = ArtifactCleanupCompletedV1{
            .authorization_digest = authorization.self_digest,
            .cleanup_intent_identity = cleanup_intent_identity,
            .parent_directory_durability_confirmed = true,
            .expected_namespace_absent = true,
            .self_digest = {},
        };

        phase = BuildPhase::completion_sealing;
        if (const auto status = seal_distributed_sieve_record(sealed); !status) {
            return {std::nullopt, diagnostic(phase, BuildStatus::record_sealing_failed, status)};
        }
        auto encoded_completion = encode_distributed_sieve_record(sealed);
        if (!encoded_completion || !encoded_completion.bytes.has_value()) {
            return {std::nullopt, diagnostic(phase, BuildStatus::record_encoding_failed,
                                             encoded_completion.status)};
        }

        phase = BuildPhase::completion_round_trip;
        auto decoded_completion = decode_distributed_sieve_record(*encoded_completion.bytes);
        if (!decoded_completion || !decoded_completion.value.has_value()) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::round_trip_failed, decoded_completion.status)};
        }
        const auto* completion =
            std::get_if<ArtifactCleanupCompletedV1>(&*decoded_completion.value);
        if (completion == nullptr ||
            !completion_matches_evidence(*completion, authorization, absence_evidence)) {
            return {
                std::nullopt,
                diagnostic(phase, BuildStatus::round_trip_failed,
                           protocol_failure(DistributedSieveProtocolError::record_type_mismatch))};
        }
        auto reencoded_completion = encode_distributed_sieve_record(*decoded_completion.value);
        if (!reencoded_completion || !reencoded_completion.bytes.has_value() ||
            *reencoded_completion.bytes != *encoded_completion.bytes) {
            const auto status =
                reencoded_completion.status
                    ? protocol_failure(DistributedSieveProtocolError::digest_mismatch)
                    : reencoded_completion.status;
            return {std::nullopt, diagnostic(phase, BuildStatus::round_trip_failed, status)};
        }

        phase = BuildPhase::completion_dependency_validation;
        if (const auto status =
                validate_artifact_cleanup_completion_dependency(authorization, *completion);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::dependency_validation_failed, status)};
        }

        ArtifactCleanupCompletedV1 typed =
            std::get<ArtifactCleanupCompletedV1>(std::move(*decoded_completion.value));
        return {
            DistributedSieveWorkerCleanupCompletionV1{
                .record = std::move(typed),
                .canonical_bytes = std::move(*encoded_completion.bytes),
            },
            diagnostic(BuildPhase::complete, BuildStatus::ready),
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, resource_diagnostic(phase)};
    } catch (const std::length_error&) {
        return {std::nullopt, resource_diagnostic(phase)};
    } catch (...) {
        return {std::nullopt,
                diagnostic(phase, BuildStatus::unexpected_failure,
                           protocol_failure(DistributedSieveProtocolError::invalid_value))};
    }
}

DistributedSieveWorkerCleanupRelationBindingBuildResultV1
build_distributed_sieve_worker_cleanup_relation_binding_v1(
    const std::filesystem::path& absolute_root, const WaveManifestV1& manifest,
    const ArtifactCleanupAuthorizedV1& authorization) noexcept {
    BuildPhase phase = BuildPhase::absolute_root_validation;
    try {
        if (!path_is_exact_absolute_root(absolute_root)) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::absolute_root_invalid,
                               protocol_failure(DistributedSieveProtocolError::invalid_value))};
        }

        phase = BuildPhase::manifest_validation;
        if (const auto status =
                validate_distributed_sieve_record(DistributedSieveProtocolRecordV1{manifest}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }
        phase = BuildPhase::authorization_validation;
        if (const auto status = validate_distributed_sieve_record(
                DistributedSieveProtocolRecordV1{authorization}, true);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::protocol_validation_failed, status)};
        }
        if (authorization.authorizer != CleanupAuthorizerKindV1::merge_commit_worker ||
            authorization.artifact_kind != CleanupArtifactKindV1::worker ||
            authorization.manifest_digest != manifest.self_digest ||
            authorization_contains_manifest_control_identity(manifest, authorization)) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::input_invalid,
                               protocol_failure(DistributedSieveProtocolError::invalid_value))};
        }

        phase = BuildPhase::worker_attempt_naming;
        auto derived = derive_worker_attempt(manifest, authorization.manifest_order_ordinal,
                                             authorization.lease.relative_stem);
        if (!derived.has_value()) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::worker_attempt_naming_failed,
                               protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                authorization.manifest_order_ordinal))};
        }

        phase = BuildPhase::relation_binding_projection;
        auto base_path = absolute_root / derived->names.private_directory_leaf / "corpus";
        if (!base_path.is_absolute() || base_path.lexically_normal() != base_path ||
            base_path.parent_path().parent_path() != absolute_root ||
            base_path.parent_path().filename() != derived->names.private_directory_leaf ||
            base_path.filename() != "corpus") {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::absolute_root_invalid,
                               protocol_failure(DistributedSieveProtocolError::invalid_value))};
        }

        RelationBinding binding{
            .base_path = base_path,
            .external_authorization_digest = authorization.self_digest,
            .generic_handoff_self_digest = authorization.private_handoff_digest,
            .lease_id = authorization.lease.lease_id.limbs,
            .parent_directory_identity = relation_identity(manifest.wave_root_identity),
            .lock_identity = relation_identity(authorization.base_lock_identity),
            .directory_identity = relation_identity(authorization.lease.directory),
            .owner_marker_identity = relation_identity(authorization.lease.owner_marker),
            .owned_marker_identity = relation_identity(authorization.owned_marker_identity),
            .pair = relation_pair(authorization.artifact),
            .handoff = relation_artifact(authorization.private_handoff_record),
            .index = relation_artifact(authorization.artifact.index_file),
            .data = relation_artifact(authorization.artifact.data_file),
        };
        if (!binding_matches_authorization(binding, base_path, manifest, authorization)) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::relation_binding_invalid,
                               protocol_failure(DistributedSieveProtocolError::invalid_value))};
        }

        phase = BuildPhase::relation_binding_validation;
        const auto base_path_digest = frozen_path_digest(base_path);
        if (!base_path_digest.has_value()) {
            return {
                std::nullopt,
                diagnostic(phase, BuildStatus::resource_exhausted,
                           protocol_failure(DistributedSieveProtocolError::digest_unavailable))};
        }
        relation::OOCAuthorizedCleanupIntentV2 structural_probe{
            .schema_version = relation::OOC_AUTHORIZED_CLEANUP_INTENT_SCHEMA_VERSION_V2,
            .platform_id = relation::OOC_AUTHORIZED_CLEANUP_INTENT_CURRENT_PLATFORM_V1,
            .marker_kind = relation::OOCAuthorizedCleanupMarkerKindV2::intent,
            .base_path_digest = *base_path_digest,
            .external_authorization_digest = binding.external_authorization_digest,
            .generic_handoff_self_digest = binding.generic_handoff_self_digest,
            .lease_id = binding.lease_id,
            .parent_directory_identity = binding.parent_directory_identity,
            .lock_identity = binding.lock_identity,
            .directory_identity = binding.directory_identity,
            .owner_marker_identity = binding.owner_marker_identity,
            .owned_marker_identity = binding.owned_marker_identity,
            .pair = binding.pair,
            .handoff = binding.handoff,
            .pending_handoff = std::nullopt,
            .index = binding.index,
            .data = binding.data,
            .self_digest = {},
        };
        if (const auto status = relation::seal_ooc_authorized_cleanup_intent(structural_probe);
            !status) {
            return {std::nullopt,
                    diagnostic(phase, BuildStatus::relation_binding_invalid, {}, {}, status)};
        }

        return {
            DistributedSieveWorkerCleanupRelationBindingV1{
                .attempt_names = std::move(derived->names),
                .binding = std::move(binding),
            },
            diagnostic(BuildPhase::complete, BuildStatus::ready),
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, resource_diagnostic(phase)};
    } catch (const std::length_error&) {
        return {std::nullopt, resource_diagnostic(phase)};
    } catch (...) {
        return {std::nullopt,
                diagnostic(phase, BuildStatus::unexpected_failure,
                           protocol_failure(DistributedSieveProtocolError::invalid_value))};
    }
}

} // namespace gnfs::sieve::distributed_sieve_worker_cleanup_codec_detail
