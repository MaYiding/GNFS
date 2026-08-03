// test_siqs_shadow_proof_prefer.cpp - pure V2 shadow-route decision contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/shadow_proof_prefer.hpp>
#include <gnfs/siqs/shadow_proof_runner.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using std::size_t;
using std::uint64_t;

using gnfs::core::Integer;
using gnfs::siqs::emit_siqs_shadow_proof_prefer_decision;
using gnfs::siqs::evaluate_siqs_shadow_proof_prefer;
using gnfs::siqs::finalize_siqs_shadow_proof_prefer;
using gnfs::siqs::make_siqs_shadow_proof_prefer_source_view;
using gnfs::siqs::run_siqs_shadow_proof;
using gnfs::siqs::siqs_shadow_proof_prefer_decision_name;
using gnfs::siqs::siqs_shadow_proof_prefer_factor_identity_name;
using gnfs::siqs::siqs_shadow_proof_prefer_reason_name;
using gnfs::siqs::SIQSPostMergeDependencyStatus;
using gnfs::siqs::SIQSPostMergeFactorization;
using gnfs::siqs::SIQSPostMergeFactorStatus;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::SIQSShadowAssemblyStatus;
using gnfs::siqs::SIQSShadowMatrixStatus;
using gnfs::siqs::SIQSShadowProofEvidence;
using gnfs::siqs::SIQSShadowProofFallbackReason;
using gnfs::siqs::SIQSShadowProofPreferDecision;
using gnfs::siqs::SIQSShadowProofPreferDecisionKind;
using gnfs::siqs::SIQSShadowProofPreferDraft;
using gnfs::siqs::SIQSShadowProofPreferFactorIdentity;
using gnfs::siqs::SIQSShadowProofPreferReason;
using gnfs::siqs::SIQSShadowProofPreferSourceView;
using gnfs::siqs::SIQSShadowProofStage;
using gnfs::siqs::SIQSShadowProofTerminalStatus;

static_assert(noexcept(
    evaluate_siqs_shadow_proof_prefer(std::declval<const SIQSShadowProofPreferSourceView&>(),
                                      std::declval<const Integer&>(), size_t{})));
static_assert(noexcept(finalize_siqs_shadow_proof_prefer(std::declval<SIQSShadowProofPreferDraft>(),
                                                         uint64_t{})));
static_assert(noexcept(emit_siqs_shadow_proof_prefer_decision(
    static_cast<std::FILE*>(nullptr), std::declval<const Integer&>(),
    std::declval<const SIQSShadowProofPreferDecision&>())));

template <typename Draft>
concept CanFinalizePreferDraft = requires(Draft&& draft) {
    finalize_siqs_shadow_proof_prefer(std::forward<Draft>(draft), uint64_t{});
};

static_assert(CanFinalizePreferDraft<SIQSShadowProofPreferDraft>);
static_assert(!CanFinalizePreferDraft<SIQSShadowProofPreferDraft&>);

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

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

struct NoSplit {
    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t) const noexcept {
        return {0, 0};
    }
};

[[nodiscard]] SIQSShadowProofPreferSourceView
make_source(const SIQSShadowProofEvidence* evidence,
            const SIQSPostMergeFactorization* factorization,
            SIQSShadowProofTerminalStatus terminal = SIQSShadowProofTerminalStatus::factor_found,
            SIQSShadowProofStage stage = SIQSShadowProofStage::factor_extraction,
            SIQSShadowProofFallbackReason fallback = SIQSShadowProofFallbackReason::none) {
    return SIQSShadowProofPreferSourceView{terminal, stage, fallback, evidence, factorization};
}

[[nodiscard]] SIQSShadowProofPreferDecision decide(const SIQSShadowProofPreferSourceView& source,
                                                   const Integer& modulus,
                                                   size_t polynomials_used = 17,
                                                   uint64_t decision_wall_ns = 2'500'000'000ULL) {
    auto draft = evaluate_siqs_shadow_proof_prefer(source, modulus, polynomials_used);
    return finalize_siqs_shadow_proof_prefer(std::move(draft), decision_wall_ns);
}

[[nodiscard]] std::string read_stream(std::FILE* file) {
    if (file == nullptr || std::fflush(file) != 0 || std::fseek(file, 0, SEEK_END) != 0) {
        return {};
    }
    const long length = std::ftell(file);
    if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        return {};
    }
    std::string output(static_cast<size_t>(length), '\0');
    if (!output.empty()) {
        const size_t read = std::fread(output.data(), 1, output.size(), file);
        output.resize(read);
    }
    return output;
}

[[nodiscard]] std::pair<bool, std::string>
emit_to_string(const SIQSShadowProofPreferDecision& decision, const Integer& modulus) {
    std::FILE* file = std::tmpfile();
    if (file == nullptr) {
        return {false, {}};
    }
    const bool emitted = emit_siqs_shadow_proof_prefer_decision(file, modulus, decision);
    std::string output = read_stream(file);
    (void)std::fclose(file);
    return {emitted, std::move(output)};
}

