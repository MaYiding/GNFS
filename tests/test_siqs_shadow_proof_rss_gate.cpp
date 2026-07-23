// Outcome-blind contract tests for the sealed SIQS shadow-proof RSS gate.
// This test deliberately never includes or calls production SIQS factoring,
// probes, holdout measurements, or live process-memory capture.

#include "fixtures/siqs_shadow_observe_rss_holdouts_v1.hpp"

#include <gnfs/siqs/shadow_proof_rss_gate.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;

using gnfs::siqs::emit_siqs_shadow_proof_rss_gate_outcome;
using gnfs::siqs::evaluate_siqs_shadow_proof_rss_gate;
using gnfs::siqs::siqs_shadow_proof_rss_architecture_name;
using gnfs::siqs::siqs_shadow_proof_rss_evidence_name;
using gnfs::siqs::siqs_shadow_proof_rss_factor_identity_name;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_MAX_TOKEN_BYTES;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS;
using gnfs::siqs::siqs_shadow_proof_rss_gate_policy_error;
using gnfs::siqs::siqs_shadow_proof_rss_gate_reason_name;
using gnfs::siqs::siqs_shadow_proof_rss_gate_status_name;
using gnfs::siqs::siqs_shadow_proof_rss_operating_system_name;
using gnfs::siqs::siqs_shadow_proof_rss_sample_mode_name;
using gnfs::siqs::SIQSShadowProofRssArchitecture;
using gnfs::siqs::SIQSShadowProofRssCorpusDigest;
using gnfs::siqs::SIQSShadowProofRssEvidence;
using gnfs::siqs::SIQSShadowProofRssFactorIdentity;
using gnfs::siqs::SIQSShadowProofRssGateOutcome;
using gnfs::siqs::SIQSShadowProofRssGatePolicy;
using gnfs::siqs::SIQSShadowProofRssGateReason;
using gnfs::siqs::SIQSShadowProofRssGateSample;
using gnfs::siqs::SIQSShadowProofRssGateStatus;
using gnfs::siqs::SIQSShadowProofRssOperatingSystem;
using gnfs::siqs::SIQSShadowProofRssSampleMode;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH;
using gnfs::tests::fixtures::SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW;
using gnfs::util::ProcessMemoryBackend;

static_assert(SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT == 8);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS == 3);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS == 7);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT == 80);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_MAX_TOKEN_BYTES <= static_cast<size_t>(INT_MAX));
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID == SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW ==
              SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH ==
              SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH);
static_assert(noexcept(evaluate_siqs_shadow_proof_rss_gate(
    static_cast<const SIQSShadowProofRssGatePolicy*>(nullptr),
    std::declval<std::span<const SIQSShadowProofRssGateSample>>())));
static_assert(noexcept(emit_siqs_shadow_proof_rss_gate_outcome(
    static_cast<std::FILE*>(nullptr), static_cast<const SIQSShadowProofRssGatePolicy*>(nullptr),
    std::declval<std::span<const SIQSShadowProofRssGateSample>>(),
    std::declval<const SIQSShadowProofRssGateOutcome&>())));

constexpr uint64_t DEPLOYMENT_BUDGET_BYTES = UINT64_C(1000);
constexpr uint64_t RESERVED_HEADROOM_BYTES = UINT64_C(100);
constexpr uint64_t RSS_LIMIT_BYTES = DEPLOYMENT_BUDGET_BYTES - RESERVED_HEADROOM_BYTES;

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

[[nodiscard]] SIQSShadowProofRssGatePolicy make_policy() {
    SIQSShadowProofRssGatePolicy policy;
    policy.approved = true;
    policy.corpus_id = SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID;
    policy.corpus_digest = {SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW,
                            SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH};
    policy.operating_system = SIQSShadowProofRssOperatingSystem::darwin;
    policy.architecture = SIQSShadowProofRssArchitecture::arm64;
    policy.memory_backend = ProcessMemoryBackend::DarwinGetrusage;
    policy.resolved_production_sieve_workers = 4;
    policy.candidate_revision = "candidate-revision-1";
    policy.approval_id = "approval-ticket-1";
    policy.journal_store = {{UINT64_C(1010101010101010), UINT64_C(2020202020202020)},
                            {UINT64_C(1111222233334444), UINT64_C(5555666677778888)},
                            "rss-campaign-prod-v1"};
    policy.deployment_budget_bytes = DEPLOYMENT_BUDGET_BYTES;
    policy.reserved_headroom_bytes = RESERVED_HEADROOM_BYTES;
    return policy;
}

[[nodiscard]] SIQSShadowProofRssGateSample
make_sample(const SIQSShadowProofRssGatePolicy& policy, uint32_t fixture_id,
            SIQSShadowProofRssSampleMode mode, uint32_t ordinal, uint64_t observe_peak_rss_bytes) {
    SIQSShadowProofRssGateSample sample;
    sample.policy_approved = policy.approved;
    sample.corpus_id = policy.corpus_id;
    sample.corpus_digest = policy.corpus_digest;
    sample.operating_system = policy.operating_system;
    sample.architecture = policy.architecture;
    sample.memory_backend = policy.memory_backend;
    sample.resolved_production_sieve_workers = policy.resolved_production_sieve_workers;
    sample.candidate_revision = policy.candidate_revision;
    sample.approval_id = policy.approval_id;
    sample.journal_store = policy.journal_store;
    sample.deployment_budget_bytes = policy.deployment_budget_bytes;
    sample.reserved_headroom_bytes = policy.reserved_headroom_bytes;
    sample.fixture_id = fixture_id;
    sample.mode = mode;
    sample.ordinal = ordinal;
    sample.fresh_process = true;
    sample.completed = true;
    sample.factor_identity = SIQSShadowProofRssFactorIdentity::pass;
    sample.proof_evidence = mode == SIQSShadowProofRssSampleMode::observe
                                ? SIQSShadowProofRssEvidence::pass
                                : SIQSShadowProofRssEvidence::not_applicable;
    sample.matrix_evidence = mode == SIQSShadowProofRssSampleMode::observe
                                 ? SIQSShadowProofRssEvidence::pass
                                 : SIQSShadowProofRssEvidence::not_applicable;
    if (mode == SIQSShadowProofRssSampleMode::observe) {
        sample.absolute_peak_rss_bytes = observe_peak_rss_bytes;
    }
    sample.observe_minus_off_peak_bytes = INT64_C(17);
    sample.current_rss_bytes = UINT64_C(111);
    sample.peak_growth_bytes = UINT64_C(22);
    sample.wall_ns = UINT64_C(333);
    return sample;
}

[[nodiscard]] std::vector<SIQSShadowProofRssGateSample>
make_complete_samples(const SIQSShadowProofRssGatePolicy& policy, uint64_t observe_peak_rss_bytes) {
    std::vector<SIQSShadowProofRssGateSample> samples;
    samples.reserve(SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);
    for (uint32_t fixture_id = 1; fixture_id <= SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT;
         ++fixture_id) {
        for (uint32_t ordinal = 1; ordinal <= SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS;
             ++ordinal) {
            samples.push_back(make_sample(policy, fixture_id, SIQSShadowProofRssSampleMode::off,
                                          ordinal, observe_peak_rss_bytes));
        }
        for (uint32_t ordinal = 1; ordinal <= SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
             ++ordinal) {
            samples.push_back(make_sample(policy, fixture_id, SIQSShadowProofRssSampleMode::observe,
                                          ordinal, observe_peak_rss_bytes));
        }
    }
    return samples;
}

[[nodiscard]] std::span<const SIQSShadowProofRssGateSample>
sample_span(const std::vector<SIQSShadowProofRssGateSample>& samples) noexcept {
    return {samples.data(), samples.size()};
}

[[nodiscard]] SIQSShadowProofRssGateOutcome
evaluate_closed(const SIQSShadowProofRssGatePolicy* policy,
                const std::vector<SIQSShadowProofRssGateSample>& samples) {
    const SIQSShadowProofRssGateOutcome result =
        evaluate_siqs_shadow_proof_rss_gate(policy, sample_span(samples));
    CHECK(!result.shadow_outcome_routed);
    CHECK(!result.promotion);
    return result;
}

SIQSShadowProofRssGateOutcome
expect_outcome(const SIQSShadowProofRssGatePolicy* policy,
               const std::vector<SIQSShadowProofRssGateSample>& samples,
               SIQSShadowProofRssGateStatus status, SIQSShadowProofRssGateReason reason) {
    const SIQSShadowProofRssGateOutcome result = evaluate_closed(policy, samples);
    CHECK(result.status == status);
    CHECK(result.reason == reason);
    return result;
}

[[nodiscard]] size_t find_first_mode(const std::vector<SIQSShadowProofRssGateSample>& samples,
                                     SIQSShadowProofRssSampleMode mode) {
    const auto found = std::find_if(samples.begin(), samples.end(),
                                    [mode](const auto& sample) { return sample.mode == mode; });
    CHECK(found != samples.end());
    return found == samples.end() ? 0 : static_cast<size_t>(found - samples.begin());
}

void test_frozen_contract_and_enum_names() {
    CHECK(SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID == SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_CORPUS_ID);
    CHECK(SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW ==
          SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_LOW);
    CHECK(SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH ==
          SIQS_SHADOW_OBSERVE_RSS_HOLDOUT_V1_DIGEST_HIGH);
    CHECK(SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT ==
          SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT *
              (SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS +
               SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS));

    constexpr std::array operating_systems{
        std::pair{SIQSShadowProofRssOperatingSystem::unknown, std::string_view("unknown")},
        std::pair{SIQSShadowProofRssOperatingSystem::darwin, std::string_view("darwin")},
        std::pair{SIQSShadowProofRssOperatingSystem::linux, std::string_view("linux")},
        std::pair{SIQSShadowProofRssOperatingSystem::windows, std::string_view("windows")},
    };
    for (const auto& [value, name] : operating_systems) {
        CHECK(siqs_shadow_proof_rss_operating_system_name(value) == name);
    }
    CHECK(siqs_shadow_proof_rss_operating_system_name(
              static_cast<SIQSShadowProofRssOperatingSystem>(255)) == "unknown");

    constexpr std::array architectures{
        std::pair{SIQSShadowProofRssArchitecture::unknown, std::string_view("unknown")},
        std::pair{SIQSShadowProofRssArchitecture::x86_64, std::string_view("x86_64")},
        std::pair{SIQSShadowProofRssArchitecture::arm64, std::string_view("arm64")},
    };
    for (const auto& [value, name] : architectures) {
        CHECK(siqs_shadow_proof_rss_architecture_name(value) == name);
    }
    CHECK(siqs_shadow_proof_rss_architecture_name(
              static_cast<SIQSShadowProofRssArchitecture>(255)) == "unknown");

    constexpr std::array modes{
        std::pair{SIQSShadowProofRssSampleMode::unknown, std::string_view("unknown")},
        std::pair{SIQSShadowProofRssSampleMode::off, std::string_view("off")},
        std::pair{SIQSShadowProofRssSampleMode::observe, std::string_view("observe")},
    };
    for (const auto& [value, name] : modes) {
        CHECK(siqs_shadow_proof_rss_sample_mode_name(value) == name);
    }
    CHECK(siqs_shadow_proof_rss_sample_mode_name(static_cast<SIQSShadowProofRssSampleMode>(255)) ==
          "unknown");

    constexpr std::array evidence{
        std::pair{SIQSShadowProofRssEvidence::unknown, std::string_view("unknown")},
        std::pair{SIQSShadowProofRssEvidence::not_applicable, std::string_view("not_applicable")},
        std::pair{SIQSShadowProofRssEvidence::pass, std::string_view("pass")},
        std::pair{SIQSShadowProofRssEvidence::fail, std::string_view("fail")},
    };
    for (const auto& [value, name] : evidence) {
        CHECK(siqs_shadow_proof_rss_evidence_name(value) == name);
    }
    CHECK(siqs_shadow_proof_rss_evidence_name(static_cast<SIQSShadowProofRssEvidence>(255)) ==
          "unknown");

    constexpr std::array identities{
        std::pair{SIQSShadowProofRssFactorIdentity::unknown, std::string_view("unknown")},
        std::pair{SIQSShadowProofRssFactorIdentity::pass, std::string_view("pass")},
        std::pair{SIQSShadowProofRssFactorIdentity::fail, std::string_view("fail")},
        std::pair{SIQSShadowProofRssFactorIdentity::not_checked, std::string_view("not_checked")},
    };
    for (const auto& [value, name] : identities) {
        CHECK(siqs_shadow_proof_rss_factor_identity_name(value) == name);
    }
    CHECK(siqs_shadow_proof_rss_factor_identity_name(
              static_cast<SIQSShadowProofRssFactorIdentity>(255)) == "unknown");

    constexpr std::array statuses{
        std::pair{SIQSShadowProofRssGateStatus::blocked, std::string_view("blocked")},
        std::pair{SIQSShadowProofRssGateStatus::invalid, std::string_view("invalid")},
        std::pair{SIQSShadowProofRssGateStatus::limit_exceeded, std::string_view("limit_exceeded")},
        std::pair{SIQSShadowProofRssGateStatus::manual_review_candidate,
                  std::string_view("manual_review_candidate")},
    };
    for (const auto& [value, name] : statuses) {
        CHECK(siqs_shadow_proof_rss_gate_status_name(value) == name);
    }
    CHECK(siqs_shadow_proof_rss_gate_status_name(static_cast<SIQSShadowProofRssGateStatus>(255)) ==
          "unknown");

    constexpr std::array reasons{
        std::pair{SIQSShadowProofRssGateReason::policy_missing, std::string_view("policy_missing")},
        std::pair{SIQSShadowProofRssGateReason::policy_not_approved,
                  std::string_view("policy_not_approved")},
        std::pair{SIQSShadowProofRssGateReason::policy_budget_missing,
                  std::string_view("policy_budget_missing")},
        std::pair{SIQSShadowProofRssGateReason::policy_headroom_missing,
                  std::string_view("policy_headroom_missing")},
        std::pair{SIQSShadowProofRssGateReason::policy_budget_not_above_headroom,
                  std::string_view("policy_budget_not_above_headroom")},
        std::pair{SIQSShadowProofRssGateReason::policy_binding_invalid,
                  std::string_view("policy_binding_invalid")},
        std::pair{SIQSShadowProofRssGateReason::sample_count_invalid,
                  std::string_view("sample_count_invalid")},
        std::pair{SIQSShadowProofRssGateReason::sample_enum_invalid,
                  std::string_view("sample_enum_invalid")},
        std::pair{SIQSShadowProofRssGateReason::sample_binding_mismatch,
                  std::string_view("sample_binding_mismatch")},
        std::pair{SIQSShadowProofRssGateReason::sample_fixture_out_of_range,
                  std::string_view("sample_fixture_out_of_range")},
        std::pair{SIQSShadowProofRssGateReason::sample_ordinal_out_of_range,
                  std::string_view("sample_ordinal_out_of_range")},
        std::pair{SIQSShadowProofRssGateReason::sample_duplicate,
                  std::string_view("sample_duplicate")},
        std::pair{SIQSShadowProofRssGateReason::sample_missing, std::string_view("sample_missing")},
        std::pair{SIQSShadowProofRssGateReason::sample_execution_invalid,
                  std::string_view("sample_execution_invalid")},
        std::pair{SIQSShadowProofRssGateReason::sample_factor_identity_invalid,
                  std::string_view("sample_factor_identity_invalid")},
        std::pair{SIQSShadowProofRssGateReason::observe_evidence_invalid,
                  std::string_view("observe_evidence_invalid")},
        std::pair{SIQSShadowProofRssGateReason::observe_peak_missing,
                  std::string_view("observe_peak_missing")},
        std::pair{SIQSShadowProofRssGateReason::observe_peak_zero,
                  std::string_view("observe_peak_zero")},
        std::pair{SIQSShadowProofRssGateReason::observe_peak_over_limit,
                  std::string_view("observe_peak_over_limit")},
        std::pair{SIQSShadowProofRssGateReason::all_observe_peaks_within_limit,
                  std::string_view("all_observe_peaks_within_limit")},
        std::pair{SIQSShadowProofRssGateReason::internal_failure,
                  std::string_view("internal_failure")},
    };
    for (const auto& [value, name] : reasons) {
        CHECK(siqs_shadow_proof_rss_gate_reason_name(value) == name);
    }
    CHECK(siqs_shadow_proof_rss_gate_reason_name(static_cast<SIQSShadowProofRssGateReason>(255)) ==
          "unknown");
}

void test_policy_and_budget_boundaries() {
    const auto approved = make_policy();
    const auto complete = make_complete_samples(approved, RSS_LIMIT_BYTES);

    CHECK(!siqs_shadow_proof_rss_gate_policy_error(&approved).has_value());
    CHECK(siqs_shadow_proof_rss_gate_policy_error(nullptr) ==
          SIQSShadowProofRssGateReason::policy_missing);

    const auto missing = expect_outcome(nullptr, complete, SIQSShadowProofRssGateStatus::blocked,
                                        SIQSShadowProofRssGateReason::policy_missing);
    CHECK(missing.total_sample_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);
    CHECK(missing.rss_limit_bytes == 0);

    auto policy = approved;
    policy.approved = false;
    auto rejected = expect_outcome(&policy, complete, SIQSShadowProofRssGateStatus::blocked,
                                   SIQSShadowProofRssGateReason::policy_not_approved);
    CHECK(rejected.rss_limit_bytes == 0);

    policy = approved;
    policy.deployment_budget_bytes.reset();
    rejected = expect_outcome(&policy, complete, SIQSShadowProofRssGateStatus::blocked,
                              SIQSShadowProofRssGateReason::policy_budget_missing);
    CHECK(rejected.rss_limit_bytes == 0);

    policy = approved;
    policy.reserved_headroom_bytes.reset();
    rejected = expect_outcome(&policy, complete, SIQSShadowProofRssGateStatus::blocked,
                              SIQSShadowProofRssGateReason::policy_headroom_missing);
    CHECK(rejected.rss_limit_bytes == 0);

    for (const auto [budget, headroom] :
         std::array{std::pair<uint64_t, uint64_t>{0, 0}, std::pair<uint64_t, uint64_t>{100, 100},
                    std::pair<uint64_t, uint64_t>{99, 100},
                    std::pair<uint64_t, uint64_t>{std::numeric_limits<uint64_t>::max(),
                                                  std::numeric_limits<uint64_t>::max()}}) {
        policy = approved;
        policy.deployment_budget_bytes = budget;
        policy.reserved_headroom_bytes = headroom;
        rejected = expect_outcome(&policy, complete, SIQSShadowProofRssGateStatus::invalid,
                                  SIQSShadowProofRssGateReason::policy_budget_not_above_headroom);
        CHECK(rejected.rss_limit_bytes == 0);
    }

    policy = approved;
    policy.deployment_budget_bytes = std::numeric_limits<uint64_t>::max();
    policy.reserved_headroom_bytes = std::numeric_limits<uint64_t>::max() - 1;
    auto samples = make_complete_samples(policy, 1);
    auto result =
        expect_outcome(&policy, samples, SIQSShadowProofRssGateStatus::manual_review_candidate,
                       SIQSShadowProofRssGateReason::all_observe_peaks_within_limit);
    CHECK(result.rss_limit_bytes == 1);
    CHECK(result.max_observe_peak_rss_bytes == 1);

    samples = make_complete_samples(policy, 2);
    result = expect_outcome(&policy, samples, SIQSShadowProofRssGateStatus::limit_exceeded,
                            SIQSShadowProofRssGateReason::observe_peak_over_limit);
    CHECK(result.rss_limit_bytes == 1);
    CHECK(result.max_observe_peak_rss_bytes == 2);

    policy = approved;
    policy.deployment_budget_bytes = std::numeric_limits<uint64_t>::max();
    policy.reserved_headroom_bytes = 0;
    samples = make_complete_samples(policy, std::numeric_limits<uint64_t>::max());
    result = expect_outcome(&policy, samples, SIQSShadowProofRssGateStatus::manual_review_candidate,
                            SIQSShadowProofRssGateReason::all_observe_peaks_within_limit);
    CHECK(result.rss_limit_bytes == std::numeric_limits<uint64_t>::max());
    CHECK(result.max_observe_peak_rss_bytes == std::numeric_limits<uint64_t>::max());
}

void test_policy_binding_and_tokens() {
    const auto approved = make_policy();
    const auto samples = make_complete_samples(approved, RSS_LIMIT_BYTES);
    const auto expect_invalid_policy = [&](const SIQSShadowProofRssGatePolicy& policy) {
        const auto outcome = expect_outcome(&policy, samples, SIQSShadowProofRssGateStatus::invalid,
                                            SIQSShadowProofRssGateReason::policy_binding_invalid);
        CHECK(outcome.rss_limit_bytes == RSS_LIMIT_BYTES);
    };

    auto policy = approved;
    policy.corpus_id = "wrong-corpus";
    expect_invalid_policy(policy);
    policy = approved;
    ++policy.corpus_digest.low;
    expect_invalid_policy(policy);
    policy = approved;
    ++policy.corpus_digest.high;
    expect_invalid_policy(policy);
    policy = approved;
    policy.corpus_digest = {};
    expect_invalid_policy(policy);

    policy = approved;
    policy.operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    expect_invalid_policy(policy);
    policy = approved;
    policy.operating_system = static_cast<SIQSShadowProofRssOperatingSystem>(255);
    expect_invalid_policy(policy);
    policy = approved;
    policy.architecture = SIQSShadowProofRssArchitecture::unknown;
    expect_invalid_policy(policy);
    policy = approved;
    policy.architecture = static_cast<SIQSShadowProofRssArchitecture>(255);
    expect_invalid_policy(policy);
    policy = approved;
    policy.memory_backend = ProcessMemoryBackend::Unsupported;
    expect_invalid_policy(policy);
    policy = approved;
    policy.memory_backend = static_cast<ProcessMemoryBackend>(255);
    expect_invalid_policy(policy);
    policy = approved;
    policy.resolved_production_sieve_workers = 0;
    expect_invalid_policy(policy);
    policy = approved;
    policy.journal_store.trusted_base_id = {};
    expect_invalid_policy(policy);
    policy = approved;
    policy.journal_store.store_id = {};
    expect_invalid_policy(policy);
    for (const std::string_view bad_locator : std::array<std::string_view, 18>{
             "", ".", "..", ".hidden", "trailing.", "two words", "child/path", "child\\path", "a=b",
             "Store", "A_b.c-9", "CON", "con.txt", "nul", "Com1", "lpt9", "LPT1.log",
             std::string_view("bad\x80", 4)}) {
        policy = approved;
        policy.journal_store.relative_locator = bad_locator;
        expect_invalid_policy(policy);
    }

    for (const auto [operating_system, backend] :
         std::array{std::pair{SIQSShadowProofRssOperatingSystem::darwin,
                              ProcessMemoryBackend::LinuxGetrusage},
                    std::pair{SIQSShadowProofRssOperatingSystem::linux,
                              ProcessMemoryBackend::WindowsPsapi},
                    std::pair{SIQSShadowProofRssOperatingSystem::windows,
                              ProcessMemoryBackend::DarwinGetrusage}}) {
        policy = approved;
        policy.operating_system = operating_system;
        policy.memory_backend = backend;
        expect_invalid_policy(policy);
    }

    for (const auto [operating_system, architecture, backend] :
         std::array{std::tuple{SIQSShadowProofRssOperatingSystem::darwin,
                               SIQSShadowProofRssArchitecture::arm64,
                               ProcessMemoryBackend::DarwinGetrusage},
                    std::tuple{SIQSShadowProofRssOperatingSystem::linux,
                               SIQSShadowProofRssArchitecture::x86_64,
                               ProcessMemoryBackend::LinuxGetrusage},
                    std::tuple{SIQSShadowProofRssOperatingSystem::windows,
                               SIQSShadowProofRssArchitecture::x86_64,
                               ProcessMemoryBackend::WindowsPsapi}}) {
        policy = approved;
        policy.operating_system = operating_system;
        policy.architecture = architecture;
        policy.memory_backend = backend;
        const auto matching = make_complete_samples(policy, RSS_LIMIT_BYTES);
        expect_outcome(&policy, matching, SIQSShadowProofRssGateStatus::manual_review_candidate,
                       SIQSShadowProofRssGateReason::all_observe_peaks_within_limit);
    }

    for (const std::string_view bad_token : std::array<std::string_view, 7>{
             "", "two words", "tab\tvalue", "line\nvalue", "a=b", std::string_view("bad\x7f", 4),
             std::string_view("bad\x80", 4)}) {
        policy = approved;
        policy.approval_id = bad_token;
        expect_invalid_policy(policy);
        policy = approved;
        policy.candidate_revision = bad_token;
        expect_invalid_policy(policy);
    }

    const std::string maximum_token(SIQS_SHADOW_PROOF_RSS_GATE_MAX_TOKEN_BYTES, 'x');
    policy = approved;
    policy.approval_id = maximum_token;
    auto matching = make_complete_samples(policy, RSS_LIMIT_BYTES);
    expect_outcome(&policy, matching, SIQSShadowProofRssGateStatus::manual_review_candidate,
                   SIQSShadowProofRssGateReason::all_observe_peaks_within_limit);

    const std::string oversized_token(SIQS_SHADOW_PROOF_RSS_GATE_MAX_TOKEN_BYTES + 1, 'x');
    policy = approved;
    policy.approval_id = oversized_token;
    expect_invalid_policy(policy);

    policy = approved;
    policy.journal_store.relative_locator = maximum_token;
    matching = make_complete_samples(policy, RSS_LIMIT_BYTES);
    expect_outcome(&policy, matching, SIQSShadowProofRssGateStatus::manual_review_candidate,
                   SIQSShadowProofRssGateReason::all_observe_peaks_within_limit);
    policy.journal_store.relative_locator = oversized_token;
    expect_invalid_policy(policy);

    for (const auto binding : std::array{gnfs::siqs::SIQSShadowProofRssJournalStoreBinding{
                                             {0, approved.journal_store.trusted_base_id.high},
                                             approved.journal_store.store_id,
                                             "a_b.c-9"},
                                         gnfs::siqs::SIQSShadowProofRssJournalStoreBinding{
                                             {approved.journal_store.trusted_base_id.low, 0},
                                             approved.journal_store.store_id,
                                             "a_b.c-9"},
                                         gnfs::siqs::SIQSShadowProofRssJournalStoreBinding{
                                             approved.journal_store.trusted_base_id,
                                             {0, approved.journal_store.store_id.high},
                                             "a_b.c-9"},
                                         gnfs::siqs::SIQSShadowProofRssJournalStoreBinding{
                                             approved.journal_store.trusted_base_id,
                                             {approved.journal_store.store_id.low, 0},
                                             "a_b.c-9"}}) {
        policy = approved;
        policy.journal_store = binding;
        matching = make_complete_samples(policy, RSS_LIMIT_BYTES);
        expect_outcome(&policy, matching, SIQSShadowProofRssGateStatus::manual_review_candidate,
                       SIQSShadowProofRssGateReason::all_observe_peaks_within_limit);
    }
}

void test_exact_coverage_and_sample_bindings() {
    const auto policy = make_policy();
    const auto complete = make_complete_samples(policy, RSS_LIMIT_BYTES);
    CHECK(complete.size() == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);

    const auto baseline =
        expect_outcome(&policy, complete, SIQSShadowProofRssGateStatus::manual_review_candidate,
                       SIQSShadowProofRssGateReason::all_observe_peaks_within_limit);
    CHECK(baseline.total_sample_count == 80);
    CHECK(baseline.valid_off_sample_count == 24);
    CHECK(baseline.valid_observe_sample_count == 56);
    CHECK(baseline.rss_limit_bytes == RSS_LIMIT_BYTES);
    CHECK(baseline.max_observe_peak_rss_bytes == RSS_LIMIT_BYTES);

    auto changed = complete;
    changed.pop_back();
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_count_invalid);

    changed = complete;
    changed.push_back(changed.front());
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_count_invalid);

    changed = complete;
    changed.back() = changed.front();
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_duplicate);

    changed = complete;
    std::reverse(changed.begin(), changed.end());
    CHECK(evaluate_closed(&policy, changed) == baseline);

    const auto expect_binding_mismatch = [&](const auto& mutate) {
        auto mismatched = complete;
        mutate(mismatched.back());
        expect_outcome(&policy, mismatched, SIQSShadowProofRssGateStatus::invalid,
                       SIQSShadowProofRssGateReason::sample_binding_mismatch);
    };
    expect_binding_mismatch([](auto& sample) { sample.policy_approved = false; });
    expect_binding_mismatch([](auto& sample) { sample.corpus_id = "wrong-corpus"; });
    expect_binding_mismatch([](auto& sample) { ++sample.corpus_digest.low; });
    expect_binding_mismatch([](auto& sample) { ++sample.corpus_digest.high; });
    expect_binding_mismatch(
        [](auto& sample) { sample.operating_system = SIQSShadowProofRssOperatingSystem::linux; });
    expect_binding_mismatch(
        [](auto& sample) { sample.architecture = SIQSShadowProofRssArchitecture::x86_64; });
    expect_binding_mismatch(
        [](auto& sample) { sample.memory_backend = ProcessMemoryBackend::LinuxGetrusage; });
    expect_binding_mismatch([](auto& sample) { ++sample.resolved_production_sieve_workers; });
    expect_binding_mismatch([](auto& sample) { sample.candidate_revision = "different-revision"; });
    expect_binding_mismatch([](auto& sample) { sample.approval_id = "different-approval"; });
    expect_binding_mismatch([](auto& sample) { ++sample.journal_store.trusted_base_id.low; });
    expect_binding_mismatch([](auto& sample) { ++sample.journal_store.trusted_base_id.high; });
    expect_binding_mismatch([](auto& sample) { ++sample.journal_store.store_id.low; });
    expect_binding_mismatch([](auto& sample) { ++sample.journal_store.store_id.high; });
    expect_binding_mismatch(
        [](auto& sample) { sample.journal_store.relative_locator = "different-store"; });
    expect_binding_mismatch([](auto& sample) { sample.deployment_budget_bytes = 1001; });
    expect_binding_mismatch([](auto& sample) { sample.reserved_headroom_bytes = 101; });
}

void test_sample_enums_ordinals_and_execution() {
    const auto policy = make_policy();
    const auto complete = make_complete_samples(policy, RSS_LIMIT_BYTES);
    const size_t off_index = find_first_mode(complete, SIQSShadowProofRssSampleMode::off);
    const size_t observe_index = find_first_mode(complete, SIQSShadowProofRssSampleMode::observe);

    const auto expect_sample_enum_invalid = [&](const auto& mutate) {
        auto changed = complete;
        mutate(changed.back());
        expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                       SIQSShadowProofRssGateReason::sample_enum_invalid);
    };
    expect_sample_enum_invalid(
        [](auto& sample) { sample.mode = SIQSShadowProofRssSampleMode::unknown; });
    expect_sample_enum_invalid(
        [](auto& sample) { sample.mode = static_cast<SIQSShadowProofRssSampleMode>(255); });
    expect_sample_enum_invalid(
        [](auto& sample) { sample.proof_evidence = SIQSShadowProofRssEvidence::unknown; });
    expect_sample_enum_invalid([](auto& sample) {
        sample.matrix_evidence = static_cast<SIQSShadowProofRssEvidence>(255);
    });
    expect_sample_enum_invalid(
        [](auto& sample) { sample.factor_identity = SIQSShadowProofRssFactorIdentity::unknown; });
    expect_sample_enum_invalid([](auto& sample) {
        sample.factor_identity = static_cast<SIQSShadowProofRssFactorIdentity>(255);
    });
    expect_sample_enum_invalid(
        [](auto& sample) { sample.operating_system = SIQSShadowProofRssOperatingSystem::unknown; });
    expect_sample_enum_invalid(
        [](auto& sample) { sample.architecture = SIQSShadowProofRssArchitecture::unknown; });
    expect_sample_enum_invalid(
        [](auto& sample) { sample.memory_backend = static_cast<ProcessMemoryBackend>(255); });

    auto changed = complete;
    changed[off_index].fixture_id = 0;
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_fixture_out_of_range);
    changed = complete;
    changed[off_index].fixture_id = SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT + 1;
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_fixture_out_of_range);

    changed = complete;
    changed[off_index].ordinal = 0;
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_ordinal_out_of_range);
    changed = complete;
    changed[off_index].ordinal = SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS + 1;
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_ordinal_out_of_range);
    changed = complete;
    changed[observe_index].ordinal = 0;
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_ordinal_out_of_range);
    changed = complete;
    changed[observe_index].ordinal = SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS + 1;
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_ordinal_out_of_range);

    changed = complete;
    changed.back().fresh_process = false;
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_execution_invalid);
    changed = complete;
    changed.back().completed = false;
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_execution_invalid);

    for (const auto identity :
         {SIQSShadowProofRssFactorIdentity::fail, SIQSShadowProofRssFactorIdentity::not_checked}) {
        changed = complete;
        changed.back().factor_identity = identity;
        expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                       SIQSShadowProofRssGateReason::sample_factor_identity_invalid);
    }
}

void test_observe_evidence_and_diagnostic_independence() {
    const auto policy = make_policy();
    const auto complete = make_complete_samples(policy, RSS_LIMIT_BYTES);
    const size_t off_index = find_first_mode(complete, SIQSShadowProofRssSampleMode::off);
    const size_t observe_index = find_first_mode(complete, SIQSShadowProofRssSampleMode::observe);

    for (const auto evidence :
         {SIQSShadowProofRssEvidence::not_applicable, SIQSShadowProofRssEvidence::fail}) {
        auto changed = complete;
        changed[observe_index].proof_evidence = evidence;
        expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                       SIQSShadowProofRssGateReason::observe_evidence_invalid);
        changed = complete;
        changed[observe_index].matrix_evidence = evidence;
        expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                       SIQSShadowProofRssGateReason::observe_evidence_invalid);
    }

    auto changed = complete;
    changed[observe_index].absolute_peak_rss_bytes.reset();
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::observe_peak_missing);
    changed = complete;
    changed[observe_index].absolute_peak_rss_bytes = 0;
    expect_outcome(&policy, changed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::observe_peak_zero);

    for (const auto& sample : complete) {
        if (sample.mode == SIQSShadowProofRssSampleMode::off) {
            CHECK(!sample.absolute_peak_rss_bytes.has_value());
        }
    }
    const auto baseline = evaluate_closed(&policy, complete);

    for (const auto evidence :
         {SIQSShadowProofRssEvidence::not_applicable, SIQSShadowProofRssEvidence::pass,
          SIQSShadowProofRssEvidence::fail}) {
        changed = complete;
        changed[off_index].proof_evidence = evidence;
        changed[off_index].matrix_evidence = evidence;
        CHECK(evaluate_closed(&policy, changed) == baseline);
    }

    changed = complete;
    for (auto& sample : changed) {
        sample.observe_minus_off_peak_bytes = std::numeric_limits<int64_t>::min();
        sample.current_rss_bytes = std::numeric_limits<uint64_t>::max();
        sample.peak_growth_bytes = std::numeric_limits<uint64_t>::max();
        sample.wall_ns = std::numeric_limits<uint64_t>::max();
        if (sample.mode == SIQSShadowProofRssSampleMode::off) {
            sample.absolute_peak_rss_bytes = std::numeric_limits<uint64_t>::max();
        }
    }
    CHECK(evaluate_closed(&policy, changed) == baseline);

    changed = complete;
    for (auto& sample : changed) {
        sample.observe_minus_off_peak_bytes.reset();
        sample.current_rss_bytes.reset();
        sample.peak_growth_bytes.reset();
        sample.wall_ns.reset();
        if (sample.mode == SIQSShadowProofRssSampleMode::off) {
            sample.absolute_peak_rss_bytes.reset();
        }
    }
    CHECK(evaluate_closed(&policy, changed) == baseline);
}

void test_inclusive_limit_and_validation_precedence() {
    const auto policy = make_policy();
    auto samples = make_complete_samples(policy, RSS_LIMIT_BYTES);
    auto result =
        expect_outcome(&policy, samples, SIQSShadowProofRssGateStatus::manual_review_candidate,
                       SIQSShadowProofRssGateReason::all_observe_peaks_within_limit);
    CHECK(result.max_observe_peak_rss_bytes == result.rss_limit_bytes);

    samples = make_complete_samples(policy, RSS_LIMIT_BYTES + 1);
    result = expect_outcome(&policy, samples, SIQSShadowProofRssGateStatus::limit_exceeded,
                            SIQSShadowProofRssGateReason::observe_peak_over_limit);
    CHECK(result.max_observe_peak_rss_bytes == result.rss_limit_bytes + 1);

    const size_t observe_index = find_first_mode(samples, SIQSShadowProofRssSampleMode::observe);
    CHECK(*samples[observe_index].absolute_peak_rss_bytes > RSS_LIMIT_BYTES);

    auto malformed = samples;
    malformed.back() = malformed.front();
    expect_outcome(&policy, malformed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_duplicate);

    malformed = samples;
    malformed.back().candidate_revision = "mixed-revision";
    expect_outcome(&policy, malformed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_binding_mismatch);

    malformed = samples;
    malformed.pop_back();
    expect_outcome(&policy, malformed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_count_invalid);

    malformed = samples;
    malformed.back().mode = SIQSShadowProofRssSampleMode::unknown;
    expect_outcome(&policy, malformed, SIQSShadowProofRssGateStatus::invalid,
                   SIQSShadowProofRssGateReason::sample_enum_invalid);
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
        const size_t bytes_read = std::fread(output.data(), 1, output.size(), file);
        output.resize(bytes_read);
    }
    return output;
}

[[nodiscard]] std::pair<bool, std::string>
emit_to_string(const SIQSShadowProofRssGatePolicy* policy,
               const std::vector<SIQSShadowProofRssGateSample>& samples,
               const SIQSShadowProofRssGateOutcome& outcome) {
    std::FILE* file = std::tmpfile();
    CHECK(file != nullptr);
    if (file == nullptr) {
        return {false, {}};
    }
    const bool emitted =
        emit_siqs_shadow_proof_rss_gate_outcome(file, policy, sample_span(samples), outcome);
    std::string output = read_stream(file);
    CHECK(std::fclose(file) == 0);
    return {emitted, std::move(output)};
}

