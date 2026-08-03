#include "distributed_sieve_worker_cleanup_orchestrator_internal.hpp"

#include <gnfs/relation/relation_corpus.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail {
namespace {

using OrchestrationContinuation = DistributedSieveWorkerCleanupOrchestrationContinuationV1;
using OrchestrationDiagnostic = DistributedSieveWorkerCleanupOrchestrationDiagnosticV1;
using OrchestrationDisposition = DistributedSieveWorkerCleanupOrchestrationDispositionV1;
using OrchestrationPhase = DistributedSieveWorkerCleanupOrchestrationPhaseV1;
using OrchestrationResult = DistributedSieveWorkerCleanupOrchestrationResultV1;
using OrchestrationStage = DistributedSieveWorkerCleanupOrchestrationStageV1;
using OrchestrationStatus = DistributedSieveWorkerCleanupOrchestrationStatusV1;
using RetainedMergedResult = DistributedSieveWorkerCleanupRetainedMergedResultV1;

using AuthorizationContinuation = DistributedSieveWorkerCleanupAuthorizationPublishedContinuationV1;
using AuthorizationResult = DistributedSieveWorkerCleanupAuthorizationPublicationResultV1;
using AuthorizationDisposition = DistributedSieveWorkerCleanupAuthorizationPublicationDispositionV1;
using CompletionPreparationResult = DistributedSieveWorkerCleanupCompletionPreparationResultV1;
using CompletionPreparationDisposition =
    DistributedSieveWorkerCleanupCompletionPreparationDispositionV1;
using CompletionPublicationResult = DistributedSieveWorkerCleanupCompletionPublicationResultV1;
using CompletionPublicationDisposition =
    DistributedSieveWorkerCleanupCompletionPublicationDispositionV1;
using CompletionPublishedContinuation =
    DistributedSieveWorkerCleanupCompletionPublishedContinuationV1;
using CompletionReady = DistributedSieveWorkerCleanupCompletionReadyCapsuleV1;
using IntentConversionCapsule = DistributedSieveWorkerCleanupIntentConversionCapsuleV1;
using IntentConversionResult = DistributedSieveWorkerCleanupIntentConversionExecuteResultV1;
using RootAdmission = DistributedSieveWorkerCleanupRootAdmissionV1;
using TerminalContinuation = DistributedSieveWorkerCleanupAllWorkersCompletedContinuationV1;

struct AuthorizationRecoveryState final {
    explicit AuthorizationRecoveryState(RootAdmission&& value) noexcept : root(std::move(value)) {}
    RootAdmission root;
};

struct IntentConversionState final {
    explicit IntentConversionState(IntentConversionCapsule&& value) noexcept
        : capsule(std::move(value)) {}
    IntentConversionCapsule capsule;
};

struct CompletionPublicationFreshState final {
    explicit CompletionPublicationFreshState(CompletionReady&& value) noexcept
        : completion_ready(std::move(value)) {}
    CompletionReady completion_ready;
};

struct AuthorizationAdvanceState final {
    explicit AuthorizationAdvanceState(CompletionPublishedContinuation&& value) noexcept
        : completion(std::move(value)) {}
    CompletionPublishedContinuation completion;
};

struct CompletionPreparationState final {
    explicit CompletionPreparationState(AuthorizationContinuation&& value) noexcept
        : authorization(std::move(value)) {}
    AuthorizationContinuation authorization;
};

struct CompletionPublicationRecoveryState final {
    explicit CompletionPublicationRecoveryState(RootAdmission&& value) noexcept
        : root(std::move(value)) {}
    RootAdmission root;
};

struct TerminalRetentionState final {
    explicit TerminalRetentionState(TerminalContinuation&& value) noexcept
        : terminal(std::move(value)) {}
    TerminalContinuation terminal;
};

using DriveState =
    std::variant<AuthorizationRecoveryState, IntentConversionState, CompletionPublicationFreshState,
                 AuthorizationAdvanceState, CompletionPreparationState,
                 CompletionPublicationRecoveryState, TerminalRetentionState>;

[[nodiscard]] OrchestrationDiagnostic diagnostic(OrchestrationPhase phase,
                                                 OrchestrationStatus status,
                                                 std::error_code native_error = {}) noexcept {
    OrchestrationDiagnostic outcome;
    outcome.phase = phase;
    outcome.status = status;
    outcome.native_error = native_error;
    return outcome;
}

[[nodiscard]] OrchestrationResult cold_reopen(OrchestrationDiagnostic outcome) noexcept {
    outcome.disposition = OrchestrationDisposition::cold_reopen_required;
    outcome.retry_stage.reset();
    if (!outcome.native_error) {
        outcome.native_error = std::make_error_code(std::errc::state_not_recoverable);
    }
    return {std::nullopt, std::nullopt, std::move(outcome)};
}

[[nodiscard]] OrchestrationResult retryable(OrchestrationContinuation&& continuation,
                                            OrchestrationDiagnostic outcome) noexcept {
    const OrchestrationStage retry_stage = continuation.stage();
    std::optional<OrchestrationContinuation> retry;
    retry.emplace(std::move(continuation));
    outcome.phase = OrchestrationPhase::complete;
    outcome.status = OrchestrationStatus::retryable;
    outcome.disposition = OrchestrationDisposition::retryable;
    outcome.retry_stage = retry_stage;
    outcome.native_error.clear();
    return {std::move(retry), std::nullopt, std::move(outcome)};
}

[[nodiscard]] std::optional<std::size_t>
transition_budget_for(std::size_t expected_worker_count) noexcept {
    constexpr std::size_t overhead = 4U;
    constexpr std::size_t transitions_per_worker = 4U;
    if (expected_worker_count >
        (std::numeric_limits<std::size_t>::max() - overhead) / transitions_per_worker) {
        return std::nullopt;
    }
    return expected_worker_count * transitions_per_worker + overhead;
}

[[nodiscard]] std::size_t expected_worker_count(const RootAdmission& root) {
    return static_cast<std::size_t>(std::count_if(
        root.commit().chunks.begin(), root.commit().chunks.end(), [](const auto& chunk) {
            return chunk.input.disposition != gnfs::sieve::ChunkDispositionV1::empty;
        }));
}

template <typename... Optionals>
[[nodiscard]] bool exactly_one_present(const Optionals&... values) noexcept {
    return (static_cast<std::size_t>(values.has_value()) + ...) == 1U;
}

template <typename... Optionals>
[[nodiscard]] bool none_present(const Optionals&... values) noexcept {
    return (!values.has_value() && ...);
}

[[nodiscard]] std::error_code child_error(std::error_code error) noexcept {
    return error ? error : std::make_error_code(std::errc::state_not_recoverable);
}

} // namespace

struct DistributedSieveWorkerCleanupRetainedMergedResultV1::State final {
    State(TerminalContinuation&& terminal_value,
          const relation::OOCRelationReader* expected_reader_value,
          WaveMergeCommitV1 commit_value) noexcept
        : terminal(std::move(terminal_value)), expected_reader(expected_reader_value),
          commit(std::move(commit_value)) {}

    // Reverse destruction releases the borrowed view before the terminal root,
    // merged reader, and WaveLock.
    TerminalContinuation terminal;
    const relation::OOCRelationReader* expected_reader = nullptr;
    WaveMergeCommitV1 commit;
    std::optional<relation::ReadOnlyRelationCorpusView> merged_relations;
};

struct DistributedSieveWorkerCleanupOrchestrationAuthorityV1::ChildOperations final {
    bool trusted_test_hooks = false;
    trusted_test::DistributedSieveWorkerCleanupOrchestrationTestHooksV1 hooks;

    [[nodiscard]] AuthorizationResult resume_authorization(RootAdmission&& root) const noexcept {
        return trusted_test_hooks
                   ? trusted_test::
                         resume_distributed_sieve_worker_cleanup_authorization_v1_with_hooks(
                             std::move(root), hooks.authorization_publication)
                   : resume_distributed_sieve_worker_cleanup_authorization_v1(std::move(root));
    }

    [[nodiscard]] AuthorizationResult
    advance_authorization(CompletionPublishedContinuation&& completion) const noexcept {
        return trusted_test_hooks
                   ? trusted_test::
                         advance_distributed_sieve_worker_cleanup_authorization_v1_with_hooks(
                             std::move(completion), hooks.authorization_publication)
                   : advance_distributed_sieve_worker_cleanup_authorization_v1(
                         std::move(completion));
    }

    [[nodiscard]] CompletionPreparationResult
    prepare_completion(AuthorizationContinuation&& authorization) const noexcept {
        return trusted_test_hooks
                   ? trusted_test::
                         drive_distributed_sieve_worker_cleanup_to_completion_ready_v1_with_hooks(
                             std::move(authorization), hooks.intent_publication)
                   : drive_distributed_sieve_worker_cleanup_to_completion_ready_v1(
                         std::move(authorization));
    }

    [[nodiscard]] IntentConversionResult
    execute_intent_conversion(IntentConversionCapsule&& capsule) const noexcept {
        return trusted_test_hooks
                   ? trusted_test::
                         execute_distributed_sieve_worker_cleanup_intent_conversion_v1_with_hooks(
                             std::move(capsule), hooks.intent_publication)
                   : execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
                         std::move(capsule));
    }

    [[nodiscard]] CompletionPublicationResult
    publish_completion(CompletionReady&& completion_ready) const noexcept {
        return trusted_test_hooks
                   ? trusted_test::
                         publish_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
                             std::move(completion_ready), hooks.completion_publication)
                   : publish_distributed_sieve_worker_cleanup_completion_v1(
                         std::move(completion_ready));
    }

    [[nodiscard]] CompletionPublicationResult
    reconcile_completion(RootAdmission&& root) const noexcept {
        return trusted_test_hooks
                   ? trusted_test::
                         reconcile_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
                             std::move(root), hooks.completion_publication)
                   : reconcile_distributed_sieve_worker_cleanup_completion_v1(std::move(root));
    }
};

