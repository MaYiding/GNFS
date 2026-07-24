// Pure strict-parser and typed-join tests. No child process, holdout factor(),
// journal store, artifact publication, receipt, or launch permit is used.

#include "shadow_proof_rss_holdout_stream_join_internal.hpp"

#include <gnfs/siqs/siqs.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using namespace gnfs::siqs;
using namespace gnfs::siqs::shadow_proof_rss_holdout_fixture_detail;
using namespace gnfs::siqs::shadow_proof_rss_holdout_detail;
using gnfs::util::ProcessMemoryBackend;

static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_SCHEMA_VERSION == 1);
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_FACTOR_BASE_COLUMNS == 1601);
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS == 1701);
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS.size() == 8);
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS.front() == UINT64_C(3494760));
static_assert(SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS.back() == UINT64_C(3593640));
static_assert(!std::is_same_v<SIQSShadowProofRssUncommittedSampleDraft,
                              SIQSShadowProofRssJournalCommitPayload>);
static_assert(noexcept(join_siqs_shadow_proof_rss_holdout_streams(nullptr, nullptr, nullptr,
                                                                  std::string_view{},
                                                                  std::string_view{})));

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
    policy.deployment_budget_bytes = UINT64_C(1'000'000'000);
    policy.reserved_headroom_bytes = UINT64_C(100'000'000);
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

[[nodiscard]] SIQSShadowProofRssHoldoutProbeRecord
make_probe_record(const SIQSShadowProofRssCampaignSlot& slot,
                  ProcessMemoryBackend backend = ProcessMemoryBackend::DarwinGetrusage,
                  uint64_t workers = 4) {
    const auto& fixture = SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1[slot.fixture_id - 1];
    SIQSShadowProofRssHoldoutProbeRecord record;
    record.fixture_id = slot.fixture_id;
    record.mode = slot.mode == SIQSShadowProofRssSampleMode::off
                      ? SIQSShadowProofRssHoldoutProbeMode::off
                      : SIQSShadowProofRssHoldoutProbeMode::observe;
    record.ordinal = slot.ordinal;
    record.environment_value = siqs_shadow_proof_rss_holdout_probe_environment_value(record.mode);
    record.digits = SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS;
    record.modulus = fixture.modulus;
    record.expected_factor = fixture.factor_p;
    record.expected_cofactor = fixture.factor_q;
    record.factor = fixture.factor_p;
    record.cofactor = fixture.factor_q;
    record.relations_found = SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS;
    record.polynomials_used = 9;
    record.resolved_production_sieve_workers = workers;
    record.factor_wall_ns = UINT64_C(1'000'000'000);
    record.rss_backend = gnfs::util::process_memory_backend_name(backend);
    record.before_current_rss_supported = true;
    record.before_current_rss_bytes = 100;
    record.before_peak_rss_supported = true;
    record.before_peak_rss_bytes = 200;
    record.after_current_rss_supported = true;
    record.after_current_rss_bytes = 700;
    record.after_peak_rss_supported = true;
    record.after_peak_rss_bytes = 1000;
    record.absolute_peak_rss_supported = true;
    record.absolute_peak_rss_bytes = 1000;
    record.peak_growth_supported = true;
    record.peak_growth_bytes = 800;
    return record;
}

[[nodiscard]] std::string emit_probe(const SIQSShadowProofRssHoldoutProbeRecord& record) {
    std::string output;
    CHECK(emit_siqs_shadow_proof_rss_holdout_probe_record(record, output));
    return output;
}

[[nodiscard]] SIQSShadowProofObserveRecord make_observe_record(uint32_t fixture_id = 1) {
    const SIQSShadowProofOptions defaults{};
    SIQSShadowProofObserveRecord record;
    record.proof_attempted = true;
    record.terminal_status = SIQSShadowProofTerminalStatus::factor_found;
    record.stage = SIQSShadowProofStage::factor_extraction;
    record.fallback_reason = SIQSShadowProofFallbackReason::none;
    record.factor_found = true;
    record.observe_wall_ns = UINT64_C(500'000'000);
    record.raw_relations = 8457;
    record.raw_payload_supported = true;
    record.raw_payload_bytes = 14'151'664;
    record.factor_base_columns = SIQS_SHADOW_PROOF_RSS_HOLDOUT_FACTOR_BASE_COLUMNS;
    record.large_prime_bound = SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS[fixture_id - 1];
    record.raw_relation_cap = defaults.limits.max_raw_relations;
    record.raw_payload_cap_bytes = defaults.limits.max_raw_payload_bytes;
    record.graph_edge_cap = defaults.limits.graph.max_edges;
    record.graph_cycle_cap = defaults.limits.graph.max_cycles;
    record.graph_incidence_cap = defaults.limits.graph.max_cycle_incidences;
    record.row_candidate_cap = defaults.limits.max_row_candidates;
    record.pretrim_row_cap = defaults.limits.max_pretrim_rows;
    record.minimum_row_excess = defaults.limits.minimum_row_excess;
    record.trim_excess_rows = defaults.assembly.trim_excess_rows;
    record.assembly_workers = defaults.assembly.materialization_workers;
    record.matrix_max_dependencies = defaults.matrix.max_dependencies;
    record.matrix_workers = defaults.matrix.elimination_workers;
    record.matrix_parallel_column_threshold = defaults.matrix.parallel_column_threshold;
    record.matrix_dense_bytes_cap = defaults.matrix.max_dense_matrix_bytes;
    record.matrix_dense_variable_cap = defaults.matrix.max_dense_variable_count;
    record.adapter_input_relations = 8457;
    record.adapter_full_relations = 1243;
    record.adapter_accepted_one_lp = 7214;
    record.graph_evidence_supported = true;
    record.graph_vertices = 6641;
    record.graph_edges = 7214;
    record.graph_components = 1;
    record.graph_cycles = 574;
    record.graph_cycle_incidences = 1148;
    record.graph_max_cycle_length = 2;
    record.row_candidate_upper = 1817;
    record.assembly_evidence_supported = true;
    record.assembly_pretrim_rows = 1816;
    record.assembly_selected_rows = SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS;
    record.assembly_selected_full_rows = 1242;
    record.assembly_selected_cycle_rows = 459;
    record.assembly_trimmed_rows = 115;
    record.assembly_fingerprint_supported = true;
    record.assembly_source_fingerprint_low = 11;
    record.assembly_source_fingerprint_high = 12;
    record.assembly_pretrim_fingerprint_low = 13;
    record.assembly_pretrim_fingerprint_high = 14;
    record.assembly_selected_fingerprint_low = 15;
    record.assembly_selected_fingerprint_high = 16;
    record.projected_dense_bytes_supported = true;
    record.projected_dense_bytes =
        *checked_siqs_shadow_dense_matrix_bytes(SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS,
                                                SIQS_SHADOW_PROOF_RSS_HOLDOUT_FACTOR_BASE_COLUMNS);
    record.matrix_evidence_supported = true;
    record.matrix_rows = SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS;
    record.matrix_columns = SIQS_SHADOW_PROOF_RSS_HOLDOUT_FACTOR_BASE_COLUMNS;
    record.minimum_nullity = 100;
    record.dependencies_returned = defaults.matrix.max_dependencies;
    record.dependencies_examined = 1;
    record.dependencies_verified = 1;
    record.factor_found_count = 1;
    record.dependency_cap_reached = true;
    record.dependency_fingerprint_supported = true;
    record.dependency_fingerprint_low = 17;
    record.dependency_fingerprint_high = 18;
    record.winning_dependency_supported = true;
    record.winning_dependency = 0;
    record.winning_dependency_size_supported = true;
    record.winning_dependency_size = 671;
    record.before_memory = {
        ProcessMemoryBackend::DarwinGetrusage,
        UINT64_C(250),
        UINT64_C(300),
    };
    record.after_memory = {
        ProcessMemoryBackend::DarwinGetrusage,
        UINT64_C(600),
        UINT64_C(900),
    };
    record.peak_growth_supported = true;
    record.peak_growth_bytes = 600;
    return record;
}

[[nodiscard]] std::string emit_observe(const SIQSShadowProofObserveRecord& record) {
    std::FILE* file = std::tmpfile();
    CHECK(file != nullptr);
    if (file == nullptr) {
        return {};
    }
    CHECK(emit_siqs_shadow_proof_observe_record(file, record));
    CHECK(std::fseek(file, 0, SEEK_SET) == 0);
    std::array<char, SIQS_SHADOW_PROOF_OBSERVE_RECORD_MAX_BYTES> buffer{};
    const std::size_t byte_count = std::fread(buffer.data(), 1, buffer.size(), file);
    CHECK(std::ferror(file) == 0);
    CHECK(std::fclose(file) == 0);
    return std::string(buffer.data(), byte_count);
}

[[nodiscard]] SIQSShadowProofRssHoldoutStreamJoinResult
join(const SIQSShadowProofRssGatePolicy& policy,
     const SIQSShadowProofRssCampaignRuntimeFacts& facts,
     const SIQSShadowProofRssCampaignSlot& slot, const std::string& stdout_bytes,
     const std::string& stderr_bytes) {
    return join_siqs_shadow_proof_rss_holdout_streams(&policy, &facts, &slot, stdout_bytes,
                                                      stderr_bytes);
}

void expect_error(const SIQSShadowProofRssHoldoutStreamJoinResult& result,
                  SIQSShadowProofRssHoldoutStreamJoinError error) {
    CHECK(!result);
    CHECK(!result.draft.has_value());
    CHECK(result.error == error);
}

void test_error_names() {
    for (unsigned value = 0;
         value <=
         static_cast<unsigned>(SIQSShadowProofRssHoldoutStreamJoinError::allocation_failure);
         ++value) {
        CHECK(siqs_shadow_proof_rss_holdout_stream_join_error_name(
                  static_cast<SIQSShadowProofRssHoldoutStreamJoinError>(value)) != "unknown");
    }
    CHECK(siqs_shadow_proof_rss_holdout_stream_join_error_name(
              static_cast<SIQSShadowProofRssHoldoutStreamJoinError>(255)) == "unknown");
}

void test_outcome_blind_fixture_profiles() {
    const SIQSParams params = select_params(SIQS_SHADOW_PROOF_RSS_HOLDOUT_PROBE_DIGITS);
    const SIQSShadowProofOptions shadow_defaults{};
    CHECK(params.fb_size == 1600);
    CHECK(params.lp_multiplier == 120);
    for (const auto& fixture : SIQS_SHADOW_OBSERVE_RSS_HOLDOUTS_V1) {
        gnfs::core::Integer modulus{std::string(fixture.modulus)};
        const uint32_t multiplier = select_multiplier(modulus);
        gnfs::core::Integer multiplied_modulus;
        mpz_mul_ui(multiplied_modulus.get_mpz(), modulus.get_mpz(), multiplier);
        const auto factor_base = build_factor_base(multiplied_modulus, params.fb_size);
        CHECK(factor_base.size() == SIQS_SHADOW_PROOF_RSS_HOLDOUT_FACTOR_BASE_COLUMNS);
        CHECK(factor_base.size() + shadow_defaults.assembly.trim_excess_rows ==
              SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS);
        CHECK(!factor_base.empty());
        if (!factor_base.empty()) {
            const uint64_t derived_bound = static_cast<uint64_t>(factor_base.back().p) *
                                           static_cast<uint64_t>(params.lp_multiplier);
            CHECK(derived_bound ==
                  SIQS_SHADOW_PROOF_RSS_HOLDOUT_LARGE_PRIME_BOUNDS[fixture.id - 1]);
        }
    }
}

void test_all_slots_and_draft_contract() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto plan = make_siqs_shadow_proof_rss_campaign_plan(&policy);
    CHECK(plan.status == SIQSShadowProofRssCampaignPlanStatus::ready);
    CHECK(plan.slot_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);
    for (std::size_t index = 0; index < plan.slot_count; ++index) {
        const auto& slot = plan.slots[index];
        const std::string stdout_bytes = emit_probe(make_probe_record(slot));
        const std::string stderr_bytes = slot.mode == SIQSShadowProofRssSampleMode::off
                                             ? std::string{}
                                             : emit_observe(make_observe_record(slot.fixture_id));
        const auto result = join(policy, facts, slot, stdout_bytes, stderr_bytes);
        CHECK(result);
        if (!result) {
            continue;
        }
        const auto& draft = *result.draft;
        CHECK(draft.slot_number == slot.slot_number);
        CHECK(draft.fixture_id == slot.fixture_id);
        CHECK(draft.mode == slot.mode);
        CHECK(draft.ordinal == slot.ordinal);
        CHECK(draft.fresh_process);
        CHECK(draft.completed);
        CHECK(draft.factor_identity == SIQSShadowProofRssFactorIdentity::pass);
        const auto expected_evidence = slot.mode == SIQSShadowProofRssSampleMode::off
                                           ? SIQSShadowProofRssEvidence::not_applicable
                                           : SIQSShadowProofRssEvidence::pass;
        CHECK(draft.proof_evidence == expected_evidence);
        CHECK(draft.matrix_evidence == expected_evidence);
        CHECK(draft.relations_found == SIQS_SHADOW_PROOF_RSS_HOLDOUT_SELECTED_ROWS);
        CHECK(draft.polynomials_used == 9);
        CHECK(draft.absolute_peak_rss_bytes == 1000);
        CHECK(draft.current_rss_bytes == UINT64_C(700));
        CHECK(draft.peak_growth_bytes == UINT64_C(800));
        CHECK(draft.wall_ns == UINT64_C(1'000'000'000));
        CHECK(draft.stdout_bytes == stdout_bytes);
        CHECK(draft.stderr_bytes == stderr_bytes);
        CHECK(draft.stdout_fingerprint.kind == SIQSShadowProofRssArtifactKind::probe_stdout);
        CHECK(draft.stdout_fingerprint.byte_count == stdout_bytes.size());
        CHECK(draft.stderr_fingerprint.kind == SIQSShadowProofRssArtifactKind::probe_stderr);
        CHECK(draft.stderr_fingerprint.byte_count == stderr_bytes.size());
        CHECK(draft.joined_bytes.starts_with(SIQS_SHADOW_PROOF_RSS_HOLDOUT_JOINED_DRAFT_PREFIX));
        CHECK(draft.joined_bytes.find(" authority=uncommitted ") != std::string::npos);
        CHECK(draft.joined_bytes.find(" committed=") == std::string::npos);
        CHECK(draft.joined_bytes.ends_with(" route=none promotion=false\n"));
    }
}

void test_preflight_and_stream_failures() {
    const auto approved = make_policy();
    const auto valid_facts = make_facts();
    const auto plan = make_siqs_shadow_proof_rss_campaign_plan(&approved);
    const auto& off_slot = plan.slots[0];
    const auto& observe_slot = plan.slots[3];
    const std::string off_stdout = emit_probe(make_probe_record(off_slot));
    const std::string observe_stdout = emit_probe(make_probe_record(observe_slot));
    const std::string observe_stderr = emit_observe(make_observe_record());

    expect_error(join_siqs_shadow_proof_rss_holdout_streams(nullptr, &valid_facts, &off_slot,
                                                            off_stdout, {}),
                 SIQSShadowProofRssHoldoutStreamJoinError::policy_invalid);
    auto policy = approved;
    policy.approved = false;
    expect_error(join_siqs_shadow_proof_rss_holdout_streams(&policy, &valid_facts, &off_slot,
                                                            off_stdout, {}),
                 SIQSShadowProofRssHoldoutStreamJoinError::policy_invalid);

    expect_error(
        join_siqs_shadow_proof_rss_holdout_streams(&approved, nullptr, &off_slot, off_stdout, {}),
        SIQSShadowProofRssHoldoutStreamJoinError::runtime_facts_invalid);
    auto facts = valid_facts;
    facts.memory_backend = ProcessMemoryBackend::LinuxGetrusage;
    expect_error(
        join_siqs_shadow_proof_rss_holdout_streams(&approved, &facts, &off_slot, off_stdout, {}),
        SIQSShadowProofRssHoldoutStreamJoinError::runtime_facts_invalid);
    facts = valid_facts;
    facts.resolved_production_sieve_workers = 8;
    expect_error(
        join_siqs_shadow_proof_rss_holdout_streams(&approved, &facts, &off_slot, off_stdout, {}),
        SIQSShadowProofRssHoldoutStreamJoinError::runtime_facts_mismatch);
    facts = valid_facts;
    facts.release_build = false;
    expect_error(
        join_siqs_shadow_proof_rss_holdout_streams(&approved, &facts, &off_slot, off_stdout, {}),
        SIQSShadowProofRssHoldoutStreamJoinError::release_ndebug_required);
    expect_error(join_siqs_shadow_proof_rss_holdout_streams(&approved, &valid_facts, nullptr,
                                                            off_stdout, {}),
                 SIQSShadowProofRssHoldoutStreamJoinError::slot_invalid);
    auto changed_slot = off_slot;
    changed_slot.ordinal = 2;
    expect_error(join(approved, valid_facts, changed_slot, off_stdout, {}),
                 SIQSShadowProofRssHoldoutStreamJoinError::slot_invalid);

    expect_error(join(approved, valid_facts, off_slot, "invalid\n", {}),
                 SIQSShadowProofRssHoldoutStreamJoinError::stdout_invalid);
    CHECK(join(approved, valid_facts, off_slot, "invalid\n", {}).stdout_diagnostic.error !=
          SIQSShadowProofRssHoldoutProbeRecordCodecError::none);
    expect_error(join(approved, valid_facts, observe_slot, off_stdout, observe_stderr),
                 SIQSShadowProofRssHoldoutStreamJoinError::stdout_binding_mismatch);
    const std::string wrong_workers =
        emit_probe(make_probe_record(off_slot, ProcessMemoryBackend::DarwinGetrusage, 8));
    expect_error(join(approved, valid_facts, off_slot, wrong_workers, {}),
                 SIQSShadowProofRssHoldoutStreamJoinError::stdout_binding_mismatch);
    const std::string wrong_backend =
        emit_probe(make_probe_record(off_slot, ProcessMemoryBackend::LinuxGetrusage));
    expect_error(join(approved, valid_facts, off_slot, wrong_backend, {}),
                 SIQSShadowProofRssHoldoutStreamJoinError::stdout_binding_mismatch);
    auto wrong_off_rows_record = make_probe_record(off_slot);
    --wrong_off_rows_record.relations_found;
    expect_error(join(approved, valid_facts, off_slot, emit_probe(wrong_off_rows_record), {}),
                 SIQSShadowProofRssHoldoutStreamJoinError::stdout_binding_mismatch);
    expect_error(join(approved, valid_facts, off_slot, off_stdout, "unexpected\n"),
                 SIQSShadowProofRssHoldoutStreamJoinError::stderr_presence_invalid);
    expect_error(join(approved, valid_facts, observe_slot, observe_stdout, {}),
                 SIQSShadowProofRssHoldoutStreamJoinError::stderr_presence_invalid);
    expect_error(join(approved, valid_facts, observe_slot, observe_stdout, "invalid\n"),
                 SIQSShadowProofRssHoldoutStreamJoinError::stderr_invalid);
    CHECK(join(approved, valid_facts, observe_slot, observe_stdout, "invalid\n")
              .stderr_diagnostic.error != SIQSShadowProofObserveRecordError::none);
}

void test_observe_semantic_failures() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto plan = make_siqs_shadow_proof_rss_campaign_plan(&policy);
    const auto& slot = plan.slots[3];
    const std::string stdout_bytes = emit_probe(make_probe_record(slot));
    const auto expect_record_error = [&](const SIQSShadowProofObserveRecord& record,
                                         SIQSShadowProofRssHoldoutStreamJoinError error) {
        expect_error(join(policy, facts, slot, stdout_bytes, emit_observe(record)), error);
    };

    auto record = make_observe_record();
    record.proof_attempted = false;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_terminal_invalid);
    record = make_observe_record();
    record.matrix_workers = 2;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_profile_invalid);
    record = make_observe_record();
    ++record.large_prime_bound;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_profile_invalid);
    record = make_observe_record();
    record.adapter_full_relations--;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_adapter_invalid);
    record = make_observe_record();
    record.graph_cycles--;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_graph_invalid);
    record = make_observe_record();
    record.graph_cycle_incidences = 2000;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_graph_invalid);
    record = make_observe_record();
    ++record.graph_components;
    ++record.graph_vertices;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_graph_invalid);
    record = make_observe_record();
    record.graph_max_cycle_length = 3;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_graph_invalid);
    record = make_observe_record();
    record.assembly_selected_cycle_rows--;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_assembly_invalid);
    record = make_observe_record();
    record.projected_dense_bytes++;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_matrix_invalid);
    record = make_observe_record();
    record.factor_found_count = 0;
    expect_record_error(record,
                        SIQSShadowProofRssHoldoutStreamJoinError::observe_dependency_invalid);
    record = make_observe_record();
    record.before_memory.backend = ProcessMemoryBackend::LinuxGetrusage;
    record.after_memory.backend = ProcessMemoryBackend::LinuxGetrusage;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_memory_invalid);
    record = make_observe_record();
    record.before_memory.lifetime_peak_rss_bytes = UINT64_C(900);
    record.after_memory.lifetime_peak_rss_bytes = UINT64_C(800);
    record.peak_growth_supported = false;
    record.peak_growth_bytes = 0;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::observe_memory_invalid);
    record = make_observe_record();
    record.observe_wall_ns = UINT64_C(1'000'000'001);
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::cross_stream_mismatch);
    record = make_observe_record();
    record.after_memory.lifetime_peak_rss_bytes = UINT64_C(1100);
    record.peak_growth_bytes = 800;
    expect_record_error(record, SIQSShadowProofRssHoldoutStreamJoinError::cross_stream_mismatch);

    record = make_observe_record();
    --record.adapter_full_relations;
    record.adapter_rejected_relations = 1;
    record.adapter_exact_duplicate = 1;
    --record.row_candidate_upper;
    CHECK(join(policy, facts, slot, stdout_bytes, emit_observe(record)));
}

void test_fingerprint_and_projection_binding() {
    const auto policy = make_policy();
    const auto facts = make_facts();
    const auto plan = make_siqs_shadow_proof_rss_campaign_plan(&policy);
    const auto& first = plan.slots[0];
    const auto& second = plan.slots[1];
    const auto first_result = join(policy, facts, first, emit_probe(make_probe_record(first)), {});
    const auto second_result =
        join(policy, facts, second, emit_probe(make_probe_record(second)), {});
    CHECK(first_result);
    CHECK(second_result);
    if (first_result && second_result) {
        CHECK(first_result.draft->stdout_fingerprint.digest !=
              second_result.draft->stdout_fingerprint.digest);
        CHECK(first_result.draft->joined_bytes != second_result.draft->joined_bytes);
        CHECK(first_result.draft->stderr_fingerprint.digest ==
              second_result.draft->stderr_fingerprint.digest);
        CHECK(first_result.draft->stderr_fingerprint.byte_count == 0);
    }
}

} // namespace

int main() {
    test_error_names();
    test_outcome_blind_fixture_profiles();
    test_all_slots_and_draft_contract();
    test_preflight_and_stream_failures();
    test_observe_semantic_failures();
    test_fingerprint_and_projection_binding();

    std::cout << "SIQS RSS holdout stream join: " << checks_passed << " passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
