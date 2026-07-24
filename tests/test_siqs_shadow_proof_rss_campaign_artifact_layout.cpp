// Pure contract tests for the SIQS RSS campaign artifact layout.
// This test never opens a directory, launches a process, or samples live RSS.

#include <gnfs/siqs/shadow_proof_rss_campaign_artifact_layout.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace gnfs::siqs;
using gnfs::util::ProcessMemoryBackend;

using Address = SIQSShadowProofRssCampaignArtifactAddress;
using ConsistencyError = SIQSShadowProofRssCampaignArtifactConsistencyError;
using Entry = SIQSShadowProofRssCampaignArtifactLayoutEntry;
using EntryKind = SIQSShadowProofRssCampaignArtifactLayoutEntryKind;
using LayoutError = SIQSShadowProofRssCampaignArtifactLayoutError;
using Record = SIQSShadowProofRssCampaignJournalRecord;

static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_DIRECTORY_LEAF == ".artifacts-v1");
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES == 240);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_LEAF_SIZE == 27);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_MAX_BYTES == 4096);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_MAX_BYTES == 16384);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_MAX_BYTES == 4096);

constexpr auto CONSTEXPR_STDOUT_LEAF = make_siqs_shadow_proof_rss_campaign_artifact_leaf(
    1, SIQSShadowProofRssArtifactKind::probe_stdout);
static_assert(CONSTEXPR_STDOUT_LEAF.has_value());
static_assert(CONSTEXPR_STDOUT_LEAF->view() == "slot-0000000001-stdout.rssa");
static_assert(parse_siqs_shadow_proof_rss_campaign_artifact_leaf(CONSTEXPR_STDOUT_LEAF->view()) ==
              Address{1, SIQSShadowProofRssArtifactKind::probe_stdout});

constexpr std::array CONSTEXPR_LAYOUT_ENTRIES{
    Entry{
        .leaf_name = "slot-0000000001-stdout.rssa",
        .kind = EntryKind::regular_file,
        .link_count = 1,
        .observed_size = 1,
        .bytes = "x",
    },
    Entry{
        .leaf_name = "slot-0000000001-stderr.rssa",
        .kind = EntryKind::regular_file,
        .link_count = 1,
        .observed_size = 0,
        .bytes = "",
    },
};
constexpr auto CONSTEXPR_LAYOUT =
    inspect_siqs_shadow_proof_rss_campaign_artifact_layout(CONSTEXPR_LAYOUT_ENTRIES);
static_assert(static_cast<bool>(CONSTEXPR_LAYOUT));
static_assert(CONSTEXPR_LAYOUT.value->artifact_count == 2);
static_assert(
    CONSTEXPR_LAYOUT.value->seal(1, SIQSShadowProofRssArtifactKind::probe_stdout).has_value());
static_assert(
    CONSTEXPR_LAYOUT.value->seal(1, SIQSShadowProofRssArtifactKind::probe_stderr)->byte_count == 0);

int checks_passed = 0;
int checks_failed = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) {
        ++checks_passed;
        return;
    }
    ++checks_failed;
    std::cerr << "FAIL: " << expression << " at " << __FILE__ << ':' << line << '\n';
}

#define CHECK(condition) check(static_cast<bool>(condition), #condition, __LINE__)

struct OwnedEntry final {
    std::string leaf_name;
    EntryKind kind = EntryKind::regular_file;
    uint64_t link_count = 1;
    uint64_t observed_size = 0;
    std::string bytes;
};

[[nodiscard]] std::string artifact_leaf(uint32_t slot_number, SIQSShadowProofRssArtifactKind kind) {
    const auto leaf = make_siqs_shadow_proof_rss_campaign_artifact_leaf(slot_number, kind);
    CHECK(leaf.has_value());
    if (!leaf.has_value()) {
        std::terminate();
    }
    return std::string(leaf->view());
}

[[nodiscard]] OwnedEntry make_entry(uint32_t slot_number, SIQSShadowProofRssArtifactKind kind,
                                    std::string bytes,
                                    EntryKind entry_kind = EntryKind::regular_file,
                                    uint64_t link_count = 1,
                                    std::optional<uint64_t> observed_size = std::nullopt) {
    const uint64_t size = observed_size.value_or(static_cast<uint64_t>(bytes.size()));
    return {
        .leaf_name = artifact_leaf(slot_number, kind),
        .kind = entry_kind,
        .link_count = link_count,
        .observed_size = size,
        .bytes = std::move(bytes),
    };
}

[[nodiscard]] auto inspect(const std::vector<OwnedEntry>& owned_entries) {
    std::vector<Entry> entries;
    entries.reserve(owned_entries.size());
    for (const auto& owned : owned_entries) {
        entries.push_back({
            .leaf_name = owned.leaf_name,
            .kind = owned.kind,
            .link_count = owned.link_count,
            .observed_size = owned.observed_size,
            .bytes = owned.bytes,
        });
    }
    return inspect_siqs_shadow_proof_rss_campaign_artifact_layout(entries);
}

void expect_layout_error(
    const SIQSShadowProofRssCampaignArtifactLayoutResult& result, LayoutError error,
    uint32_t slot_number = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT,
    SIQSShadowProofRssArtifactKind kind = SIQSShadowProofRssArtifactKind::unknown) {
    CHECK(!static_cast<bool>(result));
    CHECK(!result.value.has_value());
    CHECK(result.diagnostic.error == error);
    CHECK(result.diagnostic.slot_number == slot_number);
    CHECK(result.diagnostic.kind == kind);
}

void expect_layout_success(const SIQSShadowProofRssCampaignArtifactLayoutResult& result,
                           std::size_t artifact_count) {
    CHECK(static_cast<bool>(result));
    CHECK(result.value.has_value());
    CHECK(result.diagnostic.error == LayoutError::none);
    CHECK(result.diagnostic.slot_number == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT);
    CHECK(result.diagnostic.kind == SIQSShadowProofRssArtifactKind::unknown);
    if (result.value.has_value()) {
        CHECK(result.value->artifact_count == artifact_count);
    }
}

void expect_consistency(
    const SIQSShadowProofRssCampaignArtifactConsistencyResult& result,
    ConsistencyError error = ConsistencyError::none,
    uint32_t slot_number = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_NO_SLOT,
    SIQSShadowProofRssArtifactKind kind = SIQSShadowProofRssArtifactKind::unknown) {
    CHECK(static_cast<bool>(result) == (error == ConsistencyError::none));
    CHECK(result.diagnostic.error == error);
    CHECK(result.diagnostic.slot_number == slot_number);
    CHECK(result.diagnostic.kind == kind);
}

void test_leaf_domain_is_exact() {
    constexpr std::array kinds{
        SIQSShadowProofRssArtifactKind::probe_stdout,
        SIQSShadowProofRssArtifactKind::probe_stderr,
        SIQSShadowProofRssArtifactKind::joined_gate_sample,
    };
    for (uint32_t slot = 1; slot <= SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT; ++slot) {
        for (const auto kind : kinds) {
            const auto leaf = make_siqs_shadow_proof_rss_campaign_artifact_leaf(slot, kind);
            CHECK(leaf.has_value());
            if (!leaf.has_value()) {
                continue;
            }
            CHECK(leaf->view().size() == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_LEAF_SIZE);
            CHECK((parse_siqs_shadow_proof_rss_campaign_artifact_leaf(leaf->view()) ==
                   Address{slot, kind}));
        }
    }

    CHECK(artifact_leaf(1, SIQSShadowProofRssArtifactKind::probe_stderr) ==
          "slot-0000000001-stderr.rssa");
    CHECK(artifact_leaf(80, SIQSShadowProofRssArtifactKind::joined_gate_sample) ==
          "slot-0000000080-joined.rssa");
    CHECK(!make_siqs_shadow_proof_rss_campaign_artifact_leaf(
               0, SIQSShadowProofRssArtifactKind::probe_stdout)
               .has_value());
    CHECK(!make_siqs_shadow_proof_rss_campaign_artifact_leaf(
               81, SIQSShadowProofRssArtifactKind::probe_stdout)
               .has_value());
    CHECK(!make_siqs_shadow_proof_rss_campaign_artifact_leaf(
               1, SIQSShadowProofRssArtifactKind::unknown)
               .has_value());
    CHECK(!make_siqs_shadow_proof_rss_campaign_artifact_leaf(
               1, static_cast<SIQSShadowProofRssArtifactKind>(255))
               .has_value());

    std::vector<std::string> invalid{
        "",
        ".artifacts-v1",
        "Slot-0000000001-stdout.rssa",
        "slot-000000001-stdout.rssa",
        "slot-00000000001-stdout.rssa",
        "slot-0000000000-stdout.rssa",
        "slot-0000000081-stdout.rssa",
        "slot-00000x0001-stdout.rssa",
        "slot-0000000001_stdout.rssa",
        "slot-0000000001-STDOUT.rssa",
        "slot-0000000001-output.rssa",
        "slot-0000000001-stdout.RSSA",
        "slot-0000000001-stdout.rssa.tmp",
        "../slot-0000000001-stdout.rssa",
        "nested/slot-0000000001-stdout.rssa",
        "slot-0000000001-stdout.rssa/child",
    };
    auto embedded_nul = std::string("slot-0000000001-stdout.rssa");
    embedded_nul[10] = '\0';
    invalid.push_back(std::move(embedded_nul));
    for (const auto& leaf : invalid) {
        CHECK(!parse_siqs_shadow_proof_rss_campaign_artifact_leaf(leaf).has_value());
    }
}

