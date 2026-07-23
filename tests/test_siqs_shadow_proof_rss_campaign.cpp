// Pure contract tests for deterministic SIQS shadow-proof RSS campaign planning.
// This test never invokes SIQS, captures memory, reads fixtures, or launches a process.

#include <gnfs/siqs/shadow_proof_rss_campaign.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;

using gnfs::siqs::make_siqs_shadow_proof_rss_campaign_plan;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_CAMPAIGN_MAX_CONCURRENCY;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_HIGH;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_DIGEST_LOW;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_CORPUS_ID;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
using gnfs::siqs::SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS;
using gnfs::siqs::SIQSShadowProofRssArchitecture;
using gnfs::siqs::SIQSShadowProofRssCampaignPlan;
using gnfs::siqs::SIQSShadowProofRssCampaignPlanReason;
using gnfs::siqs::SIQSShadowProofRssCampaignPlanStatus;
using gnfs::siqs::SIQSShadowProofRssCampaignSlot;
using gnfs::siqs::SIQSShadowProofRssGatePolicy;
using gnfs::siqs::SIQSShadowProofRssOperatingSystem;
using gnfs::siqs::SIQSShadowProofRssSampleMode;
using gnfs::util::ProcessMemoryBackend;

static_assert(SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT == 8);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS == 3);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS == 7);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT == 80);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_MAX_CONCURRENCY == 1);
static_assert(SIQSShadowProofRssCampaignPlan::max_concurrency == 1);
static_assert(std::tuple_size_v<decltype(SIQSShadowProofRssCampaignPlan{}.slots)> == 80);
static_assert(std::is_same_v<decltype(make_siqs_shadow_proof_rss_campaign_plan(
                                 static_cast<const SIQSShadowProofRssGatePolicy*>(nullptr))),
                             SIQSShadowProofRssCampaignPlan>);
static_assert(noexcept(make_siqs_shadow_proof_rss_campaign_plan(
    static_cast<const SIQSShadowProofRssGatePolicy*>(nullptr))));

constexpr uint64_t DEPLOYMENT_BUDGET_BYTES = UINT64_C(1000);
constexpr uint64_t RESERVED_HEADROOM_BYTES = UINT64_C(100);
constexpr uint64_t EXPECTED_POLICY_DIGEST_LOW = UINT64_C(1693149446838404574);
constexpr uint64_t EXPECTED_POLICY_DIGEST_HIGH = UINT64_C(13930391788833022626);

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

[[nodiscard]] constexpr SIQSShadowProofRssGatePolicy make_policy() noexcept {
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

[[nodiscard]] constexpr bool constexpr_campaign_contract() noexcept {
    const SIQSShadowProofRssGatePolicy policy = make_policy();
    const SIQSShadowProofRssCampaignPlan plan = make_siqs_shadow_proof_rss_campaign_plan(&policy);
    return plan.status == SIQSShadowProofRssCampaignPlanStatus::ready &&
           plan.reason == SIQSShadowProofRssCampaignPlanReason::ready &&
           plan.slot_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT &&
           plan.slots[0].slot_number == 1 && plan.slots[0].fixture_id == 1 &&
           plan.slots[0].mode == SIQSShadowProofRssSampleMode::off && plan.slots[0].ordinal == 1 &&
           plan.slots[3].slot_number == 4 && plan.slots[3].fixture_id == 1 &&
           plan.slots[3].mode == SIQSShadowProofRssSampleMode::observe &&
           plan.slots[3].ordinal == 1 && plan.slots[79].slot_number == 80 &&
           plan.slots[79].fixture_id == 8 &&
           plan.slots[79].mode == SIQSShadowProofRssSampleMode::observe &&
           plan.slots[79].ordinal == 7;
}

static_assert(constexpr_campaign_contract());

void expect_empty_plan(const SIQSShadowProofRssCampaignPlan& plan,
                       SIQSShadowProofRssCampaignPlanStatus expected_status,
                       SIQSShadowProofRssCampaignPlanReason expected_reason) {
    CHECK(plan.status == expected_status);
    CHECK(plan.reason == expected_reason);
    CHECK(plan.slot_count == 0);
    for (const auto& slot : plan.slots) {
        CHECK(slot == SIQSShadowProofRssCampaignSlot{});
    }
}

void expect_invalid_binding(const SIQSShadowProofRssGatePolicy& policy) {
    expect_empty_plan(make_siqs_shadow_proof_rss_campaign_plan(&policy),
                      SIQSShadowProofRssCampaignPlanStatus::invalid,
                      SIQSShadowProofRssCampaignPlanReason::policy_binding_invalid);
}

[[nodiscard]] bool slot_binds_policy(const SIQSShadowProofRssCampaignSlot& slot,
                                     const SIQSShadowProofRssGatePolicy& policy) noexcept {
    return slot.policy_approved == policy.approved && slot.corpus_id == policy.corpus_id &&
           slot.corpus_digest == policy.corpus_digest &&
           slot.operating_system == policy.operating_system &&
           slot.architecture == policy.architecture &&
           slot.memory_backend == policy.memory_backend &&
           slot.resolved_production_sieve_workers == policy.resolved_production_sieve_workers &&
           slot.candidate_revision == policy.candidate_revision &&
           slot.approval_id == policy.approval_id && slot.journal_store == policy.journal_store &&
           slot.deployment_budget_bytes == policy.deployment_budget_bytes &&
           slot.reserved_headroom_bytes == policy.reserved_headroom_bytes;
}

void test_policy_gates() {
    expect_empty_plan(make_siqs_shadow_proof_rss_campaign_plan(nullptr),
                      SIQSShadowProofRssCampaignPlanStatus::blocked,
                      SIQSShadowProofRssCampaignPlanReason::policy_missing);

    const auto approved = make_policy();
    auto policy = approved;
    policy.approved = false;
    expect_empty_plan(make_siqs_shadow_proof_rss_campaign_plan(&policy),
                      SIQSShadowProofRssCampaignPlanStatus::blocked,
                      SIQSShadowProofRssCampaignPlanReason::policy_not_approved);

    policy = approved;
    policy.deployment_budget_bytes.reset();
    expect_empty_plan(make_siqs_shadow_proof_rss_campaign_plan(&policy),
                      SIQSShadowProofRssCampaignPlanStatus::blocked,
                      SIQSShadowProofRssCampaignPlanReason::policy_budget_missing);

    policy = approved;
    policy.reserved_headroom_bytes.reset();
    expect_empty_plan(make_siqs_shadow_proof_rss_campaign_plan(&policy),
                      SIQSShadowProofRssCampaignPlanStatus::blocked,
                      SIQSShadowProofRssCampaignPlanReason::policy_headroom_missing);

    policy = approved;
    policy.deployment_budget_bytes = RESERVED_HEADROOM_BYTES;
    expect_empty_plan(make_siqs_shadow_proof_rss_campaign_plan(&policy),
                      SIQSShadowProofRssCampaignPlanStatus::invalid,
                      SIQSShadowProofRssCampaignPlanReason::policy_budget_not_above_headroom);

    policy = approved;
    policy.deployment_budget_bytes = RESERVED_HEADROOM_BYTES - 1;
    expect_empty_plan(make_siqs_shadow_proof_rss_campaign_plan(&policy),
                      SIQSShadowProofRssCampaignPlanStatus::invalid,
                      SIQSShadowProofRssCampaignPlanReason::policy_budget_not_above_headroom);

    policy = approved;
    policy.deployment_budget_bytes = 0;
    policy.reserved_headroom_bytes = 0;
    expect_empty_plan(make_siqs_shadow_proof_rss_campaign_plan(&policy),
                      SIQSShadowProofRssCampaignPlanStatus::invalid,
                      SIQSShadowProofRssCampaignPlanReason::policy_budget_not_above_headroom);
}

void test_invalid_policy_bindings() {
    const auto approved = make_policy();
    auto policy = approved;
    policy.corpus_id = "wrong-corpus";
    expect_invalid_binding(policy);
    policy = approved;
    ++policy.corpus_digest.low;
    expect_invalid_binding(policy);
    policy = approved;
    ++policy.corpus_digest.high;
    expect_invalid_binding(policy);
    policy = approved;
    policy.operating_system = SIQSShadowProofRssOperatingSystem::unknown;
    expect_invalid_binding(policy);
    policy = approved;
    policy.operating_system = static_cast<SIQSShadowProofRssOperatingSystem>(255);
    expect_invalid_binding(policy);
    policy = approved;
    policy.architecture = SIQSShadowProofRssArchitecture::unknown;
    expect_invalid_binding(policy);
    policy = approved;
    policy.architecture = static_cast<SIQSShadowProofRssArchitecture>(255);
    expect_invalid_binding(policy);
    policy = approved;
    policy.memory_backend = ProcessMemoryBackend::Unsupported;
    expect_invalid_binding(policy);
    policy = approved;
    policy.memory_backend = ProcessMemoryBackend::LinuxGetrusage;
    expect_invalid_binding(policy);
    policy = approved;
    policy.memory_backend = static_cast<ProcessMemoryBackend>(255);
    expect_invalid_binding(policy);
    policy = approved;
    policy.resolved_production_sieve_workers = 0;
    expect_invalid_binding(policy);
    policy = approved;
    policy.candidate_revision = {};
    expect_invalid_binding(policy);
    policy = approved;
    policy.candidate_revision = "unsafe revision";
    expect_invalid_binding(policy);
    policy = approved;
    policy.approval_id = {};
    expect_invalid_binding(policy);
    policy = approved;
    policy.approval_id = "unsafe=approval";
    expect_invalid_binding(policy);
}

void test_ready_plan_shape_and_binding() {
    const SIQSShadowProofRssGatePolicy policy = make_policy();
    const SIQSShadowProofRssCampaignPlan plan = make_siqs_shadow_proof_rss_campaign_plan(&policy);
    CHECK(plan == make_siqs_shadow_proof_rss_campaign_plan(&policy));
    CHECK(plan.status == SIQSShadowProofRssCampaignPlanStatus::ready);
    CHECK(plan.reason == SIQSShadowProofRssCampaignPlanReason::ready);
    CHECK(plan.slot_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);

    std::array<bool, SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT> occupied{};
    uint32_t off_count = 0;
    uint32_t observe_count = 0;
    size_t expected_index = 0;
    for (uint32_t fixture_id = 1; fixture_id <= SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT;
         ++fixture_id) {
        for (uint32_t ordinal = 1; ordinal <= SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS;
             ++ordinal) {
            const auto& slot = plan.slots[expected_index];
            CHECK(slot.slot_number == expected_index + 1);
            CHECK(slot.fixture_id == fixture_id);
            CHECK(slot.mode == SIQSShadowProofRssSampleMode::off);
            CHECK(slot.ordinal == ordinal);
            CHECK(slot_binds_policy(slot, policy));
            CHECK(slot.policy_binding_digest.low == EXPECTED_POLICY_DIGEST_LOW);
            CHECK(slot.policy_binding_digest.high == EXPECTED_POLICY_DIGEST_HIGH);
            CHECK(!occupied[expected_index]);
            occupied[expected_index] = true;
            ++off_count;
            ++expected_index;
        }
        for (uint32_t ordinal = 1; ordinal <= SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
             ++ordinal) {
            const auto& slot = plan.slots[expected_index];
            CHECK(slot.slot_number == expected_index + 1);
            CHECK(slot.fixture_id == fixture_id);
            CHECK(slot.mode == SIQSShadowProofRssSampleMode::observe);
            CHECK(slot.ordinal == ordinal);
            CHECK(slot_binds_policy(slot, policy));
            CHECK(slot.policy_binding_digest.low == EXPECTED_POLICY_DIGEST_LOW);
            CHECK(slot.policy_binding_digest.high == EXPECTED_POLICY_DIGEST_HIGH);
            CHECK(!occupied[expected_index]);
            occupied[expected_index] = true;
            ++observe_count;
            ++expected_index;
        }
    }
    CHECK(expected_index == plan.slots.size());
    CHECK(off_count == 24);
    CHECK(observe_count == 56);
    for (const bool present : occupied) {
        CHECK(present);
    }
}

void test_policy_digest_sensitivity_and_typed_platform_independence() {
    const SIQSShadowProofRssGatePolicy baseline_policy = make_policy();
    const SIQSShadowProofRssCampaignPlan baseline =
        make_siqs_shadow_proof_rss_campaign_plan(&baseline_policy);
    const auto expect_distinct_ready_plan = [&](const SIQSShadowProofRssGatePolicy& policy) {
        const SIQSShadowProofRssCampaignPlan plan =
            make_siqs_shadow_proof_rss_campaign_plan(&policy);
        CHECK(plan.status == SIQSShadowProofRssCampaignPlanStatus::ready);
        CHECK(plan.reason == SIQSShadowProofRssCampaignPlanReason::ready);
        CHECK(plan.slot_count == 80);
        CHECK(plan.slots[0].policy_binding_digest != baseline.slots[0].policy_binding_digest);
        for (const auto& slot : plan.slots) {
            CHECK(slot_binds_policy(slot, policy));
            CHECK(slot.policy_binding_digest == plan.slots[0].policy_binding_digest);
        }
    };

    auto policy = baseline_policy;
    policy.candidate_revision = "candidate-revision-2";
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    policy.approval_id = "approval-ticket-2";
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    ++policy.journal_store.trusted_base_id.low;
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    ++policy.journal_store.trusted_base_id.high;
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    ++policy.journal_store.store_id.low;
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    ++policy.journal_store.store_id.high;
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    policy.journal_store.relative_locator = "rss-campaign-prod-v2";
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    policy.architecture = SIQSShadowProofRssArchitecture::x86_64;
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    policy.resolved_production_sieve_workers = 5;
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    policy.deployment_budget_bytes = DEPLOYMENT_BUDGET_BYTES + 1;
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    policy.reserved_headroom_bytes = RESERVED_HEADROOM_BYTES + 1;
    expect_distinct_ready_plan(policy);

    policy = baseline_policy;
    policy.operating_system = SIQSShadowProofRssOperatingSystem::linux;
    policy.memory_backend = ProcessMemoryBackend::LinuxGetrusage;
    expect_distinct_ready_plan(policy);
    policy = baseline_policy;
    policy.operating_system = SIQSShadowProofRssOperatingSystem::windows;
    policy.architecture = SIQSShadowProofRssArchitecture::x86_64;
    policy.memory_backend = ProcessMemoryBackend::WindowsPsapi;
    expect_distinct_ready_plan(policy);

    policy = baseline_policy;
    policy.deployment_budget_bytes = std::numeric_limits<uint64_t>::max();
    policy.reserved_headroom_bytes = 0;
    expect_distinct_ready_plan(policy);
}

} // namespace

int main() {
    test_policy_gates();
    test_invalid_policy_bindings();
    test_ready_plan_shape_and_binding();
    test_policy_digest_sensitivity_and_typed_platform_independence();

    std::cout << "SIQS shadow proof RSS campaign: " << checks_passed << " passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
