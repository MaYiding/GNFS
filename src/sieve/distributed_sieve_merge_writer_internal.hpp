#pragma once

// Source-private, authority-free streaming of manifest-ordered worker corpora
// into one caller-owned merged OOC writer. The caller retains every adopted
// reader and the writer for the complete call. This layer never finalizes,
// publishes, cleans up, retries, or reopens an artifact path.

#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace gnfs::relation {
class OOCRelationReader;
class OOCRelationWriter;
} // namespace gnfs::relation

namespace gnfs::sieve::distributed_sieve_merge_writer_detail {

inline constexpr std::size_t DISTRIBUTED_SIEVE_MERGE_WRITER_NO_INPUT_SLOT =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::uint64_t DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL =
    std::numeric_limits<std::uint64_t>::max();

enum class DistributedSieveMergeWriterPhaseV1 : std::uint8_t {
    request_validation,
    merge_chain_validation,
    input_validation,
    input_read,
    output_write,
    count_validation,
    complete,
};

enum class DistributedSieveMergeWriterStatusV1 : std::uint8_t {
    ready,
    empty_merge_chain,
    merge_chain_invalid,
    unsupported_merge_policy,
    unsupported_semantic_version,
    input_count_overflow,
    input_reader_invalid,
    input_corpus_count_mismatch,
    input_corpus_receipt_mismatch,
    output_writer_invalid,
    input_read_failed,
    output_write_failed,
    count_conservation_invalid,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_merge_writer_status_name(DistributedSieveMergeWriterStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveMergeWriterStatusV1::ready:
        return "ready";
    case DistributedSieveMergeWriterStatusV1::empty_merge_chain:
        return "empty_merge_chain";
    case DistributedSieveMergeWriterStatusV1::merge_chain_invalid:
        return "merge_chain_invalid";
    case DistributedSieveMergeWriterStatusV1::unsupported_merge_policy:
        return "unsupported_merge_policy";
    case DistributedSieveMergeWriterStatusV1::unsupported_semantic_version:
        return "unsupported_semantic_version";
    case DistributedSieveMergeWriterStatusV1::input_count_overflow:
        return "input_count_overflow";
    case DistributedSieveMergeWriterStatusV1::input_reader_invalid:
        return "input_reader_invalid";
    case DistributedSieveMergeWriterStatusV1::input_corpus_count_mismatch:
        return "input_corpus_count_mismatch";
    case DistributedSieveMergeWriterStatusV1::input_corpus_receipt_mismatch:
        return "input_corpus_receipt_mismatch";
    case DistributedSieveMergeWriterStatusV1::output_writer_invalid:
        return "output_writer_invalid";
    case DistributedSieveMergeWriterStatusV1::input_read_failed:
        return "input_read_failed";
    case DistributedSieveMergeWriterStatusV1::output_write_failed:
        return "output_write_failed";
    case DistributedSieveMergeWriterStatusV1::count_conservation_invalid:
        return "count_conservation_invalid";
    case DistributedSieveMergeWriterStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveMergeWriterStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveMergeWriterDiagnosticV1 final {
    DistributedSieveMergeWriterPhaseV1 phase =
        DistributedSieveMergeWriterPhaseV1::request_validation;
    DistributedSieveMergeWriterStatusV1 status =
        DistributedSieveMergeWriterStatusV1::unexpected_failure;
    DistributedSieveProtocolStatus protocol;
    std::size_t input_slot = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_INPUT_SLOT;
    std::uint64_t relation_ordinal = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL;
};

struct DistributedSieveMergeWriterReceiptV1 final {
    std::uint64_t input_relation_count = 0;
    std::uint64_t duplicate_relation_count = 0;
    std::uint64_t output_relation_count = 0;
    std::vector<PerChunkRetainedCountV1> per_chunk_retained_counts;
};

struct DistributedSieveMergeWriterResultV1 final {
    std::optional<DistributedSieveMergeWriterReceiptV1> receipt;
    DistributedSieveMergeWriterDiagnosticV1 diagnostic;
    bool artifacts_may_remain = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return receipt.has_value() &&
               diagnostic.phase == DistributedSieveMergeWriterPhaseV1::complete &&
               diagnostic.status == DistributedSieveMergeWriterStatusV1::ready &&
               static_cast<bool>(diagnostic.protocol);
    }
};

/// Stream every terminal input in manifest chunk order and each reader's
/// original relation order. Exact full-width `(a,b)` is the sole raw-row
/// identity. The first occurrence writes its complete Relation payload
/// unchanged; later occurrences contribute only to the duplicate count.
/// Before advancing past a handoff input, its streamed sequence receipt and
/// corpus SHA-256 must match the authenticated manifest-ordered terminal
/// projection. This binds the actual reader content to that terminal input.
///
/// Heap growth is limited to the exact ABPair set, one decoded Relation, and
/// O(chunks) retained counts. On failure the function does not finalize,
/// publish, clean up, or retry. The caller must roll back or reconcile the
/// caller-owned writer. For the entire call, every reader owner and adoption
/// root must remain alive, each reader must remain at the supplied address and
/// must not be moved, and the caller must grant exclusive access to the writer.
[[nodiscard]] DistributedSieveMergeWriterResultV1 stream_distributed_sieve_merge_inputs_v1(
    const WaveManifestV1& manifest, std::span<const MergeStartedV1> merge_started_chain,
    std::span<const relation::OOCRelationReader* const> input_readers,
    relation::OOCRelationWriter& output_writer) noexcept;

namespace trusted_test {

struct DistributedSieveMergeWriterTestHooksV1 final {
    void (*after_output_write)(std::size_t input_slot, std::uint64_t relation_ordinal,
                               void* context) noexcept = nullptr;
    void* context = nullptr;
};

[[nodiscard]] DistributedSieveMergeWriterResultV1
stream_distributed_sieve_merge_inputs_v1_with_hooks(
    const WaveManifestV1& manifest, std::span<const MergeStartedV1> merge_started_chain,
    std::span<const relation::OOCRelationReader* const> input_readers,
    relation::OOCRelationWriter& output_writer,
    DistributedSieveMergeWriterTestHooksV1 hooks) noexcept;

} // namespace trusted_test

} // namespace gnfs::sieve::distributed_sieve_merge_writer_detail
