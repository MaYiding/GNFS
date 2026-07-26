#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>
#include <gnfs/relation/ooc_durable_handoff.hpp>
#include <gnfs/relation/ooc_relation_format.hpp>
#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace relation = gnfs::relation;
using AuthorizedCode = relation::OOCAuthorizedCleanupIntentProtocolCode;
using AuthorizedRecord = relation::OOCAuthorizedCleanupIntentV2;
using AuthorizedStatus = relation::OOCAuthorizedCleanupIntentProtocolStatus;
using Code = relation::OOCPrivateHandoffProtocolCode;
using Digest = gnfs::util::Sha256Digest;
using Record = relation::OOCPrivateHandoffRecordV1;
using Status = relation::OOCPrivateHandoffProtocolStatus;

static_assert(relation::OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES == 64U * 1024U);
static_assert(relation::OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 == 328);
static_assert(relation::OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES ==
              relation::OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 +
                  relation::OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES);
static_assert(
    noexcept(relation::validate_ooc_private_handoff_record(std::declval<const Record&>())));
static_assert(noexcept(relation::ooc_private_handoff_record_digest(std::declval<const Record&>())));
static_assert(noexcept(relation::seal_ooc_private_handoff_record(std::declval<Record&>())));
static_assert(noexcept(relation::encode_ooc_private_handoff_record(std::declval<const Record&>())));
static_assert(noexcept(
    relation::decode_ooc_private_handoff_record(std::declval<std::span<const std::byte>>())));
static_assert(relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 == 480);
static_assert(noexcept(
    relation::validate_ooc_authorized_cleanup_intent(std::declval<const AuthorizedRecord&>())));
static_assert(noexcept(
    relation::ooc_authorized_cleanup_intent_digest(std::declval<const AuthorizedRecord&>())));
static_assert(
    noexcept(relation::seal_ooc_authorized_cleanup_intent(std::declval<AuthorizedRecord&>())));
static_assert(noexcept(
    relation::encode_ooc_authorized_cleanup_intent(std::declval<const AuthorizedRecord&>())));
static_assert(noexcept(relation::decode_ooc_authorized_cleanup_intent(
    std::declval<std::span<const std::byte>>(),
    std::declval<relation::OOCAuthorizedCleanupMarkerKindV2>())));

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view expression, int line, std::string_view detail = {}) {
    std::string message = "CHECK failed at line " + std::to_string(line) + ": ";
    message.append(expression);
    if (!detail.empty()) {
        message.append(" (");
        message.append(detail);
        message.push_back(')');
    }
    throw TestFailure(message);
}

void check(bool condition, std::string_view expression, int line) {
    if (!condition) {
        fail(expression, line);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void require_ok(const Status& status, std::string_view context) {
    if (!status) {
        fail(context, __LINE__, relation::ooc_private_handoff_protocol_code_name(status.code));
    }
}

void require_code(const Status& status, Code expected, std::string_view context) {
    if (status || status.code != expected) {
        fail(context, __LINE__, relation::ooc_private_handoff_protocol_code_name(status.code));
    }
}

[[nodiscard]] gnfs::util::durable_immutable_record::NativeIdentity
native_identity(std::uint64_t seed) {
    return {
        .first = seed,
        .second = seed + 1,
        .third = seed + 2,
    };
}

[[nodiscard]] std::vector<std::byte> payload_with_size(std::size_t size) {
    std::vector<std::byte> payload(size);
    for (std::size_t index = 0; index < size; ++index) {
        payload[index] =
            static_cast<std::byte>(static_cast<std::uint8_t>((index * 131U + 17U) & 0xffU));
    }
    return payload;
}

[[nodiscard]] constexpr std::uint64_t index_extent_for_count(std::uint64_t count) {
    return relation::OOCRelationStoreFormat::INDEX_HEADER_BYTES +
           relation::OOCRelationStoreFormat::INDEX_SENTINEL_BYTES + count * sizeof(std::uint64_t);
}

[[nodiscard]] Record make_unsealed_record(std::uint64_t count = 3,
                                          std::vector<std::byte> payload = payload_with_size(7)) {
    Record record;
    record.schema_version = relation::OOC_PRIVATE_HANDOFF_SCHEMA_VERSION_V1;
    record.platform_id = relation::OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1;
    record.lease_id = {
        UINT64_C(0x0807060504030201),
        UINT64_C(0x1817161514131211),
    };
    record.lock_identity = native_identity(UINT64_C(0x2122232425262728));
    record.directory_identity = native_identity(UINT64_C(0x3132333435363738));
    record.owner_marker_identity = native_identity(UINT64_C(0x4142434445464748));
    record.owned_marker_identity = native_identity(UINT64_C(0x5152535455565758));
    record.pair.format_version = relation::OOCRelationStoreFormat::FORMAT_VERSION_V3;
    record.pair.store_id = UINT64_C(0x6162636465666768);
    record.pair.generation = UINT64_C(0x7172737475767778);
    record.pair.count = count;
    record.pair.index_extent = index_extent_for_count(count);
    record.pair.data_extent =
        count == 0 ? relation::OOCRelationStoreFormat::DATA_HEADER_BYTES : UINT64_C(0x280);
    record.index.identity = native_identity(UINT64_C(0x8182838485868788));
    record.index.extent = record.pair.index_extent;
    record.data.identity = native_identity(UINT64_C(0x9192939495969798));
    record.data.extent = record.pair.data_extent;
    record.payload_kind = UINT32_C(0xa1a2a3a4);
    record.payload_version = UINT32_C(0xb1b2b3b4);
    record.opaque_payload = std::move(payload);
    return record;
}

[[nodiscard]] Record make_sealed_record(std::uint64_t count = 3,
                                        std::vector<std::byte> payload = payload_with_size(7)) {
    Record record = make_unsealed_record(count, std::move(payload));
    require_ok(relation::seal_ooc_private_handoff_record(record), "seal fixture");
    return record;
}

[[nodiscard]] std::vector<std::byte> encode_or_fail(const Record& record) {
    const auto encoded = relation::encode_ooc_private_handoff_record(record);
    if (!encoded) {
        fail("encode_ooc_private_handoff_record", __LINE__,
             relation::ooc_private_handoff_protocol_code_name(encoded.status.code));
    }
    CHECK(encoded.bytes.has_value());
    CHECK(encoded.status.code == Code::none);
    CHECK(encoded.status.byte_offset == relation::OOC_PRIVATE_HANDOFF_NO_OFFSET);
    return *encoded.bytes;
}

[[nodiscard]] Digest digest_or_fail(const Record& record) {
    const auto digest = relation::ooc_private_handoff_record_digest(record);
    if (!digest) {
        fail("ooc_private_handoff_record_digest", __LINE__,
             relation::ooc_private_handoff_protocol_code_name(digest.status.code));
    }
    CHECK(digest.digest.has_value());
    CHECK(digest.status.code == Code::none);
    CHECK(digest.status.byte_offset == relation::OOC_PRIVATE_HANDOFF_NO_OFFSET);
    return *digest.digest;
}

[[nodiscard]] std::uint32_t load_u32_le(std::span<const std::byte> bytes, std::size_t offset) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= sizeof(std::uint32_t));
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t load_u64_le(std::span<const std::byte> bytes, std::size_t offset) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= sizeof(std::uint64_t));
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

void check_identity_le(std::span<const std::byte> bytes, std::size_t offset,
                       const gnfs::util::durable_immutable_record::NativeIdentity& identity) {
    CHECK(load_u64_le(bytes, offset) == identity.first);
    CHECK(load_u64_le(bytes, offset + sizeof(std::uint64_t)) == identity.second);
    CHECK(load_u64_le(bytes, offset + 2U * sizeof(std::uint64_t)) == identity.third);
}

void store_u32_le(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= sizeof(value));
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void store_u64_le(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= sizeof(value));
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void perturb_digest(Digest& digest) {
    digest.bytes.front() ^= std::byte{0x80};
}

[[nodiscard]] Digest digest_from_hex(std::string_view encoded) {
    const auto digest = gnfs::util::decode_sha256_hex(encoded);
    if (!digest.has_value()) {
        fail("decode_sha256_hex", __LINE__, encoded);
    }
    return *digest;
}

void test_protocol_constants_and_closed_names() {
    CHECK(relation::OOC_PRIVATE_HANDOFF_SCHEMA_VERSION_V1 == 1);
    CHECK(relation::OOC_PRIVATE_HANDOFF_WIRE_VERSION_V1 == 1);
    CHECK(relation::OOC_PRIVATE_HANDOFF_PLATFORM_POSIX_V1 == 1);
    CHECK(relation::OOC_PRIVATE_HANDOFF_PLATFORM_WINDOWS_V1 == 2);
    CHECK(relation::OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1 ==
              relation::OOC_PRIVATE_HANDOFF_PLATFORM_POSIX_V1 ||
          relation::OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1 ==
              relation::OOC_PRIVATE_HANDOFF_PLATFORM_WINDOWS_V1);

    constexpr std::array CODES{
        Code::none,
        Code::input_too_large,
        Code::output_too_large,
        Code::truncated,
        Code::trailing_bytes,
        Code::invalid_magic,
        Code::unsupported_wire_version,
        Code::unsupported_schema_version,
        Code::platform_mismatch,
        Code::declared_size_mismatch,
        Code::invalid_value,
        Code::payload_too_large,
        Code::integer_out_of_range,
        Code::digest_mismatch,
        Code::digest_unavailable,
        Code::resource_exhausted,
    };
    static_assert(static_cast<std::uint8_t>(Code::resource_exhausted) + 1U == CODES.size());
    for (std::size_t index = 0; index < CODES.size(); ++index) {
        CHECK(static_cast<std::uint8_t>(CODES[index]) == index);
        CHECK(!relation::ooc_private_handoff_protocol_code_name(CODES[index]).empty());
    }
    CHECK(!relation::ooc_private_handoff_protocol_code_name(static_cast<Code>(0xff)).empty());
}

void test_sealed_round_trip_and_determinism() {
    constexpr std::string_view EXPECTED_PAYLOAD_DIGEST =
        "3ee1a2306d13d6997cfc2dd52277bf6e774e81900ca5be136a1e046486840aa5";
    [[maybe_unused]] constexpr std::string_view EXPECTED_POSIX_SELF_DIGEST =
        "1051bc7cc95441a0a9f68fdbcd409f455d0d5afcc8976ed8ed088e812be5c9c9";
    [[maybe_unused]] constexpr std::string_view EXPECTED_WINDOWS_SELF_DIGEST =
        "05bc235014aee057124ebaaf78879e20193c455dbdae41aeb8482d6802c806b4";
#ifdef _WIN32
    constexpr std::string_view EXPECTED_SELF_DIGEST = EXPECTED_WINDOWS_SELF_DIGEST;
#else
    constexpr std::string_view EXPECTED_SELF_DIGEST = EXPECTED_POSIX_SELF_DIGEST;
#endif
    constexpr std::array EXPECTED_MAGIC{
        std::byte{'G'}, std::byte{'N'}, std::byte{'F'}, std::byte{'S'},
        std::byte{'O'}, std::byte{'C'}, std::byte{'H'}, std::byte{'1'},
    };
    const Record first = make_sealed_record();
    const Record second = make_sealed_record();
    CHECK(first == second);
    require_ok(relation::validate_ooc_private_handoff_record(first), "validate sealed fixture");

    const auto payload_digest = gnfs::util::sha256(first.opaque_payload);
    CHECK(payload_digest.has_value());
    CHECK(first.payload_digest == *payload_digest);
    CHECK(first.payload_digest == digest_from_hex(EXPECTED_PAYLOAD_DIGEST));
    CHECK(first.self_digest == digest_or_fail(first));
    CHECK(first.self_digest == digest_from_hex(EXPECTED_SELF_DIGEST));

    const auto first_bytes = encode_or_fail(first);
    const auto second_bytes = encode_or_fail(second);
    CHECK(first_bytes == second_bytes);
    CHECK(first_bytes.size() ==
          relation::OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 + first.opaque_payload.size());
    CHECK(std::equal(EXPECTED_MAGIC.begin(), EXPECTED_MAGIC.end(), first_bytes.begin()));

    const auto decoded = relation::decode_ooc_private_handoff_record(first_bytes);
    CHECK(decoded);
    CHECK(decoded.value.has_value());
    CHECK(decoded.status.code == Code::none);
    CHECK(decoded.status.byte_offset == relation::OOC_PRIVATE_HANDOFF_NO_OFFSET);
    CHECK(*decoded.value == first);
    CHECK(encode_or_fail(*decoded.value) == first_bytes);
}

void test_little_endian_wire_fields() {
    const Record record = make_sealed_record();
    const auto bytes = encode_or_fail(record);

    constexpr std::size_t WIRE_VERSION_OFFSET = 8;
    constexpr std::size_t SCHEMA_VERSION_OFFSET = 12;
    constexpr std::size_t PLATFORM_OFFSET = 16;
    constexpr std::size_t DECLARED_SIZE_OFFSET = 24;
    constexpr std::size_t PAYLOAD_SIZE_OFFSET = 28;
    constexpr std::size_t LEASE_LOW_OFFSET = 32;
    constexpr std::size_t LEASE_HIGH_OFFSET = 40;
    constexpr std::size_t LOCK_IDENTITY_OFFSET = 48;
    constexpr std::size_t DIRECTORY_IDENTITY_OFFSET = 72;
    constexpr std::size_t OWNER_IDENTITY_OFFSET = 96;
    constexpr std::size_t OWNED_IDENTITY_OFFSET = 120;
    constexpr std::size_t PAIR_FORMAT_OFFSET = 144;
    constexpr std::size_t PAIR_STORE_OFFSET = 152;
    constexpr std::size_t PAIR_GENERATION_OFFSET = 160;
    constexpr std::size_t PAIR_COUNT_OFFSET = 168;
    constexpr std::size_t PAIR_INDEX_EXTENT_OFFSET = 176;
    constexpr std::size_t PAIR_DATA_EXTENT_OFFSET = 184;
    constexpr std::size_t INDEX_IDENTITY_OFFSET = 192;
    constexpr std::size_t INDEX_EXTENT_OFFSET = 216;
    constexpr std::size_t DATA_IDENTITY_OFFSET = 224;
    constexpr std::size_t DATA_EXTENT_OFFSET = 248;
    constexpr std::size_t PAYLOAD_KIND_OFFSET = 256;
    constexpr std::size_t PAYLOAD_VERSION_OFFSET = 260;
    constexpr std::size_t PAYLOAD_DIGEST_OFFSET = 264;
    constexpr std::size_t PAYLOAD_OFFSET = 296;

    CHECK(load_u32_le(bytes, WIRE_VERSION_OFFSET) == relation::OOC_PRIVATE_HANDOFF_WIRE_VERSION_V1);
    CHECK(load_u32_le(bytes, DECLARED_SIZE_OFFSET) == bytes.size());
    CHECK(load_u32_le(bytes, SCHEMA_VERSION_OFFSET) == record.schema_version);
    CHECK(load_u64_le(bytes, PLATFORM_OFFSET) == record.platform_id);
    CHECK(load_u64_le(bytes, LEASE_LOW_OFFSET) == record.lease_id[0]);
    CHECK(load_u64_le(bytes, LEASE_HIGH_OFFSET) == record.lease_id[1]);
    check_identity_le(bytes, LOCK_IDENTITY_OFFSET, record.lock_identity);
    check_identity_le(bytes, DIRECTORY_IDENTITY_OFFSET, record.directory_identity);
    check_identity_le(bytes, OWNER_IDENTITY_OFFSET, record.owner_marker_identity);
    check_identity_le(bytes, OWNED_IDENTITY_OFFSET, record.owned_marker_identity);
    CHECK(load_u64_le(bytes, PAIR_FORMAT_OFFSET) == record.pair.format_version);
    CHECK(load_u64_le(bytes, PAIR_STORE_OFFSET) == record.pair.store_id);
    CHECK(load_u64_le(bytes, PAIR_GENERATION_OFFSET) == record.pair.generation);
    CHECK(load_u64_le(bytes, PAIR_COUNT_OFFSET) == record.pair.count);
    CHECK(load_u64_le(bytes, PAIR_INDEX_EXTENT_OFFSET) == record.pair.index_extent);
    CHECK(load_u64_le(bytes, PAIR_DATA_EXTENT_OFFSET) == record.pair.data_extent);
    check_identity_le(bytes, INDEX_IDENTITY_OFFSET, record.index.identity);
    CHECK(load_u64_le(bytes, INDEX_EXTENT_OFFSET) == record.index.extent);
    check_identity_le(bytes, DATA_IDENTITY_OFFSET, record.data.identity);
    CHECK(load_u64_le(bytes, DATA_EXTENT_OFFSET) == record.data.extent);
    CHECK(load_u32_le(bytes, PAYLOAD_KIND_OFFSET) == record.payload_kind);
    CHECK(load_u32_le(bytes, PAYLOAD_VERSION_OFFSET) == record.payload_version);
    CHECK(load_u32_le(bytes, PAYLOAD_SIZE_OFFSET) == record.opaque_payload.size());
    CHECK(std::equal(record.payload_digest.bytes.begin(), record.payload_digest.bytes.end(),
                     bytes.begin() + static_cast<std::ptrdiff_t>(PAYLOAD_DIGEST_OFFSET)));
    CHECK(std::equal(record.opaque_payload.begin(), record.opaque_payload.end(),
                     bytes.begin() + static_cast<std::ptrdiff_t>(PAYLOAD_OFFSET)));
    CHECK(std::equal(record.self_digest.bytes.begin(), record.self_digest.bytes.end(),
                     bytes.end() - static_cast<std::ptrdiff_t>(gnfs::util::SHA256_DIGEST_BYTES)));
}

void test_zero_row_and_payload_boundary() {
    const Record zero_row = make_sealed_record(0, {});
    require_ok(relation::validate_ooc_private_handoff_record(zero_row), "zero-row record");
    CHECK(zero_row.pair.index_extent == relation::OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                                            relation::OOCRelationStoreFormat::INDEX_SENTINEL_BYTES);
    CHECK(zero_row.pair.data_extent == relation::OOCRelationStoreFormat::DATA_HEADER_BYTES);
    CHECK(zero_row.index.extent == zero_row.pair.index_extent);
    CHECK(zero_row.data.extent == zero_row.pair.data_extent);
    const auto zero_bytes = encode_or_fail(zero_row);
    const auto zero_decoded = relation::decode_ooc_private_handoff_record(zero_bytes);
    CHECK(zero_decoded);
    CHECK(zero_decoded.value == zero_row);

    const Record maximum = make_sealed_record(
        3, payload_with_size(relation::OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES));
    const auto maximum_bytes = encode_or_fail(maximum);
    CHECK(maximum_bytes.size() == relation::OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES);
    const auto maximum_decoded = relation::decode_ooc_private_handoff_record(maximum_bytes);
    CHECK(maximum_decoded);
    CHECK(maximum_decoded.value == maximum);

    Record oversized = make_unsealed_record(
        3, payload_with_size(relation::OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES + 1U));
    require_code(relation::seal_ooc_private_handoff_record(oversized), Code::payload_too_large,
                 "64 KiB plus one seal");
    const auto oversized_encoded = relation::encode_ooc_private_handoff_record(oversized);
    CHECK(!oversized_encoded);
    CHECK(!oversized_encoded.bytes.has_value());
    CHECK(oversized_encoded.status.code == Code::payload_too_large);
}

template <typename Mutation>
void check_digest_binding(const Record& baseline, std::string_view name, Mutation mutation) {
    Record changed = baseline;
    mutation(changed);
    require_ok(relation::validate_ooc_private_handoff_record(changed, false), name);
    require_code(relation::validate_ooc_private_handoff_record(changed), Code::digest_mismatch,
                 name);
    const Digest changed_digest = digest_or_fail(changed);
    CHECK(changed_digest != baseline.self_digest);
    require_ok(relation::seal_ooc_private_handoff_record(changed), name);
    CHECK(changed.self_digest == changed_digest);
    CHECK(changed.self_digest != baseline.self_digest);
}

template <typename IdentityAccessor>
void check_identity_components(const Record& baseline, std::string_view name,
                               IdentityAccessor identity_accessor) {
    check_digest_binding(baseline, name,
                         [&](Record& record) { ++identity_accessor(record).first; });
    check_digest_binding(baseline, name,
                         [&](Record& record) { ++identity_accessor(record).second; });
    check_digest_binding(baseline, name,
                         [&](Record& record) { ++identity_accessor(record).third; });
}

void test_every_binding_field_affects_self_digest() {
    const Record baseline = make_sealed_record();

    check_digest_binding(baseline, "lease low", [](Record& record) { ++record.lease_id[0]; });
    check_digest_binding(baseline, "lease high", [](Record& record) { ++record.lease_id[1]; });
    check_identity_components(baseline, "lock identity",
                              [](Record& record) -> auto& { return record.lock_identity; });
    check_identity_components(baseline, "directory identity",
                              [](Record& record) -> auto& { return record.directory_identity; });
    check_identity_components(baseline, "owner identity",
                              [](Record& record) -> auto& { return record.owner_marker_identity; });
    check_identity_components(baseline, "owned identity",
                              [](Record& record) -> auto& { return record.owned_marker_identity; });

    check_digest_binding(baseline, "store id", [](Record& record) { ++record.pair.store_id; });
    check_digest_binding(baseline, "generation", [](Record& record) { ++record.pair.generation; });
    check_digest_binding(baseline, "count and index extent", [](Record& record) {
        ++record.pair.count;
        record.pair.index_extent = index_extent_for_count(record.pair.count);
        record.index.extent = record.pair.index_extent;
    });
    check_digest_binding(baseline, "data extent", [](Record& record) {
        ++record.pair.data_extent;
        record.data.extent = record.pair.data_extent;
    });
    check_identity_components(baseline, "index identity",
                              [](Record& record) -> auto& { return record.index.identity; });
    check_identity_components(baseline, "data identity",
                              [](Record& record) -> auto& { return record.data.identity; });
    check_digest_binding(baseline, "payload kind", [](Record& record) { ++record.payload_kind; });
    check_digest_binding(baseline, "payload version",
                         [](Record& record) { ++record.payload_version; });
    check_digest_binding(baseline, "opaque payload", [](Record& record) {
        record.opaque_payload.front() ^= std::byte{0x40};
        const auto digest = gnfs::util::sha256(record.opaque_payload);
        CHECK(digest.has_value());
        record.payload_digest = *digest;
    });
}

template <typename Mutation>
void check_invalid_record(const Record& baseline, Code expected, std::string_view name,
                          Mutation mutation) {
    Record changed = baseline;
    mutation(changed);
    require_code(relation::validate_ooc_private_handoff_record(changed, false), expected, name);
    changed.self_digest = {};
    require_code(relation::seal_ooc_private_handoff_record(changed), expected, name);
    const auto encoded = relation::encode_ooc_private_handoff_record(changed);
    CHECK(!encoded);
    CHECK(!encoded.bytes.has_value());
    CHECK(encoded.status.code == expected);
    const auto digest = relation::ooc_private_handoff_record_digest(changed);
    CHECK(!digest);
    CHECK(!digest.digest.has_value());
    CHECK(digest.status.code == expected);
}

[[nodiscard]] gnfs::util::durable_immutable_record::NativeIdentity& identity_at(Record& record,
                                                                                std::size_t index) {
    switch (index) {
    case 0:
        return record.lock_identity;
    case 1:
        return record.directory_identity;
    case 2:
        return record.owner_marker_identity;
    case 3:
        return record.owned_marker_identity;
    case 4:
        return record.index.identity;
    case 5:
        return record.data.identity;
    default:
        throw TestFailure("identity index is out of range");
    }
}

void test_invalid_versions_platform_identities_and_values() {
    const Record baseline = make_sealed_record();

    check_invalid_record(baseline, Code::unsupported_schema_version, "schema version",
                         [](Record& record) { ++record.schema_version; });
    check_invalid_record(baseline, Code::platform_mismatch, "unknown platform",
                         [](Record& record) { record.platform_id = UINT64_C(0xffff); });
    check_invalid_record(
        baseline, Code::platform_mismatch, "other known platform", [](Record& record) {
            record.platform_id = relation::OOC_PRIVATE_HANDOFF_CURRENT_PLATFORM_V1 ==
                                         relation::OOC_PRIVATE_HANDOFF_PLATFORM_POSIX_V1
                                     ? relation::OOC_PRIVATE_HANDOFF_PLATFORM_WINDOWS_V1
                                     : relation::OOC_PRIVATE_HANDOFF_PLATFORM_POSIX_V1;
        });
    check_invalid_record(baseline, Code::invalid_value, "nil lease",
                         [](Record& record) { record.lease_id = {}; });
    check_invalid_record(baseline, Code::invalid_value, "zero lock identity",
                         [](Record& record) { record.lock_identity = {}; });
    check_invalid_record(baseline, Code::invalid_value, "zero directory identity",
                         [](Record& record) { record.directory_identity = {}; });
    check_invalid_record(baseline, Code::invalid_value, "zero owner identity",
                         [](Record& record) { record.owner_marker_identity = {}; });
    check_invalid_record(baseline, Code::invalid_value, "zero owned identity",
                         [](Record& record) { record.owned_marker_identity = {}; });
    check_invalid_record(baseline, Code::invalid_value, "zero index identity",
                         [](Record& record) { record.index.identity = {}; });
    check_invalid_record(baseline, Code::invalid_value, "zero data identity",
                         [](Record& record) { record.data.identity = {}; });
    constexpr std::size_t IDENTITY_COUNT = 6;
    for (std::size_t left = 0; left < IDENTITY_COUNT; ++left) {
        for (std::size_t right = left + 1; right < IDENTITY_COUNT; ++right) {
            Record aliased = baseline;
            identity_at(aliased, right) = identity_at(aliased, left);
            require_code(relation::validate_ooc_private_handoff_record(aliased, false),
                         Code::invalid_value, "aliased native identities");
        }
    }
    check_invalid_record(
        baseline, Code::invalid_value, "unsupported OOC format", [](Record& record) {
            record.pair.format_version = relation::OOCRelationStoreFormat::FORMAT_VERSION_V2;
        });
    check_invalid_record(baseline, Code::invalid_value, "zero store id",
                         [](Record& record) { record.pair.store_id = 0; });
    check_invalid_record(baseline, Code::invalid_value, "zero generation",
                         [](Record& record) { record.pair.generation = 0; });
    check_invalid_record(baseline, Code::invalid_value, "zero payload kind",
                         [](Record& record) { record.payload_kind = 0; });
    check_invalid_record(baseline, Code::invalid_value, "zero payload version",
                         [](Record& record) { record.payload_version = 0; });
}

void test_extent_formula_and_overflow_rejection() {
    const Record baseline = make_sealed_record();

    check_invalid_record(baseline, Code::invalid_value, "pair index extent formula",
                         [](Record& record) { ++record.pair.index_extent; });
    check_invalid_record(baseline, Code::invalid_value, "bound index extent mismatch",
                         [](Record& record) { ++record.index.extent; });
    check_invalid_record(baseline, Code::invalid_value, "bound data extent mismatch",
                         [](Record& record) { ++record.data.extent; });
    check_invalid_record(
        baseline, Code::invalid_value, "data extent below header", [](Record& record) {
            record.pair.data_extent = relation::OOCRelationStoreFormat::DATA_HEADER_BYTES - 1U;
            record.data.extent = record.pair.data_extent;
        });
    check_invalid_record(
        baseline, Code::invalid_value, "nonempty data at header", [](Record& record) {
            record.pair.data_extent = relation::OOCRelationStoreFormat::DATA_HEADER_BYTES;
            record.data.extent = record.pair.data_extent;
        });

    const Record zero_row = make_sealed_record(0, {});
    check_invalid_record(zero_row, Code::invalid_value, "zero-row extra data", [](Record& record) {
        ++record.pair.data_extent;
        record.data.extent = record.pair.data_extent;
    });

    check_invalid_record(
        baseline, Code::integer_out_of_range, "index extent overflow",
        [](Record& record) { record.pair.count = std::numeric_limits<std::uint64_t>::max(); });
    check_invalid_record(
        baseline, Code::integer_out_of_range, "signed index extent overflow", [](Record& record) {
            constexpr std::uint64_t FIXED_INDEX_BYTES =
                relation::OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                relation::OOCRelationStoreFormat::INDEX_SENTINEL_BYTES;
            record.pair.count =
                (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                 FIXED_INDEX_BYTES) /
                    sizeof(std::uint64_t) +
                1U;
        });
    check_invalid_record(
        baseline, Code::invalid_value, "signed data extent overflow", [](Record& record) {
            record.pair.data_extent =
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
            record.data.extent = record.pair.data_extent;
        });
}

void test_payload_and_self_digest_rejection() {
    const Record baseline = make_sealed_record();

    Record payload_changed = baseline;
    payload_changed.opaque_payload.front() ^= std::byte{0x01};
    require_code(relation::validate_ooc_private_handoff_record(payload_changed, false),
                 Code::digest_mismatch, "payload digest mismatch without self verification");

    Record payload_digest_changed = baseline;
    perturb_digest(payload_digest_changed.payload_digest);
    require_code(relation::validate_ooc_private_handoff_record(payload_digest_changed, false),
                 Code::digest_mismatch, "payload digest field mismatch");

    Record self_changed = baseline;
    perturb_digest(self_changed.self_digest);
    require_ok(relation::validate_ooc_private_handoff_record(self_changed, false),
               "self digest ignored only when requested");
    require_code(relation::validate_ooc_private_handoff_record(self_changed), Code::digest_mismatch,
                 "self digest mismatch");
}

void test_wire_framing_magic_versions_and_unknown_platform() {
    const Record baseline = make_sealed_record();
    const auto encoded = encode_or_fail(baseline);

    for (std::size_t prefix_size = 0; prefix_size < encoded.size(); ++prefix_size) {
        const auto decoded = relation::decode_ooc_private_handoff_record(
            std::span<const std::byte>(encoded).first(prefix_size));
        CHECK(!decoded);
        CHECK(!decoded.value.has_value());
        CHECK(decoded.status.code != Code::none);
    }

    auto trailing = encoded;
    trailing.push_back(std::byte{0x42});
    const auto trailing_result = relation::decode_ooc_private_handoff_record(trailing);
    CHECK(!trailing_result);
    CHECK(!trailing_result.value.has_value());
    CHECK(trailing_result.status.code == Code::trailing_bytes);

    auto bad_magic = encoded;
    bad_magic.front() ^= std::byte{0x80};
    const auto bad_magic_result = relation::decode_ooc_private_handoff_record(bad_magic);
    CHECK(!bad_magic_result);
    CHECK(bad_magic_result.status.code == Code::invalid_magic);

    auto bad_wire_version = encoded;
    store_u32_le(bad_wire_version, 8, relation::OOC_PRIVATE_HANDOFF_WIRE_VERSION_V1 + 1U);
    const auto bad_wire_result = relation::decode_ooc_private_handoff_record(bad_wire_version);
    CHECK(!bad_wire_result);
    CHECK(bad_wire_result.status.code == Code::unsupported_wire_version);

    auto bad_declared_size = encoded;
    store_u32_le(bad_declared_size, 24, static_cast<std::uint32_t>(encoded.size() - 1U));
    const auto bad_size_result = relation::decode_ooc_private_handoff_record(bad_declared_size);
    CHECK(!bad_size_result);
    CHECK(bad_size_result.status.code == Code::declared_size_mismatch);

    auto bad_schema = encoded;
    store_u32_le(bad_schema, 12, relation::OOC_PRIVATE_HANDOFF_SCHEMA_VERSION_V1 + 1U);
    const auto bad_schema_result = relation::decode_ooc_private_handoff_record(bad_schema);
    CHECK(!bad_schema_result);
    CHECK(bad_schema_result.status.code == Code::unsupported_schema_version);

    auto unknown_platform = encoded;
    store_u64_le(unknown_platform, 16, UINT64_C(0xffffffffffffffff));
    const auto unknown_platform_result =
        relation::decode_ooc_private_handoff_record(unknown_platform);
    CHECK(!unknown_platform_result);
    CHECK(unknown_platform_result.status.code == Code::platform_mismatch);

    auto payload_too_large = encoded;
    store_u32_le(
        payload_too_large, 28,
        static_cast<std::uint32_t>(relation::OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES + 1U));
    const auto payload_too_large_result =
        relation::decode_ooc_private_handoff_record(payload_too_large);
    CHECK(!payload_too_large_result);
    CHECK(payload_too_large_result.status.code == Code::payload_too_large);
    CHECK(payload_too_large_result.status.byte_offset == 28);

    auto payload_digest_tamper = encoded;
    payload_digest_tamper[264] ^= std::byte{0x01};
    const auto payload_digest_result =
        relation::decode_ooc_private_handoff_record(payload_digest_tamper);
    CHECK(!payload_digest_result);
    CHECK(payload_digest_result.status.code == Code::digest_mismatch);
    CHECK(payload_digest_result.status.byte_offset == 264);

    auto payload_tamper = encoded;
    payload_tamper[296] ^= std::byte{0x01};
    const auto payload_result = relation::decode_ooc_private_handoff_record(payload_tamper);
    CHECK(!payload_result);
    CHECK(payload_result.status.code == Code::digest_mismatch);
    CHECK(payload_result.status.byte_offset == 264);

    auto self_digest_tamper = encoded;
    self_digest_tamper.back() ^= std::byte{0x01};
    const auto self_digest_result = relation::decode_ooc_private_handoff_record(self_digest_tamper);
    CHECK(!self_digest_result);
    CHECK(self_digest_result.status.code == Code::digest_mismatch);
    CHECK(self_digest_result.status.byte_offset ==
          encoded.size() - gnfs::util::SHA256_DIGEST_BYTES);

    std::vector<std::byte> oversized(relation::OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES + 1U,
                                     std::byte{0});
    const auto oversized_result = relation::decode_ooc_private_handoff_record(oversized);
    CHECK(!oversized_result);
    CHECK(!oversized_result.value.has_value());
    CHECK(oversized_result.status.code == Code::input_too_large);
}

void test_result_invariants() {
    const Record valid = make_sealed_record();

    const auto valid_encode = relation::encode_ooc_private_handoff_record(valid);
    CHECK(static_cast<bool>(valid_encode) ==
          (valid_encode.bytes.has_value() && static_cast<bool>(valid_encode.status)));
    CHECK(valid_encode);
    CHECK(valid_encode.bytes.has_value());
    CHECK(valid_encode.status.code == Code::none);
    CHECK(valid_encode.status.byte_offset == relation::OOC_PRIVATE_HANDOFF_NO_OFFSET);

    const auto valid_decode = relation::decode_ooc_private_handoff_record(*valid_encode.bytes);
    CHECK(static_cast<bool>(valid_decode) ==
          (valid_decode.value.has_value() && static_cast<bool>(valid_decode.status)));
    CHECK(valid_decode);
    CHECK(valid_decode.value.has_value());
    CHECK(valid_decode.status.code == Code::none);
    CHECK(valid_decode.status.byte_offset == relation::OOC_PRIVATE_HANDOFF_NO_OFFSET);

    const auto valid_digest = relation::ooc_private_handoff_record_digest(valid);
    CHECK(static_cast<bool>(valid_digest) ==
          (valid_digest.digest.has_value() && static_cast<bool>(valid_digest.status)));
    CHECK(valid_digest);
    CHECK(valid_digest.digest.has_value());
    CHECK(valid_digest.status.code == Code::none);
    CHECK(valid_digest.status.byte_offset == relation::OOC_PRIVATE_HANDOFF_NO_OFFSET);

    Record invalid = valid;
    invalid.payload_kind = 0;
    const Record before_failed_seal = invalid;
    require_code(relation::seal_ooc_private_handoff_record(invalid), Code::invalid_value,
                 "failed seal status");
    CHECK(invalid == before_failed_seal);

    const auto invalid_encode = relation::encode_ooc_private_handoff_record(invalid);
    CHECK(static_cast<bool>(invalid_encode) ==
          (invalid_encode.bytes.has_value() && static_cast<bool>(invalid_encode.status)));
    CHECK(!invalid_encode);
    CHECK(!invalid_encode.bytes.has_value());
    CHECK(invalid_encode.status.code != Code::none);

    const auto invalid_digest = relation::ooc_private_handoff_record_digest(invalid);
    CHECK(static_cast<bool>(invalid_digest) ==
          (invalid_digest.digest.has_value() && static_cast<bool>(invalid_digest.status)));
    CHECK(!invalid_digest);
    CHECK(!invalid_digest.digest.has_value());
    CHECK(invalid_digest.status.code != Code::none);

    const std::array<std::byte, 1> truncated{std::byte{0}};
    const auto invalid_decode = relation::decode_ooc_private_handoff_record(truncated);
    CHECK(static_cast<bool>(invalid_decode) ==
          (invalid_decode.value.has_value() && static_cast<bool>(invalid_decode.status)));
    CHECK(!invalid_decode);
    CHECK(!invalid_decode.value.has_value());
    CHECK(invalid_decode.status.code == Code::truncated);
    CHECK(invalid_decode.status.byte_offset != relation::OOC_PRIVATE_HANDOFF_NO_OFFSET);
}

void require_authorized_ok(const AuthorizedStatus& status, std::string_view context) {
    if (!status) {
        fail(context, __LINE__,
             relation::ooc_authorized_cleanup_intent_protocol_code_name(status.code));
    }
}

void require_authorized_code(const AuthorizedStatus& status, AuthorizedCode expected,
                             std::string_view context) {
    if (status || status.code != expected) {
        fail(context, __LINE__,
             relation::ooc_authorized_cleanup_intent_protocol_code_name(status.code));
    }
}

[[nodiscard]] Digest seeded_digest(std::uint8_t seed) {
    Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index * 17U));
    }
    return digest;
}

[[nodiscard]] AuthorizedRecord make_unsealed_authorized_intent(std::uint64_t count = 3) {
    AuthorizedRecord intent;
    intent.schema_version = relation::OOC_AUTHORIZED_CLEANUP_INTENT_SCHEMA_VERSION_V2;
    intent.platform_id = relation::OOC_AUTHORIZED_CLEANUP_INTENT_CURRENT_PLATFORM_V1;
    intent.base_path_digest = seeded_digest(0x01);
    intent.external_authorization_digest = seeded_digest(0x21);
    intent.generic_handoff_self_digest = seeded_digest(0x41);
    intent.lease_id = {
        UINT64_C(0x0807060504030201),
        UINT64_C(0x1817161514131211),
    };
    intent.parent_directory_identity = native_identity(UINT64_C(0x2122232425262728));
    intent.lock_identity = native_identity(UINT64_C(0x3132333435363738));
    intent.directory_identity = native_identity(UINT64_C(0x4142434445464748));
    intent.owner_marker_identity = native_identity(UINT64_C(0x5152535455565758));
    intent.owned_marker_identity = native_identity(UINT64_C(0x6162636465666768));
    intent.pair.format_version = relation::OOCRelationStoreFormat::FORMAT_VERSION_V3;
    intent.pair.store_id = UINT64_C(0x7172737475767778);
    intent.pair.generation = UINT64_C(0x8182838485868788);
    intent.pair.count = count;
    intent.pair.index_extent = index_extent_for_count(count);
    intent.pair.data_extent =
        count == 0 ? relation::OOCRelationStoreFormat::DATA_HEADER_BYTES : UINT64_C(0x280);
    intent.handoff.identity = native_identity(UINT64_C(0x9192939495969798));
    intent.handoff.extent = relation::OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 + 17U;
    intent.pending_handoff = relation::OOCPrivateHandoffArtifactBindingV1{
        .identity = native_identity(UINT64_C(0x999a9b9c9d9e9fa0)),
        .extent = intent.handoff.extent,
    };
    intent.index.identity = native_identity(UINT64_C(0xa1a2a3a4a5a6a7a8));
    intent.index.extent = intent.pair.index_extent;
    intent.data.identity = native_identity(UINT64_C(0xb1b2b3b4b5b6b7b8));
    intent.data.extent = intent.pair.data_extent;
    return intent;
}

[[nodiscard]] AuthorizedRecord make_sealed_authorized_intent(std::uint64_t count = 3) {
    AuthorizedRecord intent = make_unsealed_authorized_intent(count);
    require_authorized_ok(relation::seal_ooc_authorized_cleanup_intent(intent),
                          "seal authorized intent fixture");
    return intent;
}

[[nodiscard]] std::vector<std::byte> encode_authorized_or_fail(const AuthorizedRecord& intent) {
    const auto encoded = relation::encode_ooc_authorized_cleanup_intent(intent);
    if (!encoded) {
        fail("encode_ooc_authorized_cleanup_intent", __LINE__,
             relation::ooc_authorized_cleanup_intent_protocol_code_name(encoded.status.code));
    }
    CHECK(encoded.bytes.has_value());
    return *encoded.bytes;
}

[[nodiscard]] Digest authorized_digest_or_fail(const AuthorizedRecord& intent) {
    const auto digest = relation::ooc_authorized_cleanup_intent_digest(intent);
    if (!digest) {
        fail("ooc_authorized_cleanup_intent_digest", __LINE__,
             relation::ooc_authorized_cleanup_intent_protocol_code_name(digest.status.code));
    }
    CHECK(digest.digest.has_value());
    return *digest.digest;
}

void test_authorized_intent_constants_and_roundtrip() {
    [[maybe_unused]] constexpr std::string_view EXPECTED_POSIX_SELF_DIGEST =
        "25ba7fb211fc5886b6c02d8b2bf0c53530b3e3e19b508db91dd1886f0c9ef12f";
    [[maybe_unused]] constexpr std::string_view EXPECTED_WINDOWS_SELF_DIGEST =
        "931124e3d7d1690efeb6e1c884ea530e2b95d9998f4de59772928414fbec77e0";
#ifdef _WIN32
    constexpr std::string_view EXPECTED_SELF_DIGEST = EXPECTED_WINDOWS_SELF_DIGEST;
#else
    constexpr std::string_view EXPECTED_SELF_DIGEST = EXPECTED_POSIX_SELF_DIGEST;
#endif
    CHECK(relation::OOC_AUTHORIZED_CLEANUP_INTENT_SCHEMA_VERSION_V2 == 2);
    CHECK(relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_VERSION_V1 == 1);
    CHECK(relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 == 480);

    constexpr std::array CODES{
        AuthorizedCode::none,
        AuthorizedCode::input_too_large,
        AuthorizedCode::output_too_large,
        AuthorizedCode::truncated,
        AuthorizedCode::trailing_bytes,
        AuthorizedCode::invalid_magic,
        AuthorizedCode::unsupported_wire_version,
        AuthorizedCode::unsupported_schema_version,
        AuthorizedCode::platform_mismatch,
        AuthorizedCode::declared_size_mismatch,
        AuthorizedCode::invalid_value,
        AuthorizedCode::unexpected_marker_kind,
        AuthorizedCode::integer_out_of_range,
        AuthorizedCode::digest_mismatch,
        AuthorizedCode::digest_unavailable,
        AuthorizedCode::resource_exhausted,
    };
    static_assert(static_cast<std::uint8_t>(AuthorizedCode::resource_exhausted) + 1U ==
                  CODES.size());
    for (std::size_t index = 0; index < CODES.size(); ++index) {
        CHECK(static_cast<std::uint8_t>(CODES[index]) == index);
        CHECK(!relation::ooc_authorized_cleanup_intent_protocol_code_name(CODES[index]).empty());
    }
    CHECK(!relation::ooc_authorized_cleanup_intent_protocol_code_name(
               static_cast<AuthorizedCode>(0xff))
               .empty());

    const AuthorizedRecord first = make_sealed_authorized_intent();
    const AuthorizedRecord second = make_sealed_authorized_intent();
    CHECK(first == second);
    require_authorized_ok(relation::validate_ooc_authorized_cleanup_intent(first),
                          "validate authorized intent fixture");
    CHECK(first.self_digest == authorized_digest_or_fail(first));
    CHECK(first.self_digest == digest_from_hex(EXPECTED_SELF_DIGEST));

    constexpr std::array EXPECTED_MAGIC{
        std::byte{'G'}, std::byte{'N'}, std::byte{'F'}, std::byte{'S'},
        std::byte{'A'}, std::byte{'C'}, std::byte{'I'}, std::byte{'2'},
    };
    const auto first_bytes = encode_authorized_or_fail(first);
    const auto second_bytes = encode_authorized_or_fail(second);
    CHECK(first_bytes == second_bytes);
    CHECK(first_bytes.size() == relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2);
    CHECK(std::equal(EXPECTED_MAGIC.begin(), EXPECTED_MAGIC.end(), first_bytes.begin()));

    const auto decoded =
        relation::decode_ooc_authorized_cleanup_intent(first_bytes, first.marker_kind);
    CHECK(decoded);
    CHECK(decoded.value == first);
    CHECK(decoded.status.code == AuthorizedCode::none);
    CHECK(decoded.status.byte_offset == relation::OOC_AUTHORIZED_CLEANUP_INTENT_NO_OFFSET);
    CHECK(encode_authorized_or_fail(*decoded.value) == first_bytes);
}

void test_authorized_intent_little_endian_fields() {
    const AuthorizedRecord intent = make_sealed_authorized_intent();
    const auto bytes = encode_authorized_or_fail(intent);

    CHECK(load_u32_le(bytes, 8) == relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_VERSION_V1);
    CHECK(load_u32_le(bytes, 12) == intent.schema_version);
    CHECK(load_u64_le(bytes, 16) == intent.platform_id);
    CHECK(load_u32_le(bytes, 24) == relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2);
    CHECK(load_u32_le(bytes, 28) ==
          static_cast<std::uint32_t>(relation::OOCAuthorizedCleanupMarkerKindV2::intent));
    CHECK(std::equal(intent.base_path_digest.bytes.begin(), intent.base_path_digest.bytes.end(),
                     bytes.begin() + 32));
    CHECK(std::equal(intent.external_authorization_digest.bytes.begin(),
                     intent.external_authorization_digest.bytes.end(), bytes.begin() + 64));
    CHECK(std::equal(intent.generic_handoff_self_digest.bytes.begin(),
                     intent.generic_handoff_self_digest.bytes.end(), bytes.begin() + 96));
    CHECK(load_u64_le(bytes, 128) == intent.lease_id[0]);
    CHECK(load_u64_le(bytes, 136) == intent.lease_id[1]);
    check_identity_le(bytes, 144, intent.parent_directory_identity);
    check_identity_le(bytes, 168, intent.lock_identity);
    check_identity_le(bytes, 192, intent.directory_identity);
    check_identity_le(bytes, 216, intent.owner_marker_identity);
    check_identity_le(bytes, 240, intent.owned_marker_identity);
    CHECK(load_u64_le(bytes, 264) == intent.pair.format_version);
    CHECK(load_u64_le(bytes, 272) == intent.pair.store_id);
    CHECK(load_u64_le(bytes, 280) == intent.pair.generation);
    CHECK(load_u64_le(bytes, 288) == intent.pair.count);
    CHECK(load_u64_le(bytes, 296) == intent.pair.index_extent);
    CHECK(load_u64_le(bytes, 304) == intent.pair.data_extent);
    check_identity_le(bytes, 312, intent.handoff.identity);
    CHECK(load_u64_le(bytes, 336) == intent.handoff.extent);
    CHECK(intent.pending_handoff.has_value());
    CHECK(load_u32_le(bytes, 344) == 1);
    CHECK(load_u32_le(bytes, 348) == 0);
    check_identity_le(bytes, 352, intent.pending_handoff->identity);
    CHECK(load_u64_le(bytes, 376) == intent.pending_handoff->extent);
    check_identity_le(bytes, 384, intent.index.identity);
    CHECK(load_u64_le(bytes, 408) == intent.index.extent);
    check_identity_le(bytes, 416, intent.data.identity);
    CHECK(load_u64_le(bytes, 440) == intent.data.extent);
    CHECK(std::equal(intent.self_digest.bytes.begin(), intent.self_digest.bytes.end(),
                     bytes.begin() + 448));
}

void test_authorized_intent_zero_digest_and_zero_row() {
    AuthorizedRecord zero_digests = make_unsealed_authorized_intent();
    zero_digests.base_path_digest = {};
    zero_digests.external_authorization_digest = {};
    zero_digests.generic_handoff_self_digest = {};
    require_authorized_ok(relation::seal_ooc_authorized_cleanup_intent(zero_digests),
                          "all-zero input digests are present values");
    const auto decoded = relation::decode_ooc_authorized_cleanup_intent(
        encode_authorized_or_fail(zero_digests), zero_digests.marker_kind);
    CHECK(decoded);
    CHECK(decoded.value == zero_digests);

    AuthorizedRecord no_pending = make_unsealed_authorized_intent();
    no_pending.pending_handoff.reset();
    require_authorized_ok(relation::seal_ooc_authorized_cleanup_intent(no_pending),
                          "absent pending handoff is explicit");
    const auto no_pending_bytes = encode_authorized_or_fail(no_pending);
    CHECK(load_u32_le(no_pending_bytes, 344) == 0);
    CHECK(std::all_of(no_pending_bytes.begin() + 348, no_pending_bytes.begin() + 384,
                      [](std::byte byte) { return byte == std::byte{0}; }));
    const auto no_pending_decoded =
        relation::decode_ooc_authorized_cleanup_intent(no_pending_bytes, no_pending.marker_kind);
    CHECK(no_pending_decoded);
    CHECK(no_pending_decoded.value == no_pending);

    const AuthorizedRecord zero_row = make_sealed_authorized_intent(0);
    CHECK(zero_row.pair.index_extent == relation::OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                                            relation::OOCRelationStoreFormat::INDEX_SENTINEL_BYTES);
    CHECK(zero_row.pair.data_extent == relation::OOCRelationStoreFormat::DATA_HEADER_BYTES);
    const auto zero_decoded = relation::decode_ooc_authorized_cleanup_intent(
        encode_authorized_or_fail(zero_row), zero_row.marker_kind);
    CHECK(zero_decoded);
    CHECK(zero_decoded.value == zero_row);
}

template <typename Mutation>
void check_authorized_digest_binding(const AuthorizedRecord& baseline, std::string_view name,
                                     Mutation mutation) {
    AuthorizedRecord changed = baseline;
    mutation(changed);
    require_authorized_ok(relation::validate_ooc_authorized_cleanup_intent(changed, false), name);
    require_authorized_code(relation::validate_ooc_authorized_cleanup_intent(changed),
                            AuthorizedCode::digest_mismatch, name);
    const Digest changed_digest = authorized_digest_or_fail(changed);
    CHECK(changed_digest != baseline.self_digest);
    require_authorized_ok(relation::seal_ooc_authorized_cleanup_intent(changed), name);
    CHECK(changed.self_digest == changed_digest);
}

[[nodiscard]] gnfs::util::durable_immutable_record::NativeIdentity&
authorized_identity_at(AuthorizedRecord& intent, std::size_t index) {
    switch (index) {
    case 0:
        return intent.parent_directory_identity;
    case 1:
        return intent.lock_identity;
    case 2:
        return intent.directory_identity;
    case 3:
        return intent.owner_marker_identity;
    case 4:
        return intent.owned_marker_identity;
    case 5:
        return intent.handoff.identity;
    case 6:
        return intent.pending_handoff->identity;
    case 7:
        return intent.index.identity;
    case 8:
        return intent.data.identity;
    default:
        throw TestFailure("authorized identity index is out of range");
    }
}

void test_authorized_intent_binding_digest() {
    const AuthorizedRecord baseline = make_sealed_authorized_intent();
    check_authorized_digest_binding(baseline, "marker kind", [](AuthorizedRecord& intent) {
        intent.marker_kind = relation::OOCAuthorizedCleanupMarkerKindV2::staged;
    });
    check_authorized_digest_binding(baseline, "base path digest", [](AuthorizedRecord& intent) {
        intent.base_path_digest.bytes.front() ^= std::byte{0x01};
    });
    check_authorized_digest_binding(
        baseline, "external authorization digest", [](AuthorizedRecord& intent) {
            intent.external_authorization_digest.bytes.front() ^= std::byte{0x02};
        });
    check_authorized_digest_binding(
        baseline, "generic handoff digest", [](AuthorizedRecord& intent) {
            intent.generic_handoff_self_digest.bytes.front() ^= std::byte{0x04};
        });
    check_authorized_digest_binding(baseline, "lease low",
                                    [](AuthorizedRecord& intent) { ++intent.lease_id[0]; });
    check_authorized_digest_binding(baseline, "lease high",
                                    [](AuthorizedRecord& intent) { ++intent.lease_id[1]; });
    for (std::size_t index = 0; index < 9; ++index) {
        check_authorized_digest_binding(
            baseline, "native identity first",
            [index](AuthorizedRecord& intent) { ++authorized_identity_at(intent, index).first; });
        check_authorized_digest_binding(
            baseline, "native identity second",
            [index](AuthorizedRecord& intent) { ++authorized_identity_at(intent, index).second; });
        check_authorized_digest_binding(
            baseline, "native identity third",
            [index](AuthorizedRecord& intent) { ++authorized_identity_at(intent, index).third; });
    }
    check_authorized_digest_binding(baseline, "store id",
                                    [](AuthorizedRecord& intent) { ++intent.pair.store_id; });
    check_authorized_digest_binding(baseline, "generation",
                                    [](AuthorizedRecord& intent) { ++intent.pair.generation; });
    check_authorized_digest_binding(
        baseline, "count and index extent", [](AuthorizedRecord& intent) {
            ++intent.pair.count;
            intent.pair.index_extent = index_extent_for_count(intent.pair.count);
            intent.index.extent = intent.pair.index_extent;
        });
    check_authorized_digest_binding(baseline, "data extent", [](AuthorizedRecord& intent) {
        ++intent.pair.data_extent;
        intent.data.extent = intent.pair.data_extent;
    });
    check_authorized_digest_binding(baseline, "handoff extent", [](AuthorizedRecord& intent) {
        ++intent.handoff.extent;
        ++intent.pending_handoff->extent;
    });
    check_authorized_digest_binding(
        baseline, "pending handoff presence",
        [](AuthorizedRecord& intent) { intent.pending_handoff.reset(); });
}

template <typename Mutation>
void check_invalid_authorized_intent(const AuthorizedRecord& baseline, AuthorizedCode expected,
                                     std::string_view name, Mutation mutation) {
    AuthorizedRecord changed = baseline;
    mutation(changed);
    require_authorized_code(relation::validate_ooc_authorized_cleanup_intent(changed, false),
                            expected, name);
    const AuthorizedRecord before = changed;
    require_authorized_code(relation::seal_ooc_authorized_cleanup_intent(changed), expected, name);
    CHECK(changed == before);
    const auto encoded = relation::encode_ooc_authorized_cleanup_intent(changed);
    CHECK(!encoded);
    CHECK(!encoded.bytes.has_value());
    CHECK(encoded.status.code == expected);
}

void test_authorized_intent_invalid_bindings() {
    const AuthorizedRecord baseline = make_sealed_authorized_intent();
    check_invalid_authorized_intent(baseline, AuthorizedCode::unsupported_schema_version,
                                    "schema version",
                                    [](AuthorizedRecord& intent) { ++intent.schema_version; });
    check_invalid_authorized_intent(
        baseline, AuthorizedCode::platform_mismatch, "platform",
        [](AuthorizedRecord& intent) { intent.platform_id = UINT64_C(0xffff); });
    check_invalid_authorized_intent(
        baseline, AuthorizedCode::invalid_value, "marker kind", [](AuthorizedRecord& intent) {
            intent.marker_kind = static_cast<relation::OOCAuthorizedCleanupMarkerKindV2>(0);
        });
    check_invalid_authorized_intent(baseline, AuthorizedCode::invalid_value, "nil lease",
                                    [](AuthorizedRecord& intent) { intent.lease_id = {}; });
    for (std::size_t index = 0; index < 9; ++index) {
        AuthorizedRecord zero = baseline;
        authorized_identity_at(zero, index) = {};
        require_authorized_code(relation::validate_ooc_authorized_cleanup_intent(zero, false),
                                AuthorizedCode::invalid_value, "zero native identity");
        for (std::size_t right = index + 1; right < 9; ++right) {
            AuthorizedRecord aliased = baseline;
            authorized_identity_at(aliased, right) = authorized_identity_at(aliased, index);
            require_authorized_code(
                relation::validate_ooc_authorized_cleanup_intent(aliased, false),
                AuthorizedCode::invalid_value, "aliased native identities");
        }
    }
    check_invalid_authorized_intent(
        baseline, AuthorizedCode::invalid_value, "format version", [](AuthorizedRecord& intent) {
            intent.pair.format_version = relation::OOCRelationStoreFormat::FORMAT_VERSION_V2;
        });
    check_invalid_authorized_intent(baseline, AuthorizedCode::invalid_value, "store id",
                                    [](AuthorizedRecord& intent) { intent.pair.store_id = 0; });
    check_invalid_authorized_intent(baseline, AuthorizedCode::invalid_value, "generation",
                                    [](AuthorizedRecord& intent) { intent.pair.generation = 0; });
    check_invalid_authorized_intent(baseline, AuthorizedCode::invalid_value, "index extent formula",
                                    [](AuthorizedRecord& intent) { ++intent.pair.index_extent; });
    check_invalid_authorized_intent(baseline, AuthorizedCode::invalid_value, "index binding extent",
                                    [](AuthorizedRecord& intent) { ++intent.index.extent; });
    check_invalid_authorized_intent(baseline, AuthorizedCode::invalid_value, "data binding extent",
                                    [](AuthorizedRecord& intent) { ++intent.data.extent; });
    check_invalid_authorized_intent(baseline, AuthorizedCode::invalid_value,
                                    "handoff below minimum", [](AuthorizedRecord& intent) {
                                        intent.handoff.extent =
                                            relation::OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1 - 1U;
                                    });
    check_invalid_authorized_intent(baseline, AuthorizedCode::invalid_value,
                                    "handoff above maximum", [](AuthorizedRecord& intent) {
                                        intent.handoff.extent =
                                            relation::OOC_PRIVATE_HANDOFF_MAX_RECORD_BYTES + 1U;
                                    });
    check_invalid_authorized_intent(
        baseline, AuthorizedCode::invalid_value, "pending handoff extent",
        [](AuthorizedRecord& intent) { ++intent.pending_handoff->extent; });
    check_invalid_authorized_intent(baseline, AuthorizedCode::integer_out_of_range,
                                    "index overflow", [](AuthorizedRecord& intent) {
                                        intent.pair.count =
                                            std::numeric_limits<std::uint64_t>::max();
                                    });
}

