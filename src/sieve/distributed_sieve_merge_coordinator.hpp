#pragma once

// Source-private transition from a complete worker-coordinator result to one
// durable merged-generation start. The admission is a lifetime root only; it
// exposes no namespace mutation or artifact authority.

#include "distributed_sieve_worker_coordinator_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace gnfs::sieve::distributed_sieve_merge_coordinator_detail {

using namespace distributed_sieve_worker_coordinator_detail;

enum class DistributedSieveMergeCoordinatorPhaseV1 : std::uint8_t {
    request_validation,
    generation_cursor,
    reservation,
    start_publication,
};

enum class DistributedSieveMergeCoordinatorStatusV1 : std::uint8_t {
    admitted,
    invalid_worker_result,
    terminal_worker_failure,
    generation_unavailable,
    reservation_failed,
    start_publication_failed,
    resource_exhausted,
    unexpected_failure,
};

struct DistributedSieveMergeCoordinatorDiagnosticV1 final {
    DistributedSieveMergeCoordinatorPhaseV1 phase =
        DistributedSieveMergeCoordinatorPhaseV1::request_validation;
    DistributedSieveMergeCoordinatorStatusV1 status =
        DistributedSieveMergeCoordinatorStatusV1::unexpected_failure;
    std::size_t manifest_slot = static_cast<std::size_t>(-1);
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
};

class DistributedSieveMergeGenerationAdmissionV1 final {
public:
    DistributedSieveMergeGenerationAdmissionV1() = delete;
    DistributedSieveMergeGenerationAdmissionV1(const DistributedSieveMergeGenerationAdmissionV1&) =
        delete;
    DistributedSieveMergeGenerationAdmissionV1&
    operator=(const DistributedSieveMergeGenerationAdmissionV1&) = delete;
    DistributedSieveMergeGenerationAdmissionV1(
        DistributedSieveMergeGenerationAdmissionV1&&) noexcept = default;
    DistributedSieveMergeGenerationAdmissionV1&
    operator=(DistributedSieveMergeGenerationAdmissionV1&&) = delete;
    ~DistributedSieveMergeGenerationAdmissionV1() = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const DistributedSieveMergeCoordinatorDiagnosticV1& diagnostic() const noexcept;
    [[nodiscard]] const distributed_sieve_resume_detail::DistributedSieveMergeStartedReceiptV1*
    started_receipt() const noexcept;

private:
    explicit DistributedSieveMergeGenerationAdmissionV1(
        DistributedSieveWorkerCoordinatorResultV1&& worker_result,
        distributed_sieve_resume_detail::DistributedSieveMergeStartResultV1&& started,
        DistributedSieveMergeCoordinatorDiagnosticV1 diagnostic) noexcept
        : worker_result_(std::move(worker_result)), started_receipt_(std::move(started.receipt)),
          diagnostic_(std::move(diagnostic)) {}

    DistributedSieveWorkerCoordinatorResultV1 worker_result_;
    std::optional<distributed_sieve_resume_detail::DistributedSieveMergeStartedReceiptV1>
        started_receipt_;
    DistributedSieveMergeCoordinatorDiagnosticV1 diagnostic_;

    friend DistributedSieveMergeGenerationAdmissionV1
    begin_or_resume_distributed_sieve_merge_generation_v1(
        DistributedSieveWorkerCoordinatorResultV1&& worker_result) noexcept;
};

[[nodiscard]] DistributedSieveMergeGenerationAdmissionV1
begin_or_resume_distributed_sieve_merge_generation_v1(
    DistributedSieveWorkerCoordinatorResultV1&& worker_result) noexcept;

} // namespace gnfs::sieve::distributed_sieve_merge_coordinator_detail