[[nodiscard]] bool same_factorization(const SIQSPostMergeFactorization& lhs,
                                      const SIQSPostMergeFactorization& rhs) {
    return lhs.factor == rhs.factor && lhs.cofactor == rhs.cofactor;
}

[[nodiscard]] bool same_decision(const SIQSShadowProofPreferDecision& lhs,
                                 const SIQSShadowProofPreferDecision& rhs) {
    if (lhs.decision != rhs.decision || lhs.reason != rhs.reason ||
        lhs.shadow_terminal != rhs.shadow_terminal || lhs.shadow_stage != rhs.shadow_stage ||
        lhs.shadow_fallback != rhs.shadow_fallback ||
        lhs.factorization_present != rhs.factorization_present ||
        lhs.factor_identity != rhs.factor_identity ||
        lhs.candidate.has_value() != rhs.candidate.has_value()) {
        return false;
    }
    if (!lhs.candidate) {
        return true;
    }
    return same_factorization(lhs.candidate->factorization, rhs.candidate->factorization) &&
           lhs.candidate->relations_found == rhs.candidate->relations_found &&
           lhs.candidate->polynomials_used == rhs.candidate->polynomials_used &&
           lhs.candidate->decision_wall_ns == rhs.candidate->decision_wall_ns &&
           lhs.candidate->time_seconds == rhs.candidate->time_seconds;
}

void check_fallback(const SIQSShadowProofPreferDraft& draft,
                    SIQSShadowProofPreferReason expected_reason) {
    CHECK(draft.decision == SIQSShadowProofPreferDecisionKind::legacy_fallback);
    CHECK(draft.reason == expected_reason);
    CHECK(!draft.accepted.has_value());
}

void check_fallback(const SIQSShadowProofPreferDecision& decision,
                    SIQSShadowProofPreferReason expected_reason) {
    CHECK(decision.decision == SIQSShadowProofPreferDecisionKind::legacy_fallback);
    CHECK(decision.reason == expected_reason);
    CHECK(!decision.is_shadow_candidate());
    CHECK(!decision.candidate.has_value());
}

void test_enum_names() {
    constexpr std::array decisions{
        std::pair{SIQSShadowProofPreferDecisionKind::shadow_candidate,
                  std::string_view("shadow_candidate")},
        std::pair{SIQSShadowProofPreferDecisionKind::legacy_fallback,
                  std::string_view("legacy_fallback")},
    };
    for (const auto& [value, name] : decisions) {
        CHECK(siqs_shadow_proof_prefer_decision_name(value) == name);
    }
    CHECK(siqs_shadow_proof_prefer_decision_name(
              static_cast<SIQSShadowProofPreferDecisionKind>(255)) == "unknown");

    constexpr std::array reasons{
        std::pair{SIQSShadowProofPreferReason::shadow_factor_valid,
                  std::string_view("shadow_factor_valid")},
        std::pair{SIQSShadowProofPreferReason::shadow_not_factor,
                  std::string_view("shadow_not_factor")},
        std::pair{SIQSShadowProofPreferReason::shadow_contract_invalid,
                  std::string_view("shadow_contract_invalid")},
        std::pair{SIQSShadowProofPreferReason::factor_identity_invalid,
                  std::string_view("factor_identity_invalid")},
        std::pair{SIQSShadowProofPreferReason::result_metadata_invalid,
                  std::string_view("result_metadata_invalid")},
        std::pair{SIQSShadowProofPreferReason::decision_internal_failure,
                  std::string_view("decision_internal_failure")},
    };
    for (const auto& [value, name] : reasons) {
        CHECK(siqs_shadow_proof_prefer_reason_name(value) == name);
    }
    CHECK(siqs_shadow_proof_prefer_reason_name(static_cast<SIQSShadowProofPreferReason>(255)) ==
          "unknown");

    constexpr std::array identities{
        std::pair{SIQSShadowProofPreferFactorIdentity::pass, std::string_view("pass")},
        std::pair{SIQSShadowProofPreferFactorIdentity::fail, std::string_view("fail")},
        std::pair{SIQSShadowProofPreferFactorIdentity::not_checked,
                  std::string_view("not_checked")},
    };
    for (const auto& [value, name] : identities) {
        CHECK(siqs_shadow_proof_prefer_factor_identity_name(value) == name);
    }
    CHECK(siqs_shadow_proof_prefer_factor_identity_name(
              static_cast<SIQSShadowProofPreferFactorIdentity>(255)) == "unknown");
}

void check_success_case(const Integer& modulus, const Integer& factor, const Integer& cofactor) {
    SIQSShadowProofEvidence evidence = make_success_evidence();
    SIQSPostMergeFactorization factorization{factor, cofactor};
    const SIQSShadowProofEvidence evidence_before = evidence;
    const SIQSPostMergeFactorization factors_before = factorization;
    const Integer modulus_before = modulus;
    const auto source = make_source(&evidence, &factorization);

    auto draft = evaluate_siqs_shadow_proof_prefer(source, modulus, 17);
    CHECK(draft.decision == SIQSShadowProofPreferDecisionKind::shadow_candidate);
    CHECK(draft.reason == SIQSShadowProofPreferReason::shadow_factor_valid);
    CHECK(draft.shadow_terminal == SIQSShadowProofTerminalStatus::factor_found);
    CHECK(draft.shadow_stage == SIQSShadowProofStage::factor_extraction);
    CHECK(draft.shadow_fallback == SIQSShadowProofFallbackReason::none);
    CHECK(draft.factorization_present);
    CHECK(draft.factor_identity == SIQSShadowProofPreferFactorIdentity::pass);
    CHECK(draft.accepted.has_value());
    if (draft.accepted) {
        CHECK(same_factorization(draft.accepted->factorization, factorization));
        CHECK(draft.accepted->relations_found == 3);
        CHECK(draft.accepted->polynomials_used == 17);
    }

    const auto decision = finalize_siqs_shadow_proof_prefer(std::move(draft), 2'500'000'000ULL);
    CHECK(decision.is_shadow_candidate());
    CHECK(decision.decision == SIQSShadowProofPreferDecisionKind::shadow_candidate);
    CHECK(decision.reason == SIQSShadowProofPreferReason::shadow_factor_valid);
    CHECK(decision.factorization_present);
    CHECK(decision.factor_identity == SIQSShadowProofPreferFactorIdentity::pass);
    CHECK(decision.candidate.has_value());
    if (decision.candidate) {
        CHECK(same_factorization(decision.candidate->factorization, factorization));
        CHECK(decision.candidate->relations_found == 3);
        CHECK(decision.candidate->polynomials_used == 17);
        CHECK(decision.candidate->decision_wall_ns == 2'500'000'000ULL);
        CHECK(decision.candidate->time_seconds == 2.5);
    }
    CHECK(evidence == evidence_before);
    CHECK(same_factorization(factorization, factors_before));
    CHECK(modulus == modulus_before);
}

void test_success_square_large_repeat_and_ownership() {
    check_success_case(Integer(91), Integer(7), Integer(13));
    check_success_case(Integer(49), Integer(7), Integer(7));
    check_success_case(Integer("18027426610499408447671494571938206274555088868093"),
                       Integer("2041646378661656688438487"), Integer("8829847714527711737483339"));

    SIQSShadowProofEvidence evidence = make_success_evidence();
    SIQSPostMergeFactorization factorization{Integer(7), Integer(13)};
    const auto source = make_source(&evidence, &factorization);
    const auto first = decide(source, Integer(91));
    const auto second = decide(source, Integer(91));
    const auto third = decide(source, Integer(91));
    CHECK(same_decision(first, second));
    CHECK(same_decision(second, third));

    auto owned_draft = evaluate_siqs_shadow_proof_prefer(source, Integer(91), 17);
    factorization.factor = Integer(1);
    factorization.cofactor = Integer(90);
    evidence.assembly.selected_rows = 0;
    const auto owned = finalize_siqs_shadow_proof_prefer(std::move(owned_draft), 1'000'000'000ULL);
    CHECK(owned.is_shadow_candidate());
    if (owned.candidate) {
        CHECK(owned.candidate->factorization.factor == Integer(7));
        CHECK(owned.candidate->factorization.cofactor == Integer(13));
        CHECK(owned.candidate->relations_found == 3);
    }
}

void test_real_result_source_view_and_overload() {
    std::array<SIQSRelation, 2> relations{};
    relations[0].value = Integer(27);
    relations[0].exponents = {0};
    relations[1].value = Integer(1);
    relations[1].exponents = {0};
    constexpr std::array<std::uint32_t, 1> factor_base{0};
    NoSplit splitter;
    const auto result = run_siqs_shadow_proof(
        std::span<const SIQSRelation>(relations.data(), relations.size()),
        std::span<const std::uint32_t>(factor_base.data(), factor_base.size()), Integer(91),
        Integer(91), 47, splitter);
    CHECK(result.status() == SIQSShadowProofTerminalStatus::factor_found);
    CHECK(result.factorization().has_value());
    if (!result.factorization()) {
        return;
    }

    const SIQSShadowProofEvidence evidence_before = result.evidence();
    const SIQSPostMergeFactorization factors_before = *result.factorization();
    const auto source = make_siqs_shadow_proof_prefer_source_view(result);
    CHECK(source.terminal_status == result.status());
    CHECK(source.stage == result.stage());
    CHECK(source.fallback_reason == result.fallback_reason());
    CHECK(source.evidence == &result.evidence());
    CHECK(source.factorization == &*result.factorization());

    auto draft = evaluate_siqs_shadow_proof_prefer(result, Integer(91), 23);
    CHECK(draft.decision == SIQSShadowProofPreferDecisionKind::shadow_candidate);
    CHECK(draft.reason == SIQSShadowProofPreferReason::shadow_factor_valid);
    CHECK(draft.accepted.has_value());
    if (draft.accepted) {
        CHECK(draft.accepted->factorization.factor == Integer(7));
        CHECK(draft.accepted->factorization.cofactor == Integer(13));
        CHECK(draft.accepted->relations_found == 2);
        CHECK(draft.accepted->polynomials_used == 23);
    }
    const auto decision = finalize_siqs_shadow_proof_prefer(std::move(draft), 1'000'000ULL);
    CHECK(decision.is_shadow_candidate());
    CHECK(result.evidence() == evidence_before);
    CHECK(same_factorization(*result.factorization(), factors_before));
}

void test_all_terminals_and_fallbacks() {
    SIQSShadowProofEvidence evidence = make_success_evidence();
    SIQSPostMergeFactorization factorization{Integer(7), Integer(13)};
    struct TerminalCase {
        SIQSShadowProofTerminalStatus terminal;
        SIQSShadowProofStage stage;
        SIQSShadowProofFallbackReason fallback;
    };
    constexpr std::array terminals{
        TerminalCase{SIQSShadowProofTerminalStatus::factor_found,
                     SIQSShadowProofStage::factor_extraction, SIQSShadowProofFallbackReason::none},
        TerminalCase{SIQSShadowProofTerminalStatus::no_factor, SIQSShadowProofStage::complete,
                     SIQSShadowProofFallbackReason::none},
        TerminalCase{SIQSShadowProofTerminalStatus::bounded_fallback,
                     SIQSShadowProofStage::assembly,
                     SIQSShadowProofFallbackReason::raw_relation_limit},
        TerminalCase{SIQSShadowProofTerminalStatus::invalid_input,
                     SIQSShadowProofStage::input_validation, SIQSShadowProofFallbackReason::none},
        TerminalCase{SIQSShadowProofTerminalStatus::stage_failure, SIQSShadowProofStage::matrix,
                     SIQSShadowProofFallbackReason::none},
        TerminalCase{SIQSShadowProofTerminalStatus::resource_exhausted,
                     SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none},
        TerminalCase{SIQSShadowProofTerminalStatus::exception_failure,
                     SIQSShadowProofStage::dependency_verification,
                     SIQSShadowProofFallbackReason::none},
        TerminalCase{SIQSShadowProofTerminalStatus::internal_invariant_failure,
                     SIQSShadowProofStage::not_started, SIQSShadowProofFallbackReason::none},
    };
    for (const auto& item : terminals) {
        const auto source = make_source(
            &evidence,
            item.terminal == SIQSShadowProofTerminalStatus::factor_found ? &factorization : nullptr,
            item.terminal, item.stage, item.fallback);
        const auto draft = evaluate_siqs_shadow_proof_prefer(source, Integer(91), 17);
        if (item.terminal == SIQSShadowProofTerminalStatus::factor_found) {
            CHECK(draft.decision == SIQSShadowProofPreferDecisionKind::shadow_candidate);
        } else {
            check_fallback(draft, SIQSShadowProofPreferReason::shadow_not_factor);
        }
    }

    constexpr std::array fallback_reasons{
        SIQSShadowProofFallbackReason::raw_relation_limit,
        SIQSShadowProofFallbackReason::raw_payload_limit,
        SIQSShadowProofFallbackReason::graph_edge_limit,
        SIQSShadowProofFallbackReason::graph_cycle_limit,
        SIQSShadowProofFallbackReason::graph_incidence_limit,
        SIQSShadowProofFallbackReason::row_candidate_limit,
        SIQSShadowProofFallbackReason::pretrim_row_limit,
        SIQSShadowProofFallbackReason::insufficient_rows,
        SIQSShadowProofFallbackReason::matrix_resource_limit,
        SIQSShadowProofFallbackReason::matrix_backend_unavailable,
    };
    for (const auto fallback : fallback_reasons) {
        const auto bounded =
            make_source(&evidence, nullptr, SIQSShadowProofTerminalStatus::bounded_fallback,
                        SIQSShadowProofStage::assembly, fallback);
        check_fallback(evaluate_siqs_shadow_proof_prefer(bounded, Integer(91), 17),
                       SIQSShadowProofPreferReason::shadow_not_factor);

        const auto mismatch =
            make_source(&evidence, nullptr, SIQSShadowProofTerminalStatus::no_factor,
                        SIQSShadowProofStage::complete, fallback);
        check_fallback(evaluate_siqs_shadow_proof_prefer(mismatch, Integer(91), 17),
                       SIQSShadowProofPreferReason::shadow_contract_invalid);
    }
    const auto missing_reason =
        make_source(&evidence, nullptr, SIQSShadowProofTerminalStatus::bounded_fallback,
                    SIQSShadowProofStage::assembly, SIQSShadowProofFallbackReason::none);
    check_fallback(evaluate_siqs_shadow_proof_prefer(missing_reason, Integer(91), 17),
                   SIQSShadowProofPreferReason::shadow_contract_invalid);
}

void test_typed_mismatches_and_unknown_source_enums() {
    SIQSShadowProofEvidence evidence = make_success_evidence();
    SIQSPostMergeFactorization factors{Integer(7), Integer(13)};
    constexpr auto contract = SIQSShadowProofPreferReason::shadow_contract_invalid;

    check_fallback(
        evaluate_siqs_shadow_proof_prefer(make_source(nullptr, &factors), Integer(91), 17),
        contract);
    check_fallback(
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, nullptr), Integer(91), 17),
        contract);
    check_fallback(evaluate_siqs_shadow_proof_prefer(
                       make_source(&evidence, &factors, SIQSShadowProofTerminalStatus::factor_found,
                                   SIQSShadowProofStage::complete),
                       Integer(91), 17),
                   contract);
    check_fallback(evaluate_siqs_shadow_proof_prefer(
                       make_source(&evidence, &factors, SIQSShadowProofTerminalStatus::no_factor,
                                   SIQSShadowProofStage::complete),
                       Integer(91), 17),
                   contract);
    check_fallback(evaluate_siqs_shadow_proof_prefer(
                       make_source(&evidence, nullptr, SIQSShadowProofTerminalStatus::no_factor,
                                   SIQSShadowProofStage::matrix),
                       Integer(91), 17),
                   contract);
    check_fallback(evaluate_siqs_shadow_proof_prefer(
                       make_source(&evidence, nullptr, SIQSShadowProofTerminalStatus::invalid_input,
                                   SIQSShadowProofStage::not_started),
                       Integer(91), 17),
                   contract);

    auto unknown_terminal = make_source(&evidence, &factors);
    unknown_terminal.terminal_status = static_cast<SIQSShadowProofTerminalStatus>(255);
    check_fallback(evaluate_siqs_shadow_proof_prefer(unknown_terminal, Integer(91), 17), contract);
    auto unknown_stage = make_source(&evidence, &factors);
    unknown_stage.stage = static_cast<SIQSShadowProofStage>(255);
    check_fallback(evaluate_siqs_shadow_proof_prefer(unknown_stage, Integer(91), 17), contract);
    auto unknown_fallback = make_source(&evidence, &factors);
    unknown_fallback.fallback_reason = static_cast<SIQSShadowProofFallbackReason>(255);
    check_fallback(evaluate_siqs_shadow_proof_prefer(unknown_fallback, Integer(91), 17), contract);
}

void expect_evidence_rejected(SIQSShadowProofEvidence evidence,
                              SIQSShadowProofPreferReason expected_reason) {
    const SIQSPostMergeFactorization factors{Integer(7), Integer(13)};
    check_fallback(
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, &factors), Integer(91), 17),
        expected_reason);
}

void test_evidence_and_metadata_conservation() {
    constexpr auto contract = SIQSShadowProofPreferReason::shadow_contract_invalid;
    constexpr auto metadata = SIQSShadowProofPreferReason::result_metadata_invalid;
    SIQSShadowProofEvidence evidence;

    evidence = make_success_evidence();
    evidence.dependency_status.reset();
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.dependency_status = SIQSPostMergeDependencyStatus::invalid_dependency;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.factor_status.reset();
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.factor_status = SIQSPostMergeFactorStatus::no_factor;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.dependencies_returned = 0;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.dependencies_examined = 0;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.dependencies_examined = 3;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.dependencies_verified = 1;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.factor_found_count = 0;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.factor_found_count = 2;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.first_failed_dependency = 0;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.winning_dependency.reset();
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.winning_dependency_size.reset();
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.no_factor_count = std::numeric_limits<size_t>::max();
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.no_factor_count = 0;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.winning_dependency = 2;
    expect_evidence_rejected(evidence, contract);
    evidence = make_success_evidence();
    evidence.winning_dependency = 0;
    expect_evidence_rejected(evidence, contract);

    evidence = make_success_evidence();
    evidence.assembly_status.reset();
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.assembly_status = SIQSShadowAssemblyStatus::worker_failure;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.matrix_status.reset();
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.matrix_status = SIQSShadowMatrixStatus::worker_failure;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.matrix_columns = 0;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.matrix_rows = 2;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.minimum_nullity = 2;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.assembly.selected_rows = 2;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.winning_dependency_size = 0;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.winning_dependency_size = 4;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.assembly.selected_full_rows = 1;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.assembly.selected_full_rows = std::numeric_limits<size_t>::max();
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.assembly.pretrim_rows = 4;
    expect_evidence_rejected(evidence, metadata);
    evidence = make_success_evidence();
    evidence.assembly.trimmed_rows = std::numeric_limits<size_t>::max();
    expect_evidence_rejected(evidence, metadata);

    evidence = make_success_evidence();
    const SIQSPostMergeFactorization factors{Integer(7), Integer(13)};
    check_fallback(
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, &factors), Integer(91), 0),
        metadata);
}

void expect_factor_rejected(const Integer& modulus, const Integer& factor,
                            const Integer& cofactor) {
    SIQSShadowProofEvidence evidence = make_success_evidence();
    const SIQSPostMergeFactorization factors{factor, cofactor};
    const auto draft =
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, &factors), modulus, 17);
    check_fallback(draft, SIQSShadowProofPreferReason::factor_identity_invalid);
    CHECK(draft.factorization_present);
    CHECK(draft.factor_identity == SIQSShadowProofPreferFactorIdentity::fail);
}

void test_factor_and_wall_boundaries() {
    expect_factor_rejected(Integer(-91), Integer(7), Integer(13));
    expect_factor_rejected(Integer(0), Integer(7), Integer(13));
    expect_factor_rejected(Integer(1), Integer(7), Integer(13));
    expect_factor_rejected(Integer(91), Integer(-7), Integer(13));
    expect_factor_rejected(Integer(91), Integer(0), Integer(13));
    expect_factor_rejected(Integer(91), Integer(1), Integer(91));
    expect_factor_rejected(Integer(91), Integer(7), Integer(-13));
    expect_factor_rejected(Integer(91), Integer(7), Integer(0));
    expect_factor_rejected(Integer(91), Integer(7), Integer(1));
    expect_factor_rejected(Integer(91), Integer(91), Integer(13));
    expect_factor_rejected(Integer(91), Integer(7), Integer(91));
    expect_factor_rejected(Integer(91), Integer(13), Integer(7));
    expect_factor_rejected(Integer(91), Integer(7), Integer(11));
    expect_factor_rejected(Integer(77), Integer(7), Integer(13));

    SIQSShadowProofEvidence evidence = make_success_evidence();
    SIQSPostMergeFactorization factors{Integer(7), Integer(13)};
    auto draft =
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, &factors), Integer(91), 17);
    const auto zero_wall = finalize_siqs_shadow_proof_prefer(std::move(draft), 0);
    check_fallback(zero_wall, SIQSShadowProofPreferReason::result_metadata_invalid);
    CHECK(zero_wall.factorization_present);
    CHECK(zero_wall.factor_identity == SIQSShadowProofPreferFactorIdentity::pass);
}

void expect_emit_rejected(const SIQSShadowProofPreferDecision& decision,
                          const Integer& modulus = Integer(91)) {
    const auto [emitted, output] = emit_to_string(decision, modulus);
    CHECK(!emitted);
    CHECK(output.empty());
}

void test_exact_emitter_schema_and_failures() {
    SIQSShadowProofEvidence evidence = make_success_evidence();
    SIQSPostMergeFactorization factors{Integer(7), Integer(13)};
    const auto candidate = decide(make_source(&evidence, &factors), Integer(91));
    const auto [candidate_emitted, candidate_line] = emit_to_string(candidate, Integer(91));
    CHECK(candidate_emitted);
    CHECK(candidate_line ==
          "GNFS_SIQS_SHADOW_PROOF_PREFER_DECISION_V2 schema_version=2 status=valid mode=prefer"
          " decision=shadow_candidate reason=shadow_factor_valid next_route=shadow_return"
          " shadow_terminal=factor_found shadow_stage=factor_extraction shadow_fallback=none"
          " factorization_present=true input_n=91 factor=7 cofactor=13 factor_identity=pass"
          " result_present=true relations_found=3 relations_source=shadow_selected_rows"
          " polynomials_used=17 polynomials_source=production_sieve_counter"
          " decision_wall_ns_supported=true decision_wall_ns=2500000000"
          " time_scope=siqs_timer_to_pre_emit_decision emit_phase=before_route promotion=false\n");
    CHECK(std::count(candidate_line.begin(), candidate_line.end(), '\n') == 1);

    const auto no_factor_source =
        make_source(&evidence, nullptr, SIQSShadowProofTerminalStatus::no_factor,
                    SIQSShadowProofStage::complete);
    const auto fallback = decide(no_factor_source, Integer(91));
    const auto [fallback_emitted, fallback_line] = emit_to_string(fallback, Integer(91));
    CHECK(fallback_emitted);
    CHECK(fallback_line ==
          "GNFS_SIQS_SHADOW_PROOF_PREFER_DECISION_V2 schema_version=2 status=valid mode=prefer"
          " decision=legacy_fallback reason=shadow_not_factor next_route=legacy_continue"
          " shadow_terminal=no_factor shadow_stage=complete shadow_fallback=none"
          " factorization_present=false input_n=91 factor=0 cofactor=0"
          " factor_identity=not_checked result_present=false relations_found=0"
          " relations_source=none polynomials_used=0 polynomials_source=none"
          " decision_wall_ns_supported=false decision_wall_ns=0 time_scope=unavailable"
          " emit_phase=before_route promotion=false\n");
    CHECK(!emit_siqs_shadow_proof_prefer_decision(nullptr, Integer(91), candidate));

    try {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("gnfs-shadow-prefer-read-only-" + std::to_string(nonce) + ".tmp");
        std::FILE* writable = std::fopen(path.string().c_str(), "wb");
        CHECK(writable != nullptr);
        if (writable != nullptr) {
            CHECK(std::fclose(writable) == 0);
            std::FILE* read_only = std::fopen(path.string().c_str(), "rb");
            CHECK(read_only != nullptr);
            if (read_only != nullptr) {
                CHECK(!emit_siqs_shadow_proof_prefer_decision(read_only, Integer(91), candidate));
                (void)std::fclose(read_only);
            }
        }
        std::error_code error;
        (void)std::filesystem::remove(path, error);
        CHECK(!error);
    } catch (...) {
        CHECK(false);
    }
}

