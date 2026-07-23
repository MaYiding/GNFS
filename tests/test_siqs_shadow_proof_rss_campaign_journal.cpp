// Pure contract tests for the SIQS RSS campaign journal.
// This test never launches a process, opens a fixture, calls factor(), or samples live RSS.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace gnfs::siqs;
using gnfs::util::ProcessMemoryBackend;

static_assert(!std::is_default_constructible_v<SIQSShadowProofRssPreparedSlotStart>);
static_assert(!std::is_copy_constructible_v<SIQSShadowProofRssPreparedSlotStart>);
static_assert(std::is_move_constructible_v<SIQSShadowProofRssPreparedSlotStart>);
static_assert(!std::is_default_constructible_v<SIQSShadowProofRssLaunchPermit>);
static_assert(!std::is_copy_constructible_v<SIQSShadowProofRssLaunchPermit>);
static_assert(std::is_move_constructible_v<SIQSShadowProofRssLaunchPermit>);
static_assert(!std::is_copy_constructible_v<SIQSShadowProofRssCampaignJournalResume>);
static_assert(SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT == 80);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_MAX_CONCURRENCY == 1);

constexpr auto EMPTY_STDERR_SEAL =
    seal_siqs_shadow_proof_rss_artifact(SIQSShadowProofRssArtifactKind::probe_stderr, {});
static_assert(EMPTY_STDERR_SEAL.committed);
static_assert(EMPTY_STDERR_SEAL.byte_count == 0);
static_assert(EMPTY_STDERR_SEAL.digest != SIQSShadowProofRssCorpusDigest{});

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
    policy.deployment_budget_bytes = UINT64_C(1000000000);
    policy.reserved_headroom_bytes = UINT64_C(100000000);
    return policy;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignRuntimeFacts make_facts() noexcept {
    return {
        .operating_system = SIQSShadowProofRssOperatingSystem::darwin,
        .architecture = SIQSShadowProofRssArchitecture::arm64,
        .memory_backend = ProcessMemoryBackend::DarwinGetrusage,
        .resolved_production_sieve_workers = 4,
        .candidate_revision = "candidate-revision-1",
        .release_build = true,
        .ndebug = true,
    };
}

[[nodiscard]] SIQSShadowProofRssJournalCommitPayload make_payload(SIQSShadowProofRssSampleMode mode,
                                                                  uint64_t peak) {
    SIQSShadowProofRssJournalCommitPayload payload;
    payload.actual_operating_system = SIQSShadowProofRssOperatingSystem::darwin;
    payload.actual_architecture = SIQSShadowProofRssArchitecture::arm64;
    payload.actual_memory_backend = ProcessMemoryBackend::DarwinGetrusage;
    payload.actual_resolved_sieve_workers = 4;
    payload.fresh_process = true;
    payload.completed = true;
    payload.factor_identity = SIQSShadowProofRssFactorIdentity::pass;
    payload.proof_evidence = mode == SIQSShadowProofRssSampleMode::off
                                 ? SIQSShadowProofRssEvidence::not_applicable
                                 : SIQSShadowProofRssEvidence::pass;
    payload.matrix_evidence = payload.proof_evidence;
    payload.absolute_peak_rss_bytes = peak;
    payload.observe_minus_off_peak_bytes = INT64_C(17);
    payload.current_rss_bytes = peak / 2;
    payload.peak_growth_bytes = peak / 3;
    payload.wall_ns = UINT64_C(1000) + peak;
    payload.stdout_seal = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::probe_stdout, "synthetic-stdout-record\n");
    payload.stderr_seal = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::probe_stderr,
        mode == SIQSShadowProofRssSampleMode::off ? std::string_view{}
                                                  : std::string_view{"synthetic-observe-record\n"});
    payload.joined_sample_seal = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::joined_gate_sample, "synthetic-joined-sample\n");
    return payload;
}

[[nodiscard]] SIQSShadowProofRssCampaignJournalHeader
make_header(const SIQSShadowProofRssGatePolicy& policy,
            const SIQSShadowProofRssCampaignRuntimeFacts& facts) {
    auto absent = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {});
    CHECK(absent.status == SIQSShadowProofRssJournalStatus::ready);
    CHECK(absent.reason == SIQSShadowProofRssJournalReason::ready);
    CHECK(absent.action == SIQSShadowProofRssJournalAction::create_header);
    CHECK(absent.next_slot_number == 1);
    CHECK(absent.header_to_create.has_value());
    CHECK(!absent.prepared_slot_start.has_value());
    return *absent.header_to_create;
}

[[nodiscard]] bool append_one_slot(const SIQSShadowProofRssGatePolicy& policy,
                                   const SIQSShadowProofRssCampaignRuntimeFacts& facts,
                                   const SIQSShadowProofRssCampaignJournalHeader& header,
                                   std::vector<SIQSShadowProofRssCampaignJournalRecord>& records,
                                   bool check_dangling) {
    auto ready = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, records);
    CHECK(ready.status == SIQSShadowProofRssJournalStatus::ready);
    CHECK(ready.action == SIQSShadowProofRssJournalAction::append_slot_start);
    CHECK(ready.prepared_slot_start.has_value());
    if (!ready.prepared_slot_start.has_value()) {
        return false;
    }

    auto prepared = std::move(*ready.prepared_slot_start);
    const auto start = prepared.record();
    CHECK(prepared.active());
    CHECK(start.kind == SIQSShadowProofRssJournalRecordKind::slot_started);
    CHECK(start.slot_number == records.size() / 2 + 1);

    if (check_dangling) {
        auto dangling_records = records;
        dangling_records.push_back(start);
        auto dangling = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, dangling_records);
        CHECK(dangling.status == SIQSShadowProofRssJournalStatus::tainted);
        CHECK(dangling.reason == SIQSShadowProofRssJournalReason::dangling_slot_start);
        CHECK(dangling.action == SIQSShadowProofRssJournalAction::append_taint);
        CHECK(dangling.committed_slot_count == records.size() / 2);
        CHECK(dangling.next_slot_number == start.slot_number);
        CHECK(dangling.taint_to_append.has_value());
        if (dangling.taint_to_append.has_value()) {
            dangling_records.push_back(*dangling.taint_to_append);
            auto sealed_taint = resume_siqs_shadow_proof_rss_campaign_journal(
                &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header,
                dangling_records);
            CHECK(sealed_taint.status == SIQSShadowProofRssJournalStatus::tainted);
            CHECK(sealed_taint.reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
            CHECK(sealed_taint.action == SIQSShadowProofRssJournalAction::none);
            CHECK(!sealed_taint.prepared_slot_start.has_value());
        }
    }

    records.push_back(start); // Synthetic durable append boundary.
    auto permit =
        acknowledge_siqs_shadow_proof_rss_durable_slot_start(std::move(prepared), records.back());
    CHECK(permit.has_value());
    CHECK(!prepared.active());
    if (!permit.has_value()) {
        return false;
    }

    const auto plan = make_siqs_shadow_proof_rss_campaign_plan(&policy);
    const auto& slot = plan.slots[start.slot_number - 1];
    const auto payload = make_payload(slot.mode, UINT64_C(1000000) + start.slot_number);
    auto commit = make_siqs_shadow_proof_rss_slot_commit(std::move(*permit), &policy, payload);
    CHECK(commit.has_value());
    CHECK(!permit->active());
    if (!commit.has_value()) {
        return false;
    }
    CHECK(commit->kind == SIQSShadowProofRssJournalRecordKind::slot_committed);
    CHECK(commit->sequence_number == start.sequence_number + 1);
    CHECK(commit->previous_record_digest == start.record_digest);
    records.push_back(*commit);
    return true;
}

void expect_no_action(const SIQSShadowProofRssCampaignJournalResume& result,
                      SIQSShadowProofRssJournalStatus status,
                      SIQSShadowProofRssJournalReason reason) {
    CHECK(result.status == status);
    CHECK(result.reason == reason);
    CHECK(result.action == SIQSShadowProofRssJournalAction::none);
    CHECK(!result.header_to_create.has_value());
    CHECK(!result.prepared_slot_start.has_value());
}

void test_policy_and_runtime_preflight() {
    const auto approved = make_policy();
    const auto facts = make_facts();

    expect_no_action(resume_siqs_shadow_proof_rss_campaign_journal(
                         nullptr, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
                     SIQSShadowProofRssJournalStatus::blocked,
                     SIQSShadowProofRssJournalReason::policy_missing);

    auto policy = approved;
    policy.approved = false;
    expect_no_action(resume_siqs_shadow_proof_rss_campaign_journal(
                         &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
                     SIQSShadowProofRssJournalStatus::blocked,
                     SIQSShadowProofRssJournalReason::policy_not_approved);

    policy = approved;
    policy.deployment_budget_bytes.reset();
    expect_no_action(resume_siqs_shadow_proof_rss_campaign_journal(
                         &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
                     SIQSShadowProofRssJournalStatus::blocked,
                     SIQSShadowProofRssJournalReason::policy_budget_missing);

    policy = approved;
    policy.reserved_headroom_bytes.reset();
    expect_no_action(resume_siqs_shadow_proof_rss_campaign_journal(
                         &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
                     SIQSShadowProofRssJournalStatus::blocked,
                     SIQSShadowProofRssJournalReason::policy_headroom_missing);
    policy = approved;
    policy.deployment_budget_bytes = policy.reserved_headroom_bytes;
    expect_no_action(resume_siqs_shadow_proof_rss_campaign_journal(
                         &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
                     SIQSShadowProofRssJournalStatus::invalid,
                     SIQSShadowProofRssJournalReason::policy_budget_not_above_headroom);
    policy = approved;
    policy.corpus_id = "wrong-corpus";
    expect_no_action(resume_siqs_shadow_proof_rss_campaign_journal(
                         &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
                     SIQSShadowProofRssJournalStatus::invalid,
                     SIQSShadowProofRssJournalReason::policy_binding_invalid);

    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &approved, nullptr, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
        SIQSShadowProofRssJournalStatus::invalid,
        SIQSShadowProofRssJournalReason::runtime_facts_missing);

    auto changed = facts;
    changed.operating_system = SIQSShadowProofRssOperatingSystem::linux;
    changed.memory_backend = ProcessMemoryBackend::LinuxGetrusage;
    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &approved, &changed, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
        SIQSShadowProofRssJournalStatus::invalid,
        SIQSShadowProofRssJournalReason::runtime_facts_mismatch);
    changed = facts;
    changed.architecture = SIQSShadowProofRssArchitecture::x86_64;
    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &approved, &changed, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
        SIQSShadowProofRssJournalStatus::invalid,
        SIQSShadowProofRssJournalReason::runtime_facts_mismatch);
    changed = facts;
    changed.memory_backend = ProcessMemoryBackend::LinuxGetrusage;
    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &approved, &changed, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
        SIQSShadowProofRssJournalStatus::invalid,
        SIQSShadowProofRssJournalReason::runtime_facts_invalid);
    changed = facts;
    changed.resolved_production_sieve_workers = 3;
    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &approved, &changed, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
        SIQSShadowProofRssJournalStatus::invalid,
        SIQSShadowProofRssJournalReason::runtime_facts_mismatch);
    changed = facts;
    changed.candidate_revision = "other-revision";
    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &approved, &changed, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
        SIQSShadowProofRssJournalStatus::invalid,
        SIQSShadowProofRssJournalReason::runtime_facts_mismatch);
    changed = facts;
    changed.release_build = false;
    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &approved, &changed, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
        SIQSShadowProofRssJournalStatus::invalid,
        SIQSShadowProofRssJournalReason::release_ndebug_required);
    changed = facts;
    changed.ndebug = false;
    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &approved, &changed, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
        SIQSShadowProofRssJournalStatus::invalid,
        SIQSShadowProofRssJournalReason::release_ndebug_required);

    for (const auto invalid_facts : {
             SIQSShadowProofRssCampaignRuntimeFacts{
                 .operating_system = SIQSShadowProofRssOperatingSystem::unknown,
                 .architecture = facts.architecture,
                 .memory_backend = facts.memory_backend,
                 .resolved_production_sieve_workers = facts.resolved_production_sieve_workers,
                 .candidate_revision = facts.candidate_revision,
                 .release_build = true,
                 .ndebug = true,
             },
             SIQSShadowProofRssCampaignRuntimeFacts{
                 .operating_system = facts.operating_system,
                 .architecture = SIQSShadowProofRssArchitecture::unknown,
                 .memory_backend = facts.memory_backend,
                 .resolved_production_sieve_workers = facts.resolved_production_sieve_workers,
                 .candidate_revision = facts.candidate_revision,
                 .release_build = true,
                 .ndebug = true,
             },
             SIQSShadowProofRssCampaignRuntimeFacts{
                 .operating_system = facts.operating_system,
                 .architecture = facts.architecture,
                 .memory_backend = ProcessMemoryBackend::Unsupported,
                 .resolved_production_sieve_workers = facts.resolved_production_sieve_workers,
                 .candidate_revision = facts.candidate_revision,
                 .release_build = true,
                 .ndebug = true,
             },
             SIQSShadowProofRssCampaignRuntimeFacts{
                 .operating_system = facts.operating_system,
                 .architecture = facts.architecture,
                 .memory_backend = facts.memory_backend,
                 .resolved_production_sieve_workers = 0,
                 .candidate_revision = facts.candidate_revision,
                 .release_build = true,
                 .ndebug = true,
             },
             SIQSShadowProofRssCampaignRuntimeFacts{
                 .operating_system = facts.operating_system,
                 .architecture = facts.architecture,
                 .memory_backend = facts.memory_backend,
                 .resolved_production_sieve_workers = facts.resolved_production_sieve_workers,
                 .candidate_revision = "unsafe revision",
                 .release_build = true,
                 .ndebug = true,
             },
         }) {
        expect_no_action(
            resume_siqs_shadow_proof_rss_campaign_journal(
                &approved, &invalid_facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {}),
            SIQSShadowProofRssJournalStatus::invalid,
            SIQSShadowProofRssJournalReason::runtime_facts_invalid);
    }

    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &approved, &facts, static_cast<SIQSShadowProofRssJournalPresence>(255), nullptr, {}),
        SIQSShadowProofRssJournalStatus::invalid,
        SIQSShadowProofRssJournalReason::journal_presence_invalid);
}

void test_presence_header_and_capability_boundaries() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto header = make_header(policy, facts);

    expect_no_action(resume_siqs_shadow_proof_rss_campaign_journal(
                         &policy, &facts, SIQSShadowProofRssJournalPresence::present, nullptr, {}),
                     SIQSShadowProofRssJournalStatus::invalid,
                     SIQSShadowProofRssJournalReason::present_journal_missing_header);
    expect_no_action(resume_siqs_shadow_proof_rss_campaign_journal(
                         &policy, &facts, SIQSShadowProofRssJournalPresence::absent, &header, {}),
                     SIQSShadowProofRssJournalStatus::invalid,
                     SIQSShadowProofRssJournalReason::absent_journal_has_state);

    auto bad_header = header;
    ++bad_header.plan_digest.low;
    expect_no_action(
        resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &bad_header, {}),
        SIQSShadowProofRssJournalStatus::invalid, SIQSShadowProofRssJournalReason::header_invalid);

    auto ready = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, {});
    CHECK(ready.status == SIQSShadowProofRssJournalStatus::ready);
    CHECK(ready.action == SIQSShadowProofRssJournalAction::append_slot_start);
    CHECK(ready.next_slot_number == 1);
    CHECK(ready.prepared_slot_start.has_value());

    auto first = std::move(*ready.prepared_slot_start);
    const auto start = first.record();
    auto moved = std::move(first);
    CHECK(!first.active());
    CHECK(moved.active());
    CHECK(!acknowledge_siqs_shadow_proof_rss_durable_slot_start(std::move(first), start));

    auto wrong = start;
    ++wrong.sequence_number;
    CHECK(!acknowledge_siqs_shadow_proof_rss_durable_slot_start(std::move(moved), wrong));
    CHECK(moved.active());
    auto permit = acknowledge_siqs_shadow_proof_rss_durable_slot_start(std::move(moved), start);
    CHECK(permit.has_value());
    CHECK(!moved.active());
    if (permit.has_value()) {
        auto moved_permit = std::move(*permit);
        CHECK(!permit->active());
        CHECK(moved_permit.active());
        auto bad_payload = make_payload(SIQSShadowProofRssSampleMode::off, 1);
        bad_payload.completed = false;
        CHECK(
            !make_siqs_shadow_proof_rss_slot_commit(std::move(moved_permit), &policy, bad_payload));
        CHECK(!moved_permit.active());
        CHECK(!make_siqs_shadow_proof_rss_slot_commit(
            std::move(moved_permit), &policy, make_payload(SIQSShadowProofRssSampleMode::off, 1)));
    }
}

void test_post_start_failures_are_tainted() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto header = make_header(policy, facts);
    auto ready = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, {});
    const auto start = ready.prepared_slot_start->record();

    auto corrupt = start;
    ++corrupt.sequence_number;
    auto before_start = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header,
        std::span<const SIQSShadowProofRssCampaignJournalRecord>(&corrupt, 1));
    expect_no_action(before_start, SIQSShadowProofRssJournalStatus::invalid,
                     SIQSShadowProofRssJournalReason::record_invalid);

    SIQSShadowProofRssCampaignJournalRecord bad_commit;
    bad_commit.schema_version = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION;
    bad_commit.sequence_number = start.sequence_number + 1;
    bad_commit.kind = SIQSShadowProofRssJournalRecordKind::slot_committed;
    bad_commit.previous_record_digest = start.record_digest;
    bad_commit.plan_digest = start.plan_digest;
    bad_commit.slot_number = start.slot_number;
    bad_commit.slot_digest = start.slot_digest;
    bad_commit.commit_payload = make_payload(SIQSShadowProofRssSampleMode::off, 1);
    bad_commit.commit_payload.completed = false;
    bad_commit.record_digest = shadow_proof_rss_campaign_journal_detail::record_digest(bad_commit);
    const std::vector records{start, bad_commit};
    auto after_start = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, records);
    expect_no_action(after_start, SIQSShadowProofRssJournalStatus::tainted,
                     SIQSShadowProofRssJournalReason::committed_sample_invalid);

    auto noncanonical_empty = bad_commit;
    noncanonical_empty.commit_payload.completed = true;
    ++noncanonical_empty.commit_payload.stderr_seal.digest.low;
    noncanonical_empty.record_digest =
        shadow_proof_rss_campaign_journal_detail::record_digest(noncanonical_empty);
    const std::vector bad_empty_records{start, noncanonical_empty};
    auto bad_empty = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, bad_empty_records);
    expect_no_action(bad_empty, SIQSShadowProofRssJournalStatus::tainted,
                     SIQSShadowProofRssJournalReason::committed_sample_invalid);
}

void test_header_field_closure() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto header = make_header(policy, facts);
    std::vector<SIQSShadowProofRssCampaignJournalHeader> mutations;

    const auto add_rehashed = [&](auto mutate) {
        auto changed = header;
        mutate(changed);
        changed.header_digest = shadow_proof_rss_campaign_journal_detail::header_digest(changed);
        mutations.push_back(changed);
    };
    add_rehashed([](auto& value) { ++value.schema_version; });
    add_rehashed([](auto& value) { ++value.policy_binding_digest.low; });
    add_rehashed([](auto& value) { ++value.policy_binding_digest.high; });
    add_rehashed([](auto& value) { ++value.runtime_facts_digest.low; });
    add_rehashed([](auto& value) { ++value.runtime_facts_digest.high; });
    add_rehashed([](auto& value) { ++value.plan_digest.low; });
    add_rehashed([](auto& value) { ++value.plan_digest.high; });
    add_rehashed([](auto& value) { ++value.slot_count; });
    add_rehashed([](auto& value) { ++value.max_concurrency; });
    auto changed = header;
    ++changed.header_digest.low;
    mutations.push_back(changed);
    changed = header;
    ++changed.header_digest.high;
    mutations.push_back(changed);

    for (const auto& mutation : mutations) {
        expect_no_action(
            resume_siqs_shadow_proof_rss_campaign_journal(
                &policy, &facts, SIQSShadowProofRssJournalPresence::present, &mutation, {}),
            SIQSShadowProofRssJournalStatus::invalid,
            SIQSShadowProofRssJournalReason::header_invalid);
    }
}

void test_record_chain_and_order_closure() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto header = make_header(policy, facts);
    std::vector<SIQSShadowProofRssCampaignJournalRecord> pair;
    CHECK(append_one_slot(policy, facts, header, pair, false));
    CHECK(pair.size() == 2);
    const auto start = pair.front();
    const auto commit = pair.back();

    std::vector<SIQSShadowProofRssCampaignJournalRecord> bad_starts;
    const auto add_bad_start = [&](auto mutate, bool rehash = true) {
        auto value = start;
        mutate(value);
        if (rehash) {
            value.record_digest = shadow_proof_rss_campaign_journal_detail::record_digest(value);
        }
        bad_starts.push_back(value);
    };
    add_bad_start([](auto& value) { ++value.schema_version; });
    add_bad_start([](auto& value) { ++value.sequence_number; });
    add_bad_start([](auto& value) { ++value.previous_record_digest.low; });
    add_bad_start([](auto& value) { ++value.previous_record_digest.high; });
    add_bad_start([](auto& value) { ++value.plan_digest.low; });
    add_bad_start([](auto& value) { ++value.plan_digest.high; });
    add_bad_start([](auto& value) { ++value.slot_number; });
    add_bad_start([](auto& value) { ++value.slot_digest.low; });
    add_bad_start([](auto& value) { ++value.slot_digest.high; });
    add_bad_start([](auto& value) { value.kind = SIQSShadowProofRssJournalRecordKind::unknown; });
    add_bad_start(
        [](auto& value) { value.kind = SIQSShadowProofRssJournalRecordKind::slot_committed; });
    add_bad_start([](auto& value) { value.commit_payload.fresh_process = true; });
    add_bad_start([](auto& value) { ++value.record_digest.low; }, false);
    add_bad_start([](auto& value) { ++value.record_digest.high; }, false);
    for (const auto& mutation : bad_starts) {
        const auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header,
            std::span<const SIQSShadowProofRssCampaignJournalRecord>(&mutation, 1));
        CHECK(replay.status == SIQSShadowProofRssJournalStatus::invalid);
        CHECK(replay.action == SIQSShadowProofRssJournalAction::none);
    }

    std::vector<SIQSShadowProofRssCampaignJournalRecord> bad_commits;
    const auto add_bad_commit = [&](auto mutate, bool rehash = true) {
        auto value = commit;
        mutate(value);
        if (rehash) {
            value.record_digest = shadow_proof_rss_campaign_journal_detail::record_digest(value);
        }
        bad_commits.push_back(value);
    };
    add_bad_commit([](auto& value) { ++value.schema_version; });
    add_bad_commit([](auto& value) { ++value.sequence_number; });
    add_bad_commit([](auto& value) { ++value.previous_record_digest.low; });
    add_bad_commit([](auto& value) { ++value.previous_record_digest.high; });
    add_bad_commit([](auto& value) { ++value.plan_digest.low; });
    add_bad_commit([](auto& value) { ++value.plan_digest.high; });
    add_bad_commit([](auto& value) { ++value.slot_number; });
    add_bad_commit([](auto& value) { ++value.slot_digest.low; });
    add_bad_commit([](auto& value) { ++value.slot_digest.high; });
    add_bad_commit([](auto& value) { value.kind = SIQSShadowProofRssJournalRecordKind::unknown; });
    add_bad_commit(
        [](auto& value) { value.kind = SIQSShadowProofRssJournalRecordKind::slot_started; });
    add_bad_commit([](auto& value) { ++value.record_digest.low; }, false);
    add_bad_commit([](auto& value) { ++value.record_digest.high; }, false);
    for (const auto& mutation : bad_commits) {
        const std::vector records{start, mutation};
        const auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, records);
        CHECK(replay.status == SIQSShadowProofRssJournalStatus::tainted);
        CHECK(replay.action == SIQSShadowProofRssJournalAction::none);
        CHECK(!replay.prepared_slot_start.has_value());
    }

    const std::array commit_before_start{commit};
    const auto early_commit = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, commit_before_start);
    CHECK(early_commit.status == SIQSShadowProofRssJournalStatus::invalid);

    auto second_start = start;
    second_start.sequence_number = 2;
    second_start.previous_record_digest = start.record_digest;
    second_start.record_digest =
        shadow_proof_rss_campaign_journal_detail::record_digest(second_start);
    const std::vector double_start{start, second_start};
    const auto double_start_replay = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, double_start);
    CHECK(double_start_replay.status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(double_start_replay.reason == SIQSShadowProofRssJournalReason::record_order_invalid);

    const auto plan = make_siqs_shadow_proof_rss_campaign_plan(&policy);
    auto skipped = start;
    skipped.slot_number = 2;
    skipped.slot_digest =
        shadow_proof_rss_campaign_journal_detail::slot_digest(header.plan_digest, plan.slots[1]);
    skipped.record_digest = shadow_proof_rss_campaign_journal_detail::record_digest(skipped);
    const auto skipped_replay = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header,
        std::span<const SIQSShadowProofRssCampaignJournalRecord>(&skipped, 1));
    CHECK(skipped_replay.status == SIQSShadowProofRssJournalStatus::invalid);
    CHECK(skipped_replay.reason == SIQSShadowProofRssJournalReason::record_order_invalid);
}

void test_commit_payload_and_artifact_closure() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto header = make_header(policy, facts);
    std::vector<SIQSShadowProofRssCampaignJournalRecord> off_pair;
    CHECK(append_one_slot(policy, facts, header, off_pair, false));
    const auto off_commit = off_pair.back();
    std::vector<SIQSShadowProofRssJournalCommitPayload> off_mutations;
    const auto add_off = [&](auto mutate) {
        auto value = off_commit.commit_payload;
        mutate(value);
        off_mutations.push_back(value);
    };
    add_off([](auto& value) {
        value.actual_operating_system = SIQSShadowProofRssOperatingSystem::linux;
    });
    add_off(
        [](auto& value) { value.actual_architecture = SIQSShadowProofRssArchitecture::x86_64; });
    add_off(
        [](auto& value) { value.actual_memory_backend = ProcessMemoryBackend::LinuxGetrusage; });
    add_off([](auto& value) { value.actual_resolved_sieve_workers = 3; });
    add_off([](auto& value) { value.fresh_process = false; });
    add_off([](auto& value) { value.completed = false; });
    add_off([](auto& value) { value.factor_identity = SIQSShadowProofRssFactorIdentity::unknown; });
    add_off([](auto& value) { value.factor_identity = SIQSShadowProofRssFactorIdentity::fail; });
    add_off(
        [](auto& value) { value.factor_identity = SIQSShadowProofRssFactorIdentity::not_checked; });
    add_off([](auto& value) { value.absolute_peak_rss_bytes.reset(); });
    add_off([](auto& value) { value.absolute_peak_rss_bytes = 0; });
    add_off([](auto& value) { value.proof_evidence = SIQSShadowProofRssEvidence::pass; });
    add_off([](auto& value) { value.matrix_evidence = SIQSShadowProofRssEvidence::fail; });
    add_off([](auto& value) { value.stdout_seal.committed = false; });
    add_off([](auto& value) { value.stdout_seal.kind = SIQSShadowProofRssArtifactKind::unknown; });
    add_off([](auto& value) { value.stdout_seal.byte_count = 0; });
    add_off([](auto& value) { value.stdout_seal.digest = {}; });
    add_off([](auto& value) { value.stderr_seal.committed = false; });
    add_off([](auto& value) { value.stderr_seal.kind = SIQSShadowProofRssArtifactKind::unknown; });
    add_off([](auto& value) { value.stderr_seal.byte_count = 1; });
    add_off([](auto& value) { value.stderr_seal.digest = {}; });
    add_off([](auto& value) { ++value.stderr_seal.digest.low; });
    add_off([](auto& value) { value.joined_sample_seal.committed = false; });
    add_off([](auto& value) {
        value.joined_sample_seal.kind = SIQSShadowProofRssArtifactKind::unknown;
    });
    add_off([](auto& value) { value.joined_sample_seal.byte_count = 0; });
    add_off([](auto& value) { value.joined_sample_seal.digest = {}; });

    for (const auto& payload : off_mutations) {
        auto records = off_pair;
        records.back().commit_payload = payload;
        records.back().record_digest =
            shadow_proof_rss_campaign_journal_detail::record_digest(records.back());
        const auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, records);
        CHECK(replay.status == SIQSShadowProofRssJournalStatus::tainted);
        CHECK(replay.reason == SIQSShadowProofRssJournalReason::committed_sample_invalid);
        CHECK(replay.action == SIQSShadowProofRssJournalAction::none);
    }

    std::vector<SIQSShadowProofRssCampaignJournalRecord> observe_prefix;
    for (int slot = 0; slot < 4; ++slot) {
        CHECK(append_one_slot(policy, facts, header, observe_prefix, false));
    }
    const auto observe_commit = observe_prefix.back();
    CHECK(observe_commit.slot_number == 4);
    std::vector<SIQSShadowProofRssJournalCommitPayload> observe_mutations;
    for (const auto evidence :
         {SIQSShadowProofRssEvidence::unknown, SIQSShadowProofRssEvidence::not_applicable,
          SIQSShadowProofRssEvidence::fail}) {
        auto proof = observe_commit.commit_payload;
        proof.proof_evidence = evidence;
        observe_mutations.push_back(proof);
        auto matrix = observe_commit.commit_payload;
        matrix.matrix_evidence = evidence;
        observe_mutations.push_back(matrix);
    }
    auto empty_stderr = observe_commit.commit_payload;
    empty_stderr.stderr_seal = EMPTY_STDERR_SEAL;
    observe_mutations.push_back(empty_stderr);
    for (const auto& payload : observe_mutations) {
        auto records = observe_prefix;
        records.back().commit_payload = payload;
        records.back().record_digest =
            shadow_proof_rss_campaign_journal_detail::record_digest(records.back());
        const auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, records);
        CHECK(replay.status == SIQSShadowProofRssJournalStatus::tainted);
        CHECK(replay.reason == SIQSShadowProofRssJournalReason::committed_sample_invalid);
    }
}

void test_taint_record_closure_and_absorption() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto header = make_header(policy, facts);
    auto ready = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, {});
    const auto start = ready.prepared_slot_start->record();
    const std::vector dangling_records{start};
    auto dangling = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, dangling_records);
    CHECK(dangling.taint_to_append.has_value());
    if (!dangling.taint_to_append.has_value()) {
        return;
    }
    const auto taint = *dangling.taint_to_append;
    const std::vector sealed{start, taint};
    auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, sealed);
    CHECK(replay.status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(replay.action == SIQSShadowProofRssJournalAction::none);
    CHECK(!reconstruct_siqs_shadow_proof_rss_gate_samples(&policy, &facts, &header, sealed));

    auto trailing = sealed;
    trailing.push_back(start);
    replay = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, trailing);
    CHECK(replay.status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(replay.action == SIQSShadowProofRssJournalAction::none);

    std::vector<SIQSShadowProofRssCampaignJournalRecord> bad_taints;
    const auto add_bad_taint = [&](auto mutate, bool rehash = true) {
        auto value = taint;
        mutate(value);
        if (rehash) {
            value.record_digest = shadow_proof_rss_campaign_journal_detail::record_digest(value);
        }
        bad_taints.push_back(value);
    };
    add_bad_taint([](auto& value) { ++value.schema_version; });
    add_bad_taint([](auto& value) { ++value.sequence_number; });
    add_bad_taint([](auto& value) { ++value.previous_record_digest.low; });
    add_bad_taint([](auto& value) { ++value.plan_digest.low; });
    add_bad_taint([](auto& value) { ++value.slot_number; });
    add_bad_taint([](auto& value) { ++value.slot_digest.low; });
    add_bad_taint([](auto& value) { value.commit_payload.completed = true; });
    add_bad_taint([](auto& value) { ++value.record_digest.low; }, false);
    for (const auto& mutation : bad_taints) {
        const std::vector records{start, mutation};
        replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, records);
        CHECK(replay.status == SIQSShadowProofRssJournalStatus::tainted);
        CHECK(replay.action == SIQSShadowProofRssJournalAction::none);
    }
}

