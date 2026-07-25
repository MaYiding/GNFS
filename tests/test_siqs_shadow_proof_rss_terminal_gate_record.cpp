#include "shadow_proof_rss_terminal_gate_record_internal.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace gnfs::siqs;
using namespace gnfs::siqs::shadow_proof_rss_terminal_gate_record_detail;

using CodecError = SIQSShadowProofRssTerminalGateRecordCodecError;
using Record = SIQSShadowProofRssTerminalGateRecord;

static_assert(SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_SCHEMA_VERSION == 1);
static_assert(SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_VERSION == 1);
static_assert(SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE == 192);
static_assert(SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_LEAF == "terminal-gate.rtgr");
static_assert(TERMINAL_GATE_RECORD_DIGEST_OFFSET == 168);
static_assert(TERMINAL_GATE_TAIL_OFFSET == 184);

static_assert(std::same_as<decltype(Record{}.schema_version), std::uint32_t>);
static_assert(std::same_as<decltype(Record{}.total_sample_count), std::uint32_t>);
static_assert(std::same_as<decltype(Record{}.valid_off_sample_count), std::uint32_t>);
static_assert(std::same_as<decltype(Record{}.valid_observe_sample_count), std::uint32_t>);
static_assert(std::same_as<decltype(Record{}.rss_limit_bytes), std::uint64_t>);
static_assert(std::same_as<decltype(Record{}.max_observe_peak_rss_bytes), std::uint64_t>);
static_assert(std::is_trivially_copyable_v<Record>);

static_assert(noexcept(make_terminal_gate_record(
    std::declval<SIQSShadowProofRssCorpusDigest>(), std::declval<SIQSShadowProofRssCorpusDigest>(),
    std::declval<const SIQSShadowProofRssGateOutcome&>())));
static_assert(noexcept(terminal_gate_record_is_valid(std::declval<const Record&>())));
static_assert(noexcept(encode_terminal_gate_record(std::declval<const Record&>())));
static_assert(noexcept(decode_terminal_gate_record(std::declval<std::span<const std::byte>>())));

[[nodiscard]] constexpr SIQSShadowProofRssProbeExecutionIdentity test_identity() noexcept {
    SIQSShadowProofRssProbeExecutionIdentity identity;
    for (std::size_t index = 0; index < identity.executable_sha256.bytes.size(); ++index) {
        identity.executable_sha256.bytes[index] = static_cast<std::byte>(index + 1);
        identity.execution_contract_sha256.bytes[index] = static_cast<std::byte>(index + 33);
    }
    return identity;
}

inline constexpr SIQSShadowProofRssCorpusDigest TEST_PLAN_DIGEST{UINT64_C(0x0102030405060708),
                                                                 UINT64_C(0x1112131415161718)};
inline constexpr SIQSShadowProofRssCorpusDigest TEST_FINAL_JOURNAL_DIGEST{
    UINT64_C(0x2122232425262728), UINT64_C(0x3132333435363738)};
inline constexpr SIQSShadowProofRssCorpusDigest TEST_POLICY_DIGEST{UINT64_C(0x5152535455565758),
                                                                   UINT64_C(0x6162636465666768)};
inline constexpr SIQSShadowProofRssProbeExecutionIdentity TEST_IDENTITY = test_identity();

[[nodiscard]] constexpr SIQSShadowProofRssGateOutcome manual_review_outcome() noexcept {
    SIQSShadowProofRssGateOutcome outcome;
    outcome.status = SIQSShadowProofRssGateStatus::manual_review_candidate;
    outcome.reason = SIQSShadowProofRssGateReason::all_observe_peaks_within_limit;
    outcome.total_sample_count = SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT;
    outcome.valid_off_sample_count =
        SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT * SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS;
    outcome.valid_observe_sample_count =
        SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT * SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS;
    outcome.rss_limit_bytes = UINT64_C(0x4142434445464748);
    outcome.max_observe_peak_rss_bytes = UINT64_C(0x4041424344454647);
    outcome.policy_binding_digest = TEST_POLICY_DIGEST;
    outcome.probe_execution_identity = TEST_IDENTITY;
    return outcome;
}

[[nodiscard]] constexpr SIQSShadowProofRssGateOutcome limit_exceeded_outcome() noexcept {
    auto outcome = manual_review_outcome();
    outcome.status = SIQSShadowProofRssGateStatus::limit_exceeded;
    outcome.reason = SIQSShadowProofRssGateReason::observe_peak_over_limit;
    outcome.max_observe_peak_rss_bytes = outcome.rss_limit_bytes + 1;
    return outcome;
}

inline constexpr auto TEST_OUTCOME = manual_review_outcome();
inline constexpr auto TEST_RECORD =
    make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST, TEST_OUTCOME);
static_assert(TEST_RECORD.has_value());
static_assert(terminal_gate_record_is_valid(*TEST_RECORD));
static_assert(TEST_RECORD->gate_outcome() == TEST_OUTCOME);
static_assert(TEST_RECORD->record_digest.low == UINT64_C(0xce2164de57b336c8));
static_assert(TEST_RECORD->record_digest.high == UINT64_C(0x53140e35c1005ea3));
inline constexpr auto TEST_ENCODED = encode_terminal_gate_record(*TEST_RECORD);
static_assert(static_cast<bool>(TEST_ENCODED));
static_assert(TEST_ENCODED.bytes.has_value());
inline constexpr auto TEST_DECODED = decode_terminal_gate_record(*TEST_ENCODED.bytes);
static_assert(static_cast<bool>(TEST_DECODED));
static_assert(TEST_DECODED.record == TEST_RECORD);

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
[[nodiscard]] std::uint8_t byte_at(const std::array<std::byte, Size>& bytes,
                                   std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

template <std::size_t Size>
void put_u8(std::array<std::byte, Size>& bytes, std::size_t offset, std::uint8_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value);
}

template <std::size_t Size>
void put_u32(std::array<std::byte, Size>& bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        put_u8(bytes, offset++, static_cast<std::uint8_t>(value >> shift));
    }
}

template <std::size_t Size>
void put_u64(std::array<std::byte, Size>& bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        put_u8(bytes, offset++, static_cast<std::uint8_t>(value >> shift));
    }
}

template <std::size_t Size>
[[nodiscard]] std::uint32_t get_u32(const std::array<std::byte, Size>& bytes,
                                    std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(byte_at(bytes, offset++)) << shift;
    }
    return value;
}

template <std::size_t Size>
[[nodiscard]] std::uint64_t get_u64(const std::array<std::byte, Size>& bytes,
                                    std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(byte_at(bytes, offset++)) << shift;
    }
    return value;
}

void expect_decode_error(std::span<const std::byte> bytes, CodecError error, std::size_t offset) {
    const auto decoded = decode_terminal_gate_record(bytes);
    EXPECT(!static_cast<bool>(decoded));
    EXPECT(!decoded.record.has_value());
    EXPECT(decoded.error == error);
    EXPECT(decoded.error_offset == offset);
}

void expect_encode_error(Record record, CodecError error, std::size_t offset) {
    record.record_digest = terminal_gate_record_digest(record);
    const auto encoded = encode_terminal_gate_record(record);
    EXPECT(!static_cast<bool>(encoded));
    EXPECT(!encoded.bytes.has_value());
    EXPECT(encoded.error == error);
    EXPECT(encoded.error_offset == offset);
}

void test_make_and_typed_contract() {
    const auto manual = make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST,
                                                  manual_review_outcome());
    EXPECT(manual.has_value());
    EXPECT(terminal_gate_record_is_valid(*manual));
    EXPECT(manual->gate_outcome() == manual_review_outcome());

    const auto exceeded = make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST,
                                                    limit_exceeded_outcome());
    EXPECT(exceeded.has_value());
    EXPECT(terminal_gate_record_is_valid(*exceeded));
    EXPECT(exceeded->gate_outcome() == limit_exceeded_outcome());
    EXPECT(exceeded->record_digest != manual->record_digest);

    auto rejected = manual_review_outcome();
    rejected.status = SIQSShadowProofRssGateStatus::blocked;
    rejected.reason = SIQSShadowProofRssGateReason::policy_missing;
    EXPECT(!make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST, rejected)
                .has_value());

    rejected = manual_review_outcome();
    rejected.reason = SIQSShadowProofRssGateReason::observe_peak_over_limit;
    EXPECT(!make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST, rejected)
                .has_value());

    rejected = manual_review_outcome();
    rejected.shadow_outcome_routed = true;
    EXPECT(!make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST, rejected)
                .has_value());

    rejected = manual_review_outcome();
    rejected.promotion = true;
    EXPECT(!make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST, rejected)
                .has_value());

    rejected = manual_review_outcome();
    rejected.total_sample_count = 79;
    EXPECT(!make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST, rejected)
                .has_value());

    rejected = manual_review_outcome();
    rejected.policy_binding_digest = {};
    EXPECT(!make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST, rejected)
                .has_value());

    rejected = manual_review_outcome();
    rejected.probe_execution_identity = {};
    EXPECT(!make_terminal_gate_record(TEST_PLAN_DIGEST, TEST_FINAL_JOURNAL_DIGEST, rejected)
                .has_value());

    EXPECT(!make_terminal_gate_record({}, TEST_FINAL_JOURNAL_DIGEST, manual_review_outcome())
                .has_value());
    EXPECT(!make_terminal_gate_record(TEST_PLAN_DIGEST, {}, manual_review_outcome()).has_value());
}

void test_canonical_wire_layout_and_round_trip() {
    const Record record = *TEST_RECORD;
    const auto encoded = encode_terminal_gate_record(record);
    EXPECT(static_cast<bool>(encoded));
    const auto& bytes = *encoded.bytes;

    constexpr std::string_view magic = "GNFSTGRC";
    for (std::size_t index = 0; index < magic.size(); ++index) {
        EXPECT(byte_at(bytes, index) ==
               static_cast<std::uint8_t>(static_cast<unsigned char>(magic[index])));
    }
    EXPECT(get_u32(bytes, TERMINAL_GATE_WIRE_VERSION_OFFSET) == 1);
    EXPECT(get_u32(bytes, TERMINAL_GATE_WIRE_SIZE_OFFSET) == bytes.size());
    EXPECT(get_u32(bytes, TERMINAL_GATE_SCHEMA_OFFSET) == 1);
    EXPECT(byte_at(bytes, TERMINAL_GATE_STATUS_OFFSET) == 2);
    EXPECT(byte_at(bytes, TERMINAL_GATE_REASON_OFFSET) == 2);
    EXPECT(byte_at(bytes, TERMINAL_GATE_ROUTED_OFFSET) == 0);
    EXPECT(byte_at(bytes, TERMINAL_GATE_PROMOTION_OFFSET) == 0);
    EXPECT(get_u32(bytes, TERMINAL_GATE_TOTAL_SAMPLE_COUNT_OFFSET) == 80);
    EXPECT(get_u32(bytes, TERMINAL_GATE_VALID_OFF_SAMPLE_COUNT_OFFSET) == 24);
    EXPECT(get_u32(bytes, TERMINAL_GATE_VALID_OBSERVE_SAMPLE_COUNT_OFFSET) == 56);
    EXPECT(get_u64(bytes, TERMINAL_GATE_RSS_LIMIT_OFFSET) == record.rss_limit_bytes);
    EXPECT(get_u64(bytes, TERMINAL_GATE_MAX_OBSERVE_PEAK_OFFSET) ==
           record.max_observe_peak_rss_bytes);
    EXPECT(get_u64(bytes, TERMINAL_GATE_PLAN_DIGEST_OFFSET) == TEST_PLAN_DIGEST.low);
    EXPECT(get_u64(bytes, TERMINAL_GATE_PLAN_DIGEST_OFFSET + 8) == TEST_PLAN_DIGEST.high);
    EXPECT(get_u64(bytes, TERMINAL_GATE_FINAL_JOURNAL_DIGEST_OFFSET) ==
           TEST_FINAL_JOURNAL_DIGEST.low);
    EXPECT(get_u64(bytes, TERMINAL_GATE_POLICY_DIGEST_OFFSET) == TEST_POLICY_DIGEST.low);
    EXPECT(byte_at(bytes, TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET) == 1);
    EXPECT(byte_at(bytes, TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET + 31) == 32);
    EXPECT(byte_at(bytes, TERMINAL_GATE_EXECUTION_CONTRACT_SHA256_OFFSET) == 33);
    EXPECT(byte_at(bytes, TERMINAL_GATE_EXECUTION_CONTRACT_SHA256_OFFSET + 31) == 64);
    EXPECT(get_u64(bytes, TERMINAL_GATE_RECORD_DIGEST_OFFSET) == record.record_digest.low);
    EXPECT(get_u64(bytes, TERMINAL_GATE_RECORD_DIGEST_OFFSET + 8) == record.record_digest.high);
    for (std::size_t offset = 36; offset < TERMINAL_GATE_RSS_LIMIT_OFFSET; ++offset) {
        EXPECT(byte_at(bytes, offset) == 0);
    }
    for (std::size_t offset = TERMINAL_GATE_TAIL_OFFSET; offset < bytes.size(); ++offset) {
        EXPECT(byte_at(bytes, offset) == 0);
    }

    const auto decoded = decode_terminal_gate_record(bytes);
    EXPECT(static_cast<bool>(decoded));
    EXPECT(decoded.record == record);
    EXPECT(decoded.record->gate_outcome() == TEST_OUTCOME);
    const auto reencoded = encode_terminal_gate_record(*decoded.record);
    EXPECT(static_cast<bool>(reencoded));
    EXPECT(reencoded.bytes == encoded.bytes);
}

void test_frame_tag_reserved_and_tail_rejection() {
    const auto valid = *TEST_ENCODED.bytes;

    expect_decode_error(std::span<const std::byte>(valid).first(15), CodecError::truncated, 15);
    expect_decode_error(std::span<const std::byte>(valid).first(valid.size() - 1),
                        CodecError::truncated, valid.size() - 1);
    std::vector<std::byte> extra(valid.begin(), valid.end());
    extra.push_back(std::byte{0});
    expect_decode_error(extra, CodecError::trailing_bytes, valid.size());

    auto changed = valid;
    put_u8(changed, TERMINAL_GATE_MAGIC_OFFSET + 3, 0xff);
    expect_decode_error(changed, CodecError::invalid_magic, TERMINAL_GATE_MAGIC_OFFSET + 3);

    changed = valid;
    put_u32(changed, TERMINAL_GATE_WIRE_VERSION_OFFSET, 2);
    expect_decode_error(changed, CodecError::unsupported_wire_version,
                        TERMINAL_GATE_WIRE_VERSION_OFFSET);

    changed = valid;
    put_u32(changed, TERMINAL_GATE_WIRE_SIZE_OFFSET, static_cast<std::uint32_t>(valid.size() - 1));
    expect_decode_error(changed, CodecError::declared_size_mismatch,
                        TERMINAL_GATE_WIRE_SIZE_OFFSET);

    changed = valid;
    put_u32(changed, TERMINAL_GATE_SCHEMA_OFFSET, 2);
    expect_decode_error(changed, CodecError::unsupported_schema_version,
                        TERMINAL_GATE_SCHEMA_OFFSET);

    for (const std::uint8_t tag :
         std::array<std::uint8_t, 3>{UINT8_C(0), UINT8_C(3), UINT8_C(255)}) {
        changed = valid;
        put_u8(changed, TERMINAL_GATE_STATUS_OFFSET, tag);
        expect_decode_error(changed, CodecError::invalid_status_tag, TERMINAL_GATE_STATUS_OFFSET);

        changed = valid;
        put_u8(changed, TERMINAL_GATE_REASON_OFFSET, tag);
        expect_decode_error(changed, CodecError::invalid_reason_tag, TERMINAL_GATE_REASON_OFFSET);
    }

    changed = valid;
    put_u8(changed, TERMINAL_GATE_ROUTED_OFFSET, 2);
    expect_decode_error(changed, CodecError::invalid_boolean, TERMINAL_GATE_ROUTED_OFFSET);
    changed = valid;
    put_u8(changed, TERMINAL_GATE_PROMOTION_OFFSET, 2);
    expect_decode_error(changed, CodecError::invalid_boolean, TERMINAL_GATE_PROMOTION_OFFSET);

    changed = valid;
    put_u8(changed, TERMINAL_GATE_ROUTED_OFFSET, 1);
    expect_decode_error(changed, CodecError::invalid_terminal_outcome, TERMINAL_GATE_STATUS_OFFSET);
    changed = valid;
    put_u8(changed, TERMINAL_GATE_PROMOTION_OFFSET, 1);
    expect_decode_error(changed, CodecError::invalid_terminal_outcome, TERMINAL_GATE_STATUS_OFFSET);

    for (std::size_t offset = 36; offset < TERMINAL_GATE_RSS_LIMIT_OFFSET; ++offset) {
        changed = valid;
        put_u8(changed, offset, 1);
        expect_decode_error(changed, CodecError::nonzero_reserved, offset);
    }
    for (std::size_t offset = TERMINAL_GATE_TAIL_OFFSET; offset < valid.size(); ++offset) {
        changed = valid;
        put_u8(changed, offset, 1);
        expect_decode_error(changed, CodecError::nonzero_tail, offset);
    }
}

void test_field_and_self_digest_tamper_rejection() {
    const auto valid = *TEST_ENCODED.bytes;

    auto changed = valid;
    put_u64(changed, TERMINAL_GATE_RSS_LIMIT_OFFSET,
            get_u64(valid, TERMINAL_GATE_RSS_LIMIT_OFFSET) + 1);
    expect_decode_error(changed, CodecError::digest_mismatch, TERMINAL_GATE_RECORD_DIGEST_OFFSET);

    changed = valid;
    put_u8(changed, TERMINAL_GATE_PLAN_DIGEST_OFFSET,
           static_cast<std::uint8_t>(byte_at(changed, TERMINAL_GATE_PLAN_DIGEST_OFFSET) ^
                                     UINT8_C(0x80)));
    expect_decode_error(changed, CodecError::digest_mismatch, TERMINAL_GATE_RECORD_DIGEST_OFFSET);

    changed = valid;
    put_u8(changed, TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET + 7,
           static_cast<std::uint8_t>(byte_at(changed, TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET + 7) ^
                                     UINT8_C(0x40)));
    expect_decode_error(changed, CodecError::digest_mismatch, TERMINAL_GATE_RECORD_DIGEST_OFFSET);

    changed = valid;
    put_u8(changed, TERMINAL_GATE_RECORD_DIGEST_OFFSET,
           static_cast<std::uint8_t>(byte_at(changed, TERMINAL_GATE_RECORD_DIGEST_OFFSET) ^
                                     UINT8_C(1)));
    expect_decode_error(changed, CodecError::digest_mismatch, TERMINAL_GATE_RECORD_DIGEST_OFFSET);

    changed = valid;
    for (std::size_t offset = TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET;
         offset < TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET + 32; ++offset) {
        put_u8(changed, offset, 0);
    }
    expect_decode_error(changed, CodecError::invalid_probe_execution_identity,
                        TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET);
}

