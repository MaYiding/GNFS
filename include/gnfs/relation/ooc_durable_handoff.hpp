#pragma once

/// @file ooc_durable_handoff.hpp
/// @brief Pure V1 values and canonical codec for an immutable private OOC handoff.

#include <gnfs/relation/ooc_relation_format.hpp>
#include <gnfs/util/durable_immutable_record.hpp>
#include <gnfs/util/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace gnfs::relation {

inline constexpr std::uint32_t OOC_PRIVATE_HANDOFF_SCHEMA_VERSION_V1 = 1;
inline constexpr std::uint32_t OOC_PRIVATE_HANDOFF_WIRE_VERSION_V1 = 1;
inline constexpr std::uint64_t OOC_PRIVATE_HANDOFF_PLATFORM_POSIX_V1 = 1;
inline constexpr std::uint64_t OOC_PRIVATE_HANDOFF_PLATFORM_WINDOWS_V1 = 2;
#ifdef _WIN32
inline constexpr std::uint64_t OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1 =
    OOC_PRIVATE_HANDOFF_PLATFORM_WINDOWS_V1;
#else
inline constexpr std::uint64_t OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1 =
    OOC_PRIVATE_HANDOFF_PLATFORM_POSIX_V1;
#endif
inline constexpr std::size_t OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES = 64U * 1024U;
inline constexpr std::size_t OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 = 328;
inline constexpr std::size_t OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES =
    OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 + OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES;
inline constexpr std::uint64_t OOC_PRIVATE_HANDOFF_NO_OFFSET =
    std::numeric_limits<std::uint64_t>::max();

enum class OOCPrivateHandoffProtocolCode : std::uint8_t {
    none,
    input_too_large,
    output_too_large,
    truncated,
    trailing_bytes,
    invalid_magic,
    unsupported_wire_version,
    unsupported_schema_version,
    platform_mismatch,
    declared_size_mismatch,
    invalid_value,
    payload_too_large,
    integer_out_of_range,
    digest_mismatch,
    digest_unavailable,
    resource_exhausted,
};

[[nodiscard]] std::string_view
ooc_private_handoff_protocol_code_name(OOCPrivateHandoffProtocolCode code) noexcept;

struct OOCPrivateHandoffProtocolStatus final {
    OOCPrivateHandoffProtocolCode code = OOCPrivateHandoffProtocolCode::none;
    std::uint64_t byte_offset = OOC_PRIVATE_HANDOFF_NO_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return code == OOCPrivateHandoffProtocolCode::none;
    }
};

struct OOCPrivateHandoffPairDescriptorV1 final {
    std::uint64_t format_version = 0;
    std::uint64_t store_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t count = 0;
    std::uint64_t index_extent = 0;
    std::uint64_t data_extent = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const OOCPrivateHandoffPairDescriptorV1&,
               const OOCPrivateHandoffPairDescriptorV1&) noexcept = default;
};

struct OOCPrivateHandoffArtifactBindingV1 final {
    util::durable_immutable_record::NativeIdentity identity;
    std::uint64_t extent = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const OOCPrivateHandoffArtifactBindingV1&,
               const OOCPrivateHandoffArtifactBindingV1&) noexcept = default;
};

struct OOCPrivateHandoffRecordV1 final {
    std::uint32_t schema_version = OOC_PRIVATE_HANDOFF_SCHEMA_VERSION_V1;
    std::uint64_t platform_id = OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1;
    std::array<std::uint64_t, 2> lease_id{};
    util::durable_immutable_record::NativeIdentity lock_identity;
    util::durable_immutable_record::NativeIdentity directory_identity;
    util::durable_immutable_record::NativeIdentity owner_marker_identity;
    util::durable_immutable_record::NativeIdentity owned_marker_identity;
    OOCPrivateHandoffPairDescriptorV1 pair;
    OOCPrivateHandoffArtifactBindingV1 index;
    OOCPrivateHandoffArtifactBindingV1 data;
    std::uint32_t payload_kind = 0;
    std::uint32_t payload_version = 0;
    std::vector<std::byte> opaque_payload;
    util::Sha256Digest payload_digest;
    util::Sha256Digest self_digest;

    [[nodiscard]] friend bool operator==(const OOCPrivateHandoffRecordV1&,
                                         const OOCPrivateHandoffRecordV1&) noexcept = default;
};

struct OOCPrivateHandoffProtocolEncodeResult final {
    std::optional<std::vector<std::byte>> bytes;
    OOCPrivateHandoffProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return bytes.has_value() && static_cast<bool>(status);
    }
};

struct OOCPrivateHandoffProtocolDecodeResult final {
    std::optional<OOCPrivateHandoffRecordV1> value;
    OOCPrivateHandoffProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return value.has_value() && static_cast<bool>(status);
    }
};

struct OOCPrivateHandoffProtocolDigestResult final {
    std::optional<util::Sha256Digest> digest;
    OOCPrivateHandoffProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return digest.has_value() && static_cast<bool>(status);
    }
};

[[nodiscard]] OOCPrivateHandoffProtocolStatus
validate_ooc_private_handoff_record(const OOCPrivateHandoffRecordV1& record,
                                    bool verify_self_digest = true) noexcept;

[[nodiscard]] OOCPrivateHandoffProtocolDigestResult
ooc_private_handoff_record_digest(const OOCPrivateHandoffRecordV1& record) noexcept;

[[nodiscard]] OOCPrivateHandoffProtocolStatus
seal_ooc_private_handoff_record(OOCPrivateHandoffRecordV1& record) noexcept;

[[nodiscard]] OOCPrivateHandoffProtocolEncodeResult
encode_ooc_private_handoff_record(const OOCPrivateHandoffRecordV1& record) noexcept;

[[nodiscard]] OOCPrivateHandoffProtocolDecodeResult
decode_ooc_private_handoff_record(std::span<const std::byte> bytes) noexcept;

} // namespace gnfs::relation
