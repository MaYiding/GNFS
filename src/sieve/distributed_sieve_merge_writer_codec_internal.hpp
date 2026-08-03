#pragma once

// Source-private, authority-free codec for one MergePreparedV1 payload. The
// builder accepts only immutable protocol values and finalized corpus evidence;
// paths, native handles, writers, lease receipts, and cleanup authority remain
// unreachable.

#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace gnfs::relation {
struct OOCFinalizedCorpusEvidenceV1;
}

namespace gnfs::sieve::distributed_sieve_merge_writer_codec_detail {

enum class DistributedSieveMergePreparedPayloadBuildPhaseV1 : std::uint8_t {
    request_validation,
    merge_chain_validation,
    count_validation,
    evidence_projection,
    record_sealing,
    record_encoding,
    round_trip_decoding,
    round_trip_validation,
    predecessor_chain_validation,
    complete,
};

enum class DistributedSieveMergePreparedPayloadBuildStatusV1 : std::uint8_t {
    ready,
    empty_merge_chain,
    merge_chain_invalid,
    count_overflow,
    count_conservation_invalid,
    per_chunk_counts_invalid,
    corpus_evidence_invalid,
    record_sealing_failed,
    record_encoding_failed,
    round_trip_decoding_failed,
    round_trip_type_mismatch,
    round_trip_mismatch,
    predecessor_chain_invalid,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_merge_prepared_payload_build_status_name(
    DistributedSieveMergePreparedPayloadBuildStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveMergePreparedPayloadBuildStatusV1::ready:
        return "ready";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::empty_merge_chain:
        return "empty_merge_chain";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::merge_chain_invalid:
        return "merge_chain_invalid";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::count_overflow:
        return "count_overflow";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::count_conservation_invalid:
        return "count_conservation_invalid";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::per_chunk_counts_invalid:
        return "per_chunk_counts_invalid";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::corpus_evidence_invalid:
        return "corpus_evidence_invalid";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::record_sealing_failed:
        return "record_sealing_failed";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::record_encoding_failed:
        return "record_encoding_failed";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::round_trip_decoding_failed:
        return "round_trip_decoding_failed";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::round_trip_type_mismatch:
        return "round_trip_type_mismatch";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::round_trip_mismatch:
        return "round_trip_mismatch";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::predecessor_chain_invalid:
        return "predecessor_chain_invalid";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveMergePreparedPayloadBuildStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveMergePreparedPayloadBuildDiagnosticV1 final {
    DistributedSieveMergePreparedPayloadBuildPhaseV1 phase =
        DistributedSieveMergePreparedPayloadBuildPhaseV1::request_validation;
    DistributedSieveMergePreparedPayloadBuildStatusV1 status =
        DistributedSieveMergePreparedPayloadBuildStatusV1::unexpected_failure;
    DistributedSieveProtocolStatus protocol;

    [[nodiscard]] explicit operator bool() const noexcept {
        return phase == DistributedSieveMergePreparedPayloadBuildPhaseV1::complete &&
               status == DistributedSieveMergePreparedPayloadBuildStatusV1::ready &&
               static_cast<bool>(protocol);
    }
};

struct DistributedSieveMergePreparedPayloadV1 final {
    MergePreparedV1 record;
    std::vector<std::byte> opaque_payload;
};

struct DistributedSieveMergePreparedPayloadBuildResultV1 final {
    std::optional<DistributedSieveMergePreparedPayloadV1> prepared;
    DistributedSieveMergePreparedPayloadBuildDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return prepared.has_value() && static_cast<bool>(diagnostic);
    }
};

/// Build the canonical payload for the latest entry in one complete, sealed
/// MergeStartedV1 predecessor chain. The function is authority-free and
/// fail-closed: every error is returned as a diagnostic and no exception
/// escapes.
[[nodiscard]] DistributedSieveMergePreparedPayloadBuildResultV1
build_distributed_sieve_merge_prepared_payload_v1(
    const WaveManifestV1& manifest, std::span<const MergeStartedV1> merge_started_chain,
    std::uint64_t input_relation_count, std::uint64_t duplicate_relation_count,
    std::uint64_t output_relation_count,
    std::span<const PerChunkRetainedCountV1> per_chunk_retained_counts,
    const relation::OOCFinalizedCorpusEvidenceV1& evidence) noexcept;

} // namespace gnfs::sieve::distributed_sieve_merge_writer_codec_detail
