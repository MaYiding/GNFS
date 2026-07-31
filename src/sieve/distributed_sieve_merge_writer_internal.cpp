#include "distributed_sieve_merge_writer_internal.hpp"

#include "distributed_sieve_bound_work_internal.hpp"

#include <gnfs/core/relation.hpp>
#include <gnfs/core/types.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/relation/relation_corpus_sha256.hpp>
#include <gnfs/relation/relation_sequence_receipt.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace gnfs::sieve::distributed_sieve_merge_writer_detail {
namespace {

using MergePhase = DistributedSieveMergeWriterPhaseV1;
using MergeReceipt = DistributedSieveMergeWriterReceiptV1;
using MergeResult = DistributedSieveMergeWriterResultV1;
using MergeStatus = DistributedSieveMergeWriterStatusV1;

inline constexpr std::uint32_t MERGE_INPUT_RECEIPT_VERSION_V1 = 1;

[[nodiscard]] constexpr DistributedSieveProtocolStatus
protocol_failure(DistributedSieveProtocolError error,
                 std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {
        .error = error,
        .byte_offset = DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
        .element_index = element_index,
    };
}

[[nodiscard]] MergeResult failure(
    MergePhase phase, MergeStatus status, bool artifacts_may_remain,
    DistributedSieveProtocolStatus protocol =
        protocol_failure(DistributedSieveProtocolError::invalid_value),
    std::size_t input_slot = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_INPUT_SLOT,
    std::uint64_t relation_ordinal = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL) noexcept {
    return {
        .receipt = std::nullopt,
        .diagnostic =
            {
                .phase = phase,
                .status = status,
                .protocol = protocol,
                .input_slot = input_slot,
                .relation_ordinal = relation_ordinal,
            },
        .artifacts_may_remain = artifacts_may_remain,
    };
}

[[nodiscard]] MergeResult resource_failure(
    MergePhase phase, bool artifacts_may_remain,
    std::size_t input_slot = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_INPUT_SLOT,
    std::uint64_t relation_ordinal = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL) noexcept {
    return failure(phase, MergeStatus::resource_exhausted, artifacts_may_remain,
                   protocol_failure(DistributedSieveProtocolError::resource_exhausted), input_slot,
                   relation_ordinal);
}

[[nodiscard]] constexpr bool checked_add(std::uint64_t& accumulator, std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - accumulator) {
        return false;
    }
    accumulator += value;
    return true;
}

[[nodiscard]] constexpr bool checked_increment(std::uint64_t& value) noexcept {
    return checked_add(value, 1);
}

} // namespace

[[nodiscard]] DistributedSieveMergeWriterResultV1 stream_distributed_sieve_merge_inputs_impl_v1(
    const WaveManifestV1& manifest, std::span<const MergeStartedV1> merge_started_chain,
    std::span<const relation::OOCRelationReader* const> input_readers,
    relation::OOCRelationWriter& output_writer,
    trusted_test::DistributedSieveMergeWriterTestHooksV1 hooks) noexcept {
    MergePhase phase = MergePhase::request_validation;
    std::size_t input_slot = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_INPUT_SLOT;
    std::uint64_t relation_ordinal = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL;
    bool artifacts_may_remain = false;

    try {
        if (output_writer.state() != relation::OOCWriterState::Open || output_writer.count() != 0) {
            return failure(phase, MergeStatus::output_writer_invalid, true);
        }
        if (merge_started_chain.empty()) {
            return failure(phase, MergeStatus::empty_merge_chain, artifacts_may_remain);
        }

        phase = MergePhase::merge_chain_validation;
        if (const auto chain =
                validate_merge_predecessor_chain(manifest, merge_started_chain, nullptr, nullptr);
            !chain) {
            return failure(phase, MergeStatus::merge_chain_invalid, artifacts_may_remain, chain);
        }
        constexpr auto versions =
            distributed_sieve_execution_policy_detail::DISTRIBUTED_SIEVE_BOUND_WORK_VERSIONS_V1;
        if (manifest.merge_policy_version != versions.merge_policy_version) {
            return failure(phase, MergeStatus::unsupported_merge_policy, artifacts_may_remain);
        }
        if (manifest.ooc_format_version != versions.ooc_format_version ||
            manifest.relation_serialization_version != versions.relation_serialization_version ||
            manifest.handoff_version != versions.handoff_version ||
            manifest.receipt_version != MERGE_INPUT_RECEIPT_VERSION_V1 ||
            manifest.digest_version != versions.digest_version) {
            return failure(phase, MergeStatus::unsupported_semantic_version, artifacts_may_remain);
        }

        const auto& latest = merge_started_chain.back();
        phase = MergePhase::input_validation;
        if (input_readers.size() != latest.ordered_inputs.size()) {
            return failure(phase, MergeStatus::input_reader_invalid, artifacts_may_remain);
        }

        MergeReceipt receipt;
        receipt.per_chunk_retained_counts.reserve(latest.ordered_inputs.size());
        std::uint64_t expected_input_count = 0;
        for (std::size_t index = 0; index < latest.ordered_inputs.size(); ++index) {
            input_slot = index;
            const auto& input = latest.ordered_inputs[index];
            const auto* reader = input_readers[index];
            if (!checked_add(expected_input_count, input.raw_relation_count)) {
                return failure(phase, MergeStatus::input_count_overflow, artifacts_may_remain,
                               protocol_failure(DistributedSieveProtocolError::integer_out_of_range,
                                                static_cast<std::uint32_t>(index)),
                               index);
            }

            switch (input.disposition) {
            case ChunkDispositionV1::empty:
                if (reader != nullptr || input.raw_relation_count != 0) {
                    return failure(phase, MergeStatus::input_reader_invalid, artifacts_may_remain,
                                   protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                    static_cast<std::uint32_t>(index)),
                                   index);
                }
                break;
            case ChunkDispositionV1::handoff:
                if (reader == nullptr || !reader->valid()) {
                    return failure(phase, MergeStatus::input_reader_invalid, artifacts_may_remain,
                                   protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                    static_cast<std::uint32_t>(index)),
                                   index);
                }
                if (input.raw_relation_count >
                        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
                    reader->count() != static_cast<std::size_t>(input.raw_relation_count)) {
                    return failure(phase, MergeStatus::input_corpus_count_mismatch,
                                   artifacts_may_remain,
                                   protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                    static_cast<std::uint32_t>(index)),
                                   index);
                }
                break;
            default:
                return failure(phase, MergeStatus::input_reader_invalid, artifacts_may_remain,
                               protocol_failure(DistributedSieveProtocolError::unknown_enum,
                                                static_cast<std::uint32_t>(index)),
                               index);
            }
            receipt.per_chunk_retained_counts.push_back({
                .chunk_id = input.chunk_id,
                .retained_relation_count = 0,
            });
        }
        try {
            // Every input may be unique. Prove the output index can represent
            // that upper bound before the first source row is read or written.
            (void)relation::OOCRelationWriter::index_size_for_count(expected_input_count);
        } catch (const std::overflow_error&) {
            return failure(phase, MergeStatus::input_count_overflow, artifacts_may_remain,
                           protocol_failure(DistributedSieveProtocolError::integer_out_of_range));
        }

        std::unordered_set<core::ABPair, core::ABPairHash> seen;
        for (std::size_t index = 0; index < latest.ordered_inputs.size(); ++index) {
            input_slot = index;
            const auto& input = latest.ordered_inputs[index];
            const auto* reader = input_readers[index];
            if (input.disposition == ChunkDispositionV1::empty) {
                continue;
            }

            relation::RelationSequenceReceiptAccumulator sequence;
            relation::RelationCorpusSha256AccumulatorV1 corpus;
            for (std::size_t ordinal = 0; ordinal < reader->count(); ++ordinal) {
                relation_ordinal = static_cast<std::uint64_t>(ordinal);
                phase = MergePhase::input_read;

                core::Relation relation_value;
                try {
                    relation_value = reader->read(ordinal);
                } catch (const std::bad_alloc&) {
                    return resource_failure(phase, artifacts_may_remain, input_slot,
                                            relation_ordinal);
                } catch (const std::length_error&) {
                    return resource_failure(phase, artifacts_may_remain, input_slot,
                                            relation_ordinal);
                } catch (...) {
                    return failure(phase, MergeStatus::input_read_failed, artifacts_may_remain,
                                   protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                    static_cast<std::uint32_t>(input_slot)),
                                   input_slot, relation_ordinal);
                }

                sequence.append(relation_value);
                if (!corpus.append(relation_value)) {
                    return failure(
                        phase, MergeStatus::input_corpus_receipt_mismatch, true,
                        protocol_failure(DistributedSieveProtocolError::digest_unavailable,
                                         static_cast<std::uint32_t>(input_slot)),
                        input_slot, relation_ordinal);
                }
                if (!checked_increment(receipt.input_relation_count)) {
                    return failure(
                        phase, MergeStatus::input_count_overflow, artifacts_may_remain,
                        protocol_failure(DistributedSieveProtocolError::integer_out_of_range,
                                         static_cast<std::uint32_t>(input_slot)),
                        input_slot, relation_ordinal);
                }

                const auto [seen_position, inserted] = seen.insert(relation_value.ab());
                (void)seen_position;
                if (!inserted) {
                    if (!checked_increment(receipt.duplicate_relation_count)) {
                        return failure(
                            phase, MergeStatus::count_conservation_invalid, artifacts_may_remain,
                            protocol_failure(DistributedSieveProtocolError::integer_out_of_range,
                                             static_cast<std::uint32_t>(input_slot)),
                            input_slot, relation_ordinal);
                    }
                    continue;
                }

                phase = MergePhase::output_write;
                // Set this before invoking the writer: even a throwing append
                // may have changed one or both exact output files.
                artifacts_may_remain = true;
                std::size_t written_ordinal = 0;
                try {
                    written_ordinal = output_writer.write(relation_value);
                } catch (const std::bad_alloc&) {
                    return resource_failure(phase, artifacts_may_remain, input_slot,
                                            relation_ordinal);
                } catch (const std::length_error&) {
                    return resource_failure(phase, artifacts_may_remain, input_slot,
                                            relation_ordinal);
                } catch (...) {
                    return failure(phase, MergeStatus::output_write_failed, artifacts_may_remain,
                                   protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                    static_cast<std::uint32_t>(input_slot)),
                                   input_slot, relation_ordinal);
                }
                if (receipt.output_relation_count >
                        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
                    written_ordinal != static_cast<std::size_t>(receipt.output_relation_count)) {
                    return failure(
                        phase, MergeStatus::output_write_failed, artifacts_may_remain,
                        protocol_failure(DistributedSieveProtocolError::noncanonical_order,
                                         static_cast<std::uint32_t>(input_slot)),
                        input_slot, relation_ordinal);
                }
                if (!checked_increment(receipt.output_relation_count) ||
                    !checked_increment(
                        receipt.per_chunk_retained_counts[index].retained_relation_count)) {
                    return failure(
                        phase, MergeStatus::count_conservation_invalid, artifacts_may_remain,
                        protocol_failure(DistributedSieveProtocolError::integer_out_of_range,
                                         static_cast<std::uint32_t>(input_slot)),
                        input_slot, relation_ordinal);
                }
                if (hooks.after_output_write != nullptr) {
                    hooks.after_output_write(input_slot, relation_ordinal, hooks.context);
                }
            }

            phase = MergePhase::input_validation;
            relation_ordinal = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL;
            const auto sequence_receipt = sequence.finish();
            const auto corpus_digest = corpus.finalize();
            if (!corpus_digest.has_value() ||
                sequence_receipt.relation_count != input.sequence_receipt.relation_count ||
                sequence_receipt.low != input.sequence_receipt.low ||
                sequence_receipt.high != input.sequence_receipt.high ||
                *corpus_digest != input.corpus_sha256 ||
                sequence.count() != input.raw_relation_count ||
                corpus.count() != input.raw_relation_count) {
                return failure(phase, MergeStatus::input_corpus_receipt_mismatch, true,
                               protocol_failure(DistributedSieveProtocolError::digest_mismatch,
                                                static_cast<std::uint32_t>(input_slot)),
                               input_slot);
            }
        }

        phase = MergePhase::count_validation;
        input_slot = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_INPUT_SLOT;
        relation_ordinal = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL;
        std::uint64_t observed_output_count = 0;
        for (std::size_t index = 0; index < receipt.per_chunk_retained_counts.size(); ++index) {
            const auto& retained = receipt.per_chunk_retained_counts[index];
            const auto& input = latest.ordered_inputs[index];
            if (retained.chunk_id != input.chunk_id ||
                retained.retained_relation_count > input.raw_relation_count ||
                !checked_add(observed_output_count, retained.retained_relation_count)) {
                return failure(phase, MergeStatus::count_conservation_invalid, artifacts_may_remain,
                               protocol_failure(DistributedSieveProtocolError::invalid_value,
                                                static_cast<std::uint32_t>(index)),
                               index);
            }
        }

        std::uint64_t conserved_count = receipt.duplicate_relation_count;
        if (!checked_add(conserved_count, receipt.output_relation_count) ||
            receipt.input_relation_count != expected_input_count ||
            conserved_count != receipt.input_relation_count ||
            observed_output_count != receipt.output_relation_count ||
            receipt.output_relation_count >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            output_writer.state() != relation::OOCWriterState::Open ||
            output_writer.count() != static_cast<std::size_t>(receipt.output_relation_count)) {
            return failure(phase, MergeStatus::count_conservation_invalid, artifacts_may_remain);
        }

        return {
            .receipt = std::move(receipt),
            .diagnostic =
                {
                    .phase = MergePhase::complete,
                    .status = MergeStatus::ready,
                    .protocol = {},
                    .input_slot = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_INPUT_SLOT,
                    .relation_ordinal = DISTRIBUTED_SIEVE_MERGE_WRITER_NO_RELATION_ORDINAL,
                },
            .artifacts_may_remain = artifacts_may_remain,
        };
    } catch (const std::bad_alloc&) {
        return resource_failure(phase, artifacts_may_remain, input_slot, relation_ordinal);
    } catch (const std::length_error&) {
        return resource_failure(phase, artifacts_may_remain, input_slot, relation_ordinal);
    } catch (...) {
        return failure(phase, MergeStatus::unexpected_failure, artifacts_may_remain,
                       protocol_failure(DistributedSieveProtocolError::invalid_value), input_slot,
                       relation_ordinal);
    }
}

DistributedSieveMergeWriterResultV1 stream_distributed_sieve_merge_inputs_v1(
    const WaveManifestV1& manifest, std::span<const MergeStartedV1> merge_started_chain,
    std::span<const relation::OOCRelationReader* const> input_readers,
    relation::OOCRelationWriter& output_writer) noexcept {
    return stream_distributed_sieve_merge_inputs_impl_v1(manifest, merge_started_chain,
                                                         input_readers, output_writer, {});
}

namespace trusted_test {

DistributedSieveMergeWriterResultV1 stream_distributed_sieve_merge_inputs_v1_with_hooks(
    const WaveManifestV1& manifest, std::span<const MergeStartedV1> merge_started_chain,
    std::span<const relation::OOCRelationReader* const> input_readers,
    relation::OOCRelationWriter& output_writer,
    DistributedSieveMergeWriterTestHooksV1 hooks) noexcept {
    return stream_distributed_sieve_merge_inputs_impl_v1(manifest, merge_started_chain,
                                                         input_readers, output_writer, hooks);
}

} // namespace trusted_test

} // namespace gnfs::sieve::distributed_sieve_merge_writer_detail
