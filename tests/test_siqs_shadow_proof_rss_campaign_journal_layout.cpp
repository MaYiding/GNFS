// Pure contract tests for the SIQS RSS campaign journal directory layout.
// This test never opens a directory, launches a process, or samples live RSS.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal_layout.hpp>

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

using CodecError = SIQSShadowProofRssCampaignJournalCodecError;
using Entry = SIQSShadowProofRssCampaignJournalLayoutEntry;
using EntryKind = SIQSShadowProofRssCampaignJournalLayoutEntryKind;
using LayoutError = SIQSShadowProofRssCampaignJournalLayoutError;
using Record = SIQSShadowProofRssCampaignJournalRecord;

static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS == 160);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_ENTRIES == 162);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE == 0);

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

[[nodiscard]] Record make_synthetic_commit(const Record& start,
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

template <std::size_t Size>
[[nodiscard]] std::vector<std::byte> copy_bytes(const std::array<std::byte, Size>& bytes) {
    return {bytes.begin(), bytes.end()};
}

struct JournalFixture final {
    SIQSShadowProofRssGatePolicy policy = make_policy();
    SIQSShadowProofRssCampaignRuntimeFacts facts = make_facts();
    SIQSShadowProofRssCampaignJournalHeader header;
    std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE> header_bytes{};
    std::vector<Record> records;
    std::vector<std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE>>
        record_bytes;

    JournalFixture() {
        auto absent = resume_siqs_shadow_proof_rss_campaign_journal(
            &policy, &facts, SIQSShadowProofRssJournalPresence::absent, nullptr, {});
        CHECK(absent.header_to_create.has_value());
        if (!absent.header_to_create.has_value()) {
            std::terminate();
        }
        header = *absent.header_to_create;

        const auto encoded_header = encode_siqs_shadow_proof_rss_campaign_journal_header(header);
        CHECK(encoded_header.bytes.has_value());
        if (!encoded_header.bytes.has_value()) {
            std::terminate();
        }
        header_bytes = *encoded_header.bytes;

        const auto plan = make_siqs_shadow_proof_rss_campaign_plan(&policy);
        CHECK(plan.status == SIQSShadowProofRssCampaignPlanStatus::ready);
        CHECK(plan.slot_count == SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT);
        records.reserve(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS);
        record_bytes.reserve(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS);

        for (std::size_t slot_index = 0; slot_index < plan.slot_count; ++slot_index) {
            auto ready = resume_siqs_shadow_proof_rss_campaign_journal(
                &policy, &facts, SIQSShadowProofRssJournalPresence::present, &header, records);
            CHECK(ready.status == SIQSShadowProofRssJournalStatus::ready);
            CHECK(ready.prepared_slot_start.has_value());
            if (!ready.prepared_slot_start.has_value()) {
                std::terminate();
            }
            records.push_back(ready.prepared_slot_start->record());

            const auto payload =
                make_payload(plan.slots[slot_index].mode, UINT64_C(1000000) + slot_index);
            records.push_back(make_synthetic_commit(records.back(), payload));
        }

        for (const auto& record : records) {
            const auto encoded = encode_siqs_shadow_proof_rss_campaign_journal_record(record);
            CHECK(encoded.bytes.has_value());
            if (!encoded.bytes.has_value()) {
                std::terminate();
            }
            record_bytes.push_back(*encoded.bytes);
        }
    }
};

struct OwnedEntry final {
    std::string leaf_name;
    EntryKind kind = EntryKind::regular_file;
    uint64_t link_count = 1;
    uint64_t observed_size = 0;
    std::vector<std::byte> bytes;
};

[[nodiscard]] OwnedEntry make_entry(std::string leaf_name, std::vector<std::byte> bytes = {},
                                    EntryKind kind = EntryKind::regular_file,
                                    uint64_t link_count = 1,
                                    std::optional<uint64_t> observed_size = std::nullopt) {
    const auto size = observed_size.value_or(static_cast<uint64_t>(bytes.size()));
    return {
        .leaf_name = std::move(leaf_name),
        .kind = kind,
        .link_count = link_count,
        .observed_size = size,
        .bytes = std::move(bytes),
    };
}

[[nodiscard]] std::string record_leaf(uint32_t sequence_number) {
    const auto leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(sequence_number);
    CHECK(leaf.has_value());
    if (!leaf.has_value()) {
        std::terminate();
    }
    return std::string(leaf->view());
}

[[nodiscard]] std::vector<OwnedEntry> entries_for_prefix(const JournalFixture& fixture,
                                                         std::size_t record_count) {
    CHECK(record_count <= fixture.record_bytes.size());
    std::vector<OwnedEntry> entries;
    entries.reserve(2 + record_count);
    entries.push_back(
        make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF)));
    entries.push_back(make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF),
                                 copy_bytes(fixture.header_bytes)));
    for (std::size_t index = 0; index < record_count; ++index) {
        entries.push_back(make_entry(record_leaf(static_cast<uint32_t>(index + 1)),
                                     copy_bytes(fixture.record_bytes[index])));
    }
    return entries;
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
            .bytes = std::span<const std::byte>(owned.bytes),
        });
    }
    return inspect_siqs_shadow_proof_rss_campaign_journal_layout(entries);
}

template <typename Result>
void expect_layout_error(
    const Result& result, LayoutError error, CodecError codec_error = CodecError::none,
    std::size_t codec_offset = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET,
    uint32_t record_sequence = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE) {
    CHECK(!static_cast<bool>(result));
    CHECK(!result.value.has_value());
    CHECK(result.diagnostic.layout_error == error);
    CHECK(result.diagnostic.codec_error == codec_error);
    CHECK(result.diagnostic.codec_error_offset == codec_offset);
    CHECK(result.diagnostic.record_sequence == record_sequence);
}

template <typename Result> void expect_layout_success(const Result& result) {
    CHECK(static_cast<bool>(result));
    CHECK(result.value.has_value());
    CHECK(result.diagnostic.layout_error == LayoutError::none);
    CHECK(result.diagnostic.codec_error == CodecError::none);
    CHECK(result.diagnostic.codec_error_offset ==
          SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET);
    CHECK(result.diagnostic.record_sequence ==
          SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE);
}

void test_record_leaf_domain_is_exact() {
    for (uint32_t sequence = 1; sequence <= SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS;
         ++sequence) {
        const auto leaf = make_siqs_shadow_proof_rss_campaign_journal_record_leaf(sequence);
        CHECK(leaf.has_value());
        if (!leaf.has_value()) {
            continue;
        }
        CHECK(leaf->view().size() == 22);
        const auto parsed = parse_siqs_shadow_proof_rss_campaign_journal_record_leaf(leaf->view());
        CHECK(parsed.has_value());
        CHECK(parsed == sequence);
    }

    CHECK(record_leaf(1) == "record-0000000001.rjrc");
    CHECK(record_leaf(160) == "record-0000000160.rjrc");
    CHECK(!make_siqs_shadow_proof_rss_campaign_journal_record_leaf(0).has_value());
    CHECK(!make_siqs_shadow_proof_rss_campaign_journal_record_leaf(161).has_value());

    std::vector<std::string> invalid{
        ".Session.lock",
        "Campaign-header.rjhd",
        "campaign-header.RJHD",
        "Record-0000000001.rjrc",
        "record-000000001.rjrc",
        "record-00000000001.rjrc",
        "record-0000000000.rjrc",
        "record-0000000161.rjrc",
        "record-00000x0001.rjrc",
        "record-0000000001.RJRC",
        "record-0000000001.rjrc.tmp",
        "record-0000000001.tmp",
        "record-+000000001.rjrc",
        "record--000000001.rjrc",
        "record- 000000001.rjrc",
        "record-9999999999.rjrc",
        ".record-0000000001.rjrc",
        ".DS_Store",
        "../record-0000000001.rjrc",
        "nested/record-0000000001.rjrc",
        "nested\\record-0000000001.rjrc",
        "record-0000000001.rjrc/child",
        "record-00000000\xC3\xA9.rjrc",
    };
    auto embedded_nul = std::string("record-0000000001.rjrc");
    embedded_nul[10] = '\0';
    invalid.push_back(std::move(embedded_nul));

    for (const auto& leaf : invalid) {
        CHECK(!parse_siqs_shadow_proof_rss_campaign_journal_record_leaf(leaf).has_value());
        std::vector<OwnedEntry> entries{
            make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF)),
            make_entry(leaf),
        };
        expect_layout_error(inspect(entries), LayoutError::unknown_entry);
    }
}

void test_absent_present_and_structural_rejection(const JournalFixture& fixture) {
    std::vector<OwnedEntry> lock_only{
        make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF)),
    };
    const auto absent = inspect(lock_only);
    expect_layout_success(absent);
    if (absent.value.has_value()) {
        CHECK(absent.value->presence == SIQSShadowProofRssJournalPresence::absent);
        CHECK(!absent.value->header.has_value());
        CHECK(absent.value->record_count == 0);
    }

    expect_layout_error(inspect({}), LayoutError::session_lock_missing);

    std::vector<OwnedEntry> header_without_lock{
        make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF),
                   copy_bytes(fixture.header_bytes)),
    };
    expect_layout_error(inspect(header_without_lock), LayoutError::session_lock_missing);

    const auto header_only = inspect(entries_for_prefix(fixture, 0));
    expect_layout_success(header_only);
    if (header_only.value.has_value()) {
        CHECK(header_only.value->presence == SIQSShadowProofRssJournalPresence::present);
        CHECK(header_only.value->header == fixture.header);
        CHECK(header_only.value->record_count == 0);
    }

    auto record_without_header = std::vector<OwnedEntry>{
        make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF)),
        make_entry(record_leaf(1), copy_bytes(fixture.record_bytes[0])),
    };
    expect_layout_error(inspect(record_without_header), LayoutError::record_without_header);

    auto duplicate_lock = entries_for_prefix(fixture, 0);
    duplicate_lock.push_back(
        make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF)));
    expect_layout_error(inspect(duplicate_lock), LayoutError::duplicate_session_lock);

    auto duplicate_header = entries_for_prefix(fixture, 0);
    duplicate_header.push_back(
        make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_LEAF),
                   copy_bytes(fixture.header_bytes)));
    expect_layout_error(inspect(duplicate_header), LayoutError::duplicate_header);

    auto duplicate_record = entries_for_prefix(fixture, 1);
    duplicate_record.push_back(make_entry(record_leaf(1), copy_bytes(fixture.record_bytes[0])));
    expect_layout_error(inspect(duplicate_record), LayoutError::duplicate_record, CodecError::none,
                        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET, 1);

    auto gap = entries_for_prefix(fixture, 0);
    gap.push_back(make_entry(record_leaf(2), copy_bytes(fixture.record_bytes[1])));
    expect_layout_error(inspect(gap), LayoutError::record_gap, CodecError::none,
                        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET, 1);
}

void test_input_order_and_capacity_are_deterministic(const JournalFixture& fixture) {
    const auto canonical_entries = entries_for_prefix(fixture, 20);
    const auto canonical = inspect(canonical_entries);
    expect_layout_success(canonical);

    auto reversed_entries = canonical_entries;
    std::reverse(reversed_entries.begin(), reversed_entries.end());
    const auto reversed = inspect(reversed_entries);
    expect_layout_success(reversed);

    auto shuffled_entries = canonical_entries;
    std::vector<OwnedEntry> deterministic_shuffle;
    deterministic_shuffle.reserve(shuffled_entries.size());
    for (std::size_t index = 0; index < shuffled_entries.size(); index += 2) {
        deterministic_shuffle.push_back(std::move(shuffled_entries[index]));
    }
    for (std::size_t index = 1; index < shuffled_entries.size(); index += 2) {
        deterministic_shuffle.push_back(std::move(shuffled_entries[index]));
    }
    const auto shuffled = inspect(deterministic_shuffle);
    expect_layout_success(shuffled);

    for (const auto* result : {&canonical, &reversed, &shuffled}) {
        if (!result->value.has_value()) {
            continue;
        }
        CHECK(result->value->record_count == 20);
        for (std::size_t index = 0; index < result->value->record_count; ++index) {
            CHECK(result->value->records[index] == fixture.records[index]);
        }
    }

    const auto full_entries =
        entries_for_prefix(fixture, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS);
    CHECK(full_entries.size() == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_ENTRIES);
    const auto full = inspect(full_entries);
    expect_layout_success(full);
    if (full.value.has_value()) {
        CHECK(full.value->record_count == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS);
        CHECK(full.value->records[159] == fixture.records[159]);

        auto caller_mutated = *full.value;
        caller_mutated.record_count = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS + 1;
        CHECK(caller_mutated.record_span().size() ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS);
        caller_mutated.record_count = std::numeric_limits<std::size_t>::max();
        CHECK(caller_mutated.record_span().size() ==
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS);
    }

    auto over_capacity = full_entries;
    over_capacity.push_back(make_entry("unexpected"));
    CHECK(over_capacity.size() == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_ENTRIES + 1);
    expect_layout_error(inspect(over_capacity), LayoutError::too_many_entries);
}

void test_entry_kind_link_count_and_sizes(const JournalFixture& fixture) {
    const std::array bad_kinds{
        EntryKind::unknown, EntryKind::directory,        EntryKind::link_or_reparse_point,
        EntryKind::other,   static_cast<EntryKind>(255),
    };
    for (const auto kind : bad_kinds) {
        for (std::size_t entry_index = 0; entry_index < 3; ++entry_index) {
            auto entries = entries_for_prefix(fixture, 1);
            entries[entry_index].kind = kind;
            expect_layout_error(
                inspect(entries), LayoutError::entry_not_regular_file, CodecError::none,
                SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET,
                entry_index == 2 ? 1 : SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE);
        }
    }

    for (const uint64_t link_count :
         {UINT64_C(0), UINT64_C(2), std::numeric_limits<uint64_t>::max()}) {
        for (std::size_t entry_index = 0; entry_index < 3; ++entry_index) {
            auto entries = entries_for_prefix(fixture, 1);
            entries[entry_index].link_count = link_count;
            expect_layout_error(
                inspect(entries), LayoutError::link_count_invalid, CodecError::none,
                SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET,
                entry_index == 2 ? 1 : SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_RECORD_SEQUENCE);
        }
    }

    auto nonempty_lock = entries_for_prefix(fixture, 0);
    nonempty_lock[0].bytes.push_back(std::byte{1});
    nonempty_lock[0].observed_size = 1;
    expect_layout_error(inspect(nonempty_lock), LayoutError::session_lock_not_empty);

    auto lock_bytes_mismatch = entries_for_prefix(fixture, 0);
    lock_bytes_mismatch[0].bytes.push_back(std::byte{1});
    expect_layout_error(inspect(lock_bytes_mismatch), LayoutError::session_lock_not_empty);

    auto header_observed_size = entries_for_prefix(fixture, 0);
    header_observed_size[1].observed_size =
        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE - 1;
    expect_layout_error(inspect(header_observed_size), LayoutError::header_size_invalid);

    auto header_bytes_size = entries_for_prefix(fixture, 0);
    header_bytes_size[1].bytes.pop_back();
    expect_layout_error(inspect(header_bytes_size), LayoutError::header_size_invalid);

    auto record_observed_size = entries_for_prefix(fixture, 1);
    record_observed_size[2].observed_size =
        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE - 1;
    expect_layout_error(inspect(record_observed_size), LayoutError::record_size_invalid,
                        CodecError::none, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET,
                        1);

    auto record_bytes_size = entries_for_prefix(fixture, 1);
    record_bytes_size[2].bytes.pop_back();
    expect_layout_error(inspect(record_bytes_size), LayoutError::record_size_invalid,
                        CodecError::none, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET,
                        1);

    for (const uint64_t size :
         {UINT64_C(0),
          static_cast<uint64_t>(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE + 1),
          std::numeric_limits<uint64_t>::max()}) {
        auto entries = entries_for_prefix(fixture, 0);
        entries[1].observed_size = size;
        expect_layout_error(inspect(entries), LayoutError::header_size_invalid);
    }
    for (const uint64_t size :
         {UINT64_C(0),
          static_cast<uint64_t>(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE + 1),
          std::numeric_limits<uint64_t>::max()}) {
        auto entries = entries_for_prefix(fixture, 1);
        entries[2].observed_size = size;
        expect_layout_error(inspect(entries), LayoutError::record_size_invalid, CodecError::none,
                            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET, 1);
    }
}

void test_wire_diagnostics_and_filename_binding(const JournalFixture& fixture) {
    auto bad_header = entries_for_prefix(fixture, 0);
    bad_header[1].bytes[SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET] = std::byte{0xff};
    expect_layout_error(inspect(bad_header), LayoutError::header_codec_invalid,
                        CodecError::unsupported_wire_version,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET);

    auto bad_record = entries_for_prefix(fixture, 1);
    bad_record[2].bytes[SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET] = std::byte{0xff};
    expect_layout_error(inspect(bad_record), LayoutError::record_codec_invalid,
                        CodecError::invalid_record_kind,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET, 1);

    auto filename_wire_mismatch = entries_for_prefix(fixture, 0);
    filename_wire_mismatch.push_back(
        make_entry(record_leaf(1), copy_bytes(fixture.record_bytes[1])));
    expect_layout_error(inspect(filename_wire_mismatch), LayoutError::record_sequence_mismatch,
                        CodecError::none, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET,
                        1);

    auto duplicate_wire_sequence = entries_for_prefix(fixture, 2);
    duplicate_wire_sequence[3].bytes = duplicate_wire_sequence[2].bytes;
    expect_layout_error(inspect(duplicate_wire_sequence), LayoutError::record_sequence_mismatch,
                        CodecError::none, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET,
                        2);

    auto swapped_wire_sequences = entries_for_prefix(fixture, 2);
    std::swap(swapped_wire_sequences[2].bytes, swapped_wire_sequences[3].bytes);
    expect_layout_error(inspect(swapped_wire_sequences), LayoutError::record_sequence_mismatch,
                        CodecError::none, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET,
                        1);
}

void test_error_precedence_is_order_independent(const JournalFixture& fixture) {
    auto unknown_and_duplicate = entries_for_prefix(fixture, 0);
    unknown_and_duplicate.push_back(
        make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF)));
    unknown_and_duplicate.push_back(make_entry("unexpected.tmp"));
    expect_layout_error(inspect(unknown_and_duplicate), LayoutError::unknown_entry);
    std::reverse(unknown_and_duplicate.begin(), unknown_and_duplicate.end());
    expect_layout_error(inspect(unknown_and_duplicate), LayoutError::unknown_entry);

    auto duplicate_and_bad_kind = entries_for_prefix(fixture, 0);
    duplicate_and_bad_kind[1].kind = EntryKind::directory;
    duplicate_and_bad_kind.push_back(
        make_entry(std::string(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SESSION_LOCK_LEAF)));
    expect_layout_error(inspect(duplicate_and_bad_kind), LayoutError::duplicate_session_lock);
    std::reverse(duplicate_and_bad_kind.begin(), duplicate_and_bad_kind.end());
    expect_layout_error(inspect(duplicate_and_bad_kind), LayoutError::duplicate_session_lock);

    auto two_bad_sizes = entries_for_prefix(fixture, 2);
    two_bad_sizes[2].observed_size = 0;
    two_bad_sizes[3].observed_size = 0;
    expect_layout_error(inspect(two_bad_sizes), LayoutError::record_size_invalid, CodecError::none,
                        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET, 1);
    std::reverse(two_bad_sizes.begin(), two_bad_sizes.end());
    expect_layout_error(inspect(two_bad_sizes), LayoutError::record_size_invalid, CodecError::none,
                        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET, 1);

    auto mismatch_then_codec_error = entries_for_prefix(fixture, 2);
    mismatch_then_codec_error[2].bytes = copy_bytes(fixture.record_bytes[1]);
    mismatch_then_codec_error[3].bytes[SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET] =
        std::byte{0xff};
    expect_layout_error(inspect(mismatch_then_codec_error), LayoutError::record_codec_invalid,
                        CodecError::invalid_record_kind,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET, 2);
    std::reverse(mismatch_then_codec_error.begin(), mismatch_then_codec_error.end());
    expect_layout_error(inspect(mismatch_then_codec_error), LayoutError::record_codec_invalid,
                        CodecError::invalid_record_kind,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET, 2);
}

void test_layout_error_names_are_total() {
    constexpr std::array errors{
        LayoutError::none,
        LayoutError::too_many_entries,
        LayoutError::unknown_entry,
        LayoutError::duplicate_session_lock,
        LayoutError::duplicate_header,
        LayoutError::duplicate_record,
        LayoutError::session_lock_missing,
        LayoutError::entry_not_regular_file,
        LayoutError::link_count_invalid,
        LayoutError::session_lock_not_empty,
        LayoutError::header_size_invalid,
        LayoutError::record_size_invalid,
        LayoutError::record_without_header,
        LayoutError::record_gap,
        LayoutError::header_codec_invalid,
        LayoutError::record_codec_invalid,
        LayoutError::record_sequence_mismatch,
    };
    for (const auto error : errors) {
        CHECK(!siqs_shadow_proof_rss_campaign_journal_layout_error_name(error).empty());
        CHECK(siqs_shadow_proof_rss_campaign_journal_layout_error_name(error) != "unknown");
    }
    CHECK(siqs_shadow_proof_rss_campaign_journal_layout_error_name(static_cast<LayoutError>(255)) ==
          "unknown");
}

void test_every_prefix_closes_through_replay(const JournalFixture& fixture) {
    for (std::size_t prefix = 0; prefix <= SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS;
         ++prefix) {
        const auto layout = inspect(entries_for_prefix(fixture, prefix));
        expect_layout_success(layout);
        if (!layout.value.has_value() || !layout.value->header.has_value()) {
            continue;
        }
        const auto& snapshot = *layout.value;
        const auto replay = resume_siqs_shadow_proof_rss_campaign_journal(
            &fixture.policy, &fixture.facts, snapshot.presence, &*snapshot.header,
            snapshot.record_span());
        CHECK(replay.committed_slot_count == prefix / 2);
        CHECK(!replay.header_to_create.has_value());
        if (prefix == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS) {
            CHECK(replay.status == SIQSShadowProofRssJournalStatus::complete);
            CHECK(replay.reason == SIQSShadowProofRssJournalReason::complete);
            CHECK(replay.action == SIQSShadowProofRssJournalAction::evaluate_gate);
            CHECK(!replay.prepared_slot_start.has_value());
            CHECK(!replay.taint_to_append.has_value());
        } else if (prefix % 2 == 0) {
            CHECK(replay.status == SIQSShadowProofRssJournalStatus::ready);
            CHECK(replay.reason == SIQSShadowProofRssJournalReason::ready);
            CHECK(replay.action == SIQSShadowProofRssJournalAction::append_slot_start);
            CHECK(replay.prepared_slot_start.has_value());
            CHECK(!replay.taint_to_append.has_value());
        } else {
            CHECK(replay.status == SIQSShadowProofRssJournalStatus::tainted);
            CHECK(replay.reason == SIQSShadowProofRssJournalReason::dangling_slot_start);
            CHECK(replay.action == SIQSShadowProofRssJournalAction::append_taint);
            CHECK(!replay.prepared_slot_start.has_value());
            CHECK(replay.taint_to_append.has_value());
        }
    }

    auto dangling_entries = entries_for_prefix(fixture, 1);
    const auto dangling_layout = inspect(dangling_entries);
    CHECK(dangling_layout.value.has_value());
    if (!dangling_layout.value.has_value() || !dangling_layout.value->header.has_value()) {
        return;
    }
    const auto dangling_replay = resume_siqs_shadow_proof_rss_campaign_journal(
        &fixture.policy, &fixture.facts, dangling_layout.value->presence,
        &*dangling_layout.value->header, dangling_layout.value->record_span());
    CHECK(dangling_replay.taint_to_append.has_value());
    if (!dangling_replay.taint_to_append.has_value()) {
        return;
    }

    const auto encoded_taint =
        encode_siqs_shadow_proof_rss_campaign_journal_record(*dangling_replay.taint_to_append);
    CHECK(encoded_taint.bytes.has_value());
    if (!encoded_taint.bytes.has_value()) {
        return;
    }
    dangling_entries.push_back(make_entry(record_leaf(2), copy_bytes(*encoded_taint.bytes)));
    const auto tainted_layout = inspect(dangling_entries);
    expect_layout_success(tainted_layout);
    if (!tainted_layout.value.has_value() || !tainted_layout.value->header.has_value()) {
        return;
    }
    const auto tainted_replay = resume_siqs_shadow_proof_rss_campaign_journal(
        &fixture.policy, &fixture.facts, tainted_layout.value->presence,
        &*tainted_layout.value->header, tainted_layout.value->record_span());
    CHECK(tainted_replay.status == SIQSShadowProofRssJournalStatus::tainted);
    CHECK(tainted_replay.reason == SIQSShadowProofRssJournalReason::explicitly_tainted);
    CHECK(tainted_replay.action == SIQSShadowProofRssJournalAction::none);
    CHECK(!tainted_replay.prepared_slot_start.has_value());
    CHECK(!tainted_replay.taint_to_append.has_value());
}

} // namespace

int main() {
    test_record_leaf_domain_is_exact();
    test_layout_error_names_are_total();

    const JournalFixture fixture;
    CHECK(fixture.records.size() == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS);
    CHECK(fixture.record_bytes.size() == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_MAX_RECORDS);

    test_absent_present_and_structural_rejection(fixture);
    test_input_order_and_capacity_are_deterministic(fixture);
    test_entry_kind_link_count_and_sizes(fixture);
    test_wire_diagnostics_and_filename_binding(fixture);
    test_error_precedence_is_order_independent(fixture);
    test_every_prefix_closes_through_replay(fixture);

    std::cout << "SIQS shadow-proof RSS journal layout: " << checks_passed << " checks passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
