#include "distributed_sieve_work_package_codec_internal.hpp"

#include <gnfs/util/sha256.hpp>

#include <algorithm>
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
namespace identity_codec = gnfs::sieve::distributed_sieve_work_identity_codec_detail;
namespace package_codec = gnfs::sieve::distributed_sieve_work_package_codec_detail;

using Digest = gnfs::util::Sha256Digest;
using Identity = sieve::DistributedSieveWorkIdentityV1;

constexpr std::string_view WORK_DIGEST_DOMAIN = "GNFS-DISTRIBUTED-SIEVE-WORK-V1";
constexpr std::string_view PACKAGE_DIGEST_DOMAIN = "GNFS-DISTRIBUTED-SIEVE-WORK-PACKAGE-V1";
constexpr std::string_view EXPECTED_WORK_DIGEST_HEX =
    "309f23d06c9d0fca261661e4cc90ca3d3ca8424e776a5cb7ccf77bea3056dd63";
constexpr std::string_view EXPECTED_PACKAGE_DIGEST_HEX =
    "b9663c3891f5f96477ad7d67ea395c52c2ddcf44518a074a2d1805504d47a6f1";
constexpr std::size_t EXPECTED_BODY_BYTES = 1015;
constexpr std::size_t EXPECTED_PACKAGE_BYTES = 1127;
constexpr std::size_t HEADER_BYTES = package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1;
constexpr std::size_t TRAILER_BYTES =
    package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1;
constexpr std::size_t MAGIC_OFFSET = package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_OFFSET_V1;
constexpr std::size_t WIRE_VERSION_OFFSET =
    package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_OFFSET_V1;
constexpr std::size_t WORK_SCHEMA_OFFSET =
    package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SCHEMA_OFFSET_V1;
constexpr std::size_t HEADER_BYTES_OFFSET =
    package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_OFFSET_V1;
constexpr std::size_t TRAILER_BYTES_OFFSET =
    package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_OFFSET_V1;
constexpr std::size_t BODY_BYTES_OFFSET =
    package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1;
constexpr std::size_t TOTAL_BYTES_OFFSET =
    package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_TOTAL_BYTES_OFFSET_V1;
constexpr std::size_t WORK_SHA256_OFFSET =
    package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SHA256_OFFSET_V1;
constexpr std::size_t RESERVED_OFFSET =
    package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_RESERVED_OFFSET_V1;

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
    identity.effective_sq_bounds = {
        2,
        5,
        0,
        std::numeric_limits<std::uint32_t>::max(),
    };

    identity.distributed.worker_count = 2;
    identity.distributed.chunks = {
        {0, 2, 3, "chunk_0"},
        {1, 3, 5, "chunk_1"},
    };
    identity.distributed.sq_cap_per_worker = 10;
    identity.distributed.relation_cap_per_worker = 100;
    identity.distributed.max_worker_attempts = 2;
    identity.distributed.max_merge_build_attempts = 3;
    identity.distributed.max_consumption_attempts = 4;
    identity.execution_policy = make_execution_policy();
    identity.semantic_versions = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(sieve::validate_distributed_sieve_work_identity(identity));
    return identity;
}

[[nodiscard]] Digest hash_domain_and_bytes(std::string_view domain,
                                           std::span<const std::byte> bytes) {
    gnfs::util::Sha256Accumulator accumulator;
    constexpr std::array<std::byte, 1> SEPARATOR{std::byte{0}};
    CHECK(accumulator.update(domain));
    CHECK(accumulator.update(SEPARATOR));
    CHECK(accumulator.update(bytes));
    const auto digest = accumulator.finalize();
    CHECK(digest.has_value());
    return *digest;
}

[[nodiscard]] std::string digest_hex(const Digest& digest) {
    const auto encoded = gnfs::util::encode_sha256_hex(digest);
    return {encoded.begin(), encoded.end()};
}

void oracle_store_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= 4);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift));
    }
}

void oracle_store_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= 8);
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes[offset++] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift));
    }
}

[[nodiscard]] std::uint32_t oracle_load_u32(std::span<const std::byte> bytes, std::size_t offset) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= 4);
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset++]))
                 << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t oracle_load_u64(std::span<const std::byte> bytes, std::size_t offset) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= 8);
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset++]))
                 << shift;
    }
    return value;
}

void oracle_store_digest(std::span<std::byte> bytes, std::size_t offset, const Digest& digest) {
    CHECK(offset <= bytes.size());
    CHECK(bytes.size() - offset >= digest.bytes.size());
    std::copy(digest.bytes.begin(), digest.bytes.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

struct OraclePackage final {
    std::vector<std::byte> bytes;
    Digest work_digest;
    Digest package_digest;
};

[[nodiscard]] OraclePackage make_oracle_package(const Identity& identity) {
    // This oracle independently constructs the package envelope. The
    // canonical identity body it encloses is separately frozen by the
    // identity codec's independent body oracle and work-digest golden.
    const auto encoded_body = identity_codec::encode_distributed_sieve_work_identity_v1(identity);
    CHECK(encoded_body);
    const auto& body = *encoded_body.bytes;
    const Digest work_digest = hash_domain_and_bytes(WORK_DIGEST_DOMAIN, body);

    std::array<std::byte, HEADER_BYTES> header{};
    constexpr std::array<char, 8> MAGIC{
        'G', 'N', 'F', 'S', 'D', 'W', 'P', '1',
    };
    for (std::size_t index = 0; index < MAGIC.size(); ++index) {
        header[MAGIC_OFFSET + index] = static_cast<std::byte>(
            static_cast<std::uint8_t>(static_cast<unsigned char>(MAGIC[index])));
    }
    oracle_store_u32(header, WIRE_VERSION_OFFSET, 1);
    oracle_store_u32(header, WORK_SCHEMA_OFFSET, 1);
    oracle_store_u32(header, HEADER_BYTES_OFFSET, HEADER_BYTES);
    oracle_store_u32(header, TRAILER_BYTES_OFFSET, TRAILER_BYTES);
    oracle_store_u64(header, BODY_BYTES_OFFSET, body.size());
    oracle_store_u64(header, TOTAL_BYTES_OFFSET, HEADER_BYTES + body.size() + TRAILER_BYTES);
    oracle_store_digest(header, WORK_SHA256_OFFSET, work_digest);
    oracle_store_u64(header, RESERVED_OFFSET, 0);

    std::vector<std::byte> bytes;
    bytes.reserve(HEADER_BYTES + body.size() + TRAILER_BYTES);
    bytes.insert(bytes.end(), header.begin(), header.end());
    bytes.insert(bytes.end(), body.begin(), body.end());
    const Digest package_digest = hash_domain_and_bytes(PACKAGE_DIGEST_DOMAIN, bytes);
    bytes.insert(bytes.end(), package_digest.bytes.begin(), package_digest.bytes.end());
    return {std::move(bytes), work_digest, package_digest};
}

void refresh_package_digest(std::vector<std::byte>& bytes) {
    CHECK(bytes.size() >= TRAILER_BYTES);
    const std::size_t trailer_offset = bytes.size() - TRAILER_BYTES;
    const Digest digest = hash_domain_and_bytes(
        PACKAGE_DIGEST_DOMAIN, std::span<const std::byte>(bytes).first(trailer_offset));
    oracle_store_digest(bytes, trailer_offset, digest);
}

void refresh_work_and_package_digests(std::vector<std::byte>& bytes) {
    CHECK(bytes.size() >= HEADER_BYTES + TRAILER_BYTES);
    const std::size_t body_bytes = bytes.size() - HEADER_BYTES - TRAILER_BYTES;
    const Digest work_digest = hash_domain_and_bytes(
        WORK_DIGEST_DOMAIN, std::span<const std::byte>(bytes).subspan(HEADER_BYTES, body_bytes));
    oracle_store_digest(bytes, WORK_SHA256_OFFSET, work_digest);
    refresh_package_digest(bytes);
}

class VectorProbeSink final {
public:
    void put_bytes(std::span<const std::byte> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] constexpr bool good() const noexcept {
        return true;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

private:
    std::vector<std::byte> bytes_;
};

class FailingByteSink final {
public:
    explicit FailingByteSink(std::size_t byte_limit) noexcept : byte_limit_(byte_limit) {}

    void put_bytes(std::span<const std::byte> bytes) {
        ++calls_;
        if (!good_) {
            ++calls_after_failure_;
            return;
        }
        if (bytes.size() > byte_limit_ - bytes_.size()) {
            failed_put_bytes_ = bytes.size();
            good_ = false;
            return;
        }
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] bool good() const noexcept {
        return good_;
    }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_;
    }

    [[nodiscard]] std::size_t calls_after_failure() const noexcept {
        return calls_after_failure_;
    }

    [[nodiscard]] std::size_t failed_put_bytes() const noexcept {
        return failed_put_bytes_;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

private:
    std::size_t byte_limit_;
    std::vector<std::byte> bytes_;
    std::size_t calls_ = 0;
    std::size_t calls_after_failure_ = 0;
    std::size_t failed_put_bytes_ = 0;
    bool good_ = true;
};

class PrefixFailingByteSink final {
public:
    explicit PrefixFailingByteSink(std::size_t failure_offset) noexcept
        : failure_offset_(failure_offset) {}

    void put_bytes(std::span<const std::byte> bytes) {
        ++calls_;
        if (!good_) {
            ++calls_after_failure_;
            return;
        }
        if (bytes_.size() >= failure_offset_) {
            failed_put_bytes_ = bytes.size();
            good_ = false;
            return;
        }
        const std::size_t remaining = failure_offset_ - bytes_.size();
        if (bytes.size() <= remaining) {
            bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
            return;
        }
        const auto prefix = bytes.first(remaining);
        bytes_.insert(bytes_.end(), prefix.begin(), prefix.end());
        failed_put_bytes_ = bytes.size();
        good_ = false;
    }

    [[nodiscard]] bool good() const noexcept {
        return good_;
    }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_;
    }

    [[nodiscard]] std::size_t calls_after_failure() const noexcept {
        return calls_after_failure_;
    }

    [[nodiscard]] std::size_t failed_put_bytes() const noexcept {
        return failed_put_bytes_;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

private:
    std::size_t failure_offset_;
    std::vector<std::byte> bytes_;
    std::size_t calls_ = 0;
    std::size_t calls_after_failure_ = 0;
    std::size_t failed_put_bytes_ = 0;
    bool good_ = true;
};

class ThrowingGoodSink final {
public:
    void put_bytes(std::span<const std::byte>) {}

    [[nodiscard]] bool good() const {
        return true;
    }
};

static_assert(package_codec::DistributedSieveWorkPackageByteSinkV1<VectorProbeSink>);
static_assert(package_codec::DistributedSieveWorkPackageByteSinkV1<FailingByteSink>);
static_assert(package_codec::DistributedSieveWorkPackageByteSinkV1<PrefixFailingByteSink>);
static_assert(!package_codec::DistributedSieveWorkPackageByteSinkV1<ThrowingGoodSink>);

void require_decode_error(
    std::span<const std::byte> bytes, sieve::DistributedSieveProtocolError expected_error,
    std::uint64_t expected_offset,
    std::uint32_t expected_index = sieve::DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) {
    const auto decoded = package_codec::decode_distributed_sieve_work_package_v1(bytes);
    CHECK(!decoded);
    CHECK(!decoded.package.has_value());
    CHECK(decoded.status.error == expected_error);
    CHECK(decoded.status.byte_offset == expected_offset);
    CHECK(decoded.status.element_index == expected_index);
}

void test_golden_oracle_and_roundtrip() {
    const Identity identity = make_identity();
    const auto oracle = make_oracle_package(identity);
    CHECK(oracle.bytes.size() == EXPECTED_PACKAGE_BYTES);
    CHECK(oracle_load_u64(oracle.bytes, BODY_BYTES_OFFSET) == EXPECTED_BODY_BYTES);
    CHECK(digest_hex(oracle.work_digest) == EXPECTED_WORK_DIGEST_HEX);

    const auto prepared = package_codec::prepare_distributed_sieve_work_package_v1(identity);
    CHECK(prepared);
    CHECK(prepared.prepared->body_bytes == EXPECTED_BODY_BYTES);
    CHECK(prepared.prepared->total_bytes == EXPECTED_PACKAGE_BYTES);
    CHECK(prepared.prepared->work_sha256 == oracle.work_digest);

    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);
    CHECK(encoded.package->bytes == oracle.bytes);
    CHECK(encoded.package->witness.body_bytes == EXPECTED_BODY_BYTES);
    CHECK(encoded.package->witness.total_bytes == EXPECTED_PACKAGE_BYTES);
    CHECK(encoded.package->witness.work_sha256 == oracle.work_digest);
    CHECK(encoded.package->witness.package_sha256 == oracle.package_digest);

    const std::string actual_package_digest_hex = digest_hex(oracle.package_digest);
    if (actual_package_digest_hex != EXPECTED_PACKAGE_DIGEST_HEX) {
        throw std::runtime_error("work package trailer golden mismatch: expected " +
                                 std::string(EXPECTED_PACKAGE_DIGEST_HEX) + ", actual " +
                                 actual_package_digest_hex);
    }

    const auto& bytes = encoded.package->bytes;
    CHECK(std::to_integer<std::uint8_t>(bytes[MAGIC_OFFSET]) == 'G');
    CHECK(std::to_integer<std::uint8_t>(bytes[MAGIC_OFFSET + 7]) == '1');
    CHECK(oracle_load_u32(bytes, WIRE_VERSION_OFFSET) == 1);
    CHECK(oracle_load_u32(bytes, WORK_SCHEMA_OFFSET) == 1);
    CHECK(oracle_load_u32(bytes, HEADER_BYTES_OFFSET) == HEADER_BYTES);
    CHECK(oracle_load_u32(bytes, TRAILER_BYTES_OFFSET) == TRAILER_BYTES);
    CHECK(oracle_load_u64(bytes, BODY_BYTES_OFFSET) == EXPECTED_BODY_BYTES);
    CHECK(oracle_load_u64(bytes, TOTAL_BYTES_OFFSET) == EXPECTED_PACKAGE_BYTES);
    CHECK(oracle_load_u64(bytes, RESERVED_OFFSET) == 0);
    CHECK(std::to_integer<std::uint8_t>(bytes[HEADER_BYTES + 353]) == 1);
    CHECK(std::to_integer<std::uint8_t>(bytes[HEADER_BYTES + 354]) == 0);
    CHECK(std::to_integer<std::uint8_t>(bytes[HEADER_BYTES + 355]) == 1);

    const auto decoded = package_codec::decode_distributed_sieve_work_package_v1(bytes);
    CHECK(decoded);
    CHECK(decoded.package->witness.body_bytes == EXPECTED_BODY_BYTES);
    CHECK(decoded.package->witness.total_bytes == EXPECTED_PACKAGE_BYTES);
    CHECK(decoded.package->witness.work_sha256 == oracle.work_digest);
    CHECK(decoded.package->witness.package_sha256 == oracle.package_digest);
    CHECK(decoded.package->identity.cofactor.allow_1lp);
    CHECK(!decoded.package->identity.cofactor.allow_2lp);
    CHECK(decoded.package->identity.cofactor.allow_3lp);
    CHECK(decoded.package->identity.distributed.max_worker_attempts == 2);
    CHECK(decoded.package->identity.distributed.max_merge_build_attempts == 3);
    CHECK(decoded.package->identity.distributed.max_consumption_attempts == 4);
    CHECK(decoded.package->identity.semantic_versions.relation_serialization_version == 1);
    CHECK(decoded.package->identity.semantic_versions.merge_policy_version == 9);

    const auto reencoded_identity =
        identity_codec::encode_distributed_sieve_work_identity_v1(decoded.package->identity);
    CHECK(reencoded_identity);
    CHECK(reencoded_identity.bytes->size() == EXPECTED_BODY_BYTES);
    CHECK(std::equal(reencoded_identity.bytes->begin(), reencoded_identity.bytes->end(),
                     bytes.begin() + HEADER_BYTES));
}

void test_all_exact_prefix_truncations_and_trailing_bytes() {
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(make_identity());
    CHECK(encoded);
    const auto& bytes = encoded.package->bytes;
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        require_decode_error(std::span<const std::byte>(bytes).first(size),
                             sieve::DistributedSieveProtocolError::truncated, size);
    }

    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    require_decode_error(trailing, sieve::DistributedSieveProtocolError::trailing_bytes,
                         bytes.size());
}

void test_every_header_field_fails_closed() {
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(make_identity());
    CHECK(encoded);
    const auto& baseline = encoded.package->bytes;

    auto magic = baseline;
    magic[MAGIC_OFFSET] ^= std::byte{1};
    require_decode_error(magic, sieve::DistributedSieveProtocolError::invalid_magic, 0);

    auto wire_version = baseline;
    oracle_store_u32(wire_version, WIRE_VERSION_OFFSET, 2);
    require_decode_error(wire_version,
                         sieve::DistributedSieveProtocolError::unsupported_wire_version,
                         WIRE_VERSION_OFFSET);

    auto work_schema = baseline;
    oracle_store_u32(work_schema, WORK_SCHEMA_OFFSET, 2);
    require_decode_error(work_schema,
                         sieve::DistributedSieveProtocolError::unsupported_schema_version,
                         WORK_SCHEMA_OFFSET);

    auto header_bytes = baseline;
    oracle_store_u32(header_bytes, HEADER_BYTES_OFFSET, HEADER_BYTES - 1);
    require_decode_error(header_bytes, sieve::DistributedSieveProtocolError::invalid_value,
                         HEADER_BYTES_OFFSET);

    auto trailer_bytes = baseline;
    oracle_store_u32(trailer_bytes, TRAILER_BYTES_OFFSET, TRAILER_BYTES - 1);
    require_decode_error(trailer_bytes, sieve::DistributedSieveProtocolError::invalid_value,
                         TRAILER_BYTES_OFFSET);

    auto body_bytes = baseline;
    oracle_store_u64(body_bytes, BODY_BYTES_OFFSET,
                     identity_codec::DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES + 1);
    require_decode_error(body_bytes, sieve::DistributedSieveProtocolError::input_too_large,
                         BODY_BYTES_OFFSET);

    auto total_bytes = baseline;
    oracle_store_u64(total_bytes, TOTAL_BYTES_OFFSET, EXPECTED_PACKAGE_BYTES + 1);
    require_decode_error(total_bytes, sieve::DistributedSieveProtocolError::declared_size_mismatch,
                         TOTAL_BYTES_OFFSET);

    auto reserved = baseline;
    oracle_store_u64(reserved, RESERVED_OFFSET, 1);
    require_decode_error(reserved, sieve::DistributedSieveProtocolError::invalid_value,
                         RESERVED_OFFSET);
}

void test_digest_integrity_order_and_body_error_offset() {
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(make_identity());
    CHECK(encoded);
    const auto& baseline = encoded.package->bytes;
    const std::size_t trailer_offset = baseline.size() - TRAILER_BYTES;

    auto package_digest_corruption = baseline;
    package_digest_corruption.back() ^= std::byte{1};
    require_decode_error(package_digest_corruption,
                         sieve::DistributedSieveProtocolError::digest_mismatch, trailer_offset);

    auto work_digest_corruption = baseline;
    work_digest_corruption[WORK_SHA256_OFFSET] ^= std::byte{1};
    refresh_package_digest(work_digest_corruption);
    require_decode_error(work_digest_corruption,
                         sieve::DistributedSieveProtocolError::digest_mismatch, WORK_SHA256_OFFSET);

    constexpr std::size_t FIRST_COEFFICIENT_OFFSET = HEADER_BYTES + 47;
    CHECK(baseline[FIRST_COEFFICIENT_OFFSET] == std::byte{'-'});

    auto integrity_unbound_semantic_corruption = baseline;
    integrity_unbound_semantic_corruption[FIRST_COEFFICIENT_OFFSET] = std::byte{'+'};
    require_decode_error(integrity_unbound_semantic_corruption,
                         sieve::DistributedSieveProtocolError::digest_mismatch, trailer_offset);

    auto integrity_bound_semantic_corruption = baseline;
    integrity_bound_semantic_corruption[FIRST_COEFFICIENT_OFFSET] = std::byte{'+'};
    refresh_work_and_package_digests(integrity_bound_semantic_corruption);
    require_decode_error(integrity_bound_semantic_corruption,
                         sieve::DistributedSieveProtocolError::invalid_string,
                         sieve::DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, 0);

    auto integrity_bound_structural_corruption = baseline;
    constexpr std::size_t BODY_BOOLEAN_OFFSET = 309;
    integrity_bound_structural_corruption[HEADER_BYTES + BODY_BOOLEAN_OFFSET] = std::byte{2};
    refresh_work_and_package_digests(integrity_bound_structural_corruption);
    require_decode_error(integrity_bound_structural_corruption,
                         sieve::DistributedSieveProtocolError::invalid_boolean,
                         HEADER_BYTES + BODY_BOOLEAN_OFFSET);

    constexpr std::size_t BODY_COEFFICIENT_COUNT_OFFSET = 35;
    auto integrity_unbound_huge_count = baseline;
    oracle_store_u32(integrity_unbound_huge_count, HEADER_BYTES + BODY_COEFFICIENT_COUNT_OFFSET,
                     std::numeric_limits<std::uint32_t>::max());
    integrity_unbound_huge_count.back() ^= std::byte{1};
    // Error priority is the allocation-order evidence available through this
    // API: the invalid outer trailer must stop decoding before the forged body
    // count can reach the owning identity decoder.
    require_decode_error(integrity_unbound_huge_count,
                         sieve::DistributedSieveProtocolError::digest_mismatch, trailer_offset);

    auto package_bound_work_unbound_huge_count = baseline;
    oracle_store_u32(package_bound_work_unbound_huge_count,
                     HEADER_BYTES + BODY_COEFFICIENT_COUNT_OFFSET,
                     std::numeric_limits<std::uint32_t>::max());
    refresh_package_digest(package_bound_work_unbound_huge_count);
    require_decode_error(package_bound_work_unbound_huge_count,
                         sieve::DistributedSieveProtocolError::digest_mismatch, WORK_SHA256_OFFSET);

    auto integrity_bound_huge_count = integrity_unbound_huge_count;
    refresh_work_and_package_digests(integrity_bound_huge_count);
    require_decode_error(integrity_bound_huge_count,
                         sieve::DistributedSieveProtocolError::collection_too_large,
                         HEADER_BYTES + BODY_COEFFICIENT_COUNT_OFFSET);
}

void test_sink_failure_and_pass_drift_never_emit_trailer() {
    const Identity identity = make_identity();
    const auto prepared = package_codec::prepare_distributed_sieve_work_package_v1(identity);
    CHECK(prepared);

    auto total_size_mismatch = *prepared.prepared;
    ++total_size_mismatch.total_bytes;
    VectorProbeSink malformed_prepared_sink;
    const auto malformed_prepared = package_codec::emit_distributed_sieve_work_package_v1(
        total_size_mismatch, identity, malformed_prepared_sink);
    CHECK(!malformed_prepared);
    CHECK(!malformed_prepared.trailer_emitted);
    CHECK(malformed_prepared.status.error ==
          sieve::DistributedSieveProtocolError::declared_size_mismatch);
    CHECK(malformed_prepared.status.byte_offset == TOTAL_BYTES_OFFSET);
    CHECK(malformed_prepared.bytes_emitted == 0);
    CHECK(malformed_prepared_sink.bytes().empty());

    VectorProbeSink vector_sink;
    const auto emitted = package_codec::emit_distributed_sieve_work_package_v1(
        *prepared.prepared, identity, vector_sink);
    CHECK(emitted);
    CHECK(emitted.trailer_emitted);
    CHECK(emitted.bytes_emitted == EXPECTED_PACKAGE_BYTES);
    const auto encoded = package_codec::encode_distributed_sieve_work_package_v1(identity);
    CHECK(encoded);
    CHECK(vector_sink.bytes() == encoded.package->bytes);

    FailingByteSink header_failure_sink(HEADER_BYTES - 1);
    const auto header_failure = package_codec::emit_distributed_sieve_work_package_v1(
        *prepared.prepared, identity, header_failure_sink);
    CHECK(!header_failure);
    CHECK(!header_failure.trailer_emitted);
    CHECK(header_failure.status.error == sieve::DistributedSieveProtocolError::output_too_large);
    CHECK(header_failure.bytes_emitted == 0);
    CHECK(header_failure.status.byte_offset == header_failure.bytes_emitted);
    CHECK(!header_failure_sink.good());
    CHECK(header_failure_sink.calls() == 1);
    CHECK(header_failure_sink.calls_after_failure() == 0);
    CHECK(header_failure_sink.failed_put_bytes() == HEADER_BYTES);
    CHECK(header_failure_sink.bytes().empty());

    FailingByteSink failing_sink(HEADER_BYTES + EXPECTED_BODY_BYTES - 1);
    const auto failed = package_codec::emit_distributed_sieve_work_package_v1(
        *prepared.prepared, identity, failing_sink);
    CHECK(!failed);
    CHECK(!failed.trailer_emitted);
    CHECK(failed.status.error == sieve::DistributedSieveProtocolError::output_too_large);
    CHECK(!failing_sink.good());
    CHECK(failing_sink.calls() > 1);
    CHECK(failing_sink.calls_after_failure() == 0);
    CHECK(failing_sink.failed_put_bytes() != 0);
    CHECK(failing_sink.bytes().size() < HEADER_BYTES + EXPECTED_BODY_BYTES);
    CHECK(failed.bytes_emitted == failing_sink.bytes().size());
    CHECK(failed.status.byte_offset == failed.bytes_emitted);

    FailingByteSink trailer_failure_sink(HEADER_BYTES + EXPECTED_BODY_BYTES);
    const auto trailer_failure = package_codec::emit_distributed_sieve_work_package_v1(
        *prepared.prepared, identity, trailer_failure_sink);
    CHECK(!trailer_failure);
    CHECK(!trailer_failure.trailer_emitted);
    CHECK(trailer_failure.status.error == sieve::DistributedSieveProtocolError::output_too_large);
    CHECK(trailer_failure.bytes_emitted == HEADER_BYTES + EXPECTED_BODY_BYTES);
    CHECK(trailer_failure.status.byte_offset == trailer_failure.bytes_emitted);
    CHECK(!trailer_failure_sink.good());
    CHECK(trailer_failure_sink.calls() > 1);
    CHECK(trailer_failure_sink.calls_after_failure() == 0);
    CHECK(trailer_failure_sink.failed_put_bytes() == TRAILER_BYTES);
    CHECK(trailer_failure_sink.bytes().size() == HEADER_BYTES + EXPECTED_BODY_BYTES);

    PrefixFailingByteSink partial_header_sink(17);
    const auto partial_header = package_codec::emit_distributed_sieve_work_package_v1(
        *prepared.prepared, identity, partial_header_sink);
    CHECK(!partial_header);
    CHECK(!partial_header.trailer_emitted);
    CHECK(!partial_header.witness.has_value());
    CHECK(partial_header.status.error == sieve::DistributedSieveProtocolError::output_too_large);
    CHECK(partial_header.status.byte_offset == 0);
    CHECK(partial_header.bytes_emitted == 0);
    CHECK(partial_header_sink.bytes().size() == 17);
    CHECK(partial_header_sink.failed_put_bytes() == HEADER_BYTES);
    CHECK(partial_header_sink.calls() == 1);
    CHECK(partial_header_sink.calls_after_failure() == 0);

    PrefixFailingByteSink partial_body_sink(HEADER_BYTES + 2);
    const auto partial_body = package_codec::emit_distributed_sieve_work_package_v1(
        *prepared.prepared, identity, partial_body_sink);
    CHECK(!partial_body);
    CHECK(!partial_body.trailer_emitted);
    CHECK(!partial_body.witness.has_value());
    CHECK(partial_body.status.error == sieve::DistributedSieveProtocolError::output_too_large);
    CHECK(partial_body.status.byte_offset == HEADER_BYTES);
    CHECK(partial_body.bytes_emitted == HEADER_BYTES);
    CHECK(partial_body_sink.bytes().size() == HEADER_BYTES + 2);
    CHECK(partial_body_sink.failed_put_bytes() == 4);
    CHECK(partial_body_sink.calls_after_failure() == 0);

    PrefixFailingByteSink partial_trailer_sink(HEADER_BYTES + EXPECTED_BODY_BYTES + 7);
    const auto partial_trailer = package_codec::emit_distributed_sieve_work_package_v1(
        *prepared.prepared, identity, partial_trailer_sink);
    CHECK(!partial_trailer);
    CHECK(!partial_trailer.trailer_emitted);
    CHECK(!partial_trailer.witness.has_value());
    CHECK(partial_trailer.status.error == sieve::DistributedSieveProtocolError::output_too_large);
    CHECK(partial_trailer.status.byte_offset == HEADER_BYTES + EXPECTED_BODY_BYTES);
    CHECK(partial_trailer.bytes_emitted == HEADER_BYTES + EXPECTED_BODY_BYTES);
    CHECK(partial_trailer_sink.bytes().size() == HEADER_BYTES + EXPECTED_BODY_BYTES + 7);
    CHECK(partial_trailer_sink.failed_put_bytes() == TRAILER_BYTES);
    CHECK(partial_trailer_sink.calls_after_failure() == 0);

    auto same_size_drift = identity;
    same_size_drift.semantic_versions.merge_policy_version = 10;
    CHECK(sieve::validate_distributed_sieve_work_identity(same_size_drift));
    VectorProbeSink digest_drift_sink;
    const auto digest_drift = package_codec::emit_distributed_sieve_work_package_v1(
        *prepared.prepared, same_size_drift, digest_drift_sink);
    CHECK(!digest_drift);
    CHECK(!digest_drift.trailer_emitted);
    CHECK(digest_drift.status.error == sieve::DistributedSieveProtocolError::digest_mismatch);
    CHECK(digest_drift.status.byte_offset == WORK_SHA256_OFFSET);
    CHECK(digest_drift.bytes_emitted == HEADER_BYTES + EXPECTED_BODY_BYTES);
    CHECK(digest_drift_sink.bytes().size() == HEADER_BYTES + EXPECTED_BODY_BYTES);

    auto size_drift = identity;
    size_drift.distributed.chunks[0].relative_artifact_stem.push_back('x');
    CHECK(sieve::validate_distributed_sieve_work_identity(size_drift));
    VectorProbeSink size_drift_sink;
    const auto count_drift = package_codec::emit_distributed_sieve_work_package_v1(
        *prepared.prepared, size_drift, size_drift_sink);
    CHECK(!count_drift);
    CHECK(!count_drift.trailer_emitted);
    CHECK(count_drift.status.error == sieve::DistributedSieveProtocolError::declared_size_mismatch);
    CHECK(count_drift.status.byte_offset == BODY_BYTES_OFFSET);
    CHECK(count_drift.bytes_emitted == size_drift_sink.bytes().size());
    CHECK(size_drift_sink.bytes().size() <= HEADER_BYTES + EXPECTED_BODY_BYTES);
}

} // namespace

int main() {
    try {
        static_assert(package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1 == 80);
        static_assert(package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1 == 32);
        static_assert(MAGIC_OFFSET == 0);
        static_assert(WIRE_VERSION_OFFSET == 8);
        static_assert(WORK_SCHEMA_OFFSET == 12);
        static_assert(HEADER_BYTES_OFFSET == 16);
        static_assert(TRAILER_BYTES_OFFSET == 20);
        static_assert(BODY_BYTES_OFFSET == 24);
        static_assert(TOTAL_BYTES_OFFSET == 32);
        static_assert(WORK_SHA256_OFFSET == 40);
        static_assert(RESERVED_OFFSET == 72);
        static_assert(package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_STRUCTURAL_MAX_BYTES_V1 ==
                      UINT64_C(739266647));
        static_assert(package_codec::DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1 ==
                      UINT64_C(739266636));
        test_golden_oracle_and_roundtrip();
        test_all_exact_prefix_truncations_and_trailing_bytes();
        test_every_header_field_fails_closed();
        test_digest_integrity_order_and_body_error_offset();
        test_sink_failure_and_pass_drift_never_emit_trailer();
        std::cout << "distributed sieve work package codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
