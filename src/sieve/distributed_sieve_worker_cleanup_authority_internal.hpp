#pragma once

// Source-private, one-shot lock-generation bridge from a committed merge tail
// to the worker-cleanup cold-open authority. The bridge never publishes,
// repairs, or removes durable state.

#include "distributed_sieve_merge_prepared_admission_internal.hpp"
#include "distributed_sieve_wave_store_internal.hpp"
#include "distributed_sieve_worker_cleanup_codec_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail {

using distributed_sieve_merge_writer_authority_detail::DistributedSieveCommittedTailAdmissionV1;
using distributed_sieve_resume_detail::DistributedSieveWorkerCleanupRootAdmissionV1;

enum class DistributedSieveWorkerCleanupReceiptMintPhaseV1 : std::uint8_t {
    admission_validation,
    relation_binding_projection,
    live_claim,
    root_revalidation,
    receipt_construction,
    complete,
};

enum class DistributedSieveWorkerCleanupReceiptMintStatusV1 : std::uint8_t {
    ready,
    invalid_admission,
    authorization_not_canonical,
    relation_binding_invalid,
    receipt_already_live,
    root_authority_invalid,
    process_mismatch,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_worker_cleanup_receipt_mint_status_name(
    DistributedSieveWorkerCleanupReceiptMintStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::ready:
        return "ready";
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::invalid_admission:
        return "invalid_admission";
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::authorization_not_canonical:
        return "authorization_not_canonical";
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::relation_binding_invalid:
        return "relation_binding_invalid";
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::receipt_already_live:
        return "receipt_already_live";
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::root_authority_invalid:
        return "root_authority_invalid";
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerCleanupReceiptMintStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveWorkerCleanupReceiptMintDiagnosticV1 final {
    DistributedSieveWorkerCleanupReceiptMintPhaseV1 phase =
        DistributedSieveWorkerCleanupReceiptMintPhaseV1::admission_validation;
    DistributedSieveWorkerCleanupReceiptMintStatusV1 status =
        DistributedSieveWorkerCleanupReceiptMintStatusV1::unexpected_failure;
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
    distributed_sieve_worker_cleanup_codec_detail::DistributedSieveWorkerCleanupCodecDiagnosticV1
        codec;
    std::error_code native_error;
    bool cold_reopen_required = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return phase == DistributedSieveWorkerCleanupReceiptMintPhaseV1::complete &&
               status == DistributedSieveWorkerCleanupReceiptMintStatusV1::ready && !native_error &&
               !cold_reopen_required;
    }
};

struct DistributedSieveWorkerCleanupReceiptMintedV1 final {
    DistributedSieveWorkerCleanupReceiptMintedV1(
        std::uint32_t ordinal,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding&&
            relation_binding_value,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt&&
            receipt_value) noexcept
        : manifest_order_ordinal(ordinal), relation_binding(std::move(relation_binding_value)),
          receipt(std::move(receipt_value)) {}
    DistributedSieveWorkerCleanupReceiptMintedV1(
        const DistributedSieveWorkerCleanupReceiptMintedV1&) = delete;
    DistributedSieveWorkerCleanupReceiptMintedV1&
    operator=(const DistributedSieveWorkerCleanupReceiptMintedV1&) = delete;
    DistributedSieveWorkerCleanupReceiptMintedV1(
        DistributedSieveWorkerCleanupReceiptMintedV1&&) noexcept = default;
    DistributedSieveWorkerCleanupReceiptMintedV1&
    operator=(DistributedSieveWorkerCleanupReceiptMintedV1&&) = delete;

    std::uint32_t manifest_order_ordinal = 0;
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding relation_binding;
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt receipt;
};

struct DistributedSieveWorkerCleanupReceiptMintResultV1 final {
    DistributedSieveWorkerCleanupReceiptMintResultV1() = default;
    DistributedSieveWorkerCleanupReceiptMintResultV1(
        std::optional<DistributedSieveWorkerCleanupReceiptMintedV1> minted_value,
        DistributedSieveWorkerCleanupReceiptMintDiagnosticV1 diagnostic_value) noexcept
        : minted(std::move(minted_value)), diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWorkerCleanupReceiptMintResultV1(
        const DistributedSieveWorkerCleanupReceiptMintResultV1&) = delete;
    DistributedSieveWorkerCleanupReceiptMintResultV1&
    operator=(const DistributedSieveWorkerCleanupReceiptMintResultV1&) = delete;
    DistributedSieveWorkerCleanupReceiptMintResultV1(
        DistributedSieveWorkerCleanupReceiptMintResultV1&&) noexcept = default;
    DistributedSieveWorkerCleanupReceiptMintResultV1&
    operator=(DistributedSieveWorkerCleanupReceiptMintResultV1&&) = delete;

    std::optional<DistributedSieveWorkerCleanupReceiptMintedV1> minted;
    DistributedSieveWorkerCleanupReceiptMintDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return minted.has_value() && static_cast<bool>(diagnostic);
    }
};

[[nodiscard]] DistributedSieveWorkerCleanupReceiptMintResultV1
mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
    DistributedSieveWorkerCleanupRootAdmissionV1& admission) noexcept;

class DistributedSieveWorkerCleanupReceiptMintAuthorityV1 final {
public:
    DistributedSieveWorkerCleanupReceiptMintAuthorityV1() = delete;

private:
    [[nodiscard]] static DistributedSieveWorkerCleanupReceiptMintResultV1
    mint(DistributedSieveWorkerCleanupRootAdmissionV1& admission) noexcept;

    friend DistributedSieveWorkerCleanupReceiptMintResultV1
    mint_distributed_sieve_worker_cleanup_authorization_receipt_v1(
        DistributedSieveWorkerCleanupRootAdmissionV1& admission) noexcept;
};

class DistributedSieveWorkerCleanupIntentConversionAuthorityV1;

/// Sealed, process-bound T2a authority capsule for one active cleanup worker.
///
/// The retained root keeps WaveLock and the merged read-only corpus alive. The
/// receipt owns the root-only liveness claim, while the relation reader owns
/// the duplicated descriptor for the exact adopted worker BaseLock. Member
/// order is intentional: reverse destruction closes the worker reader before
/// releasing its receipt claim and finally the cleanup-root WaveLock.
class DistributedSieveWorkerCleanupIntentConversionCapsuleV1 final {
public:
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1() = delete;
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1(
        const DistributedSieveWorkerCleanupIntentConversionCapsuleV1&) = delete;
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1&
    operator=(const DistributedSieveWorkerCleanupIntentConversionCapsuleV1&) = delete;
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1(
        DistributedSieveWorkerCleanupIntentConversionCapsuleV1&&) noexcept;
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1&
    operator=(DistributedSieveWorkerCleanupIntentConversionCapsuleV1&&) = delete;
    ~DistributedSieveWorkerCleanupIntentConversionCapsuleV1() noexcept;

    /// Local held-state validation plus the receipt's root-control-only sticky
    /// scan. This never calls RootAdmission::valid(), performs a full cleanup-
    /// root observation, or enters a worker relation/BaseLock namespace. The
    /// receipt may open and re-read root control leaves through the retained
    /// root handle and sticky-invalidate on drift; it never duplicates or
    /// flocks the retained reader/BaseLock open-file description.
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint32_t manifest_order_ordinal() const noexcept {
        return manifest_order_ordinal_;
    }

private:
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1(
        std::uint32_t manifest_order_ordinal, std::uint64_t creator_process_id,
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt&& receipt,
        std::unique_ptr<relation::OOCPrivateHandoffReader> reader) noexcept;

    std::uint32_t manifest_order_ordinal_ = 0;
    std::uint64_t creator_process_id_ = 0;
    DistributedSieveWorkerCleanupRootAdmissionV1 root_;
    std::optional<relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt>
        receipt_;
    std::unique_ptr<relation::OOCPrivateHandoffReader> reader_;

    friend class DistributedSieveWorkerCleanupIntentConversionAuthorityV1;
};

enum class DistributedSieveWorkerCleanupIntentConversionPreparePhaseV1 : std::uint8_t {
    admission_validation,
    receipt_mint,
    active_worker_selection,
    worker_adoption,
    capsule_construction,
    complete,
};

enum class DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1 : std::uint8_t {
    ready,
    invalid_admission,
    process_mismatch,
    receipt_mint_failed,
    active_worker_missing,
    active_worker_ambiguous,
    worker_adoption_failed,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_worker_cleanup_intent_conversion_prepare_status_name(
    DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::ready:
        return "ready";
    case DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::invalid_admission:
        return "invalid_admission";
    case DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::receipt_mint_failed:
        return "receipt_mint_failed";
    case DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::active_worker_missing:
        return "active_worker_missing";
    case DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::active_worker_ambiguous:
        return "active_worker_ambiguous";
    case DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::worker_adoption_failed:
        return "worker_adoption_failed";
    case DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveWorkerCleanupIntentConversionPrepareDiagnosticV1 final {
    DistributedSieveWorkerCleanupIntentConversionPreparePhaseV1 phase =
        DistributedSieveWorkerCleanupIntentConversionPreparePhaseV1::admission_validation;
    DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1 status =
        DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::unexpected_failure;
    DistributedSieveWorkerCleanupReceiptMintDiagnosticV1 receipt_mint;
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
    std::error_code native_error;
    bool root_retained = false;
    bool cold_reopen_required = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return phase == DistributedSieveWorkerCleanupIntentConversionPreparePhaseV1::complete &&
               status == DistributedSieveWorkerCleanupIntentConversionPrepareStatusV1::ready &&
               !native_error && !root_retained && !cold_reopen_required;
    }
};

struct DistributedSieveWorkerCleanupIntentConversionPrepareResultV1 final {
    DistributedSieveWorkerCleanupIntentConversionPrepareResultV1() = default;
    DistributedSieveWorkerCleanupIntentConversionPrepareResultV1(
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> retryable_root_value,
        std::optional<DistributedSieveWorkerCleanupIntentConversionCapsuleV1> capsule_value,
        DistributedSieveWorkerCleanupIntentConversionPrepareDiagnosticV1 diagnostic_value) noexcept
        : retryable_root(std::move(retryable_root_value)), capsule(std::move(capsule_value)),
          diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWorkerCleanupIntentConversionPrepareResultV1(
        const DistributedSieveWorkerCleanupIntentConversionPrepareResultV1&) = delete;
    DistributedSieveWorkerCleanupIntentConversionPrepareResultV1&
    operator=(const DistributedSieveWorkerCleanupIntentConversionPrepareResultV1&) = delete;
    DistributedSieveWorkerCleanupIntentConversionPrepareResultV1(
        DistributedSieveWorkerCleanupIntentConversionPrepareResultV1&&) noexcept = default;
    DistributedSieveWorkerCleanupIntentConversionPrepareResultV1&
    operator=(DistributedSieveWorkerCleanupIntentConversionPrepareResultV1&&) = delete;

    std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> retryable_root;
    std::optional<DistributedSieveWorkerCleanupIntentConversionCapsuleV1> capsule;
    DistributedSieveWorkerCleanupIntentConversionPrepareDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(diagnostic) && !retryable_root.has_value() &&
               capsule.has_value() && capsule->valid();
    }
};

enum class DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1 : std::uint8_t {
    capsule_validation,
    intent_publication,
    capability_release,
    complete,
};

enum class DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1 : std::uint8_t {
    capabilities_retained,
    intent_published,
    canonical_reconciliation_required,
    invalid_capsule,
    process_mismatch,
    publication_failed,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_worker_cleanup_intent_conversion_execute_status_name(
    DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::capabilities_retained:
        return "capabilities_retained";
    case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::intent_published:
        return "intent_published";
    case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::
        canonical_reconciliation_required:
        return "canonical_reconciliation_required";
    case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::invalid_capsule:
        return "invalid_capsule";
    case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::publication_failed:
        return "publication_failed";
    case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

struct DistributedSieveWorkerCleanupIntentConversionExecuteDiagnosticV1 final {
    DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1 phase =
        DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1::capsule_validation;
    DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1 status =
        DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::unexpected_failure;
    std::error_code native_error;
    bool capsule_retained = false;
    bool root_continuation_retained = false;
    bool cold_reopen_required = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        if (phase != DistributedSieveWorkerCleanupIntentConversionExecutePhaseV1::complete ||
            native_error || cold_reopen_required) {
            return false;
        }
        switch (status) {
        case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::capabilities_retained:
            return capsule_retained && !root_continuation_retained;
        case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::intent_published:
        case DistributedSieveWorkerCleanupIntentConversionExecuteStatusV1::
            canonical_reconciliation_required:
            return !capsule_retained && root_continuation_retained;
        default:
            return false;
        }
    }
};

struct DistributedSieveWorkerCleanupIntentConversionExecuteResultV1 final {
    DistributedSieveWorkerCleanupIntentConversionExecuteResultV1() = default;
    DistributedSieveWorkerCleanupIntentConversionExecuteResultV1(
        std::optional<DistributedSieveWorkerCleanupIntentConversionCapsuleV1>
            retryable_capsule_value,
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> root_continuation_value,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationResultV2
            publication_value,
        DistributedSieveWorkerCleanupIntentConversionExecuteDiagnosticV1 diagnostic_value) noexcept
        : retryable_capsule(std::move(retryable_capsule_value)),
          root_continuation(std::move(root_continuation_value)),
          publication(std::move(publication_value)), diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWorkerCleanupIntentConversionExecuteResultV1(
        const DistributedSieveWorkerCleanupIntentConversionExecuteResultV1&) = delete;
    DistributedSieveWorkerCleanupIntentConversionExecuteResultV1&
    operator=(const DistributedSieveWorkerCleanupIntentConversionExecuteResultV1&) = delete;
    DistributedSieveWorkerCleanupIntentConversionExecuteResultV1(
        DistributedSieveWorkerCleanupIntentConversionExecuteResultV1&&) noexcept = default;
    DistributedSieveWorkerCleanupIntentConversionExecuteResultV1&
    operator=(DistributedSieveWorkerCleanupIntentConversionExecuteResultV1&&) = delete;

    std::optional<DistributedSieveWorkerCleanupIntentConversionCapsuleV1> retryable_capsule;
    std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> root_continuation;
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationResultV2 publication;
    DistributedSieveWorkerCleanupIntentConversionExecuteDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        if (!static_cast<bool>(diagnostic)) {
            return false;
        }
        if (diagnostic.capsule_retained) {
            return retryable_capsule.has_value() && !root_continuation.has_value();
        }
        return !retryable_capsule.has_value() && root_continuation.has_value();
    }
};

[[nodiscard]] DistributedSieveWorkerCleanupIntentConversionPrepareResultV1
prepare_distributed_sieve_worker_cleanup_intent_conversion_v1(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;

[[nodiscard]] DistributedSieveWorkerCleanupIntentConversionExecuteResultV1
execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule) noexcept;

namespace trusted_test {

[[nodiscard]] DistributedSieveWorkerCleanupIntentConversionExecuteResultV1
execute_distributed_sieve_worker_cleanup_intent_conversion_v1_with_hooks(
    DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule,
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2
        hooks) noexcept;

} // namespace trusted_test

class DistributedSieveWorkerCleanupIntentConversionAuthorityV1 final {
public:
    DistributedSieveWorkerCleanupIntentConversionAuthorityV1() = delete;

private:
    [[nodiscard]] static bool
    valid(const DistributedSieveWorkerCleanupIntentConversionCapsuleV1& capsule) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupIntentConversionPrepareResultV1
    prepare(DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupIntentConversionExecuteResultV1 execute(
        DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2 hooks,
        bool trusted_test_hooks) noexcept;

    friend class DistributedSieveWorkerCleanupIntentConversionCapsuleV1;
    friend DistributedSieveWorkerCleanupIntentConversionPrepareResultV1
    prepare_distributed_sieve_worker_cleanup_intent_conversion_v1(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;
    friend DistributedSieveWorkerCleanupIntentConversionExecuteResultV1
    execute_distributed_sieve_worker_cleanup_intent_conversion_v1(
        DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule) noexcept;
    friend DistributedSieveWorkerCleanupIntentConversionExecuteResultV1
    trusted_test::execute_distributed_sieve_worker_cleanup_intent_conversion_v1_with_hooks(
        DistributedSieveWorkerCleanupIntentConversionCapsuleV1&& capsule,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentPublicationTestHooksV2
            hooks) noexcept;
};

class DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1;
class DistributedSieveWorkerCleanupCompletionPublicationAuthorityV1;

/// Sealed R1 result proving that one canonical root authorization has reached
/// parent-durable relation-namespace absence. This capsule publishes no root
/// record. Its fresh, unspent receipt keeps the exact root-control generation
/// sticky-live until the future typed completion publisher consumes it.
class DistributedSieveWorkerCleanupCompletionReadyCapsuleV1 final {
public:
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1() = delete;
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1(
        const DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&) = delete;
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&
    operator=(const DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&) = delete;
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1(
        DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&&) noexcept;
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&
    operator=(DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&&) = delete;
    ~DistributedSieveWorkerCleanupCompletionReadyCapsuleV1() noexcept;

    /// Local evidence/binding checks plus the receipt's root-control-only
    /// sticky scan. Relation cleanup has deliberately changed the cold-open
    /// relation prefix, so this does not call RootAdmission::valid().
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint32_t manifest_order_ordinal() const noexcept {
        return manifest_order_ordinal_;
    }

private:
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1(
        std::uint32_t manifest_order_ordinal, std::uint64_t creator_process_id,
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding&&
            relation_binding,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt&&
            completion_receipt,
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAbsenceEvidenceV2&&
            absence_evidence) noexcept;

    std::uint32_t manifest_order_ordinal_ = 0;
    std::uint64_t creator_process_id_ = 0;
    // Reverse destruction releases the completion receipt before the retained
    // merged reader and cleanup-root WaveLock.
    DistributedSieveWorkerCleanupRootAdmissionV1 root_;
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationBinding relation_binding_;
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAuthorizationReceipt completion_receipt_;
    relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupAbsenceEvidenceV2 absence_evidence_;

    friend class DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1;
    friend class DistributedSieveWorkerCleanupCompletionPublicationAuthorityV1;
};

enum class DistributedSieveWorkerCleanupCompletionPreparationPhaseV1 : std::uint8_t {
    admission_validation,
    authorization_validation,
    initial_receipt_mint,
    relation_prefix_observation,
    intent_conversion_prepare,
    intent_conversion_execute,
    intent_reconciliation,
    continuation_receipt_mint,
    cleanup_resume,
    completion_receipt_mint,
    completion_capsule_construction,
    complete,
};

enum class DistributedSieveWorkerCleanupCompletionPreparationStatusV1 : std::uint8_t {
    ready,
    retryable_root,
    retryable_intent_conversion,
    invalid_admission,
    process_mismatch,
    authorization_not_canonical,
    receipt_mint_failed,
    relation_observation_failed,
    relation_observation_changed,
    intent_conversion_failed,
    intent_reconciliation_failed,
    cleanup_resume_failed,
    absence_evidence_invalid,
    platform_unsupported,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_worker_cleanup_completion_preparation_status_name(
    DistributedSieveWorkerCleanupCompletionPreparationStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::ready:
        return "ready";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::retryable_root:
        return "retryable_root";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::retryable_intent_conversion:
        return "retryable_intent_conversion";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::invalid_admission:
        return "invalid_admission";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::authorization_not_canonical:
        return "authorization_not_canonical";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::receipt_mint_failed:
        return "receipt_mint_failed";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::relation_observation_failed:
        return "relation_observation_failed";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::relation_observation_changed:
        return "relation_observation_changed";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::intent_conversion_failed:
        return "intent_conversion_failed";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::intent_reconciliation_failed:
        return "intent_reconciliation_failed";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::cleanup_resume_failed:
        return "cleanup_resume_failed";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::absence_evidence_invalid:
        return "absence_evidence_invalid";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerCleanupCompletionPreparationStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

enum class DistributedSieveWorkerCleanupCompletionPreparationDispositionV1 : std::uint8_t {
    retryable_root,
    retryable_intent_conversion,
    completion_ready,
    cold_reopen_required,
};

struct DistributedSieveWorkerCleanupCompletionPreparationDiagnosticV1 final {
    DistributedSieveWorkerCleanupCompletionPreparationPhaseV1 phase =
        DistributedSieveWorkerCleanupCompletionPreparationPhaseV1::admission_validation;
    DistributedSieveWorkerCleanupCompletionPreparationStatusV1 status =
        DistributedSieveWorkerCleanupCompletionPreparationStatusV1::unexpected_failure;
    DistributedSieveWorkerCleanupCompletionPreparationDispositionV1 disposition =
        DistributedSieveWorkerCleanupCompletionPreparationDispositionV1::cold_reopen_required;
    DistributedSieveWorkerCleanupReceiptMintDiagnosticV1 receipt_mint;
    DistributedSieveWorkerCleanupIntentConversionPrepareDiagnosticV1 intent_prepare;
    DistributedSieveWorkerCleanupIntentConversionExecuteDiagnosticV1 intent_execute;
    relation::OOCCleanupResult relation;
    std::optional<relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupPrefixStateV2>
        observed_prefix;
    std::optional<
        relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupIntentReconciliationDispositionV2>
        reconciliation_disposition;
    std::optional<relation::ooc_cleanup_detail::OOCPrivateHandoffCleanupResumeDispositionV2>
        cleanup_disposition;
    std::error_code native_error;
};

struct DistributedSieveWorkerCleanupCompletionPreparationResultV1 final {
    DistributedSieveWorkerCleanupCompletionPreparationResultV1() = default;
    DistributedSieveWorkerCleanupCompletionPreparationResultV1(
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> retryable_root_value,
        std::optional<DistributedSieveWorkerCleanupIntentConversionCapsuleV1>
            retryable_intent_conversion_value,
        std::optional<DistributedSieveWorkerCleanupCompletionReadyCapsuleV1> completion_ready_value,
        DistributedSieveWorkerCleanupCompletionPreparationDiagnosticV1 diagnostic_value) noexcept
        : retryable_root(std::move(retryable_root_value)),
          retryable_intent_conversion(std::move(retryable_intent_conversion_value)),
          completion_ready(std::move(completion_ready_value)),
          diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWorkerCleanupCompletionPreparationResultV1(
        const DistributedSieveWorkerCleanupCompletionPreparationResultV1&) = delete;
    DistributedSieveWorkerCleanupCompletionPreparationResultV1&
    operator=(const DistributedSieveWorkerCleanupCompletionPreparationResultV1&) = delete;
    DistributedSieveWorkerCleanupCompletionPreparationResultV1(
        DistributedSieveWorkerCleanupCompletionPreparationResultV1&&) noexcept = default;
    DistributedSieveWorkerCleanupCompletionPreparationResultV1&
    operator=(DistributedSieveWorkerCleanupCompletionPreparationResultV1&&) = delete;

    std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> retryable_root;
    std::optional<DistributedSieveWorkerCleanupIntentConversionCapsuleV1>
        retryable_intent_conversion;
    std::optional<DistributedSieveWorkerCleanupCompletionReadyCapsuleV1> completion_ready;
    DistributedSieveWorkerCleanupCompletionPreparationDiagnosticV1 diagnostic;

    /// True for each structurally valid ownership-bearing continuation. A cold
    /// reopen result owns nothing and is false.
    [[nodiscard]] explicit operator bool() const noexcept;
};

[[nodiscard]] DistributedSieveWorkerCleanupCompletionPreparationResultV1
drive_distributed_sieve_worker_cleanup_to_completion_ready_v1(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;

class DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1 final {
public:
    DistributedSieveWorkerCleanupCompletionPreparationAuthorityV1() = delete;

private:
    [[nodiscard]] static bool
    valid(const DistributedSieveWorkerCleanupCompletionReadyCapsuleV1& capsule) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupCompletionPreparationResultV1
    drive(DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;

    friend class DistributedSieveWorkerCleanupCompletionReadyCapsuleV1;
    friend DistributedSieveWorkerCleanupCompletionPreparationResultV1
    drive_distributed_sieve_worker_cleanup_to_completion_ready_v1(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;
};

/// Sealed R2 continuation proving that one exact worker cleanup completion is
/// canonical-durable and that the retained WaveLock/merged reader have been
/// refreshed to the exact one-record successor. The underlying cleanup-root
/// admission is intentionally not exposed as a standalone continuation.
class DistributedSieveWorkerCleanupCompletionPublishedContinuationV1 final {
public:
    DistributedSieveWorkerCleanupCompletionPublishedContinuationV1() = delete;
    DistributedSieveWorkerCleanupCompletionPublishedContinuationV1(
        const DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&) = delete;
    DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&
    operator=(const DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&) = delete;
    DistributedSieveWorkerCleanupCompletionPublishedContinuationV1(
        DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&&) noexcept;
    DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&
    operator=(DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&&) = delete;
    ~DistributedSieveWorkerCleanupCompletionPublishedContinuationV1() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint32_t manifest_order_ordinal() const noexcept {
        return manifest_order_ordinal_;
    }
    [[nodiscard]] const util::Sha256Digest& completion_digest() const noexcept {
        return completion_digest_;
    }

private:
    DistributedSieveWorkerCleanupCompletionPublishedContinuationV1(
        std::uint32_t manifest_order_ordinal, std::uint64_t creator_process_id,
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        std::vector<std::byte>&& completion_bytes,
        util::durable_immutable_record::RecordSnapshot completion_snapshot,
        util::Sha256Digest completion_digest) noexcept;

    std::uint32_t manifest_order_ordinal_ = 0;
    std::uint64_t creator_process_id_ = 0;
    // Reverse destruction releases pure completion material before WaveLock.
    DistributedSieveWorkerCleanupRootAdmissionV1 root_;
    std::vector<std::byte> completion_bytes_;
    util::durable_immutable_record::RecordSnapshot completion_snapshot_;
    util::Sha256Digest completion_digest_;

    friend class DistributedSieveWorkerCleanupCompletionPublicationAuthorityV1;
};

namespace trusted_test {

enum class DistributedSieveWorkerCleanupCompletionPublicationFaultPointV1 : std::uint8_t {
    FreshBeforeReceiptSpend,
    FreshAfterReceiptSpend,
    RecoveryBeforePublication,
    PendingDurable,
    CanonicalPromoted,
    CanonicalDurable,
    AfterFirstSuccessorObservation,
    Count,
};

struct DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1 final {
    using StopAfter = bool (*)(DistributedSieveWorkerCleanupCompletionPublicationFaultPointV1 point,
                               void* context) noexcept;

    StopAfter stop_after = nullptr;
    void* context = nullptr;
};

} // namespace trusted_test

enum class DistributedSieveWorkerCleanupCompletionPublicationPhaseV1 : std::uint8_t {
    input_validation,
    recovery_target_selection,
    completion_build,
    baseline_observation,
    successor_preparation,
    authority_spend,
    record_publication,
    successor_observation,
    root_refresh,
    continuation_construction,
    complete,
};

enum class DistributedSieveWorkerCleanupCompletionPublicationStatusV1 : std::uint8_t {
    published,
    retryable_completion_ready,
    retryable_recovery_root,
    invalid_input,
    process_mismatch,
    completion_build_failed,
    completion_target_missing,
    completion_target_invalid,
    baseline_observation_failed,
    baseline_changed,
    test_interrupted,
    publication_failed,
    publication_disposition_mismatch,
    successor_observation_failed,
    successor_mismatch,
    root_refresh_failed,
    platform_unsupported,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view
distributed_sieve_worker_cleanup_completion_publication_status_name(
    DistributedSieveWorkerCleanupCompletionPublicationStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::published:
        return "published";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::retryable_completion_ready:
        return "retryable_completion_ready";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::retryable_recovery_root:
        return "retryable_recovery_root";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::invalid_input:
        return "invalid_input";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::completion_build_failed:
        return "completion_build_failed";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::completion_target_missing:
        return "completion_target_missing";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::completion_target_invalid:
        return "completion_target_invalid";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::baseline_observation_failed:
        return "baseline_observation_failed";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::baseline_changed:
        return "baseline_changed";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::test_interrupted:
        return "test_interrupted";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::publication_failed:
        return "publication_failed";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::
        publication_disposition_mismatch:
        return "publication_disposition_mismatch";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::successor_observation_failed:
        return "successor_observation_failed";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::successor_mismatch:
        return "successor_mismatch";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::root_refresh_failed:
        return "root_refresh_failed";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerCleanupCompletionPublicationStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

enum class DistributedSieveWorkerCleanupCompletionPublicationDispositionV1 : std::uint8_t {
    retryable_completion_ready,
    retryable_recovery_root,
    completion_published,
    cold_reopen_required,
};

struct DistributedSieveWorkerCleanupCompletionPublicationDiagnosticV1 final {
    DistributedSieveWorkerCleanupCompletionPublicationPhaseV1 phase =
        DistributedSieveWorkerCleanupCompletionPublicationPhaseV1::input_validation;
    DistributedSieveWorkerCleanupCompletionPublicationStatusV1 status =
        DistributedSieveWorkerCleanupCompletionPublicationStatusV1::unexpected_failure;
    DistributedSieveWorkerCleanupCompletionPublicationDispositionV1 disposition =
        DistributedSieveWorkerCleanupCompletionPublicationDispositionV1::cold_reopen_required;
    std::optional<std::uint32_t> manifest_order_ordinal;
    distributed_sieve_worker_cleanup_codec_detail::DistributedSieveWorkerCleanupCodecDiagnosticV1
        codec;
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
    std::optional<util::durable_immutable_record::RecordPublishStatus> publication_status;
    std::optional<util::durable_immutable_record::RecordPublishDisposition> publication_disposition;
    std::optional<trusted_test::DistributedSieveWorkerCleanupCompletionPublicationFaultPointV1>
        last_fault_point;
    std::error_code native_error;
    bool authority_spent = false;
    bool publication_started = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return phase == DistributedSieveWorkerCleanupCompletionPublicationPhaseV1::complete &&
               status == DistributedSieveWorkerCleanupCompletionPublicationStatusV1::published &&
               disposition == DistributedSieveWorkerCleanupCompletionPublicationDispositionV1::
                                  completion_published &&
               publication_status == util::durable_immutable_record::RecordPublishStatus::durable &&
               publication_disposition.has_value() && authority_spent && publication_started &&
               !native_error;
    }
};

struct DistributedSieveWorkerCleanupCompletionPublicationResultV1 final {
    DistributedSieveWorkerCleanupCompletionPublicationResultV1() = default;
    DistributedSieveWorkerCleanupCompletionPublicationResultV1(
        std::optional<DistributedSieveWorkerCleanupCompletionReadyCapsuleV1>
            retryable_completion_ready_value,
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> retryable_recovery_root_value,
        std::optional<DistributedSieveWorkerCleanupCompletionPublishedContinuationV1>
            published_continuation_value,
        DistributedSieveWorkerCleanupCompletionPublicationDiagnosticV1 diagnostic_value) noexcept
        : retryable_completion_ready(std::move(retryable_completion_ready_value)),
          retryable_recovery_root(std::move(retryable_recovery_root_value)),
          published_continuation(std::move(published_continuation_value)),
          diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWorkerCleanupCompletionPublicationResultV1(
        const DistributedSieveWorkerCleanupCompletionPublicationResultV1&) = delete;
    DistributedSieveWorkerCleanupCompletionPublicationResultV1&
    operator=(const DistributedSieveWorkerCleanupCompletionPublicationResultV1&) = delete;
    DistributedSieveWorkerCleanupCompletionPublicationResultV1(
        DistributedSieveWorkerCleanupCompletionPublicationResultV1&&) noexcept = default;
    DistributedSieveWorkerCleanupCompletionPublicationResultV1&
    operator=(DistributedSieveWorkerCleanupCompletionPublicationResultV1&&) = delete;

    std::optional<DistributedSieveWorkerCleanupCompletionReadyCapsuleV1> retryable_completion_ready;
    std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> retryable_recovery_root;
    std::optional<DistributedSieveWorkerCleanupCompletionPublishedContinuationV1>
        published_continuation;
    DistributedSieveWorkerCleanupCompletionPublicationDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept;
};

[[nodiscard]] DistributedSieveWorkerCleanupCompletionPublicationResultV1
publish_distributed_sieve_worker_cleanup_completion_v1(
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&& completion_ready) noexcept;

[[nodiscard]] DistributedSieveWorkerCleanupCompletionPublicationResultV1
reconcile_distributed_sieve_worker_cleanup_completion_v1(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;

namespace trusted_test {

[[nodiscard]] DistributedSieveWorkerCleanupCompletionPublicationResultV1
publish_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
    DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&& completion_ready,
    DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1 hooks) noexcept;

[[nodiscard]] DistributedSieveWorkerCleanupCompletionPublicationResultV1
reconcile_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
    DistributedSieveWorkerCleanupRootAdmissionV1&& root,
    DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1 hooks) noexcept;

} // namespace trusted_test

class DistributedSieveWorkerCleanupCompletionPublicationAuthorityV1 final {
public:
    DistributedSieveWorkerCleanupCompletionPublicationAuthorityV1() = delete;

private:
    [[nodiscard]] static bool
    valid(const DistributedSieveWorkerCleanupCompletionPublishedContinuationV1&
              continuation) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupCompletionPublicationResultV1 publish(
        DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&& completion_ready,
        trusted_test::DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1 hooks) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupCompletionPublicationResultV1 reconcile(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        trusted_test::DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1 hooks) noexcept;
    [[nodiscard]] static DistributedSieveWorkerCleanupCompletionPublicationResultV1 drive(
        std::optional<DistributedSieveWorkerCleanupCompletionReadyCapsuleV1>&& fresh,
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1>&& recovery,
        trusted_test::DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1 hooks) noexcept;

    friend class DistributedSieveWorkerCleanupCompletionPublishedContinuationV1;
    friend DistributedSieveWorkerCleanupCompletionPublicationResultV1
    publish_distributed_sieve_worker_cleanup_completion_v1(
        DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&& completion_ready) noexcept;
    friend DistributedSieveWorkerCleanupCompletionPublicationResultV1
    reconcile_distributed_sieve_worker_cleanup_completion_v1(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root) noexcept;
    friend DistributedSieveWorkerCleanupCompletionPublicationResultV1
    trusted_test::publish_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
        DistributedSieveWorkerCleanupCompletionReadyCapsuleV1&& completion_ready,
        trusted_test::DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1 hooks) noexcept;
    friend DistributedSieveWorkerCleanupCompletionPublicationResultV1
    trusted_test::reconcile_distributed_sieve_worker_cleanup_completion_v1_with_hooks(
        DistributedSieveWorkerCleanupRootAdmissionV1&& root,
        trusted_test::DistributedSieveWorkerCleanupCompletionPublicationTestHooksV1 hooks) noexcept;
};

/// Pure-data result of the committed tail's rvalue-only lock-generation
/// release. No descriptor, path mutation, publication, or cleanup capability
/// is retained here.
struct DistributedSieveCommittedTailCleanupTransitionV1 final {
    DistributedSieveCommittedTailCleanupTransitionV1() = default;
    DistributedSieveCommittedTailCleanupTransitionV1(
        const DistributedSieveCommittedTailCleanupTransitionV1&) = delete;
    DistributedSieveCommittedTailCleanupTransitionV1&
    operator=(const DistributedSieveCommittedTailCleanupTransitionV1&) = delete;
    DistributedSieveCommittedTailCleanupTransitionV1(
        DistributedSieveCommittedTailCleanupTransitionV1&&) noexcept = default;
    DistributedSieveCommittedTailCleanupTransitionV1&
    operator=(DistributedSieveCommittedTailCleanupTransitionV1&&) noexcept = default;

    std::optional<std::filesystem::path> absolute_root;
    std::optional<distributed_sieve_resume_detail::DistributedSieveWorkerCleanupRootExactAnchorV1>
        exact_anchor;
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
    std::error_code native_error;
    bool admission_validated = false;
    bool retryable_tail_retained = false;
    bool tail_spent = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return absolute_root.has_value() && exact_anchor.has_value() && !native_error &&
               admission_validated && !retryable_tail_retained && tail_spent &&
               wave_store.status ==
                   distributed_sieve_resume_detail::DistributedSieveWaveStoreStatus::ready;
    }
};

enum class DistributedSieveWorkerCleanupTailPhaseV1 : std::uint8_t {
    admission_validation,
    root_snapshot,
    old_epoch_release,
    cleanup_root_open,
    complete,
};

enum class DistributedSieveWorkerCleanupTailStatusV1 : std::uint8_t {
    ready,
    invalid_admission,
    process_mismatch,
    root_snapshot_failed,
    platform_unsupported,
    cleanup_root_open_failed,
    resource_exhausted,
    unexpected_failure,
};

[[nodiscard]] constexpr std::string_view distributed_sieve_worker_cleanup_tail_status_name(
    DistributedSieveWorkerCleanupTailStatusV1 status) noexcept {
    switch (status) {
    case DistributedSieveWorkerCleanupTailStatusV1::ready:
        return "ready";
    case DistributedSieveWorkerCleanupTailStatusV1::invalid_admission:
        return "invalid_admission";
    case DistributedSieveWorkerCleanupTailStatusV1::process_mismatch:
        return "process_mismatch";
    case DistributedSieveWorkerCleanupTailStatusV1::root_snapshot_failed:
        return "root_snapshot_failed";
    case DistributedSieveWorkerCleanupTailStatusV1::platform_unsupported:
        return "platform_unsupported";
    case DistributedSieveWorkerCleanupTailStatusV1::cleanup_root_open_failed:
        return "cleanup_root_open_failed";
    case DistributedSieveWorkerCleanupTailStatusV1::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveWorkerCleanupTailStatusV1::unexpected_failure:
        return "unexpected_failure";
    }
    return "unknown";
}

[[nodiscard]] constexpr DistributedSieveWorkerCleanupTailStatusV1
distributed_sieve_worker_cleanup_tail_root_snapshot_status(
    distributed_sieve_resume_detail::DistributedSieveWaveStoreStatus status) noexcept {
    switch (status) {
    case distributed_sieve_resume_detail::DistributedSieveWaveStoreStatus::platform_unsupported:
        return DistributedSieveWorkerCleanupTailStatusV1::platform_unsupported;
    case distributed_sieve_resume_detail::DistributedSieveWaveStoreStatus::resource_exhausted:
        return DistributedSieveWorkerCleanupTailStatusV1::resource_exhausted;
    case distributed_sieve_resume_detail::DistributedSieveWaveStoreStatus::unexpected_failure:
        return DistributedSieveWorkerCleanupTailStatusV1::unexpected_failure;
    default:
        return DistributedSieveWorkerCleanupTailStatusV1::root_snapshot_failed;
    }
}

struct DistributedSieveWorkerCleanupTailDiagnosticV1 final {
    DistributedSieveWorkerCleanupTailPhaseV1 phase =
        DistributedSieveWorkerCleanupTailPhaseV1::admission_validation;
    DistributedSieveWorkerCleanupTailStatusV1 status =
        DistributedSieveWorkerCleanupTailStatusV1::unexpected_failure;
    distributed_sieve_resume_detail::DistributedSieveWaveStoreDiagnostic wave_store;
    std::error_code native_error;
    bool tail_spent = false;
    bool cold_reopen_required = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return phase == DistributedSieveWorkerCleanupTailPhaseV1::complete &&
               status == DistributedSieveWorkerCleanupTailStatusV1::ready && tail_spent &&
               !cold_reopen_required && !native_error &&
               wave_store.status ==
                   distributed_sieve_resume_detail::DistributedSieveWaveStoreStatus::ready;
    }
};

/// Closed result of crossing the committed-tail lock-generation boundary.
///
/// A failure before the old origin is released may return `retryable_tail`.
/// Once `tail_spent` is true, neither the old tail nor a partial cleanup-root
/// admission is returned; the caller must cold-open the root again.
struct DistributedSieveWorkerCleanupTailResultV1 final {
    DistributedSieveWorkerCleanupTailResultV1() = default;
    DistributedSieveWorkerCleanupTailResultV1(
        std::optional<DistributedSieveCommittedTailAdmissionV1> retryable_tail_value,
        std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> admission_value,
        DistributedSieveWorkerCleanupTailDiagnosticV1 diagnostic_value) noexcept
        : retryable_tail(std::move(retryable_tail_value)), admission(std::move(admission_value)),
          diagnostic(std::move(diagnostic_value)) {}
    DistributedSieveWorkerCleanupTailResultV1(const DistributedSieveWorkerCleanupTailResultV1&) =
        delete;
    DistributedSieveWorkerCleanupTailResultV1&
    operator=(const DistributedSieveWorkerCleanupTailResultV1&) = delete;
    DistributedSieveWorkerCleanupTailResultV1(
        DistributedSieveWorkerCleanupTailResultV1&&) noexcept = default;
    DistributedSieveWorkerCleanupTailResultV1&
    operator=(DistributedSieveWorkerCleanupTailResultV1&&) = delete;

    std::optional<DistributedSieveCommittedTailAdmissionV1> retryable_tail;
    std::optional<DistributedSieveWorkerCleanupRootAdmissionV1> admission;
    DistributedSieveWorkerCleanupTailDiagnosticV1 diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(diagnostic) && !retryable_tail.has_value() &&
               admission.has_value() && admission->valid();
    }
};

namespace trusted_test {

/// Test-only observation boundary after the old origin is irreversibly
/// released and before the cleanup-root cold open starts.
struct DistributedSieveWorkerCleanupTailTestHooksV1 final {
    /// Runs after the exact anchor is frozen but before the tail's final
    /// validity check. Returning true injects a pre-spend failure; returning
    /// false continues through the ordinary final revalidation.
    using BeforeFinalTailRevalidation = bool (*)(void* context) noexcept;
    using AfterOldEpochRelease = void (*)(void* context) noexcept;

    BeforeFinalTailRevalidation before_final_tail_revalidation = nullptr;
    AfterOldEpochRelease after_old_epoch_release = nullptr;
    void* context = nullptr;
};

[[nodiscard]] DistributedSieveWorkerCleanupTailResultV1
consume_distributed_sieve_committed_tail_for_worker_cleanup_v1_with_hooks(
    DistributedSieveCommittedTailAdmissionV1&& tail,
    DistributedSieveWorkerCleanupTailTestHooksV1 hooks) noexcept;

} // namespace trusted_test

[[nodiscard]] DistributedSieveWorkerCleanupTailResultV1
consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
    DistributedSieveCommittedTailAdmissionV1&& tail) noexcept;

class DistributedSieveWorkerCleanupTailAuthorityV1 final {
public:
    DistributedSieveWorkerCleanupTailAuthorityV1() = delete;

private:
    [[nodiscard]] static DistributedSieveWorkerCleanupTailResultV1
    consume(DistributedSieveCommittedTailAdmissionV1&& tail,
            trusted_test::DistributedSieveWorkerCleanupTailTestHooksV1 hooks) noexcept;

    friend DistributedSieveWorkerCleanupTailResultV1
    consume_distributed_sieve_committed_tail_for_worker_cleanup_v1(
        DistributedSieveCommittedTailAdmissionV1&& tail) noexcept;
    friend DistributedSieveWorkerCleanupTailResultV1
    trusted_test::consume_distributed_sieve_committed_tail_for_worker_cleanup_v1_with_hooks(
        DistributedSieveCommittedTailAdmissionV1&& tail,
        trusted_test::DistributedSieveWorkerCleanupTailTestHooksV1 hooks) noexcept;
};

} // namespace gnfs::sieve::distributed_sieve_worker_cleanup_authority_detail