void test_layout_bounds_and_snapshot_seals() {
    expect_layout_success(inspect({}), 0);

    std::vector<OwnedEntry> all;
    all.reserve(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES);
    for (uint32_t slot = 1; slot <= SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT; ++slot) {
        all.push_back(make_entry(slot, SIQSShadowProofRssArtifactKind::probe_stdout, "stdout"));
        all.push_back(make_entry(slot, SIQSShadowProofRssArtifactKind::probe_stderr, ""));
        all.push_back(
            make_entry(slot, SIQSShadowProofRssArtifactKind::joined_gate_sample, "joined"));
    }
    const auto full = inspect(all);
    expect_layout_success(full, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES);
    if (full.value.has_value()) {
        CHECK(full.value->seal(1, SIQSShadowProofRssArtifactKind::probe_stdout) ==
              seal_siqs_shadow_proof_rss_artifact(SIQSShadowProofRssArtifactKind::probe_stdout,
                                                  "stdout"));
        CHECK(
            full.value->seal(80, SIQSShadowProofRssArtifactKind::probe_stderr) ==
            seal_siqs_shadow_proof_rss_artifact(SIQSShadowProofRssArtifactKind::probe_stderr, ""));
        CHECK(full.value->seal(80, SIQSShadowProofRssArtifactKind::joined_gate_sample) ==
              seal_siqs_shadow_proof_rss_artifact(
                  SIQSShadowProofRssArtifactKind::joined_gate_sample, "joined"));
        CHECK(!full.value->seal(0, SIQSShadowProofRssArtifactKind::probe_stdout).has_value());
        CHECK(!full.value->seal(81, SIQSShadowProofRssArtifactKind::probe_stdout).has_value());
        CHECK(!full.value->seal(1, SIQSShadowProofRssArtifactKind::unknown).has_value());
    }

    auto too_many = all;
    too_many.push_back(make_entry(1, SIQSShadowProofRssArtifactKind::probe_stdout, "duplicate"));
    expect_layout_error(inspect(too_many), LayoutError::too_many_entries);

    auto binary = std::string{"a\0b", 3};
    const auto binary_result =
        inspect({make_entry(1, SIQSShadowProofRssArtifactKind::probe_stdout, binary)});
    expect_layout_success(binary_result, 1);
    if (binary_result.value.has_value()) {
        CHECK(binary_result.value->seal(1, SIQSShadowProofRssArtifactKind::probe_stdout) ==
              seal_siqs_shadow_proof_rss_artifact(SIQSShadowProofRssArtifactKind::probe_stdout,
                                                  binary));
    }
}