void test_typed_encode_rejection_and_error_names() {
    Record changed = *TEST_RECORD;
    changed.schema_version = 2;
    expect_encode_error(changed, CodecError::unsupported_schema_version,
                        TERMINAL_GATE_SCHEMA_OFFSET);

    changed = *TEST_RECORD;
    changed.status = SIQSShadowProofRssGateStatus::blocked;
    expect_encode_error(changed, CodecError::invalid_status_tag, TERMINAL_GATE_STATUS_OFFSET);

    changed = *TEST_RECORD;
    changed.reason = SIQSShadowProofRssGateReason::observe_peak_over_limit;
    expect_encode_error(changed, CodecError::invalid_terminal_outcome, TERMINAL_GATE_STATUS_OFFSET);

    changed = *TEST_RECORD;
    changed.total_sample_count = 79;
    expect_encode_error(changed, CodecError::invalid_terminal_outcome, TERMINAL_GATE_STATUS_OFFSET);

    changed = *TEST_RECORD;
    changed.plan_digest = {};
    expect_encode_error(changed, CodecError::invalid_terminal_outcome,
                        TERMINAL_GATE_PLAN_DIGEST_OFFSET);

    changed = *TEST_RECORD;
    changed.final_journal_record_digest = {};
    expect_encode_error(changed, CodecError::invalid_terminal_outcome,
                        TERMINAL_GATE_FINAL_JOURNAL_DIGEST_OFFSET);

    changed = *TEST_RECORD;
    changed.policy_binding_digest = {};
    expect_encode_error(changed, CodecError::invalid_terminal_outcome,
                        TERMINAL_GATE_POLICY_DIGEST_OFFSET);

    changed = *TEST_RECORD;
    changed.probe_execution_identity = {};
    expect_encode_error(changed, CodecError::invalid_probe_execution_identity,
                        TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET);

    changed = *TEST_RECORD;
    changed.record_digest.low ^= 1;
    const auto bad_digest = encode_terminal_gate_record(changed);
    EXPECT(!static_cast<bool>(bad_digest));
    EXPECT(bad_digest.error == CodecError::digest_mismatch);
    EXPECT(bad_digest.error_offset == TERMINAL_GATE_RECORD_DIGEST_OFFSET);

    constexpr std::array expected_names{
        std::pair{CodecError::none, std::string_view{"none"}},
        std::pair{CodecError::truncated, std::string_view{"truncated"}},
        std::pair{CodecError::trailing_bytes, std::string_view{"trailing_bytes"}},
        std::pair{CodecError::invalid_magic, std::string_view{"invalid_magic"}},
        std::pair{CodecError::unsupported_wire_version,
                  std::string_view{"unsupported_wire_version"}},
        std::pair{CodecError::declared_size_mismatch, std::string_view{"declared_size_mismatch"}},
        std::pair{CodecError::unsupported_schema_version,
                  std::string_view{"unsupported_schema_version"}},
        std::pair{CodecError::invalid_status_tag, std::string_view{"invalid_status_tag"}},
        std::pair{CodecError::invalid_reason_tag, std::string_view{"invalid_reason_tag"}},
        std::pair{CodecError::invalid_boolean, std::string_view{"invalid_boolean"}},
        std::pair{CodecError::nonzero_reserved, std::string_view{"nonzero_reserved"}},
        std::pair{CodecError::nonzero_tail, std::string_view{"nonzero_tail"}},
        std::pair{CodecError::invalid_terminal_outcome,
                  std::string_view{"invalid_terminal_outcome"}},
        std::pair{CodecError::invalid_probe_execution_identity,
                  std::string_view{"invalid_probe_execution_identity"}},
        std::pair{CodecError::digest_mismatch, std::string_view{"digest_mismatch"}},
    };
    for (const auto& [error, name] : expected_names) {
        EXPECT(siqs_shadow_proof_rss_terminal_gate_record_codec_error_name(error) == name);
    }
    EXPECT(siqs_shadow_proof_rss_terminal_gate_record_codec_error_name(
               static_cast<CodecError>(255)) == "unknown");
}

} // namespace

int main() {
    test_make_and_typed_contract();
    test_canonical_wire_layout_and_round_trip();
    test_frame_tag_reserved_and_tail_rejection();
    test_field_and_self_digest_tamper_rejection();
    test_typed_encode_rejection_and_error_names();

    if (checks_failed != 0) {
        std::cerr << checks_failed << " terminal gate record checks failed; " << checks_passed
                  << " passed\n";
        return 1;
    }
    std::cout << "SIQS terminal gate record tests passed (" << checks_passed << " checks)\n";
    return 0;
}