void test_public_decision_shape_mutations_are_rejected() {
    SIQSShadowProofEvidence evidence = make_success_evidence();
    SIQSPostMergeFactorization factors{Integer(7), Integer(13)};
    const auto candidate = decide(make_source(&evidence, &factors), Integer(91));
    SIQSShadowProofPreferDecision mutated;

    mutated = candidate;
    mutated.decision = SIQSShadowProofPreferDecisionKind::legacy_fallback;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.reason = SIQSShadowProofPreferReason::shadow_not_factor;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.shadow_terminal = SIQSShadowProofTerminalStatus::no_factor;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.shadow_stage = SIQSShadowProofStage::complete;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.shadow_fallback = SIQSShadowProofFallbackReason::raw_relation_limit;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.factorization_present = false;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.factor_identity = SIQSShadowProofPreferFactorIdentity::fail;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.candidate.reset();
    expect_emit_rejected(mutated);

    mutated = candidate;
    mutated.candidate->relations_found = 0;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.candidate->polynomials_used = 0;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.candidate->decision_wall_ns = 0;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.candidate->time_seconds = 0.0;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.candidate->time_seconds = std::numeric_limits<double>::infinity();
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.candidate->time_seconds = std::numeric_limits<double>::quiet_NaN();
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.candidate->time_seconds = 2.6;
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.candidate->factorization.factor = Integer(13);
    mutated.candidate->factorization.cofactor = Integer(7);
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.candidate->factorization.cofactor = Integer(11);
    expect_emit_rejected(mutated);

    mutated = candidate;
    mutated.decision = static_cast<SIQSShadowProofPreferDecisionKind>(255);
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.reason = static_cast<SIQSShadowProofPreferReason>(255);
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.shadow_terminal = static_cast<SIQSShadowProofTerminalStatus>(255);
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.shadow_stage = static_cast<SIQSShadowProofStage>(255);
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.shadow_fallback = static_cast<SIQSShadowProofFallbackReason>(255);
    expect_emit_rejected(mutated);
    mutated = candidate;
    mutated.factor_identity = static_cast<SIQSShadowProofPreferFactorIdentity>(255);
    expect_emit_rejected(mutated);

    const auto fallback =
        decide(make_source(&evidence, nullptr, SIQSShadowProofTerminalStatus::no_factor,
                           SIQSShadowProofStage::complete),
               Integer(91));
    mutated = fallback;
    mutated.decision = SIQSShadowProofPreferDecisionKind::shadow_candidate;
    expect_emit_rejected(mutated);
    mutated = fallback;
    mutated.reason = SIQSShadowProofPreferReason::shadow_factor_valid;
    expect_emit_rejected(mutated);
    mutated = fallback;
    mutated.reason = static_cast<SIQSShadowProofPreferReason>(255);
    expect_emit_rejected(mutated);
    mutated = fallback;
    mutated.factor_identity = static_cast<SIQSShadowProofPreferFactorIdentity>(255);
    expect_emit_rejected(mutated);
    mutated = fallback;
    mutated.candidate = candidate.candidate;
    expect_emit_rejected(mutated);
    mutated = fallback;
    mutated.shadow_terminal = SIQSShadowProofTerminalStatus::factor_found;
    expect_emit_rejected(mutated);
    mutated = fallback;
    mutated.shadow_stage = SIQSShadowProofStage::input_validation;
    expect_emit_rejected(mutated);
    mutated = fallback;
    mutated.shadow_fallback = SIQSShadowProofFallbackReason::raw_relation_limit;
    expect_emit_rejected(mutated);
    mutated = fallback;
    mutated.factorization_present = true;
    expect_emit_rejected(mutated);
    mutated = fallback;
    mutated.factor_identity = SIQSShadowProofPreferFactorIdentity::pass;
    expect_emit_rejected(mutated);

    const SIQSPostMergeFactorization bad_factors{Integer(7), Integer(11)};
    auto factor_invalid_draft =
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, &bad_factors), Integer(91), 17);
    const auto factor_invalid =
        finalize_siqs_shadow_proof_prefer(std::move(factor_invalid_draft), 1);
    CHECK(emit_to_string(factor_invalid, Integer(91)).first);
    mutated = factor_invalid;
    mutated.shadow_stage = SIQSShadowProofStage::complete;
    expect_emit_rejected(mutated);
    mutated = factor_invalid;
    mutated.factorization_present = false;
    expect_emit_rejected(mutated);
    mutated = factor_invalid;
    mutated.factor_identity = SIQSShadowProofPreferFactorIdentity::not_checked;
    expect_emit_rejected(mutated);

    SIQSShadowProofEvidence invalid_metadata_evidence = make_success_evidence();
    invalid_metadata_evidence.matrix_columns = 0;
    auto metadata_invalid_draft = evaluate_siqs_shadow_proof_prefer(
        make_source(&invalid_metadata_evidence, &factors), Integer(91), 17);
    const auto metadata_invalid =
        finalize_siqs_shadow_proof_prefer(std::move(metadata_invalid_draft), 1);
    CHECK(emit_to_string(metadata_invalid, Integer(91)).first);
    mutated = metadata_invalid;
    mutated.shadow_terminal = SIQSShadowProofTerminalStatus::no_factor;
    expect_emit_rejected(mutated);
    mutated = metadata_invalid;
    mutated.factorization_present = false;
    expect_emit_rejected(mutated);
    mutated = metadata_invalid;
    mutated.factor_identity = SIQSShadowProofPreferFactorIdentity::fail;
    expect_emit_rejected(mutated);

    auto contract_invalid_draft =
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, nullptr), Integer(91), 17);
    const auto contract_invalid =
        finalize_siqs_shadow_proof_prefer(std::move(contract_invalid_draft), 1);
    CHECK(emit_to_string(contract_invalid, Integer(91)).first);
    mutated = contract_invalid;
    mutated.factor_identity = SIQSShadowProofPreferFactorIdentity::pass;
    expect_emit_rejected(mutated);

    auto malformed_draft =
        evaluate_siqs_shadow_proof_prefer(make_source(&evidence, &factors), Integer(91), 17);
    malformed_draft.reason = SIQSShadowProofPreferReason::shadow_not_factor;
    check_fallback(finalize_siqs_shadow_proof_prefer(std::move(malformed_draft), 1),
                   SIQSShadowProofPreferReason::decision_internal_failure);

    auto malformed_fallback_draft = evaluate_siqs_shadow_proof_prefer(
        make_source(&evidence, nullptr, SIQSShadowProofTerminalStatus::no_factor,
                    SIQSShadowProofStage::complete),
        Integer(91), 17);
    malformed_fallback_draft.shadow_terminal = SIQSShadowProofTerminalStatus::factor_found;
    malformed_fallback_draft.shadow_stage = SIQSShadowProofStage::factor_extraction;
    malformed_fallback_draft.factorization_present = true;
    malformed_fallback_draft.factor_identity = SIQSShadowProofPreferFactorIdentity::pass;
    auto internal_failure =
        finalize_siqs_shadow_proof_prefer(std::move(malformed_fallback_draft), 1);
    check_fallback(internal_failure, SIQSShadowProofPreferReason::decision_internal_failure);
    CHECK(emit_to_string(internal_failure, Integer(91)).first);
    internal_failure.factor_identity = SIQSShadowProofPreferFactorIdentity::pass;
    expect_emit_rejected(internal_failure);
}

} // namespace

int main() {
    test_enum_names();
    test_success_square_large_repeat_and_ownership();
    test_real_result_source_view_and_overload();
    test_all_terminals_and_fallbacks();
    test_typed_mismatches_and_unknown_source_enums();
    test_evidence_and_metadata_conservation();
    test_factor_and_wall_boundaries();
    test_exact_emitter_schema_and_failures();
    test_public_decision_shape_mutations_are_rejected();

    std::cout << "SIQS shadow proof prefer: " << checks_passed << " passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
