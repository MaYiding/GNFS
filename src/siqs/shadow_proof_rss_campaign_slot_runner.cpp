#include "shadow_proof_rss_campaign_slot_runner_internal.hpp"

#include "support/siqs_shadow_proof_rss_holdout_stream_join.hpp"

#include <gnfs/siqs/shadow_proof_rss_campaign_artifact_layout.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail {
namespace {

namespace join_support = gnfs::tests::support;

using StoreError = SIQSShadowProofRssCampaignJournalStoreError;
using StoreObject = SIQSShadowProofRssCampaignJournalStoreObject;

constexpr auto MAX_RUNNER_TIMEOUT = std::chrono::seconds(60);

[[nodiscard]] SIQSShadowProofRssCampaignJournalStoreDiagnostic
make_store_diagnostic(StoreError error, StoreObject object = StoreObject::none) noexcept {
    SIQSShadowProofRssCampaignJournalStoreDiagnostic diagnostic;
    diagnostic.error = error;
    diagnostic.object = object;
    return diagnostic;
}

void retain_transport_diagnostic(SlotRunnerDiagnostic& diagnostic,
                                 const util::BoundedChildProcessResult& transport) noexcept {
    diagnostic.transport_error = transport.error;
    diagnostic.termination = transport.termination;
    diagnostic.stdout_byte_count = transport.stdout_bytes.size();
    diagnostic.stderr_byte_count = transport.stderr_bytes.size();
    diagnostic.child_started = transport.child_started;
    diagnostic.stdout_eof = transport.stdout_eof;
    diagnostic.stderr_eof = transport.stderr_eof;
    diagnostic.cleanup_complete = transport.cleanup_complete;
}

[[nodiscard]] bool executable_binding_is_valid(const ProbeExecutableBinding& executable) noexcept {
    if (executable.executable.empty() || !executable.executable.is_absolute() ||
        executable.candidate_revision.empty() ||
        executable.timeout <= std::chrono::milliseconds::zero() ||
        executable.timeout > MAX_RUNNER_TIMEOUT) {
        return false;
    }
#if defined(_WIN32)
    return false;
#else
    struct stat metadata{};
    return ::lstat(executable.executable.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode) &&
           metadata.st_nlink == 1 &&
           static_cast<uint64_t>(metadata.st_uid) == executable.expected_owner &&
           (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
#endif
}

[[nodiscard]] util::BoundedChildProcessSpec
make_process_spec(const SessionSlotRunContext& context) {
    util::BoundedChildProcessSpec spec;
    spec.executable = context.executable->executable;
    spec.arguments = {
        "--fixture-id", std::to_string(context.slot.fixture_id),
        "--mode",       std::string(siqs_shadow_proof_rss_sample_mode_name(context.slot.mode)),
        "--ordinal",    std::to_string(context.slot.ordinal),
    };
    spec.environment = context.executable->environment;
    spec.deadline = std::chrono::steady_clock::now() + context.executable->timeout;
    spec.stdout_limit = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_MAX_BYTES;
    spec.stderr_limit = context.slot.mode == SIQSShadowProofRssSampleMode::off
                            ? 0
                            : SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_MAX_BYTES;
    return spec;
}

[[nodiscard]] SameChildExecutionEvidence
make_execution_evidence(const SIQSShadowProofRssCampaignJournalRecord& durable_start_record,
                        const SIQSShadowProofRssCampaignSlot& slot,
                        join_support::SIQSShadowProofRssUncommittedSampleDraft&& draft) {
    SameChildExecutionEvidence evidence;
    evidence.durable_start_record = durable_start_record;
    evidence.policy_binding_digest = draft.policy_binding_digest;
    evidence.slot = slot;
    evidence.operating_system = draft.operating_system;
    evidence.architecture = draft.architecture;
    evidence.memory_backend = draft.memory_backend;
    evidence.resolved_production_sieve_workers = draft.resolved_production_sieve_workers;
    evidence.fresh_process = draft.fresh_process;
    evidence.completed = draft.completed;
    evidence.factor_identity = draft.factor_identity;
    evidence.proof_evidence = draft.proof_evidence;
    evidence.matrix_evidence = draft.matrix_evidence;
    evidence.relations_found = draft.relations_found;
    evidence.polynomials_used = draft.polynomials_used;
    evidence.absolute_peak_rss_bytes = draft.absolute_peak_rss_bytes;
    evidence.current_rss_bytes = draft.current_rss_bytes;
    evidence.peak_growth_bytes = draft.peak_growth_bytes;
    evidence.wall_ns = draft.wall_ns;
    evidence.stdout_bytes = std::move(draft.stdout_bytes);
    evidence.stderr_bytes = std::move(draft.stderr_bytes);
    evidence.joined_bytes = std::move(draft.joined_bytes);
    return evidence;
}

} // namespace

SlotRunnerResult::SlotRunnerResult(SlotRunnerDiagnostic diagnostic,
                                   SIQSShadowProofRssCampaignJournalSessionView view) noexcept
    : view_(view), diagnostic_(std::move(diagnostic)) {}

SlotRunnerResult::SlotRunnerResult(SIQSShadowProofRssCampaignJournalSession&& session) noexcept
    : session_(std::move(session)), view_(session_->view()) {}

SlotRunnerResult::~SlotRunnerResult() = default;

SlotRunnerResult::SlotRunnerResult(SlotRunnerResult&& other) noexcept
    : session_(std::move(other.session_)), view_(other.view_),
      diagnostic_(std::move(other.diagnostic_)) {
    other.session_.reset();
}

SlotRunnerResult& SlotRunnerResult::operator=(SlotRunnerResult&& other) noexcept {
    if (this != &other) {
        session_ = std::move(other.session_);
        view_ = other.view_;
        diagnostic_ = std::move(other.diagnostic_);
        other.session_.reset();
    }
    return *this;
}

SlotRunnerResult::operator bool() const noexcept {
    return session_.has_value() && session_->active() && diagnostic_.error == SlotRunnerError::none;
}

const SlotRunnerDiagnostic& SlotRunnerResult::diagnostic() const noexcept {
    return diagnostic_;
}

SIQSShadowProofRssCampaignJournalSessionView SlotRunnerResult::view() const noexcept {
    return view_;
}

std::optional<SIQSShadowProofRssCampaignJournalSession>
SlotRunnerResult::take_session() && noexcept {
    auto session = std::move(session_);
    session_.reset();
    return session;
}

SlotRunnerResult
SlotRunnerFactory::finish_with_taint(SIQSShadowProofRssCampaignJournalActiveSlot&& active_slot,
                                     SlotRunnerDiagnostic diagnostic) noexcept {
    diagnostic.taint_attempted = active_slot.core_ != nullptr;
    if (active_slot.core_ == nullptr) {
        diagnostic.error = SlotRunnerError::taint_failed;
        diagnostic.taint_diagnostic = make_store_diagnostic(StoreError::session_inactive);
        return SlotRunnerResult(std::move(diagnostic));
    }

    active_slot.permit_.reset();
    auto core = std::move(active_slot.core_);
    SessionTaintResult taint = core->append_pending_taint();
    diagnostic.taint_diagnostic = taint.diagnostic;
    diagnostic.taint_durable = static_cast<bool>(taint);
    if (!diagnostic.taint_durable) {
        diagnostic.error = SlotRunnerError::taint_failed;
    }
    return SlotRunnerResult(std::move(diagnostic), taint.view);
}

SlotRunnerResult
SlotRunnerFactory::run(SIQSShadowProofRssCampaignJournalActiveSlot&& active_slot) noexcept {
    try {
        if (!active_slot.active() || active_slot.core_ == nullptr ||
            !active_slot.permit_.has_value()) {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::session_inactive;
            diagnostic.store_diagnostic = make_store_diagnostic(StoreError::session_inactive);
            return SlotRunnerResult(std::move(diagnostic));
        }

        const auto durable_start_record = active_slot.permit_->durable_start_record();
        SessionPrepareRunResult prepared =
            active_slot.core_->prepare_pending_slot_run(durable_start_record);
        if (!prepared) {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = prepared.diagnostic.error == StoreError::binding_not_registered
                                   ? SlotRunnerError::deployment_unavailable
                                   : SlotRunnerError::deployment_invalid;
            diagnostic.store_diagnostic = std::move(prepared.diagnostic);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }
        const SessionSlotRunContext& context = *prepared.context;
#if defined(_WIN32)
        SlotRunnerDiagnostic diagnostic;
        diagnostic.error = SlotRunnerError::platform_unavailable;
        diagnostic.store_diagnostic = make_store_diagnostic(StoreError::platform_unavailable);
        return finish_with_taint(std::move(active_slot), std::move(diagnostic));
#else
        if (!executable_binding_is_valid(*context.executable)) {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::deployment_invalid;
            diagnostic.store_diagnostic = make_store_diagnostic(
                StoreError::registry_binding_mismatch, StoreObject::deployment_registry);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }

        const util::BoundedChildProcessResult transport =
            util::run_bounded_child_process(make_process_spec(context));
        if (!transport.succeeded()) {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::transport_failed;
            retain_transport_diagnostic(diagnostic, transport);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }

        auto joined = join_support::join_siqs_shadow_proof_rss_holdout_streams(
            context.policy, context.runtime_facts, &context.slot, transport.stdout_bytes,
            transport.stderr_bytes);
        if (!joined) {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::stream_join_failed;
            retain_transport_diagnostic(diagnostic, transport);
            diagnostic.stream_join_error = static_cast<uint8_t>(joined.error);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }

        SameChildExecutionReceipt execution_receipt(
            make_execution_evidence(durable_start_record, context.slot, std::move(*joined.draft)));
        const auto& evidence = execution_receipt.evidence_;
        SessionArtifactBatchResult artifacts =
            active_slot.core_->publish_artifact_batch(durable_start_record, evidence.stdout_bytes,
                                                      evidence.stderr_bytes, evidence.joined_bytes);
        if (!artifacts) {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::artifact_publication_failed;
            retain_transport_diagnostic(diagnostic, transport);
            diagnostic.store_diagnostic = std::move(artifacts.diagnostic);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }

        SessionCommitResult committed = active_slot.core_->commit_same_child_execution(
            std::move(*active_slot.permit_), std::move(execution_receipt));
        active_slot.permit_.reset();
        if (committed.status == SessionCommitStatus::committed && static_cast<bool>(committed)) {
            auto core = std::move(active_slot.core_);
            return SlotRunnerResult(SIQSShadowProofRssCampaignJournalSession(std::move(core)));
        }

        SlotRunnerDiagnostic diagnostic;
        retain_transport_diagnostic(diagnostic, transport);
        diagnostic.store_diagnostic = std::move(committed.diagnostic);
        if (committed.status == SessionCommitStatus::outcome_uncertain) {
            diagnostic.error = SlotRunnerError::commit_outcome_uncertain;
            active_slot.core_.reset();
            return SlotRunnerResult(std::move(diagnostic), committed.view);
        }
        diagnostic.error = SlotRunnerError::commit_failed;
        return finish_with_taint(std::move(active_slot), std::move(diagnostic));
#endif
    } catch (const std::bad_alloc&) {
        SlotRunnerDiagnostic diagnostic;
        diagnostic.error = SlotRunnerError::resource_exhausted;
        return finish_with_taint(std::move(active_slot), std::move(diagnostic));
    } catch (...) {
        SlotRunnerDiagnostic diagnostic;
        diagnostic.error = SlotRunnerError::unexpected_failure;
        return finish_with_taint(std::move(active_slot), std::move(diagnostic));
    }
}

} // namespace gnfs::siqs::shadow_proof_rss_campaign_journal_store_detail
