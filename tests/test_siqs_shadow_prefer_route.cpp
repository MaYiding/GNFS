// test_siqs_shadow_prefer_route.cpp - production prefer-route composition

#include "support/scoped_environment_stderr.hpp"

#include <gnfs/siqs/siqs.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using gnfs::core::Integer;
using gnfs::siqs::evaluate_siqs_shadow_proof_prefer;
using gnfs::siqs::finalize_siqs_shadow_proof_prefer;
using gnfs::siqs::siqs_factor_detail::commit_prefer_route;
using gnfs::siqs::siqs_factor_detail::prefer_shadow_return_authorized;
using gnfs::siqs::SIQSPostMergeDependencyStatus;
using gnfs::siqs::SIQSPostMergeFactorization;
using gnfs::siqs::SIQSPostMergeFactorStatus;
using gnfs::siqs::SIQSResult;
using gnfs::siqs::SIQSShadowAssemblyStatus;
using gnfs::siqs::SIQSShadowMatrixStatus;
using gnfs::siqs::SIQSShadowProofEvidence;
using gnfs::siqs::SIQSShadowProofFallbackReason;
using gnfs::siqs::SIQSShadowProofMode;
using gnfs::siqs::SIQSShadowProofPreferDecision;
using gnfs::siqs::SIQSShadowProofPreferSourceView;
using gnfs::siqs::SIQSShadowProofStage;
using gnfs::siqs::SIQSShadowProofTerminalStatus;
using gnfs::tests::support::ScopedEnvironmentVariable;
using gnfs::tests::support::ScopedStderrCapture;
using gnfs::tests::support::ScopedUnwritableStderr;

void require_test(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

[[nodiscard]] SIQSShadowProofEvidence make_success_evidence() {
    SIQSShadowProofEvidence evidence{};
    evidence.assembly_status = SIQSShadowAssemblyStatus::valid;
    evidence.assembly.pretrim_rows = 3;
    evidence.assembly.selected_rows = 3;
    evidence.assembly.selected_full_rows = 2;
    evidence.assembly.selected_cycle_rows = 1;
    evidence.matrix_status = SIQSShadowMatrixStatus::valid;
    evidence.matrix_rows = 3;
    evidence.matrix_columns = 2;
    evidence.minimum_nullity = 1;
    evidence.dependencies_returned = 2;
    evidence.dependencies_examined = 2;
    evidence.dependencies_verified = 2;
    evidence.no_factor_count = 1;
    evidence.factor_found_count = 1;
    evidence.dependency_cap_reached = false;
    evidence.winning_dependency = 1;
    evidence.winning_dependency_size = 1;
    evidence.dependency_status = SIQSPostMergeDependencyStatus::valid;
    evidence.factor_status = SIQSPostMergeFactorStatus::factor_found;
    return evidence;
}

[[nodiscard]] SIQSShadowProofPreferSourceView
make_source(const SIQSShadowProofEvidence* evidence,
            const SIQSPostMergeFactorization* factorization,
            SIQSShadowProofTerminalStatus terminal = SIQSShadowProofTerminalStatus::factor_found,
            SIQSShadowProofStage stage = SIQSShadowProofStage::factor_extraction,
            SIQSShadowProofFallbackReason fallback = SIQSShadowProofFallbackReason::none) {
    return SIQSShadowProofPreferSourceView{terminal, stage, fallback, evidence, factorization};
}

[[nodiscard]] SIQSShadowProofPreferDecision make_candidate_decision() {
    SIQSShadowProofEvidence evidence = make_success_evidence();
    const SIQSPostMergeFactorization factors{Integer(7), Integer(13)};
    auto draft =
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, &factors), Integer(91), 17);
    return finalize_siqs_shadow_proof_prefer(std::move(draft), 2'500'000'000ULL);
}

[[nodiscard]] SIQSShadowProofPreferDecision make_no_factor_decision() {
    SIQSShadowProofEvidence evidence = make_success_evidence();
    auto draft = evaluate_siqs_shadow_proof_prefer(
        make_source(&evidence, nullptr, SIQSShadowProofTerminalStatus::no_factor,
                    SIQSShadowProofStage::complete),
        Integer(91), 17);
    return finalize_siqs_shadow_proof_prefer(std::move(draft), 2'500'000'000ULL);
}

[[nodiscard]] SIQSShadowProofPreferDecision make_zero_wall_decision() {
    SIQSShadowProofEvidence evidence = make_success_evidence();
    const SIQSPostMergeFactorization factors{Integer(7), Integer(13)};
    auto draft =
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, &factors), Integer(91), 17);
    return finalize_siqs_shadow_proof_prefer(std::move(draft), 0);
}

[[nodiscard]] SIQSResult make_prepared_result() {
    SIQSResult result;
    result.factor1 = Integer(7);
    result.factor2 = Integer(13);
    result.time_seconds = 0.0;
    result.relations_found = 3;
    result.polynomials_used = 17;
    result.resolved_sieve_workers = 2;
    result.shadow_proof_observe_record_committed = false;
    return result;
}

[[nodiscard]] std::pair<std::optional<SIQSResult>, std::string>
commit_with_capture(const SIQSShadowProofPreferDecision& decision,
                    std::optional<SIQSResult> prepared_result) {
    ScopedStderrCapture capture;
    auto routed =
        commit_prefer_route(stderr, Integer(91), decision, std::move(prepared_result));
    return {std::move(routed), capture.finish()};
}

void test_route_authority_truth_table() {
    for (unsigned mask = 0; mask < 8; ++mask) {
        const bool decision_is_shadow_candidate = (mask & 1U) != 0;
        const bool prepared_candidate_matches = (mask & 2U) != 0;
        const bool prefer_decision_committed = (mask & 4U) != 0;
        const bool expected =
            decision_is_shadow_candidate && prepared_candidate_matches && prefer_decision_committed;
        require_test(prefer_shadow_return_authorized(decision_is_shadow_candidate,
                                                     prepared_candidate_matches,
                                                     prefer_decision_committed) == expected,
                     "prefer route authority admitted a fail-open combination");
    }
}

void test_candidate_commit_and_emit_failure() {
    const auto candidate = make_candidate_decision();
    const auto [routed, output] = commit_with_capture(candidate, make_prepared_result());
    require_test(routed.has_value(), "committed candidate did not return the prepared result");
    require_test(routed->factor1 == Integer(7) && routed->factor2 == Integer(13),
                 "committed candidate returned different factors");
    require_test(routed->time_seconds == 2.5 && routed->relations_found == 3 &&
                     routed->polynomials_used == 17 && routed->resolved_sieve_workers == 2 &&
                     !routed->shadow_proof_observe_record_committed,
                 "committed candidate returned different production metadata");
    require_test(
        output ==
            "GNFS_SIQS_SHADOW_PROOF_PREFER_DECISION_V2 schema_version=2 status=valid mode=prefer"
            " decision=shadow_candidate reason=shadow_factor_valid next_route=shadow_return"
            " shadow_terminal=factor_found shadow_stage=factor_extraction shadow_fallback=none"
            " factorization_present=true input_n=91 factor=7 cofactor=13 factor_identity=pass"
            " result_present=true relations_found=3 relations_source=shadow_selected_rows"
            " polynomials_used=17 polynomials_source=production_sieve_counter"
            " decision_wall_ns_supported=true decision_wall_ns=2500000000"
            " time_scope=siqs_timer_to_pre_emit_decision emit_phase=before_route promotion=false\n",
        "candidate route did not emit the exact V2 pre-route record");

    ScopedUnwritableStderr unwritable;
    auto failed = commit_prefer_route(stderr, Integer(91), candidate, make_prepared_result());
    unwritable.finish();
    require_test(!failed.has_value(), "candidate returned after its V2 record failed to commit");

    auto null_output =
        commit_prefer_route(nullptr, Integer(91), candidate, make_prepared_result());
    require_test(!null_output.has_value(), "candidate returned without an output stream");
}

void test_fallbacks_never_return_stale_candidates() {
    const auto no_factor = make_no_factor_decision();
    const auto [fallback, fallback_output] =
        commit_with_capture(no_factor, make_prepared_result());
    require_test(!fallback.has_value(), "no-factor fallback returned a stale candidate");
    require_test(
        fallback_output ==
            "GNFS_SIQS_SHADOW_PROOF_PREFER_DECISION_V2 schema_version=2 status=valid mode=prefer"
            " decision=legacy_fallback reason=shadow_not_factor next_route=legacy_continue"
            " shadow_terminal=no_factor shadow_stage=complete shadow_fallback=none"
            " factorization_present=false input_n=91 factor=0 cofactor=0"
            " factor_identity=not_checked result_present=false relations_found=0"
            " relations_source=none polynomials_used=0 polynomials_source=none"
            " decision_wall_ns_supported=false decision_wall_ns=0 time_scope=unavailable"
            " emit_phase=before_route promotion=false\n",
        "no-factor fallback did not emit the exact legacy-continuation record");

    const auto zero_wall = make_zero_wall_decision();
    const auto [metadata_fallback, metadata_output] =
        commit_with_capture(zero_wall, make_prepared_result());
    require_test(!metadata_fallback.has_value(),
                 "finalize-time metadata fallback returned a pre-finalization candidate");
    require_test(metadata_output.find("decision=legacy_fallback") != std::string::npos &&
                     metadata_output.find("reason=result_metadata_invalid") != std::string::npos &&
                     metadata_output.find("next_route=legacy_continue") != std::string::npos,
                 "finalize-time metadata fallback did not commit legacy continuation");
}

void test_candidate_mismatch_emits_nothing() {
    const auto candidate = make_candidate_decision();

    SIQSResult mismatched = make_prepared_result();
    mismatched.factor2 = Integer(11);
    const auto [wrong_factor, wrong_factor_output] =
        commit_with_capture(candidate, std::move(mismatched));
    require_test(!wrong_factor.has_value() && wrong_factor_output.empty(),
                 "factor mismatch emitted or returned a candidate");

    SIQSResult invalid_metadata = make_prepared_result();
    invalid_metadata.resolved_sieve_workers = 0;
    const auto [wrong_metadata, wrong_metadata_output] =
        commit_with_capture(candidate, std::move(invalid_metadata));
    require_test(!wrong_metadata.has_value() && wrong_metadata_output.empty(),
                 "metadata mismatch emitted or returned a candidate");

    const auto [missing, missing_output] = commit_with_capture(candidate, std::nullopt);
    require_test(!missing.has_value() && missing_output.empty(),
                 "missing prepared candidate emitted or returned a route");
}

void test_runtime_environment_paths() {
    struct ModeCase final {
        const char* value;
        SIQSShadowProofMode expected;
    };
    constexpr std::array cases{
        ModeCase{nullptr, SIQSShadowProofMode::off},
        ModeCase{"0", SIQSShadowProofMode::off},
        ModeCase{"observe", SIQSShadowProofMode::observe},
        ModeCase{"prefer", SIQSShadowProofMode::prefer},
    };
    for (const auto& test_case : cases) {
        ScopedEnvironmentVariable environment(gnfs::siqs::SIQS_SHADOW_PROOF_ENV, test_case.value);
        require_test(gnfs::siqs::parse_siqs_shadow_proof_mode(
                         std::getenv(gnfs::siqs::SIQS_SHADOW_PROOF_ENV)) == test_case.expected,
                     "runtime environment mode did not round trip");
    }

    ScopedEnvironmentVariable invalid(gnfs::siqs::SIQS_SHADOW_PROOF_ENV, "Prefer");
    try {
        (void)gnfs::siqs::parse_siqs_shadow_proof_mode(
            std::getenv(gnfs::siqs::SIQS_SHADOW_PROOF_ENV));
        require_test(false, "invalid runtime environment mode was accepted");
    } catch (const std::invalid_argument& error) {
        require_test(
            std::string_view(error.what()) ==
                "GNFS_SIQS_SHADOW_PROOF must be unset or exactly one of: 0, observe, prefer",
            "invalid runtime environment mode returned the wrong diagnostic");
    }
}

} // namespace

int main() {
    test_route_authority_truth_table();
    test_candidate_commit_and_emit_failure();
    test_fallbacks_never_return_stale_candidates();
    test_candidate_mismatch_emits_nothing();
    test_runtime_environment_paths();

    std::puts("SIQS shadow prefer route tests passed");
    return 0;
}
