#include "distributed_sieve_bound_work_internal.hpp"
#include "distributed_sieve_worker_coordinator_internal.hpp"

#include <filesystem>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

namespace gnfs::sieve::distributed_sieve_worker_coordinator_detail {
namespace {

namespace execution = distributed_sieve_execution_policy_detail;
namespace launcher = distributed_sieve_worker_launcher_detail;
namespace process = distributed_sieve_worker_process_detail;
namespace resume = distributed_sieve_resume_detail;

constexpr std::size_t NO_MANIFEST_SLOT = std::numeric_limits<std::size_t>::max();

enum class PlannedChunkAction : std::uint8_t {
    empty,
    adopt,
    recover,
    launch,
};

enum class LateHandoffObservation : std::uint8_t {
    exact_incomplete,
    adopted,
    failed,
};

void set_failure(DistributedSieveWorkerCoordinatorResultV1& result,
                 DistributedSieveWorkerCoordinatorPhaseV1 phase,
                 DistributedSieveWorkerCoordinatorStatusV1 status,
                 std::size_t manifest_slot = NO_MANIFEST_SLOT) noexcept {
    result.diagnostic.phase = phase;
    result.diagnostic.status = status;
    result.diagnostic.manifest_slot = manifest_slot;
}

[[nodiscard]] DistributedSieveWorkerCoordinatorStatusV1
wave_failure_status(const resume::DistributedSieveWaveStoreDiagnostic& diagnostic,
                    DistributedSieveWorkerCoordinatorStatusV1 fallback) noexcept {
    if (diagnostic.status == resume::DistributedSieveWaveStoreStatus::resource_exhausted) {
        return DistributedSieveWorkerCoordinatorStatusV1::resource_exhausted;
    }
    return fallback;
}

[[nodiscard]] bool
protocol_is_resource_exhausted(const DistributedSieveProtocolStatus& status) noexcept {
    return status.error == DistributedSieveProtocolError::resource_exhausted;
}

void set_protocol_request_failure(DistributedSieveWorkerCoordinatorResultV1& result,
                                  const DistributedSieveProtocolStatus& status) noexcept {
    result.diagnostic.wave_store.protocol_status = status;
    if (protocol_is_resource_exhausted(status)) {
        result.diagnostic.wave_store.status =
            resume::DistributedSieveWaveStoreStatus::resource_exhausted;
        result.diagnostic.wave_store.native_error =
            std::make_error_code(std::errc::not_enough_memory);
        set_failure(result, DistributedSieveWorkerCoordinatorPhaseV1::request_validation,
                    DistributedSieveWorkerCoordinatorStatusV1::resource_exhausted);
        return;
    }
    result.diagnostic.wave_store.status = resume::DistributedSieveWaveStoreStatus::invalid_request;
    result.diagnostic.wave_store.native_error = std::make_error_code(std::errc::invalid_argument);
    set_failure(result, DistributedSieveWorkerCoordinatorPhaseV1::request_validation,
                DistributedSieveWorkerCoordinatorStatusV1::invalid_request);
}

[[nodiscard]] bool
executable_request_is_valid(const DistributedSieveWorkerCoordinatorRequestV1& request) {
    if (request.executable_path.empty() ||
        request.executable_path.find('\0') != std::string::npos ||
        !std::filesystem::path(request.executable_path).is_absolute()) {
        return false;
    }
    for (const auto& argument : request.worker_arguments) {
        if (argument.find('\0') != std::string::npos) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool attempt_matches_chunk(const AttemptStartedV1& attempt, const ChunkPlanV1& chunk,
                                         const util::Sha256Digest& manifest_digest,
                                         std::uint32_t expected_ordinal) noexcept {
    return attempt.manifest_digest == manifest_digest && attempt.chunk_id == chunk.chunk_id &&
           attempt.sq_begin == chunk.sq_begin && attempt.sq_end == chunk.sq_end &&
           attempt.attempt_ordinal == expected_ordinal;
}

[[nodiscard]] bool attempts_equal(const AttemptStartedV1& left,
                                  const AttemptStartedV1& right) noexcept {
    return left.manifest_digest == right.manifest_digest && left.chunk_id == right.chunk_id &&
           left.sq_begin == right.sq_begin && left.sq_end == right.sq_end &&
           left.attempt_ordinal == right.attempt_ordinal &&
           left.predecessor_digest == right.predecessor_digest && left.lease == right.lease &&
           left.retry_policy_version == right.retry_policy_version &&
           left.self_digest == right.self_digest;
}

[[nodiscard]] bool handoffs_equal(const WorkerHandoffV1& left,
                                  const WorkerHandoffV1& right) noexcept {
    return left.manifest_digest == right.manifest_digest && left.work_digest == right.work_digest &&
           left.wave_id == right.wave_id && left.chunk_id == right.chunk_id &&
           left.sq_begin == right.sq_begin && left.sq_end == right.sq_end &&
           left.attempt_ordinal == right.attempt_ordinal &&
           left.attempt_started_digest == right.attempt_started_digest &&
           left.lease == right.lease && left.artifact == right.artifact &&
           left.processed_sq_count == right.processed_sq_count &&
           left.next_sq_index == right.next_sq_index &&
           left.completion_reason == right.completion_reason &&
           left.relation_count == right.relation_count &&
           left.cleanup_intent_absent == right.cleanup_intent_absent &&
           left.self_digest == right.self_digest;
}

[[nodiscard]] bool handoff_matches_chunk(const WorkerHandoffV1& handoff, const ChunkPlanV1& chunk,
                                         const util::Sha256Digest& manifest_digest) noexcept {
    return handoff.manifest_digest == manifest_digest && handoff.chunk_id == chunk.chunk_id &&
           handoff.sq_begin == chunk.sq_begin && handoff.sq_end == chunk.sq_end;
}

[[nodiscard]] WorkerWaitFactsV1
wait_facts_from(const process::DistributedSieveWorkerProcessWaitResult& wait) noexcept {
    WorkerWaitFactsV1 facts;
    if (wait.reaped && wait.native_error == 0 && wait.signal == 0 && wait.exit_status >= 0 &&
        wait.exit_status <= 255) {
        facts.kind = WorkerWaitFactKindV1::exited;
        facts.exit_code = static_cast<std::int32_t>(wait.exit_status);
        return facts;
    }
    if (wait.reaped && wait.native_error == 0 && wait.exit_status < 0 && wait.signal > 0 &&
        wait.signal <= 255) {
        facts.kind = WorkerWaitFactKindV1::signaled;
        facts.signal = static_cast<std::uint32_t>(wait.signal);
        return facts;
    }
    if (wait.native_error > 0) {
        facts.kind = WorkerWaitFactKindV1::native_wait_failure;
        facts.native_error = static_cast<std::uint32_t>(wait.native_error);
    }
    return facts;
}

[[nodiscard]] bool
wait_is_terminal(const process::DistributedSieveWorkerProcessWaitResult& wait) noexcept {
    if (!wait.reaped || wait.native_error != 0) {
        return false;
    }
    const bool exited = wait.signal == 0 && wait.exit_status >= 0 && wait.exit_status <= 255;
    const bool signaled = wait.exit_status < 0 && wait.signal > 0 && wait.signal <= 255;
    return exited != signaled;
}

[[nodiscard]] std::size_t
manifest_slot_for_chunk(const std::vector<DistributedSieveWorkerCoordinatedChunkV1>& chunks,
                        std::uint32_t chunk_id) noexcept {
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        if (chunks[index].chunk.chunk_id == chunk_id) {
            return index;
        }
    }
    return NO_MANIFEST_SLOT;
}

[[nodiscard]] bool launcher_resource_exhausted(
    const launcher::DistributedSieveWorkerLaunchDiagnosticV1& diagnostic) noexcept {
    return diagnostic.wave_store.status ==
               resume::DistributedSieveWaveStoreStatus::resource_exhausted ||
           diagnostic.protocol.error == DistributedSieveProtocolError::resource_exhausted ||
           diagnostic.carrier.status ==
               distributed_sieve_worker_work_package_file_detail::
                   DistributedSieveWorkerWorkPackageFileStatus::resource_exhausted ||
           diagnostic.transport.error ==
               process::DistributedSieveWorkerProcessTransportError::resource_exhausted;
}

[[nodiscard]] bool
retry_claim_is_busy(const resume::DistributedSieveWaveStoreDiagnostic& diagnostic) noexcept {
    return diagnostic.status == resume::DistributedSieveWaveStoreStatus::private_lease_root_busy ||
           diagnostic.status == resume::DistributedSieveWaveStoreStatus::private_lease_lock_busy;
}

[[nodiscard]] bool
terminal_failure_prefix(resume::DistributedSieveWorkerChunkDurableStateV1 state) noexcept {
    return state == resume::DistributedSieveWorkerChunkDurableStateV1::terminal_failure_pending ||
           state == resume::DistributedSieveWorkerChunkDurableStateV1::terminal_failure;
}

void set_resource_exhausted(DistributedSieveWorkerCoordinatorResultV1& result,
                            DistributedSieveWorkerCoordinatorPhaseV1 phase,
                            std::size_t manifest_slot) noexcept {
    result.diagnostic.wave_store.status =
        resume::DistributedSieveWaveStoreStatus::resource_exhausted;
    result.diagnostic.wave_store.native_error = std::make_error_code(std::errc::not_enough_memory);
    set_failure(result, phase, DistributedSieveWorkerCoordinatorStatusV1::resource_exhausted,
                manifest_slot);
}

} // namespace

DistributedSieveWorkerCoordinatorResultV1 coordinate_missing_distributed_sieve_workers_v1(
    DistributedSieveWorkerCoordinatorRequestV1&& request,
    const DistributedSieveWorkIdentityV1& identity,
    const execution::DistributedSieveFrozenExecutionPolicyV1& frozen_policy,
    const core::PolynomialContext& polynomial,
    const factor_base::FactorBase& factor_base) noexcept {
    DistributedSieveWorkerCoordinatorResultV1 result;
    result.store = std::move(request.store);

    auto active_phase = DistributedSieveWorkerCoordinatorPhaseV1::request_validation;
    std::size_t active_slot = NO_MANIFEST_SLOT;

    try {
        if (result.store == nullptr) {
            result.diagnostic.wave_store.status =
                resume::DistributedSieveWaveStoreStatus::invalid_request;
            result.diagnostic.wave_store.native_error =
                std::make_error_code(std::errc::invalid_argument);
            set_failure(result, active_phase,
                        DistributedSieveWorkerCoordinatorStatusV1::invalid_request);
            return result;
        }

        active_phase = DistributedSieveWorkerCoordinatorPhaseV1::coordinator_claim;
        auto claimed = result.store->claim_worker_coordinator_v1();
        if (!claimed) {
            result.diagnostic.wave_store = claimed.diagnostic;
            const auto status =
                claimed.diagnostic.status ==
                        resume::DistributedSieveWaveStoreStatus::worker_coordinator_busy
                    ? DistributedSieveWorkerCoordinatorStatusV1::coordinator_busy
                    : wave_failure_status(
                          claimed.diagnostic,
                          claimed.diagnostic.status ==
                                  resume::DistributedSieveWaveStoreStatus::invalid_request
                              ? DistributedSieveWorkerCoordinatorStatusV1::invalid_request
                              : DistributedSieveWorkerCoordinatorStatusV1::unexpected_failure);
            set_failure(result, active_phase, status);
            return result;
        }
        result.coordinator_claim = std::move(claimed.claim);

        active_phase = DistributedSieveWorkerCoordinatorPhaseV1::initial_observation;
        auto initial = result.store->observe_worker_chunks_v1();
        if (!initial) {
            result.diagnostic.wave_store = initial.diagnostic;
            set_failure(
                result, active_phase,
                wave_failure_status(initial.diagnostic,
                                    DistributedSieveWorkerCoordinatorStatusV1::observation_failed));
            return result;
        }

        const auto& manifest = result.store->manifest();
        if (initial.chunks.size() != manifest.chunks.size()) {
            result.diagnostic.wave_store.status =
                resume::DistributedSieveWaveStoreStatus::namespace_conflict;
            set_failure(result, active_phase,
                        DistributedSieveWorkerCoordinatorStatusV1::observation_failed);
            return result;
        }

        result.chunks.reserve(manifest.chunks.size());
        for (std::size_t index = 0; index < manifest.chunks.size(); ++index) {
            active_slot = index;
            const auto& chunk = manifest.chunks[index];
            const auto& observed = initial.chunks[index];
            if (observed.chunk != chunk) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::observation_failed, index);
                return result;
            }

            DistributedSieveWorkerCoordinatedChunkV1 coordinated;
            coordinated.chunk = chunk;
            result.chunks.push_back(std::move(coordinated));
        }

        const auto finish_terminal_failure =
            [&](DistributedSieveWorkerCoordinatorPhaseV1 phase, std::size_t manifest_slot,
                resume::DistributedSieveWorkerChunkInventoryV1 terminal) {
                result.chunks[manifest_slot].disposition =
                    DistributedSieveWorkerCoordinationDispositionV1::terminal_failed;
                result.chunks[manifest_slot].terminal_failure.emplace(std::move(terminal));
                set_failure(result, phase,
                            DistributedSieveWorkerCoordinatorStatusV1::retry_exhausted,
                            manifest_slot);
            };

        std::vector<PlannedChunkAction> actions;
        actions.reserve(manifest.chunks.size());
        std::vector<std::size_t> recovery_slots;
        recovery_slots.reserve(manifest.chunks.size());
        std::vector<std::optional<std::uint32_t>> launch_ordinals(manifest.chunks.size());
        std::vector<std::optional<WorkerHandoffV1>> expected_adopted_handoffs(
            manifest.chunks.size());
        std::vector<std::optional<resume::DistributedSieveWorkerHandoffInventoryWitnessV1>>
            expected_adopted_witnesses(manifest.chunks.size());

        std::optional<std::size_t> terminal_recovery_slot;
        bool all_empty = true;
        for (std::size_t index = 0; index < manifest.chunks.size(); ++index) {
            active_slot = index;
            const auto& chunk = manifest.chunks[index];
            const auto& observed = initial.chunks[index];
            switch (observed.state) {
            case resume::DistributedSieveWorkerChunkDurableStateV1::empty:
                if (chunk.sq_begin != chunk.sq_end || observed.latest_attempt.has_value() ||
                    observed.terminal_failure.has_value() || observed.handoff.has_value()) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                index);
                    return result;
                }
                actions.push_back(PlannedChunkAction::empty);
                result.chunks[index].disposition =
                    DistributedSieveWorkerCoordinationDispositionV1::empty;
                break;
            case resume::DistributedSieveWorkerChunkDurableStateV1::missing:
                if (chunk.sq_begin == chunk.sq_end || observed.latest_attempt.has_value() ||
                    observed.terminal_failure.has_value() || observed.handoff.has_value()) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                index);
                    return result;
                }
                all_empty = false;
                actions.push_back(PlannedChunkAction::launch);
                launch_ordinals[index] = 0U;
                break;
            case resume::DistributedSieveWorkerChunkDurableStateV1::incomplete_attempt:
                all_empty = false;
                if (chunk.sq_begin == chunk.sq_end || !observed.latest_attempt.has_value() ||
                    observed.terminal_failure.has_value() || observed.handoff.has_value() ||
                    !attempt_matches_chunk(*observed.latest_attempt, chunk,
                                           result.store->manifest_digest(),
                                           observed.latest_attempt->attempt_ordinal)) {
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::incomplete_attempt,
                                index);
                    return result;
                }
                actions.push_back(PlannedChunkAction::recover);
                recovery_slots.push_back(index);
                break;
            case resume::DistributedSieveWorkerChunkDurableStateV1::terminal_failure_pending:
            case resume::DistributedSieveWorkerChunkDurableStateV1::terminal_failure:
                all_empty = false;
                if (chunk.sq_begin == chunk.sq_end || !observed.latest_attempt.has_value() ||
                    !observed.terminal_failure.has_value() || observed.handoff.has_value() ||
                    !attempt_matches_chunk(*observed.latest_attempt, chunk,
                                           result.store->manifest_digest(),
                                           observed.latest_attempt->attempt_ordinal) ||
                    observed.latest_attempt->attempt_ordinal + 1U != manifest.max_worker_attempts ||
                    observed.terminal_failure->last_attempt_digest !=
                        observed.latest_attempt->self_digest) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                index);
                    return result;
                }
                if (terminal_recovery_slot.has_value()) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                index);
                    return result;
                }
                terminal_recovery_slot = index;
                actions.push_back(PlannedChunkAction::recover);
                recovery_slots.push_back(index);
                break;
            case resume::DistributedSieveWorkerChunkDurableStateV1::handoff:
                if (chunk.sq_begin == chunk.sq_end || !observed.latest_attempt.has_value() ||
                    observed.terminal_failure.has_value() || !observed.handoff.has_value() ||
                    observed.latest_attempt->self_digest !=
                        observed.handoff->attempt_started_digest ||
                    observed.latest_attempt->attempt_ordinal != observed.handoff->attempt_ordinal ||
                    observed.latest_attempt->lease != observed.handoff->lease ||
                    !handoff_matches_chunk(*observed.handoff, chunk,
                                           result.store->manifest_digest())) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                index);
                    return result;
                }
                all_empty = false;
                actions.push_back(PlannedChunkAction::adopt);
                expected_adopted_handoffs[index] = *observed.handoff;
                break;
            }
        }

        if (terminal_recovery_slot.has_value()) {
            recovery_slots.clear();
            recovery_slots.push_back(*terminal_recovery_slot);
        }

        if (all_empty) {
            result.diagnostic = {};
            result.diagnostic.status = DistributedSieveWorkerCoordinatorStatusV1::succeeded;
            return result;
        }

        const bool execution_may_be_required =
            !terminal_recovery_slot.has_value() &&
            std::any_of(actions.begin(), actions.end(), [](PlannedChunkAction action) {
                return action == PlannedChunkAction::launch ||
                       action == PlannedChunkAction::recover;
            });
        if (execution_may_be_required) {
            active_phase = DistributedSieveWorkerCoordinatorPhaseV1::request_validation;
            active_slot = NO_MANIFEST_SLOT;
            if (!executable_request_is_valid(request)) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::invalid_request;
                result.diagnostic.wave_store.native_error =
                    std::make_error_code(std::errc::invalid_argument);
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::invalid_request);
                return result;
            }
            if (const auto status = validate_manifest_work_identity(manifest, identity); !status) {
                set_protocol_request_failure(result, status);
                return result;
            }
            auto bound = execution::bind_distributed_sieve_work_v1(identity, frozen_policy,
                                                                   polynomial, factor_base);
            if (!bound) {
                set_protocol_request_failure(result, bound.status);
                return result;
            }
            if (bound.work->work_digest != manifest.work_sha256) {
                const DistributedSieveProtocolStatus status{
                    .error = DistributedSieveProtocolError::invalid_value,
                };
                set_protocol_request_failure(result, status);
                return result;
            }
        }

        if (request.coordinator_hooks.after_initial_observation != nullptr) {
            request.coordinator_hooks.after_initial_observation(request.coordinator_hooks.context);
        }

        const auto try_adopt_late_handoff =
            [&](std::size_t manifest_slot) -> LateHandoffObservation {
            active_phase = DistributedSieveWorkerCoordinatorPhaseV1::retry_observation;
            active_slot = manifest_slot;
            auto observed = result.store->observe_worker_chunks_v1();
            if (!observed) {
                result.diagnostic.wave_store = observed.diagnostic;
                set_failure(result, active_phase,
                            wave_failure_status(
                                observed.diagnostic,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed),
                            manifest_slot);
                return LateHandoffObservation::failed;
            }
            if (observed.chunks.size() != manifest.chunks.size() ||
                manifest_slot >= observed.chunks.size()) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                            manifest_slot);
                return LateHandoffObservation::failed;
            }

            const auto& candidate = observed.chunks[manifest_slot];
            const auto& initial_attempt = initial.chunks[manifest_slot].latest_attempt;
            if (candidate.chunk != manifest.chunks[manifest_slot] || !initial_attempt.has_value()) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                            manifest_slot);
                return LateHandoffObservation::failed;
            }
            const bool initial_terminal_prefix =
                terminal_failure_prefix(initial.chunks[manifest_slot].state);
            if (terminal_failure_prefix(candidate.state)) {
                if (!candidate.latest_attempt.has_value() ||
                    !candidate.terminal_failure.has_value() || candidate.handoff.has_value() ||
                    !attempts_equal(*candidate.latest_attempt, *initial_attempt)) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                manifest_slot);
                    return LateHandoffObservation::failed;
                }
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::reconciliation_required;
                set_failure(
                    result, active_phase,
                    DistributedSieveWorkerCoordinatorStatusV1::attempt_reconciliation_failed,
                    manifest_slot);
                return LateHandoffObservation::failed;
            }
            if (!initial_terminal_prefix &&
                candidate.state ==
                    resume::DistributedSieveWorkerChunkDurableStateV1::incomplete_attempt &&
                candidate.latest_attempt.has_value() && !candidate.handoff.has_value() &&
                !candidate.terminal_failure.has_value() &&
                attempts_equal(*candidate.latest_attempt, *initial_attempt)) {
                return LateHandoffObservation::exact_incomplete;
            }
            if (initial_terminal_prefix ||
                candidate.state != resume::DistributedSieveWorkerChunkDurableStateV1::handoff ||
                !candidate.latest_attempt.has_value() || !candidate.handoff.has_value() ||
                candidate.terminal_failure.has_value() ||
                !attempts_equal(*candidate.latest_attempt, *initial_attempt) ||
                candidate.handoff->attempt_started_digest != initial_attempt->self_digest ||
                candidate.handoff->attempt_ordinal != initial_attempt->attempt_ordinal ||
                candidate.handoff->lease != initial_attempt->lease ||
                !handoff_matches_chunk(*candidate.handoff, manifest.chunks[manifest_slot],
                                       result.store->manifest_digest())) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                            manifest_slot);
                return LateHandoffObservation::failed;
            }
            actions[manifest_slot] = PlannedChunkAction::adopt;
            expected_adopted_handoffs[manifest_slot] = *candidate.handoff;
            return LateHandoffObservation::adopted;
        };

        if (!recovery_slots.empty()) {
            active_phase = DistributedSieveWorkerCoordinatorPhaseV1::retry_observation;
            active_slot = NO_MANIFEST_SLOT;
            auto retry_observation = result.store->observe_worker_chunks_v1();
            if (!retry_observation) {
                result.diagnostic.wave_store = retry_observation.diagnostic;
                set_failure(result, active_phase,
                            wave_failure_status(
                                retry_observation.diagnostic,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed));
                return result;
            }
            if (retry_observation.chunks.size() != manifest.chunks.size()) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::observation_failed);
                return result;
            }

            for (const std::size_t manifest_slot : recovery_slots) {
                active_slot = manifest_slot;
                const auto& observed = retry_observation.chunks[manifest_slot];
                const auto& initial_attempt = initial.chunks[manifest_slot].latest_attempt;
                if (observed.chunk != manifest.chunks[manifest_slot] ||
                    !initial_attempt.has_value()) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                manifest_slot);
                    return result;
                }
                const bool initial_terminal_prefix =
                    terminal_failure_prefix(initial.chunks[manifest_slot].state);
                if (terminal_failure_prefix(observed.state)) {
                    if (!observed.latest_attempt.has_value() ||
                        !observed.terminal_failure.has_value() || observed.handoff.has_value() ||
                        !attempts_equal(*observed.latest_attempt, *initial_attempt)) {
                        result.diagnostic.wave_store.status =
                            resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                        set_failure(result, active_phase,
                                    DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                    manifest_slot);
                        return result;
                    }
                    if (!initial_terminal_prefix) {
                        result.diagnostic.wave_store.status =
                            resume::DistributedSieveWaveStoreStatus::reconciliation_required;
                        set_failure(result, active_phase,
                                    DistributedSieveWorkerCoordinatorStatusV1::
                                        attempt_reconciliation_failed,
                                    manifest_slot);
                        return result;
                    }
                    continue;
                }
                if (observed.state == resume::DistributedSieveWorkerChunkDurableStateV1::handoff) {
                    if (initial_terminal_prefix || !observed.latest_attempt.has_value() ||
                        !observed.handoff.has_value() || observed.terminal_failure.has_value() ||
                        !attempts_equal(*observed.latest_attempt, *initial_attempt) ||
                        observed.handoff->attempt_started_digest != initial_attempt->self_digest ||
                        observed.handoff->attempt_ordinal != initial_attempt->attempt_ordinal ||
                        observed.handoff->lease != initial_attempt->lease ||
                        !handoff_matches_chunk(*observed.handoff, manifest.chunks[manifest_slot],
                                               result.store->manifest_digest())) {
                        result.diagnostic.wave_store.status =
                            resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                        set_failure(result, active_phase,
                                    DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                    manifest_slot);
                        return result;
                    }
                    actions[manifest_slot] = PlannedChunkAction::adopt;
                    expected_adopted_handoffs[manifest_slot] = *observed.handoff;
                    continue;
                }
                if (initial_terminal_prefix ||
                    observed.state !=
                        resume::DistributedSieveWorkerChunkDurableStateV1::incomplete_attempt ||
                    !observed.latest_attempt.has_value() || observed.handoff.has_value() ||
                    observed.terminal_failure.has_value() ||
                    !attempts_equal(*observed.latest_attempt, *initial_attempt)) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                manifest_slot);
                    return result;
                }
            }

            if (request.coordinator_hooks.after_retry_observation != nullptr) {
                request.coordinator_hooks.after_retry_observation(
                    request.coordinator_hooks.context);
            }

            for (const std::size_t manifest_slot : recovery_slots) {
                if (actions[manifest_slot] != PlannedChunkAction::recover) {
                    continue;
                }
                active_phase = DistributedSieveWorkerCoordinatorPhaseV1::attempt_reconciliation;
                active_slot = manifest_slot;
                const auto& initial_attempt = *initial.chunks[manifest_slot].latest_attempt;
                auto opened = result.store->open_worker_attempt_private_lease_root(
                    initial_attempt.chunk_id, initial_attempt.attempt_ordinal);
                if (!opened) {
                    if (retry_claim_is_busy(opened.diagnostic)) {
                        if (request.coordinator_hooks.before_retry_busy_observation != nullptr) {
                            request.coordinator_hooks.before_retry_busy_observation(
                                request.coordinator_hooks.context);
                        }
                        const auto late = try_adopt_late_handoff(manifest_slot);
                        if (late == LateHandoffObservation::failed) {
                            return result;
                        }
                        if (late == LateHandoffObservation::adopted) {
                            continue;
                        }
                        result.diagnostic.wave_store = opened.diagnostic;
                        set_failure(
                            result,
                            DistributedSieveWorkerCoordinatorPhaseV1::attempt_reconciliation,
                            DistributedSieveWorkerCoordinatorStatusV1::retry_busy, manifest_slot);
                        return result;
                    }
                    result.diagnostic.wave_store = opened.diagnostic;
                    set_failure(result,
                                DistributedSieveWorkerCoordinatorPhaseV1::attempt_reconciliation,
                                wave_failure_status(opened.diagnostic,
                                                    DistributedSieveWorkerCoordinatorStatusV1::
                                                        attempt_reconciliation_failed),
                                manifest_slot);
                    return result;
                }

                auto reconciled = resume::reconcile_worker_attempt_started(std::move(opened));
                if (!reconciled) {
                    if (reconciled.terminal_handoff.has_value()) {
                        const auto& terminal = *reconciled.terminal_handoff;
                        if (terminal_failure_prefix(initial.chunks[manifest_slot].state) ||
                            reconciled.diagnostic.status !=
                                resume::DistributedSieveWaveStoreStatus::reconciliation_required ||
                            terminal.handoff.chunk_id != initial_attempt.chunk_id ||
                            terminal.handoff.attempt_ordinal != initial_attempt.attempt_ordinal ||
                            terminal.handoff.attempt_started_digest !=
                                initial_attempt.self_digest ||
                            terminal.handoff.lease != initial_attempt.lease ||
                            !handoff_matches_chunk(terminal.handoff, manifest.chunks[manifest_slot],
                                                   result.store->manifest_digest())) {
                            result.diagnostic.wave_store.status =
                                resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                            set_failure(
                                result,
                                DistributedSieveWorkerCoordinatorPhaseV1::attempt_reconciliation,
                                DistributedSieveWorkerCoordinatorStatusV1::
                                    attempt_reconciliation_failed,
                                manifest_slot);
                            return result;
                        }
                        actions[manifest_slot] = PlannedChunkAction::adopt;
                        expected_adopted_handoffs[manifest_slot] = terminal.handoff;
                        expected_adopted_witnesses[manifest_slot] = terminal;
                        continue;
                    }
                    result.diagnostic.wave_store = reconciled.diagnostic;
                    set_failure(result,
                                DistributedSieveWorkerCoordinatorPhaseV1::attempt_reconciliation,
                                wave_failure_status(reconciled.diagnostic,
                                                    DistributedSieveWorkerCoordinatorStatusV1::
                                                        attempt_reconciliation_failed),
                                manifest_slot);
                    return result;
                }
                if (!attempts_equal(reconciled.reconciled->record, initial_attempt)) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(
                        result, active_phase,
                        DistributedSieveWorkerCoordinatorStatusV1::attempt_reconciliation_failed,
                        manifest_slot);
                    return result;
                }
                result.chunks[manifest_slot].reconciled_attempt.emplace(
                    std::move(*reconciled.reconciled));
                const auto next_ordinal =
                    result.chunks[manifest_slot].reconciled_attempt->next_attempt_ordinal;
                const auto expected_next =
                    static_cast<std::uint64_t>(initial_attempt.attempt_ordinal) + 1U;
                const bool retry_budget_exhausted =
                    expected_next >= static_cast<std::uint64_t>(manifest.max_worker_attempts);
                if ((!retry_budget_exhausted &&
                     (!next_ordinal.has_value() ||
                      static_cast<std::uint64_t>(*next_ordinal) != expected_next ||
                      reconciled.terminal_failure_admission.has_value())) ||
                    (retry_budget_exhausted &&
                     (next_ordinal.has_value() ||
                      !reconciled.terminal_failure_admission.has_value()))) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(
                        result, active_phase,
                        DistributedSieveWorkerCoordinatorStatusV1::attempt_reconciliation_failed,
                        manifest_slot);
                    return result;
                }
                if (!next_ordinal.has_value()) {
                    active_phase =
                        DistributedSieveWorkerCoordinatorPhaseV1::terminal_failure_publication;
                    auto published = resume::publish_chunk_terminal_failure_v1(
                        std::move(*reconciled.terminal_failure_admission));
                    if (!published) {
                        result.diagnostic.wave_store = published.diagnostic;
                        set_failure(result, active_phase,
                                    wave_failure_status(published.diagnostic,
                                                        DistributedSieveWorkerCoordinatorStatusV1::
                                                            terminal_failure_publication_failed),
                                    manifest_slot);
                        return result;
                    }
                    resume::DistributedSieveWorkerChunkInventoryV1 terminal{
                        .chunk = manifest.chunks[manifest_slot],
                        .state =
                            resume::DistributedSieveWorkerChunkDurableStateV1::terminal_failure,
                        .latest_attempt = result.chunks[manifest_slot].reconciled_attempt->record,
                        .terminal_failure = std::move(published.terminal_failure),
                    };
                    finish_terminal_failure(active_phase, manifest_slot, std::move(terminal));
                    return result;
                }
                launch_ordinals[manifest_slot] = *next_ordinal;
            }

            active_phase =
                DistributedSieveWorkerCoordinatorPhaseV1::post_reconciliation_observation;
            active_slot = NO_MANIFEST_SLOT;
            auto post_reconciliation = result.store->observe_worker_chunks_v1();
            if (!post_reconciliation) {
                result.diagnostic.wave_store = post_reconciliation.diagnostic;
                set_failure(result, active_phase,
                            wave_failure_status(
                                post_reconciliation.diagnostic,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed));
                return result;
            }
            if (post_reconciliation.chunks.size() != manifest.chunks.size()) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::observation_failed);
                return result;
            }
            for (std::size_t index = 0; index < manifest.chunks.size(); ++index) {
                active_slot = index;
                const auto& observed = post_reconciliation.chunks[index];
                if (observed.chunk != manifest.chunks[index]) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                index);
                    return result;
                }
                if (actions[index] == PlannedChunkAction::recover) {
                    const auto& reconciled = result.chunks[index].reconciled_attempt;
                    if (!reconciled.has_value() || !launch_ordinals[index].has_value() ||
                        observed.state !=
                            resume::DistributedSieveWorkerChunkDurableStateV1::incomplete_attempt ||
                        !observed.latest_attempt.has_value() ||
                        observed.terminal_failure.has_value() || observed.handoff.has_value() ||
                        !attempts_equal(*observed.latest_attempt, reconciled->record)) {
                        result.diagnostic.wave_store.status =
                            resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                        set_failure(result, active_phase,
                                    DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                    index);
                        return result;
                    }
                    actions[index] = PlannedChunkAction::launch;
                    continue;
                }
                if (actions[index] == PlannedChunkAction::adopt) {
                    if (observed.state !=
                            resume::DistributedSieveWorkerChunkDurableStateV1::handoff ||
                        observed.terminal_failure.has_value() || !observed.handoff.has_value() ||
                        !expected_adopted_handoffs[index].has_value() ||
                        !handoffs_equal(*observed.handoff, *expected_adopted_handoffs[index])) {
                        result.diagnostic.wave_store.status =
                            resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                        set_failure(result, active_phase,
                                    DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                    index);
                        return result;
                    }
                    continue;
                }
                if (actions[index] == PlannedChunkAction::launch) {
                    if (observed.state !=
                            resume::DistributedSieveWorkerChunkDurableStateV1::missing ||
                        observed.latest_attempt.has_value() ||
                        observed.terminal_failure.has_value() || observed.handoff.has_value()) {
                        result.diagnostic.wave_store.status =
                            resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                        set_failure(result, active_phase,
                                    DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                    index);
                        return result;
                    }
                    continue;
                }
                if (observed.state != resume::DistributedSieveWorkerChunkDurableStateV1::empty ||
                    observed.latest_attempt.has_value() || observed.terminal_failure.has_value() ||
                    observed.handoff.has_value()) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::observation_failed,
                                index);
                    return result;
                }
            }
        }

        std::vector<std::size_t> launch_manifest_slots;
        launch_manifest_slots.reserve(manifest.chunks.size());
        for (std::size_t index = 0; index < actions.size(); ++index) {
            if (actions[index] == PlannedChunkAction::launch) {
                if (!launch_ordinals[index].has_value()) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(
                        result, DistributedSieveWorkerCoordinatorPhaseV1::attempt_reconciliation,
                        DistributedSieveWorkerCoordinatorStatusV1::attempt_reconciliation_failed,
                        index);
                    return result;
                }
                launch_manifest_slots.push_back(index);
            }
        }

        if (!launch_manifest_slots.empty()) {
            std::vector<std::vector<std::string>> argument_copies;
            argument_copies.reserve(launch_manifest_slots.size());
            for (std::size_t unused = 0; unused < launch_manifest_slots.size(); ++unused) {
                static_cast<void>(unused);
                argument_copies.push_back(request.worker_arguments);
            }

            std::vector<launcher::DistributedSieveWorkerLaunchSlotV1> launch_slots;
            launch_slots.reserve(launch_manifest_slots.size());
            for (std::size_t launch_index = 0; launch_index < launch_manifest_slots.size();
                 ++launch_index) {
                active_slot = launch_manifest_slots[launch_index];
                const auto& chunk = manifest.chunks[active_slot];
                const std::uint32_t attempt_ordinal = *launch_ordinals[active_slot];

                active_phase = DistributedSieveWorkerCoordinatorPhaseV1::attempt_reservation;
                auto root_claim = result.store->create_worker_attempt_private_lease_root(
                    chunk.chunk_id, attempt_ordinal);
                if (!root_claim) {
                    result.diagnostic.wave_store = root_claim.diagnostic;
                    set_failure(
                        result, active_phase,
                        wave_failure_status(
                            root_claim.diagnostic,
                            DistributedSieveWorkerCoordinatorStatusV1::attempt_preparation_failed),
                        active_slot);
                    return result;
                }
                auto reservation =
                    resume::reserve_worker_attempt_private_lease(std::move(root_claim));
                if (!reservation) {
                    result.diagnostic.wave_store = reservation.diagnostic;
                    set_failure(
                        result, active_phase,
                        wave_failure_status(
                            reservation.diagnostic,
                            DistributedSieveWorkerCoordinatorStatusV1::attempt_preparation_failed),
                        active_slot);
                    return result;
                }

                active_phase = DistributedSieveWorkerCoordinatorPhaseV1::attempt_start;
                auto started =
                    resume::publish_worker_attempt_started(std::move(*reservation.receipt));
                if (!started) {
                    result.diagnostic.wave_store = started.diagnostic;
                    set_failure(
                        result, active_phase,
                        wave_failure_status(
                            started.diagnostic,
                            DistributedSieveWorkerCoordinatorStatusV1::attempt_preparation_failed),
                        active_slot);
                    return result;
                }
                const auto& started_record = started.receipt->record();
                const util::Sha256Digest* expected_predecessor = nullptr;
                if (attempt_ordinal == 0U) {
                    expected_predecessor = &manifest.self_digest;
                } else if (result.chunks[active_slot].reconciled_attempt.has_value() &&
                           result.chunks[active_slot].reconciled_attempt->next_attempt_ordinal ==
                               attempt_ordinal) {
                    expected_predecessor =
                        &result.chunks[active_slot].reconciled_attempt->record.self_digest;
                }
                if (expected_predecessor == nullptr ||
                    !attempt_matches_chunk(started_record, chunk, result.store->manifest_digest(),
                                           attempt_ordinal) ||
                    started_record.predecessor_digest != *expected_predecessor ||
                    started_record.retry_policy_version != manifest.retry_policy_version) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(
                        result, active_phase,
                        DistributedSieveWorkerCoordinatorStatusV1::attempt_preparation_failed,
                        active_slot);
                    return result;
                }

                result.chunks[active_slot].launched_attempt.emplace(started_record);
                launch_slots.emplace_back(std::move(*started.receipt),
                                          std::move(argument_copies[launch_index]));
            }

            active_phase = DistributedSieveWorkerCoordinatorPhaseV1::batch_launch;
            active_slot = NO_MANIFEST_SLOT;
            auto launched = result.store->launch_worker_process_batch_v1(
                launcher::DistributedSieveWorkerLaunchRequestV1(std::move(request.executable_path),
                                                                std::move(launch_slots),
                                                                request.launcher_hooks),
                identity, frozen_policy, polynomial, factor_base);
            result.diagnostic.launcher = launched.diagnostic;

            bool exact_launch =
                launched.disposition == launcher::DistributedSieveWorkerLaunchDispositionV1::all &&
                launched.children.size() == launch_manifest_slots.size();
            std::size_t launch_mismatch_slot = NO_MANIFEST_SLOT;
            for (std::size_t index = 0;
                 index < launched.children.size() && index < launch_manifest_slots.size();
                 ++index) {
                const auto& child = launched.children[index];
                const std::size_t manifest_slot = launch_manifest_slots[index];
                const auto& ledger = result.chunks[manifest_slot].launched_attempt;
                if (!child || !ledger.has_value() ||
                    child.chunk_id != result.chunks[manifest_slot].chunk.chunk_id ||
                    child.attempt_ordinal != ledger->attempt_ordinal ||
                    child.attempt_started_digest != ledger->self_digest) {
                    exact_launch = false;
                    if (launch_mismatch_slot == NO_MANIFEST_SLOT) {
                        launch_mismatch_slot = manifest_slot;
                    }
                }
            }
            if (launched.children.size() != launch_manifest_slots.size() &&
                launch_mismatch_slot == NO_MANIFEST_SLOT) {
                launch_mismatch_slot = launched.children.size() < launch_manifest_slots.size()
                                           ? launch_manifest_slots[launched.children.size()]
                                           : NO_MANIFEST_SLOT;
            }

            active_phase = DistributedSieveWorkerCoordinatorPhaseV1::terminal_wait;
            bool every_known_child_reaped = true;
            bool any_known_child = false;
            std::size_t first_uncertain_slot = NO_MANIFEST_SLOT;
            std::optional<WorkerWaitFactsV1> first_uncertain_facts;
            for (auto& child : launched.children) {
                if (!child.worker.has_value()) {
                    continue;
                }
                any_known_child = true;
                const auto waited = child.worker->wait_terminal(request.wait_hooks);
                const auto facts = wait_facts_from(waited);
                const std::size_t manifest_slot =
                    manifest_slot_for_chunk(result.chunks, child.chunk_id);
                if (manifest_slot != NO_MANIFEST_SLOT &&
                    actions[manifest_slot] == PlannedChunkAction::launch) {
                    result.chunks[manifest_slot].wait_facts = facts;
                }
                if (!wait_is_terminal(waited)) {
                    every_known_child_reaped = false;
                    if (!first_uncertain_facts.has_value()) {
                        first_uncertain_slot = manifest_slot;
                        first_uncertain_facts = facts;
                    }
                }
            }

            // A launched-attempt owner releases its receipt ordinarily only
            // after terminal reap; uncertain owners quarantine it. In either
            // case no launch receipt remains live across the final observe.
            for (auto child = launched.children.rbegin(); child != launched.children.rend();
                 ++child) {
                child->worker.reset();
            }

            if (!every_known_child_reaped) {
                result.diagnostic.wait_facts = first_uncertain_facts;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::wait_uncertain,
                            first_uncertain_slot);
                return result;
            }
            if (!exact_launch) {
                const auto status =
                    !any_known_child && launcher_resource_exhausted(launched.diagnostic)
                        ? DistributedSieveWorkerCoordinatorStatusV1::resource_exhausted
                        : DistributedSieveWorkerCoordinatorStatusV1::launch_failed;
                set_failure(result, DistributedSieveWorkerCoordinatorPhaseV1::batch_launch, status,
                            launch_mismatch_slot != NO_MANIFEST_SLOT ? launch_mismatch_slot
                            : launched.diagnostic.slot < launch_manifest_slots.size()
                                ? launch_manifest_slots[launched.diagnostic.slot]
                                : NO_MANIFEST_SLOT);
                return result;
            }
        }

        active_phase = DistributedSieveWorkerCoordinatorPhaseV1::final_observation;
        active_slot = NO_MANIFEST_SLOT;
        auto final_observation = result.store->observe_worker_chunks_v1();
        if (!final_observation) {
            result.diagnostic.wave_store = final_observation.diagnostic;
            set_failure(result, active_phase,
                        wave_failure_status(
                            final_observation.diagnostic,
                            DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed));
            return result;
        }
        if (final_observation.chunks.size() != manifest.chunks.size()) {
            result.diagnostic.wave_store.status =
                resume::DistributedSieveWaveStoreStatus::namespace_conflict;
            set_failure(result, active_phase,
                        DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed);
            return result;
        }

        // A terminal prefix observed without this invocation's retained
        // admission is not yet a durable absorbing result. Stop before
        // adoption and let the next round reacquire the final attempt and run
        // the typed normalizer.
        for (std::size_t index = 0; index < manifest.chunks.size(); ++index) {
            active_slot = index;
            const auto& observed = final_observation.chunks[index];
            if (observed.chunk != manifest.chunks[index]) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed,
                            index);
                return result;
            }
            if (!terminal_failure_prefix(observed.state)) {
                continue;
            }
            if (!observed.latest_attempt.has_value() || !observed.terminal_failure.has_value() ||
                observed.handoff.has_value()) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed,
                            index);
                return result;
            }
            result.diagnostic.wave_store.status =
                resume::DistributedSieveWaveStoreStatus::reconciliation_required;
            set_failure(result, active_phase,
                        DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed, index);
            return result;
        }

        for (std::size_t index = 0; index < manifest.chunks.size(); ++index) {
            active_slot = index;
            const auto& observed = final_observation.chunks[index];
            if (observed.chunk != manifest.chunks[index]) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed,
                            index);
                return result;
            }
            if (observed.state ==
                resume::DistributedSieveWorkerChunkDurableStateV1::incomplete_attempt) {
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::incomplete_attempt, index);
                return result;
            }
            if (actions[index] == PlannedChunkAction::empty) {
                if (observed.state != resume::DistributedSieveWorkerChunkDurableStateV1::empty ||
                    observed.latest_attempt.has_value() || observed.terminal_failure.has_value() ||
                    observed.handoff.has_value()) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed,
                                index);
                    return result;
                }
                continue;
            }

            if (observed.state != resume::DistributedSieveWorkerChunkDurableStateV1::handoff ||
                !observed.latest_attempt.has_value() || observed.terminal_failure.has_value() ||
                !observed.handoff.has_value() ||
                observed.latest_attempt->self_digest != observed.handoff->attempt_started_digest ||
                !handoff_matches_chunk(*observed.handoff, manifest.chunks[index],
                                       result.store->manifest_digest())) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed,
                            index);
                return result;
            }

            if (actions[index] == PlannedChunkAction::adopt) {
                if (!expected_adopted_handoffs[index].has_value() ||
                    !handoffs_equal(*expected_adopted_handoffs[index], *observed.handoff)) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed,
                                index);
                    return result;
                }
                continue;
            }

            const auto& ledger = result.chunks[index].launched_attempt;
            if (!ledger.has_value() || !attempts_equal(*observed.latest_attempt, *ledger) ||
                observed.handoff->attempt_started_digest != ledger->self_digest ||
                observed.handoff->attempt_ordinal != ledger->attempt_ordinal ||
                observed.handoff->lease != ledger->lease) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::final_observation_failed,
                            index);
                return result;
            }
        }

        active_phase = DistributedSieveWorkerCoordinatorPhaseV1::handoff_adoption;
        for (std::size_t index = 0; index < manifest.chunks.size(); ++index) {
            active_slot = index;
            if (actions[index] == PlannedChunkAction::empty) {
                result.chunks[index].disposition =
                    DistributedSieveWorkerCoordinationDispositionV1::empty;
                continue;
            }

            auto adoption =
                expected_adopted_witnesses[index].has_value()
                    ? result.store->adopt_expected_worker_handoff_v1(
                          *expected_adopted_witnesses[index])
                    : result.store->adopt_worker_handoff_v1(manifest.chunks[index].chunk_id);
            if (!adoption) {
                result.diagnostic.wave_store = adoption.diagnostic;
                set_failure(result, active_phase,
                            wave_failure_status(
                                adoption.diagnostic,
                                DistributedSieveWorkerCoordinatorStatusV1::handoff_adoption_failed),
                            index);
                return result;
            }
            const auto& expected = *final_observation.chunks[index].handoff;
            if (!handoffs_equal(adoption.adopted->handoff(), expected)) {
                result.diagnostic.wave_store.status =
                    resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                set_failure(result, active_phase,
                            DistributedSieveWorkerCoordinatorStatusV1::handoff_adoption_failed,
                            index);
                return result;
            }

            if (actions[index] == PlannedChunkAction::launch) {
                const auto& ledger = result.chunks[index].launched_attempt;
                if (!ledger.has_value() ||
                    adoption.adopted->handoff().attempt_started_digest != ledger->self_digest ||
                    adoption.adopted->handoff().attempt_ordinal != ledger->attempt_ordinal ||
                    adoption.adopted->handoff().lease != ledger->lease) {
                    result.diagnostic.wave_store.status =
                        resume::DistributedSieveWaveStoreStatus::namespace_conflict;
                    set_failure(result, active_phase,
                                DistributedSieveWorkerCoordinatorStatusV1::handoff_adoption_failed,
                                index);
                    return result;
                }
                result.chunks[index].disposition =
                    DistributedSieveWorkerCoordinationDispositionV1::executed;
            } else {
                result.chunks[index].disposition =
                    DistributedSieveWorkerCoordinationDispositionV1::adopted;
            }
            result.chunks[index].adopted.emplace(std::move(*adoption.adopted));
        }

        result.diagnostic = {};
        result.diagnostic.status = DistributedSieveWorkerCoordinatorStatusV1::succeeded;
        return result;
    } catch (const std::bad_alloc&) {
        set_resource_exhausted(result, active_phase, active_slot);
        return result;
    } catch (...) {
        result.diagnostic.wave_store.status =
            resume::DistributedSieveWaveStoreStatus::unexpected_failure;
        result.diagnostic.wave_store.native_error = std::make_error_code(std::errc::io_error);
        set_failure(result, active_phase,
                    DistributedSieveWorkerCoordinatorStatusV1::unexpected_failure, active_slot);
        return result;
    }
}

} // namespace gnfs::sieve::distributed_sieve_worker_coordinator_detail
