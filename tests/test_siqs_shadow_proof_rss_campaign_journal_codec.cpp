#include <gnfs/siqs/shadow_proof_rss_campaign_journal_codec.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace gnfs::siqs;
using gnfs::util::ProcessMemoryBackend;

using CodecError = SIQSShadowProofRssCampaignJournalCodecError;
using Header = SIQSShadowProofRssCampaignJournalHeader;
using Record = SIQSShadowProofRssCampaignJournalRecord;

static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE == 96);
static_assert(SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE == 256);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_ARTIFACT_SEAL_WIRE_SIZE == 32);

static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_MAGIC_OFFSET == 0);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET == 8);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET == 12);

static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET == 16);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_POLICY_DIGEST_OFFSET == 24);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_RUNTIME_DIGEST_OFFSET == 40);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PLAN_DIGEST_OFFSET == 56);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SLOT_COUNT_OFFSET == 72);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_CONCURRENCY_OFFSET == 76);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET == 80);

static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET == 16);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET == 20);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET == 24);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PREVIOUS_DIGEST_OFFSET == 32);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PLAN_DIGEST_OFFSET == 48);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_NUMBER_OFFSET == 64);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_DIGEST_OFFSET == 72);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PAYLOAD_OFFSET == 88);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET == 96);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET == 100);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET == 104);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET == 112);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_CURRENT_RSS_OFFSET == 120);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PEAK_GROWTH_OFFSET == 128);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WALL_NS_OFFSET == 136);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET == 144);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET == 176);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET == 208);
static_assert(SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET == 240);

int checks_passed = 0;
int checks_failed = 0;

void expect(bool condition, const char* expression, int line) {
    if (condition) {
        ++checks_passed;
        return;
    }
    ++checks_failed;
    std::cerr << "FAIL: " << expression << " at " << __FILE__ << ':' << line << '\n';
}

#define EXPECT(condition) expect(static_cast<bool>(condition), #condition, __LINE__)

template <std::size_t Size>
[[nodiscard]] uint8_t byte_at(const std::array<std::byte, Size>& bytes,
                              std::size_t offset) noexcept {
    return std::to_integer<uint8_t>(bytes[offset]);
}

template <std::size_t Size>
void put_u8(std::array<std::byte, Size>& bytes, std::size_t offset, uint8_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value);
}

template <std::size_t Size>
void put_u32(std::array<std::byte, Size>& bytes, std::size_t offset, uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        put_u8(bytes, offset++, static_cast<uint8_t>(value >> shift));
    }
}

template <std::size_t Size>
void put_u64(std::array<std::byte, Size>& bytes, std::size_t offset, uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        put_u8(bytes, offset++, static_cast<uint8_t>(value >> shift));
    }
}

template <std::size_t Size>
void put_magic(std::array<std::byte, Size>& bytes, std::string_view magic) noexcept {
    EXPECT(magic.size() == 8);
    for (std::size_t index = 0; index < magic.size(); ++index) {
        put_u8(bytes, index, static_cast<uint8_t>(static_cast<unsigned char>(magic[index])));
    }
}

template <std::size_t Size>
void put_digest(std::array<std::byte, Size>& bytes, std::size_t offset,
                SIQSShadowProofRssCorpusDigest digest) noexcept {
    put_u64(bytes, offset, digest.low);
    put_u64(bytes, offset + 8, digest.high);
}

template <std::size_t Size>
[[nodiscard]] uint32_t get_u32(const std::array<std::byte, Size>& bytes,
                               std::size_t offset) noexcept {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(byte_at(bytes, offset++)) << shift;
    }
    return value;
}

template <std::size_t Size>
[[nodiscard]] uint64_t get_u64(const std::array<std::byte, Size>& bytes,
                               std::size_t offset) noexcept {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(byte_at(bytes, offset++)) << shift;
    }
    return value;
}

[[nodiscard]] constexpr uint8_t record_kind_tag(SIQSShadowProofRssJournalRecordKind value) {
    switch (value) {
    case SIQSShadowProofRssJournalRecordKind::slot_started:
        return 1;
    case SIQSShadowProofRssJournalRecordKind::slot_committed:
        return 2;
    case SIQSShadowProofRssJournalRecordKind::campaign_tainted:
        return 3;
    case SIQSShadowProofRssJournalRecordKind::unknown:
        break;
    }
    return 0xff;
}

[[nodiscard]] constexpr uint8_t operating_system_tag(SIQSShadowProofRssOperatingSystem value) {
    switch (value) {
    case SIQSShadowProofRssOperatingSystem::unknown:
        return 0;
    case SIQSShadowProofRssOperatingSystem::darwin:
        return 1;
    case SIQSShadowProofRssOperatingSystem::linux:
        return 2;
    case SIQSShadowProofRssOperatingSystem::windows:
        return 3;
    }
    return 0xff;
}

[[nodiscard]] constexpr uint8_t architecture_tag(SIQSShadowProofRssArchitecture value) {
    switch (value) {
    case SIQSShadowProofRssArchitecture::unknown:
        return 0;
    case SIQSShadowProofRssArchitecture::x86_64:
        return 1;
    case SIQSShadowProofRssArchitecture::arm64:
        return 2;
    }
    return 0xff;
}

[[nodiscard]] constexpr uint8_t memory_backend_tag(ProcessMemoryBackend value) {
    switch (value) {
    case ProcessMemoryBackend::Unsupported:
        return 0;
    case ProcessMemoryBackend::DarwinGetrusage:
        return 1;
    case ProcessMemoryBackend::LinuxGetrusage:
        return 2;
    case ProcessMemoryBackend::WindowsPsapi:
        return 3;
    }
    return 0xff;
}

[[nodiscard]] constexpr uint8_t factor_identity_tag(SIQSShadowProofRssFactorIdentity value) {
    switch (value) {
    case SIQSShadowProofRssFactorIdentity::unknown:
        return 0;
    case SIQSShadowProofRssFactorIdentity::pass:
        return 1;
    case SIQSShadowProofRssFactorIdentity::fail:
        return 2;
    case SIQSShadowProofRssFactorIdentity::not_checked:
        return 3;
    }
    return 0xff;
}

[[nodiscard]] constexpr uint8_t evidence_tag(SIQSShadowProofRssEvidence value) {
    switch (value) {
    case SIQSShadowProofRssEvidence::unknown:
        return 0;
    case SIQSShadowProofRssEvidence::not_applicable:
        return 1;
    case SIQSShadowProofRssEvidence::pass:
        return 2;
    case SIQSShadowProofRssEvidence::fail:
        return 3;
    }
    return 0xff;
}

[[nodiscard]] constexpr uint8_t artifact_kind_tag(SIQSShadowProofRssArtifactKind value) {
    switch (value) {
    case SIQSShadowProofRssArtifactKind::unknown:
        return 0;
    case SIQSShadowProofRssArtifactKind::probe_stdout:
        return 1;
    case SIQSShadowProofRssArtifactKind::probe_stderr:
        return 2;
    case SIQSShadowProofRssArtifactKind::joined_gate_sample:
        return 3;
    }
    return 0xff;
}

template <std::size_t Size>
void put_artifact(std::array<std::byte, Size>& bytes, std::size_t offset,
                  const SIQSShadowProofRssArtifactSeal& seal) noexcept {
    put_u8(bytes, offset, seal.committed ? 1 : 0);
    put_u8(bytes, offset + 1, artifact_kind_tag(seal.kind));
    put_u64(bytes, offset + 8, seal.byte_count);
    put_digest(bytes, offset + 16, seal.digest);
}

void reseal(Header& header) {
    header.header_digest = shadow_proof_rss_campaign_journal_detail::header_digest(header);
}

void reseal(Record& record) {
    record.record_digest = shadow_proof_rss_campaign_journal_detail::record_digest(record);
}

[[nodiscard]] Header make_header() {
    Header header;
    header.schema_version = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION;
    header.policy_binding_digest = {UINT64_C(0x0102030405060708), UINT64_C(0x1112131415161718)};
    header.runtime_facts_digest = {UINT64_C(0x2122232425262728), UINT64_C(0x3132333435363738)};
    header.plan_digest = {UINT64_C(0x4142434445464748), UINT64_C(0x5152535455565758)};
    header.slot_count = 80;
    header.max_concurrency = 1;
    reseal(header);
    return header;
}

[[nodiscard]] SIQSShadowProofRssArtifactSeal make_artifact(bool committed,
                                                           SIQSShadowProofRssArtifactKind kind,
                                                           uint64_t byte_count, uint64_t digest_low,
                                                           uint64_t digest_high) {
    return {
        .committed = committed,
        .kind = kind,
        .byte_count = byte_count,
        .digest = {digest_low, digest_high},
    };
}

[[nodiscard]] Record make_record() {
    Record record;
    record.schema_version = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION;
    record.sequence_number = UINT32_C(0x01020304);
    record.kind = SIQSShadowProofRssJournalRecordKind::slot_committed;
    record.previous_record_digest = {UINT64_C(0x0102030405060708), UINT64_C(0x1112131415161718)};
    record.plan_digest = {UINT64_C(0x2122232425262728), UINT64_C(0x3132333435363738)};
    record.slot_number = UINT32_C(0xa1b2c3d4);
    record.slot_digest = {UINT64_C(0x4142434445464748), UINT64_C(0x5152535455565758)};

    auto& payload = record.commit_payload;
    payload.actual_operating_system = SIQSShadowProofRssOperatingSystem::windows;
    payload.actual_architecture = SIQSShadowProofRssArchitecture::arm64;
    payload.actual_memory_backend = ProcessMemoryBackend::WindowsPsapi;
    payload.actual_resolved_sieve_workers = UINT32_C(0x01020304);
    payload.fresh_process = true;
    payload.completed = false;
    payload.factor_identity = SIQSShadowProofRssFactorIdentity::not_checked;
    payload.proof_evidence = SIQSShadowProofRssEvidence::pass;
    payload.matrix_evidence = SIQSShadowProofRssEvidence::fail;
    payload.absolute_peak_rss_bytes = UINT64_C(0x1122334455667788);
    payload.observe_minus_off_peak_bytes = -INT64_C(0x0102030405060708);
    payload.current_rss_bytes = UINT64_C(0x99aabbccddeeff00);
    payload.peak_growth_bytes = UINT64_C(0x8877665544332211);
    payload.wall_ns = UINT64_C(0x1020304050607080);
    payload.stdout_seal = make_artifact(true, SIQSShadowProofRssArtifactKind::probe_stdout,
                                        UINT64_C(0x0102030405060708), UINT64_C(0x1112131415161718),
                                        UINT64_C(0x2122232425262728));
    payload.stderr_seal = make_artifact(false, SIQSShadowProofRssArtifactKind::probe_stderr,
                                        UINT64_C(0x3132333435363738), UINT64_C(0x4142434445464748),
                                        UINT64_C(0x5152535455565758));
    payload.joined_sample_seal = make_artifact(
        true, SIQSShadowProofRssArtifactKind::joined_gate_sample, UINT64_C(0x6162636465666768),
        UINT64_C(0x7172737475767778), UINT64_C(0x8182838485868788));
    reseal(record);
    return record;
}

