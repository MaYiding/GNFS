#include "distributed_sieve_worker_cleanup_authority_internal.hpp"

#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail {

namespace {

using TailTestHooks = trusted_test::DistributedSieveWorkerCleanupTailTestHooksV1;

thread_local const TailTestHooks* active_tail_release_test_hooks = nullptr;

class ScopedTailReleaseTestHooksV1 final {
public:
    explicit ScopedTailReleaseTestHooksV1(const TailTestHooks& hooks) noexcept
        : previous_(std::exchange(active_tail_release_test_hooks, std::addressof(hooks))) {}

    ScopedTailReleaseTestHooksV1(const ScopedTailReleaseTestHooksV1&) = delete;
    ScopedTailReleaseTestHooksV1& operator=(const ScopedTailReleaseTestHooksV1&) = delete;

    ~ScopedTailReleaseTestHooksV1() noexcept {
        active_tail_release_test_hooks = previous_;
    }

private:
    const TailTestHooks* previous_ = nullptr;
};

[[nodiscard]] bool invoke_before_final_tail_revalidation_test_hook_v1() noexcept {
    const auto* hooks = active_tail_release_test_hooks;
    return hooks != nullptr && hooks->before_final_tail_revalidation != nullptr &&
           hooks->before_final_tail_revalidation(hooks->context);
}

} // namespace

} // namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail

namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail {

distributed_sieve_worker_cleanup_authority_detail::DistributedSieveCommittedTailCleanupTransitionV1
DistributedSieveCommittedTailAdmissionV1::release_for_worker_cleanup_cold_open_v1() && noexcept {
    namespace cleanup = distributed_sieve_worker_cleanup_authority_detail;
    namespace wave = distributed_sieve_resume_detail;

    cleanup::DistributedSieveCommittedTailCleanupTransitionV1 result;
    const auto spend_tail = [&]() noexcept {
        prepared_record_ = nullptr;
        creator_process_id_ = 0;
        origin_.reset();
        result.retryable_tail_retained = false;
        result.tail_spent = true;
    };
    const auto retain_or_spend_failed_tail = [&]() noexcept {
        if (valid()) {
            result.retryable_tail_retained = true;
        } else {
            spend_tail();
        }
    };
    const int process_id = gnfs::util::process_id();
    if (!valid()) {
        const bool process_mismatch =
            creator_process_id_ != 0 &&
            (process_id <= 0 || creator_process_id_ != static_cast<std::uint64_t>(process_id));
        result.native_error = process_mismatch ? std::make_error_code(std::errc::no_such_process)
                                               : std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    result.admission_validated = true;

    try {
        auto* store = origin_->retained_wave_store();
        if (store == nullptr) {
            result.native_error = std::make_error_code(std::errc::state_not_recoverable);
            retain_or_spend_failed_tail();
            return result;
        }
        std::filesystem::path absolute_root = store->absolute_root();
        if (absolute_root.empty() || !absolute_root.is_absolute() ||
            absolute_root.lexically_normal() != absolute_root) {
            result.native_error = std::make_error_code(std::errc::invalid_argument);
            retain_or_spend_failed_tail();
            return result;
        }
        auto captured = store->freeze_worker_cleanup_exact_anchor_v1(commit_, canonical_snapshot_);
        if (!captured || !captured.anchor.has_value()) {
            result.wave_store = std::move(captured.diagnostic);
            result.native_error = result.wave_store.native_error
                                      ? result.wave_store.native_error
                                      : std::make_error_code(std::errc::state_not_recoverable);
            retain_or_spend_failed_tail();
            return result;
        }
        if (captured.anchor->manifest_digest != commit_.manifest_digest ||
            captured.anchor->merge_commit_digest != commit_.self_digest) {
            result.native_error = std::make_error_code(std::errc::state_not_recoverable);
            spend_tail();
            return result;
        }
        if (cleanup::invoke_before_final_tail_revalidation_test_hook_v1()) {
            result.wave_store.status = wave::DistributedSieveWaveStoreStatus::interrupted;
            result.native_error = std::make_error_code(std::errc::operation_canceled);
            retain_or_spend_failed_tail();
            return result;
        }
        if (auto authority = store->revalidate_authority();
            authority.status != wave::DistributedSieveWaveStoreStatus::ready) {
            result.wave_store = std::move(authority);
            result.native_error = result.wave_store.native_error
                                      ? result.wave_store.native_error
                                      : std::make_error_code(std::errc::state_not_recoverable);
            spend_tail();
            return result;
        }
        if (!valid()) {
            result.wave_store.status = wave::DistributedSieveWaveStoreStatus::namespace_conflict;
            result.native_error = std::make_error_code(std::errc::state_not_recoverable);
            spend_tail();
            return result;
        }

        // Complete every possibly-throwing copy before crossing the lock
        // generation. A failure up to this point leaves this tail unchanged.
        result.absolute_root.emplace(std::move(absolute_root));
        result.exact_anchor.emplace(std::move(*captured.anchor));

        spend_tail();
        return result;
    } catch (const std::bad_alloc&) {
        result.wave_store.status = wave::DistributedSieveWaveStoreStatus::resource_exhausted;
        result.native_error = std::make_error_code(std::errc::not_enough_memory);
        if (result.admission_validated && !result.tail_spent) {
            retain_or_spend_failed_tail();
        }
        return result;
    } catch (const std::filesystem::filesystem_error& error) {
        result.wave_store.status = wave::DistributedSieveWaveStoreStatus::unexpected_failure;
        result.native_error = error.code();
        if (result.admission_validated && !result.tail_spent) {
            retain_or_spend_failed_tail();
        }
        return result;
    } catch (...) {
        result.wave_store.status = wave::DistributedSieveWaveStoreStatus::unexpected_failure;
        result.native_error = std::make_error_code(std::errc::io_error);
        if (result.admission_validated && !result.tail_spent) {
            retain_or_spend_failed_tail();
        }
        return result;
    }
}

} // namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail

namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail {

namespace {

namespace wave = distributed_sieve_resume_detail;

using Phase = DistributedSieveWorkerCleanupTailPhaseV1;
using Status = DistributedSieveWorkerCleanupTailStatusV1;
using Diagnostic = DistributedSieveWorkerCleanupTailDiagnosticV1;
using Result = DistributedSieveWorkerCleanupTailResultV1;
using Tail = DistributedSieveCommittedTailAdmissionV1;

[[nodiscard]] Diagnostic failure(Phase phase, Status status, bool tail_spent = false,
                                 bool cold_reopen_required = false,
                                 std::error_code native_error = {}) noexcept {
    Diagnostic diagnostic;
    diagnostic.phase = phase;
    diagnostic.status = status;
    diagnostic.native_error = native_error;
    diagnostic.tail_spent = tail_spent;
    diagnostic.cold_reopen_required = cold_reopen_required;
    return diagnostic;
}

[[nodiscard]] Result retryable_failure(Tail&& tail, Diagnostic diagnostic) noexcept {
    std::optional<Tail> retryable;
    retryable.emplace(std::move(tail));
    return {std::move(retryable), std::nullopt, std::move(diagnostic)};
}

[[nodiscard]] Status map_open_status(wave::DistributedSieveWaveStoreStatus status) noexcept {
    switch (status) {
    case wave::DistributedSieveWaveStoreStatus::platform_unsupported:
        return Status::platform_unsupported;
    case wave::DistributedSieveWaveStoreStatus::resource_exhausted:
        return Status::resource_exhausted;
    case wave::DistributedSieveWaveStoreStatus::unexpected_failure:
        return Status::unexpected_failure;
    default:
        return Status::cleanup_root_open_failed;
    }
}

} // namespace

DistributedSieveWorkerCleanupTailResultV1 DistributedSieveWorkerCleanupTailAuthorityV1::consume(
    DistributedSieveCommittedTailAdmissionV1&& tail,
    trusted_test::DistributedSieveWorkerCleanupTailTestHooksV1 hooks) noexcept {
    auto released = [&] {
        ScopedTailReleaseTestHooksV1 scoped_hooks(hooks);
        return std::move(tail).release_for_worker_cleanup_cold_open_v1();
    }();
    if (!released) {
        const bool process_mismatch =
            released.native_error == std::make_error_code(std::errc::no_such_process);
        const auto snapshot_status =
            distributed_sieve_worker_cleanup_tail_root_snapshot_status(released.wave_store.status);
        auto diagnostic = failure(
            released.tail_spent ? Phase::old_epoch_release
                                : (released.admission_validated ? Phase::root_snapshot
                                                                : Phase::admission_validation),
            process_mismatch
                ? Status::process_mismatch
                : (released.admission_validated ? snapshot_status : Status::invalid_admission),
            released.tail_spent, released.tail_spent, released.native_error);
        diagnostic.wave_store = std::move(released.wave_store);
        if (released.retryable_tail_retained && !released.tail_spent &&
            released.admission_validated) {
            return retryable_failure(std::move(tail), std::move(diagnostic));
        }
        if (released.admission_validated && !released.tail_spent) {
            std::optional<Tail> closed_tail;
            closed_tail.emplace(std::move(tail));
            closed_tail.reset();
            diagnostic.phase = Phase::old_epoch_release;
            diagnostic.status = Status::unexpected_failure;
            diagnostic.tail_spent = true;
            diagnostic.cold_reopen_required = true;
            if (!diagnostic.native_error) {
                diagnostic.native_error = std::make_error_code(std::errc::state_not_recoverable);
            }
        }
        return {std::nullopt, std::nullopt, std::move(diagnostic)};
    }

    if (hooks.after_old_epoch_release != nullptr) {
        hooks.after_old_epoch_release(hooks.context);
    }

    try {
        auto opened = wave::open_worker_cleanup_root_v1(*released.absolute_root,
                                                        released.exact_anchor->manifest_digest, {},
                                                        std::addressof(*released.exact_anchor));
        if (!opened || !opened.admission.has_value()) {
            auto diagnostic =
                failure(Phase::cleanup_root_open, map_open_status(opened.diagnostic.status), true,
                        true, opened.diagnostic.native_error);
            diagnostic.wave_store = std::move(opened.diagnostic);
            return {std::nullopt, std::nullopt, std::move(diagnostic)};
        }
        Diagnostic diagnostic;
        diagnostic.phase = Phase::complete;
        diagnostic.status = Status::ready;
        diagnostic.wave_store = std::move(opened.diagnostic);
        diagnostic.tail_spent = true;
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> admission;
        admission.emplace(std::move(*opened.admission));
        return {std::nullopt, std::move(admission), std::move(diagnostic)};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, std::nullopt,
                failure(Phase::cleanup_root_open, Status::resource_exhausted, true, true,
                        std::make_error_code(std::errc::not_enough_memory))};
    } catch (const std::filesystem::filesystem_error& error) {
        return {std::nullopt, std::nullopt,
                failure(Phase::cleanup_root_open, Status::cleanup_root_open_failed, true, true,
                        error.code())};
    } catch (...) {
        return {std::nullopt, std::nullopt,
                failure(Phase::cleanup_root_open, Status::unexpected_failure, true, true,
                        std::make_error_code(std::errc::io_error))};
    }
}

DistributedSieveWorkerCleanupTailResultV1
consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
    DistributedSieveCommittedTailAdmissionV1&& tail) noexcept {
    return DistributedSieveWorkerCleanupTailAuthorityV1::consume(std::move(tail), {});
}

namespace trusted_test {

DistributedSieveWorkerCleanupTailResultV1
consume_distributed_sieve_committed_tail_for_worker_cleanup_v1_with_hooks(
    DistributedSieveCommittedTailAdmissionV1&& tail,
    DistributedSieveWorkerCleanupTailTestHooksV1 hooks) noexcept {
    return DistributedSieveWorkerCleanupTailAuthorityV1::consume(std::move(tail), hooks);
}

} // namespace trusted_test

DistributedSieveWorkerCleanupReceiptMintResultV1
mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
    DistributedSieveWorkerCleanupRootAdmissionV1& admission) noexcept {
    return DistributedSieveWorkerCleanupReceiptMintAuthorityV1::mint(admission);
}

DistributedSieveWorkerCleanupIntentConversionExecuteResultV1
DistributedSieveWorkerCleanupIntentConversionAuthorityV1::execute(
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule,
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2 hooks,
    bool trusted_test_hooks) noexcept {
    auto retained_capsule = std::move(capsule);
    namespace relation_cleanup = relation::ooc_cleanup_detail;
    using Diagnostic = DistributedSieveWorkerCleanupIntentConversionExecuteDiagnosticV1;
    using Phase = DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1;
    using Publication = relation_cleanup::OOCPrivateHandoffCleanupIntentPublicationResultV2;
    using Result = DistributedSieveWorkerCleanupIntentConversionExecuteResultV1;
    using Status = DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1;

    const auto failure = [](Phase phase, Status status, std::error_code native_error,
                            bool cold_reopen_required) noexcept {
        Diagnostic diagnostic;
        diagnostic.phase = phase;
        diagnostic.status = status;
        diagnostic.native_error = native_error;
        diagnostic.cold_reopen_required = cold_reopen_required;
        return diagnostic;
    };
    const auto release_for_cold_reopen = [&]() noexcept {
        retained_capsule.reader_.reset();
        retained_capsule.receipt_.reset();
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> released_root;
        released_root.emplace(std::move(retained_capsule.root_));
        released_root.reset();
    };

    const int process_id = gnfs::util::process_id();
    const bool process_mismatch = retained_capsule.creator_process_id_ != 0 &&
                                  (process_id <= 0 || retained_capsule.creator_process_id_ !=
                                                          static_cast<std::uint64_t>(process_id));
    if (!valid(retained_capsule)) {
        release_for_cold_reopen();
        return Result(std::nullopt, std::nullopt, Publication{},
                      failure(Phase::capsule_validation,
                              process_mismatch ? Status::process_mismatch : Status::invalid_capsule,
                              process_mismatch ? std::make_error_code(std::errc::no_such_process)
                                               : std::make_error_code(std::errc::invalid_argument),
                              true));
    }

    Publication publication =
        trusted_test_hooks
            ? relation_cleanup::
                  convert_authorized_private_handoff_to_cleanup_intent_v2_for_trusted_test(
                      relation_cleanup::OOCPrivateHandoffCleanupIntentPublicationTestKeyV2{},
                      std::move(*retained_capsule.reader_), std::move(*retained_capsule.receipt_),
                      hooks)
            : relation_cleanup::convert_authorized_private_handoff_to_cleanup_intent_v2(
                  std::move(*retained_capsule.reader_), std::move(*retained_capsule.receipt_));

    if (publication.capabilities_retained()) {
        std::optional<DistributedSieveWorkerCleanupIntentConversionCapsuleV1> retryable_capsule;
        retryable_capsule.emplace(std::move(retained_capsule));
        Diagnostic diagnostic;
        diagnostic.phase = Phase::complete;
        diagnostic.status = Status::capabilities_retained;
        diagnostic.capsule_retained = true;
        return Result(std::move(retryable_capsule), std::nullopt, std::move(publication),
                      std::move(diagnostic));
    }

    if (publication.intent_published() || publication.canonical_reconciliation_required()) {
        const bool reconciliation_required = publication.canonical_reconciliation_required();
        retained_capsule.reader_.reset();
        retained_capsule.receipt_.reset();
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> root_continuation;
        root_continuation.emplace(std::move(retained_capsule.root_));
        Diagnostic diagnostic;
        diagnostic.phase = Phase::complete;
        diagnostic.status = reconciliation_required ? Status::canonical_reconciliation_required
                                                    : Status::intent_published;
        diagnostic.root_continuation_retained = true;
        return Result(std::nullopt, std::move(root_continuation), std::move(publication),
                      std::move(diagnostic));
    }

    const std::error_code publication_error =
        publication.result.native_error ? publication.result.native_error
                                        : std::make_error_code(std::errc::state_not_recoverable);
    release_for_cold_reopen();
    return Result(
        std::nullopt, std::nullopt, std::move(publication),
        failure(Phase::intent_publication, Status::publication_failed, publication_error, true));
}

DistributedSieveWorkerCleanupIntentConversionExecuteResultV1
execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule) noexcept {
    return DistributedSieveWorkerCleanupIntentConversionAuthorityV1::execute(std::move(capsule), {},
                                                                             false);
}

namespace trusted_test {

DistributedSieveWorkerCleanupIntentConversionExecuteResultV1
execute_distributed_sieve_worker_cleanup_intent_conversion_v1_with_hooks(
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule,
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2
        hooks) noexcept {
    return DistributedSieveWorkerCleanupIntentConversionAuthorityV1::execute(std::move(capsule),
                                                                             hooks, true);
}

} // namespace trusted_test

DistributedSieveWorkerCleanupCompletionReadyCapsuleV1::
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1(
        std::uint32_t manifest_order_ordinal, std::uint64_t creator_process_id,
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding&&
            relation_binding,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt&&
            completion_receipt,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAbsenceEvidenceV2&&
            absence_evidence) noexcept
    : manifest_order_ordinal_(manifest_order_ordinal), creator_process_id_(creator_process_id),
      root_(std::move(root)), relation_binding_(std::move(relation_binding)),
      completion_receipt_(std::move(completion_receipt)),
      absence_evidence_(std::move(absence_evidence)) {}

DistributedSieveWorkerCleanupCompletionReadyCapsuleV1::
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1(
        DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&&) noexcept = default;

DistributedSieveWorkerCleanupCompletionReadyCapsuleV1::
    ~DistributedSieveWorkerCleanupCompletionReadyCapsuleV1() noexcept = default;

bool DistributedSieveWorkerCleanupCompletionReadyCapsuleV1::valid() const noexcept {
    return DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1::valid(*this);
}

DistributedSieveWorkerCleanupCompletionPreparationResultV1::operator bool() const noexcept {
    using Disposition = DistributedSieveWorkerCleanupCompletionPreparationDispositionV1;
    switch (diagnostic.disposition) {
    case Disposition::retryable_root:
        return retryable_root.has_value() && retryable_root->valid() &&
               !retryable_intent_conversion.has_value() && !completion_ready.has_value();
    case Disposition::retryable_intent_conversion:
        return !retryable_root.has_value() && retryable_intent_conversion.has_value() &&
               retryable_intent_conversion->valid() && !completion_ready.has_value();
    case Disposition::completion_ready:
        return !retryable_root.has_value() && !retryable_intent_conversion.has_value() &&
               completion_ready.has_value() && completion_ready->valid();
    case Disposition::cold_reopen_required:
        return false;
    }
    return false;
}

namespace {

namespace relation_cleanup = relation::ooc_cleanup_detail;

using CompletionCapsule = DistributedSieveWorkerCleanupCompletionReadyCapsuleV1;
using CompletionDiagnostic = DistributedSieveWorkerCleanupCompletionPreparationDiagnosticV1;
using CompletionDisposition = DistributedSieveWorkerCleanupCompletionPreparationDispositionV1;
using CompletionPhase = DistributedSieveWorkerCleanupCompletionPreparationPhaseV1;
using CompletionResult = DistributedSieveWorkerCleanupCompletionPreparationResultV1;
using CompletionStatus = DistributedSieveWorkerCleanupCompletionPreparationStatusV1;
using ConversionCapsule = DistributedSieveWorkerCleanupIntentConversionCapsuleV1;
using Minted = DistributedSieveWorkerCleanupReceiptMintedV1;
using RootAdmission = DistributedSieveWorkerCleanupRootAdmissionV1;

[[nodiscard]] CompletionDiagnostic completion_diagnostic(
    CompletionPhase phase, CompletionStatus status,
    CompletionDisposition disposition = CompletionDisposition::cold_reopen_required,
    std::error_code native_error = {}) noexcept {
    CompletionDiagnostic diagnostic;
    diagnostic.phase = phase;
    diagnostic.status = status;
    diagnostic.disposition = disposition;
    diagnostic.native_error = native_error;
    return diagnostic;
}

[[nodiscard]] CompletionResult cold_reopen(CompletionDiagnostic diagnostic) noexcept {
    diagnostic.disposition = CompletionDisposition::cold_reopen_required;
    if (!diagnostic.native_error) {
        diagnostic.native_error = std::make_error_code(std::errc::state_not_recoverable);
    }
    return {std::nullopt, std::nullopt, std::nullopt, std::move(diagnostic)};
}

[[nodiscard]] CompletionResult retryable_root_or_cold(RootAdmission&& root,
                                                      CompletionDiagnostic diagnostic) noexcept {
    if (!root.valid()) {
        return cold_reopen(std::move(diagnostic));
    }
    std::optional<RootAdmission> retryable_root;
    retryable_root.emplace(std::move(root));
    diagnostic.status = CompletionStatus::retryable_root;
    diagnostic.disposition = CompletionDisposition::retryable_root;
    return {std::move(retryable_root), std::nullopt, std::nullopt, std::move(diagnostic)};
}

[[nodiscard]] CompletionResult
retryable_conversion_or_cold(ConversionCapsule&& capsule,
                             CompletionDiagnostic diagnostic) noexcept {
    if (!capsule.valid()) {
        return cold_reopen(std::move(diagnostic));
    }
    std::optional<ConversionCapsule> retryable_conversion;
    retryable_conversion.emplace(std::move(capsule));
    diagnostic.status = CompletionStatus::retryable_intent_conversion;
    diagnostic.disposition = CompletionDisposition::retryable_intent_conversion;
    return {std::nullopt, std::move(retryable_conversion), std::nullopt, std::move(diagnostic)};
}

[[nodiscard]] CompletionStatus
mint_failure_status(DistributedSieveWorkerCleanupReceiptMintStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::process_mismatch:
        return CompletionStatus::process_mismatch;
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::authorization_not_canonical:
        return CompletionStatus::authorization_not_canonical;
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::resource_exhausted:
        return CompletionStatus::resource_exhausted;
    default:
        return CompletionStatus::receipt_mint_failed;
    }
}

