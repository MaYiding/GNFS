#pragma once

// Source-private, path-free execution of one exact distributed-sieve chunk.
// Preparation constructs every allocation-heavy sieve/cofactor object and
// preflights every reachable lattice projection before a caller transfers
// writer authority. Execution emits accepted relations only through the narrow
// callback below.

#include "distributed_sieve_bound_work_internal.hpp"

#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace gnfs::core {
struct Relation;
}

namespace gnfs::core {
class PolynomialContext;
}

namespace gnfs::factor_base {
class FactorBase;
}

namespace gnfs::sieve::distributed_sieve_worker_execution_detail {

struct DistributedSieveWorkerRelationSinkV1 final {
    using Append = bool (*)(void* context, const core::Relation& relation) noexcept;

    void* context = nullptr;
    Append append = nullptr;
};

enum class DistributedSieveWorkerChunkStatusV1 : std::uint8_t {
    ready,
    completed,
    already_executed,
    binding_invalid,
    sink_invalid,
    sink_failed,
    resource_exhausted,
    execution_failed,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_worker_chunk_status_name(DistributedSieveWorkerChunkStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerChunkStatusV1::ready:
        return "ready";
    case DistributedSieveWorkerChunkStatusV1::completed:
        return "completed";
    case DistributedSieveWorkerChunkStatusV1::already_executed:
        return "already_executed";
    case DistributedSieveWorkerChunkStatusV1::binding_invalid:
        return "binding_invalid";
    case DistributedSieveWorkerChunkStatusV1::sink_invalid:
        return "sink_invalid";
    case DistributedSieveWorkerChunkStatusV1::sink_failed:
        return "sink_failed";
    case DistributedSieveWorkerChunkStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerChunkStatusV1::execution_failed:
        return "execution_failed";
    }
    return "unknown";
}

enum class DistributedSieveWorkerAdmissionStatusV1 : std::uint8_t {
    accepted,
    invalid,
    duplicate,
    sink_failed,
    resource_exhausted,
    unexpected_failure,
};

/// Deterministic first-AB-wins admission boundary shared by production chunk
/// execution and its direct invariant tests.
class DistributedSieveWorkerRelationAdmissionV1 final {
public:
    explicit DistributedSieveWorkerRelationAdmissionV1(const core::PolynomialContext& polynomial);
    DistributedSieveWorkerRelationAdmissionV1(const DistributedSieveWorkerRelationAdmissionV1&) =
        delete;
    DistributedSieveWorkerRelationAdmissionV1&
    operator=(const DistributedSieveWorkerRelationAdmissionV1&) = delete;
    DistributedSieveWorkerRelationAdmissionV1(DistributedSieveWorkerRelationAdmissionV1&&) = delete;
    DistributedSieveWorkerRelationAdmissionV1&
    operator=(DistributedSieveWorkerRelationAdmissionV1&&) = delete;
    ~DistributedSieveWorkerRelationAdmissionV1() noexcept;

    [[nodiscard]] DistributedSieveWorkerAdmissionStatusV1
    admit(const core::Relation& relation, DistributedSieveWorkerRelationSinkV1 sink) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

struct DistributedSieveWorkerChunkCompletionV1 final {
    std::uint64_t processed_sq_count = 0;
    std::uint32_t next_sq_index = 0;
    WorkerCompletionReasonV1 completion_reason = WorkerCompletionReasonV1::range_exhausted;

    [[nodiscard]] friend constexpr bool
    operator==(const DistributedSieveWorkerChunkCompletionV1&,
               const DistributedSieveWorkerChunkCompletionV1&) noexcept = default;
};

struct DistributedSieveWorkerChunkExecutionResultV1 final {
    std::optional<DistributedSieveWorkerChunkCompletionV1> completion;
    DistributedSieveWorkerChunkStatusV1 status =
        DistributedSieveWorkerChunkStatusV1::execution_failed;
    std::uint64_t accepted_relation_count = 0;
    std::uint64_t duplicate_relation_count = 0;
    std::uint64_t invalid_relation_count = 0;
    bool artifacts_may_remain = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return completion.has_value() && status == DistributedSieveWorkerChunkStatusV1::completed;
    }
};

struct DistributedSieveWorkerChunkPrepareResultV1;

class PreparedDistributedSieveWorkerChunkV1 final {
public:
    PreparedDistributedSieveWorkerChunkV1() = delete;
    PreparedDistributedSieveWorkerChunkV1(const PreparedDistributedSieveWorkerChunkV1&) = delete;
    PreparedDistributedSieveWorkerChunkV1&
    operator=(const PreparedDistributedSieveWorkerChunkV1&) = delete;
    PreparedDistributedSieveWorkerChunkV1(PreparedDistributedSieveWorkerChunkV1&&) noexcept;
    PreparedDistributedSieveWorkerChunkV1&
    operator=(PreparedDistributedSieveWorkerChunkV1&&) = delete;
    ~PreparedDistributedSieveWorkerChunkV1() noexcept;

    [[nodiscard]] DistributedSieveWorkerChunkExecutionResultV1
    execute(DistributedSieveWorkerRelationSinkV1 sink) noexcept;

private:
    struct State;
    explicit PreparedDistributedSieveWorkerChunkV1(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend struct DistributedSieveWorkerChunkPrepareResultV1;
    friend DistributedSieveWorkerChunkPrepareResultV1 prepare_distributed_sieve_worker_chunk_v1(
        const core::PolynomialContext& polynomial, const factor_base::FactorBase& factor_base,
        const distributed_sieve_execution_policy_detail::DistributedSieveBoundWorkV1& work,
        const ChunkPlanV1& chunk) noexcept;
};

struct DistributedSieveWorkerChunkPrepareResultV1 final {
    std::optional<PreparedDistributedSieveWorkerChunkV1> prepared;
    DistributedSieveWorkerChunkStatusV1 status =
        DistributedSieveWorkerChunkStatusV1::binding_invalid;

    [[nodiscard]] explicit operator bool() const noexcept {
        return prepared.has_value() && status == DistributedSieveWorkerChunkStatusV1::ready;
    }
};

[[nodiscard]] DistributedSieveWorkerChunkPrepareResultV1 prepare_distributed_sieve_worker_chunk_v1(
    const core::PolynomialContext& polynomial, const factor_base::FactorBase& factor_base,
    const distributed_sieve_execution_policy_detail::DistributedSieveBoundWorkV1& work,
    const ChunkPlanV1& chunk) noexcept;

// The prepared chunk borrows polynomial, factor_base, and work. All three
// providers must outlive the prepared value and its one execute() call. The
// policy checker restricts production composition to the owning runtime facade.

} // namespace gnfs::sieve::distributed_sieve_worker_execution_detail
