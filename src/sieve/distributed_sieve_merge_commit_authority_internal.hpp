#pragma once

// Source-private, one-shot authority for publishing the singleton
// WaveMergeCommitV1 and retaining the exact prepared-origin lifetime for the
// later worker-cleanup tail.

#include "distributed_sieve_merge_prepared_admission_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace gnfs::sieve::distributed_sieve_merge_commit_authority_detail {

using distributed_sieve_merge_writer_authority_detail::DistributedSieveCommittedTailAdmissionV1;
using distributed_sieve_merge_writer_authority_detail::DistributedSieveMergePreparedAdmissionV1;
using distributed_sieve_resume_detail::DistributedSieveWaveMergeCommitFaultPointV1;
using distributed_sieve_resume_detail::DistributedSieveWaveMergeCommitTestHooksV1;

enum class DistributedSieveWaveMergeCommitPhaseV1 : std::uint8_t {
    admission_validation,
    platform_gate,
    context_revalidation,
    commit_build,
    dependency_validation,
    publication,
    canonical_revalidation,
    complete,
};

enum class DistributedSieveWaveMergeCommitStatusV1 : std::uint8_t {
    ready,
    invalid_admission,
    platform_unsupported,
    context_invalid,
    commit_build_failed,
    dependency_invalid,
    publication_failed,
    process_mismatch,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_wave_merge_commit_status_name(
    DistributedSieveWaveMergeCommitStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWaveMergeCommitStatusV1::ready:
        return "ready";
    case DistributedSieveWaveMergeCommitStatusV1::invalid_admission:
        return "invalid_admission";
    case DistributedSieveWaveMergeCommitStatusV1::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveWaveMergeCommitStatusV1::context_invalid:
        return "context_invalid";
    case DistributedSieveWaveMergeCommitStatusV1::commit_build_failed:
        return "commit_build_failed";
    case DistributedSieveWaveMergeCommitStatusV1::dependency_invalid:
        return "dependency_invalid";
    case DistributedSieveWaveMergeCommitStatusV1::publication_failed:
        return "publication_failed";
    case DistributedSieveWaveMergeCommitStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveWaveMergeCommitStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWaveMergeCommitStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveWaveMergeCommitDiagnosticV1 final {
    DistributedSieveWaveMergeCommitPhaseV1 phase =
        DistributedSieveWaveMergeCommitPhaseV1::admission_validation;
    DistributedSieveWaveMergeCommitStatusV1 status =
        DistributedSieveWaveMergeCommitStatusV1::unexpected_failure;
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
    DistributedSieveProtocolStatus protocol;
    std::error_code native_error;
    bool admission_spent = false;
    bool reconciliation_required = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return phase == DistributedSieveWaveMergeCommitPhaseV1::complete &&
               status == DistributedSieveWaveMergeCommitStatusV1::ready && admission_spent &&
               !reconciliation_required && !native_error;
    }
};

/// Closed result of one commit attempt.
///
/// Success returns only `committed_tail`. A deterministic failure before any
/// commit prefix exists or publication begins returns only
/// `retryable_prepared`. Once the continuation is spent, failure returns
/// neither and the caller must reopen the wave root.
struct DistributedSieveWaveMergeCommitResultV1 final {
    std::optional<DistributedSieveMergePreparedAdmissionV1> retryable_prepared;
    std::optional<DistributedSieveCommittedTailAdmissionV1> committed_tail;
    DistributedSieveWaveMergeCommitDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        const bool retryable = retryable_prepared.has_value() && retryable_prepared->valid() &&
                               !committed_tail.has_value();
        const bool committed = !retryable_prepared.has_value() && committed_tail.has_value() &&
                               committed_tail->valid();
        return static_cast<bool>(diagnostic) && committed && !retryable;
    }
};

[[nodiscard]] DistributedSieveWaveMergeCommitResultV1 consume_distributed_sieve_merge_prepared_v1(
    DistributedSieveMergePreparedAdmissionV1&& admission) noexcept;

namespace trusted_test {

[[nodiscard]] DistributedSieveWaveMergeCommitResultV1
consume_distributed_sieve_merge_prepared_v1_with_hooks(
    DistributedSieveMergePreparedAdmissionV1&& admission,
    DistributedSieveWaveMergeCommitTestHooksV1 hooks) noexcept;

} // namespace trusted_test

class DistributedSieveWaveMergeCommitAuthorityV1 final {
public:
    DistributedSieveWaveMergeCommitAuthorityV1() = delete;

private:
    [[nodiscard]] static DistributedSieveWaveMergeCommitResultV1
    consume(DistributedSieveMergePreparedAdmissionV1&& admission,
            DistributedSieveWaveMergeCommitTestHooksV1 hooks) noexcept;

    friend DistributedSieveWaveMergeCommitResultV1 consume_distributed_sieve_merge_prepared_v1(
        DistributedSieveMergePreparedAdmissionV1&& admission) noexcept;
    friend DistributedSieveWaveMergeCommitResultV1
    trusted_test::consume_distributed_sieve_merge_prepared_v1_with_hooks(
        DistributedSieveMergePreparedAdmissionV1&& admission,
        DistributedSieveWaveMergeCommitTestHooksV1 hooks) noexcept;
};

} // namespace gnfs::sieve::distributed_sieve_merge_commit_authority_detail
