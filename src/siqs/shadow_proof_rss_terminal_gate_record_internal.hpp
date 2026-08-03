#pragma once

// Source-private, allocation-free fixed-width record for one terminal SIQS
// RSS gate outcome. This file is not installed as public API and grants no
// filesystem, journal, routing, or promotion authority.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace gnfs::siqs::shadow_proof_rss_terminal_gate_record_detail {

inline constexpr std::uint32_t SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_SCHEMA_VERSION = 1;
inline constexpr std::uint32_t SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_VERSION = 1;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE = 192;
inline constexpr std::string_view SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_LEAF =
    "terminal-gate.rtgr";
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_NO_ERROR_OFFSET =
    std::numeric_limits<std::size_t>::max();

inline constexpr std::size_t TERMINAL_GATE_MAGIC_OFFSET = 0;
inline constexpr std::size_t TERMINAL_GATE_WIRE_VERSION_OFFSET = 8;
inline constexpr std::size_t TERMINAL_GATE_WIRE_SIZE_OFFSET = 12;
inline constexpr std::size_t TERMINAL_GATE_SCHEMA_OFFSET = 16;
inline constexpr std::size_t TERMINAL_GATE_STATUS_OFFSET = 20;
inline constexpr std::size_t TERMINAL_GATE_REASON_OFFSET = 21;
inline constexpr std::size_t TERMINAL_GATE_ROUTED_OFFSET = 22;
inline constexpr std::size_t TERMINAL_GATE_PROMOTION_OFFSET = 23;
inline constexpr std::size_t TERMINAL_GATE_TOTAL_SAMPLE_COUNT_OFFSET = 24;
inline constexpr std::size_t TERMINAL_GATE_VALID_OFF_SAMPLE_COUNT_OFFSET = 28;
inline constexpr std::size_t TERMINAL_GATE_VALID_OBSERVE_SAMPLE_COUNT_OFFSET = 32;
inline constexpr std::size_t TERMINAL_GATE_RSS_LIMIT_OFFSET = 40;
inline constexpr std::size_t TERMINAL_GATE_MAX_OBSERVE_PEAK_OFFSET = 48;
inline constexpr std::size_t TERMINAL_GATE_PLAN_DIGEST_OFFSET = 56;
inline constexpr std::size_t TERMINAL_GATE_FINAL_JOURNAL_DIGEST_OFFSET = 72;
inline constexpr std::size_t TERMINAL_GATE_POLICY_DIGEST_OFFSET = 88;
inline constexpr std::size_t TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET = 104;
inline constexpr std::size_t TERMINAL_GATE_EXECUTION_CONTRACT_SHA256_OFFSET = 136;
inline constexpr std::size_t TERMINAL_GATE_RECORD_DIGEST_OFFSET = 168;
inline constexpr std::size_t TERMINAL_GATE_TAIL_OFFSET = 184;

/// Fixed-width scalar projection of one complete terminal gate outcome. The
/// three counts deliberately use uint32_t rather than the GateOutcome's native
/// size_t so the typed record has the same value domain on every platform.
struct SIQSShadowProofRssTerminalGateRecord final {
    std::uint32_t schema_version = SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_SCHEMA_VERSION;
    SIQSShadowProofRssCorpusDigest plan_digest;
    SIQSShadowProofRssCorpusDigest final_journal_record_digest;
    SIQSShadowProofRssGateStatus status = SIQSShadowProofRssGateStatus::invalid;
    SIQSShadowProofRssGateReason reason = SIQSShadowProofRssGateReason::internal_failure;
    std::uint32_t total_sample_count = 0;
    std::uint32_t valid_off_sample_count = 0;
    std::uint32_t valid_observe_sample_count = 0;
    std::uint64_t rss_limit_bytes = 0;
    std::uint64_t max_observe_peak_rss_bytes = 0;
    SIQSShadowProofRssCorpusDigest policy_binding_digest;
    SIQSShadowProofRssProbeExecutionIdentity probe_execution_identity;
    bool shadow_outcome_routed = false;
    bool promotion = false;
    SIQSShadowProofRssCorpusDigest record_digest;

