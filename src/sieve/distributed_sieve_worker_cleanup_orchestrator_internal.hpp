#pragma once

// Source-private typed orchestration from a committed worker-cleanup root to a
// retained, non-armable merged relation view. This file is intentionally not
// installed as public API.

#include "distributed_sieve_worker_cleanup_authority_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace gnfs::relation {
class ReadOnlyRelationCorpusView;
}

namespace gnfs::sieve::distributed_sieve_result_detail {
class DistributedSieveWaveResultAuthorityV1;
}

namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail {

class DistributedSieveWorkerCleanupOrchestrationAuthorityV1;

/// The only four ownership-bearing retry positions that may leave R4. Every
/// raw cleanup root is deliberately normalized through production R3, so a
/// root never needs a caller-selected R1/R2/R3 interpretation.
enum class DistributedSieveWorkerCleanupOrchestrationStageV1 : std::uint8_t {
    authorization_recovery,
    intent_conversion_retry,
    completion_publication_fresh_retry,
    authorization_advance_retry,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_worker_cleanup_orchestration_stage_name(
    DistributedSieveWorkerCleanupOrchestrationStageV1 stage) noexcept {
    switch (stage) {
    case DistributedSieveWorkerCleanupOrchestrationStageV1::authorization_recovery:
        return "authorization_recovery";
    case DistributedSieveWorkerCleanupOrchestrationStageV1::intent_conversion_retry:
        return "intent_conversion_retry";
    case DistributedSieveWorkerCleanupOrchestrationStageV1::completion_publication_fresh_retry:
        return "completion_publication_fresh_retry";
    case DistributedSieveWorkerCleanupOrchestrationStageV1::authorization_advance_retry:
        return "authorization_advance_retry";
    }
    return "unknown";
}

/// Sealed R4 retry continuation. The payload is intentionally private: callers
/// may resume the whole value, but cannot extract a raw root or redirect an
/// authority-bearing child continuation to another stage.
class DistributedSieveWorkerCleanupOrchestrationContinuationV1 final {
public:
    DistributedSieveWorkerCleanupOrchestrationContinuationV1() = delete;
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        const DistributedSieveWorkerCleanupOrchestrationContinuationV1&) = delete;
    DistributedSieveWorkerCleanupOrchestrationContinuationV1&
    operator=(const DistributedSieveWorkerCleanupOrchestrationContinuationV1&) = delete;
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupOrchestrationContinuationV1&&) noexcept;
    DistributedSieveWorkerCleanupOrchestrationContinuationV1&
    operator=(DistributedSieveWorkerCleanupOrchestrationContinuationV1&&) = delete;
    ~DistributedSieveWorkerCleanupOrchestrationContinuationV1() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] DistributedSieveWorkerCleanupOrchestrationStageV1 stage() const noexcept;

private:
    struct AuthorizationRecovery final {
        explicit AuthorizationRecovery(
            DistributedSieveWorkerCleanupRootAdmissionV1&& value) noexcept
            : root(std::move(value)) {}
        DistributedSieveWorkerCleanupRootAdmissionV1 root;
    };

    struct IntentConversionRetry final {
        explicit IntentConversionRetry(
            DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& value) noexcept
            : capsule(std::move(value)) {}
        DistributedSieveWorkerCleanupIntentConversionCapsuleV1 capsule;
    };

    struct CompletionPublicationFreshRetry final {
        explicit CompletionPublicationFreshRetry(
            DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&& value) noexcept
            : completion_ready(std::move(value)) {}
        DistributedSieveWorkerCleanupCompletionReadyCapsuleV1 completion_ready;
    };

    struct AuthorizationAdvanceRetry final {
        explicit AuthorizationAdvanceRetry(
            DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&& value) noexcept
            : completion(std::move(value)) {}
        DistributedSieveWorkerCleanupCompletionPublishedContinuationV1 completion;
    };

    using Payload = std::variant<AuthorizationRecovery, IntentConversionRetry,
                                 CompletionPublicationFreshRetry, AuthorizationAdvanceRetry>;

    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        std::size_t expected_worker_count) noexcept;
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule,
        std::size_t expected_worker_count) noexcept;
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&& completion_ready,
        std::size_t expected_worker_count) noexcept;
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&& completion,
        std::size_t expected_worker_count) noexcept;

    Payload payload_;
    std::size_t expected_worker_count_ = 0;

    friend class DistributedSieveWorkerCleanupOrchestrationAuthorityV1;
};

/// Move-only owner of the terminal R3 proof and its reader-bound public view.
/// The pimpl keeps both objects at stable addresses across moves of this result.
/// A borrowed `merged_relations()` reference must not outlive this owner.
class DistributedSieveWorkerCleanupRetainedMergedResultV1 final {
public:
    DistributedSieveWorkerCleanupRetainedMergedResultV1() = delete;
    DistributedSieveWorkerCleanupRetainedMergedResultV1(
        const DistributedSieveWorkerCleanupRetainedMergedResultV1&) = delete;
    DistributedSieveWorkerCleanupRetainedMergedResultV1&
    operator=(const DistributedSieveWorkerCleanupRetainedMergedResultV1&) = delete;
    DistributedSieveWorkerCleanupRetainedMergedResultV1(
        DistributedSieveWorkerCleanupRetainedMergedResultV1&&) noexcept;
    DistributedSieveWorkerCleanupRetainedMergedResultV1&
    operator=(DistributedSieveWorkerCleanupRetainedMergedResultV1&&) = delete;
    ~DistributedSieveWorkerCleanupRetainedMergedResultV1() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t completed_worker_count() const noexcept;
    [[nodiscard]] const relation::ReadOnlyRelationCorpusView& merged_relations() const&;
    [[nodiscard]] const relation::ReadOnlyRelationCorpusView& merged_relations() const&& = delete;

private:
    struct State;
    explicit DistributedSieveWorkerCleanupRetainedMergedResultV1(
        std::unique_ptr<State> state) noexcept;
    [[nodiscard]] const WaveMergeCommitV1& commit_for_wave_result_promotion_v1() const;

    std::unique_ptr<State> state_;

    friend class DistributedSieveWorkerCleanupOrchestrationAuthorityV1;
    friend class ::gnfs::sieve::distributed_sieve_result_detail::
        DistributedSieveWaveResultAuthorityV1;
};

enum class DistributedSieveWorkerCleanupOrchestrationPhaseV1 : std::uint8_t {
    input_validation,
    authorization_publication,
    completion_preparation,
    intent_conversion_execute,
    completion_publication,
    terminal_retention,
    complete,
};

enum class DistributedSieveWorkerCleanupOrchestrationStatusV1 : std::uint8_t {
    retryable,
    retained,
    invalid_input,
    child_cold_reopen_required,
    ownership_union_invalid,
    progress_budget_exhausted,
    terminal_invalid,
    reader_binding_failed,
    resource_exhausted,
    unexpected_failure,
};

enum class DistributedSieveWorkerCleanupOrchestrationDispositionV1 : std::uint8_t {
    retryable,
    retained,
    cold_reopen_required,
};

struct DistributedSieveWorkerCleanupOrchestrationDiagnosticV1 final {
    DistributedSieveWorkerCleanupOrchestrationPhaseV1 phase =
        DistributedSieveWorkerCleanupOrchestrationPhaseV1::input_validation;
    DistributedSieveWorkerCleanupOrchestrationStatusV1 status =
        DistributedSieveWorkerCleanupOrchestrationStatusV1::unexpected_failure;
    DistributedSieveWorkerCleanupOrchestrationDispositionV1 disposition =
        DistributedSieveWorkerCleanupOrchestrationDispositionV1::cold_reopen_required;
    std::optional<DistributedSieveWorkerCleanupOrchestrationStageV1> retry_stage;
    std::optional<DistributedSieveWorkerCleanupIntentConversionExecuteDiagnosticV1>
        intent_conversion;
    std::optional<DistributedSieveWorkerCleanupCompletionPreparationDiagnosticV1>
        completion_preparation;
    std::optional<DistributedSieveWorkerCleanupCompletionPublicationDiagnosticV1>
        completion_publication;
    std::optional<DistributedSieveWorkerCleanupAuthorizationPublicationDiagnosticV1>
        authorization_publication;
    std::size_t transitions_completed = 0;
    std::size_t transition_budget = 0;
    std::error_code native_error;
};

