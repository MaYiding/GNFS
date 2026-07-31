#include "distributed_sieve_merge_coordinator.hpp"

#include "gnfs/relation/ooc_relation_store.hpp"

#include <limits>
#include <memory>
#include <new>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_merge_coordinator_detail {
namespace {

namespace resume = distributed_sieve_resume_detail;
namespace worker = distributed_sieve_worker_coordinator_detail;
using namespace distributed_sieve_worker_coordinator_detail;

constexpr std::size_t NO_MANIFEST_SLOT = std::numeric_limits<std::size_t>::max();

[[nodiscard]] bool
wave_resource_exhausted(const resume::DistributedSieveWaveStoreDiagnostic& diagnostic) noexcept {
    return diagnostic.status == resume::DistributedSieveWaveStoreStatus::resource_exhausted ||
           (diagnostic.protocol_status.has_value() &&
            diagnostic.protocol_status->error == DistributedSieveProtocolError::resource_exhausted);
}

void set_invalid_request(DistributedSieveMergeCoordinatorDiagnosticV1& diagnostic,
                         DistributedSieveMergeCoordinatorStatusV1 status,
                         std::size_t manifest_slot = NO_MANIFEST_SLOT) noexcept {
    diagnostic.phase = DistributedSieveMergeCoordinatorPhaseV1::request_validation;
    diagnostic.status = status;
    diagnostic.manifest_slot = manifest_slot;
    diagnostic.wave_store.status = resume::DistributedSieveWaveStoreStatus::invalid_request;
    diagnostic.wave_store.native_error = std::make_error_code(std::errc::invalid_argument);
}

void set_wave_failure(DistributedSieveMergeCoordinatorDiagnosticV1& diagnostic,
                      DistributedSieveMergeCoordinatorPhaseV1 phase,
                      DistributedSieveMergeCoordinatorStatusV1 fallback,
                      const resume::DistributedSieveWaveStoreDiagnostic& wave_store) noexcept {
    diagnostic.phase = phase;
    diagnostic.status = wave_resource_exhausted(wave_store)
                            ? DistributedSieveMergeCoordinatorStatusV1::resource_exhausted
                            : fallback;
    diagnostic.wave_store = wave_store;
}

[[nodiscard]] bool
chunk_projection_is_admissible(const worker::DistributedSieveWorkerCoordinatedChunkV1& coordinated,
                               const ChunkPlanV1& manifest_chunk,
                               const WaveManifestV1& manifest) noexcept {
    if (coordinated.chunk != manifest_chunk ||
        coordinated.chunk.chunk_id >= manifest.chunks.size() ||
        coordinated.terminal_failure.has_value()) {
        return false;
    }
    switch (coordinated.disposition) {
    case worker::DistributedSieveWorkerCoordinationDispositionV1::empty:
        return manifest_chunk.sq_begin == manifest_chunk.sq_end &&
               !coordinated.launched_attempt.has_value() &&
               !coordinated.reconciled_attempt.has_value() && !coordinated.wait_facts.has_value() &&
               !coordinated.adopted.has_value();
    case worker::DistributedSieveWorkerCoordinationDispositionV1::adopted:
    case worker::DistributedSieveWorkerCoordinationDispositionV1::executed:
        break;
    case worker::DistributedSieveWorkerCoordinationDispositionV1::terminal_failed:
        return false;
    }
    if (manifest_chunk.sq_begin == manifest_chunk.sq_end || !coordinated.adopted.has_value() ||
        !coordinated.adopted->valid()) {
        return false;
    }
    const auto& handoff = coordinated.adopted->handoff();
    const auto& reader = coordinated.adopted->reader();
    return reader.valid() && reader.count() == handoff.relation_count &&
           handoff.manifest_digest == manifest.self_digest &&
           handoff.work_digest == manifest.work_sha256 &&
           handoff.chunk_id == manifest_chunk.chunk_id &&
           handoff.sq_begin == manifest_chunk.sq_begin && handoff.sq_end == manifest_chunk.sq_end &&
           handoff.attempt_ordinal < manifest.max_worker_attempts;
}

[[nodiscard]] TerminalChunkInputV1
terminal_projection(const worker::DistributedSieveWorkerCoordinatedChunkV1& coordinated) noexcept {
    TerminalChunkInputV1 projection;
    projection.chunk_id = coordinated.chunk.chunk_id;
    projection.sq_begin = coordinated.chunk.sq_begin;
    projection.sq_end = coordinated.chunk.sq_end;
    if (coordinated.disposition == worker::DistributedSieveWorkerCoordinationDispositionV1::empty) {
        projection.disposition = ChunkDispositionV1::empty;
        projection.next_sq_index = coordinated.chunk.sq_begin;
        projection.completion_reason = WorkerCompletionReasonV1::zero_relations;
        return projection;
    }

    const auto& handoff = coordinated.adopted->handoff();
    projection.disposition = ChunkDispositionV1::handoff;
    projection.next_sq_index = handoff.next_sq_index;
    projection.processed_sq_count = handoff.processed_sq_count;
    projection.completion_reason = handoff.completion_reason;
    projection.durable_attempt_count = handoff.attempt_ordinal + 1U;
    projection.last_attempt_digest = handoff.attempt_started_digest;
    projection.lease_id = handoff.lease.lease_id;
    projection.handoff_digest = handoff.self_digest;
    projection.raw_relation_count = handoff.relation_count;
    projection.sequence_receipt = handoff.artifact.sequence_receipt;
    projection.corpus_sha256 = handoff.artifact.corpus_sha256;
    return projection;
}

} // namespace

