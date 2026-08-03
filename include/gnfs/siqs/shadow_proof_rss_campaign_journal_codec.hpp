#pragma once

/// @file shadow_proof_rss_campaign_journal_codec.hpp
/// @brief Pure fixed-width wire codec for SIQS RSS campaign journal values.

#include <gnfs/siqs/shadow_proof_rss_campaign_journal.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace gnfs::siqs {

inline constexpr uint32_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_WIRE_VERSION = 3;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE = 160;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE = 320;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET =
    std::numeric_limits<std::size_t>::max();

inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_MAGIC_OFFSET = 0;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET = 8;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET = 12;

inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET = 16;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PROBE_KIND_OFFSET = 20;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_POLICY_DIGEST_OFFSET = 24;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_RUNTIME_DIGEST_OFFSET = 40;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PLAN_DIGEST_OFFSET = 56;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SLOT_COUNT_OFFSET = 72;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_CONCURRENCY_OFFSET = 76;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTABLE_SHA256_OFFSET = 80;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTION_CONTRACT_SHA256_OFFSET =
    112;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET = 144;

inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET = 16;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET = 20;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET = 24;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PREVIOUS_DIGEST_OFFSET = 32;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PLAN_DIGEST_OFFSET = 48;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_NUMBER_OFFSET = 64;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_DIGEST_OFFSET = 72;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PAYLOAD_OFFSET = 88;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET = 96;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET = 100;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PROBE_KIND_OFFSET = 101;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET = 104;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET = 112;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_CURRENT_RSS_OFFSET = 120;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PEAK_GROWTH_OFFSET = 128;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WALL_NS_OFFSET = 136;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET = 144;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET = 176;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET = 208;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTABLE_SHA256_OFFSET = 240;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTION_CONTRACT_SHA256_OFFSET =
    272;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET = 304;
inline constexpr std::size_t SIQS_SHADOW_PROOF_RSS_JOURNAL_ARTIFACT_SEAL_WIRE_SIZE = 32;

enum class SIQSShadowProofRssCampaignJournalCodecError : uint8_t {
    none,
    truncated,
    trailing_bytes,
    invalid_magic,
    unsupported_wire_version,
    declared_size_mismatch,
    unsupported_journal_schema_version,
    nonzero_reserved,
    invalid_boolean,
    invalid_record_kind,
    invalid_probe_kind,
    invalid_operating_system,
    invalid_architecture,
    invalid_memory_backend,
    invalid_factor_identity,
    invalid_evidence,
    invalid_artifact_kind,
    invalid_optional_mask,
    noncanonical_absent_value,
    invalid_probe_execution_identity,
    integer_out_of_range,
    digest_mismatch,
};

[[nodiscard]] constexpr std::string_view siqs_shadow_proof_rss_campaign_journal_codec_error_name(
    SIQSShadowProofRssCampaignJournalCodecError error) noexcept {
    switch (error) {
    case SIQSShadowProofRssCampaignJournalCodecError::none:
        return "none";
    case SIQSShadowProofRssCampaignJournalCodecError::truncated:
        return "truncated";
    case SIQSShadowProofRssCampaignJournalCodecError::trailing_bytes:
        return "trailing_bytes";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_magic:
        return "invalid_magic";
    case SIQSShadowProofRssCampaignJournalCodecError::unsupported_wire_version:
        return "unsupported_wire_version";
    case SIQSShadowProofRssCampaignJournalCodecError::declared_size_mismatch:
        return "declared_size_mismatch";
    case SIQSShadowProofRssCampaignJournalCodecError::unsupported_journal_schema_version:
        return "unsupported_journal_schema_version";
    case SIQSShadowProofRssCampaignJournalCodecError::nonzero_reserved:
        return "nonzero_reserved";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_boolean:
        return "invalid_boolean";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_record_kind:
        return "invalid_record_kind";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_probe_kind:
        return "invalid_probe_kind";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_operating_system:
        return "invalid_operating_system";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_architecture:
        return "invalid_architecture";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_memory_backend:
        return "invalid_memory_backend";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_factor_identity:
        return "invalid_factor_identity";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_evidence:
        return "invalid_evidence";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_artifact_kind:
        return "invalid_artifact_kind";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_optional_mask:
        return "invalid_optional_mask";
    case SIQSShadowProofRssCampaignJournalCodecError::noncanonical_absent_value:
        return "noncanonical_absent_value";
    case SIQSShadowProofRssCampaignJournalCodecError::invalid_probe_execution_identity:
        return "invalid_probe_execution_identity";
    case SIQSShadowProofRssCampaignJournalCodecError::integer_out_of_range:
        return "integer_out_of_range";
    case SIQSShadowProofRssCampaignJournalCodecError::digest_mismatch:
        return "digest_mismatch";
    }
    return "unknown";
}