void expect_emit_rejected(const SIQSShadowProofRssGatePolicy* policy,
                          const std::vector<SIQSShadowProofRssGateSample>& samples,
                          const SIQSShadowProofRssGateOutcome& outcome) {
    const auto [emitted, output] = emit_to_string(policy, samples, outcome);
    CHECK(!emitted);
    CHECK(output.empty());
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::FILE* open_read_only(const std::filesystem::path& path) {
#if defined(_WIN32)
    return ::_wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

void test_closed_emitter_schema_and_failures() {
    auto policy = make_policy();
    auto samples = make_complete_samples(policy, RSS_LIMIT_BYTES);
    const auto outcome = evaluate_closed(&policy, samples);
    const auto [emitted, line] = emit_to_string(&policy, samples, outcome);
    CHECK(emitted);
    const std::string expected_line =
        "GNFS_SIQS_SHADOW_PROOF_RSS_GATE_V2 schema_version=2"
        " status=manual_review_candidate reason=all_observe_peaks_within_limit"
        " corpus_id=siqs50_shadow_observe_rss_holdout_v1"
        " digest_low=303806906129662515 digest_high=18179245792498443738"
        " operating_system=darwin architecture=arm64 memory_backend=darwin_getrusage"
        " resolved_production_sieve_workers=4 candidate_revision=candidate-revision-1"
        " approval_id=approval-ticket-1 journal_trusted_base_id_low=1010101010101010"
        " journal_trusted_base_id_high=2020202020202020"
        " journal_store_id_low=1111222233334444"
        " journal_store_id_high=5555666677778888"
        " journal_store_locator=rss-campaign-prod-v1 deployment_budget_bytes=1000"
        " reserved_headroom_bytes=100 rss_limit_bytes=900 total_samples=80 off_samples=24"
        " observe_samples=56 max_observe_peak_rss_bytes=900"
        " policy_binding_digest_low=1693149446838404574"
        " policy_binding_digest_high=13930391788833022626"
        " gate_quantity=observe_absolute_process_peak_rss"
        " shadow_outcome_routed=false promotion=false\n";
    CHECK(line == expected_line);
    CHECK(std::count(line.begin(), line.end(), '\n') == 1);
    CHECK(line.find("shadow_outcome_routed=false promotion=false") != std::string::npos);
    CHECK(outcome.policy_binding_digest.low == UINT64_C(1693149446838404574));
    CHECK(outcome.policy_binding_digest.high == UINT64_C(13930391788833022626));

    CHECK(
        !emit_siqs_shadow_proof_rss_gate_outcome(nullptr, &policy, sample_span(samples), outcome));
    expect_emit_rejected(nullptr, samples, outcome);

    const auto expect_outcome_mutation_rejected = [&](const auto& mutate) {
        auto forged = outcome;
        mutate(forged);
        expect_emit_rejected(&policy, samples, forged);
    };
    expect_outcome_mutation_rejected(
        [](auto& value) { value.status = SIQSShadowProofRssGateStatus::limit_exceeded; });
    expect_outcome_mutation_rejected(
        [](auto& value) { value.reason = SIQSShadowProofRssGateReason::internal_failure; });
    expect_outcome_mutation_rejected([](auto& value) { --value.total_sample_count; });
    expect_outcome_mutation_rejected([](auto& value) { --value.valid_off_sample_count; });
    expect_outcome_mutation_rejected([](auto& value) { --value.valid_observe_sample_count; });
    expect_outcome_mutation_rejected([](auto& value) { --value.rss_limit_bytes; });
    expect_outcome_mutation_rejected([](auto& value) { --value.max_observe_peak_rss_bytes; });
    expect_outcome_mutation_rejected([](auto& value) { ++value.policy_binding_digest.low; });
    expect_outcome_mutation_rejected([](auto& value) { ++value.policy_binding_digest.high; });
    expect_outcome_mutation_rejected([](auto& value) { value.shadow_outcome_routed = true; });
    expect_outcome_mutation_rejected([](auto& value) { value.promotion = true; });

    auto changed_samples = samples;
    changed_samples.back().approval_id = "forged-approval";
    expect_emit_rejected(&policy, changed_samples, outcome);
    changed_samples = samples;
    changed_samples.pop_back();
    expect_emit_rejected(&policy, changed_samples, outcome);
    changed_samples = samples;
    changed_samples.back() = changed_samples.front();
    expect_emit_rejected(&policy, changed_samples, outcome);
    changed_samples = samples;
    changed_samples[find_first_mode(changed_samples, SIQSShadowProofRssSampleMode::observe)]
        .absolute_peak_rss_bytes = RSS_LIMIT_BYTES + 1;
    expect_emit_rejected(&policy, changed_samples, outcome);

    auto changed_policy = policy;
    changed_policy.approval_id = "approval-ticket-2";
    auto matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    expect_emit_rejected(&changed_policy, matching_changed_samples, outcome);
    changed_policy = policy;
    changed_policy.candidate_revision = "candidate-revision-2";
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    expect_emit_rejected(&changed_policy, matching_changed_samples, outcome);
    changed_policy = policy;
    ++changed_policy.journal_store.store_id.low;
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    expect_emit_rejected(&changed_policy, matching_changed_samples, outcome);
    changed_policy = policy;
    changed_policy.architecture = SIQSShadowProofRssArchitecture::x86_64;
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    expect_emit_rejected(&changed_policy, matching_changed_samples, outcome);
    changed_policy = policy;
    changed_policy.operating_system = SIQSShadowProofRssOperatingSystem::linux;
    changed_policy.memory_backend = ProcessMemoryBackend::LinuxGetrusage;
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    expect_emit_rejected(&changed_policy, matching_changed_samples, outcome);
    changed_policy = policy;
    ++changed_policy.resolved_production_sieve_workers;
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    expect_emit_rejected(&changed_policy, matching_changed_samples, outcome);
    changed_policy = policy;
    ++*changed_policy.deployment_budget_bytes;
    ++*changed_policy.reserved_headroom_bytes;
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    expect_emit_rejected(&changed_policy, matching_changed_samples, outcome);
    changed_policy = policy;
    changed_policy.corpus_id = "forged-corpus";
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    expect_emit_rejected(&changed_policy, matching_changed_samples, outcome);
    changed_policy = policy;
    changed_policy.approved = false;
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    expect_emit_rejected(&changed_policy, matching_changed_samples, outcome);

    const std::string maximum_token(SIQS_SHADOW_PROOF_RSS_GATE_MAX_TOKEN_BYTES, 'z');
    changed_policy = policy;
    changed_policy.approval_id = maximum_token;
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    const auto maximum_token_outcome = evaluate_closed(&changed_policy, matching_changed_samples);
    const auto [maximum_token_emitted, maximum_token_line] =
        emit_to_string(&changed_policy, matching_changed_samples, maximum_token_outcome);
    CHECK(maximum_token_emitted);
    CHECK(maximum_token_line.find(" approval_id=" + maximum_token + " ") != std::string::npos);

    const std::string oversized_token(SIQS_SHADOW_PROOF_RSS_GATE_MAX_TOKEN_BYTES + 1, 'z');
    changed_policy = policy;
    changed_policy.approval_id = oversized_token;
    matching_changed_samples = make_complete_samples(changed_policy, RSS_LIMIT_BYTES);
    const auto oversized_token_outcome = evaluate_closed(&changed_policy, matching_changed_samples);
    CHECK(oversized_token_outcome.status == SIQSShadowProofRssGateStatus::invalid);
    CHECK(oversized_token_outcome.reason == SIQSShadowProofRssGateReason::policy_binding_invalid);
    expect_emit_rejected(&changed_policy, matching_changed_samples, oversized_token_outcome);

    auto invalid_samples = samples;
    invalid_samples.pop_back();
    const auto invalid_outcome = evaluate_closed(&policy, invalid_samples);
    CHECK(invalid_outcome.status == SIQSShadowProofRssGateStatus::invalid);
    expect_emit_rejected(&policy, invalid_samples, invalid_outcome);

    const auto missing_policy_outcome = evaluate_closed(nullptr, samples);
    CHECK(missing_policy_outcome.status == SIQSShadowProofRssGateStatus::blocked);
    expect_emit_rejected(nullptr, samples, missing_policy_outcome);

    auto blocked_policy = policy;
    blocked_policy.approved = false;
    auto blocked_samples = make_complete_samples(blocked_policy, RSS_LIMIT_BYTES);
    auto blocked_outcome = evaluate_closed(&blocked_policy, blocked_samples);
    CHECK(blocked_outcome.status == SIQSShadowProofRssGateStatus::blocked);
    CHECK(blocked_outcome.reason == SIQSShadowProofRssGateReason::policy_not_approved);
    expect_emit_rejected(&blocked_policy, blocked_samples, blocked_outcome);

    blocked_policy = policy;
    blocked_policy.deployment_budget_bytes.reset();
    blocked_samples = make_complete_samples(blocked_policy, RSS_LIMIT_BYTES);
    blocked_outcome = evaluate_closed(&blocked_policy, blocked_samples);
    CHECK(blocked_outcome.status == SIQSShadowProofRssGateStatus::blocked);
    CHECK(blocked_outcome.reason == SIQSShadowProofRssGateReason::policy_budget_missing);
    expect_emit_rejected(&blocked_policy, blocked_samples, blocked_outcome);

    blocked_policy = policy;
    blocked_policy.reserved_headroom_bytes.reset();
    blocked_samples = make_complete_samples(blocked_policy, RSS_LIMIT_BYTES);
    blocked_outcome = evaluate_closed(&blocked_policy, blocked_samples);
    CHECK(blocked_outcome.status == SIQSShadowProofRssGateStatus::blocked);
    CHECK(blocked_outcome.reason == SIQSShadowProofRssGateReason::policy_headroom_missing);
    expect_emit_rejected(&blocked_policy, blocked_samples, blocked_outcome);

    changed_samples = samples;
    for (auto& sample : changed_samples) {
        sample.observe_minus_off_peak_bytes = std::numeric_limits<int64_t>::max();
        sample.current_rss_bytes = std::numeric_limits<uint64_t>::max();
        sample.peak_growth_bytes.reset();
        sample.wall_ns = 0;
        if (sample.mode == SIQSShadowProofRssSampleMode::off) {
            sample.absolute_peak_rss_bytes = std::numeric_limits<uint64_t>::max();
        }
    }
    const auto [diagnostics_emitted, diagnostics_line] =
        emit_to_string(&policy, changed_samples, outcome);
    CHECK(diagnostics_emitted);
    CHECK(diagnostics_line == line);

    const auto over_limit_samples = make_complete_samples(policy, RSS_LIMIT_BYTES + 1);
    const auto over_limit = evaluate_closed(&policy, over_limit_samples);
    const auto [over_limit_emitted, over_limit_line] =
        emit_to_string(&policy, over_limit_samples, over_limit);
    CHECK(over_limit_emitted);
    CHECK(over_limit_line.find("status=limit_exceeded reason=observe_peak_over_limit") !=
          std::string::npos);
    CHECK(over_limit_line.find("shadow_outcome_routed=false promotion=false") != std::string::npos);

    try {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("gnfs-siqs-rss-gate-read-only-" + std::to_string(nonce) + ".tmp");
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "sentinel";
            CHECK(output.good());
        }
        std::FILE* read_only = open_read_only(path);
        CHECK(read_only != nullptr);
        if (read_only != nullptr) {
            CHECK(!emit_siqs_shadow_proof_rss_gate_outcome(read_only, &policy, sample_span(samples),
                                                           outcome));
            CHECK(std::fclose(read_only) == 0);
        }
        CHECK(read_file(path) == "sentinel");

        read_only = open_read_only(path);
        CHECK(read_only != nullptr);
        if (read_only != nullptr) {
            CHECK(std::fputc('x', read_only) == EOF);
            CHECK(std::ferror(read_only) != 0);
            CHECK(!emit_siqs_shadow_proof_rss_gate_outcome(read_only, &policy, sample_span(samples),
                                                           outcome));
            CHECK(std::fclose(read_only) == 0);
        }
        CHECK(read_file(path) == "sentinel");
        std::error_code error;
        CHECK(std::filesystem::remove(path, error));
        CHECK(!error);
    } catch (...) {
        CHECK(false);
    }
}

} // namespace

int main() {
    test_frozen_contract_and_enum_names();
    test_policy_and_budget_boundaries();
    test_policy_binding_and_tokens();
    test_exact_coverage_and_sample_bindings();
    test_sample_enums_ordinals_and_execution();
    test_observe_evidence_and_diagnostic_independence();
    test_inclusive_limit_and_validation_precedence();
    test_closed_emitter_schema_and_failures();

    std::cout << "SIQS shadow proof RSS gate: " << checks_passed << " passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