DistributedSieveMergeGenerationAdmissionV1::operator bool() const noexcept {
    return diagnostic_.status == DistributedSieveMergeCoordinatorStatusV1::admitted &&
           started_receipt_.has_value() && started_receipt_->owned_by_current_process() &&
           static_cast<bool>(worker_result_);
}

const DistributedSieveMergeCoordinatorDiagnosticV1&
DistributedSieveMergeGenerationAdmissionV1::diagnostic() const noexcept {
    return diagnostic_;
}

const resume::DistributedSieveMergeStartedReceiptV1*
DistributedSieveMergeGenerationAdmissionV1::started_receipt() const noexcept {
    return started_receipt_.has_value() ? std::addressof(*started_receipt_) : nullptr;
}

DistributedSieveMergeGenerationAdmissionV1 begin_or_resume_distributed_sieve_merge_generation_v1(
    DistributedSieveWorkerCoordinatorResultV1&& worker_result) noexcept {
    DistributedSieveMergeCoordinatorDiagnosticV1 diagnostic;
    set_invalid_request(diagnostic,
                        DistributedSieveMergeCoordinatorStatusV1::invalid_worker_result);

    std::vector<TerminalChunkInputV1> terminal_inputs;
    std::vector<const resume::DistributedSieveAdoptedWorkerChunkV1*> held_worker_handoffs;
    std::uint32_t ordinal = 0;
    std::uint32_t merge_policy = 0;
    bool projection_ready = false;

    try {
        if (worker_result) {
            const auto& manifest = worker_result.store->manifest();
            if (worker_result.chunks.size() == manifest.chunks.size() && !manifest.chunks.empty()) {
                terminal_inputs.reserve(manifest.chunks.size());
                held_worker_handoffs.reserve(manifest.chunks.size());
                projection_ready = true;
                for (std::size_t index = 0; index < manifest.chunks.size(); ++index) {
                    const auto& coordinated = worker_result.chunks[index];
                    if (!chunk_projection_is_admissible(coordinated, manifest.chunks[index],
                                                        manifest)) {
                        const auto status =
                            coordinated.disposition ==
                                    worker::DistributedSieveWorkerCoordinationDispositionV1::
                                        terminal_failed
                                ? DistributedSieveMergeCoordinatorStatusV1::terminal_worker_failure
                                : DistributedSieveMergeCoordinatorStatusV1::invalid_worker_result;
                        set_invalid_request(diagnostic, status, index);
                        projection_ready = false;
                        break;
                    }
                    terminal_inputs.push_back(terminal_projection(coordinated));
                    if (coordinated.adopted.has_value()) {
                        held_worker_handoffs.push_back(std::addressof(*coordinated.adopted));
                    }
                }
                if (projection_ready) {
                    merge_policy = manifest.merge_policy_version;
                }
            }
        } else {
            bool durable_terminal_failure = false;
            const auto slot = worker_result.diagnostic.manifest_slot;
            if (worker_result.store != nullptr && worker_result.coordinator_claim != nullptr &&
                worker_result.coordinator_claim->owned_by_current_process() &&
                worker_result.diagnostic.status ==
                    worker::DistributedSieveWorkerCoordinatorStatusV1::retry_exhausted) {
                const auto& manifest = worker_result.store->manifest();
                if (worker_result.chunks.size() == manifest.chunks.size() &&
                    slot < manifest.chunks.size()) {
                    const auto& coordinated = worker_result.chunks[slot];
                    if (coordinated.chunk == manifest.chunks[slot] &&
                        coordinated.disposition ==
                            worker::DistributedSieveWorkerCoordinationDispositionV1::
                                terminal_failed &&
                        coordinated.terminal_failure.has_value()) {
                        const auto& terminal = *coordinated.terminal_failure;
                        durable_terminal_failure =
                            terminal.chunk == manifest.chunks[slot] &&
                            terminal.state == resume::DistributedSieveWorkerChunkDurableStateV1::
                                                  terminal_failure &&
                            terminal.latest_attempt.has_value() &&
                            terminal.terminal_failure.has_value() && !terminal.handoff.has_value();
                    }
                }
            }
            if (durable_terminal_failure) {
                diagnostic.phase = DistributedSieveMergeCoordinatorPhaseV1::request_validation;
                diagnostic.status =
                    DistributedSieveMergeCoordinatorStatusV1::terminal_worker_failure;
                diagnostic.manifest_slot = slot;
                diagnostic.wave_store = worker_result.diagnostic.wave_store;
            } else if (worker_result.diagnostic.wave_store.status !=
                       resume::DistributedSieveWaveStoreStatus::ready) {
                diagnostic.wave_store = worker_result.diagnostic.wave_store;
            }
        }
    } catch (const std::bad_alloc&) {
        diagnostic.status = DistributedSieveMergeCoordinatorStatusV1::resource_exhausted;
        diagnostic.wave_store.status = resume::DistributedSieveWaveStoreStatus::resource_exhausted;
        diagnostic.wave_store.native_error = std::make_error_code(std::errc::not_enough_memory);
        projection_ready = false;
    } catch (...) {
        diagnostic.status = DistributedSieveMergeCoordinatorStatusV1::unexpected_failure;
        diagnostic.wave_store.status = resume::DistributedSieveWaveStoreStatus::unexpected_failure;
        diagnostic.wave_store.native_error = std::make_error_code(std::errc::io_error);
        projection_ready = false;
    }

    const auto held_handoffs = std::span<const resume::DistributedSieveAdoptedWorkerChunkV1* const>(
        held_worker_handoffs.data(), held_worker_handoffs.size());
    auto cursor = [&]() -> resume::DistributedSieveMergeGenerationCursorResultV1 {
        if (projection_ready) {
            return resume::prepare_distributed_sieve_merge_generation_v1(*worker_result.store,
                                                                         held_handoffs);
        }
        resume::DistributedSieveMergeGenerationCursorResultV1 skipped;
        skipped.diagnostic = diagnostic.wave_store;
        return skipped;
    }();
    const bool cursor_ready = projection_ready && static_cast<bool>(cursor);
    if (projection_ready) {
        diagnostic.phase = DistributedSieveMergeCoordinatorPhaseV1::generation_cursor;
        if (!cursor_ready) {
            set_wave_failure(diagnostic, DistributedSieveMergeCoordinatorPhaseV1::generation_cursor,
                             DistributedSieveMergeCoordinatorStatusV1::generation_unavailable,
                             cursor.diagnostic);
        } else {
            ordinal = *cursor.merge_attempt_ordinal;
        }
    }

    auto reservation = [&]() -> resume::DistributedSieveMergeLeaseReservationResultV1 {
        if (cursor_ready) {
            return resume::reserve_distributed_sieve_merge_generation_v1(
                *worker_result.store, ordinal, terminal_inputs, merge_policy, {}, held_handoffs);
        }
        resume::DistributedSieveMergeLeaseReservationResultV1 skipped;
        skipped.diagnostic = diagnostic.wave_store;
        return skipped;
    }();
    const bool reservation_ready = cursor_ready && static_cast<bool>(reservation);
    if (cursor_ready) {
        diagnostic.phase = DistributedSieveMergeCoordinatorPhaseV1::reservation;
        if (!reservation_ready) {
            set_wave_failure(diagnostic, DistributedSieveMergeCoordinatorPhaseV1::reservation,
                             DistributedSieveMergeCoordinatorStatusV1::reservation_failed,
                             reservation.diagnostic);
        }
    }

    auto started = [&]() -> resume::DistributedSieveMergeStartResultV1 {
        if (reservation_ready) {
            return resume::publish_merge_started_v1(
                std::move(*reservation.receipt), terminal_inputs, merge_policy, {}, held_handoffs);
        }
        resume::DistributedSieveMergeStartResultV1 skipped;
        skipped.diagnostic = diagnostic.wave_store;
        return skipped;
    }();
    if (reservation_ready) {
        diagnostic.phase = DistributedSieveMergeCoordinatorPhaseV1::start_publication;
        if (!started) {
            set_wave_failure(diagnostic, DistributedSieveMergeCoordinatorPhaseV1::start_publication,
                             DistributedSieveMergeCoordinatorStatusV1::start_publication_failed,
                             started.diagnostic);
        } else {
            diagnostic.status = DistributedSieveMergeCoordinatorStatusV1::admitted;
            diagnostic.manifest_slot = NO_MANIFEST_SLOT;
            diagnostic.wave_store = {};
        }
    }

    return DistributedSieveMergeGenerationAdmissionV1(std::move(worker_result), std::move(started),
                                                      std::move(diagnostic));
}

} // namespace gnfs::sieve::distributed_sieve_merge_coordinator_detail