DistributedSieveWorkerCleanupOrchestrationContinuationV1::
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        std::size_t expected_worker_count) noexcept
    : payload_(std::in_place_type<AuthorizationRecovery>, std::move(root)),
      expected_worker_count_(expected_worker_count) {}

DistributedSieveWorkerCleanupOrchestrationContinuationV1::
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule,
        std::size_t expected_worker_count) noexcept
    : payload_(std::in_place_type<IntentConversionRetry>, std::move(capsule)),
      expected_worker_count_(expected_worker_count) {}

DistributedSieveWorkerCleanupOrchestrationContinuationV1::
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&& completion_ready,
        std::size_t expected_worker_count) noexcept
    : payload_(std::in_place_type<CompletionPublicationFreshRetry>, std::move(completion_ready)),
      expected_worker_count_(expected_worker_count) {}

DistributedSieveWorkerCleanupOrchestrationContinuationV1::
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&& completion,
        std::size_t expected_worker_count) noexcept
    : payload_(std::in_place_type<AuthorizationAdvanceRetry>, std::move(completion)),
      expected_worker_count_(expected_worker_count) {}

DistributedSieveWorkerCleanupOrchestrationContinuationV1::
    DistributedSieveWorkerCleanupOrchestrationContinuationV1(
        DistributedSieveWorkerCleanupOrchestrationContinuationV1&&) noexcept = default;

DistributedSieveWorkerCleanupOrchestrationContinuationV1::
    ~DistributedSieveWorkerCleanupOrchestrationContinuationV1() noexcept = default;

bool DistributedSieveWorkerCleanupOrchestrationContinuationV1::valid() const noexcept {
    try {
        if (!transition_budget_for(expected_worker_count_).has_value()) {
            return false;
        }
        return std::visit(
            [&](const auto& state) {
                using State = std::remove_cvref_t<decltype(state)>;
                if constexpr (std::same_as<State, AuthorizationRecovery>) {
                    return state.root.valid() &&
                           expected_worker_count(state.root) == expected_worker_count_;
                } else if constexpr (std::same_as<State, IntentConversionRetry>) {
                    return state.capsule.valid();
                } else if constexpr (std::same_as<State, CompletionPublicationFreshRetry>) {
                    return state.completion_ready.valid();
                } else {
                    return state.completion.valid();
                }
            },
            payload_);
    } catch (...) {
        return false;
    }
}

DistributedSieveWorkerCleanupOrchestrationStageV1
DistributedSieveWorkerCleanupOrchestrationContinuationV1::stage() const noexcept {
    return std::visit(
        [](const auto& state) noexcept {
            using State = std::remove_cvref_t<decltype(state)>;
            if constexpr (std::same_as<State, AuthorizationRecovery>) {
                return OrchestrationStage::authorization_recovery;
            } else if constexpr (std::same_as<State, IntentConversionRetry>) {
                return OrchestrationStage::intent_conversion_retry;
            } else if constexpr (std::same_as<State, CompletionPublicationFreshRetry>) {
                return OrchestrationStage::completion_publication_fresh_retry;
            } else {
                return OrchestrationStage::authorization_advance_retry;
            }
        },
        payload_);
}