/// Closed R4 ownership union. A valid result owns exactly one retry
/// continuation or one retained terminal result. A cold-reopen result owns
/// neither.
struct DistributedSieveWorkerCleanupOrchestrationResultV1 final {
    DistributedSieveWorkerCleanupOrchestrationResultV1() = default;
    DistributedSieveWorkerCleanupOrchestrationResultV1(
        std::optional<DistributedSieveWorkerCleanupOrchestrationContinuationV1> retryable_value,
        std::optional<DistributedSieveWorkerCleanupRetainedMergedResultV1> retained_value,
        DistributedSieveWorkerCleanupOrchestrationDiagnosticV1 diagnostic_value) noexcept
        : retryable(std::move(retryable_value)), retained(std::move(retained_value)),
          diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWorkerCleanupOrchestrationResultV1(
        const DistributedSieveWorkerCleanupOrchestrationResultV1&) = delete;
    DistributedSieveWorkerCleanupOrchestrationResultV1&
    operator=(const DistributedSieveWorkerCleanupOrchestrationResultV1&) = delete;
    DistributedSieveWorkerCleanupOrchestrationResultV1(
        DistributedSieveWorkerCleanupOrchestrationResultV1&&) noexcept = default;
    DistributedSieveWorkerCleanupOrchestrationResultV1&
    operator=(DistributedSieveWorkerCleanupOrchestrationResultV1&&) = delete;

    std::optional<DistributedSieveWorkerCleanupOrchestrationContinuationV1> retryable;
    std::optional<DistributedSieveWorkerCleanupRetainedMergedResultV1> retained;
    DistributedSieveWorkerCleanupOrchestrationDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept;
};

[[nodiscard]] DistributedSieveWorkerCleanupOrchestrationResultV1
drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;

[[nodiscard]] DistributedSieveWorkerCleanupOrchestrationResultV1
resume_distributed_sieve_worker_cleanup_to_retained_merged_v1(
    DistributedSieveWorkerCleanupOrchestrationContinuationV1&& continuation) noexcept;

namespace trusted_test {

/// Test-only composition of the already-existing child publication seams. R4
/// adds no independent mutation hook and production never consults this value.
struct DistributedSieveWorkerCleanupOrchestrationTestHooksV1 final {
    DistributedSieveWorkerCleanupAuthorizationPublicationTestHooksV1 authorization_publication;
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2
        intent_publication;
    DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1 completion_publication;
};

[[nodiscard]] DistributedSieveWorkerCleanupOrchestrationResultV1
drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root,
    DistributedSieveWorkerCleanupOrchestrationTestHooksV1 hooks) noexcept;

} // namespace trusted_test

class DistributedSieveWorkerCleanupOrchestrationAuthorityV1 final {
public:
    DistributedSieveWorkerCleanupOrchestrationAuthorityV1() = delete;

private:
    struct ChildOperations;

    [[nodiscard]] static DistributedSieveWorkerCleanupOrchestrationResultV1
    start(DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupOrchestrationResultV1
    drive(DistributedSieveWorkerCleanupOrchestrationContinuationV1&& continuation) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupOrchestrationResultV1 start_with_hooks(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        trusted_test::DistributedSieveWorkerCleanupOrchestrationTestHooksV1 hooks) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupOrchestrationResultV1
    start_with_operations(DistributedSieveWorkerCleanupRootAdmissionV1&& root,
                          const ChildOperations& operations) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupOrchestrationResultV1
    drive_with_operations(DistributedSieveWorkerCleanupOrchestrationContinuationV1&& continuation,
                          const ChildOperations& operations) noexcept;

    friend DistributedSieveWorkerCleanupOrchestrationResultV1
    drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;
    friend DistributedSieveWorkerCleanupOrchestrationResultV1
    resume_distributed_sieve_worker_cleanup_to_retained_merged_v1(
        DistributedSieveWorkerCleanupOrchestrationContinuationV1&& continuation) noexcept;
    friend DistributedSieveWorkerCleanupOrchestrationResultV1
    trusted_test::drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        trusted_test::DistributedSieveWorkerCleanupOrchestrationTestHooksV1 hooks) noexcept;
};

} // namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail
