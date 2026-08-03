#pragma once

// Source-private one-shot promotion from the completed R4 cleanup authority
// into the public least-authority durable wave result.

#include "distributed_sieve_worker_cleanup_orchestrator_internal.hpp"

#include <gnfs/sieve/distributed_sieve.hpp>

#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>

namespace gnfs::sieve::distributed_sieve_result_detail {

using RetainedMergedResultV1 = distributed_sieve_worker_cleanup_authority_detail::
    DistributedSieveWorkerCleanupRetainedMergedResultV1;

enum class DistributedSieveWaveResultPromotionPhaseV1 : std::uint8_t {
    input_validation,
    projection_freeze,
    state_allocation,
    complete,
};

enum class DistributedSieveWaveResultPromotionStatusV1 : std::uint8_t {
    promoted,
    invalid_input,
    projection_mismatch,
    resource_exhausted,
    unexpected_failure,
};

enum class DistributedSieveWaveResultPromotionDispositionV1 : std::uint8_t {
    promoted,
    retryable,
    cold_reopen_required,
};

struct DistributedSieveWaveResultPromotionDiagnosticV1 final {
    DistributedSieveWaveResultPromotionPhaseV1 phase =
        DistributedSieveWaveResultPromotionPhaseV1::input_validation;
    DistributedSieveWaveResultPromotionStatusV1 status =
        DistributedSieveWaveResultPromotionStatusV1::unexpected_failure;
    DistributedSieveWaveResultPromotionDispositionV1 disposition =
        DistributedSieveWaveResultPromotionDispositionV1::cold_reopen_required;
    std::error_code native_error;
};

/// Closed promotion ownership union. A successful result owns only the public
/// least-authority value. A pre-spend allocation failure returns the unchanged
/// R4 owner for retry. Invalid or contradictory input returns neither arm.
struct DistributedSieveWaveResultPromotionResultV1 final {
    DistributedSieveWaveResultPromotionResultV1() = default;
    DistributedSieveWaveResultPromotionResultV1(
        std::optional<RetainedMergedResultV1> retryable_value,
        std::optional<DistributedSieveWaveResult> promoted_value,
        DistributedSieveWaveResultPromotionDiagnosticV1 diagnostic_value) noexcept
        : retryable(std::move(retryable_value)), promoted(std::move(promoted_value)),
          diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWaveResultPromotionResultV1(
        const DistributedSieveWaveResultPromotionResultV1&) = delete;
    DistributedSieveWaveResultPromotionResultV1&
    operator=(const DistributedSieveWaveResultPromotionResultV1&) = delete;
    DistributedSieveWaveResultPromotionResultV1(
        DistributedSieveWaveResultPromotionResultV1&&) noexcept = default;
    DistributedSieveWaveResultPromotionResultV1&
    operator=(DistributedSieveWaveResultPromotionResultV1&&) = delete;

    std::optional<RetainedMergedResultV1> retryable;
    std::optional<DistributedSieveWaveResult> promoted;
    DistributedSieveWaveResultPromotionDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept;
};

[[nodiscard]] DistributedSieveWaveResultPromotionResultV1
promote_distributed_sieve_wave_result_v1(RetainedMergedResultV1&& retained) noexcept;

namespace trusted_test {

enum class DistributedSieveWaveResultPromotionFaultPointV1 : std::uint8_t {
    before_state_allocation,
};

struct DistributedSieveWaveResultPromotionTestHooksV1 final {
    using StopBefore = bool (*)(DistributedSieveWaveResultPromotionFaultPointV1 point,
                                void* context) noexcept;

    StopBefore stop_before = nullptr;
    void* context = nullptr;
};

[[nodiscard]] DistributedSieveWaveResultPromotionResultV1
promote_distributed_sieve_wave_result_v1_with_hooks(
    RetainedMergedResultV1&& retained,
    DistributedSieveWaveResultPromotionTestHooksV1 hooks) noexcept;

} // namespace trusted_test

class DistributedSieveWaveResultAuthorityV1 final {
public:
    DistributedSieveWaveResultAuthorityV1() = delete;

private:
    [[nodiscard]] static DistributedSieveWaveResultPromotionResultV1
    promote(RetainedMergedResultV1&& retained,
            trusted_test::DistributedSieveWaveResultPromotionTestHooksV1 hooks) noexcept;

    friend DistributedSieveWaveResultPromotionResultV1
    promote_distributed_sieve_wave_result_v1(RetainedMergedResultV1&& retained) noexcept;
    friend DistributedSieveWaveResultPromotionResultV1
    trusted_test::promote_distributed_sieve_wave_result_v1_with_hooks(
        RetainedMergedResultV1&& retained,
        trusted_test::DistributedSieveWaveResultPromotionTestHooksV1 hooks) noexcept;
};

} // namespace gnfs::sieve::distributed_sieve_result_detail
