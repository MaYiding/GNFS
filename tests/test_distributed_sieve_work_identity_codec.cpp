#include "distributed_sieve_work_identity_codec_internal.hpp"

#include <gnfs/util/sha256.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace sieve = gnfs::sieve;
namespace codec = gnfs::sieve::distributed_sieve_work_identity_codec_detail;

using Digest = gnfs::util::Sha256Digest;
using Identity = sieve::DistributedSieveWorkIdentityV1;

constexpr std::string_view WORK_DIGEST_DOMAIN = "GNFS-DISTRIBUTED-SIEVE-WORK-V1";
constexpr std::string_view EXPECTED_WORK_DIGEST_HEX =
    "309f23d06c9d0fca261661e4cc90ca3d3ca8424e776a5cb7ccf77bea3056dd63";
constexpr std::size_t EXPECTED_BODY_BYTES = 1015;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            throw std::runtime_error(std::string("CHECK failed: " #condition " at ") + __FILE__ +  \
                                     ":" + std::to_string(__LINE__));                              \
        }                                                                                          \
    } while (false)

[[nodiscard]] constexpr std::uint64_t binary64_bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

struct PolicySpec final {
    sieve::ExecutionPolicyScalarKindV1 kind;
    std::uint64_t bits;
};

constexpr std::array<PolicySpec, sieve::DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1>
    POLICY_SPECS{{
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.5)},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 123},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::ieee754_binary64, binary64_bits(0.125)},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 12},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 8},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 64},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 16},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 128},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 2},
        {sieve::ExecutionPolicyScalarKindV1::boolean, 1},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4},
        {sieve::ExecutionPolicyScalarKindV1::unsigned_integer, 4},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
        {sieve::ExecutionPolicyScalarKindV1::closed_mode, 1},
    }};

[[nodiscard]] sieve::DistributedSieveExecutionPolicyV1 make_execution_policy() {
    sieve::DistributedSieveExecutionPolicyV1 policy;
    policy.settings.reserve(POLICY_SPECS.size());
    for (std::size_t index = 0; index < POLICY_SPECS.size(); ++index) {
        policy.settings.push_back({
            static_cast<sieve::ExecutionPolicyKeyV1>(index + 1U),
            POLICY_SPECS[index].kind,
            POLICY_SPECS[index].bits,
        });
    }
    return policy;
}

[[nodiscard]] Identity make_identity() {
    Identity identity;
    identity.polynomial.n.decimal = "1000036000099";
    identity.polynomial.m.decimal = "10001";
    identity.polynomial.degree = 2;
    identity.polynomial.coefficients = {{"-5"}, {"3"}, {"1"}};
    identity.polynomial.skewness_ieee754_bits = binary64_bits(1.25);

    identity.factor_base.rational_bound = 100;
    identity.factor_base.algebraic_bound = 200;
    identity.factor_base.large_prime_bound = 10'000;
    identity.factor_base.log_scale = 16;
    identity.factor_base.rational = {{2, 16}, {5, 25}};
    identity.factor_base.algebraic = {
        {7, 1, 37, 1},   {11, 4, 55, 2},
        {211, 3, 61, 1}, {223, std::numeric_limits<std::uint32_t>::max(), 67, 1},
        {227, 5, 71, 1},
    };
    identity.factor_base.sieve_algebraic_count = 2;

    identity.sieve = {16, 50, 51, 10'000, true, false};
    identity.region = {-100, 100, 1, 50};
    identity.cofactor = {10'000, true, false, true, 20};
    identity.original_sq_bounds = {0, 5, 100, 1000};
    identity.effective_sq_bounds = {2, 5, 0, std::numeric_limits<std::uint32_t>::max()};

    identity.distributed.worker_count = 2;
    identity.distributed.chunks = {
        {0, 2, 3, "chunk_0"},
        {1, 3, 5, "chunk_1"},
    };
    identity.distributed.sq_cap_per_worker = 10;
    identity.distributed.relation_cap_per_worker = 100;
    // Same-width neighbors intentionally differ so a field-order regression
    // cannot preserve this fixture's golden digest.
    identity.distributed.max_worker_attempts = 2;
    identity.distributed.max_merge_build_attempts = 3;
    identity.distributed.max_consumption_attempts = 4;
    identity.execution_policy = make_execution_policy();
    identity.semantic_versions = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(sieve::validate_distributed_sieve_work_identity(identity));
    return identity;
}

class OracleWriter final {
public:
    void put_u8(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void put_u16(std::uint16_t value) {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_i64(std::int64_t value) {
        put_u64(std::bit_cast<std::uint64_t>(value));
    }

    void put_bool(bool value) {
        put_u8(value ? 1U : 0U);
    }

    void put_string(std::string_view value) {
        put_u32(static_cast<std::uint32_t>(value.size()));
        for (const char character : value) {
            put_u8(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
        }
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

private:
    std::vector<std::byte> bytes_;
};

void oracle_put_bounds(OracleWriter& writer, const sieve::SpecialQBoundsV1& bounds) {
    writer.put_u32(bounds.start_index);
    writer.put_u32(bounds.end_index);
    writer.put_u64(bounds.min_q);
    writer.put_u64(bounds.max_q);
}

[[nodiscard]] std::vector<std::byte> oracle_body(const Identity& value) {
    OracleWriter writer;
    writer.put_u32(sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1);

    writer.put_u8(0x01);
    writer.put_string(value.polynomial.n.decimal);
    writer.put_string(value.polynomial.m.decimal);
    writer.put_u32(value.polynomial.degree);
    writer.put_u32(static_cast<std::uint32_t>(value.polynomial.coefficients.size()));
    for (std::uint32_t index = 0; index < value.polynomial.coefficients.size(); ++index) {
        writer.put_u32(index);
        writer.put_string(value.polynomial.coefficients[index].decimal);
    }
    writer.put_u64(value.polynomial.skewness_ieee754_bits);

    writer.put_u8(0x02);
    writer.put_u64(value.factor_base.rational_bound);
    writer.put_u64(value.factor_base.algebraic_bound);
    writer.put_u64(value.factor_base.large_prime_bound);
    writer.put_u32(value.factor_base.log_scale);
    writer.put_u32(static_cast<std::uint32_t>(value.factor_base.rational.size()));
    for (std::uint32_t index = 0; index < value.factor_base.rational.size(); ++index) {
        writer.put_u32(index);
        writer.put_u64(value.factor_base.rational[index].p);
        writer.put_u32(value.factor_base.rational[index].log_p);
    }
    writer.put_u32(static_cast<std::uint32_t>(value.factor_base.algebraic.size()));
    for (std::uint32_t index = 0; index < value.factor_base.algebraic.size(); ++index) {
        const auto& entry = value.factor_base.algebraic[index];
        writer.put_u32(index);
        writer.put_u64(entry.p);
        writer.put_u64(entry.r);
        writer.put_u32(entry.log_p);
        writer.put_u32(entry.degree);
    }
    writer.put_u64(value.factor_base.sieve_algebraic_count);

    writer.put_u8(0x03);
    writer.put_u32(value.sieve.log_scale);
    writer.put_u16(value.sieve.rational_threshold);
    writer.put_u16(value.sieve.algebraic_threshold);
    writer.put_u64(value.sieve.large_prime_bound);
    writer.put_bool(value.sieve.allow_2lp);
    writer.put_bool(value.sieve.allow_3lp);

    writer.put_u8(0x04);
    writer.put_i64(value.region.i_min);
    writer.put_i64(value.region.i_max);
    writer.put_i64(value.region.j_min);
    writer.put_i64(value.region.j_max);

    writer.put_u8(0x05);
    writer.put_u64(value.cofactor.large_prime_bound);
    writer.put_bool(value.cofactor.allow_1lp);
    writer.put_bool(value.cofactor.allow_2lp);
    writer.put_bool(value.cofactor.allow_3lp);
    writer.put_u64(value.cofactor.max_factorization_attempts);

    writer.put_u8(0x06);
    oracle_put_bounds(writer, value.original_sq_bounds);
    oracle_put_bounds(writer, value.effective_sq_bounds);

    writer.put_u8(0x07);
    writer.put_u32(value.distributed.worker_count);
    writer.put_u32(static_cast<std::uint32_t>(value.distributed.chunks.size()));
    for (std::uint32_t index = 0; index < value.distributed.chunks.size(); ++index) {
        const auto& chunk = value.distributed.chunks[index];
        writer.put_u32(index);
        writer.put_u32(chunk.chunk_id);
        writer.put_u32(chunk.sq_begin);
        writer.put_u32(chunk.sq_end);
        writer.put_string(chunk.relative_artifact_stem);
    }
    writer.put_u64(value.distributed.sq_cap_per_worker);
    writer.put_u64(value.distributed.relation_cap_per_worker);
    writer.put_u32(value.distributed.max_worker_attempts);
    writer.put_u32(value.distributed.max_merge_build_attempts);
    writer.put_u32(value.distributed.max_consumption_attempts);

    writer.put_u8(0x08);
    writer.put_u32(value.execution_policy.schema_version);
    writer.put_u32(static_cast<std::uint32_t>(value.execution_policy.settings.size()));
    for (std::uint32_t index = 0; index < value.execution_policy.settings.size(); ++index) {
        const auto& setting = value.execution_policy.settings[index];
        writer.put_u32(index);
        writer.put_u16(static_cast<std::uint16_t>(setting.key));
        writer.put_u8(static_cast<std::uint8_t>(setting.kind));
        writer.put_u64(setting.canonical_bits);
    }

    writer.put_u8(0x09);
    writer.put_u32(value.semantic_versions.relation_serialization_version);
    writer.put_u32(value.semantic_versions.ooc_format_version);
    writer.put_u32(value.semantic_versions.digest_version);
    writer.put_u32(value.semantic_versions.handoff_version);
    writer.put_u32(value.semantic_versions.retry_policy_version);
    writer.put_u32(value.semantic_versions.chunking_version);
    writer.put_u32(value.semantic_versions.completion_version);
    writer.put_u32(value.semantic_versions.deduplication_version);
    writer.put_u32(value.semantic_versions.merge_policy_version);
    return writer.bytes();
}

[[nodiscard]] std::string digest_hex(const Digest& digest) {
    const auto encoded = gnfs::util::encode_sha256_hex(digest);
    return {encoded.begin(), encoded.end()};
}

[[nodiscard]] Digest hash_work_body(std::span<const std::byte> body) {
    gnfs::util::Sha256Accumulator accumulator;
    constexpr std::array<std::byte, 1> SEPARATOR{std::byte{0}};
    CHECK(accumulator.update(WORK_DIGEST_DOMAIN));
    CHECK(accumulator.update(SEPARATOR));
    CHECK(accumulator.update(body));
    const auto digest = accumulator.finalize();
    CHECK(digest.has_value());
    return *digest;
}

class CountingProbeSink {
public:
    void put_u8(std::uint8_t) noexcept {
        size_ += 1;
    }
    void put_u16(std::uint16_t) noexcept {
        size_ += 2;
    }
    void put_u32(std::uint32_t) noexcept {
        size_ += 4;
    }
    void put_u64(std::uint64_t) noexcept {
        size_ += 8;
    }
    void put_i64(std::int64_t) noexcept {
        size_ += 8;
    }
    void put_bool(bool) noexcept {
        size_ += 1;
    }
    void put_string(const std::string& value) noexcept {
        size_ += 4 + value.size();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr bool good() const noexcept {
        return true;
    }

private:
    std::size_t size_ = 0;
};

class FailingProbeSink final {
public:
    void put_u8(std::uint8_t) noexcept {
        fail();
    }
    void put_u16(std::uint16_t) noexcept {
        fail();
    }
    void put_u32(std::uint32_t) noexcept {
        fail();
    }
    void put_u64(std::uint64_t) noexcept {
        fail();
    }
    void put_i64(std::int64_t) noexcept {
        fail();
    }
    void put_bool(bool) noexcept {
        fail();
    }
    void put_string(const std::string&) noexcept {
        fail();
    }

    [[nodiscard]] bool good() const noexcept {
        return good_;
    }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_;
    }

private:
    void fail() noexcept {
        ++calls_;
        good_ = false;
    }

    bool good_ = true;
    std::size_t calls_ = 0;
};

class ThrowingGoodProbeSink final : public CountingProbeSink {
public:
    [[nodiscard]] bool good() const {
        return true;
    }
};

static_assert(codec::DistributedSieveWorkIdentityCanonicalSinkV1<CountingProbeSink>);
static_assert(codec::DistributedSieveWorkIdentityCanonicalSinkV1<FailingProbeSink>);
static_assert(!codec::DistributedSieveWorkIdentityCanonicalSinkV1<ThrowingGoodProbeSink>);

void put_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= 4);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift));
    }
}

void require_decode_error(
    std::span<const std::byte> bytes, sieve::DistributedSieveProtocolError expected_error,
    std::uint64_t expected_offset,
    std::uint32_t expected_index = sieve::DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) {
    const auto decoded = codec::decode_distributed_sieve_work_identity_v1(bytes);
    CHECK(!decoded);
    CHECK(!decoded.identity.has_value());
    CHECK(decoded.status.error == expected_error);
    CHECK(decoded.status.byte_offset == expected_offset);
    CHECK(decoded.status.element_index == expected_index);
}

void test_golden_digest_and_roundtrip() {
    const Identity identity = make_identity();
    const auto encoded = codec::encode_distributed_sieve_work_identity_v1(identity);
    CHECK(encoded);
    CHECK(encoded.bytes->size() == EXPECTED_BODY_BYTES);
    // A single boolean fixture cannot give three fields pairwise-distinct
    // values. Freeze their exact wire positions with two complementary rows.
    CHECK(std::to_integer<std::uint8_t>((*encoded.bytes)[353]) == 1);
    CHECK(std::to_integer<std::uint8_t>((*encoded.bytes)[354]) == 0);
    CHECK(std::to_integer<std::uint8_t>((*encoded.bytes)[355]) == 1);

    auto complementary_cofactor_flags = identity;
    complementary_cofactor_flags.cofactor.allow_1lp = false;
    complementary_cofactor_flags.cofactor.allow_2lp = true;
    complementary_cofactor_flags.cofactor.allow_3lp = true;
    const auto complementary =
        codec::encode_distributed_sieve_work_identity_v1(complementary_cofactor_flags);
    CHECK(complementary);
    CHECK(std::to_integer<std::uint8_t>((*complementary.bytes)[353]) == 0);
    CHECK(std::to_integer<std::uint8_t>((*complementary.bytes)[354]) == 1);
    CHECK(std::to_integer<std::uint8_t>((*complementary.bytes)[355]) == 1);

    const auto independent = oracle_body(identity);
    CHECK(independent.size() == EXPECTED_BODY_BYTES);
    CHECK(*encoded.bytes == independent);

    const Digest body_digest = hash_work_body(*encoded.bytes);
    const std::string actual_digest_hex = digest_hex(body_digest);
    if (actual_digest_hex != EXPECTED_WORK_DIGEST_HEX) {
        throw std::runtime_error("work identity golden mismatch: expected " +
                                 std::string(EXPECTED_WORK_DIGEST_HEX) + ", actual " +
                                 actual_digest_hex);
    }
    const auto production_digest = sieve::distributed_sieve_work_digest(identity);
    CHECK(production_digest);
    CHECK(*production_digest.digest == body_digest);

    CountingProbeSink probe;
    CHECK(codec::emit_distributed_sieve_work_identity_v1(probe, identity));
    CHECK(probe.size() == encoded.bytes->size());

    FailingProbeSink failing_probe;
    CHECK(!codec::emit_distributed_sieve_work_identity_v1(failing_probe, identity));
    CHECK(failing_probe.calls() == 1);

    const auto decoded = codec::decode_distributed_sieve_work_identity_v1(*encoded.bytes);
    CHECK(decoded);
    const auto reencoded = codec::encode_distributed_sieve_work_identity_v1(*decoded.identity);
    CHECK(reencoded);
    CHECK(*reencoded.bytes == *encoded.bytes);
    const auto decoded_digest = sieve::distributed_sieve_work_digest(*decoded.identity);
    CHECK(decoded_digest);
    CHECK(*decoded_digest.digest == body_digest);
}

void test_all_exact_prefix_truncations_fail_closed() {
    const auto encoded = codec::encode_distributed_sieve_work_identity_v1(make_identity());
    CHECK(encoded);
    for (std::size_t size = 0; size < encoded.bytes->size(); ++size) {
        require_decode_error(std::span<const std::byte>(*encoded.bytes).first(size),
                             sieve::DistributedSieveProtocolError::truncated, size);
    }
}

void test_trailing_schema_tag_and_sequence_bounds() {
    const auto encoded = codec::encode_distributed_sieve_work_identity_v1(make_identity());
    CHECK(encoded);

    auto trailing = *encoded.bytes;
    trailing.push_back(std::byte{0});
    require_decode_error(trailing, sieve::DistributedSieveProtocolError::trailing_bytes,
                         encoded.bytes->size());

    auto schema = *encoded.bytes;
    put_u32(schema, 0, sieve::DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1 + 1U);
    require_decode_error(schema, sieve::DistributedSieveProtocolError::unsupported_schema_version,
                         0);

    auto tag = *encoded.bytes;
    tag[4] = std::byte{0x7f};
    require_decode_error(tag, sieve::DistributedSieveProtocolError::invalid_value, 4);

    // Body offsets for the frozen baseline:
    // schema(4), tag(1), n length at 5, n bytes(13), m length+bytes(9),
    // degree at 31, coefficient count at 35, first ordinal at 39.
    auto oversized_string = *encoded.bytes;
    put_u32(oversized_string, 5,
            sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES + 1U);
    require_decode_error(oversized_string,
                         sieve::DistributedSieveProtocolError::collection_too_large, 5);

    auto excessive_count = *encoded.bytes;
    put_u32(excessive_count, 35, sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_COEFFICIENTS + 1U);
    require_decode_error(excessive_count,
                         sieve::DistributedSieveProtocolError::collection_too_large, 35);

    auto bounded_before_resize = *encoded.bytes;
    put_u32(bounded_before_resize, 35, sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_COEFFICIENTS);
    require_decode_error(bounded_before_resize, sieve::DistributedSieveProtocolError::truncated,
                         bounded_before_resize.size());

    auto excessive_factor_base_count = *encoded.bytes;
    put_u32(excessive_factor_base_count, 104,
            sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_FACTOR_BASE_ENTRIES + 1U);
    require_decode_error(excessive_factor_base_count,
                         sieve::DistributedSieveProtocolError::collection_too_large, 104);

    auto bounded_factor_base_before_resize = *encoded.bytes;
    put_u32(bounded_factor_base_before_resize, 104,
            sieve::DISTRIBUTED_SIEVE_PROTOCOL_MAX_FACTOR_BASE_ENTRIES);
    require_decode_error(bounded_factor_base_before_resize,
                         sieve::DistributedSieveProtocolError::truncated,
                         bounded_factor_base_before_resize.size());

    auto noncanonical_index = *encoded.bytes;
    put_u32(noncanonical_index, 39, 1);
    require_decode_error(noncanonical_index,
                         sieve::DistributedSieveProtocolError::noncanonical_order, 39, 0);

    auto invalid_boolean = *encoded.bytes;
    invalid_boolean[309] = std::byte{2};
    require_decode_error(invalid_boolean, sieve::DistributedSieveProtocolError::invalid_boolean,
                         309);
}

void test_semantic_validation_and_encode_rejection() {
    auto invalid_identity = make_identity();
    invalid_identity.polynomial.coefficients[0].decimal = "+5";
    const auto encoded = codec::encode_distributed_sieve_work_identity_v1(invalid_identity);
    CHECK(!encoded);
    CHECK(encoded.status.error == sieve::DistributedSieveProtocolError::invalid_string);
    CHECK(encoded.status.element_index == 0);

    const auto valid = codec::encode_distributed_sieve_work_identity_v1(make_identity());
    CHECK(valid);
    auto invalid_wire = *valid.bytes;
    // First coefficient bytes start after schema/tag/n/m/degree/count/index/length.
    invalid_wire[47] = std::byte{'+'};
    const auto decoded = codec::decode_distributed_sieve_work_identity_v1(invalid_wire);
    CHECK(!decoded);
    CHECK(decoded.status.error == sieve::DistributedSieveProtocolError::invalid_string);
    CHECK(decoded.status.element_index == 0);
}

} // namespace

int main() {
    try {
        static_assert(codec::DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_STRUCTURAL_MAX_BODY_BYTES ==
                      UINT64_C(739266535));
        static_assert(codec::DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES ==
                      UINT64_C(739266524));
        test_golden_digest_and_roundtrip();
        test_all_exact_prefix_truncations_fail_closed();
        test_trailing_schema_tag_and_sequence_bounds();
        test_semantic_validation_and_encode_rejection();
        std::cout << "distributed sieve work identity codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