DistributedSieveWorkerCleanupRetainedMergedResultV1::
    DistributedSieveWorkerCleanupRetainedMergedResultV1(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

DistributedSieveWorkerCleanupRetainedMergedResultV1::
    DistributedSieveWorkerCleanupRetainedMergedResultV1(
        DistributedSieveWorkerCleanupRetainedMergedResultV1&&) noexcept = default;

DistributedSieveWorkerCleanupRetainedMergedResultV1::
    ~DistributedSieveWorkerCleanupRetainedMergedResultV1() noexcept = default;

bool DistributedSieveWorkerCleanupRetainedMergedResultV1::valid() const noexcept {
    if (state_ == nullptr || state_->expected_reader == nullptr ||
        !state_->merged_relations.has_value() || !state_->terminal.valid()) {
        return false;
    }
    try {
        (void)state_->merged_relations->count();
        return true;
    } catch (...) {
        return false;
    }
}

std::size_t
DistributedSieveWorkerCleanupRetainedMergedResultV1::completed_worker_count() const noexcept {
    return state_ != nullptr ? state_->terminal.completed_worker_count() : 0U;
}

const relation::ReadOnlyRelationCorpusView&
DistributedSieveWorkerCleanupRetainedMergedResultV1::merged_relations() const& {
    if (state_ == nullptr || !state_->merged_relations.has_value()) {
        throw std::logic_error("retained merged result is moved-from or invalid");
    }
    return *state_->merged_relations;
}

const WaveMergeCommitV1&
DistributedSieveWorkerCleanupRetainedMergedResultV1::commit_for_wave_result_promotion_v1() const {
    if (state_ == nullptr || !state_->terminal.valid()) {
        throw std::logic_error("retained merged result is moved-from or invalid");
    }
    return state_->commit;
}

DistributedSieveWorkerCleanupOrchestrationResultV1::operator bool() const noexcept {
    switch (diagnostic.disposition) {
    case OrchestrationDisposition::retryable:
        return retryable.has_value() && retryable->valid() && !retained.has_value() &&
               diagnostic.phase == OrchestrationPhase::complete &&
               diagnostic.status == OrchestrationStatus::retryable &&
               diagnostic.retry_stage == retryable->stage() && !diagnostic.native_error;
    case OrchestrationDisposition::retained:
        return !retryable.has_value() && retained.has_value() && retained->valid() &&
               diagnostic.phase == OrchestrationPhase::complete &&
               diagnostic.status == OrchestrationStatus::retained &&
               !diagnostic.retry_stage.has_value() && !diagnostic.native_error;
    case OrchestrationDisposition::cold_reopen_required:
        return false;
    }
    return false;
}

DistributedSieveWorkerCleanupOrchestrationResultV1
DistributedSieveWorkerCleanupOrchestrationAuthorityV1::drive_with_operations(
    DistributedSieveWorkerCleanupOrchestrationContinuationV1&& continuation,
    const ChildOperations& operations) noexcept {
    OrchestrationDiagnostic outcome =
        diagnostic(OrchestrationPhase::input_validation, OrchestrationStatus::unexpected_failure);
    if (!continuation.valid()) {
        outcome.status = OrchestrationStatus::invalid_input;
        outcome.native_error = std::make_error_code(std::errc::invalid_argument);
        return cold_reopen(std::move(outcome));
    }

    const std::size_t worker_count = continuation.expected_worker_count_;
    const auto budget = transition_budget_for(worker_count);
    if (!budget.has_value()) {
        outcome.status = OrchestrationStatus::progress_budget_exhausted;
        outcome.native_error = std::make_error_code(std::errc::value_too_large);
        return cold_reopen(std::move(outcome));
    }
    outcome.transition_budget = *budget;

    std::optional<DriveState> current;
    if (std::holds_alternative<OrchestrationContinuation::AuthorizationRecovery>(
            continuation.payload_)) {
        current.emplace(std::in_place_type<AuthorizationRecoveryState>,
                        std::move(std::get<OrchestrationContinuation::AuthorizationRecovery>(
                                      continuation.payload_)
                                      .root));
    } else if (std::holds_alternative<OrchestrationContinuation::IntentConversionRetry>(
                   continuation.payload_)) {
        current.emplace(std::in_place_type<IntentConversionState>,
                        std::move(std::get<OrchestrationContinuation::IntentConversionRetry>(
                                      continuation.payload_)
                                      .capsule));
    } else if (std::holds_alternative<OrchestrationContinuation::CompletionPublicationFreshRetry>(
                   continuation.payload_)) {
        current.emplace(
            std::in_place_type<CompletionPublicationFreshState>,
            std::move(std::get<OrchestrationContinuation::CompletionPublicationFreshRetry>(
                          continuation.payload_)
                          .completion_ready));
    } else {
        current.emplace(std::in_place_type<AuthorizationAdvanceState>,
                        std::move(std::get<OrchestrationContinuation::AuthorizationAdvanceRetry>(
                                      continuation.payload_)
                                      .completion));
    }

    const auto begin_transition = [&](OrchestrationPhase phase) noexcept {
        outcome.phase = phase;
        if (outcome.transitions_completed >= outcome.transition_budget) {
            outcome.status = OrchestrationStatus::progress_budget_exhausted;
            outcome.native_error = std::make_error_code(std::errc::state_not_recoverable);
            return false;
        }
        ++outcome.transitions_completed;
        return true;
    };
    const auto invalid_union = [&](OrchestrationPhase phase, std::error_code error = {}) noexcept {
        outcome.phase = phase;
        outcome.status = OrchestrationStatus::ownership_union_invalid;
        outcome.native_error = child_error(error);
        return cold_reopen(std::move(outcome));
    };
    const auto child_cold = [&](OrchestrationPhase phase, std::error_code error) noexcept {
        outcome.phase = phase;
        outcome.status = OrchestrationStatus::child_cold_reopen_required;
        outcome.native_error = child_error(error);
        return cold_reopen(std::move(outcome));
    };

    try {
        for (;;) {
            if (std::holds_alternative<AuthorizationRecoveryState>(*current)) {
                if (!begin_transition(OrchestrationPhase::authorization_publication)) {
                    return cold_reopen(std::move(outcome));
                }
                auto root = std::move(std::get<AuthorizationRecoveryState>(*current).root);
                current.reset();
                AuthorizationResult child = operations.resume_authorization(std::move(root));
                outcome.authorization_publication = child.diagnostic;
                const auto disposition = child.diagnostic.disposition;
                const bool exact_one = exactly_one_present(
                    child.retryable_completion_continuation, child.retryable_recovery_root,
                    child.completion_reconciliation_root, child.authorization_published,
                    child.all_workers_completed);

                if (disposition == AuthorizationDisposition::retryable_completion_continuation) {
                    if (!child || !exact_one ||
                        !child.retryable_completion_continuation.has_value() ||
                        !child.retryable_completion_continuation->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    OrchestrationContinuation retry(
                        std::move(*child.retryable_completion_continuation), worker_count);
                    return retryable(std::move(retry), std::move(outcome));
                }
                if (disposition == AuthorizationDisposition::retryable_recovery_root) {
                    if (!child || !exact_one || !child.retryable_recovery_root.has_value() ||
                        !child.retryable_recovery_root->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    OrchestrationContinuation retry(std::move(*child.retryable_recovery_root),
                                                    worker_count);
                    return retryable(std::move(retry), std::move(outcome));
                }
                if (disposition == AuthorizationDisposition::completion_reconciliation_required) {
                    if (!child || !exact_one || !child.completion_reconciliation_root.has_value() ||
                        !child.completion_reconciliation_root->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    current.emplace(std::in_place_type<CompletionPublicationRecoveryState>,
                                    std::move(*child.completion_reconciliation_root));
                    continue;
                }
                if (disposition == AuthorizationDisposition::authorization_published) {
                    if (!child || !exact_one || !child.authorization_published.has_value() ||
                        !child.authorization_published->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    current.emplace(std::in_place_type<CompletionPreparationState>,
                                    std::move(*child.authorization_published));
                    continue;
                }
                if (disposition == AuthorizationDisposition::all_workers_completed) {
                    if (!child || !exact_one || !child.all_workers_completed.has_value() ||
                        !child.all_workers_completed->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    current.emplace(std::in_place_type<TerminalRetentionState>,
                                    std::move(*child.all_workers_completed));
                    continue;
                }
                if (disposition == AuthorizationDisposition::cold_reopen_required &&
                    none_present(child.retryable_completion_continuation,
                                 child.retryable_recovery_root,
                                 child.completion_reconciliation_root,
                                 child.authorization_published, child.all_workers_completed)) {
                    return child_cold(OrchestrationPhase::authorization_publication,
                                      child.diagnostic.native_error);
                }
                return invalid_union(OrchestrationPhase::authorization_publication,
                                     child.diagnostic.native_error);
            }

            if (std::holds_alternative<IntentConversionState>(*current)) {
                if (!begin_transition(OrchestrationPhase::intent_conversion_execute)) {
                    return cold_reopen(std::move(outcome));
                }
                auto capsule = std::move(std::get<IntentConversionState>(*current).capsule);
                current.reset();
                IntentConversionResult child =
                    operations.execute_intent_conversion(std::move(capsule));
                outcome.intent_conversion = child.diagnostic;
                const bool exact_one =
                    exactly_one_present(child.retryable_capsule, child.root_continuation);

                if (child.diagnostic.capsule_retained) {
                    if (!child || !exact_one || !child.retryable_capsule.has_value() ||
                        !child.retryable_capsule->valid() || child.root_continuation.has_value()) {
                        return invalid_union(OrchestrationPhase::intent_conversion_execute,
                                             child.diagnostic.native_error);
                    }
                    OrchestrationContinuation retry(std::move(*child.retryable_capsule),
                                                    worker_count);
                    return retryable(std::move(retry), std::move(outcome));
                }
                if (child.diagnostic.root_continuation_retained) {
                    const bool published = child.publication.intent_published();
                    const bool reconcile = child.publication.canonical_reconciliation_required();
                    if (!child || !exact_one || child.retryable_capsule.has_value() ||
                        !child.root_continuation.has_value() || !child.root_continuation->valid() ||
                        published == reconcile) {
                        return invalid_union(OrchestrationPhase::intent_conversion_execute,
                                             child.diagnostic.native_error);
                    }
                    current.emplace(std::in_place_type<AuthorizationRecoveryState>,
                                    std::move(*child.root_continuation));
                    continue;
                }
                if (none_present(child.retryable_capsule, child.root_continuation) &&
                    child.diagnostic.cold_reopen_required) {
                    return child_cold(OrchestrationPhase::intent_conversion_execute,
                                      child.diagnostic.native_error);
                }
                return invalid_union(OrchestrationPhase::intent_conversion_execute,
                                     child.diagnostic.native_error);
            }

            if (std::holds_alternative<CompletionPreparationState>(*current)) {
                if (!begin_transition(OrchestrationPhase::completion_preparation)) {
                    return cold_reopen(std::move(outcome));
                }
                auto authorization =
                    std::move(std::get<CompletionPreparationState>(*current).authorization);
                current.reset();
                CompletionPreparationResult child =
                    operations.prepare_completion(std::move(authorization));
                outcome.completion_preparation = child.diagnostic;
                const auto disposition = child.diagnostic.disposition;
                const bool exact_one =
                    exactly_one_present(child.retryable_root, child.retryable_intent_conversion,
                                        child.completion_ready);

                if (disposition == CompletionPreparationDisposition::retryable_root) {
                    if (!child || !exact_one || !child.retryable_root.has_value() ||
                        !child.retryable_root->valid()) {
                        return invalid_union(OrchestrationPhase::completion_preparation,
                                             child.diagnostic.native_error);
                    }
                    OrchestrationContinuation retry(std::move(*child.retryable_root), worker_count);
                    return retryable(std::move(retry), std::move(outcome));
                }
                if (disposition == CompletionPreparationDisposition::retryable_intent_conversion) {
                    if (!child || !exact_one || !child.retryable_intent_conversion.has_value() ||
                        !child.retryable_intent_conversion->valid()) {
                        return invalid_union(OrchestrationPhase::completion_preparation,
                                             child.diagnostic.native_error);
                    }
                    OrchestrationContinuation retry(std::move(*child.retryable_intent_conversion),
                                                    worker_count);
                    return retryable(std::move(retry), std::move(outcome));
                }
                if (disposition == CompletionPreparationDisposition::completion_ready) {
                    if (!child || !exact_one || !child.completion_ready.has_value() ||
                        !child.completion_ready->valid()) {
                        return invalid_union(OrchestrationPhase::completion_preparation,
                                             child.diagnostic.native_error);
                    }
                    current.emplace(std::in_place_type<CompletionPublicationFreshState>,
                                    std::move(*child.completion_ready));
                    continue;
                }
                if (disposition == CompletionPreparationDisposition::cold_reopen_required &&
                    none_present(child.retryable_root, child.retryable_intent_conversion,
                                 child.completion_ready)) {
                    return child_cold(OrchestrationPhase::completion_preparation,
                                      child.diagnostic.native_error);
                }
                return invalid_union(OrchestrationPhase::completion_preparation,
                                     child.diagnostic.native_error);
            }

            if (std::holds_alternative<CompletionPublicationFreshState>(*current) ||
                std::holds_alternative<CompletionPublicationRecoveryState>(*current)) {
                if (!begin_transition(OrchestrationPhase::completion_publication)) {
                    return cold_reopen(std::move(outcome));
                }
                CompletionPublicationResult child = [&]() -> CompletionPublicationResult {
                    if (std::holds_alternative<CompletionPublicationFreshState>(*current)) {
                        auto completion_ready = std::move(
                            std::get<CompletionPublicationFreshState>(*current).completion_ready);
                        current.reset();
                        return operations.publish_completion(std::move(completion_ready));
                    }
                    auto root =
                        std::move(std::get<CompletionPublicationRecoveryState>(*current).root);
                    current.reset();
                    return operations.reconcile_completion(std::move(root));
                }();
                outcome.completion_publication = child.diagnostic;
                const auto disposition = child.diagnostic.disposition;
                const bool exact_one = exactly_one_present(child.retryable_completion_ready,
                                                           child.retryable_recovery_root,
                                                           child.published_continuation);

                if (disposition == CompletionPublicationDisposition::retryable_completion_ready) {
                    if (!child || !exact_one || !child.retryable_completion_ready.has_value() ||
                        !child.retryable_completion_ready->valid()) {
                        return invalid_union(OrchestrationPhase::completion_publication,
                                             child.diagnostic.native_error);
                    }
                    OrchestrationContinuation retry(std::move(*child.retryable_completion_ready),
                                                    worker_count);
                    return retryable(std::move(retry), std::move(outcome));
                }
                if (disposition == CompletionPublicationDisposition::retryable_recovery_root) {
                    if (!child || !exact_one || !child.retryable_recovery_root.has_value() ||
                        !child.retryable_recovery_root->valid()) {
                        return invalid_union(OrchestrationPhase::completion_publication,
                                             child.diagnostic.native_error);
                    }
                    OrchestrationContinuation retry(std::move(*child.retryable_recovery_root),
                                                    worker_count);
                    return retryable(std::move(retry), std::move(outcome));
                }
                if (disposition == CompletionPublicationDisposition::completion_published) {
                    if (!child || !exact_one || !child.published_continuation.has_value() ||
                        !child.published_continuation->valid()) {
                        return invalid_union(OrchestrationPhase::completion_publication,
                                             child.diagnostic.native_error);
                    }
                    current.emplace(std::in_place_type<AuthorizationAdvanceState>,
                                    std::move(*child.published_continuation));
                    continue;
                }
                if (disposition == CompletionPublicationDisposition::cold_reopen_required &&
                    none_present(child.retryable_completion_ready, child.retryable_recovery_root,
                                 child.published_continuation)) {
                    return child_cold(OrchestrationPhase::completion_publication,
                                      child.diagnostic.native_error);
                }
                return invalid_union(OrchestrationPhase::completion_publication,
                                     child.diagnostic.native_error);
            }

            if (std::holds_alternative<AuthorizationAdvanceState>(*current)) {
                if (!begin_transition(OrchestrationPhase::authorization_publication)) {
                    return cold_reopen(std::move(outcome));
                }
                auto completion =
                    std::move(std::get<AuthorizationAdvanceState>(*current).completion);
                current.reset();
                AuthorizationResult child = operations.advance_authorization(std::move(completion));
                outcome.authorization_publication = child.diagnostic;
                const auto disposition = child.diagnostic.disposition;
                const bool exact_one = exactly_one_present(
                    child.retryable_completion_continuation, child.retryable_recovery_root,
                    child.completion_reconciliation_root, child.authorization_published,
                    child.all_workers_completed);

                if (disposition == AuthorizationDisposition::retryable_completion_continuation) {
                    if (!child || !exact_one ||
                        !child.retryable_completion_continuation.has_value() ||
                        !child.retryable_completion_continuation->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    OrchestrationContinuation retry(
                        std::move(*child.retryable_completion_continuation), worker_count);
                    return retryable(std::move(retry), std::move(outcome));
                }
                if (disposition == AuthorizationDisposition::retryable_recovery_root) {
                    if (!child || !exact_one || !child.retryable_recovery_root.has_value() ||
                        !child.retryable_recovery_root->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    OrchestrationContinuation retry(std::move(*child.retryable_recovery_root),
                                                    worker_count);
                    return retryable(std::move(retry), std::move(outcome));
                }
                if (disposition == AuthorizationDisposition::completion_reconciliation_required) {
                    if (!child || !exact_one || !child.completion_reconciliation_root.has_value() ||
                        !child.completion_reconciliation_root->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    current.emplace(std::in_place_type<CompletionPublicationRecoveryState>,
                                    std::move(*child.completion_reconciliation_root));
                    continue;
                }
                if (disposition == AuthorizationDisposition::authorization_published) {
                    if (!child || !exact_one || !child.authorization_published.has_value() ||
                        !child.authorization_published->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    current.emplace(std::in_place_type<CompletionPreparationState>,
                                    std::move(*child.authorization_published));
                    continue;
                }
                if (disposition == AuthorizationDisposition::all_workers_completed) {
                    if (!child || !exact_one || !child.all_workers_completed.has_value() ||
                        !child.all_workers_completed->valid()) {
                        return invalid_union(OrchestrationPhase::authorization_publication,
                                             child.diagnostic.native_error);
                    }
                    current.emplace(std::in_place_type<TerminalRetentionState>,
                                    std::move(*child.all_workers_completed));
                    continue;
                }
                if (disposition == AuthorizationDisposition::cold_reopen_required &&
                    none_present(child.retryable_completion_continuation,
                                 child.retryable_recovery_root,
                                 child.completion_reconciliation_root,
                                 child.authorization_published, child.all_workers_completed)) {
                    return child_cold(OrchestrationPhase::authorization_publication,
                                      child.diagnostic.native_error);
                }
                return invalid_union(OrchestrationPhase::authorization_publication,
                                     child.diagnostic.native_error);
            }

            if (!begin_transition(OrchestrationPhase::terminal_retention)) {
                return cold_reopen(std::move(outcome));
            }
            auto terminal = std::move(std::get<TerminalRetentionState>(*current).terminal);
            current.reset();
            if (!terminal.valid()) {
                outcome.status = OrchestrationStatus::terminal_invalid;
                outcome.native_error = std::make_error_code(std::errc::invalid_argument);
                return cold_reopen(std::move(outcome));
            }

            const relation::OOCRelationReader* const expected_reader =
                std::addressof(terminal.root_.reader());
            WaveMergeCommitV1 retained_commit = terminal.root_.commit();
            auto state = std::make_unique<RetainedMergedResult::State>(
                std::move(terminal), expected_reader, std::move(retained_commit));
            if (!state->terminal.valid() ||
                std::addressof(state->terminal.root_.reader()) != expected_reader) {
                outcome.status = OrchestrationStatus::reader_binding_failed;
                outcome.native_error = std::make_error_code(std::errc::state_not_recoverable);
                return cold_reopen(std::move(outcome));
            }
            state->merged_relations.emplace(state->terminal.root_.reader());
            if (std::addressof(state->terminal.root_.reader()) != expected_reader) {
                outcome.status = OrchestrationStatus::reader_binding_failed;
                outcome.native_error = std::make_error_code(std::errc::state_not_recoverable);
                return cold_reopen(std::move(outcome));
            }

            RetainedMergedResult retained(std::move(state));
            if (!retained.valid() || retained.completed_worker_count() != worker_count) {
                outcome.status = OrchestrationStatus::terminal_invalid;
                outcome.native_error = std::make_error_code(std::errc::state_not_recoverable);
                return cold_reopen(std::move(outcome));
            }
            std::optional<RetainedMergedResult> retained_result;
            retained_result.emplace(std::move(retained));
            outcome.phase = OrchestrationPhase::complete;
            outcome.status = OrchestrationStatus::retained;
            outcome.disposition = OrchestrationDisposition::retained;
            outcome.retry_stage.reset();
            outcome.native_error.clear();
            return {std::nullopt, std::move(retained_result), std::move(outcome)};
        }
    } catch (const std::bad_alloc&) {
        outcome.status = OrchestrationStatus::resource_exhausted;
        outcome.native_error = std::make_error_code(std::errc::not_enough_memory);
    } catch (const std::filesystem::filesystem_error& error) {
        outcome.status = OrchestrationStatus::unexpected_failure;
        outcome.native_error = error.code();
    } catch (...) {
        outcome.status = OrchestrationStatus::unexpected_failure;
        outcome.native_error = std::make_error_code(std::errc::io_error);
    }
    return cold_reopen(std::move(outcome));
}

DistributedSieveWorkerCleanupOrchestrationResultV1
DistributedSieveWorkerCleanupOrchestrationAuthorityV1::start(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept {
    const ChildOperations operations;
    return start_with_operations(std::move(root), operations);
}

DistributedSieveWorkerCleanupOrchestrationResultV1
DistributedSieveWorkerCleanupOrchestrationAuthorityV1::drive(
    DistributedSieveWorkerCleanupOrchestrationContinuationV1&& continuation) noexcept {
    const ChildOperations operations;
    return drive_with_operations(std::move(continuation), operations);
}

DistributedSieveWorkerCleanupOrchestrationResultV1
DistributedSieveWorkerCleanupOrchestrationAuthorityV1::start_with_hooks(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root,
    trusted_test::DistributedSieveWorkerCleanupOrchestrationTestHooksV1 hooks) noexcept {
    const ChildOperations operations{
        .trusted_test_hooks = true,
        .hooks = hooks,
    };
    return start_with_operations(std::move(root), operations);
}

DistributedSieveWorkerCleanupOrchestrationResultV1
DistributedSieveWorkerCleanupOrchestrationAuthorityV1::start_with_operations(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root,
    const ChildOperations& operations) noexcept {
    try {
        if (!root.valid()) {
            return cold_reopen(diagnostic(OrchestrationPhase::input_validation,
                                          OrchestrationStatus::invalid_input,
                                          std::make_error_code(std::errc::invalid_argument)));
        }
        const std::size_t worker_count = expected_worker_count(root);
        OrchestrationContinuation continuation(std::move(root), worker_count);
        return drive_with_operations(std::move(continuation), operations);
    } catch (const std::bad_alloc&) {
        return cold_reopen(diagnostic(OrchestrationPhase::input_validation,
                                      OrchestrationStatus::resource_exhausted,
                                      std::make_error_code(std::errc::not_enough_memory)));
    } catch (const std::filesystem::filesystem_error& error) {
        return cold_reopen(diagnostic(OrchestrationPhase::input_validation,
                                      OrchestrationStatus::unexpected_failure, error.code()));
    } catch (...) {
        return cold_reopen(diagnostic(OrchestrationPhase::input_validation,
                                      OrchestrationStatus::unexpected_failure,
                                      std::make_error_code(std::errc::io_error)));
    }
}

DistributedSieveWorkerCleanupOrchestrationResultV1
drive_distributed_sieve_worker_cleanup_to_retained_merged_v1(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept {
    return DistributedSieveWorkerCleanupOrchestrationAuthorityV1::start(std::move(root));
}

DistributedSieveWorkerCleanupOrchestrationResultV1
resume_distributed_sieve_worker_cleanup_to_retained_merged_v1(
    DistributedSieveWorkerCleanupOrchestrationContinuationV1&& continuation) noexcept {
    return DistributedSieveWorkerCleanupOrchestrationAuthorityV1::drive(std::move(continuation));
}

namespace trusted_test {

DistributedSieveWorkerCleanupOrchestrationResultV1
drive_distributed_sieve_worker_cleanup_to_retained_merged_v1_with_hooks(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root,
    DistributedSieveWorkerCleanupOrchestrationTestHooksV1 hooks) noexcept {
    return DistributedSieveWorkerCleanupOrchestrationAuthorityV1::start_with_hooks(std::move(root),
                                                                                   hooks);
}

} // namespace trusted_test

} // namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail
