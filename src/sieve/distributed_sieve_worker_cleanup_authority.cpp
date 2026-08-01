#include "distributed_sieve_worker_cleanup_authority_internal.hpp"

#include <gnfs/util/process.hpp>

#include <filesystem>
#include <memory>
#include <new>
#include <optional>
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

} // namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail
