#include "shadow_proof_rss_campaign_slot_runner_internal.hpp"

#include "shadow_proof_rss_holdout_stream_join_internal.hpp"
#include "shadow_proof_rss_probe_execution_identity_internal.hpp"

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

namespace join_support = gnfs::siqs::shadow_proof_rss_holdout_detail;

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
        (executable.probe_kind != SIQSShadowProofRssProbeKind::synthetic_test &&
         executable.probe_kind != SIQSShadowProofRssProbeKind::production_holdout) ||
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

[[nodiscard]] bool
execution_identity_matches_context(const SessionSlotRunContext& context) noexcept {
    if (context.policy == nullptr || context.runtime_facts == nullptr ||
        context.executable == nullptr) {
        return false;
    }
    const auto& identity = context.executable->probe_execution_identity;
    if (!siqs_shadow_proof_rss_probe_execution_identity_is_valid(identity) ||
        identity != context.policy->probe_execution_identity ||
        identity != context.runtime_facts->probe_execution_identity ||
        identity != context.slot.probe_execution_identity) {
        return false;
    }

    using shadow_proof_rss_probe_execution_identity_detail::
        make_siqs_shadow_proof_rss_probe_execution_identity;
    using shadow_proof_rss_probe_execution_identity_detail::ProbeExecutionContractInput;
    const auto canonical_identity =
        make_siqs_shadow_proof_rss_probe_execution_identity(ProbeExecutionContractInput{
            .executable_sha256 = identity.executable_sha256,
            .probe_kind = context.deployment_probe_kind,
            .launch_profile = context.executable->launch_profile,
            .candidate_revision = context.executable->candidate_revision,
            .operating_system = context.runtime_facts->operating_system,
            .architecture = context.runtime_facts->architecture,
            .memory_backend = context.runtime_facts->memory_backend,
            .resolved_production_sieve_workers =
                context.runtime_facts->resolved_production_sieve_workers,
            .release_build = context.runtime_facts->release_build,
            .ndebug = context.runtime_facts->ndebug,
            .environment = context.executable->environment,
            .timeout_ms = static_cast<std::uint64_t>(context.executable->timeout.count()),
            .expected_owner = context.executable->expected_owner,
        });
    return canonical_identity.has_value() && *canonical_identity == identity;
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
                        join_support::SIQSShadowProofRssUncommittedSampleDraft&& draft,
                        bool same_object_authenticated) {
    SameChildExecutionEvidence evidence;
    evidence.durable_start_record = durable_start_record;
    evidence.policy_binding_digest = draft.policy_binding_digest;
    evidence.slot = slot;
    evidence.operating_system = draft.operating_system;
    evidence.architecture = draft.architecture;
    evidence.memory_backend = draft.memory_backend;
    evidence.resolved_production_sieve_workers = draft.resolved_production_sieve_workers;
    evidence.deployment_probe_kind = draft.probe_kind;
    evidence.probe_execution_identity = draft.probe_execution_identity;
    evidence.same_object_authenticated = same_object_authenticated;
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
    : view_(view), diagnostic_(std::move(diagnostic)) {
    if (diagnostic_.primary_error == SlotRunnerError::none) {
        diagnostic_.primary_error = diagnostic_.error;
    }
}

SlotRunnerResult::SlotRunnerResult(SIQSShadowProofRssCampaignJournalSession&& session) noexcept
    : session_(std::move(session)), view_(session_->view()) {}

SlotRunnerResult::~SlotRunnerResult() = default;

SlotRunnerResult::SlotRunnerResult(SlotRunnerResult&& other) noexcept
    : session_(std::move(other.session_)), view_(other.view_),
      diagnostic_(std::move(other.diagnostic_)) {
    other.session_.reset();
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
    if (diagnostic.primary_error == SlotRunnerError::none) {
        diagnostic.primary_error = diagnostic.error;
    }
    diagnostic.taint_attempted = active_slot.core_ != nullptr;
    if (active_slot.core_ == nullptr) {
        diagnostic.error = SlotRunnerError::taint_failed;
        diagnostic.closure_error = SlotRunnerError::taint_failed;
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
        diagnostic.closure_error = SlotRunnerError::taint_failed;
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
        if (!execution_identity_matches_context(context)) {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::deployment_invalid;
            diagnostic.store_diagnostic = make_store_diagnostic(
                StoreError::registry_binding_mismatch, StoreObject::deployment_registry);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }

        using LaunchProfile =
            shadow_proof_rss_probe_execution_identity_detail::ProbeExecutableLaunchProfile;
        util::BoundedChildProcessResult transport;
        bool same_object_authenticated = false;
        util::BoundedChildProcessSpec process_spec = make_process_spec(context);
        switch (context.executable->launch_profile) {
        case LaunchProfile::linux_sealed_memfd_execveat_v1:
#if defined(__linux__)
        {
            auto authenticated = util::authenticate_executable_image(
                context.executable->executable,
                context.executable->probe_execution_identity.executable_sha256,
                context.executable->expected_owner);
            if (!authenticated) {
                SlotRunnerDiagnostic diagnostic;
                diagnostic.authentication = std::move(authenticated.diagnostic);
                switch (diagnostic.authentication.error) {
                case util::ExecutableImageAuthenticationError::platform_unavailable:
                    diagnostic.error = SlotRunnerError::platform_unavailable;
                    diagnostic.store_diagnostic = make_store_diagnostic(
                        StoreError::platform_unavailable, StoreObject::probe_executable);
                    break;
                case util::ExecutableImageAuthenticationError::resource_failure:
                    diagnostic.error = SlotRunnerError::resource_exhausted;
                    diagnostic.store_diagnostic = make_store_diagnostic(
                        StoreError::resource_exhausted, StoreObject::probe_executable);
                    break;
                case util::ExecutableImageAuthenticationError::unexpected_failure:
                    diagnostic.error = SlotRunnerError::unexpected_failure;
                    diagnostic.store_diagnostic = make_store_diagnostic(
                        StoreError::unexpected_failure, StoreObject::probe_executable);
                    break;
                default:
                    diagnostic.error = SlotRunnerError::executable_authentication_failed;
                    diagnostic.store_diagnostic =
                        make_store_diagnostic(StoreError::executable_authentication_failed,
                                              StoreObject::probe_executable);
                    break;
                }
                diagnostic.store_diagnostic.native_error = diagnostic.authentication.native_error;
                return finish_with_taint(std::move(active_slot), std::move(diagnostic));
            }
            // The approved timeout is the child launch/capture budget. Synchronous
            // executable authentication has a separate size bound and completes
            // before this clock starts.
            process_spec.deadline = std::chrono::steady_clock::now() + context.executable->timeout;
            transport = util::run_authenticated_bounded_child_process(
                std::move(*authenticated.image), process_spec,
                shadow_proof_rss_probe_execution_identity_detail::
                    SIQS_SHADOW_PROOF_RSS_PROBE_ARGV0);
            same_object_authenticated = true;
            break;
        }
#else
        {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::platform_unavailable;
            diagnostic.authentication.error =
                util::ExecutableImageAuthenticationError::platform_unavailable;
            diagnostic.store_diagnostic = make_store_diagnostic(StoreError::platform_unavailable,
                                                                StoreObject::probe_executable);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }
#endif
        case LaunchProfile::synthetic_path_spawn_v1:
            if (context.deployment_probe_kind != SIQSShadowProofRssProbeKind::synthetic_test ||
                !executable_binding_is_valid(*context.executable)) {
                SlotRunnerDiagnostic diagnostic;
                diagnostic.error = SlotRunnerError::deployment_invalid;
                diagnostic.store_diagnostic = make_store_diagnostic(
                    StoreError::registry_binding_mismatch, StoreObject::deployment_registry);
                return finish_with_taint(std::move(active_slot), std::move(diagnostic));
            }
#if !defined(_WIN32)
            transport = util::detail::run_bounded_child_process_with_argv0(
                process_spec, shadow_proof_rss_probe_execution_identity_detail::
                                  SIQS_SHADOW_PROOF_RSS_PROBE_ARGV0);
            break;
#else
            {
                SlotRunnerDiagnostic diagnostic;
                diagnostic.error = SlotRunnerError::platform_unavailable;
                diagnostic.store_diagnostic = make_store_diagnostic(
                    StoreError::platform_unavailable, StoreObject::probe_executable);
                return finish_with_taint(std::move(active_slot), std::move(diagnostic));
            }
#endif
        case LaunchProfile::darwin_hardened_suspended_v1: {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::platform_unavailable;
            diagnostic.authentication.error =
                util::ExecutableImageAuthenticationError::platform_unavailable;
            diagnostic.store_diagnostic = make_store_diagnostic(StoreError::platform_unavailable,
                                                                StoreObject::probe_executable);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }
        case LaunchProfile::unknown: {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::deployment_invalid;
            diagnostic.store_diagnostic = make_store_diagnostic(
                StoreError::registry_binding_mismatch, StoreObject::deployment_registry);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }
        }
        if (!transport.succeeded()) {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error =
                transport.error == util::BoundedChildProcessError::platform_unavailable
                    ? SlotRunnerError::platform_unavailable
                    : SlotRunnerError::transport_failed;
            retain_transport_diagnostic(diagnostic, transport);
            if (diagnostic.error == SlotRunnerError::platform_unavailable) {
                diagnostic.store_diagnostic = make_store_diagnostic(
                    StoreError::platform_unavailable, StoreObject::probe_executable);
                diagnostic.store_diagnostic.native_error = transport.native_error;
            }
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }

        SIQSShadowProofRssCampaignRuntimeFacts effective_runtime_facts = *context.runtime_facts;
        effective_runtime_facts.probe_kind = context.deployment_probe_kind;
        auto joined = join_support::join_siqs_shadow_proof_rss_holdout_streams(
            context.policy, &effective_runtime_facts, &context.slot, transport.stdout_bytes,
            transport.stderr_bytes);
        if (!joined) {
            SlotRunnerDiagnostic diagnostic;
            diagnostic.error = SlotRunnerError::stream_join_failed;
            retain_transport_diagnostic(diagnostic, transport);
            diagnostic.stream_join_error = static_cast<uint8_t>(joined.error);
            return finish_with_taint(std::move(active_slot), std::move(diagnostic));
        }

        SameChildExecutionReceipt execution_receipt(
            make_execution_evidence(durable_start_record, context.slot, std::move(*joined.draft),
                                    same_object_authenticated));
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
            // Publication may already have committed. Withhold the pre-commit
            // replay view so no caller can treat stale state as authoritative.
            return SlotRunnerResult(std::move(diagnostic));
        }
        diagnostic.error = SlotRunnerError::commit_failed;
        return finish_with_taint(std::move(active_slot), std::move(diagnostic));
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
