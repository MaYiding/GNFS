#pragma once

// Source-private execution and bounded-retry coordinator for one durable sieve
// wave.
//
// The coordinator consumes exclusive ownership of a WaveStore, claims its
// whole-round gate, classifies every manifest chunk, reconciles quiescent
// incomplete attempts, starts one fixed batch containing only missing or
// authorized retry chunks, reaps every known child, and finally adopts all
// canonical handoffs through same-handle readers.

#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_launcher_internal.hpp"
#include "distributed_sieve_worker_process_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gnfs::sieve::distributed_sieve_worker_coordinator_detail {

enum class DistributedSieveWorkerCoordinationDispositionV1 : std::uint8_t {
    adopted,
    executed,
    empty,
};

enum class DistributedSieveWorkerCoordinatorPhaseV1 : std::uint8_t {
    none,
    request_validation,
    coordinator_claim,
    initial_observation,
    retry_observation,
    attempt_reconciliation,
    post_reconciliation_observation,
    attempt_reservation,
    attempt_start,
    batch_launch,
    terminal_wait,
    final_observation,
    handoff_adoption,
};

enum class DistributedSieveWorkerCoordinatorStatusV1 : std::uint8_t {
    succeeded,
    invalid_request,
    coordinator_busy,
    observation_failed,
    incomplete_attempt,
    retry_busy,
    retry_exhausted,
    attempt_reconciliation_failed,
    attempt_preparation_failed,
    launch_failed,
    wait_uncertain,
    final_observation_failed,
    handoff_adoption_failed,
    resource_exhausted,
    unexpected_failure,
};

struct DistributedSieveWorkerCoordinatorDiagnosticV1 final {
    DistributedSieveWorkerCoordinatorPhaseV1 phase = DistributedSieveWorkerCoordinatorPhaseV1::none;
    DistributedSieveWorkerCoordinatorStatusV1 status =
        DistributedSieveWorkerCoordinatorStatusV1::unexpected_failure;
    std::size_t manifest_slot = static_cast<std::size_t>(-1);
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
    distributed_sieve_worker_launcher_detail::DistributedSieveWorkerLaunchDiagnosticV1 launcher;
    std::optional<WorkerWaitFactsV1> wait_facts;
};

/// Trusted test-only coordinator observation boundary. Production callers
/// leave the callback empty.
struct DistributedSieveWorkerCoordinatorTestHooksV1 final {
    using ObservationBoundary = void (*)(void* context) noexcept;

    ObservationBoundary after_initial_observation = nullptr;
    ObservationBoundary after_retry_observation = nullptr;
    ObservationBoundary before_retry_busy_observation = nullptr;
    void* context = nullptr;
};

/// Complete owned request for one coordinator round.
///
/// Arguments exclude argv[0] and are copied into every missing or
/// reconciler-authorized retry launch slot. The request never accepts
/// descriptors, paths below the frozen wave root, attempt records, or
/// caller-selected ordinals.
struct DistributedSieveWorkerCoordinatorRequestV1 final {
    std::unique_ptr<distributed_sieve_resume_detail::DistributedSieveWaveStore> store;
    std::string executable_path;
    std::vector<std::string> worker_arguments;
    distributed_sieve_worker_launcher_detail::DistributedSieveWorkerLauncherTestHooksV1
        launcher_hooks;
    distributed_sieve_worker_process_detail::DistributedSieveWorkerProcessWaitTestHooks wait_hooks;
    DistributedSieveWorkerCoordinatorTestHooksV1 coordinator_hooks;
};

/// One terminal manifest-ordered chunk result.
///
/// `executed` is used only when the final adopted handoff matches the exact
/// AttemptStartedV1 digest placed in this invocation's launch ledger.
struct DistributedSieveWorkerCoordinatedChunkV1 final {
    ChunkPlanV1 chunk;
    DistributedSieveWorkerCoordinationDispositionV1 disposition =
        DistributedSieveWorkerCoordinationDispositionV1::empty;
    std::optional<AttemptStartedV1> launched_attempt;
    std::optional<distributed_sieve_resume_detail::DistributedSieveReconciledWorkerAttemptV1>
        reconciled_attempt;
    std::optional<WorkerWaitFactsV1> wait_facts;
    std::optional<distributed_sieve_resume_detail::DistributedSieveAdoptedWorkerChunkV1> adopted;

    DistributedSieveWorkerCoordinatedChunkV1() = default;
    DistributedSieveWorkerCoordinatedChunkV1(const DistributedSieveWorkerCoordinatedChunkV1&) =
        delete;
    DistributedSieveWorkerCoordinatedChunkV1&
    operator=(const DistributedSieveWorkerCoordinatedChunkV1&) = delete;
    DistributedSieveWorkerCoordinatedChunkV1(DistributedSieveWorkerCoordinatedChunkV1&&) noexcept =
        default;
    DistributedSieveWorkerCoordinatedChunkV1&
    operator=(DistributedSieveWorkerCoordinatedChunkV1&&) = delete;
    ~DistributedSieveWorkerCoordinatedChunkV1() = default;
};

/// Whole-round result and lifetime root.
///
/// Declaration order is intentional: reverse destruction closes adopted
/// readers first, releases the coordinator gate second, and drops the
/// permanent WaveLock last.
struct DistributedSieveWorkerCoordinatorResultV1 final {
    std::unique_ptr<distributed_sieve_resume_detail::DistributedSieveWaveStore> store;
    std::unique_ptr<distributed_sieve_resume_detail::DistributedSieveWorkerCoordinatorClaimV1>
        coordinator_claim;
    std::vector<DistributedSieveWorkerCoordinatedChunkV1> chunks;
    DistributedSieveWorkerCoordinatorDiagnosticV1 diagnostic;

    DistributedSieveWorkerCoordinatorResultV1() = default;
    DistributedSieveWorkerCoordinatorResultV1(const DistributedSieveWorkerCoordinatorResultV1&) =
        delete;
    DistributedSieveWorkerCoordinatorResultV1&
    operator=(const DistributedSieveWorkerCoordinatorResultV1&) = delete;
    DistributedSieveWorkerCoordinatorResultV1(
        DistributedSieveWorkerCoordinatorResultV1&&) noexcept = default;
    DistributedSieveWorkerCoordinatorResultV1&
    operator=(DistributedSieveWorkerCoordinatorResultV1&&) = delete;
    ~DistributedSieveWorkerCoordinatorResultV1() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return store != nullptr && coordinator_claim != nullptr &&
               coordinator_claim->owned_by_current_process() &&
               diagnostic.status == DistributedSieveWorkerCoordinatorStatusV1::succeeded;
    }
};

[[nodiscard]] DistributedSieveWorkerCoordinatorResultV1
coordinate_missing_distributed_sieve_workers_v1(
    DistributedSieveWorkerCoordinatorRequestV1&& request,
    const DistributedSieveWorkIdentityV1& identity,
    const distributed_sieve_execution_policy_detail::DistributedSieveFrozenExecutionPolicyV1&
        frozen_policy,
    const core::PolynomialContext& polynomial, const factor_base::FactorBase& factor_base) noexcept;

} // namespace gnfs::sieve::distributed_sieve_worker_coordinator_detail