    [[nodiscard]] constexpr SIQSShadowProofRssGateOutcome gate_outcome() const noexcept {
        SIQSShadowProofRssGateOutcome outcome;
        outcome.status = status;
        outcome.reason = reason;
        outcome.total_sample_count = static_cast<std::size_t>(total_sample_count);
        outcome.valid_off_sample_count = valid_off_sample_count;
        outcome.valid_observe_sample_count = valid_observe_sample_count;
        outcome.rss_limit_bytes = rss_limit_bytes;
        outcome.max_observe_peak_rss_bytes = max_observe_peak_rss_bytes;
        outcome.policy_binding_digest = policy_binding_digest;
        outcome.probe_execution_identity = probe_execution_identity;
        outcome.shadow_outcome_routed = shadow_outcome_routed;
        outcome.promotion = promotion;
        return outcome;
    }

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowProofRssTerminalGateRecord&,
               const SIQSShadowProofRssTerminalGateRecord&) noexcept = default;
};

enum class SIQSShadowProofRssTerminalGateRecordCodecError : std::uint8_t {
    none,
    truncated,
    trailing_bytes,
    invalid_magic,
    unsupported_wire_version,
    declared_size_mismatch,
    unsupported_schema_version,
    invalid_status_tag,
    invalid_reason_tag,
    invalid_boolean,
    nonzero_reserved,
    nonzero_tail,
    invalid_terminal_outcome,
    invalid_probe_execution_identity,
    digest_mismatch,
};

[[nodiscard]] constexpr std::string_view
siqs_shadow_proof_rss_terminal_gate_record_codec_error_name(
    SIQSShadowProofRssTerminalGateRecordCodecError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssTerminalGateRecordCodecError::none:
        return "none";
    case SIQSShadowProofRssTerminalGateRecordCodecError::truncated:
        return "truncated";
    case SIQSShadowProofRssTerminalGateRecordCodecError::trailing_bytes:
        return "trailing_bytes";
    case SIQSShadowProofRssTerminalGateRecordCodecError::invalid_magic:
        return "invalid_magic";
    case SIQSShadowProofRssTerminalGateRecordCodecError::unsupported_wire_version:
        return "unsupported_wire_version";
    case SIQSShadowProofRssTerminalGateRecordCodecError::declared_size_mismatch:
        return "declared_size_mismatch";
    case SIQSShadowProofRssTerminalGateRecordCodecError::unsupported_schema_version:
        return "unsupported_schema_version";
    case SIQSShadowProofRssTerminalGateRecordCodecError::invalid_status_tag:
        return "invalid_status_tag";
    case SIQSShadowProofRssTerminalGateRecordCodecError::invalid_reason_tag:
        return "invalid_reason_tag";
    case SIQSShadowProofRssTerminalGateRecordCodecError::invalid_boolean:
        return "invalid_boolean";
    case SIQSShadowProofRssTerminalGateRecordCodecError::nonzero_reserved:
        return "nonzero_reserved";
    case SIQSShadowProofRssTerminalGateRecordCodecError::nonzero_tail:
        return "nonzero_tail";
    case SIQSShadowProofRssTerminalGateRecordCodecError::invalid_terminal_outcome:
        return "invalid_terminal_outcome";
    case SIQSShadowProofRssTerminalGateRecordCodecError::invalid_probe_execution_identity:
        return "invalid_probe_execution_identity";
    case SIQSShadowProofRssTerminalGateRecordCodecError::digest_mismatch:
        return "digest_mismatch";
    }
    return "unknown";
}

struct SIQSShadowProofRssTerminalGateRecordEncodeResult final {
    std::optional<std::array<std::byte, SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE>>
        bytes;
    SIQSShadowProofRssTerminalGateRecordCodecError error =
        SIQSShadowProofRssTerminalGateRecordCodecError::none;
    std::size_t error_offset = SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_NO_ERROR_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return bytes.has_value() && error == SIQSShadowProofRssTerminalGateRecordCodecError::none;
    }
};

struct SIQSShadowProofRssTerminalGateRecordDecodeResult final {
    std::optional<SIQSShadowProofRssTerminalGateRecord> record;
    SIQSShadowProofRssTerminalGateRecordCodecError error =
        SIQSShadowProofRssTerminalGateRecordCodecError::none;
    std::size_t error_offset = SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_NO_ERROR_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return record.has_value() && error == SIQSShadowProofRssTerminalGateRecordCodecError::none;
    }
};