void test_authorized_intent_wire_rejection_and_codec_separation() {
    constexpr auto INTENT_KIND = relation::OOCAuthorizedCleanupMarkerKindV2::intent;
    constexpr auto STAGED_KIND = relation::OOCAuthorizedCleanupMarkerKindV2::staged;
    const AuthorizedRecord baseline = make_sealed_authorized_intent();
    const auto encoded = encode_authorized_or_fail(baseline);
    CHECK(relation::decode_ooc_authorized_cleanup_intent(
              encoded, static_cast<relation::OOCAuthorizedCleanupMarkerKindV2>(0))
              .status.code == AuthorizedCode::invalid_value);
    for (std::size_t prefix = 0; prefix < encoded.size(); ++prefix) {
        const auto decoded = relation::decode_ooc_authorized_cleanup_intent(
            std::span<const std::byte>(encoded).first(prefix), INTENT_KIND);
        CHECK(!decoded);
        CHECK(decoded.status.code == AuthorizedCode::truncated);
    }

    auto trailing = encoded;
    trailing.push_back(std::byte{0x42});
    CHECK(relation::decode_ooc_authorized_cleanup_intent(trailing, INTENT_KIND).status.code ==
          AuthorizedCode::trailing_bytes);
    std::vector<std::byte> oversized(encoded.size() + 64U * 1024U + 1U, std::byte{0});
    CHECK(relation::decode_ooc_authorized_cleanup_intent(oversized, INTENT_KIND).status.code ==
          AuthorizedCode::input_too_large);

    auto changed = encoded;
    changed.front() ^= std::byte{0x01};
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND).status.code ==
          AuthorizedCode::invalid_magic);
    changed = encoded;
    store_u32_le(changed, 8, relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_VERSION_V1 + 1U);
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND).status.code ==
          AuthorizedCode::unsupported_wire_version);
    changed = encoded;
    store_u32_le(changed, 12, relation::OOC_AUTHORIZED_CLEANUP_INTENT_SCHEMA_VERSION_V2 + 1U);
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND).status.code ==
          AuthorizedCode::unsupported_schema_version);
    changed = encoded;
    store_u64_le(changed, 16, UINT64_C(0xffff));
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND).status.code ==
          AuthorizedCode::platform_mismatch);
    changed = encoded;
    store_u32_le(
        changed, 24,
        static_cast<std::uint32_t>(relation::OOC_AUTHORIZED_CLEANUP_INTENT_WIRE_BYTES_V2 - 1U));
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND).status.code ==
          AuthorizedCode::declared_size_mismatch);
    changed = encoded;
    store_u32_le(changed, 28, 0);
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND).status.code ==
          AuthorizedCode::invalid_value);
    changed = encoded;
    store_u32_le(changed, 344, 2);
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND).status.code ==
          AuthorizedCode::invalid_value);
    changed = encoded;
    store_u32_le(changed, 348, 1);
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND).status.code ==
          AuthorizedCode::invalid_value);
    changed = encoded;
    store_u32_le(changed, 344, 0);
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND).status.code ==
          AuthorizedCode::invalid_value);

    AuthorizedRecord staged = baseline;
    staged.marker_kind = STAGED_KIND;
    require_authorized_code(relation::validate_ooc_authorized_cleanup_intent(staged),
                            AuthorizedCode::digest_mismatch,
                            "intent digest cannot authorize staged marker");
    require_authorized_ok(relation::seal_ooc_authorized_cleanup_intent(staged),
                          "seal staged marker");
    const auto staged_bytes = encode_authorized_or_fail(staged);
    CHECK(staged_bytes != encoded);
    const auto staged_decoded =
        relation::decode_ooc_authorized_cleanup_intent(staged_bytes, STAGED_KIND);
    CHECK(staged_decoded);
    CHECK(staged_decoded.value->marker_kind == STAGED_KIND);
    CHECK(relation::decode_ooc_authorized_cleanup_intent(encoded, STAGED_KIND).status.code ==
          AuthorizedCode::unexpected_marker_kind);
    CHECK(relation::decode_ooc_authorized_cleanup_intent(staged_bytes, INTENT_KIND).status.code ==
          AuthorizedCode::unexpected_marker_kind);
    changed = encoded;
    store_u32_le(changed, 28, static_cast<std::uint32_t>(STAGED_KIND));
    CHECK(relation::decode_ooc_authorized_cleanup_intent(changed, STAGED_KIND).status.code ==
          AuthorizedCode::digest_mismatch);

    changed = encoded;
    changed.back() ^= std::byte{0x80};
    const auto digest_mismatch =
        relation::decode_ooc_authorized_cleanup_intent(changed, INTENT_KIND);
    CHECK(digest_mismatch.status.code == AuthorizedCode::digest_mismatch);
    CHECK(digest_mismatch.status.byte_offset == 448);

    const auto legacy_bytes = encode_or_fail(make_sealed_record());
    CHECK(relation::decode_ooc_authorized_cleanup_intent(legacy_bytes, INTENT_KIND).status.code ==
          AuthorizedCode::truncated);
    CHECK(relation::decode_ooc_private_handoff_record(encoded).status.code == Code::invalid_magic);
}

using TestFunction = void (*)();

void run_suite(std::string_view suite) {
    const std::array<std::pair<std::string_view, TestFunction>, 7> core_tests = {{
        {"protocol constants and closed names", test_protocol_constants_and_closed_names},
        {"sealed roundtrip and deterministic fixture", test_sealed_round_trip_and_determinism},
        {"little-endian wire fields", test_little_endian_wire_fields},
        {"zero-row and payload boundary", test_zero_row_and_payload_boundary},
        {"authorized intent constants and roundtrip",
         test_authorized_intent_constants_and_roundtrip},
        {"authorized intent little-endian fields", test_authorized_intent_little_endian_fields},
        {"authorized intent zero digest and zero row",
         test_authorized_intent_zero_digest_and_zero_row},
    }};
    const std::array<std::pair<std::string_view, TestFunction>, 9> negative_tests = {{
        {"binding-field digest drift", test_every_binding_field_affects_self_digest},
        {"invalid versions, platform, identities, and values",
         test_invalid_versions_platform_identities_and_values},
        {"extent formula and overflow", test_extent_formula_and_overflow_rejection},
        {"payload and self digest rejection", test_payload_and_self_digest_rejection},
        {"wire framing, magic, version, and unknown platform",
         test_wire_framing_magic_versions_and_unknown_platform},
        {"result invariants", test_result_invariants},
        {"authorized intent binding digest", test_authorized_intent_binding_digest},
        {"authorized intent invalid bindings", test_authorized_intent_invalid_bindings},
        {"authorized intent wire rejection and codec separation",
         test_authorized_intent_wire_rejection_and_codec_separation},
    }};

    const auto run = [](std::string_view heading, const auto& tests) {
        std::cout << "===== " << heading << " =====\n";
        for (const auto& [name, function] : tests) {
            function();
            std::cout << "  " << name << ": PASS\n";
        }
    };

    if (suite == "core" || suite == "all") {
        run("OOC Durable Handoff Core Tests", core_tests);
    }
    if (suite == "negative" || suite == "all") {
        run("OOC Durable Handoff Negative Tests", negative_tests);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string_view suite = "all";
    if (argc == 3 && std::string_view(argv[1]) == "--suite") {
        suite = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--suite core|negative]\n";
        return 2;
    }
    if (suite != "all" && suite != "core" && suite != "negative") {
        std::cerr << "usage: " << argv[0] << " [--suite core|negative]\n";
        return 2;
    }

    try {
        run_suite(suite);
        std::cout << "===== OOC Durable Handoff Tests PASSED =====\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
