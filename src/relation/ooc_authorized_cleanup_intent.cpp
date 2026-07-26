#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace gnfs::relation {
namespace ooc_authorized_cleanup_intent_detail {

using Code = OOCAuthorizedCleanupIntentProtocolCode;
using NativeIdentity = util::durable_immutable_record::NativeIdentity;
using Status = OOCAuthorizedCleanupIntentProtocolStatus;

constexpr auto& RECORD_MAGIC = OOC_AUTHORIZED_CLEANUP_INTENT_MAGIC_V2;
constexpr std::string_view RECORD_DIGEST_DOMAIN = "GNFS-OOC-AUTHORIZED-CLEANUP-INTENT-V2";
constexpr std::array<std::byte, 1> DIGEST_DOMAIN_SEPARATOR{std::byte{0}};

constexpr std::size_t MAGIC_OFFSET = 0;
constexpr std::size_t WIRE_VERSION_OFFSET = 8;
constexpr std::size_t SCHEMA_VERSION_OFFSET = 12;
constexpr std::size_t PLATFORM_ID_OFFSET = 16;
constexpr std::size_t DECLARED_SIZE_OFFSET = 24;
constexpr std::size_t MARKER_KIND_OFFSET = 28;
constexpr std::size_t PENDING_PRESENCE_OFFSET = 344;
constexpr std::size_t PENDING_RESERVED_OFFSET = 348;
constexpr std::size_t PENDING_ARTIFACT_OFFSET = 352;
constexpr std::size_t SELF_DIGEST_OFFSET =
    OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 - util::SHA256_DIGEST_BYTES;
constexpr std::size_t MAX_INPUT_BYTES = OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 + 64U * 1024U;

static_assert(SELF_DIGEST_OFFSET == 448);
static_assert(OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 <=
              static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));

[[nodiscard]] constexpr Status
failure(Code code, std::uint64_t byte_offset = OOC_AUTHORIZED_CLEANUP_INTENT_NO_OFFSET) noexcept {
    return {code, byte_offset};
}

[[nodiscard]] constexpr bool identity_is_zero(const NativeIdentity& identity) noexcept {
    return identity.first == 0 && identity.second == 0 && identity.third == 0;
}

[[nodiscard]] constexpr bool
lease_id_is_zero(const std::array<std::uint64_t, 2>& lease_id) noexcept {
    return lease_id[0] == 0 && lease_id[1] == 0;
}

[[nodiscard]] constexpr bool
identities_are_valid_and_distinct(const OOCAuthorizedCleanupIntentV2& intent) noexcept {
    const std::array<NativeIdentity, 8> identities{
        intent.parent_directory_identity,
        intent.lock_identity,
        intent.directory_identity,
        intent.owner_marker_identity,
        intent.owned_marker_identity,
        intent.handoff.identity,
        intent.index.identity,
        intent.data.identity,
    };
    for (std::size_t left = 0; left < identities.size(); ++left) {
        if (identity_is_zero(identities[left])) {
            return false;
        }
        for (std::size_t right = left + 1; right < identities.size(); ++right) {
            if (identities[left] == identities[right]) {
                return false;
            }
        }
    }
    if (intent.pending_handoff.has_value()) {
        if (identity_is_zero(intent.pending_handoff->identity)) {
            return false;
        }
        for (const auto& identity : identities) {
            if (intent.pending_handoff->identity == identity) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] constexpr Status
validate_structural_fields(const OOCAuthorizedCleanupIntentV2& intent) noexcept {
    if (intent.schema_version != OOC_AUTHORIZED_CLEANUP_INTENT_SCHEMA_VERSION_V2) {
        return failure(Code::unsupported_schema_version);
    }
    if (intent.platform_id != OOC_AUTHORIZED_CLEANUP_INTENT_CURRENT_PLATFORM_V1) {
        return failure(Code::platform_mismatch);
    }
    if (intent.marker_kind != OOCAuthorizedCleanupMarkerKindV2::intent &&
        intent.marker_kind != OOCAuthorizedCleanupMarkerKindV2::staged) {
        return failure(Code::invalid_value);
    }
    if (lease_id_is_zero(intent.lease_id) || !identities_are_valid_and_distinct(intent)) {
        return failure(Code::invalid_value);
    }
    if (intent.pair.format_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
        intent.pair.store_id == 0 || intent.pair.generation == 0) {
        return failure(Code::invalid_value);
    }

    constexpr std::uint64_t OFFSET_BYTES = sizeof(std::uint64_t);
    constexpr std::uint64_t FIXED_INDEX_BYTES =
        OOCRelationStoreFormat::INDEX_HEADER_BYTES + OOCRelationStoreFormat::INDEX_SENTINEL_BYTES;
    if (intent.pair.count >
        (std::numeric_limits<std::uint64_t>::max() - FIXED_INDEX_BYTES) / OFFSET_BYTES) {
        return failure(Code::integer_out_of_range);
    }
    const std::uint64_t expected_index_extent =
        FIXED_INDEX_BYTES + intent.pair.count * OFFSET_BYTES;
    if (expected_index_extent >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return failure(Code::integer_out_of_range);
    }
    if (intent.pair.index_extent != expected_index_extent ||
        intent.index.extent != expected_index_extent ||
        intent.data.extent != intent.pair.data_extent) {
        return failure(Code::invalid_value);
    }
    if (intent.pair.data_extent < OOCRelationStoreFormat::DATA_HEADER_BYTES ||
        intent.pair.data_extent >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return failure(Code::invalid_value);
    }
    if ((intent.pair.count == 0 &&
         intent.pair.data_extent != OOCRelationStoreFormat::DATA_HEADER_BYTES) ||
        (intent.pair.count != 0 &&
         intent.pair.data_extent == OOCRelationStoreFormat::DATA_HEADER_BYTES)) {
        return failure(Code::invalid_value);
    }
    if (intent.handoff.extent < OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 ||
        intent.handoff.extent > OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES ||
        intent.handoff.extent >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return failure(Code::invalid_value);
    }
    if (intent.pending_handoff.has_value() &&
        intent.pending_handoff->extent != intent.handoff.extent) {
        return failure(Code::invalid_value);
    }
    return {};
}

class Writer final {
public:
    void put_u8(std::uint8_t value) {
        if (!reserve(1)) {
            return;
        }
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void put_u32(std::uint32_t value) {
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u64(std::uint64_t value) {
        for (unsigned int shift = 0; shift < 64; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_bytes(std::span<const std::byte> bytes) {
        if (!reserve(bytes.size())) {
            return;
        }
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    void put_digest(const util::Sha256Digest& digest) {
        put_bytes(digest.bytes);
    }

    [[nodiscard]] Status status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::vector<std::byte>& bytes() noexcept {
        return bytes_;
    }

private:
    [[nodiscard]] bool reserve(std::size_t amount) noexcept {
        if (!status_) {
            return false;
        }
        if (amount > OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 ||
            bytes_.size() > OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 - amount) {
            status_ = failure(Code::output_too_large, bytes_.size());
            return false;
        }
        return true;
    }

    std::vector<std::byte> bytes_;
    Status status_;
};

class Reader final {
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool get_u8(std::uint8_t& value) noexcept {
        if (!take(1)) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(bytes_[offset_]);
        ++offset_;
        return true;
    }

    [[nodiscard]] bool get_u32(std::uint32_t& value) noexcept {
        value = 0;
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            std::uint8_t byte = 0;
            if (!get_u8(byte)) {
                return false;
            }
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }

    [[nodiscard]] bool get_u64(std::uint64_t& value) noexcept {
        value = 0;
        for (unsigned int shift = 0; shift < 64; shift += 8) {
            std::uint8_t byte = 0;
            if (!get_u8(byte)) {
                return false;
            }
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }

    [[nodiscard]] bool get_bytes(std::span<std::byte> output) noexcept {
        if (!take(output.size())) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), output.size(),
                    output.begin());
        offset_ += output.size();
        return true;
    }

    [[nodiscard]] bool get_digest(util::Sha256Digest& digest) noexcept {
        return get_bytes(digest.bytes);
    }

    [[nodiscard]] std::size_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] Status status() const noexcept {
        return status_;
    }

private:
    [[nodiscard]] bool take(std::size_t amount) noexcept {
        if (!status_) {
            return false;
        }
        if (offset_ > bytes_.size() || amount > bytes_.size() - offset_) {
            status_ = failure(Code::truncated, bytes_.size());
            return false;
        }
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
    Status status_;
};

void write_identity(Writer& writer, const NativeIdentity& identity) {
    writer.put_u64(identity.first);
    writer.put_u64(identity.second);
    writer.put_u64(identity.third);
}

[[nodiscard]] bool read_identity(Reader& reader, NativeIdentity& identity) noexcept {
    return reader.get_u64(identity.first) && reader.get_u64(identity.second) &&
           reader.get_u64(identity.third);
}

void write_pair(Writer& writer, const OOCPrivateHandoffPairDescriptorV1& pair) {
    writer.put_u64(pair.format_version);
    writer.put_u64(pair.store_id);
    writer.put_u64(pair.generation);
    writer.put_u64(pair.count);
    writer.put_u64(pair.index_extent);
    writer.put_u64(pair.data_extent);
}

[[nodiscard]] bool read_pair(Reader& reader, OOCPrivateHandoffPairDescriptorV1& pair) noexcept {
    return reader.get_u64(pair.format_version) && reader.get_u64(pair.store_id) &&
           reader.get_u64(pair.generation) && reader.get_u64(pair.count) &&
           reader.get_u64(pair.index_extent) && reader.get_u64(pair.data_extent);
}

void write_artifact(Writer& writer, const OOCPrivateHandoffArtifactBindingV1& artifact) {
    write_identity(writer, artifact.identity);
    writer.put_u64(artifact.extent);
}

[[nodiscard]] bool read_artifact(Reader& reader,
                                 OOCPrivateHandoffArtifactBindingV1& artifact) noexcept {
    return read_identity(reader, artifact.identity) && reader.get_u64(artifact.extent);
}

[[nodiscard]] Status write_record_preimage(const OOCAuthorizedCleanupIntentV2& intent,
                                           Writer& writer) {
    for (const char character : RECORD_MAGIC) {
        writer.put_u8(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    writer.put_u32(OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_VERSION_V1);
    writer.put_u32(intent.schema_version);
    writer.put_u64(intent.platform_id);
    writer.put_u32(static_cast<std::uint32_t>(OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2));
    writer.put_u32(static_cast<std::uint32_t>(intent.marker_kind));
    writer.put_digest(intent.base_path_digest);
    writer.put_digest(intent.external_authorization_digest);
    writer.put_digest(intent.generic_handoff_self_digest);
    writer.put_u64(intent.lease_id[0]);
    writer.put_u64(intent.lease_id[1]);
    write_identity(writer, intent.parent_directory_identity);
    write_identity(writer, intent.lock_identity);
    write_identity(writer, intent.directory_identity);
    write_identity(writer, intent.owner_marker_identity);
    write_identity(writer, intent.owned_marker_identity);
    write_pair(writer, intent.pair);
    write_artifact(writer, intent.handoff);
    writer.put_u32(intent.pending_handoff.has_value() ? 1U : 0U);
    writer.put_u32(0);
    write_artifact(writer, intent.pending_handoff.value_or(OOCPrivateHandoffArtifactBindingV1{}));
    write_artifact(writer, intent.index);
    write_artifact(writer, intent.data);
    return writer.status();
}

[[nodiscard]] std::optional<util::Sha256Digest>
hash_record_preimage(std::span<const std::byte> preimage) noexcept {
    util::Sha256Accumulator accumulator;
    if (!accumulator.update(RECORD_DIGEST_DOMAIN) || !accumulator.update(DIGEST_DOMAIN_SEPARATOR) ||
        !accumulator.update(preimage)) {
        return std::nullopt;
    }
    return accumulator.finalize();
}

[[nodiscard]] OOCAuthorizedCleanupIntentProtocolDigestResult
digest_structural(const OOCAuthorizedCleanupIntentV2& intent) {
    Writer writer;
    if (const auto status = write_record_preimage(intent, writer); !status) {
        return {std::nullopt, status};
    }
    auto digest = hash_record_preimage(writer.bytes());
    if (!digest.has_value()) {
        return {std::nullopt, failure(Code::digest_unavailable)};
    }
    return {digest, {}};
}

} // namespace ooc_authorized_cleanup_intent_detail

std::string_view ooc_authorized_cleanup_intent_protocol_code_name(
    OOCAuthorizedCleanupIntentProtocolCode code) noexcept {
    using Code = OOCAuthorizedCleanupIntentProtocolCode;
    switch (code) {
    case Code::none:
        return "none";
    case Code::input_too_large:
        return "input_too_large";
    case Code::output_too_large:
        return "output_too_large";
    case Code::truncated:
        return "truncated";
    case Code::trailing_bytes:
        return "trailing_bytes";
    case Code::invalid_magic:
        return "invalid_magic";
    case Code::unsupported_wire_version:
        return "unsupported_wire_version";
    case Code::unsupported_schema_version:
        return "unsupported_schema_version";
    case Code::platform_mismatch:
        return "platform_mismatch";
    case Code::declared_size_mismatch:
        return "declared_size_mismatch";
    case Code::invalid_value:
        return "invalid_value";
    case Code::unexpected_marker_kind:
        return "unexpected_marker_kind";
    case Code::integer_out_of_range:
        return "integer_out_of_range";
    case Code::digest_mismatch:
        return "digest_mismatch";
    case Code::digest_unavailable:
        return "digest_unavailable";
    case Code::resource_exhausted:
        return "resource_exhausted";
    }
    return "unknown";
}

OOCAuthorizedCleanupIntentProtocolStatus
validate_ooc_authorized_cleanup_intent(const OOCAuthorizedCleanupIntentV2& intent,
                                       bool verify_self_digest) noexcept {
    using namespace ooc_authorized_cleanup_intent_detail;
    if (const auto status = validate_structural_fields(intent); !status) {
        return status;
    }
    if (!verify_self_digest) {
        return {};
    }
    const auto digest = ooc_authorized_cleanup_intent_digest(intent);
    if (!digest) {
        return digest.status;
    }
    return *digest.digest == intent.self_digest ? Status{} : failure(Code::digest_mismatch);
}

OOCAuthorizedCleanupIntentProtocolDigestResult
ooc_authorized_cleanup_intent_digest(const OOCAuthorizedCleanupIntentV2& intent) noexcept {
    using namespace ooc_authorized_cleanup_intent_detail;
    try {
        if (const auto status = validate_structural_fields(intent); !status) {
            return {std::nullopt, status};
        }
        return digest_structural(intent);
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(Code::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(Code::resource_exhausted)};
    }
}

OOCAuthorizedCleanupIntentProtocolStatus
seal_ooc_authorized_cleanup_intent(OOCAuthorizedCleanupIntentV2& intent) noexcept {
    using namespace ooc_authorized_cleanup_intent_detail;
    try {
        const auto digest = ooc_authorized_cleanup_intent_digest(intent);
        if (!digest) {
            return digest.status;
        }
        intent.self_digest = *digest.digest;
        return {};
    } catch (const std::bad_alloc&) {
        return failure(Code::resource_exhausted);
    } catch (...) {
        return failure(Code::resource_exhausted);
    }
}

OOCAuthorizedCleanupIntentProtocolEncodeResult
encode_ooc_authorized_cleanup_intent(const OOCAuthorizedCleanupIntentV2& intent) noexcept {
    using namespace ooc_authorized_cleanup_intent_detail;
    try {
        if (const auto status = validate_ooc_authorized_cleanup_intent(intent, true); !status) {
            return {std::nullopt, status};
        }
        Writer writer;
        if (const auto status = write_record_preimage(intent, writer); !status) {
            return {std::nullopt, status};
        }
        writer.put_digest(intent.self_digest);
        if (!writer.status()) {
            return {std::nullopt, writer.status()};
        }
        if (writer.bytes().size() != OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2) {
            return {std::nullopt, failure(Code::output_too_large, writer.bytes().size())};
        }
        return {std::move(writer.bytes()), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(Code::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(Code::resource_exhausted)};
    }
}

OOCAuthorizedCleanupIntentProtocolDecodeResult
decode_ooc_authorized_cleanup_intent(std::span<const std::byte> bytes,
                                     OOCAuthorizedCleanupMarkerKindV2 expected_kind) noexcept {
    using namespace ooc_authorized_cleanup_intent_detail;
    try {
        if (expected_kind != OOCAuthorizedCleanupMarkerKindV2::intent &&
            expected_kind != OOCAuthorizedCleanupMarkerKindV2::staged) {
            return {std::nullopt, failure(Code::invalid_value, MARKER_KIND_OFFSET)};
        }
        if (bytes.size() > MAX_INPUT_BYTES) {
            return {std::nullopt, failure(Code::input_too_large, bytes.size())};
        }
        if (bytes.size() < OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2) {
            return {std::nullopt, failure(Code::truncated, bytes.size())};
        }
        if (bytes.size() > OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2) {
            return {std::nullopt,
                    failure(Code::trailing_bytes, OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2)};
        }

        Reader reader(bytes);
        std::array<std::byte, RECORD_MAGIC.size()> magic{};
        std::uint32_t wire_version = 0;
        std::uint32_t declared_size = 0;
        std::uint32_t marker_kind = 0;
        OOCAuthorizedCleanupIntentV2 intent;
        if (!reader.get_bytes(magic) || !reader.get_u32(wire_version) ||
            !reader.get_u32(intent.schema_version) || !reader.get_u64(intent.platform_id) ||
            !reader.get_u32(declared_size) || !reader.get_u32(marker_kind)) {
            return {std::nullopt, reader.status()};
        }
        for (std::size_t index = 0; index < RECORD_MAGIC.size(); ++index) {
            const auto expected = static_cast<std::byte>(
                static_cast<std::uint8_t>(static_cast<unsigned char>(RECORD_MAGIC[index])));
            if (magic[index] != expected) {
                return {std::nullopt, failure(Code::invalid_magic, MAGIC_OFFSET + index)};
            }
        }
        if (wire_version != OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_VERSION_V1) {
            return {std::nullopt, failure(Code::unsupported_wire_version, WIRE_VERSION_OFFSET)};
        }
        if (intent.schema_version != OOC_AUTHORIZED_CLEANUP_INTENT_SCHEMA_VERSION_V2) {
            return {std::nullopt, failure(Code::unsupported_schema_version, SCHEMA_VERSION_OFFSET)};
        }
        if (intent.platform_id != OOC_AUTHORIZED_CLEANUP_INTENT_CURRENT_PLATFORM_V1) {
            return {std::nullopt, failure(Code::platform_mismatch, PLATFORM_ID_OFFSET)};
        }
        if (declared_size != OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2) {
            return {std::nullopt, failure(Code::declared_size_mismatch, DECLARED_SIZE_OFFSET)};
        }
        if (marker_kind != static_cast<std::uint32_t>(OOCAuthorizedCleanupMarkerKindV2::intent) &&
            marker_kind != static_cast<std::uint32_t>(OOCAuthorizedCleanupMarkerKindV2::staged)) {
            return {std::nullopt, failure(Code::invalid_value, MARKER_KIND_OFFSET)};
        }
        intent.marker_kind = static_cast<OOCAuthorizedCleanupMarkerKindV2>(marker_kind);
        if (intent.marker_kind != expected_kind) {
            return {std::nullopt, failure(Code::unexpected_marker_kind, MARKER_KIND_OFFSET)};
        }

        if (!reader.get_digest(intent.base_path_digest) ||
            !reader.get_digest(intent.external_authorization_digest) ||
            !reader.get_digest(intent.generic_handoff_self_digest) ||
            !reader.get_u64(intent.lease_id[0]) || !reader.get_u64(intent.lease_id[1]) ||
            !read_identity(reader, intent.parent_directory_identity) ||
            !read_identity(reader, intent.lock_identity) ||
            !read_identity(reader, intent.directory_identity) ||
            !read_identity(reader, intent.owner_marker_identity) ||
            !read_identity(reader, intent.owned_marker_identity) ||
            !read_pair(reader, intent.pair) || !read_artifact(reader, intent.handoff)) {
            return {std::nullopt, reader.status()};
        }
        std::uint32_t pending_presence = 0;
        std::uint32_t pending_reserved = 0;
        OOCPrivateHandoffArtifactBindingV1 pending_handoff;
        if (!reader.get_u32(pending_presence) || !reader.get_u32(pending_reserved) ||
            !read_artifact(reader, pending_handoff) || !read_artifact(reader, intent.index) ||
            !read_artifact(reader, intent.data) || !reader.get_digest(intent.self_digest)) {
            return {std::nullopt, reader.status()};
        }
        if (pending_presence > 1U) {
            return {std::nullopt, failure(Code::invalid_value, PENDING_PRESENCE_OFFSET)};
        }
        if (pending_reserved != 0) {
            return {std::nullopt, failure(Code::invalid_value, PENDING_RESERVED_OFFSET)};
        }
        if (pending_presence == 0U) {
            if (!identity_is_zero(pending_handoff.identity) || pending_handoff.extent != 0) {
                return {std::nullopt, failure(Code::invalid_value, PENDING_ARTIFACT_OFFSET)};
            }
        } else {
            intent.pending_handoff = pending_handoff;
        }
        if (reader.offset() != bytes.size()) {
            return {std::nullopt, failure(Code::trailing_bytes, reader.offset())};
        }
        if (const auto status = validate_structural_fields(intent); !status) {
            return {std::nullopt, status};
        }
        const auto digest = digest_structural(intent);
        if (!digest) {
            return {std::nullopt, digest.status};
        }
        if (*digest.digest != intent.self_digest) {
            return {std::nullopt, failure(Code::digest_mismatch, SELF_DIGEST_OFFSET)};
        }
        return {std::move(intent), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(Code::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(Code::resource_exhausted)};
    }
}

} // namespace gnfs::relation