void test_strict_naming_duplicates_type_and_links() {
    auto unknown = make_entry(1, SIQSShadowProofRssArtifactKind::probe_stdout, "x");
    unknown.leaf_name = "unknown.rssa";
    expect_layout_error(inspect({unknown}), LayoutError::unknown_entry);

    auto duplicate = std::vector<OwnedEntry>{
        make_entry(1, SIQSShadowProofRssArtifactKind::probe_stdout, "first"),
        make_entry(1, SIQSShadowProofRssArtifactKind::probe_stdout, "second"),
    };
    expect_layout_error(inspect(duplicate), LayoutError::duplicate_artifact, 1,
                        SIQSShadowProofRssArtifactKind::probe_stdout);
    std::reverse(duplicate.begin(), duplicate.end());
    expect_layout_error(inspect(duplicate), LayoutError::duplicate_artifact, 1,
                        SIQSShadowProofRssArtifactKind::probe_stdout);

    auto unknown_and_duplicate = duplicate;
    unknown_and_duplicate.push_back(unknown);
    expect_layout_error(inspect(unknown_and_duplicate), LayoutError::unknown_entry);
    std::reverse(unknown_and_duplicate.begin(), unknown_and_duplicate.end());
    expect_layout_error(inspect(unknown_and_duplicate), LayoutError::unknown_entry);

    constexpr std::array bad_kinds{
        EntryKind::unknown, EntryKind::directory,        EntryKind::link_or_reparse_point,
        EntryKind::other,   static_cast<EntryKind>(255),
    };
    for (const auto entry_kind : bad_kinds) {
        expect_layout_error(
            inspect({make_entry(2, SIQSShadowProofRssArtifactKind::probe_stderr, "", entry_kind)}),
            LayoutError::entry_not_regular_file, 2, SIQSShadowProofRssArtifactKind::probe_stderr);
    }
    for (const uint64_t link_count :
         {UINT64_C(0), UINT64_C(2), std::numeric_limits<uint64_t>::max()}) {
        expect_layout_error(
            inspect({make_entry(3, SIQSShadowProofRssArtifactKind::joined_gate_sample, "x",
                                EntryKind::regular_file, link_count)}),
            LayoutError::link_count_invalid, 3, SIQSShadowProofRssArtifactKind::joined_gate_sample);
    }

    auto duplicate_and_bad_type = duplicate;
    duplicate_and_bad_type[0].kind = EntryKind::directory;
    expect_layout_error(inspect(duplicate_and_bad_type), LayoutError::duplicate_artifact, 1,
                        SIQSShadowProofRssArtifactKind::probe_stdout);
}