template <std::size_t Size> struct SIQSShadowProofRssCampaignJournalEncodeResult final {
    std::optional<std::array<std::byte, Size>> bytes;
    SIQSShadowProofRssCampaignJournalCodecError error =
        SIQSShadowProofRssCampaignJournalCodecError::none;
    std::size_t error_offset = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return bytes.has_value() && error == SIQSShadowProofRssCampaignJournalCodecError::none;
    }
};

template <typename Value> struct SIQSShadowProofRssCampaignJournalDecodeResult final {
    std::optional<Value> value;
    SIQSShadowProofRssCampaignJournalCodecError error =
        SIQSShadowProofRssCampaignJournalCodecError::none;
    std::size_t error_offset = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value.has_value() && error == SIQSShadowProofRssCampaignJournalCodecError::none;
    }
};

using SIQSShadowProofRssCampaignJournalHeaderEncodeResult =
    SIQSShadowProofRssCampaignJournalEncodeResult<
        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE>;
using SIQSShadowProofRssCampaignJournalRecordEncodeResult =
    SIQSShadowProofRssCampaignJournalEncodeResult<
        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE>;
using SIQSShadowProofRssCampaignJournalHeaderDecodeResult =
    SIQSShadowProofRssCampaignJournalDecodeResult<SIQSShadowProofRssCampaignJournalHeader>;
using SIQSShadowProofRssCampaignJournalRecordDecodeResult =
    SIQSShadowProofRssCampaignJournalDecodeResult<SIQSShadowProofRssCampaignJournalRecord>;

namespace shadow_proof_rss_campaign_journal_codec_detail {

inline constexpr std::array<char, 8> HEADER_MAGIC{'G', 'N', 'F', 'S', 'R', 'J', 'H', 'D'};
inline constexpr std::array<char, 8> RECORD_MAGIC{'G', 'N', 'F', 'S', 'R', 'J', 'R', 'C'};
inline constexpr uint8_t OPTIONAL_MASK_ALL = 0x1fU;

struct Status final {
    SIQSShadowProofRssCampaignJournalCodecError error =
        SIQSShadowProofRssCampaignJournalCodecError::none;
    std::size_t offset = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_NO_ERROR_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return error == SIQSShadowProofRssCampaignJournalCodecError::none;
    }
};

[[nodiscard]] constexpr Status failure(SIQSShadowProofRssCampaignJournalCodecError error,
                                       std::size_t offset) noexcept {
    return {error, offset};
}

template <std::size_t Size>
constexpr void write_u8(std::array<std::byte, Size>& bytes, std::size_t offset,
                        uint8_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value);
}

template <std::size_t Size>
constexpr void write_u32(std::array<std::byte, Size>& bytes, std::size_t offset,
                         uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        write_u8(bytes, offset++, static_cast<uint8_t>(value >> shift));
    }
}

template <std::size_t Size>
constexpr void write_u64(std::array<std::byte, Size>& bytes, std::size_t offset,
                         uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        write_u8(bytes, offset++, static_cast<uint8_t>(value >> shift));
    }
}

template <std::size_t Size>
constexpr void write_magic(std::array<std::byte, Size>& bytes, const std::array<char, 8>& magic) {
    for (std::size_t index = 0; index < magic.size(); ++index) {
        write_u8(bytes, index, static_cast<uint8_t>(static_cast<unsigned char>(magic[index])));
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

[[nodiscard]] constexpr uint8_t read_u8(std::span<const std::byte> bytes,
                                        std::size_t offset) noexcept {
    return std::to_integer<uint8_t>(bytes[offset]);
}

[[nodiscard]] constexpr uint32_t read_u32(std::span<const std::byte> bytes,
                                          std::size_t offset) noexcept {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(read_u8(bytes, offset++)) << shift;
    }
    return value;
}

[[nodiscard]] constexpr uint64_t read_u64(std::span<const std::byte> bytes,
                                          std::size_t offset) noexcept {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(read_u8(bytes, offset++)) << shift;
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

[[nodiscard]] constexpr bool sha256_is_zero(const util::Sha256Digest& digest) noexcept {
    for (const std::byte byte : digest.bytes) {
        if (byte != std::byte{0}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr Status check_reserved(std::span<const std::byte> bytes, std::size_t begin,
                                              std::size_t end) noexcept {
    for (std::size_t offset = begin; offset < end; ++offset) {
        if (read_u8(bytes, offset) != 0) {
            return failure(SIQSShadowProofRssCampaignJournalCodecError::nonzero_reserved, offset);
        }
    }
    return {};
}

template <std::size_t ExpectedSize>
[[nodiscard]] constexpr Status check_frame(std::span<const std::byte> bytes,
                                           const std::array<char, 8>& magic) noexcept {
    constexpr std::size_t COMMON_PREFIX_SIZE = 16;
    if (bytes.size() < COMMON_PREFIX_SIZE) {
        return failure(SIQSShadowProofRssCampaignJournalCodecError::truncated, bytes.size());
    }
    for (std::size_t index = 0; index < magic.size(); ++index) {
        if (read_u8(bytes, index) !=
            static_cast<uint8_t>(static_cast<unsigned char>(magic[index]))) {
            return failure(SIQSShadowProofRssCampaignJournalCodecError::invalid_magic, index);
        }
    }
    if (read_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET) !=
        SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_WIRE_VERSION) {
        return failure(SIQSShadowProofRssCampaignJournalCodecError::unsupported_wire_version,
                       SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET);
    }
    if (read_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET) != ExpectedSize) {
        return failure(SIQSShadowProofRssCampaignJournalCodecError::declared_size_mismatch,
                       SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET);
    }
    if (bytes.size() < ExpectedSize) {
        return failure(SIQSShadowProofRssCampaignJournalCodecError::truncated, bytes.size());
    }
    if (bytes.size() > ExpectedSize) {
        return failure(SIQSShadowProofRssCampaignJournalCodecError::trailing_bytes, ExpectedSize);
    }
    return {};
}

[[nodiscard]] constexpr Status
validate_probe_execution_identity(const SIQSShadowProofRssProbeExecutionIdentity& identity,
                                  bool must_be_present, std::size_t executable_offset,
                                  std::size_t contract_offset) noexcept {
    const bool executable_zero = sha256_is_zero(identity.executable_sha256);
    if (executable_zero == must_be_present) {
        return failure(
            SIQSShadowProofRssCampaignJournalCodecError::invalid_probe_execution_identity,
            executable_offset);
    }
    const bool contract_zero = sha256_is_zero(identity.execution_contract_sha256);
    if (contract_zero == must_be_present) {
        return failure(
            SIQSShadowProofRssCampaignJournalCodecError::invalid_probe_execution_identity,
            contract_offset);
    }
    return {};
}

[[nodiscard]] constexpr std::optional<uint8_t>
record_kind_tag(SIQSShadowProofRssJournalRecordKind value) noexcept {
    switch (value) {
    case SIQSShadowProofRssJournalRecordKind::slot_started:
        return 1;
    case SIQSShadowProofRssJournalRecordKind::slot_committed:
        return 2;
    case SIQSShadowProofRssJournalRecordKind::campaign_tainted:
        return 3;
    case SIQSShadowProofRssJournalRecordKind::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssJournalRecordKind>
record_kind_from_tag(uint8_t value) noexcept {
    switch (value) {
    case 1:
        return SIQSShadowProofRssJournalRecordKind::slot_started;
    case 2:
        return SIQSShadowProofRssJournalRecordKind::slot_committed;
    case 3:
        return SIQSShadowProofRssJournalRecordKind::campaign_tainted;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<uint8_t>
probe_kind_tag(SIQSShadowProofRssProbeKind value) noexcept {
    switch (value) {
    case SIQSShadowProofRssProbeKind::unknown:
        return 0;
    case SIQSShadowProofRssProbeKind::synthetic_test:
        return 1;
    case SIQSShadowProofRssProbeKind::production_holdout:
        return 2;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssProbeKind>
probe_kind_from_tag(uint8_t value) noexcept {
    switch (value) {
    case 0:
        return SIQSShadowProofRssProbeKind::unknown;
    case 1:
        return SIQSShadowProofRssProbeKind::synthetic_test;
    case 2:
        return SIQSShadowProofRssProbeKind::production_holdout;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<uint8_t>
operating_system_tag(SIQSShadowProofRssOperatingSystem value) noexcept {
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
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssOperatingSystem>
operating_system_from_tag(uint8_t value) noexcept {
    switch (value) {
    case 0:
        return SIQSShadowProofRssOperatingSystem::unknown;
    case 1:
        return SIQSShadowProofRssOperatingSystem::darwin;
    case 2:
        return SIQSShadowProofRssOperatingSystem::linux;
    case 3:
        return SIQSShadowProofRssOperatingSystem::windows;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<uint8_t>
architecture_tag(SIQSShadowProofRssArchitecture value) noexcept {
    switch (value) {
    case SIQSShadowProofRssArchitecture::unknown:
        return 0;
    case SIQSShadowProofRssArchitecture::x86_64:
        return 1;
    case SIQSShadowProofRssArchitecture::arm64:
        return 2;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssArchitecture>
architecture_from_tag(uint8_t value) noexcept {
    switch (value) {
    case 0:
        return SIQSShadowProofRssArchitecture::unknown;
    case 1:
        return SIQSShadowProofRssArchitecture::x86_64;
    case 2:
        return SIQSShadowProofRssArchitecture::arm64;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<uint8_t>
memory_backend_tag(util::ProcessMemoryBackend value) noexcept {
    switch (value) {
    case util::ProcessMemoryBackend::Unsupported:
        return 0;
    case util::ProcessMemoryBackend::DarwinGetrusage:
        return 1;
    case util::ProcessMemoryBackend::LinuxGetrusage:
        return 2;
    case util::ProcessMemoryBackend::WindowsPsapi:
        return 3;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<util::ProcessMemoryBackend>
memory_backend_from_tag(uint8_t value) noexcept {
    switch (value) {
    case 0:
        return util::ProcessMemoryBackend::Unsupported;
    case 1:
        return util::ProcessMemoryBackend::DarwinGetrusage;
    case 2:
        return util::ProcessMemoryBackend::LinuxGetrusage;
    case 3:
        return util::ProcessMemoryBackend::WindowsPsapi;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<uint8_t>
factor_identity_tag(SIQSShadowProofRssFactorIdentity value) noexcept {
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
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssFactorIdentity>
factor_identity_from_tag(uint8_t value) noexcept {
    switch (value) {
    case 0:
        return SIQSShadowProofRssFactorIdentity::unknown;
    case 1:
        return SIQSShadowProofRssFactorIdentity::pass;
    case 2:
        return SIQSShadowProofRssFactorIdentity::fail;
    case 3:
        return SIQSShadowProofRssFactorIdentity::not_checked;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<uint8_t>
evidence_tag(SIQSShadowProofRssEvidence value) noexcept {
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
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssEvidence>
evidence_from_tag(uint8_t value) noexcept {
    switch (value) {
    case 0:
        return SIQSShadowProofRssEvidence::unknown;
    case 1:
        return SIQSShadowProofRssEvidence::not_applicable;
    case 2:
        return SIQSShadowProofRssEvidence::pass;
    case 3:
        return SIQSShadowProofRssEvidence::fail;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<uint8_t>
artifact_kind_tag(SIQSShadowProofRssArtifactKind value) noexcept {
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
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SIQSShadowProofRssArtifactKind>
artifact_kind_from_tag(uint8_t value) noexcept {
    switch (value) {
    case 0:
        return SIQSShadowProofRssArtifactKind::unknown;
    case 1:
        return SIQSShadowProofRssArtifactKind::probe_stdout;
    case 2:
        return SIQSShadowProofRssArtifactKind::probe_stderr;
    case 3:
        return SIQSShadowProofRssArtifactKind::joined_gate_sample;
    default:
        return std::nullopt;
    }
}

template <std::size_t Size>
[[nodiscard]] constexpr Status
write_artifact_seal(std::array<std::byte, Size>& bytes, std::size_t offset,
                    const SIQSShadowProofRssArtifactSeal& seal) noexcept {
    const auto kind = artifact_kind_tag(seal.kind);
    if (!kind.has_value()) {
        return failure(SIQSShadowProofRssCampaignJournalCodecError::invalid_artifact_kind,
                       offset + 1);
    }
    write_u8(bytes, offset, seal.committed ? 1 : 0);
    write_u8(bytes, offset + 1, *kind);
    write_u64(bytes, offset + 8, seal.byte_count);
    write_digest(bytes, offset + 16, seal.digest);
    return {};
}

[[nodiscard]] constexpr Status read_artifact_seal(std::span<const std::byte> bytes,
                                                  std::size_t offset,
                                                  SIQSShadowProofRssArtifactSeal& seal) noexcept {
    const uint8_t committed = read_u8(bytes, offset);
    if (committed > 1) {
        return failure(SIQSShadowProofRssCampaignJournalCodecError::invalid_boolean, offset);
    }
    const auto kind = artifact_kind_from_tag(read_u8(bytes, offset + 1));
    if (!kind.has_value()) {
        return failure(SIQSShadowProofRssCampaignJournalCodecError::invalid_artifact_kind,
                       offset + 1);
    }
    if (const Status reserved = check_reserved(bytes, offset + 2, offset + 8); !reserved) {
        return reserved;
    }
    seal.committed = committed != 0;
    seal.kind = *kind;
    seal.byte_count = read_u64(bytes, offset + 8);
    seal.digest = read_digest(bytes, offset + 16);
    return {};
}

template <std::size_t Size>
[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalEncodeResult<Size>
encode_failure(SIQSShadowProofRssCampaignJournalCodecError error, std::size_t offset) noexcept {
    SIQSShadowProofRssCampaignJournalEncodeResult<Size> result;
    result.error = error;
    result.error_offset = offset;
    return result;
}

template <typename Value>
[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalDecodeResult<Value>
decode_failure(SIQSShadowProofRssCampaignJournalCodecError error, std::size_t offset) noexcept {
    SIQSShadowProofRssCampaignJournalDecodeResult<Value> result;
    result.error = error;
    result.error_offset = offset;
    return result;
}

} // namespace shadow_proof_rss_campaign_journal_codec_detail

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalHeaderEncodeResult
encode_siqs_shadow_proof_rss_campaign_journal_header(
    const SIQSShadowProofRssCampaignJournalHeader& header) noexcept {
    using namespace shadow_proof_rss_campaign_journal_codec_detail;
    using Error = SIQSShadowProofRssCampaignJournalCodecError;
    constexpr std::size_t Size = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE;

    if (header.schema_version != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION) {
        return encode_failure<Size>(Error::unsupported_journal_schema_version,
                                    SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET);
    }
    const auto probe_kind = probe_kind_tag(header.probe_kind);
    if (!probe_kind.has_value() || header.probe_kind == SIQSShadowProofRssProbeKind::unknown) {
        return encode_failure<Size>(Error::invalid_probe_kind,
                                    SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PROBE_KIND_OFFSET);
    }
    if (const Status identity = validate_probe_execution_identity(
            header.probe_execution_identity, true,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTABLE_SHA256_OFFSET,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTION_CONTRACT_SHA256_OFFSET);
        !identity) {
        return encode_failure<Size>(identity.error, identity.offset);
    }
    if (header.header_digest != shadow_proof_rss_campaign_journal_detail::header_digest(header)) {
        return encode_failure<Size>(Error::digest_mismatch,
                                    SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET);
    }

    std::array<std::byte, Size> bytes{};
    write_magic(bytes, HEADER_MAGIC);
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET,
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_WIRE_VERSION);
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET, static_cast<uint32_t>(Size));
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET, header.schema_version);
    write_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PROBE_KIND_OFFSET, *probe_kind);
    write_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_POLICY_DIGEST_OFFSET,
                 header.policy_binding_digest);
    write_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_RUNTIME_DIGEST_OFFSET,
                 header.runtime_facts_digest);
    write_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PLAN_DIGEST_OFFSET,
                 header.plan_digest);
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SLOT_COUNT_OFFSET, header.slot_count);
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_CONCURRENCY_OFFSET,
              header.max_concurrency);
    write_sha256(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTABLE_SHA256_OFFSET,
                 header.probe_execution_identity.executable_sha256);
    write_sha256(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTION_CONTRACT_SHA256_OFFSET,
                 header.probe_execution_identity.execution_contract_sha256);
    write_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET, header.header_digest);

    SIQSShadowProofRssCampaignJournalHeaderEncodeResult result;
    result.bytes = bytes;
    return result;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalHeaderDecodeResult
decode_siqs_shadow_proof_rss_campaign_journal_header(std::span<const std::byte> bytes) noexcept {
    using namespace shadow_proof_rss_campaign_journal_codec_detail;
    using Error = SIQSShadowProofRssCampaignJournalCodecError;

    if (const Status frame = check_frame<SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_HEADER_WIRE_SIZE>(
            bytes, HEADER_MAGIC);
        !frame) {
        return decode_failure<SIQSShadowProofRssCampaignJournalHeader>(frame.error, frame.offset);
    }
    SIQSShadowProofRssCampaignJournalHeader header;
    header.schema_version = read_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET);
    if (header.schema_version != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION) {
        return decode_failure<SIQSShadowProofRssCampaignJournalHeader>(
            Error::unsupported_journal_schema_version,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SCHEMA_OFFSET);
    }
    const auto probe_kind =
        probe_kind_from_tag(read_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PROBE_KIND_OFFSET));
    if (!probe_kind.has_value() || *probe_kind == SIQSShadowProofRssProbeKind::unknown) {
        return decode_failure<SIQSShadowProofRssCampaignJournalHeader>(
            Error::invalid_probe_kind, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PROBE_KIND_OFFSET);
    }
    header.probe_kind = *probe_kind;
    if (const Status reserved = check_reserved(bytes, 21, 24); !reserved) {
        return decode_failure<SIQSShadowProofRssCampaignJournalHeader>(reserved.error,
                                                                       reserved.offset);
    }
    header.policy_binding_digest =
        read_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_POLICY_DIGEST_OFFSET);
    header.runtime_facts_digest =
        read_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_RUNTIME_DIGEST_OFFSET);
    header.plan_digest =
        read_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_PLAN_DIGEST_OFFSET);
    header.slot_count = read_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_SLOT_COUNT_OFFSET);
    header.max_concurrency =
        read_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_CONCURRENCY_OFFSET);
    header.probe_execution_identity.executable_sha256 =
        read_sha256(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTABLE_SHA256_OFFSET);
    header.probe_execution_identity.execution_contract_sha256 =
        read_sha256(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTION_CONTRACT_SHA256_OFFSET);
    if (const Status identity = validate_probe_execution_identity(
            header.probe_execution_identity, true,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTABLE_SHA256_OFFSET,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_EXECUTION_CONTRACT_SHA256_OFFSET);
        !identity) {
        return decode_failure<SIQSShadowProofRssCampaignJournalHeader>(identity.error,
                                                                       identity.offset);
    }
    header.header_digest = read_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET);
    if (header.header_digest != shadow_proof_rss_campaign_journal_detail::header_digest(header)) {
        return decode_failure<SIQSShadowProofRssCampaignJournalHeader>(
            Error::digest_mismatch, SIQS_SHADOW_PROOF_RSS_JOURNAL_HEADER_DIGEST_OFFSET);
    }

    SIQSShadowProofRssCampaignJournalHeaderDecodeResult result;
    result.value = header;
    return result;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalRecordEncodeResult
encode_siqs_shadow_proof_rss_campaign_journal_record(
    const SIQSShadowProofRssCampaignJournalRecord& record) noexcept {
    using namespace shadow_proof_rss_campaign_journal_codec_detail;
    using Error = SIQSShadowProofRssCampaignJournalCodecError;
    constexpr std::size_t Size = SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE;

    if (record.schema_version != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION) {
        return encode_failure<Size>(Error::unsupported_journal_schema_version,
                                    SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET);
    }
    const auto record_kind = record_kind_tag(record.kind);
    if (!record_kind.has_value()) {
        return encode_failure<Size>(Error::invalid_record_kind,
                                    SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET);
    }
    const auto probe_kind = probe_kind_tag(record.commit_payload.deployment_probe_kind);
    const bool probe_kind_is_canonical =
        probe_kind.has_value() &&
        (record.kind != SIQSShadowProofRssJournalRecordKind::slot_committed ||
         record.commit_payload.deployment_probe_kind != SIQSShadowProofRssProbeKind::unknown);
    if (!probe_kind_is_canonical) {
        return encode_failure<Size>(Error::invalid_probe_kind,
                                    SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PROBE_KIND_OFFSET);
    }
    const auto operating_system =
        operating_system_tag(record.commit_payload.actual_operating_system);
    if (!operating_system.has_value()) {
        return encode_failure<Size>(Error::invalid_operating_system, 88);
    }
    const auto architecture = architecture_tag(record.commit_payload.actual_architecture);
    if (!architecture.has_value()) {
        return encode_failure<Size>(Error::invalid_architecture, 89);
    }
    const auto memory_backend = memory_backend_tag(record.commit_payload.actual_memory_backend);
    if (!memory_backend.has_value()) {
        return encode_failure<Size>(Error::invalid_memory_backend, 90);
    }
    const auto factor_identity = factor_identity_tag(record.commit_payload.factor_identity);
    if (!factor_identity.has_value()) {
        return encode_failure<Size>(Error::invalid_factor_identity, 93);
    }
    const auto proof_evidence = evidence_tag(record.commit_payload.proof_evidence);
    if (!proof_evidence.has_value()) {
        return encode_failure<Size>(Error::invalid_evidence, 94);
    }
    const auto matrix_evidence = evidence_tag(record.commit_payload.matrix_evidence);
    if (!matrix_evidence.has_value()) {
        return encode_failure<Size>(Error::invalid_evidence, 95);
    }
    if (record.commit_payload.actual_resolved_sieve_workers >
        static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        return encode_failure<Size>(Error::integer_out_of_range,
                                    SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET);
    }
    if (const Status identity = validate_probe_execution_identity(
            record.commit_payload.probe_execution_identity,
            record.kind == SIQSShadowProofRssJournalRecordKind::slot_committed,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTABLE_SHA256_OFFSET,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTION_CONTRACT_SHA256_OFFSET);
        !identity) {
        return encode_failure<Size>(identity.error, identity.offset);
    }
    if (record.record_digest != shadow_proof_rss_campaign_journal_detail::record_digest(record)) {
        return encode_failure<Size>(Error::digest_mismatch,
                                    SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET);
    }

    std::array<std::byte, Size> bytes{};
    write_magic(bytes, RECORD_MAGIC);
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_VERSION_OFFSET,
              SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_WIRE_VERSION);
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_WIRE_SIZE_OFFSET, static_cast<uint32_t>(Size));
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET, record.schema_version);
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET, record.sequence_number);
    write_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET, *record_kind);
    write_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PREVIOUS_DIGEST_OFFSET,
                 record.previous_record_digest);
    write_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PLAN_DIGEST_OFFSET,
                 record.plan_digest);
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_NUMBER_OFFSET, record.slot_number);
    write_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_DIGEST_OFFSET,
                 record.slot_digest);

    const auto& payload = record.commit_payload;
    write_u8(bytes, 88, *operating_system);
    write_u8(bytes, 89, *architecture);
    write_u8(bytes, 90, *memory_backend);
    write_u8(bytes, 91, payload.fresh_process ? 1 : 0);
    write_u8(bytes, 92, payload.completed ? 1 : 0);
    write_u8(bytes, 93, *factor_identity);
    write_u8(bytes, 94, *proof_evidence);
    write_u8(bytes, 95, *matrix_evidence);
    write_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET,
              static_cast<uint32_t>(payload.actual_resolved_sieve_workers));

    uint8_t optional_mask = 0;
    optional_mask |= payload.absolute_peak_rss_bytes.has_value() ? 0x01U : 0;
    optional_mask |= payload.observe_minus_off_peak_bytes.has_value() ? 0x02U : 0;
    optional_mask |= payload.current_rss_bytes.has_value() ? 0x04U : 0;
    optional_mask |= payload.peak_growth_bytes.has_value() ? 0x08U : 0;
    optional_mask |= payload.wall_ns.has_value() ? 0x10U : 0;
    write_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET, optional_mask);
    write_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PROBE_KIND_OFFSET, *probe_kind);
    write_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET,
              payload.absolute_peak_rss_bytes.value_or(0));
    write_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET,
              std::bit_cast<uint64_t>(payload.observe_minus_off_peak_bytes.value_or(0)));
    write_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_CURRENT_RSS_OFFSET,
              payload.current_rss_bytes.value_or(0));
    write_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PEAK_GROWTH_OFFSET,
              payload.peak_growth_bytes.value_or(0));
    write_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WALL_NS_OFFSET,
              payload.wall_ns.value_or(0));

    if (const Status status = write_artifact_seal(
            bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET, payload.stdout_seal);
        !status) {
        return encode_failure<Size>(status.error, status.offset);
    }
    if (const Status status = write_artifact_seal(
            bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET, payload.stderr_seal);
        !status) {
        return encode_failure<Size>(status.error, status.offset);
    }
    if (const Status status =
            write_artifact_seal(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET,
                                payload.joined_sample_seal);
        !status) {
        return encode_failure<Size>(status.error, status.offset);
    }
    write_sha256(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTABLE_SHA256_OFFSET,
                 payload.probe_execution_identity.executable_sha256);
    write_sha256(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTION_CONTRACT_SHA256_OFFSET,
                 payload.probe_execution_identity.execution_contract_sha256);
    write_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET, record.record_digest);

    SIQSShadowProofRssCampaignJournalRecordEncodeResult result;
    result.bytes = bytes;
    return result;
}

[[nodiscard]] constexpr SIQSShadowProofRssCampaignJournalRecordDecodeResult
decode_siqs_shadow_proof_rss_campaign_journal_record(std::span<const std::byte> bytes) noexcept {
    using namespace shadow_proof_rss_campaign_journal_codec_detail;
    using Error = SIQSShadowProofRssCampaignJournalCodecError;
    using Record = SIQSShadowProofRssCampaignJournalRecord;

    if (const Status frame = check_frame<SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_RECORD_WIRE_SIZE>(
            bytes, RECORD_MAGIC);
        !frame) {
        return decode_failure<Record>(frame.error, frame.offset);
    }
    Record record;
    record.schema_version = read_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET);
    if (record.schema_version != SIQS_SHADOW_PROOF_RSS_CAMPAIGN_JOURNAL_SCHEMA_VERSION) {
        return decode_failure<Record>(Error::unsupported_journal_schema_version,
                                      SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SCHEMA_OFFSET);
    }
    record.sequence_number = read_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SEQUENCE_OFFSET);
    const auto record_kind =
        record_kind_from_tag(read_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET));
    if (!record_kind.has_value()) {
        return decode_failure<Record>(Error::invalid_record_kind,
                                      SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_KIND_OFFSET);
    }
    record.kind = *record_kind;
    if (const Status reserved = check_reserved(bytes, 25, 32); !reserved) {
        return decode_failure<Record>(reserved.error, reserved.offset);
    }
    record.previous_record_digest =
        read_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PREVIOUS_DIGEST_OFFSET);
    record.plan_digest =
        read_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PLAN_DIGEST_OFFSET);
    record.slot_number = read_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_NUMBER_OFFSET);
    if (const Status reserved = check_reserved(bytes, 68, 72); !reserved) {
        return decode_failure<Record>(reserved.error, reserved.offset);
    }
    record.slot_digest =
        read_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_SLOT_DIGEST_OFFSET);

    auto& payload = record.commit_payload;
    const auto operating_system = operating_system_from_tag(read_u8(bytes, 88));
    if (!operating_system.has_value()) {
        return decode_failure<Record>(Error::invalid_operating_system, 88);
    }
    payload.actual_operating_system = *operating_system;
    const auto architecture = architecture_from_tag(read_u8(bytes, 89));
    if (!architecture.has_value()) {
        return decode_failure<Record>(Error::invalid_architecture, 89);
    }
    payload.actual_architecture = *architecture;
    const auto memory_backend = memory_backend_from_tag(read_u8(bytes, 90));
    if (!memory_backend.has_value()) {
        return decode_failure<Record>(Error::invalid_memory_backend, 90);
    }
    payload.actual_memory_backend = *memory_backend;

    const uint8_t fresh_process = read_u8(bytes, 91);
    if (fresh_process > 1) {
        return decode_failure<Record>(Error::invalid_boolean, 91);
    }
    payload.fresh_process = fresh_process != 0;
    const uint8_t completed = read_u8(bytes, 92);
    if (completed > 1) {
        return decode_failure<Record>(Error::invalid_boolean, 92);
    }
    payload.completed = completed != 0;

    const auto factor_identity = factor_identity_from_tag(read_u8(bytes, 93));
    if (!factor_identity.has_value()) {
        return decode_failure<Record>(Error::invalid_factor_identity, 93);
    }
    payload.factor_identity = *factor_identity;
    const auto proof_evidence = evidence_from_tag(read_u8(bytes, 94));
    if (!proof_evidence.has_value()) {
        return decode_failure<Record>(Error::invalid_evidence, 94);
    }
    payload.proof_evidence = *proof_evidence;
    const auto matrix_evidence = evidence_from_tag(read_u8(bytes, 95));
    if (!matrix_evidence.has_value()) {
        return decode_failure<Record>(Error::invalid_evidence, 95);
    }
    payload.matrix_evidence = *matrix_evidence;

    const uint32_t workers = read_u32(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET);
    if (static_cast<uint64_t>(workers) >
        static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return decode_failure<Record>(Error::integer_out_of_range,
                                      SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WORKERS_OFFSET);
    }
    payload.actual_resolved_sieve_workers = static_cast<std::size_t>(workers);

    const uint8_t optional_mask =
        read_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET);
    if ((optional_mask & static_cast<uint8_t>(~OPTIONAL_MASK_ALL)) != 0) {
        return decode_failure<Record>(Error::invalid_optional_mask,
                                      SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OPTIONAL_MASK_OFFSET);
    }
    const auto probe_kind =
        probe_kind_from_tag(read_u8(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PROBE_KIND_OFFSET));
    const bool probe_kind_is_canonical =
        probe_kind.has_value() &&
        (record.kind != SIQSShadowProofRssJournalRecordKind::slot_committed ||
         *probe_kind != SIQSShadowProofRssProbeKind::unknown);
    if (!probe_kind_is_canonical) {
        return decode_failure<Record>(Error::invalid_probe_kind,
                                      SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PROBE_KIND_OFFSET);
    }
    payload.deployment_probe_kind = *probe_kind;
    if (const Status reserved = check_reserved(bytes, 102, 104); !reserved) {
        return decode_failure<Record>(reserved.error, reserved.offset);
    }

    const auto read_optional_unsigned =
        [&](uint8_t bit, std::size_t offset,
            std::optional<uint64_t>& destination) constexpr -> Status {
        const uint64_t value = read_u64(bytes, offset);
        if ((optional_mask & bit) == 0) {
            if (value != 0) {
                return failure(Error::noncanonical_absent_value, offset);
            }
            destination.reset();
        } else {
            destination = value;
        }
        return {};
    };
    if (const Status status =
            read_optional_unsigned(0x01U, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_ABSOLUTE_PEAK_OFFSET,
                                   payload.absolute_peak_rss_bytes);
        !status) {
        return decode_failure<Record>(status.error, status.offset);
    }

    const uint64_t observe_delta_bits =
        read_u64(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET);
    if ((optional_mask & 0x02U) == 0) {
        if (observe_delta_bits != 0) {
            return decode_failure<Record>(
                Error::noncanonical_absent_value,
                SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_OBSERVE_DELTA_OFFSET);
        }
        payload.observe_minus_off_peak_bytes.reset();
    } else {
        payload.observe_minus_off_peak_bytes = std::bit_cast<int64_t>(observe_delta_bits);
    }
    if (const Status status =
            read_optional_unsigned(0x04U, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_CURRENT_RSS_OFFSET,
                                   payload.current_rss_bytes);
        !status) {
        return decode_failure<Record>(status.error, status.offset);
    }
    if (const Status status =
            read_optional_unsigned(0x08U, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_PEAK_GROWTH_OFFSET,
                                   payload.peak_growth_bytes);
        !status) {
        return decode_failure<Record>(status.error, status.offset);
    }
    if (const Status status = read_optional_unsigned(
            0x10U, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_WALL_NS_OFFSET, payload.wall_ns);
        !status) {
        return decode_failure<Record>(status.error, status.offset);
    }

    if (const Status status = read_artifact_seal(
            bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDOUT_SEAL_OFFSET, payload.stdout_seal);
        !status) {
        return decode_failure<Record>(status.error, status.offset);
    }
    if (const Status status = read_artifact_seal(
            bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_STDERR_SEAL_OFFSET, payload.stderr_seal);
        !status) {
        return decode_failure<Record>(status.error, status.offset);
    }
    if (const Status status =
            read_artifact_seal(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_JOINED_SEAL_OFFSET,
                               payload.joined_sample_seal);
        !status) {
        return decode_failure<Record>(status.error, status.offset);
    }
    payload.probe_execution_identity.executable_sha256 =
        read_sha256(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTABLE_SHA256_OFFSET);
    payload.probe_execution_identity.execution_contract_sha256 =
        read_sha256(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTION_CONTRACT_SHA256_OFFSET);
    if (const Status identity = validate_probe_execution_identity(
            payload.probe_execution_identity,
            record.kind == SIQSShadowProofRssJournalRecordKind::slot_committed,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTABLE_SHA256_OFFSET,
            SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_EXECUTION_CONTRACT_SHA256_OFFSET);
        !identity) {
        return decode_failure<Record>(identity.error, identity.offset);
    }

    record.record_digest = read_digest(bytes, SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET);
    if (record.record_digest != shadow_proof_rss_campaign_journal_detail::record_digest(record)) {
        return decode_failure<Record>(Error::digest_mismatch,
                                      SIQS_SHADOW_PROOF_RSS_JOURNAL_RECORD_DIGEST_OFFSET);
    }

    SIQSShadowProofRssCampaignJournalRecordDecodeResult result;
    result.value = record;
    return result;
}

} // namespace gnfs::siqs
