#pragma once

/// @file ooc_authorized_cleanup_intent.hpp
/// @brief Pure V2 wire contract for externally authorized private-handoff cleanup.

#include <gnfs/relation/ooc_durable_handoff.hpp>
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

inline constexpr std::uint32_t OOC_AUTHORIZED_CLEANUP_INTENT_SCHEMA_VERSION_V2 = 2;
inline constexpr std::uint32_t OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_VERSION_V1 = 1;
inline constexpr std::array<char, 8> OOC_AUTHORIZED_CLEANUP_INTENT_MAGIC_V2{
    'G', 'N', 'F', 'S', 'A', 'C', 'I', '2',
};
inline constexpr std::uint64_t OOC_AUTHORIZED_CLEANUP_INTENT_PLATFORM_POSIX_V1 = 1;
inline constexpr std::uint64_t OOC_AUTHORIZED_CLEANUP_INTENT_PLATFORM_WINDOWS_V1 = 2;
#ifdef _WIN32
inline constexpr std::uint64_t OOC_AUTHORIZED_CLEANUP_INTENT_CURRENT_PLATFORM_V1 =
    OOC_AUTHORIZED_CLEANUP_INTENT_PLATFORM_WINDOWS_V1;
#else
inline constexpr std::uint64_t OOC_AUTHORIZED_CLEANUP_INTENT_CURRENT_PLATFORM_V1 =
    OOC_AUTHORIZED_CLEANUP_INTENT_PLATFORM_POSIX_V1;
#endif
inline constexpr std::size_t OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 = 480;
inline constexpr std::uint64_t OOC_AUTHORIZED_CLEANUP_INTENT_NO_OFFSET =
    std::numeric_limits<std::uint64_t>::max();

enum class OOCAuthorizedCleanupMarkerKindV2 : std::uint32_t {
    intent = 1,
    staged = 2,
};

enum class OOCAuthorizedCleanupIntentProtocolCode : std::uint8_t {
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
    unexpected_marker_kind,
    integer_out_of_range,
    digest_mismatch,
    digest_unavailable,
    resource_exhausted,
};

[[nodiscard]] std::string_view ooc_authorized_cleanup_intent_protocol_code_name(
    OOCAuthorizedCleanupIntentProtocolCode code) noexcept;

struct OOCAuthorizedCleanupIntentProtocolStatus final {
    OOCAuthorizedCleanupIntentProtocolCode code = OOCAuthorizedCleanupIntentProtocolCode::none;
    std::uint64_t byte_offset = OOC_AUTHORIZED_CLEANUP_INTENT_NO_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return code == OOCAuthorizedCleanupIntentProtocolCode::none;
    }
};

/// Canonical deletion authority for one application-authorized generic handoff.
///
/// This pure value is not itself a capability and cannot publish or resume a
/// cleanup transaction. The runtime may create its canonical namespace record
/// only after combining a typed application authorization with the matching
/// locked adoption authority. V1 cleanup-intent bytes use a different codec
/// and remain semantically unchanged.
struct OOCAuthorizedCleanupIntentV2 final {
    std::uint32_t schema_version = OOC_AUTHORIZED_CLEANUP_INTENT_SCHEMA_VERSION_V2;
    std::uint64_t platform_id = OOC_AUTHORIZED_CLEANUP_INTENT_CURRENT_PLATFORM_V1;
    OOCAuthorizedCleanupMarkerKindV2 marker_kind = OOCAuthorizedCleanupMarkerKindV2::intent;
    /// SHA-256 of the frozen path.native() code-unit bytes, exactly matching
    /// the existing private-lease frozen_path_digest() algorithm. No
    /// canonicalization, Unicode conversion, or separator normalization is
    /// permitted.
    util::Sha256Digest base_path_digest;
    util::Sha256Digest external_authorization_digest;
    util::Sha256Digest generic_handoff_self_digest;
    std::array<std::uint64_t, 2> lease_id{};
    util::durable_immutable_record::NativeIdentity parent_directory_identity;
    util::durable_immutable_record::NativeIdentity lock_identity;
    util::durable_immutable_record::NativeIdentity directory_identity;
    util::durable_immutable_record::NativeIdentity owner_marker_identity;
    util::durable_immutable_record::NativeIdentity owned_marker_identity;
    OOCPrivateHandoffPairDescriptorV1 pair;
    /// Snapshot of the canonical generic-handoff record leaf itself, including
    /// the exact encoded-record extent. This does not describe its opaque
    /// payload.
    OOCPrivateHandoffArtifactBindingV1 handoff;
    /// Duplicate pending leaf observed by the locked adopter, if any. This is
    /// a pure observation, not deletion authority. The current runtime design
    /// must remove and durably confirm this leaf before V2 canonical commit,
    /// require this field to be absent in canonical markers, and taint any
    /// pending leaf that appears afterward.
    std::optional<OOCPrivateHandoffArtifactBindingV1> pending_handoff;
    OOCPrivateHandoffArtifactBindingV1 index;
    OOCPrivateHandoffArtifactBindingV1 data;
    util::Sha256Digest self_digest;

    [[nodiscard]] friend constexpr bool
    operator==(const OOCAuthorizedCleanupIntentV2&,
               const OOCAuthorizedCleanupIntentV2&) noexcept = default;
};

struct OOCAuthorizedCleanupIntentProtocolEncodeResult final {
    std::optional<std::vector<std::byte>> bytes;
    OOCAuthorizedCleanupIntentProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return bytes.has_value() && static_cast<bool>(status);
    }
};

struct OOCAuthorizedCleanupIntentProtocolDecodeResult final {
    std::optional<OOCAuthorizedCleanupIntentV2> value;
    OOCAuthorizedCleanupIntentProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return value.has_value() && static_cast<bool>(status);
    }
};

struct OOCAuthorizedCleanupIntentProtocolDigestResult final {
    std::optional<util::Sha256Digest> digest;
    OOCAuthorizedCleanupIntentProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return digest.has_value() && static_cast<bool>(status);
    }
};

[[nodiscard]] OOCAuthorizedCleanupIntentProtocolStatus
validate_ooc_authorized_cleanup_intent(const OOCAuthorizedCleanupIntentV2& intent,
                                       bool verify_self_digest = true) noexcept;

[[nodiscard]] OOCAuthorizedCleanupIntentProtocolDigestResult
ooc_authorized_cleanup_intent_digest(const OOCAuthorizedCleanupIntentV2& intent) noexcept;

[[nodiscard]] OOCAuthorizedCleanupIntentProtocolStatus
seal_ooc_authorized_cleanup_intent(OOCAuthorizedCleanupIntentV2& intent) noexcept;

[[nodiscard]] OOCAuthorizedCleanupIntentProtocolEncodeResult
encode_ooc_authorized_cleanup_intent(const OOCAuthorizedCleanupIntentV2& intent) noexcept;

[[nodiscard]] OOCAuthorizedCleanupIntentProtocolDecodeResult
decode_ooc_authorized_cleanup_intent(std::span<const std::byte> bytes,
                                     OOCAuthorizedCleanupMarkerKindV2 expected_kind) noexcept;

} // namespace gnfs::relation