void test_size_contracts() {
    expect_layout_success(
        inspect({make_entry(1, SIQSShadowProofRssArtifactKind::probe_stdout, "x")}), 1);
    expect_layout_success(
        inspect({make_entry(
            1, SIQSShadowProofRssArtifactKind::probe_stdout,
            std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_MAX_BYTES, 'x'))}),
        1);
    expect_layout_error(inspect({make_entry(1, SIQSShadowProofRssArtifactKind::probe_stdout, "")}),
                        LayoutError::artifact_size_invalid, 1,
                        SIQSShadowProofRssArtifactKind::probe_stdout);
    expect_layout_error(
        inspect({make_entry(
            1, SIQSShadowProofRssArtifactKind::probe_stdout,
            std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDOUT_MAX_BYTES + 1, 'x'))}),
        LayoutError::artifact_size_invalid, 1, SIQSShadowProofRssArtifactKind::probe_stdout);

    expect_layout_success(
        inspect({make_entry(2, SIQSShadowProofRssArtifactKind::probe_stderr, "")}), 1);
    expect_layout_success(
        inspect({make_entry(
            2, SIQSShadowProofRssArtifactKind::probe_stderr,
            std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_MAX_BYTES, 'x'))}),
        1);
    expect_layout_error(
        inspect({make_entry(
            2, SIQSShadowProofRssArtifactKind::probe_stderr,
            std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_STDERR_MAX_BYTES + 1, 'x'))}),
        LayoutError::artifact_size_invalid, 2, SIQSShadowProofRssArtifactKind::probe_stderr);

    expect_layout_success(
        inspect({make_entry(3, SIQSShadowProofRssArtifactKind::joined_gate_sample, "x")}), 1);
    expect_layout_success(
        inspect({make_entry(
            3, SIQSShadowProofRssArtifactKind::joined_gate_sample,
            std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_MAX_BYTES, 'x'))}),
        1);
    expect_layout_error(
        inspect({make_entry(3, SIQSShadowProofRssArtifactKind::joined_gate_sample, "")}),
        LayoutError::artifact_size_invalid, 3, SIQSShadowProofRssArtifactKind::joined_gate_sample);
    expect_layout_error(
        inspect({make_entry(
            3, SIQSShadowProofRssArtifactKind::joined_gate_sample,
            std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_JOINED_MAX_BYTES + 1, 'x'))}),
        LayoutError::artifact_size_invalid, 3, SIQSShadowProofRssArtifactKind::joined_gate_sample);

    for (const auto kind : {SIQSShadowProofRssArtifactKind::probe_stdout,
                            SIQSShadowProofRssArtifactKind::probe_stderr,
                            SIQSShadowProofRssArtifactKind::joined_gate_sample}) {
        const std::string bytes = kind == SIQSShadowProofRssArtifactKind::probe_stderr ? "" : "x";
        expect_layout_error(inspect({make_entry(4, kind, bytes, EntryKind::regular_file, 1,
                                                std::numeric_limits<uint64_t>::max())}),
                            LayoutError::artifact_size_invalid, 4, kind);
        expect_layout_error(inspect({make_entry(4, kind, bytes, EntryKind::regular_file, 1,
                                                static_cast<uint64_t>(bytes.size() + 1))}),
                            LayoutError::artifact_size_invalid, 4, kind);
    }

    auto two_bad_sizes = std::vector<OwnedEntry>{
        make_entry(2, SIQSShadowProofRssArtifactKind::probe_stdout, ""),
        make_entry(1, SIQSShadowProofRssArtifactKind::probe_stdout, ""),
    };
    expect_layout_error(inspect(two_bad_sizes), LayoutError::artifact_size_invalid, 1,
                        SIQSShadowProofRssArtifactKind::probe_stdout);
    std::reverse(two_bad_sizes.begin(), two_bad_sizes.end());
    expect_layout_error(inspect(two_bad_sizes), LayoutError::artifact_size_invalid, 1,
                        SIQSShadowProofRssArtifactKind::probe_stdout);
}

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
        .probe_kind = SIQSShadowProofRssProbeKind::production_holdout,
        .candidate_revision = "candidate-revision-1",
        .release_build = true,
        .ndebug = true,
    };
}

[[nodiscard]] std::string slot_bytes(uint32_t slot, std::string_view label) {
    return std::string(label) + "-" + std::to_string(slot);
}

[[nodiscard]] SIQSShadowProofRssJournalCommitPayload
make_payload(SIQSShadowProofRssSampleMode mode, uint32_t slot, std::vector<OwnedEntry>& artifacts,
             SIQSShadowProofRssProbeKind probe_kind) {
    const std::string stdout_bytes = slot_bytes(slot, "stdout");
    const std::string stderr_bytes =
        mode == SIQSShadowProofRssSampleMode::off ? "" : slot_bytes(slot, "stderr");
    const std::string joined_bytes = slot_bytes(slot, "joined");
    artifacts.push_back(
        make_entry(slot, SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes));
    artifacts.push_back(
        make_entry(slot, SIQSShadowProofRssArtifactKind::probe_stderr, stderr_bytes));
    artifacts.push_back(
        make_entry(slot, SIQSShadowProofRssArtifactKind::joined_gate_sample, joined_bytes));

    SIQSShadowProofRssJournalCommitPayload payload;
    payload.actual_operating_system = SIQSShadowProofRssOperatingSystem::darwin;
    payload.actual_architecture = SIQSShadowProofRssArchitecture::arm64;
    payload.actual_memory_backend = ProcessMemoryBackend::DarwinGetrusage;
    payload.actual_resolved_sieve_workers = 4;
    payload.deployment_probe_kind = probe_kind;
    payload.fresh_process = true;
    payload.completed = true;
    payload.factor_identity = SIQSShadowProofRssFactorIdentity::pass;
    payload.proof_evidence = mode == SIQSShadowProofRssSampleMode::off
                                 ? SIQSShadowProofRssEvidence::not_applicable
                                 : SIQSShadowProofRssEvidence::pass;
    payload.matrix_evidence = payload.proof_evidence;
    payload.absolute_peak_rss_bytes = UINT64_C(1000000) + slot;
    payload.wall_ns = UINT64_C(1000) + slot;
    payload.stdout_seal = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::probe_stdout, stdout_bytes);
    payload.stderr_seal = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::probe_stderr, stderr_bytes);
    payload.joined_sample_seal = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::joined_gate_sample, joined_bytes);
    return payload;
}

