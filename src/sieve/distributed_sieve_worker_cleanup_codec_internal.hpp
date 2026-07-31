#pragma once

// Source-private, authority-free builders for the durable worker-cleanup
// records and their exact relation-layer binding. These values perform no I/O
// and grant no publication, namespace, lock, adoption, or cleanup authority.

#include "../relation/ooc_private_handoff_cleanup_authorization_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"

#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>
#include <gnfs/relation/ooc_durable_handoff.hpp>
#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/durable_immutable_record.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace gnfs::sieve::distributed_sieve_worker_cleanup_codec_detail {

enum class DistributedSieveWorkerCleanupCodecPhaseV1 : std::uint8_t {
    manifest_validation,
    merge_commit_validation,
    worker_handoff_projection,
    worker_handoff_validation,
    worker_attempt_naming,
    private_handoff_validation,
    private_handoff_bridge_validation,
    authorization_sealing,
    authorization_round_trip,
    authorization_dependency_validation,
    authorization_validation,
    absence_evidence_validation,
    completion_sealing,
    completion_round_trip,
    completion_dependency_validation,
    absolute_root_validation,
    relation_binding_projection,
    relation_binding_validation,
    complete,
};

enum class DistributedSieveWorkerCleanupCodecStatusV1 : std::uint8_t {
    ready,
    input_invalid,
    protocol_validation_failed,
    private_handoff_validation_failed,
    private_handoff_bridge_mismatch,
    private_handoff_snapshot_mismatch,
    worker_attempt_naming_failed,
    worker_handoff_projection_failed,
    record_sealing_failed,
    record_encoding_failed,
    round_trip_failed,
    dependency_validation_failed,
    absence_evidence_mismatch,
    absolute_root_invalid,
    relation_binding_invalid,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_worker_cleanup_codec_status_name(
    DistributedSieveWorkerCleanupCodecStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerCleanupCodecStatusV1::ready:
        return "ready";
    case DistributedSieveWorkerCleanupCodecStatusV1::input_invalid:
        return "input_invalid";
    case DistributedSieveWorkerCleanupCodecStatusV1::protocol_validation_failed:
        return "protocol_validation_failed";
    case DistributedSieveWorkerCleanupCodecStatusV1::private_handoff_validation_failed:
        return "private_handoff_validation_failed";
    case DistributedSieveWorkerCleanupCodecStatusV1::private_handoff_bridge_mismatch:
        return "private_handoff_bridge_mismatch";
    case DistributedSieveWorkerCleanupCodecStatusV1::private_handoff_snapshot_mismatch:
        return "private_handoff_snapshot_mismatch";
    case DistributedSieveWorkerCleanupCodecStatusV1::worker_attempt_naming_failed:
        return "worker_attempt_naming_failed";
    case DistributedSieveWorkerCleanupCodecStatusV1::worker_handoff_projection_failed:
        return "worker_handoff_projection_failed";
    case DistributedSieveWorkerCleanupCodecStatusV1::record_sealing_failed:
        return "record_sealing_failed";
    case DistributedSieveWorkerCleanupCodecStatusV1::record_encoding_failed:
        return "record_encoding_failed";
    case DistributedSieveWorkerCleanupCodecStatusV1::round_trip_failed:
        return "round_trip_failed";
    case DistributedSieveWorkerCleanupCodecStatusV1::dependency_validation_failed:
        return "dependency_validation_failed";
    case DistributedSieveWorkerCleanupCodecStatusV1::absence_evidence_mismatch:
        return "absence_evidence_mismatch";
    case DistributedSieveWorkerCleanupCodecStatusV1::absolute_root_invalid:
        return "absolute_root_invalid";
    case DistributedSieveWorkerCleanupCodecStatusV1::relation_binding_invalid:
        return "relation_binding_invalid";
    case DistributedSieveWorkerCleanupCodecStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerCleanupCodecStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveWorkerCleanupCodecDiagnosticV1 final {
    DistributedSieveWorkerCleanupCodecPhaseV1 phase =
        DistributedSieveWorkerCleanupCodecPhaseV1::manifest_validation;
    DistributedSieveWorkerCleanupCodecStatusV1 status =
        DistributedSieveWorkerCleanupCodecStatusV1::unexpected_failure;
    DistributedSieveProtocolStatus protocol;
    relation::OOCPrivateHandoffProtocolStatus private_handoff;
    relation::OOCAuthorizedCleanupIntentProtocolStatus cleanup_intent;

    [[nodiscard]] explicit operator bool() const noexcept {
        return phase == DistributedSieveWorkerCleanupCodecPhaseV1::complete &&
               status == DistributedSieveWorkerCleanupCodecStatusV1::ready &&
               static_cast<bool>(protocol) && static_cast<bool>(private_handoff) &&
               static_cast<bool>(cleanup_intent);
    }
};

struct DistributedSieveWorkerCleanupAuthorizationV1 final {
    ArtifactCleanupAuthorizedV1 record;
    std::vector<std::byte> canonical_bytes;
};

struct DistributedSieveWorkerCleanupHandoffProjectionBuildResultV1 final {
    std::optional<WorkerHandoffV1> worker_handoff;
    DistributedSieveWorkerCleanupCodecDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return worker_handoff.has_value() && static_cast<bool>(diagnostic);
    }
};

struct DistributedSieveWorkerCleanupAuthorizationBuildResultV1 final {
    std::optional<DistributedSieveWorkerCleanupAuthorizationV1> authorization;
    DistributedSieveWorkerCleanupCodecDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return authorization.has_value() && static_cast<bool>(diagnostic);
    }
};