void test_all_slots_resume_and_terminal_gate() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto header = make_header(policy, facts);
    std::vector<SIQSShadowProofRssCampaignJournalRecord> records;
    records.reserve(2 * SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);

    for (std::size_t index = 0; index < SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT; ++index) {
        CHECK(append_one_slot(policy, facts, header, records, true));
        CHECK(records.size() == 2 * (index + 1));
    }

    auto complete = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, records);
    CHECK(complete.status == SIQSShadowProofRssJournalStatus::complete);
    CHECK(complete.reason == SIQSShadowProofRssJournalReason::complete);
    CHECK(complete.action == SIQSShadowProofRssJournalAction::evaluate_gate);
    CHECK(complete.committed_slot_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);
    CHECK(complete.next_slot_number == 0);
    CHECK(!complete.prepared_slot_start.has_value());

    auto samples =
        reconstruct_siqs_shadow_proof_rss_gate_samples(&policy, &facts, &header, records);
    CHECK(samples.has_value());
    if (samples.has_value()) {
        const auto outcome = evaluate_siqs_shadow_proof_rss_gate(&policy, *samples);
        CHECK(outcome.status == SIQSShadowProofRssGateStatus::manual_review_candidate);
        CHECK(outcome.reason == SIQSShadowProofRssGateReason::all_observe_peaks_within_limit);
        CHECK(outcome.total_sample_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);
        CHECK(outcome.valid_off_sample_count == 24);
        CHECK(outcome.valid_observe_sample_count == 56);
        CHECK(!outcome.shadow_outcome_routed);
        CHECK(!outcome.promotion);
    }

    // Mutating a replay result cannot affect reconstruction, which replays the
    // immutable input records instead of trusting the public result aggregate.
    complete.committed_payloads[0].absolute_peak_rss_bytes = UINT64_MAX;
    CHECK(reconstruct_siqs_shadow_proof_rss_gate_samples(&policy, &facts, &header, records));

    auto bad_records = records;
    ++bad_records.back().record_digest.low;
    CHECK(!reconstruct_siqs_shadow_proof_rss_gate_samples(&policy, &facts, &header, bad_records));

    bad_records = records;
    bad_records.back().commit_payload.factor_identity = SIQSShadowProofRssFactorIdentity::fail;
    bad_records.back().record_digest =
        shadow_proof_rss_campaign_journal_detail::record_digest(bad_records.back());
    auto tainted = resume_siqs_shadow_proof_rss_campaign_journal(
        &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, bad_records);
    CHECK(tainted.status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(!reconstruct_siqs_shadow_proof_rss_gate_samples(&policy, &facts, &header, bad_records));

    auto bad_header = header;
    ++bad_header.runtime_facts_digest.high;
    CHECK(!reconstruct_siqs_shadow_proof_rss_gate_samples(&policy, &facts, &bad_header, records));

    auto bad_facts = facts;
    bad_facts.resolved_production_sieve_workers = 2;
    CHECK(!reconstruct_siqs_shadow_proof_rss_gate_samples(&policy, &bad_facts, &header, records));

    const auto expect_not_complete = [&](const auto& changed_records) {
        const auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, changed_records);
        CHECK(replay.status != SIQSShadowProofRssJournalStatus::complete);
        CHECK(replay.action != SIQSShadowProofRssJournalAction::evaluate_gate);
        CHECK(!reconstruct_siqs_shadow_proof_rss_gate_samples(&policy, &facts, &header,
                                                              changed_records));
    };
    auto missing = records;
    missing.erase(missing.begin() + 17);
    expect_not_complete(missing);
    auto swapped = records;
    std::swap(swapped[40], swapped[42]);
    std::swap(swapped[41], swapped[43]);
    expect_not_complete(swapped);
    auto duplicated = records;
    duplicated.insert(duplicated.begin() + 20, records.begin() + 18, records.begin() + 20);
    expect_not_complete(duplicated);
    auto extra_start = records;
    extra_start.push_back(records.front());
    expect_not_complete(extra_start);
    auto extra_commit = records;
    extra_commit.push_back(records.back());
    expect_not_complete(extra_commit);
}

} // namespace

int main() {
    test_policy_and_runtime_preflight();
    test_presence_header_and_capability_boundaries();
    test_post_start_failures_are_tainted();
    test_header_field_closure();
    test_record_chain_and_order_closure();
    test_commit_payload_and_artifact_closure();
    test_taint_record_closure_and_absorption();
    test_all_slots_resume_and_terminal_gate();

    std::cout << "SIQS shadow-proof RSS campaign journal: " << checks_passed << " checks passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
