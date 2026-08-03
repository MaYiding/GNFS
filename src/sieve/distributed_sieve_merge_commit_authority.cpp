#include "distributed_sieve_merge_commit_authority_internal.hpp"

#include <gnfs/util/process.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail {

bool DistributedSieveCommittedTailAdmissionV1::valid() const noexcept {
    try {
        const int process_id = gnfs::util::process_id();
        if (origin_ == nullptr || prepared_record_ == nullptr || creator_process_id_ == 0 ||
            process_id <= 0 || creator_process_id_ != static_cast<std::uint64_t>(process_id) ||
            canonical_snapshot_.size == 0 ||
            canonical_snapshot_.identity == util::durable_immutable_record::NativeIdentity{} ||
            !origin_->prepared_origin_valid(prepared_record_, creator_process_id_)) {
            return false;
        }
        auto* store = origin_->retained_wave_store();
        const auto starts = origin_->retained_merge_started_chain();
        const auto* origin_predecessor_snapshots = origin_->retained_predecessor_snapshots();
        if (store == nullptr || starts.empty() || origin_predecessor_snapshots == nullptr ||
            *origin_predecessor_snapshots != predecessor_snapshots_ ||
            origin_->retained_manifest_slot_count() != prepared_record_->ordered_inputs.size() ||
            commit_.chunks.size() != prepared_record_->ordered_inputs.size()) {
            return false;
        }
        std::vector<const WorkerHandoffV1*> handoffs;
        handoffs.reserve(commit_.chunks.size());
        for (std::size_t slot = 0; slot < commit_.chunks.size(); ++slot) {
            const auto* handoff = origin_->retained_worker_handoff(slot);
            if (commit_.chunks[slot].diagnostic.kind != NormalizedDiagnosticKindV1::none ||
                commit_.chunks[slot].diagnostic.code != 0 ||
                (prepared_record_->ordered_inputs[slot].disposition ==
                 ChunkDispositionV1::handoff) != (handoff != nullptr)) {
                return false;
            }
            handoffs.push_back(handoff);
        }
        return store->revalidate_committed_tail_v1(*prepared_record_, starts, handoffs,
                                                   predecessor_snapshots_, commit_,
                                                   canonical_snapshot_);
    } catch (...) {
        return false;
    }
}

} // namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail

namespace gnfs::sieve::distributed_sieve_merge_commit_authority_detail {

namespace {

using Phase = DistributedSieveWaveMergeCommitPhaseV1;
using Status = DistributedSieveWaveMergeCommitStatusV1;
using Diagnostic = DistributedSieveWaveMergeCommitDiagnosticV1;
namespace resume = distributed_sieve_resume_detail;
namespace writer = distributed_sieve_merge_writer_authority_detail;
namespace durable_record = gnfs::util::durable_immutable_record;

[[nodiscard]] Diagnostic failure(Phase phase, Status status, bool admission_spent = false,
                                 bool reconciliation_required = false,
                                 std::error_code native_error = {}) noexcept {
    Diagnostic diagnostic;
    diagnostic.phase = phase;
    diagnostic.status = status;
    diagnostic.native_error = native_error;
    diagnostic.admission_spent = admission_spent;
    diagnostic.reconciliation_required = reconciliation_required;
    return diagnostic;
}

[[nodiscard]] Diagnostic map_publication_failure(
    resume::DistributedSieveWaveMergeCommitPublicationResultV1& published) noexcept {
    const auto& lower = published.diagnostic;
    Phase phase = Phase::context_revalidation;
    Status status = Status::context_invalid;
    if (lower.protocol_status.has_value()) {
        phase = Phase::dependency_validation;
        status = Status::dependency_invalid;
    }
    if (lower.publication_status.has_value()) {
        phase = lower.publication_status == durable_record::RecordPublishStatus::durable
                    ? Phase::canonical_revalidation
                    : Phase::publication;
        status = Status::publication_failed;
    }
    switch (lower.status) {
    case resume::DistributedSieveWaveStoreStatus::platform_unsupported:
        phase = Phase::platform_gate;
        status = Status::platform_unsupported;
        break;
    case resume::DistributedSieveWaveStoreStatus::resource_exhausted:
        status = Status::resource_exhausted;
        break;
    case resume::DistributedSieveWaveStoreStatus::invalid_request:
        if (lower.native_error == std::make_error_code(std::errc::no_such_process)) {
            status = Status::process_mismatch;
        }
        break;
    case resume::DistributedSieveWaveStoreStatus::unexpected_failure:
        status = Status::unexpected_failure;
        break;
    default:
        break;
    }
    Diagnostic higher = failure(phase, status, published.admission_spent, published.admission_spent,
                                lower.native_error);
    higher.wave_store = lower;
    if (lower.protocol_status.has_value()) {
        higher.protocol = *lower.protocol_status;
    }
    return higher;
}

} // namespace

DistributedSieveWaveMergeCommitResultV1 DistributedSieveWaveMergeCommitAuthorityV1::consume(
    DistributedSieveMergePreparedAdmissionV1&& admission,
    DistributedSieveWaveMergeCommitTestHooksV1 hooks) noexcept {
    if (!admission.valid()) {
        return {
            .retryable_prepared = std::nullopt,
            .committed_tail = std::nullopt,
            .diagnostic = failure(Phase::admission_validation, Status::invalid_admission),
        };
    }

#if !defined(__APPLE__)
    (void)hooks;
    std::optional<DistributedSieveMergePreparedAdmissionV1> retryable;
    retryable.emplace(std::move(admission));
    return {
        .retryable_prepared = std::move(retryable),
        .committed_tail = std::nullopt,
        .diagnostic = failure(Phase::platform_gate, Status::platform_unsupported, false, false,
                              std::make_error_code(std::errc::operation_not_supported)),
    };
#else
    writer::DistributedSieveMergePreparedCommitContextV1 context(
        std::move(admission.origin_), std::exchange(admission.record_, nullptr),
        std::exchange(admission.creator_process_id_, 0));
    if (!context.valid()) {
        return {
            .retryable_prepared = std::nullopt,
            .committed_tail = std::nullopt,
            .diagnostic = failure(Phase::context_revalidation, Status::context_invalid),
        };
    }

    try {
        auto* store = context.origin_->retained_wave_store();
        const auto starts = context.origin_->retained_merge_started_chain();
        const auto* origin_predecessor_snapshots =
            context.origin_->retained_predecessor_snapshots();
        const std::size_t slots = context.origin_->retained_manifest_slot_count();
        if (store == nullptr || starts.empty() || origin_predecessor_snapshots == nullptr ||
            slots != context.record_->ordered_inputs.size()) {
            std::optional<DistributedSieveMergePreparedAdmissionV1> retryable;
            DistributedSieveMergePreparedAdmissionV1 retryable_value(
                std::move(context.origin_), context.record_, context.creator_process_id_);
            retryable.emplace(std::move(retryable_value));
            return {
                .retryable_prepared = std::move(retryable),
                .committed_tail = std::nullopt,
                .diagnostic = failure(Phase::context_revalidation, Status::context_invalid),
            };
        }
        std::vector<const WorkerHandoffV1*> handoffs;
        handoffs.reserve(slots);
        for (std::size_t slot = 0; slot < slots; ++slot) {
            handoffs.push_back(context.origin_->retained_worker_handoff(slot));
        }
        auto tail_predecessor_snapshots = *origin_predecessor_snapshots;

        auto published = store->publish_wave_merge_commit_v1(*context.record_, starts, handoffs,
                                                             tail_predecessor_snapshots, hooks);
        if (!published) {
            auto diagnostic = map_publication_failure(published);
            std::optional<DistributedSieveMergePreparedAdmissionV1> retryable;
            if (!published.admission_spent && context.valid()) {
                DistributedSieveMergePreparedAdmissionV1 retryable_value(
                    std::move(context.origin_), context.record_, context.creator_process_id_);
                retryable.emplace(std::move(retryable_value));
            }
            return {
                .retryable_prepared = std::move(retryable),
                .committed_tail = std::nullopt,
                .diagnostic = std::move(diagnostic),
            };
        }

        writer::DistributedSieveCommittedTailAdmissionV1 tail(
            std::move(context.origin_), context.record_, std::move(*published.commit),
            std::move(tail_predecessor_snapshots), *published.canonical_snapshot,
            context.creator_process_id_);
        if (!tail.valid()) {
            auto diagnostic =
                failure(Phase::canonical_revalidation, Status::publication_failed, true, true,
                        std::make_error_code(std::errc::state_not_recoverable));
            diagnostic.wave_store = std::move(published.diagnostic);
            return {
                .retryable_prepared = std::nullopt,
                .committed_tail = std::nullopt,
                .diagnostic = std::move(diagnostic),
            };
        }
        std::optional<writer::DistributedSieveCommittedTailAdmissionV1> committed;
        committed.emplace(std::move(tail));
        auto diagnostic = failure(Phase::complete, Status::ready, true);
        diagnostic.wave_store = std::move(published.diagnostic);
        return {
            .retryable_prepared = std::nullopt,
            .committed_tail = std::move(committed),
            .diagnostic = std::move(diagnostic),
        };
    } catch (const std::bad_alloc&) {
        std::optional<DistributedSieveMergePreparedAdmissionV1> retryable;
        if (context.valid()) {
            DistributedSieveMergePreparedAdmissionV1 retryable_value(
                std::move(context.origin_), context.record_, context.creator_process_id_);
            retryable.emplace(std::move(retryable_value));
        }
        return {
            .retryable_prepared = std::move(retryable),
            .committed_tail = std::nullopt,
            .diagnostic = failure(Phase::commit_build, Status::resource_exhausted, false, false,
                                  std::make_error_code(std::errc::not_enough_memory)),
        };
    } catch (...) {
        std::optional<DistributedSieveMergePreparedAdmissionV1> retryable;
        if (context.valid()) {
            DistributedSieveMergePreparedAdmissionV1 retryable_value(
                std::move(context.origin_), context.record_, context.creator_process_id_);
            retryable.emplace(std::move(retryable_value));
        }
        return {
            .retryable_prepared = std::move(retryable),
            .committed_tail = std::nullopt,
            .diagnostic = failure(Phase::commit_build, Status::unexpected_failure),
        };
    }
#endif
}

DistributedSieveWaveMergeCommitResultV1 consume_distributed_sieve_merge_prepared_v1(
    DistributedSieveMergePreparedAdmissionV1&& admission) noexcept {
    return DistributedSieveWaveMergeCommitAuthorityV1::consume(std::move(admission), {});
}

DistributedSieveWaveMergeCommitResultV1
trusted_test::consume_distributed_sieve_merge_prepared_v1_with_hooks(
    DistributedSieveMergePreparedAdmissionV1&& admission,
    DistributedSieveWaveMergeCommitTestHooksV1 hooks) noexcept {
    return DistributedSieveWaveMergeCommitAuthorityV1::consume(std::move(admission), hooks);
}

} // namespace gnfs::sieve::distributed_sieve_merge_commit_authority_detail