struct DistributedSieveWorkerCleanupCompletionV1 final {
    ArtifactCleanupCompletedV1 record;
    std::vector<std::byte> canonical_bytes;
};

struct DistributedSieveWorkerCleanupCompletionBuildResultV1 final {
    std::optional<DistributedSieveWorkerCleanupCompletionV1> completion;
    DistributedSieveWorkerCleanupCodecDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return completion.has_value() && static_cast<bool>(diagnostic);
    }
};

struct DistributedSieveWorkerCleanupRelationBindingV1 final {
    distributed_sieve_resume_detail::DistributedSieveWorkerAttemptNamesV1 attempt_names;
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding binding;
};

struct DistributedSieveWorkerCleanupRelationBindingBuildResultV1 final {
    std::optional<DistributedSieveWorkerCleanupRelationBindingV1> relation_binding;
    DistributedSieveWorkerCleanupCodecDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return relation_binding.has_value() && static_cast<bool>(diagnostic);
    }
};

/// Reconstruct and validate the sole exact worker handoff admitted by a
/// manifest, merge commit, and retained cleanup authorization. This is a pure
/// protocol projection and performs no record or namespace I/O.
[[nodiscard]] DistributedSieveWorkerCleanupHandoffProjectionBuildResultV1
project_distributed_sieve_worker_cleanup_handoff_v1(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    const ArtifactCleanupAuthorizedV1& authorization) noexcept;

/// Build one canonical worker cleanup authorization from an exact manifest,
/// committed manifest-order projection, sealed worker payload, and its sealed
/// generic private-handoff envelope plus immutable-leaf snapshot.
[[nodiscard]] DistributedSieveWorkerCleanupAuthorizationBuildResultV1
build_distributed_sieve_worker_cleanup_authorization_v1(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    std::uint32_t manifest_order_ordinal, const WorkerHandoffV1& worker_handoff,
    const relation::OOCPrivateHandoffRecordV1& private_handoff,
    const NativeIdentityV1& base_lock_identity, const NativeIdentityV1& owned_marker_identity,
    const util::durable_immutable_record::RecordSnapshot& private_handoff_snapshot) noexcept;

/// Build one canonical completion only from a retained exact authorization,
/// its uniquely derived absolute relation binding, and relation-produced,
/// parent-durable namespace-absence evidence.
[[nodiscard]] DistributedSieveWorkerCleanupCompletionBuildResultV1
build_distributed_sieve_worker_cleanup_completion_v1(
    const std::filesystem::path& absolute_root, const WaveManifestV1& manifest,
    const ArtifactCleanupAuthorizedV1& authorization,
    const relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAbsenceEvidenceV2&
        absence_evidence) noexcept;

/// Derive the sole worker-attempt namespace admitted by the manifest and
/// authorization, then project the exact authority-free relation binding.
[[nodiscard]] DistributedSieveWorkerCleanupRelationBindingBuildResultV1
build_distributed_sieve_worker_cleanup_relation_binding_v1(
    const std::filesystem::path& absolute_root, const WaveManifestV1& manifest,
    const ArtifactCleanupAuthorizedV1& authorization) noexcept;

} // namespace gnfs::sieve::distributed_sieve_worker_cleanup_codec_detail