[[nodiscard]] Record make_commit(const Record& start,
                                 const SIQSShadowProofRssJournalCommitPayload& payload) {
    Record commit;
    commit.schema_version = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION;
    commit.sequence_number = start.sequence_number + 1;
    commit.kind = SIQSShadowProofRssJournalRecordKind::slot_committed;
    commit.previous_record_digest = start.record_digest;
    commit.plan_digest = start.plan_digest;
    commit.slot_number = start.slot_number;
    commit.slot_digest = start.slot_digest;
    commit.commit_payload = payload;
    commit.record_digest = shadow_proof_rss_campaign_journal_detail::record_digest(commit);
    return commit;
}

struct JournalFixture final {
    SIQSShadowProofRssGatePolicy policy = make_policy();
    SIQSShadowProofRssCampaignRuntimeFacts facts = make_facts();
    SIQSShadowProofRssCampaignJournalHeader header;
    std::vector<Record> records;
    std::vector<OwnedEntry> artifacts;

    explicit JournalFixture(
        SIQSShadowProofRssProbeKind probe_kind = SIQSShadowProofRssProbeKind::production_holdout) {
        facts.probe_kind = probe_kind;
        auto absent = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {});
        CHECK(absent.header_to_create.has_value());
        if (!absent.header_to_create.has_value()) {
            std::terminate();
        }
        header = *absent.header_to_create;
    }

    void append_slot() {
        auto ready = resume();
        CHECK(ready.status == SIQSShadowProofRssJournalStatus::ready);
        CHECK(ready.prepared_slot_start.has_value());
        if (!ready.prepared_slot_start.has_value()) {
            std::terminate();
        }
        const Record start = ready.prepared_slot_start->record();
        records.push_back(start);
        const auto plan = make_siqs_shadow_proof_rss_campaign_plan(&policy);
        const auto payload = make_payload(plan.slots[start.slot_number - 1].mode, start.slot_number,
                                          artifacts, facts.probe_kind);
        records.push_back(make_commit(start, payload));
    }

    [[nodiscard]] SIQSShadowProofRssCampaignJournalResume resume() const {
        return resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, records);
    }

    [[nodiscard]] SIQSShadowProofRssCampaignArtifactLayoutSnapshot artifact_snapshot() const {
        const auto layout = inspect(artifacts);
        CHECK(layout);
        if (!layout.value.has_value()) {
            std::terminate();
        }
        return *layout.value;
    }
};

void test_ready_and_complete_consistency() {
    JournalFixture fixture;
    const auto empty_snapshot = fixture.artifact_snapshot();
    const auto create_header = resume_siqs_shadow_proof_rss_campaign_journal(
        &fixture.policy, &fixture.facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {});
    expect_consistency(validate_siqs_shadow_proof_rss_campaign_artifact_consistency(
        create_header, empty_snapshot));
    expect_consistency(validate_siqs_shadow_proof_rss_campaign_artifact_consistency(
        fixture.resume(), empty_snapshot));

    fixture.append_slot();
    const auto ready = fixture.resume();
    CHECK(ready.status == SIQSShadowProofRssJournalStatus::ready);
    CHECK(ready.committed_slot_count == 1);
    const auto one_slot_snapshot = fixture.artifact_snapshot();
    expect_consistency(
        validate_siqs_shadow_proof_rss_campaign_artifact_consistency(ready, one_slot_snapshot));

    auto missing = one_slot_snapshot;
    missing.seals[1].reset();
    --missing.artifact_count;
    expect_consistency(validate_siqs_shadow_proof_rss_campaign_artifact_consistency(ready, missing),
                       ConsistencyError::committed_artifact_missing, 1,
                       SIQSShadowProofRssArtifactKind::probe_stderr);

    auto mismatched = one_slot_snapshot;
    mismatched.seals[2] = seal_siqs_shadow_proof_rss_artifact(
        SIQSShadowProofRssArtifactKind::joined_gate_sample, "different");
    expect_consistency(
        validate_siqs_shadow_proof_rss_campaign_artifact_consistency(ready, mismatched),
        ConsistencyError::committed_artifact_mismatch, 1,
        SIQSShadowProofRssArtifactKind::joined_gate_sample);

    auto future_entries = fixture.artifacts;
    future_entries.push_back(make_entry(2, SIQSShadowProofRssArtifactKind::probe_stdout, "orphan"));
    const auto future_layout = inspect(future_entries);
    CHECK(future_layout);
    if (future_layout.value.has_value()) {
        expect_consistency(validate_siqs_shadow_proof_rss_campaign_artifact_consistency(
                               ready, *future_layout.value),
                           ConsistencyError::unexpected_artifact, 2,
                           SIQSShadowProofRssArtifactKind::probe_stdout);
    }

    while (fixture.records.size() / 2 < SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT) {
        fixture.append_slot();
    }
    const auto complete = fixture.resume();
    CHECK(complete.status == SIQSShadowProofRssJournalStatus::complete);
    CHECK(complete.committed_slot_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);
    const auto complete_snapshot = fixture.artifact_snapshot();
    CHECK(complete_snapshot.artifact_count == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_ARTIFACT_MAX_ENTRIES);
    expect_consistency(
        validate_siqs_shadow_proof_rss_campaign_artifact_consistency(complete, complete_snapshot));

    JournalFixture synthetic(SIQSShadowProofRssProbeKind::synthetic_test);
    while (synthetic.records.size() / 2 < SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT) {
        synthetic.append_slot();
    }
    const auto synthetic_complete = synthetic.resume();
    CHECK(synthetic_complete.status == SIQSShadowProofRssJournalStatus::complete);
    CHECK(synthetic_complete.reason == SIQSShadowProofRssJournalReason::synthetic_complete);
    CHECK(synthetic_complete.action == SIQSShadowProofRssJournalAction::none);
    expect_consistency(validate_siqs_shadow_proof_rss_campaign_artifact_consistency(
        synthetic_complete, synthetic.artifact_snapshot()));
}

void test_dangling_and_explicit_taint_orphans() {
    JournalFixture fixture;
    fixture.append_slot();
    auto ready = fixture.resume();
    CHECK(ready.prepared_slot_start.has_value());
    if (!ready.prepared_slot_start.has_value()) {
        return;
    }
    fixture.records.push_back(ready.prepared_slot_start->record());
    const auto dangling = fixture.resume();
    CHECK(dangling.status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(dangling.reason == SIQSShadowProofRssJournalReason::dangling_slot_start);

    for (std::size_t orphan_count = 0; orphan_count <= 3; ++orphan_count) {
        auto entries = fixture.artifacts;
        if (orphan_count >= 1) {
            entries.push_back(
                make_entry(2, SIQSShadowProofRssArtifactKind::probe_stdout, "orphan-stdout"));
        }
        if (orphan_count >= 2) {
            entries.push_back(make_entry(2, SIQSShadowProofRssArtifactKind::probe_stderr, ""));
        }
        if (orphan_count >= 3) {
            entries.push_back(
                make_entry(2, SIQSShadowProofRssArtifactKind::joined_gate_sample, "orphan-joined"));
        }
        const auto layout = inspect(entries);
        CHECK(layout);
        if (layout.value.has_value()) {
            expect_consistency(validate_siqs_shadow_proof_rss_campaign_artifact_consistency(
                dangling, *layout.value));
        }
    }

    auto future_entries = fixture.artifacts;
    future_entries.push_back(make_entry(3, SIQSShadowProofRssArtifactKind::probe_stdout, "future"));
    const auto future = inspect(future_entries);
    CHECK(future);
    if (future.value.has_value()) {
        expect_consistency(
            validate_siqs_shadow_proof_rss_campaign_artifact_consistency(dangling, *future.value),
            ConsistencyError::unexpected_artifact, 3, SIQSShadowProofRssArtifactKind::probe_stdout);
    }

    CHECK(dangling.taint_to_append.has_value());
    if (!dangling.taint_to_append.has_value()) {
        return;
    }
    fixture.records.push_back(*dangling.taint_to_append);
    const auto explicit_taint = fixture.resume();
    CHECK(explicit_taint.status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(explicit_taint.reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    auto orphan_entries = fixture.artifacts;
    orphan_entries.push_back(
        make_entry(2, SIQSShadowProofRssArtifactKind::probe_stdout, "orphan-stdout"));
    const auto orphan_layout = inspect(orphan_entries);
    CHECK(orphan_layout);
    if (orphan_layout.value.has_value()) {
        expect_consistency(validate_siqs_shadow_proof_rss_campaign_artifact_consistency(
            explicit_taint, *orphan_layout.value));
    }
}

void test_consistency_rejects_invalid_inputs() {
    JournalFixture fixture;
    fixture.append_slot();
    auto ready = fixture.resume();
    auto snapshot = fixture.artifact_snapshot();

    snapshot.artifact_count = 0;
    expect_consistency(
        validate_siqs_shadow_proof_rss_campaign_artifact_consistency(ready, snapshot),
        ConsistencyError::snapshot_invalid);

    snapshot = fixture.artifact_snapshot();
    snapshot.seals[0]->kind = SIQSShadowProofRssArtifactKind::probe_stderr;
    expect_consistency(
        validate_siqs_shadow_proof_rss_campaign_artifact_consistency(ready, snapshot),
        ConsistencyError::snapshot_invalid, 1, SIQSShadowProofRssArtifactKind::probe_stdout);

    snapshot = fixture.artifact_snapshot();
    ready.status = SIQSShadowProofRssJournalStatus::blocked;
    expect_consistency(
        validate_siqs_shadow_proof_rss_campaign_artifact_consistency(ready, snapshot),
        ConsistencyError::resume_state_invalid);

    ready = fixture.resume();
    ready.next_slot_number = 80;
    expect_consistency(
        validate_siqs_shadow_proof_rss_campaign_artifact_consistency(ready, snapshot),
        ConsistencyError::resume_state_invalid);
}

void test_error_names_are_total() {
    constexpr std::array layout_errors{
        LayoutError::none,
        LayoutError::too_many_entries,
        LayoutError::unknown_entry,
        LayoutError::duplicate_artifact,
        LayoutError::entry_not_regular_file,
        LayoutError::link_count_invalid,
        LayoutError::artifact_size_invalid,
    };
    for (const auto error : layout_errors) {
        CHECK(!siqs_shadow_proof_rss_campaign_artifact_layout_error_name(error).empty());
        CHECK(siqs_shadow_proof_rss_campaign_artifact_layout_error_name(error) != "unknown");
    }
    CHECK(siqs_shadow_proof_rss_campaign_artifact_layout_error_name(
              static_cast<LayoutError>(255)) == "unknown");

    constexpr std::array consistency_errors{
        ConsistencyError::none,
        ConsistencyError::resume_state_invalid,
        ConsistencyError::snapshot_invalid,
        ConsistencyError::committed_artifact_missing,
        ConsistencyError::committed_artifact_mismatch,
        ConsistencyError::unexpected_artifact,
    };
    for (const auto error : consistency_errors) {
        CHECK(!siqs_shadow_proof_rss_campaign_artifact_consistency_error_name(error).empty());
        CHECK(siqs_shadow_proof_rss_campaign_artifact_consistency_error_name(error) != "unknown");
    }
    CHECK(siqs_shadow_proof_rss_campaign_artifact_consistency_error_name(
              static_cast<ConsistencyError>(255)) == "unknown");
}

} // namespace

int main() {
    try {
        test_leaf_domain_is_exact();
        test_layout_bounds_and_snapshot_seals();
        test_strict_naming_duplicates_type_and_links();
        test_size_contracts();
        test_ready_and_complete_consistency();
        test_dangling_and_explicit_taint_orphans();
        test_consistency_rejects_invalid_inputs();
        test_error_names_are_total();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "Unexpected non-standard exception\n";
        return 2;
    }

    std::cout << "SIQS RSS campaign artifact layout: " << checks_passed << " checks passed";
    if (checks_failed != 0) {
        std::cout << ", " << checks_failed << " checks failed\n";
        return 1;
    }
    std::cout << '\n';
    return 0;
}