[[nodiscard]] std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE>
golden_header_wire(const Header& header) {
    std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE> bytes{};
    put_magic(bytes, "GNFSRJHD");
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_WIRE_VERSION);
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE);
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET, header.schema_version);
    put_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_POLICY_DIGEST_OFFSET,
               header.policy_binding_digest);
    put_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_RUNTIME_DIGEST_OFFSET,
               header.runtime_facts_digest);
    put_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PLAN_DIGEST_OFFSET, header.plan_digest);
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SLOT_COUNT_OFFSET, header.slot_count);
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_CONCURRENCY_OFFSET, header.max_concurrency);
    put_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET, header.header_digest);
    return bytes;
}

[[nodiscard]] std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE>
golden_record_wire(const Record& record) {
    std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE> bytes{};
    put_magic(bytes, "GNFSRJRC");
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_WIRE_VERSION);
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE);
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET, record.schema_version);
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET, record.sequence_number);
    put_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET, record_kind_tag(record.kind));
    put_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PREVIOUS_DIGEST_OFFSET,
               record.previous_record_digest);
    put_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PLAN_DIGEST_OFFSET, record.plan_digest);
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_NUMBER_OFFSET, record.slot_number);
    put_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_DIGEST_OFFSET, record.slot_digest);

    const auto& payload = record.commit_payload;
    put_u8(bytes, 88, operating_system_tag(payload.actual_operating_system));
    put_u8(bytes, 89, architecture_tag(payload.actual_architecture));
    put_u8(bytes, 90, memory_backend_tag(payload.actual_memory_backend));
    put_u8(bytes, 91, payload.fresh_process ? 1 : 0);
    put_u8(bytes, 92, payload.completed ? 1 : 0);
    put_u8(bytes, 93, factor_identity_tag(payload.factor_identity));
    put_u8(bytes, 94, evidence_tag(payload.proof_evidence));
    put_u8(bytes, 95, evidence_tag(payload.matrix_evidence));
    put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET,
            static_cast<uint32_t>(payload.actual_resolved_sieve_workers));

    uint8_t optional_mask = 0;
    optional_mask |= payload.absolute_peak_rss_bytes.has_value() ? 0x01U : 0;
    optional_mask |= payload.observe_minus_off_peak_bytes.has_value() ? 0x02U : 0;
    optional_mask |= payload.current_rss_bytes.has_value() ? 0x04U : 0;
    optional_mask |= payload.peak_growth_bytes.has_value() ? 0x08U : 0;
    optional_mask |= payload.wall_ns.has_value() ? 0x10U : 0;
    put_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET, optional_mask);
    put_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET,
            payload.absolute_peak_rss_bytes.value_or(0));
    put_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET,
            std::bit_cast<uint64_t>(payload.observe_minus_off_peak_bytes.value_or(0)));
    put_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_CURRENT_RSS_OFFSET,
            payload.current_rss_bytes.value_or(0));
    put_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PEAK_GROWTH_OFFSET,
            payload.peak_growth_bytes.value_or(0));
    put_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WALL_NS_OFFSET,
            payload.wall_ns.value_or(0));
    put_artifact(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET,
                 payload.stdout_seal);
    put_artifact(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET,
                 payload.stderr_seal);
    put_artifact(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET,
                 payload.joined_sample_seal);
    put_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET, record.record_digest);
    return bytes;
}

template <std::size_t Size>
void expect_encode_error(const SIQSShadowProofRssCampaignJournalEncodeResult<Size>& result,
                         CodecError error, std::size_t offset) {
    EXPECT(!result);
    EXPECT(!result.bytes.has_value());
    EXPECT(result.error == error);
    EXPECT(result.error_offset == offset);
}

template <typename Value>
void expect_decode_error(const SIQSShadowProofRssCampaignJournalDecodeResult<Value>& result,
                         CodecError error, std::size_t offset) {
    EXPECT(!result);
    EXPECT(!result.value.has_value());
    EXPECT(result.error == error);
    EXPECT(result.error_offset == offset);
}

template <std::size_t Size>
void expect_zero_range(const std::array<std::byte, Size>& bytes, std::size_t begin,
                       std::size_t end) {
    for (std::size_t offset = begin; offset < end; ++offset) {
        EXPECT(byte_at(bytes, offset) == 0);
    }
}

[[nodiscard]] std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE>
encoded_header(const Header& header) {
    const auto encoded = encode_siqs_shadow_proof_rss_campaign_journal_header(header);
    EXPECT(encoded);
    EXPECT(encoded.error == CodecError::none);
    EXPECT(encoded.error_offset == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET);
    return encoded.bytes.value_or(
        std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE>{});
}

[[nodiscard]] std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE>
encoded_record(const Record& record) {
    const auto encoded = encode_siqs_shadow_proof_rss_campaign_journal_record(record);
    EXPECT(encoded);
    EXPECT(encoded.error == CodecError::none);
    EXPECT(encoded.error_offset == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET);
    return encoded.bytes.value_or(
        std::array<std::byte, SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE>{});
}

void expect_record_round_trip(Record record) {
    reseal(record);
    const auto bytes = encoded_record(record);
    const auto decoded = decode_siqs_shadow_proof_rss_campaign_journal_record(bytes);
    EXPECT(decoded);
    EXPECT(decoded.error == CodecError::none);
    EXPECT(decoded.error_offset == SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET);
    EXPECT(decoded.value.has_value());
    if (decoded.value.has_value()) {
        EXPECT(*decoded.value == record);
    }
}

void test_fixed_width_golden_layout_and_round_trip() {
    const auto header = make_header();
    const auto header_bytes = encoded_header(header);
    EXPECT(header_bytes.size() == 96);
    EXPECT(header_bytes == golden_header_wire(header));
    EXPECT(byte_at(header_bytes, 0) == 'G');
    EXPECT(byte_at(header_bytes, 7) == 'D');
    EXPECT(byte_at(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET) == 0x60);
    EXPECT(byte_at(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET + 1) == 0x00);
    EXPECT(byte_at(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_POLICY_DIGEST_OFFSET) ==
           0x08);
    EXPECT(byte_at(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_POLICY_DIGEST_OFFSET + 7) ==
           0x01);
    expect_zero_range(header_bytes, 20, 24);

    const auto decoded_header = decode_siqs_shadow_proof_rss_campaign_journal_header(header_bytes);
    EXPECT(decoded_header);
    EXPECT(decoded_header.value.has_value());
    if (decoded_header.value.has_value()) {
        EXPECT(*decoded_header.value == header);
    }

    const auto record = make_record();
    const auto record_bytes = encoded_record(record);
    EXPECT(record_bytes.size() == 256);
    EXPECT(record_bytes == golden_record_wire(record));
    EXPECT(byte_at(record_bytes, 0) == 'G');
    EXPECT(byte_at(record_bytes, 7) == 'C');
    EXPECT(byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET) == 0x00);
    EXPECT(byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET + 1) == 0x01);
    EXPECT(byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET) == 0x04);
    EXPECT(byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET + 1) == 0x03);
    EXPECT(byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET + 2) == 0x02);
    EXPECT(byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET + 3) == 0x01);
    EXPECT(byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PREVIOUS_DIGEST_OFFSET) ==
           0x08);
    EXPECT(byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PREVIOUS_DIGEST_OFFSET + 7) ==
           0x01);
    EXPECT(get_u32(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET) ==
           UINT32_C(0x01020304));
    EXPECT(get_u64(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET) ==
           UINT64_C(0x1122334455667788));
    EXPECT(get_u64(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET) ==
           std::bit_cast<uint64_t>(-INT64_C(0x0102030405060708)));
    expect_zero_range(record_bytes, 25, 32);
    expect_zero_range(record_bytes, 68, 72);
    expect_zero_range(record_bytes, 101, 104);
    for (const std::size_t seal_offset :
         {SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET,
          SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET,
          SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET}) {
        expect_zero_range(record_bytes, seal_offset + 2, seal_offset + 8);
    }

    const auto decoded_record = decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes);
    EXPECT(decoded_record);
    EXPECT(decoded_record.value.has_value());
    if (decoded_record.value.has_value()) {
        EXPECT(*decoded_record.value == record);
    }
}

void test_frame_size_magic_version_schema_and_reserved_rejection() {
    const auto header_bytes = encoded_header(make_header());
    expect_decode_error(
        decode_siqs_shadow_proof_rss_campaign_journal_header(
            std::span<const std::byte>(header_bytes).first(header_bytes.size() - 1)),
        CodecError::truncated, header_bytes.size() - 1);
    auto header_extra = std::vector<std::byte>(header_bytes.begin(), header_bytes.end());
    header_extra.push_back(std::byte{0});
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(header_extra),
                        CodecError::trailing_bytes, header_bytes.size());

    auto changed_header = header_bytes;
    put_u8(changed_header, 3, static_cast<uint8_t>('X'));
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(changed_header),
                        CodecError::invalid_magic, 3);
    changed_header = header_bytes;
    put_u32(changed_header, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_WIRE_VERSION + 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(changed_header),
                        CodecError::unsupported_wire_version,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET);
    changed_header = header_bytes;
    put_u32(changed_header, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE - 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(changed_header),
                        CodecError::declared_size_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET);
    changed_header = header_bytes;
    put_u32(changed_header, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION + 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(changed_header),
                        CodecError::unsupported_journal_schema_version,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET);
    for (std::size_t offset = 20; offset < 24; ++offset) {
        changed_header = header_bytes;
        put_u8(changed_header, offset, 1);
        expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(changed_header),
                            CodecError::nonzero_reserved, offset);
    }

    const auto record_bytes = encoded_record(make_record());
    expect_decode_error(
        decode_siqs_shadow_proof_rss_campaign_journal_record(
            std::span<const std::byte>(record_bytes).first(record_bytes.size() - 1)),
        CodecError::truncated, record_bytes.size() - 1);
    auto record_extra = std::vector<std::byte>(record_bytes.begin(), record_bytes.end());
    record_extra.push_back(std::byte{0});
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_extra),
                        CodecError::trailing_bytes, record_bytes.size());

    auto changed_record = record_bytes;
    put_u8(changed_record, 6, static_cast<uint8_t>('X'));
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(changed_record),
                        CodecError::invalid_magic, 6);
    changed_record = record_bytes;
    put_u32(changed_record, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_WIRE_VERSION + 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(changed_record),
                        CodecError::unsupported_wire_version,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET);
    changed_record = record_bytes;
    put_u32(changed_record, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE + 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(changed_record),
                        CodecError::declared_size_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET);
    changed_record = record_bytes;
    put_u32(changed_record, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION + 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(changed_record),
                        CodecError::unsupported_journal_schema_version,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET);

    const std::array reserved_ranges{
        std::pair<std::size_t, std::size_t>{25, 32},
        std::pair<std::size_t, std::size_t>{68, 72},
        std::pair<std::size_t, std::size_t>{101, 104},
        std::pair<std::size_t, std::size_t>{
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET + 2,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET + 8},
        std::pair<std::size_t, std::size_t>{
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET + 2,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET + 8},
        std::pair<std::size_t, std::size_t>{
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET + 2,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET + 8},
    };
    for (const auto [begin, end] : reserved_ranges) {
        for (std::size_t offset = begin; offset < end; ++offset) {
            changed_record = record_bytes;
            put_u8(changed_record, offset, 1);
            expect_decode_error(
                decode_siqs_shadow_proof_rss_campaign_journal_record(changed_record),
                CodecError::nonzero_reserved, offset);
        }
    }

    auto bad_header = make_header();
    ++bad_header.schema_version;
    reseal(bad_header);
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_header(bad_header),
                        CodecError::unsupported_journal_schema_version,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET);
    auto bad_record = make_record();
    ++bad_record.schema_version;
    reseal(bad_record);
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(bad_record),
                        CodecError::unsupported_journal_schema_version,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET);
}

void test_all_enum_and_boolean_domains() {
    const std::array record_kind_cases{
        std::pair{SIQSShadowProofRssJournalRecordKind::slot_started, uint8_t{1}},
        std::pair{SIQSShadowProofRssJournalRecordKind::slot_committed, uint8_t{2}},
        std::pair{SIQSShadowProofRssJournalRecordKind::campaign_tainted, uint8_t{3}},
    };
    for (const auto [value, tag] : record_kind_cases) {
        auto record = make_record();
        record.kind = value;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT(byte_at(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET) == tag);
        expect_record_round_trip(record);
    }

    const std::array operating_system_cases{
        std::pair{SIQSShadowProofRssOperatingSystem::unknown, uint8_t{0}},
        std::pair{SIQSShadowProofRssOperatingSystem::darwin, uint8_t{1}},
        std::pair{SIQSShadowProofRssOperatingSystem::linux, uint8_t{2}},
        std::pair{SIQSShadowProofRssOperatingSystem::windows, uint8_t{3}},
    };
    for (const auto [value, tag] : operating_system_cases) {
        auto record = make_record();
        record.commit_payload.actual_operating_system = value;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT(byte_at(bytes, 88) == tag);
        expect_record_round_trip(record);
    }

    const std::array architecture_cases{
        std::pair{SIQSShadowProofRssArchitecture::unknown, uint8_t{0}},
        std::pair{SIQSShadowProofRssArchitecture::x86_64, uint8_t{1}},
        std::pair{SIQSShadowProofRssArchitecture::arm64, uint8_t{2}},
    };
    for (const auto [value, tag] : architecture_cases) {
        auto record = make_record();
        record.commit_payload.actual_architecture = value;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT(byte_at(bytes, 89) == tag);
        expect_record_round_trip(record);
    }

    const std::array backend_cases{
        std::pair{ProcessMemoryBackend::Unsupported, uint8_t{0}},
        std::pair{ProcessMemoryBackend::DarwinGetrusage, uint8_t{1}},
        std::pair{ProcessMemoryBackend::LinuxGetrusage, uint8_t{2}},
        std::pair{ProcessMemoryBackend::WindowsPsapi, uint8_t{3}},
    };
    for (const auto [value, tag] : backend_cases) {
        auto record = make_record();
        record.commit_payload.actual_memory_backend = value;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT(byte_at(bytes, 90) == tag);
        expect_record_round_trip(record);
    }

    const std::array factor_cases{
        std::pair{SIQSShadowProofRssFactorIdentity::unknown, uint8_t{0}},
        std::pair{SIQSShadowProofRssFactorIdentity::pass, uint8_t{1}},
        std::pair{SIQSShadowProofRssFactorIdentity::fail, uint8_t{2}},
        std::pair{SIQSShadowProofRssFactorIdentity::not_checked, uint8_t{3}},
    };
    for (const auto [value, tag] : factor_cases) {
        auto record = make_record();
        record.commit_payload.factor_identity = value;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT(byte_at(bytes, 93) == tag);
        expect_record_round_trip(record);
    }

    const std::array evidence_cases{
        std::pair{SIQSShadowProofRssEvidence::unknown, uint8_t{0}},
        std::pair{SIQSShadowProofRssEvidence::not_applicable, uint8_t{1}},
        std::pair{SIQSShadowProofRssEvidence::pass, uint8_t{2}},
        std::pair{SIQSShadowProofRssEvidence::fail, uint8_t{3}},
    };
    for (const auto [value, tag] : evidence_cases) {
        auto proof = make_record();
        proof.commit_payload.proof_evidence = value;
        reseal(proof);
        const auto proof_bytes = encoded_record(proof);
        EXPECT(byte_at(proof_bytes, 94) == tag);
        expect_record_round_trip(proof);

        auto matrix = make_record();
        matrix.commit_payload.matrix_evidence = value;
        reseal(matrix);
        const auto matrix_bytes = encoded_record(matrix);
        EXPECT(byte_at(matrix_bytes, 95) == tag);
        expect_record_round_trip(matrix);
    }

    for (const bool fresh_process : {false, true}) {
        auto record = make_record();
        record.commit_payload.fresh_process = fresh_process;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT(byte_at(bytes, 91) == static_cast<uint8_t>(fresh_process));
        expect_record_round_trip(record);
    }
    for (const bool completed : {false, true}) {
        auto record = make_record();
        record.commit_payload.completed = completed;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT(byte_at(bytes, 92) == static_cast<uint8_t>(completed));
        expect_record_round_trip(record);
    }

    const std::array artifact_kind_cases{
        std::pair{SIQSShadowProofRssArtifactKind::unknown, uint8_t{0}},
        std::pair{SIQSShadowProofRssArtifactKind::probe_stdout, uint8_t{1}},
        std::pair{SIQSShadowProofRssArtifactKind::probe_stderr, uint8_t{2}},
        std::pair{SIQSShadowProofRssArtifactKind::joined_gate_sample, uint8_t{3}},
    };
    const std::array artifact_offsets{
        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET,
        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET,
        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET,
    };
    for (std::size_t artifact_index = 0; artifact_index < artifact_offsets.size();
         ++artifact_index) {
        for (const auto [kind, tag] : artifact_kind_cases) {
            for (const bool committed : {false, true}) {
                auto record = make_record();
                SIQSShadowProofRssArtifactSeal* seal = nullptr;
                if (artifact_index == 0) {
                    seal = &record.commit_payload.stdout_seal;
                } else if (artifact_index == 1) {
                    seal = &record.commit_payload.stderr_seal;
                } else {
                    seal = &record.commit_payload.joined_sample_seal;
                }
                seal->committed = committed;
                seal->kind = kind;
                seal->byte_count = std::numeric_limits<uint64_t>::max();
                seal->digest = {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)};
                reseal(record);
                const auto bytes = encoded_record(record);
                const auto offset = artifact_offsets[artifact_index];
                EXPECT(byte_at(bytes, offset) == static_cast<uint8_t>(committed));
                EXPECT(byte_at(bytes, offset + 1) == tag);
                EXPECT(get_u64(bytes, offset + 8) == std::numeric_limits<uint64_t>::max());
                EXPECT(get_u64(bytes, offset + 16) == UINT64_C(0x0123456789abcdef));
                EXPECT(get_u64(bytes, offset + 24) == UINT64_C(0xfedcba9876543210));
                expect_record_round_trip(record);
            }
        }
    }
}

void test_invalid_enum_and_boolean_tags() {
    auto bad_record = make_record();
    bad_record.kind = static_cast<SIQSShadowProofRssJournalRecordKind>(255);
    reseal(bad_record);
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(bad_record),
                        CodecError::invalid_record_kind,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET);

    bad_record = make_record();
    bad_record.commit_payload.actual_operating_system =
        static_cast<SIQSShadowProofRssOperatingSystem>(255);
    reseal(bad_record);
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(bad_record),
                        CodecError::invalid_operating_system, 88);
    bad_record = make_record();
    bad_record.commit_payload.actual_architecture =
        static_cast<SIQSShadowProofRssArchitecture>(255);
    reseal(bad_record);
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(bad_record),
                        CodecError::invalid_architecture, 89);
    bad_record = make_record();
    bad_record.commit_payload.actual_memory_backend = static_cast<ProcessMemoryBackend>(255);
    reseal(bad_record);
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(bad_record),
                        CodecError::invalid_memory_backend, 90);
    bad_record = make_record();
    bad_record.commit_payload.factor_identity = static_cast<SIQSShadowProofRssFactorIdentity>(255);
    reseal(bad_record);
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(bad_record),
                        CodecError::invalid_factor_identity, 93);
    for (const std::size_t offset : {std::size_t{94}, std::size_t{95}}) {
        bad_record = make_record();
        if (offset == 94) {
            bad_record.commit_payload.proof_evidence = static_cast<SIQSShadowProofRssEvidence>(255);
        } else {
            bad_record.commit_payload.matrix_evidence =
                static_cast<SIQSShadowProofRssEvidence>(255);
        }
        reseal(bad_record);
        expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(bad_record),
                            CodecError::invalid_evidence, offset);
    }

    const std::array seal_offsets{
        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET,
        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET,
        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET,
    };
    for (std::size_t seal_index = 0; seal_index < seal_offsets.size(); ++seal_index) {
        bad_record = make_record();
        if (seal_index == 0) {
            bad_record.commit_payload.stdout_seal.kind =
                static_cast<SIQSShadowProofRssArtifactKind>(255);
        } else if (seal_index == 1) {
            bad_record.commit_payload.stderr_seal.kind =
                static_cast<SIQSShadowProofRssArtifactKind>(255);
        } else {
            bad_record.commit_payload.joined_sample_seal.kind =
                static_cast<SIQSShadowProofRssArtifactKind>(255);
        }
        reseal(bad_record);
        expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(bad_record),
                            CodecError::invalid_artifact_kind, seal_offsets[seal_index] + 1);
    }

    const auto valid_bytes = encoded_record(make_record());
    const std::array invalid_enum_offsets{
        std::pair{SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET,
                  CodecError::invalid_record_kind},
        std::pair<std::size_t, CodecError>{88, CodecError::invalid_operating_system},
        std::pair<std::size_t, CodecError>{89, CodecError::invalid_architecture},
        std::pair<std::size_t, CodecError>{90, CodecError::invalid_memory_backend},
        std::pair<std::size_t, CodecError>{93, CodecError::invalid_factor_identity},
        std::pair<std::size_t, CodecError>{94, CodecError::invalid_evidence},
        std::pair<std::size_t, CodecError>{95, CodecError::invalid_evidence},
        std::pair{SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET + 1,
                  CodecError::invalid_artifact_kind},
        std::pair{SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET + 1,
                  CodecError::invalid_artifact_kind},
        std::pair{SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET + 1,
                  CodecError::invalid_artifact_kind},
    };
    for (const auto [offset, error] : invalid_enum_offsets) {
        auto bytes = valid_bytes;
        put_u8(bytes, offset, 255);
        expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(bytes), error,
                            offset);
    }

    const std::array invalid_boolean_offsets{
        std::size_t{91},
        std::size_t{92},
        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET,
        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET,
        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET,
    };
    for (const auto offset : invalid_boolean_offsets) {
        auto bytes = valid_bytes;
        put_u8(bytes, offset, 2);
        expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(bytes),
                            CodecError::invalid_boolean, offset);
    }
}

void test_optional_masks_canonical_absence_and_signed_boundaries() {
    for (uint8_t mask = 0; mask <= 0x1fU; ++mask) {
        auto record = make_record();
        auto& payload = record.commit_payload;
        payload.absolute_peak_rss_bytes =
            (mask & 0x01U) != 0 ? std::optional<uint64_t>{UINT64_C(0x1111111111111111)}
                                : std::nullopt;
        payload.observe_minus_off_peak_bytes =
            (mask & 0x02U) != 0 ? std::optional<int64_t>{-INT64_C(0x222222222222222)}
                                : std::nullopt;
        payload.current_rss_bytes = (mask & 0x04U) != 0
                                        ? std::optional<uint64_t>{UINT64_C(0x3333333333333333)}
                                        : std::nullopt;
        payload.peak_growth_bytes = (mask & 0x08U) != 0
                                        ? std::optional<uint64_t>{UINT64_C(0x4444444444444444)}
                                        : std::nullopt;
        payload.wall_ns = (mask & 0x10U) != 0
                              ? std::optional<uint64_t>{UINT64_C(0x5555555555555555)}
                              : std::nullopt;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT(byte_at(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET) == mask);
        if ((mask & 0x01U) == 0) {
            EXPECT(get_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET) == 0);
        }
        if ((mask & 0x02U) == 0) {
            EXPECT(get_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET) == 0);
        }
        if ((mask & 0x04U) == 0) {
            EXPECT(get_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_CURRENT_RSS_OFFSET) == 0);
        }
        if ((mask & 0x08U) == 0) {
            EXPECT(get_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PEAK_GROWTH_OFFSET) == 0);
        }
        if ((mask & 0x10U) == 0) {
            EXPECT(get_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WALL_NS_OFFSET) == 0);
        }
        const auto decoded = decode_siqs_shadow_proof_rss_campaign_journal_record(bytes);
        EXPECT(decoded);
        EXPECT(decoded.value.has_value());
        if (decoded.value.has_value()) {
            EXPECT(*decoded.value == record);
        }
    }

    auto absent = make_record();
    absent.commit_payload.absolute_peak_rss_bytes.reset();
    absent.commit_payload.observe_minus_off_peak_bytes.reset();
    absent.commit_payload.current_rss_bytes.reset();
    absent.commit_payload.peak_growth_bytes.reset();
    absent.commit_payload.wall_ns.reset();
    reseal(absent);
    const auto absent_bytes = encoded_record(absent);
    EXPECT(byte_at(absent_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET) == 0);
    for (const auto offset : {SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET,
                              SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET,
                              SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_CURRENT_RSS_OFFSET,
                              SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PEAK_GROWTH_OFFSET,
                              SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WALL_NS_OFFSET}) {
        auto bytes = absent_bytes;
        put_u64(bytes, offset, 1);
        expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(bytes),
                            CodecError::noncanonical_absent_value, offset);
    }

    for (const uint8_t invalid_mask : {uint8_t{0x20}, uint8_t{0x80}, uint8_t{0xff}}) {
        auto bytes = absent_bytes;
        put_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET, invalid_mask);
        expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(bytes),
                            CodecError::invalid_optional_mask,
                            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET);
    }

    for (const int64_t value : {std::numeric_limits<int64_t>::min(), INT64_C(-1), INT64_C(0),
                                std::numeric_limits<int64_t>::max()}) {
        auto record = make_record();
        record.commit_payload.observe_minus_off_peak_bytes = value;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT((byte_at(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET) &
                0x02U) != 0);
        EXPECT(get_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET) ==
               std::bit_cast<uint64_t>(value));
        expect_record_round_trip(record);
    }
}

void test_worker_range_and_digest_guards() {
    for (const uint32_t workers : {UINT32_C(0), std::numeric_limits<uint32_t>::max()}) {
        auto record = make_record();
        record.commit_payload.actual_resolved_sieve_workers = workers;
        reseal(record);
        const auto bytes = encoded_record(record);
        EXPECT(get_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET) == workers);
        expect_record_round_trip(record);
    }

    if constexpr (std::numeric_limits<std::size_t>::max() > std::numeric_limits<uint32_t>::max()) {
        auto record = make_record();
        record.commit_payload.actual_resolved_sieve_workers =
            static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()) + 1;
        reseal(record);
        expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(record),
                            CodecError::integer_out_of_range,
                            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET);
    }

    if constexpr (std::numeric_limits<std::size_t>::max() < std::numeric_limits<uint32_t>::max()) {
        auto bytes = encoded_record(make_record());
        put_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET,
                std::numeric_limits<uint32_t>::max());
        expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(bytes),
                            CodecError::integer_out_of_range,
                            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET);
    }

    auto header = make_header();
    ++header.slot_count;
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_header(header),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET);
    header = make_header();
    ++header.header_digest.low;
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_header(header),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET);

    auto record = make_record();
    ++record.sequence_number;
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(record),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET);
    record = make_record();
    ++*record.commit_payload.wall_ns;
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(record),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET);
    record = make_record();
    ++record.record_digest.high;
    expect_encode_error(encode_siqs_shadow_proof_rss_campaign_journal_record(record),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET);

    auto header_bytes = encoded_header(make_header());
    put_u8(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SLOT_COUNT_OFFSET,
           static_cast<uint8_t>(
               byte_at(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SLOT_COUNT_OFFSET) + 1));
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(header_bytes),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET);
    header_bytes = encoded_header(make_header());
    put_u8(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET,
           static_cast<uint8_t>(
               byte_at(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET) + 1));
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(header_bytes),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET);

    auto record_bytes = encoded_record(make_record());
    put_u8(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET,
           static_cast<uint8_t>(
               byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET) + 1));
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET);
    record_bytes = encoded_record(make_record());
    put_u8(
        record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET,
        static_cast<uint8_t>(
            byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET) + 1));
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET);
    record_bytes = encoded_record(make_record());
    put_u8(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET + 15,
           static_cast<uint8_t>(
               byte_at(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET + 15) + 1));
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes),
                        CodecError::digest_mismatch,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET);
}

void test_decode_reports_lowest_failing_offset() {
    auto header_bytes = encoded_header(make_header());
    put_u32(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION + 1);
    put_u8(header_bytes, 20, 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(header_bytes),
                        CodecError::unsupported_journal_schema_version,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET);

    header_bytes = encoded_header(make_header());
    put_u8(header_bytes, 20, 1);
    put_u8(header_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET, 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_header(header_bytes),
                        CodecError::nonzero_reserved, 20);

    auto record_bytes = encoded_record(make_record());
    put_u32(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET,
            SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION + 1);
    put_u8(record_bytes, 25, 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes),
                        CodecError::unsupported_journal_schema_version,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET);

    record_bytes = encoded_record(make_record());
    put_u8(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET, 255);
    put_u8(record_bytes, 25, 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes),
                        CodecError::invalid_record_kind,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET);

    record_bytes = encoded_record(make_record());
    put_u8(record_bytes, 25, 1);
    put_u8(record_bytes, 88, 255);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes),
                        CodecError::nonzero_reserved, 25);

    record_bytes = encoded_record(make_record());
    put_u8(record_bytes, 88, 255);
    put_u8(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET, 255);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes),
                        CodecError::invalid_operating_system, 88);

    record_bytes = encoded_record(make_record());
    put_u8(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET, 255);
    put_u8(record_bytes, 101, 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes),
                        CodecError::invalid_optional_mask,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET);

    record_bytes = encoded_record(make_record());
    put_u8(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET + 1, 255);
    put_u8(record_bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET + 2, 1);
    expect_decode_error(decode_siqs_shadow_proof_rss_campaign_journal_record(record_bytes),
                        CodecError::invalid_artifact_kind,
                        SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET + 1);
}

void test_codec_error_names_are_total() {
    const std::array errors{
        CodecError::none,
        CodecError::truncated,
        CodecError::trailing_bytes,
        CodecError::invalid_magic,
        CodecError::unsupported_wire_version,
        CodecError::declared_size_mismatch,
        CodecError::unsupported_journal_schema_version,
        CodecError::nonzero_reserved,
        CodecError::invalid_boolean,
        CodecError::invalid_record_kind,
        CodecError::invalid_operating_system,
        CodecError::invalid_architecture,
        CodecError::invalid_memory_backend,
        CodecError::invalid_factor_identity,
        CodecError::invalid_evidence,
        CodecError::invalid_artifact_kind,
        CodecError::invalid_optional_mask,
        CodecError::noncanonical_absent_value,
        CodecError::integer_out_of_range,
        CodecError::digest_mismatch,
    };
    for (const auto error : errors) {
        EXPECT(!siqs_shadow_proof_rss_campaign_journal_codec_error_name(error).empty());
        EXPECT(siqs_shadow_proof_rss_campaign_journal_codec_error_name(error) != "unknown");
    }
    EXPECT(siqs_shadow_proof_rss_campaign_journal_codec_error_name(static_cast<CodecError>(255)) ==
           "unknown");
}

} // namespace

int main() {
    test_fixed_width_golden_layout_and_round_trip();
    test_frame_size_magic_version_schema_and_reserved_rejection();
    test_all_enum_and_boolean_domains();
    test_invalid_enum_and_boolean_tags();
    test_optional_masks_canonical_absence_and_signed_boundaries();
    test_worker_range_and_digest_guards();
    test_decode_reports_lowest_failing_offset();
    test_codec_error_names_are_total();

    std::cout << "SIQS shadow-proof RSS journal codec: " << checks_passed << " checks passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