namespace terminal_gate_record_codec_detail {

using Error = SIQSShadowProofRssTerminalGateRecordCodecError;
using Record = SIQSShadowProofRssTerminalGateRecord;

inline constexpr std::array<char, 8> MAGIC{'G', 'N', 'F', 'S', 'T', 'G', 'R', 'C'};

struct Status final {
    Error error = Error::none;
    std::size_t offset = SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_NO_ERROR_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return error == Error::none;
    }
};

[[nodiscard]] constexpr Status failure(Error error, std::size_t offset) noexcept {
    return {error, offset};
}

[[nodiscard]] constexpr bool digest_is_nonzero(SIQSShadowProofRssCorpusDigest digest) noexcept {
    return digest.low != 0 || digest.high != 0;
}

[[nodiscard]] constexpr std::optional<std::uint8_t>
status_tag(SIQSShadowProofRssGateStatus status) noexcept {
    switch (status) {
    case SIQSShadowProofRssGateStatus::limit_exceeded:
        return 1;
    case SIQSShadowProofRssGateStatus::manual_review_candidate:
        return 2;
    case SIQSShadowProofRssGateStatus::blocked:
    case SIQSShadowProofRssGateStatus::invalid:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssGateStatus>
status_from_tag(std::uint8_t tag) noexcept {
    switch (tag) {
    case 1:
        return SIQSShadowProofRssGateStatus::limit_exceeded;
    case 2:
        return SIQSShadowProofRssGateStatus::manual_review_candidate;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<std::uint8_t>
reason_tag(SIQSShadowProofRssGateReason reason) noexcept {
    switch (reason) {
    case SIQSShadowProofRssGateReason::observe_peak_over_limit:
        return 1;
    case SIQSShadowProofRssGateReason::all_observe_peaks_within_limit:
        return 2;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssGateReason>
reason_from_tag(std::uint8_t tag) noexcept {
    switch (tag) {
    case 1:
        return SIQSShadowProofRssGateReason::observe_peak_over_limit;
    case 2:
        return SIQSShadowProofRssGateReason::all_observe_peaks_within_limit;
    default:
        return std::nullopt;
    }
}

template <std::size_t Size>
constexpr void write_u8(std::array<std::byte, Size>& bytes, std::size_t offset,
                        std::uint8_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value);
}

template <std::size_t Size>
constexpr void write_u32(std::array<std::byte, Size>& bytes, std::size_t offset,
                         std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        write_u8(bytes, offset++, static_cast<std::uint8_t>(value >> shift));
    }
}

template <std::size_t Size>
constexpr void write_u64(std::array<std::byte, Size>& bytes, std::size_t offset,
                         std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        write_u8(bytes, offset++, static_cast<std::uint8_t>(value >> shift));
    }
}

template <std::size_t Size>
constexpr void write_digest(std::array<std::byte, Size>& bytes, std::size_t offset,
                            SIQSShadowProofRssCorpusDigest digest) noexcept {
    write_u64(bytes, offset, digest.low);
    write_u64(bytes, offset + 8, digest.high);
}

template <std::size_t Size>
constexpr void write_sha256(std::array<std::byte, Size>& bytes, std::size_t offset,
                            const util::Sha256Digest& digest) noexcept {
    for (const std::byte byte : digest.bytes) {
        bytes[offset++] = byte;
    }
}

[[nodiscard]] constexpr std::uint8_t read_u8(std::span<const std::byte> bytes,
                                             std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] constexpr std::uint32_t read_u32(std::span<const std::byte> bytes,
                                               std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(read_u8(bytes, offset++)) << shift;
    }
    return value;
}

[[nodiscard]] constexpr std::uint64_t read_u64(std::span<const std::byte> bytes,
                                               std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(read_u8(bytes, offset++)) << shift;
    }
    return value;
}

[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest read_digest(std::span<const std::byte> bytes,
                                                                   std::size_t offset) noexcept {
    return {read_u64(bytes, offset), read_u64(bytes, offset + 8)};
}

[[nodiscard]] constexpr util::Sha256Digest read_sha256(std::span<const std::byte> bytes,
                                                       std::size_t offset) noexcept {
    util::Sha256Digest digest;
    for (std::byte& byte : digest.bytes) {
        byte = bytes[offset++];
    }
    return digest;
}

[[nodiscard]] constexpr Status check_zero_range(std::span<const std::byte> bytes, std::size_t begin,
                                                std::size_t end, Error error) noexcept {
    for (std::size_t offset = begin; offset < end; ++offset) {
        if (read_u8(bytes, offset) != 0) {
            return failure(error, offset);
        }
    }
    return {};
}

[[nodiscard]] constexpr Status check_frame(std::span<const std::byte> bytes) noexcept {
    constexpr std::size_t COMMON_PREFIX_SIZE = 16;
    if (bytes.size() < COMMON_PREFIX_SIZE) {
        return failure(Error::truncated, bytes.size());
    }
    for (std::size_t index = 0; index < MAGIC.size(); ++index) {
        if (read_u8(bytes, index) !=
            static_cast<std::uint8_t>(static_cast<unsigned char>(MAGIC[index]))) {
            return failure(Error::invalid_magic, index);
        }
    }
    if (read_u32(bytes, TERMINAL_GATE_WIRE_VERSION_OFFSET) !=
        SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_VERSION) {
        return failure(Error::unsupported_wire_version, TERMINAL_GATE_WIRE_VERSION_OFFSET);
    }
    if (read_u32(bytes, TERMINAL_GATE_WIRE_SIZE_OFFSET) !=
        SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE) {
        return failure(Error::declared_size_mismatch, TERMINAL_GATE_WIRE_SIZE_OFFSET);
    }
    if (bytes.size() < SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE) {
        return failure(Error::truncated, bytes.size());
    }
    if (bytes.size() > SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE) {
        return failure(Error::trailing_bytes, SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE);
    }
    return {};
}

[[nodiscard]] constexpr Status validate_terminal_fields(const Record& record) noexcept {
    if (record.schema_version != SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_SCHEMA_VERSION) {
        return failure(Error::unsupported_schema_version, TERMINAL_GATE_SCHEMA_OFFSET);
    }
    if (!status_tag(record.status).has_value()) {
        return failure(Error::invalid_status_tag, TERMINAL_GATE_STATUS_OFFSET);
    }
    if (!reason_tag(record.reason).has_value()) {
        return failure(Error::invalid_reason_tag, TERMINAL_GATE_REASON_OFFSET);
    }
    if (!digest_is_nonzero(record.plan_digest)) {
        return failure(Error::invalid_terminal_outcome, TERMINAL_GATE_PLAN_DIGEST_OFFSET);
    }
    if (!digest_is_nonzero(record.final_journal_record_digest)) {
        return failure(Error::invalid_terminal_outcome, TERMINAL_GATE_FINAL_JOURNAL_DIGEST_OFFSET);
    }
    if (!digest_is_nonzero(record.policy_binding_digest)) {
        return failure(Error::invalid_terminal_outcome, TERMINAL_GATE_POLICY_DIGEST_OFFSET);
    }
    if (!siqs_shadow_proof_rss_probe_execution_identity_is_valid(record.probe_execution_identity)) {
        return failure(Error::invalid_probe_execution_identity,
                       TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET);
    }
    if (record.shadow_outcome_routed || record.promotion ||
        record.total_sample_count != SIQS_SHADOW_PROOF_RSS_GATE_EXPECTED_SAMPLE_COUNT ||
        record.valid_off_sample_count !=
            SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT * SIQS_SHADOW_PROOF_RSS_GATE_OFF_REPETITIONS ||
        record.valid_observe_sample_count != SIQS_SHADOW_PROOF_RSS_GATE_FIXTURE_COUNT *
                                                 SIQS_SHADOW_PROOF_RSS_GATE_OBSERVE_REPETITIONS ||
        record.rss_limit_bytes == 0 || record.max_observe_peak_rss_bytes == 0) {
        return failure(Error::invalid_terminal_outcome, TERMINAL_GATE_STATUS_OFFSET);
    }
    if (record.status == SIQSShadowProofRssGateStatus::limit_exceeded) {
        if (record.reason != SIQSShadowProofRssGateReason::observe_peak_over_limit ||
            record.max_observe_peak_rss_bytes <= record.rss_limit_bytes) {
            return failure(Error::invalid_terminal_outcome, TERMINAL_GATE_STATUS_OFFSET);
        }
        return {};
    }
    if (record.reason != SIQSShadowProofRssGateReason::all_observe_peaks_within_limit ||
        record.max_observe_peak_rss_bytes > record.rss_limit_bytes) {
        return failure(Error::invalid_terminal_outcome, TERMINAL_GATE_STATUS_OFFSET);
    }
    return {};
}

template <typename Result>
[[nodiscard]] constexpr Result result_failure(Error error, std::size_t offset) noexcept {
    Result result;
    result.error = error;
    result.error_offset = offset;
    return result;
}

} // namespace terminal_gate_record_codec_detail

/// Stable non-cryptographic self-check over every semantic record field. It
/// deliberately excludes record_digest itself and uses a domain distinct from
/// journal records and policy bindings.
[[nodiscard]] constexpr SIQSShadowProofRssCorpusDigest
terminal_gate_record_digest(const SIQSShadowProofRssTerminalGateRecord& record) noexcept {
    using namespace terminal_gate_record_codec_detail;
    shadow_proof_rss_campaign_journal_detail::DigestBuilder builder;
    builder.append_string("gnfs.siqs.shadow_proof_rss_terminal_gate_record.v1");
    builder.append_u64(record.schema_version);
    builder.append_digest(record.plan_digest);
    builder.append_digest(record.final_journal_record_digest);
    builder.append_u64(status_tag(record.status).value_or(UINT8_MAX));
    builder.append_u64(reason_tag(record.reason).value_or(UINT8_MAX));
    builder.append_u64(record.total_sample_count);
    builder.append_u64(record.valid_off_sample_count);
    builder.append_u64(record.valid_observe_sample_count);
    builder.append_u64(record.rss_limit_bytes);
    builder.append_u64(record.max_observe_peak_rss_bytes);
    builder.append_digest(record.policy_binding_digest);
    builder.append_probe_execution_identity(record.probe_execution_identity);
    builder.append_bool(record.shadow_outcome_routed);
    builder.append_bool(record.promotion);
    return builder.finish();
}

[[nodiscard]] constexpr bool
terminal_gate_record_is_valid(const SIQSShadowProofRssTerminalGateRecord& record) noexcept {
    const auto fields = terminal_gate_record_codec_detail::validate_terminal_fields(record);
    return static_cast<bool>(fields) && record.record_digest == terminal_gate_record_digest(record);
}

/// Construct an exact typed record from one already-evaluated terminal outcome.
/// Blocked, invalid, routed, promoted, incomplete, or unbound outcomes are
/// rejected rather than normalized.
[[nodiscard]] constexpr std::optional<SIQSShadowProofRssTerminalGateRecord>
make_terminal_gate_record(SIQSShadowProofRssCorpusDigest plan_digest,
                          SIQSShadowProofRssCorpusDigest final_journal_record_digest,
                          const SIQSShadowProofRssGateOutcome& outcome) noexcept {
    if (outcome.total_sample_count > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    SIQSShadowProofRssTerminalGateRecord record;
    record.plan_digest = plan_digest;
    record.final_journal_record_digest = final_journal_record_digest;
    record.status = outcome.status;
    record.reason = outcome.reason;
    record.total_sample_count = static_cast<std::uint32_t>(outcome.total_sample_count);
    record.valid_off_sample_count = outcome.valid_off_sample_count;
    record.valid_observe_sample_count = outcome.valid_observe_sample_count;
    record.rss_limit_bytes = outcome.rss_limit_bytes;
    record.max_observe_peak_rss_bytes = outcome.max_observe_peak_rss_bytes;
    record.policy_binding_digest = outcome.policy_binding_digest;
    record.probe_execution_identity = outcome.probe_execution_identity;
    record.shadow_outcome_routed = outcome.shadow_outcome_routed;
    record.promotion = outcome.promotion;
    if (!terminal_gate_record_codec_detail::validate_terminal_fields(record)) {
        return std::nullopt;
    }
    record.record_digest = terminal_gate_record_digest(record);
    return record;
}

[[nodiscard]] constexpr SIQSShadowProofRssTerminalGateRecordEncodeResult
encode_terminal_gate_record(const SIQSShadowProofRssTerminalGateRecord& record) noexcept {
    using namespace terminal_gate_record_codec_detail;
    if (const Status fields = validate_terminal_fields(record); !fields) {
        return result_failure<SIQSShadowProofRssTerminalGateRecordEncodeResult>(fields.error,
                                                                                fields.offset);
    }
    if (record.record_digest != terminal_gate_record_digest(record)) {
        return result_failure<SIQSShadowProofRssTerminalGateRecordEncodeResult>(
            Error::digest_mismatch, TERMINAL_GATE_RECORD_DIGEST_OFFSET);
    }

    std::array<std::byte, SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE> bytes{};
    for (std::size_t index = 0; index < MAGIC.size(); ++index) {
        write_u8(bytes, TERMINAL_GATE_MAGIC_OFFSET + index,
                 static_cast<std::uint8_t>(static_cast<unsigned char>(MAGIC[index])));
    }
    write_u32(bytes, TERMINAL_GATE_WIRE_VERSION_OFFSET,
              SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_VERSION);
    write_u32(bytes, TERMINAL_GATE_WIRE_SIZE_OFFSET,
              static_cast<std::uint32_t>(SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE));
    write_u32(bytes, TERMINAL_GATE_SCHEMA_OFFSET, record.schema_version);
    write_u8(bytes, TERMINAL_GATE_STATUS_OFFSET, *status_tag(record.status));
    write_u8(bytes, TERMINAL_GATE_REASON_OFFSET, *reason_tag(record.reason));
    write_u8(bytes, TERMINAL_GATE_ROUTED_OFFSET,
             record.shadow_outcome_routed ? UINT8_C(1) : UINT8_C(0));
    write_u8(bytes, TERMINAL_GATE_PROMOTION_OFFSET, record.promotion ? UINT8_C(1) : UINT8_C(0));
    write_u32(bytes, TERMINAL_GATE_TOTAL_SAMPLE_COUNT_OFFSET, record.total_sample_count);
    write_u32(bytes, TERMINAL_GATE_VALID_OFF_SAMPLE_COUNT_OFFSET, record.valid_off_sample_count);
    write_u32(bytes, TERMINAL_GATE_VALID_OBSERVE_SAMPLE_COUNT_OFFSET,
              record.valid_observe_sample_count);
    write_u64(bytes, TERMINAL_GATE_RSS_LIMIT_OFFSET, record.rss_limit_bytes);
    write_u64(bytes, TERMINAL_GATE_MAX_OBSERVE_PEAK_OFFSET, record.max_observe_peak_rss_bytes);
    write_digest(bytes, TERMINAL_GATE_PLAN_DIGEST_OFFSET, record.plan_digest);
    write_digest(bytes, TERMINAL_GATE_FINAL_JOURNAL_DIGEST_OFFSET,
                 record.final_journal_record_digest);
    write_digest(bytes, TERMINAL_GATE_POLICY_DIGEST_OFFSET, record.policy_binding_digest);
    write_sha256(bytes, TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET,
                 record.probe_execution_identity.executable_sha256);
    write_sha256(bytes, TERMINAL_GATE_EXECUTION_CONTRACT_SHA256_OFFSET,
                 record.probe_execution_identity.execution_contract_sha256);
    write_digest(bytes, TERMINAL_GATE_RECORD_DIGEST_OFFSET, record.record_digest);

    SIQSShadowProofRssTerminalGateRecordEncodeResult result;
    result.bytes = bytes;
    return result;
}

[[nodiscard]] constexpr SIQSShadowProofRssTerminalGateRecordDecodeResult
decode_terminal_gate_record(std::span<const std::byte> bytes) noexcept {
    using namespace terminal_gate_record_codec_detail;
    using DecodeResult = SIQSShadowProofRssTerminalGateRecordDecodeResult;
    if (const Status frame = check_frame(bytes); !frame) {
        return result_failure<DecodeResult>(frame.error, frame.offset);
    }

    Record record;
    record.schema_version = read_u32(bytes, TERMINAL_GATE_SCHEMA_OFFSET);
    if (record.schema_version != SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_SCHEMA_VERSION) {
        return result_failure<DecodeResult>(Error::unsupported_schema_version,
                                            TERMINAL_GATE_SCHEMA_OFFSET);
    }
    const auto status = status_from_tag(read_u8(bytes, TERMINAL_GATE_STATUS_OFFSET));
    if (!status.has_value()) {
        return result_failure<DecodeResult>(Error::invalid_status_tag, TERMINAL_GATE_STATUS_OFFSET);
    }
    record.status = *status;
    const auto reason = reason_from_tag(read_u8(bytes, TERMINAL_GATE_REASON_OFFSET));
    if (!reason.has_value()) {
        return result_failure<DecodeResult>(Error::invalid_reason_tag, TERMINAL_GATE_REASON_OFFSET);
    }
    record.reason = *reason;

    const std::uint8_t routed = read_u8(bytes, TERMINAL_GATE_ROUTED_OFFSET);
    if (routed > 1) {
        return result_failure<DecodeResult>(Error::invalid_boolean, TERMINAL_GATE_ROUTED_OFFSET);
    }
    record.shadow_outcome_routed = routed != 0;
    const std::uint8_t promotion = read_u8(bytes, TERMINAL_GATE_PROMOTION_OFFSET);
    if (promotion > 1) {
        return result_failure<DecodeResult>(Error::invalid_boolean, TERMINAL_GATE_PROMOTION_OFFSET);
    }
    record.promotion = promotion != 0;

    record.total_sample_count = read_u32(bytes, TERMINAL_GATE_TOTAL_SAMPLE_COUNT_OFFSET);
    record.valid_off_sample_count = read_u32(bytes, TERMINAL_GATE_VALID_OFF_SAMPLE_COUNT_OFFSET);
    record.valid_observe_sample_count =
        read_u32(bytes, TERMINAL_GATE_VALID_OBSERVE_SAMPLE_COUNT_OFFSET);
    if (const Status reserved =
            check_zero_range(bytes, 36, TERMINAL_GATE_RSS_LIMIT_OFFSET, Error::nonzero_reserved);
        !reserved) {
        return result_failure<DecodeResult>(reserved.error, reserved.offset);
    }
    record.rss_limit_bytes = read_u64(bytes, TERMINAL_GATE_RSS_LIMIT_OFFSET);
    record.max_observe_peak_rss_bytes = read_u64(bytes, TERMINAL_GATE_MAX_OBSERVE_PEAK_OFFSET);
    record.plan_digest = read_digest(bytes, TERMINAL_GATE_PLAN_DIGEST_OFFSET);
    record.final_journal_record_digest =
        read_digest(bytes, TERMINAL_GATE_FINAL_JOURNAL_DIGEST_OFFSET);
    record.policy_binding_digest = read_digest(bytes, TERMINAL_GATE_POLICY_DIGEST_OFFSET);
    record.probe_execution_identity.executable_sha256 =
        read_sha256(bytes, TERMINAL_GATE_EXECUTABLE_SHA256_OFFSET);
    record.probe_execution_identity.execution_contract_sha256 =
        read_sha256(bytes, TERMINAL_GATE_EXECUTION_CONTRACT_SHA256_OFFSET);
    record.record_digest = read_digest(bytes, TERMINAL_GATE_RECORD_DIGEST_OFFSET);

    if (const Status tail = check_zero_range(bytes, TERMINAL_GATE_TAIL_OFFSET,
                                             SIQS_SHADOW_PROOF_RSS_TERMINAL_GATE_RECORD_WIRE_SIZE,
                                             Error::nonzero_tail);
        !tail) {
        return result_failure<DecodeResult>(tail.error, tail.offset);
    }
    if (const Status fields = validate_terminal_fields(record); !fields) {
        return result_failure<DecodeResult>(fields.error, fields.offset);
    }
    if (record.record_digest != terminal_gate_record_digest(record)) {
        return result_failure<DecodeResult>(Error::digest_mismatch,
                                            TERMINAL_GATE_RECORD_DIGEST_OFFSET);
    }

    DecodeResult result;
    result.record = record;
    return result;
}

} // namespace gnfs::siqs::shadow_proof_rss_terminal_gate_record_detail