[[nodiscard]] CompletionStatus relation_failure_status(const relation::OOCCleanupResult& result,
                                                       CompletionStatus fallback) noexcept {
    if (result.status == relation::OOCCleanupStatus::PlatformUnsupported) {
        return CompletionStatus::platform_unsupported;
    }
    if (result.native_error == std::make_error_code(std::errc::not_enough_memory)) {
        return CompletionStatus::resource_exhausted;
    }
    return fallback;
}

[[nodiscard]] bool absence_evidence_matches(
    const relation_cleanup::OOCPrivateHandoffCleanupAuthorizationBinding& binding,
    const relation_cleanup::OOCPrivateHandoffCleanupAbsenceEvidenceV2& evidence) noexcept {
    const auto& native = binding.base_path.native();
    const auto characters =
        std::span<const std::filesystem::path::value_type>(native.data(), native.size());
    const auto base_path_digest = gnfs::util::sha256(std::as_bytes(characters));
    return base_path_digest.has_value() && evidence.base_path_digest == *base_path_digest &&
           evidence.external_authorization_digest == binding.external_authorization_digest &&
           evidence.lease_id == binding.lease_id &&
           evidence.parent_directory_identity == binding.parent_directory_identity &&
           evidence.parent_directory_durability_confirmed && evidence.expected_namespace_absent;
}

} // namespace

bool DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1::valid(
    const DistributedSieveWorkerCleanupCompletionReadyCapsuleV1& capsule) noexcept {
    try {
        const int process_id = gnfs::util::process_id();
        if (process_id <= 0 || capsule.creator_process_id_ == 0 ||
            capsule.creator_process_id_ != static_cast<std::uint64_t>(process_id) ||
            capsule.completion_receipt_.spent() || !capsule.root_.reader().valid() ||
            !absence_evidence_matches(capsule.relation_binding_, capsule.absence_evidence_)) {
            return false;
        }

        const auto& prefix = capsule.root_.cleanup_prefix();
        if (!prefix.frontier_manifest_order_ordinal.has_value() ||
            !prefix.active_manifest_order_ordinal.has_value() ||
            *prefix.frontier_manifest_order_ordinal != capsule.manifest_order_ordinal_ ||
            *prefix.active_manifest_order_ordinal != capsule.manifest_order_ordinal_) {
            return false;
        }
        const auto active = std::find_if(
            prefix.coordinates.begin(), prefix.coordinates.end(),
            [&](const distributed_sieve_resume_detail::
                    DistributedSieveWorkerCleanupCoordinateWitnessV1& coordinate) {
                return coordinate.manifest_order_ordinal == capsule.manifest_order_ordinal_;
            });
        if (active == prefix.coordinates.end() ||
            std::find_if(std::next(active), prefix.coordinates.end(),
                         [&](const distributed_sieve_resume_detail::
                                 DistributedSieveWorkerCleanupCoordinateWitnessV1& coordinate) {
                             return coordinate.manifest_order_ordinal ==
                                    capsule.manifest_order_ordinal_;
                         }) != prefix.coordinates.end() ||
            active->state !=
                distributed_sieve_resume_detail::DistributedSieveWorkerCleanupPrefixStateV1::
                    authorization_canonical_only ||
            active->completion.has_value() ||
            active->authorization.self_digest !=
                capsule.relation_binding_.external_authorization_digest ||
            active->authorization.lease.lease_id.limbs != capsule.relation_binding_.lease_id) {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

DistributedSieveWorkerCleanupCompletionPreparationResultV1
DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1::drive(
    DistributedSieveWorkerCleanupAuthorizationPublishedContinuationV1&& authorization) noexcept {
    if (!authorization.valid()) {
        return cold_reopen(
            completion_diagnostic(CompletionPhase::authorization_validation,
                                  CompletionStatus::authorization_not_canonical,
                                  CompletionDisposition::cold_reopen_required,
                                  std::make_error_code(std::errc::invalid_argument)));
    }
    return drive_with_root(std::move(authorization.root_));
}

DistributedSieveWorkerCleanupCompletionPreparationResultV1
DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1::drive_with_root(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept {
    auto retained_root = std::move(root);
    try {
        const int process_id = gnfs::util::process_id();
        if (process_id <= 0 || !retained_root.valid()) {
            return cold_reopen(completion_diagnostic(
                CompletionPhase::admission_validation,
                process_id <= 0 ? CompletionStatus::process_mismatch
                                : CompletionStatus::invalid_admission,
                CompletionDisposition::cold_reopen_required,
                process_id <= 0 ? std::make_error_code(std::errc::no_such_process)
                                : std::make_error_code(std::errc::invalid_argument)));
        }

        const auto& prefix = retained_root.cleanup_prefix();
        if (!prefix.frontier_manifest_order_ordinal.has_value() ||
            !prefix.active_manifest_order_ordinal.has_value() ||
            *prefix.frontier_manifest_order_ordinal != *prefix.active_manifest_order_ordinal) {
            return retryable_root_or_cold(
                std::move(retained_root),
                completion_diagnostic(CompletionPhase::authorization_validation,
                                      CompletionStatus::authorization_not_canonical));
        }
        const std::uint32_t active_ordinal = *prefix.active_manifest_order_ordinal;
        const auto active =
            std::find_if(prefix.coordinates.begin(), prefix.coordinates.end(),
                         [&](const distributed_sieve_resume_detail::
                                 DistributedSieveWorkerCleanupCoordinateWitnessV1& coordinate) {
                             return coordinate.manifest_order_ordinal == active_ordinal;
                         });
        if (active == prefix.coordinates.end() ||
            std::find_if(std::next(active), prefix.coordinates.end(),
                         [&](const distributed_sieve_resume_detail::
                                 DistributedSieveWorkerCleanupCoordinateWitnessV1& coordinate) {
                             return coordinate.manifest_order_ordinal == active_ordinal;
                         }) != prefix.coordinates.end() ||
            active->state !=
                distributed_sieve_resume_detail::DistributedSieveWorkerCleanupPrefixStateV1::
                    authorization_canonical_only ||
            active->completion.has_value()) {
            return retryable_root_or_cold(
                std::move(retained_root),
                completion_diagnostic(CompletionPhase::authorization_validation,
                                      CompletionStatus::authorization_not_canonical));
        }

        CompletionDiagnostic diagnostic = completion_diagnostic(
            CompletionPhase::initial_receipt_mint, CompletionStatus::unexpected_failure);
        std::optional<Minted> active_minted;
        const auto mint_receipt = [&](CompletionPhase phase) -> std::optional<CompletionResult> {
            auto minted =
                mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(retained_root);
            diagnostic.phase = phase;
            const auto mint_status = minted.diagnostic.status;
            const bool cold_required = minted.diagnostic.cold_reopen_required;
            const auto native_error = minted.diagnostic.native_error;
            diagnostic.receipt_mint = std::move(minted.diagnostic);
            if (!minted || !minted.minted.has_value()) {
                diagnostic.status = mint_failure_status(mint_status);
                diagnostic.native_error = native_error;
                if (cold_required ||
                    mint_status ==
                        DistributedSieveWorkerCleanupReceiptMintStatusV1::process_mismatch ||
                    mint_status ==
                        DistributedSieveWorkerCleanupReceiptMintStatusV1::root_authority_invalid ||
                    mint_status == DistributedSieveWorkerCleanupReceiptMintStatusV1::
                                       relation_binding_invalid ||
                    mint_status ==
                        DistributedSieveWorkerCleanupReceiptMintStatusV1::invalid_admission ||
                    mint_status == DistributedSieveWorkerCleanupReceiptMintStatusV1::
                                       authorization_not_canonical) {
                    return cold_reopen(std::move(diagnostic));
                }
                return retryable_root_or_cold(std::move(retained_root), std::move(diagnostic));
            }

            auto retained_minted = std::move(*minted.minted);
            if (retained_minted.manifest_order_ordinal != active_ordinal ||
                retained_minted.receipt.spent()) {
                diagnostic.status = CompletionStatus::receipt_mint_failed;
                diagnostic.native_error = std::make_error_code(std::errc::state_not_recoverable);
                return cold_reopen(std::move(diagnostic));
            }
            active_minted.emplace(std::move(retained_minted));
            return std::nullopt;
        };

        if (auto failed = mint_receipt(CompletionPhase::initial_receipt_mint)) {
            return std::move(*failed);
        }

        const auto observe_prefix = [&] {
            return relation_cleanup::observe_authorized_private_handoff_cleanup_prefix_v2(
                active_minted->relation_binding);
        };
        auto first_observation = observe_prefix();
        diagnostic.phase = CompletionPhase::relation_prefix_observation;
        diagnostic.relation = first_observation.result;
        if (!first_observation.observed()) {
            diagnostic.status = relation_failure_status(
                first_observation.result, CompletionStatus::relation_observation_failed);
            diagnostic.native_error = first_observation.result.native_error;
            const bool retryable =
                first_observation.result.retryable() ||
                first_observation.result.status == relation::OOCCleanupStatus::PlatformUnsupported;
            active_minted.reset();
            if (retryable) {
                return retryable_root_or_cold(std::move(retained_root), std::move(diagnostic));
            }
            return cold_reopen(std::move(diagnostic));
        }
        if (active_minted->receipt.spent() || !retained_root.valid()) {
            active_minted.reset();
            diagnostic.status = CompletionStatus::relation_observation_changed;
            diagnostic.native_error = std::make_error_code(std::errc::state_not_recoverable);
            return cold_reopen(std::move(diagnostic));
        }
        auto second_observation = observe_prefix();
        diagnostic.relation = second_observation.result;
        if (!second_observation.observed() || second_observation != first_observation ||
            active_minted->receipt.spent()) {
            diagnostic.status =
                second_observation.observed()
                    ? CompletionStatus::relation_observation_changed
                    : relation_failure_status(second_observation.result,
                                              CompletionStatus::relation_observation_failed);
            diagnostic.native_error = second_observation.result.native_error
                                          ? second_observation.result.native_error
                                          : std::make_error_code(std::errc::state_not_recoverable);
            active_minted.reset();
            return cold_reopen(std::move(diagnostic));
        }

        const auto observed_state = first_observation.witness->state;
        diagnostic.observed_prefix = observed_state;
        enum class NextAction : std::uint8_t {
            intent_conversion,
            intent_reconciliation,
            cleanup_resume,
        };
        NextAction next_action;
        switch (observed_state) {
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::LiveUnconverted:
            next_action = NextAction::intent_conversion;
            break;
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::IntentPendingOnly:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::IntentCanonicalAndPending:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::IntentCanonicalWithHandoff:
            next_action = NextAction::intent_reconciliation;
            break;
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::IntentCanonicalWithLivePair:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::
            IntentCanonicalWithQuarantinedIndex:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::
            IntentCanonicalWithQuarantinedPair:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::
            IntentCanonicalWithStagedPending:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::
            IntentCanonicalWithStagedAndPending:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::IntentCanonicalWithStagedPair:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::
            IntentCanonicalWithStagedIndex:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::IntentCanonicalWithStagedOnly:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::StagedWithOwner:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::StagedOnly:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::EmptyPrivateDirectory:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::OwnedOnly:
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::Absent:
            next_action = NextAction::cleanup_resume;
            break;
        case relation_cleanup::OOCPrivateHandoffCleanupPrefixStateV2::Count:
            active_minted.reset();
            diagnostic.status = CompletionStatus::relation_observation_failed;
            diagnostic.native_error = std::make_error_code(std::errc::protocol_error);
            return cold_reopen(std::move(diagnostic));
        }

        for (std::size_t transition = 0; transition < 4; ++transition) {
            if (next_action == NextAction::intent_conversion) {
                active_minted.reset();
                auto prepared = prepare_distributed_sieve_worker_cleanup_intent_conversion_v1(
                    std::move(retained_root));
                diagnostic.phase = CompletionPhase::intent_conversion_prepare;
                diagnostic.intent_prepare = std::move(prepared.diagnostic);
                if (prepared.retryable_root.has_value()) {
                    auto retryable = std::move(*prepared.retryable_root);
                    diagnostic.native_error = diagnostic.intent_prepare.native_error;
                    return retryable_root_or_cold(std::move(retryable), std::move(diagnostic));
                }
                if (!prepared.capsule.has_value()) {
                    diagnostic.status = CompletionStatus::intent_conversion_failed;
                    diagnostic.native_error = diagnostic.intent_prepare.native_error;
                    return cold_reopen(std::move(diagnostic));
                }

                auto conversion = execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
                    std::move(*prepared.capsule));
                diagnostic.phase = CompletionPhase::intent_conversion_execute;
                diagnostic.intent_execute = std::move(conversion.diagnostic);
                if (conversion.retryable_capsule.has_value()) {
                    auto retryable = std::move(*conversion.retryable_capsule);
                    diagnostic.native_error = diagnostic.intent_execute.native_error;
                    return retryable_conversion_or_cold(std::move(retryable),
                                                        std::move(diagnostic));
                }
                if (!conversion.root_continuation.has_value()) {
                    diagnostic.status = CompletionStatus::intent_conversion_failed;
                    diagnostic.native_error = diagnostic.intent_execute.native_error;
                    return cold_reopen(std::move(diagnostic));
                }

                const bool intent_published = conversion.publication.intent_published();
                const bool reconciliation_required =
                    conversion.publication.canonical_reconciliation_required();
                retained_root = std::move(*conversion.root_continuation);
                if (!intent_published && !reconciliation_required) {
                    diagnostic.status = CompletionStatus::intent_conversion_failed;
                    diagnostic.native_error =
                        std::make_error_code(std::errc::state_not_recoverable);
                    return cold_reopen(std::move(diagnostic));
                }
                if (auto failed = mint_receipt(CompletionPhase::continuation_receipt_mint)) {
                    return std::move(*failed);
                }
                next_action = reconciliation_required ? NextAction::intent_reconciliation
                                                      : NextAction::cleanup_resume;
                continue;
            }

            if (next_action == NextAction::intent_reconciliation) {
                auto reconciled =
                    relation_cleanup::reconcile_authorized_private_handoff_cleanup_intent_v2(
                        std::move(active_minted->receipt));
                diagnostic.phase = CompletionPhase::intent_reconciliation;
                diagnostic.relation = reconciled.result;
                diagnostic.reconciliation_disposition = reconciled.disposition;
                if (reconciled.intent_canonical()) {
                    active_minted.reset();
                    if (auto failed = mint_receipt(CompletionPhase::continuation_receipt_mint)) {
                        return std::move(*failed);
                    }
                    next_action = NextAction::cleanup_resume;
                    continue;
                }

                diagnostic.status = relation_failure_status(
                    reconciled.result, CompletionStatus::intent_reconciliation_failed);
                diagnostic.native_error = reconciled.result.native_error;
                const bool may_retry_root =
                    reconciled.authorization_retained() || reconciled.reconciliation_required();
                active_minted.reset();
                if (may_retry_root) {
                    return retryable_root_or_cold(std::move(retained_root), std::move(diagnostic));
                }
                return cold_reopen(std::move(diagnostic));
            }

            auto resumed = relation_cleanup::resume_authorized_private_handoff_cleanup_v2(
                std::move(active_minted->receipt));
            diagnostic.phase = CompletionPhase::cleanup_resume;
            diagnostic.relation = resumed.result;
            diagnostic.cleanup_disposition = resumed.disposition;
            if (resumed.namespace_absent()) {
                auto evidence = std::move(*resumed.evidence);
                auto cleanup_binding = std::move(active_minted->relation_binding);
                if (!absence_evidence_matches(cleanup_binding, evidence)) {
                    active_minted.reset();
                    diagnostic.status = CompletionStatus::absence_evidence_invalid;
                    diagnostic.native_error = std::make_error_code(std::errc::protocol_error);
                    return cold_reopen(std::move(diagnostic));
                }
                active_minted.reset();
                if (auto failed = mint_receipt(CompletionPhase::completion_receipt_mint)) {
                    return std::move(*failed);
                }

                auto completion_minted = std::move(*active_minted);
                active_minted.reset();
                if (completion_minted.manifest_order_ordinal != active_ordinal ||
                    completion_minted.relation_binding != cleanup_binding) {
                    diagnostic.status = CompletionStatus::absence_evidence_invalid;
                    diagnostic.native_error =
                        std::make_error_code(std::errc::state_not_recoverable);
                    return cold_reopen(std::move(diagnostic));
                }
                diagnostic.phase = CompletionPhase::completion_capsule_construction;
                CompletionCapsule sealed(active_ordinal, static_cast<std::uint64_t>(process_id),
                                         std::move(retained_root),
                                         std::move(completion_minted.relation_binding),
                                         std::move(completion_minted.receipt), std::move(evidence));
                if (!sealed.valid()) {
                    diagnostic.status = CompletionStatus::absence_evidence_invalid;
                    diagnostic.native_error =
                        std::make_error_code(std::errc::state_not_recoverable);
                    return cold_reopen(std::move(diagnostic));
                }
                std::optional<CompletionCapsule> completion_ready;
                completion_ready.emplace(std::move(sealed));
                diagnostic.phase = CompletionPhase::complete;
                diagnostic.status = CompletionStatus::ready;
                diagnostic.disposition = CompletionDisposition::completion_ready;
                diagnostic.native_error.clear();
                return {std::nullopt, std::nullopt, std::move(completion_ready),
                        std::move(diagnostic)};
            }

            diagnostic.status =
                relation_failure_status(resumed.result, CompletionStatus::cleanup_resume_failed);
            diagnostic.native_error = resumed.result.native_error;
            const bool may_retry_root =
                resumed.authorization_retained() || resumed.reconciliation_required();
            active_minted.reset();
            if (may_retry_root) {
                return retryable_root_or_cold(std::move(retained_root), std::move(diagnostic));
            }
            return cold_reopen(std::move(diagnostic));
        }

        active_minted.reset();
        diagnostic.status = CompletionStatus::unexpected_failure;
        diagnostic.native_error = std::make_error_code(std::errc::state_not_recoverable);
        return cold_reopen(std::move(diagnostic));
    } catch (const std::bad_alloc&) {
        return cold_reopen(completion_diagnostic(
            CompletionPhase::completion_capsule_construction, CompletionStatus::resource_exhausted,
            CompletionDisposition::cold_reopen_required,
            std::make_error_code(std::errc::not_enough_memory)));
    } catch (const std::filesystem::filesystem_error& error) {
        return cold_reopen(completion_diagnostic(
            CompletionPhase::completion_capsule_construction, CompletionStatus::unexpected_failure,
            CompletionDisposition::cold_reopen_required, error.code()));
    } catch (...) {
        return cold_reopen(completion_diagnostic(CompletionPhase::completion_capsule_construction,
                                                 CompletionStatus::unexpected_failure,
                                                 CompletionDisposition::cold_reopen_required,
                                                 std::make_error_code(std::errc::io_error)));
    }
}

DistributedSieveWorkerCleanupCompletionPreparationResultV1
drive_distributed_sieve_worker_cleanup_to_completion_ready_v1(
    DistributedSieveWorkerCleanupAuthorizationPublishedContinuationV1&& authorization) noexcept {
    return DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1::drive(
        std::move(authorization));
}

namespace trusted_test {

DistributedSieveWorkerCleanupCompletionPreparationResultV1
drive_distributed_sieve_worker_cleanup_to_completion_ready_v1_with_root(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept {
    return DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1::drive_with_root(
        std::move(root));
}

} // namespace trusted_test

} // namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail
