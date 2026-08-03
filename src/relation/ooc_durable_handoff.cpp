#include <gnfs/relation/ooc_durable_handoff.hpp>

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
namespace ooc_private_handoff_protocol_detail {

using NativeIdentity = util::durable_immutable_record::NativeIdentity;

constexpr std::array<char, 8> RECORD_MAGIC{'G', 'N', 'F', 'S', 'O', 'C', 'H', '1'};
constexpr std::string_view RECORD_DIGEST_DOMAIN = "GNFS-OOC-PRIVATE-HANDOFF-RECORD-V1";
constexpr std::array<std::byte, 1> DIGEST_DOMAIN_SEPARATOR{std::byte{0}};

constexpr std::size_t MAGIC_OFFSET = 0;
constexpr std::size_t WIRE_VERSION_OFFSET = 8;
constexpr std::size_t SCHEMA_VERSION_OFFSET = 12;
constexpr std::size_t PLATFORM_ID_OFFSET = 16;
constexpr std::size_t DECLARED_SIZE_OFFSET = 24;
constexpr std::size_t PAYLOAD_SIZE_OFFSET = 28;
constexpr std::size_t PAYLOAD_DIGEST_OFFSET = 264;
constexpr std::size_t PAYLOAD_OFFSET = 296;
constexpr std::size_t SELF_DIGEST_BYTES = util::SHA256_DIGEST_BYTES;

static_assert(PAYLOAD_OFFSET + SELF_DIGEST_BYTES == OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1);
static_assert(OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES <=
              static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));

[[nodiscard]] constexpr OOCPrivateHandoffProtocolStatus
failure(OOCPrivateHandoffProtocolCode code,
        std::uint64_t byte_offset = OOC_PRIVATE_HANDOFF_NO_OFFSET) noexcept {
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
identities_are_valid_and_distinct(const OOCPrivateHandoffRecordV1& record) noexcept {
    const std::array<NativeIdentity, 6> identities{
        record.lock_identity,         record.directory_identity, record.owner_marker_identity,
        record.owned_marker_identity, record.index.identity,     record.data.identity,
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
    return true;
}

[[nodiscard]] constexpr OOCPrivateHandoffProtocolStatus
validate_structural_fields(const OOCPrivateHandoffRecordV1& record) noexcept {
    if (record.schema_version != OOC_PRIVATE_HANDOFF_SCHEMA_VERSION_V1) {
        return failure(OOCPrivateHandoffProtocolCode::unsupported_schema_version);
    }
    if (record.platform_id != OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1) {
        return failure(OOCPrivateHandoffProtocolCode::platform_mismatch);
    }
    if (lease_id_is_zero(record.lease_id) || !identities_are_valid_and_distinct(record)) {
        return failure(OOCPrivateHandoffProtocolCode::invalid_value);
    }
    if (record.pair.format_version != OOCRelationStoreFormat::FORMAT_VERSION_V3 ||
        record.pair.store_id == 0 || record.pair.generation == 0) {
        return failure(OOCPrivateHandoffProtocolCode::invalid_value);
    }

    constexpr std::uint64_t OFFSET_BYTES = sizeof(std::uint64_t);
    constexpr std::uint64_t FIXED_INDEX_BYTES =
        OOCRelationStoreFormat::INDEX_HEADER_BYTES + OOCRelationStoreFormat::INDEX_SENTINEL_BYTES;
    if (record.pair.count >
        (std::numeric_limits<std::uint64_t>::max() - FIXED_INDEX_BYTES) / OFFSET_BYTES) {
        return failure(OOCPrivateHandoffProtocolCode::integer_out_of_range);
    }
    const std::uint64_t expected_index_extent =
        FIXED_INDEX_BYTES + record.pair.count * OFFSET_BYTES;
    if (expected_index_extent >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return failure(OOCPrivateHandoffProtocolCode::integer_out_of_range);
    }
    if (record.pair.index_extent != expected_index_extent ||
        record.index.extent != expected_index_extent ||
        record.data.extent != record.pair.data_extent) {
        return failure(OOCPrivateHandoffProtocolCode::invalid_value);
    }
    if (record.pair.data_extent < OOCRelationStoreFormat::DATA_HEADER_BYTES ||
        record.pair.data_extent >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return failure(OOCPrivateHandoffProtocolCode::invalid_value);
    }
    if ((record.pair.count == 0 &&
         record.pair.data_extent != OOCRelationStoreFormat::DATA_HEADER_BYTES) ||
        (record.pair.count != 0 &&
         record.pair.data_extent == OOCRelationStoreFormat::DATA_HEADER_BYTES)) {
        return failure(OOCPrivateHandoffProtocolCode::invalid_value);
    }
    if (record.payload_kind == 0 || record.payload_version == 0) {
        return failure(OOCPrivateHandoffProtocolCode::invalid_value);
    }
    if (record.opaque_payload.size() > OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES) {
        return failure(OOCPrivateHandoffProtocolCode::payload_too_large);
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

    [[nodiscard]] OOCPrivateHandoffProtocolStatus status() const noexcept {
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
        if (amount > OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES ||
            bytes_.size() > OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES - amount) {
            status_ = failure(OOCPrivateHandoffProtocolCode::output_too_large, bytes_.size());
            return false;
        }
        return true;
    }

    std::vector<std::byte> bytes_;
    OOCPrivateHandoffProtocolStatus status_;
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

    [[nodiscard]] OOCPrivateHandoffProtocolStatus status() const noexcept {
        return status_;
    }

private:
    [[nodiscard]] bool take(std::size_t amount) noexcept {
        if (!status_) {
            return false;
        }
        if (amount > bytes_.size() - offset_) {
            status_ = failure(OOCPrivateHandoffProtocolCode::truncated, bytes_.size());
            return false;
        }
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
    OOCPrivateHandoffProtocolStatus status_;
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

[[nodiscard]] OOCPrivateHandoffProtocolStatus
write_record_preimage(const OOCPrivateHandoffRecordV1& record,
                      const util::Sha256Digest& payload_digest, Writer& writer) {
    for (const char character : RECORD_MAGIC) {
        writer.put_u8(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    writer.put_u32(OOC_PRIVATE_HANDOFF_WIRE_VERSION_V1);
    writer.put_u32(record.schema_version);
    writer.put_u64(record.platform_id);
    const std::size_t total_size =
        OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 + record.opaque_payload.size();
    if (total_size > std::numeric_limits<std::uint32_t>::max() ||
        record.opaque_payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return failure(OOCPrivateHandoffProtocolCode::integer_out_of_range);
    }
    writer.put_u32(static_cast<std::uint32_t>(total_size));
    writer.put_u32(static_cast<std::uint32_t>(record.opaque_payload.size()));
    writer.put_u64(record.lease_id[0]);
    writer.put_u64(record.lease_id[1]);
    write_identity(writer, record.lock_identity);
    write_identity(writer, record.directory_identity);
    write_identity(writer, record.owner_marker_identity);
    write_identity(writer, record.owned_marker_identity);
    write_pair(writer, record.pair);
    write_artifact(writer, record.index);
    write_artifact(writer, record.data);
    writer.put_u32(record.payload_kind);
    writer.put_u32(record.payload_version);
    writer.put_digest(payload_digest);
    writer.put_bytes(record.opaque_payload);
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

[[nodiscard]] OOCPrivateHandoffProtocolDigestResult
digest_with_payload(const OOCPrivateHandoffRecordV1& record,
                    const util::Sha256Digest& payload_digest) {
    Writer writer;
    if (const auto status = write_record_preimage(record, payload_digest, writer); !status) {
        return {std::nullopt, status};
    }
    auto digest = hash_record_preimage(writer.bytes());
    if (!digest.has_value()) {
        return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::digest_unavailable)};
    }
    return {digest, {}};
}

[[nodiscard]] OOCPrivateHandoffProtocolStatus
validate_payload_digest(const OOCPrivateHandoffRecordV1& record) noexcept {
    const auto digest = util::sha256(std::span<const std::byte>(record.opaque_payload));
    if (!digest.has_value()) {
        return failure(OOCPrivateHandoffProtocolCode::digest_unavailable);
    }
    return *digest == record.payload_digest
               ? OOCPrivateHandoffProtocolStatus{}
               : failure(OOCPrivateHandoffProtocolCode::digest_mismatch);
}

} // namespace ooc_private_handoff_protocol_detail

std::string_view
ooc_private_handoff_protocol_code_name(OOCPrivateHandoffProtocolCode code) noexcept {
    switch (code) {
    case OOCPrivateHandoffProtocolCode::none:
        return "none";
    case OOCPrivateHandoffProtocolCode::input_too_large:
        return "input_too_large";
    case OOCPrivateHandoffProtocolCode::output_too_large:
        return "output_too_large";
    case OOCPrivateHandoffProtocolCode::truncated:
        return "truncated";
    case OOCPrivateHandoffProtocolCode::trailing_bytes:
        return "trailing_bytes";
    case OOCPrivateHandoffProtocolCode::invalid_magic:
        return "invalid_magic";
    case OOCPrivateHandoffProtocolCode::unsupported_wire_version:
        return "unsupported_wire_version";
    case OOCPrivateHandoffProtocolCode::unsupported_schema_version:
        return "unsupported_schema_version";
    case OOCPrivateHandoffProtocolCode::platform_mismatch:
        return "platform_mismatch";
    case OOCPrivateHandoffProtocolCode::declared_size_mismatch:
        return "declared_size_mismatch";
    case OOCPrivateHandoffProtocolCode::invalid_value:
        return "invalid_value";
    case OOCPrivateHandoffProtocolCode::payload_too_large:
        return "payload_too_large";
    case OOCPrivateHandoffProtocolCode::integer_out_of_range:
        return "integer_out_of_range";
    case OOCPrivateHandoffProtocolCode::digest_mismatch:
        return "digest_mismatch";
    case OOCPrivateHandoffProtocolCode::digest_unavailable:
        return "digest_unavailable";
    case OOCPrivateHandoffProtocolCode::resource_exhausted:
        return "resource_exhausted";
    }
    return "unknown";
}

OOCPrivateHandoffProtocolStatus
validate_ooc_private_handoff_record(const OOCPrivateHandoffRecordV1& record,
                                    bool verify_self_digest) noexcept {
    using namespace ooc_private_handoff_protocol_detail;
    if (const auto status = validate_structural_fields(record); !status) {
        return status;
    }
    if (const auto status = validate_payload_digest(record); !status) {
        return status;
    }
    if (!verify_self_digest) {
        return {};
    }
    const auto digest = ooc_private_handoff_record_digest(record);
    if (!digest) {
        return digest.status;
    }
    return *digest.digest == record.self_digest
               ? OOCPrivateHandoffProtocolStatus{}
               : failure(OOCPrivateHandoffProtocolCode::digest_mismatch);
}

OOCPrivateHandoffProtocolDigestResult
ooc_private_handoff_record_digest(const OOCPrivateHandoffRecordV1& record) noexcept {
    using namespace ooc_private_handoff_protocol_detail;
    try {
        if (const auto status = validate_structural_fields(record); !status) {
            return {std::nullopt, status};
        }
        if (const auto status = validate_payload_digest(record); !status) {
            return {std::nullopt, status};
        }
        return digest_with_payload(record, record.payload_digest);
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::resource_exhausted)};
    }
}

OOCPrivateHandoffProtocolStatus
seal_ooc_private_handoff_record(OOCPrivateHandoffRecordV1& record) noexcept {
    using namespace ooc_private_handoff_protocol_detail;
    try {
        if (const auto status = validate_structural_fields(record); !status) {
            return status;
        }
        const auto payload_digest = util::sha256(std::span<const std::byte>(record.opaque_payload));
        if (!payload_digest.has_value()) {
            return failure(OOCPrivateHandoffProtocolCode::digest_unavailable);
        }
        const auto self_digest = digest_with_payload(record, *payload_digest);
        if (!self_digest) {
            return self_digest.status;
        }
        record.payload_digest = *payload_digest;
        record.self_digest = *self_digest.digest;
        return {};
    } catch (const std::bad_alloc&) {
        return failure(OOCPrivateHandoffProtocolCode::resource_exhausted);
    } catch (...) {
        return failure(OOCPrivateHandoffProtocolCode::resource_exhausted);
    }
}

OOCPrivateHandoffProtocolEncodeResult
encode_ooc_private_handoff_record(const OOCPrivateHandoffRecordV1& record) noexcept {
    using namespace ooc_private_handoff_protocol_detail;
    try {
        if (const auto status = validate_ooc_private_handoff_record(record, true); !status) {
            return {std::nullopt, status};
        }
        Writer writer;
        if (const auto status = write_record_preimage(record, record.payload_digest, writer);
            !status) {
            return {std::nullopt, status};
        }
        writer.put_digest(record.self_digest);
        if (!writer.status()) {
            return {std::nullopt, writer.status()};
        }
        return {std::move(writer.bytes()), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::resource_exhausted)};
    }
}

OOCPrivateHandoffProtocolDecodeResult
decode_ooc_private_handoff_record(std::span<const std::byte> bytes) noexcept {
    using namespace ooc_private_handoff_protocol_detail;
    try {
        if (bytes.size() > OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES) {
            return {std::nullopt,
                    failure(OOCPrivateHandoffProtocolCode::input_too_large, bytes.size())};
        }
        if (bytes.size() < OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1) {
            return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::truncated, bytes.size())};
        }

        Reader reader(bytes);
        std::array<std::byte, RECORD_MAGIC.size()> magic{};
        std::uint32_t wire_version = 0;
        OOCPrivateHandoffRecordV1 record;
        std::uint32_t declared_size = 0;
        std::uint32_t payload_size = 0;
        if (!reader.get_bytes(magic) || !reader.get_u32(wire_version) ||
            !reader.get_u32(record.schema_version) || !reader.get_u64(record.platform_id) ||
            !reader.get_u32(declared_size) || !reader.get_u32(payload_size)) {
            return {std::nullopt, reader.status()};
        }
        for (std::size_t index = 0; index < RECORD_MAGIC.size(); ++index) {
            const auto expected = static_cast<std::byte>(
                static_cast<std::uint8_t>(static_cast<unsigned char>(RECORD_MAGIC[index])));
            if (magic[index] != expected) {
                return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::invalid_magic,
                                              MAGIC_OFFSET + index)};
            }
        }
        if (wire_version != OOC_PRIVATE_HANDOFF_WIRE_VERSION_V1) {
            return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::unsupported_wire_version,
                                          WIRE_VERSION_OFFSET)};
        }
        if (record.schema_version != OOC_PRIVATE_HANDOFF_SCHEMA_VERSION_V1) {
            return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::unsupported_schema_version,
                                          SCHEMA_VERSION_OFFSET)};
        }
        if (record.platform_id != OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1) {
            return {std::nullopt,
                    failure(OOCPrivateHandoffProtocolCode::platform_mismatch, PLATFORM_ID_OFFSET)};
        }
        if (payload_size > OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES) {
            return {std::nullopt,
                    failure(OOCPrivateHandoffProtocolCode::payload_too_large, PAYLOAD_SIZE_OFFSET)};
        }
        const std::size_t expected_size = OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 + payload_size;
        if (declared_size != expected_size ||
            declared_size > OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES) {
            return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::declared_size_mismatch,
                                          DECLARED_SIZE_OFFSET)};
        }
        if (bytes.size() < declared_size) {
            return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::truncated, bytes.size())};
        }
        if (bytes.size() > declared_size) {
            return {std::nullopt,
                    failure(OOCPrivateHandoffProtocolCode::trailing_bytes, declared_size)};
        }

        if (!reader.get_u64(record.lease_id[0]) || !reader.get_u64(record.lease_id[1]) ||
            !read_identity(reader, record.lock_identity) ||
            !read_identity(reader, record.directory_identity) ||
            !read_identity(reader, record.owner_marker_identity) ||
            !read_identity(reader, record.owned_marker_identity) ||
            !read_pair(reader, record.pair) || !read_artifact(reader, record.index) ||
            !read_artifact(reader, record.data) || !reader.get_u32(record.payload_kind) ||
            !reader.get_u32(record.payload_version) || !reader.get_digest(record.payload_digest)) {
            return {std::nullopt, reader.status()};
        }
        record.opaque_payload.resize(payload_size);
        if (!reader.get_bytes(record.opaque_payload) || !reader.get_digest(record.self_digest)) {
            return {std::nullopt, reader.status()};
        }
        if (reader.offset() != bytes.size()) {
            return {std::nullopt,
                    failure(OOCPrivateHandoffProtocolCode::trailing_bytes, reader.offset())};
        }

        if (const auto status = validate_structural_fields(record); !status) {
            return {std::nullopt, status};
        }
        if (const auto status = validate_payload_digest(record); !status) {
            return {std::nullopt, failure(status.code, PAYLOAD_DIGEST_OFFSET)};
        }
        const auto digest = digest_with_payload(record, record.payload_digest);
        if (!digest) {
            return {std::nullopt, digest.status};
        }
        if (*digest.digest != record.self_digest) {
            return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::digest_mismatch,
                                          declared_size - SELF_DIGEST_BYTES)};
        }
        return {std::move(record), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(OOCPrivateHandoffProtocolCode::resource_exhausted)};
    }
}

} // namespace gnfs::relation
