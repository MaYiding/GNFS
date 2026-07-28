#pragma once

// Source-private V1 envelope around the canonical distributed-sieve work
// identity body. The package is path-free and suitable for an inherited,
// already-open read-only descriptor.

#include "distributed_sieve_work_identity_codec_internal.hpp"

#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_work_package_codec_detail {

inline constexpr std::array<char, 8> DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_V1{
    'G', 'N', 'F', 'S', 'D', 'W', 'P', '1',
};
inline constexpr std::uint32_t DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_V1 = 1;
inline constexpr std::uint32_t DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1 = 80;
inline constexpr std::uint32_t DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1 = 32;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_OFFSET_V1 = 0;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_OFFSET_V1 = 8;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SCHEMA_OFFSET_V1 = 12;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_OFFSET_V1 = 16;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_OFFSET_V1 = 20;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1 = 24;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORK_PACKAGE_TOTAL_BYTES_OFFSET_V1 = 32;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SHA256_OFFSET_V1 = 40;
inline constexpr std::size_t DISTRIBUTED_SIEVE_WORK_PACKAGE_RESERVED_OFFSET_V1 = 72;
inline constexpr std::string_view DISTRIBUTED_SIEVE_WORK_PACKAGE_DIGEST_DOMAIN_V1 =
    "GNFS-DISTRIBUTED-SIEVE-WORK-PACKAGE-V1";
inline constexpr std::string_view DISTRIBUTED_SIEVE_WORK_DIGEST_DOMAIN_V1 =
    "GNFS-DISTRIBUTED-SIEVE-WORK-V1";

inline constexpr std::uint64_t DISTRIBUTED_SIEVE_WORK_PACKAGE_STRUCTURAL_MAX_BYTES_V1 =
    DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1 +
    distributed_sieve_work_identity_codec_detail::
        DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_STRUCTURAL_MAX_BODY_BYTES +
    DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1;
inline constexpr std::uint64_t DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1 =
    DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1 +
    distributed_sieve_work_identity_codec_detail::
        DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES +
    DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1;

static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_STRUCTURAL_MAX_BYTES_V1 == UINT64_C(739266647));
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1 == UINT64_C(739266636));
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_OFFSET_V1 == 0);
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_OFFSET_V1 +
                  DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_V1.size() ==
              DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_OFFSET_V1);
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_OFFSET_V1 + 4 ==
              DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SCHEMA_OFFSET_V1);
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SCHEMA_OFFSET_V1 + 4 ==
              DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_OFFSET_V1);
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_OFFSET_V1 + 4 ==
              DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_OFFSET_V1);
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_OFFSET_V1 + 4 ==
              DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1);
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1 + 8 ==
              DISTRIBUTED_SIEVE_WORK_PACKAGE_TOTAL_BYTES_OFFSET_V1);
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_TOTAL_BYTES_OFFSET_V1 + 8 ==
              DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SHA256_OFFSET_V1);
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SHA256_OFFSET_V1 + util::SHA256_DIGEST_BYTES ==
              DISTRIBUTED_SIEVE_WORK_PACKAGE_RESERVED_OFFSET_V1);
static_assert(DISTRIBUTED_SIEVE_WORK_PACKAGE_RESERVED_OFFSET_V1 + 8 ==
              DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1);

struct DistributedSievePreparedWorkPackageV1 final {
    std::uint64_t body_bytes = 0;
    std::uint64_t total_bytes = 0;
    util::Sha256Digest work_sha256;
};

struct DistributedSieveWorkPackageWitnessV1 final {
    std::uint64_t body_bytes = 0;
    std::uint64_t total_bytes = 0;
    util::Sha256Digest work_sha256;
    util::Sha256Digest package_sha256;
};

struct DistributedSieveWorkPackagePrepareResultV1 final {
    std::optional<DistributedSievePreparedWorkPackageV1> prepared;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return prepared.has_value() && static_cast<bool>(status);
    }
};

struct DistributedSieveWorkPackageEmitResultV1 final {
    std::optional<DistributedSieveWorkPackageWitnessV1> witness;
    DistributedSieveProtocolStatus status;
    std::uint64_t bytes_emitted = 0;
    bool trailer_emitted = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return witness.has_value() && trailer_emitted && static_cast<bool>(status);
    }
};

struct DistributedSieveEncodedWorkPackageV1 final {
    std::vector<std::byte> bytes;
    DistributedSieveWorkPackageWitnessV1 witness;
};

struct DistributedSieveWorkPackageEncodeResultV1 final {
    std::optional<DistributedSieveEncodedWorkPackageV1> package;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return package.has_value() && static_cast<bool>(status);
    }
};

struct DistributedSieveDecodedWorkPackageV1 final {
    DistributedSieveWorkIdentityV1 identity;
    DistributedSieveWorkPackageWitnessV1 witness;
};

struct DistributedSieveWorkPackageDecodeResultV1 final {
    std::optional<DistributedSieveDecodedWorkPackageV1> package;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return package.has_value() && static_cast<bool>(status);
    }
};

/// Allocation-free package output contract. put_bytes either consumes the
/// complete span or records failure; good() exposes that state without
/// throwing. A failed sink is never called for a trailer.
template <typename Sink>
concept DistributedSieveWorkPackageByteSinkV1 =
    requires(Sink& sink, const Sink& const_sink, std::span<const std::byte> bytes) {
        { sink.put_bytes(bytes) } -> std::same_as<void>;
        { const_sink.good() } noexcept -> std::convertible_to<bool>;
    };

namespace emission_detail {

[[nodiscard]] constexpr DistributedSieveProtocolStatus
failure(DistributedSieveProtocolError error,
        std::uint64_t byte_offset = DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
        std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {error, byte_offset, element_index};
}

[[nodiscard]] constexpr bool checked_add(std::uint64_t left, std::uint64_t right,
                                         std::uint64_t& result) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    result = left + right;
    return true;
}

inline void store_u32(std::span<std::byte> bytes, std::size_t offset,
                      std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift));
    }
}

inline void store_u64(std::span<std::byte> bytes, std::size_t offset,
                      std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes[offset++] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift));
    }
}

[[nodiscard]] inline std::array<std::byte, DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1>
make_header(const DistributedSievePreparedWorkPackageV1& prepared) noexcept {
    std::array<std::byte, DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1> header{};
    for (std::size_t index = 0; index < DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_V1.size(); ++index) {
        header[DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_OFFSET_V1 + index] =
            static_cast<std::byte>(static_cast<std::uint8_t>(
                static_cast<unsigned char>(DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_V1[index])));
    }
    store_u32(header, DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_OFFSET_V1,
              DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_V1);
    store_u32(header, DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SCHEMA_OFFSET_V1,
              DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1);
    store_u32(header, DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_OFFSET_V1,
              DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1);
    store_u32(header, DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_OFFSET_V1,
              DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1);
    store_u64(header, DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1, prepared.body_bytes);
    store_u64(header, DISTRIBUTED_SIEVE_WORK_PACKAGE_TOTAL_BYTES_OFFSET_V1, prepared.total_bytes);
    std::copy(prepared.work_sha256.bytes.begin(), prepared.work_sha256.bytes.end(),
              header.begin() + DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SHA256_OFFSET_V1);
    store_u64(header, DISTRIBUTED_SIEVE_WORK_PACKAGE_RESERVED_OFFSET_V1, 0);
    return header;
}

template <DistributedSieveWorkPackageByteSinkV1 ByteSink>
class CanonicalIdentityByteSinkAdapter final {
public:
    explicit CanonicalIdentityByteSinkAdapter(ByteSink& sink) noexcept : sink_(sink) {}

    void put_u8(std::uint8_t value) {
        const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
        put(bytes);
    }

    void put_u16(std::uint16_t value) {
        put_unsigned<2>(value);
    }

    void put_u32(std::uint32_t value) {
        put_unsigned<4>(value);
    }

    void put_u64(std::uint64_t value) {
        put_unsigned<8>(value);
    }

    void put_i64(std::int64_t value) {
        put_u64(std::bit_cast<std::uint64_t>(value));
    }

    void put_bool(bool value) {
        put_u8(value ? 1U : 0U);
    }

    void put_string(const std::string& value) {
        if (!good()) {
            return;
        }
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            good_ = false;
            return;
        }
        put_u32(static_cast<std::uint32_t>(value.size()));
        if (!good()) {
            return;
        }
        put(std::as_bytes(std::span(value.data(), value.size())));
    }

    [[nodiscard]] bool good() const noexcept {
        return good_ && static_cast<bool>(sink_.good());
    }

private:
    template <std::size_t Size, typename Integer> void put_unsigned(Integer value) {
        std::array<std::byte, Size> bytes{};
        for (unsigned shift = 0; shift < Size * 8U; shift += 8U) {
            bytes[shift / 8U] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift));
        }
        put(bytes);
    }

    void put(std::span<const std::byte> bytes) {
        if (!good()) {
            return;
        }
        sink_.put_bytes(bytes);
        if (!sink_.good()) {
            good_ = false;
        }
    }

    ByteSink& sink_;
    bool good_ = true;
};

template <DistributedSieveWorkPackageByteSinkV1 Sink> class PackageHashingOutputSink final {
public:
    explicit PackageHashingOutputSink(Sink& sink) noexcept : sink_(sink) {
        constexpr std::array<std::byte, 1> separator{std::byte{0}};
        good_ = accumulator_.update(DISTRIBUTED_SIEVE_WORK_PACKAGE_DIGEST_DOMAIN_V1) &&
                accumulator_.update(separator) && sink_.good();
    }

    void put_bytes(std::span<const std::byte> bytes) {
        if (!good()) {
            return;
        }
        constexpr std::uint64_t max_preimage_bytes =
            DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1 -
            DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1;
        if (static_cast<std::uint64_t>(bytes.size()) > max_preimage_bytes ||
            bytes_emitted_ > max_preimage_bytes - static_cast<std::uint64_t>(bytes.size())) {
            good_ = false;
            return;
        }
        sink_.put_bytes(bytes);
        if (!sink_.good() || !accumulator_.update(bytes)) {
            good_ = false;
            return;
        }
        bytes_emitted_ += static_cast<std::uint64_t>(bytes.size());
    }

    [[nodiscard]] bool good() const noexcept {
        return good_ && static_cast<bool>(sink_.good());
    }

    [[nodiscard]] std::uint64_t bytes_emitted() const noexcept {
        return bytes_emitted_;
    }

    [[nodiscard]] std::optional<util::Sha256Digest> finish() noexcept {
        if (!good()) {
            return std::nullopt;
        }
        auto digest = accumulator_.finalize();
        if (!digest.has_value()) {
            good_ = false;
        }
        return digest;
    }

private:
    Sink& sink_;
    util::Sha256Accumulator accumulator_;
    std::uint64_t bytes_emitted_ = 0;
    bool good_ = false;
};

template <DistributedSieveWorkPackageByteSinkV1 PackageSink> class WorkHashingBodySink final {
public:
    WorkHashingBodySink(PackageSink& sink, std::uint64_t expected_body_bytes) noexcept
        : sink_(sink), expected_body_bytes_(expected_body_bytes) {
        constexpr std::array<std::byte, 1> separator{std::byte{0}};
        good_ = accumulator_.update(DISTRIBUTED_SIEVE_WORK_DIGEST_DOMAIN_V1) &&
                accumulator_.update(separator) && sink_.good();
    }

    void put_bytes(std::span<const std::byte> bytes) {
        if (!good()) {
            return;
        }
        constexpr std::uint64_t max_body_bytes = distributed_sieve_work_identity_codec_detail::
            DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES;
        const auto byte_count = static_cast<std::uint64_t>(bytes.size());
        if (byte_count > max_body_bytes || body_bytes_ > max_body_bytes - byte_count) {
            good_ = false;
            return;
        }
        if (byte_count > expected_body_bytes_ || body_bytes_ > expected_body_bytes_ - byte_count) {
            expected_size_exceeded_ = true;
            good_ = false;
            return;
        }
        sink_.put_bytes(bytes);
        if (!sink_.good() || !accumulator_.update(bytes)) {
            good_ = false;
            return;
        }
        body_bytes_ += byte_count;
    }

    [[nodiscard]] bool good() const noexcept {
        return good_ && static_cast<bool>(sink_.good());
    }

    [[nodiscard]] std::uint64_t body_bytes() const noexcept {
        return body_bytes_;
    }

    [[nodiscard]] bool expected_size_exceeded() const noexcept {
        return expected_size_exceeded_;
    }

    [[nodiscard]] std::optional<util::Sha256Digest> finish() noexcept {
        if (!good()) {
            return std::nullopt;
        }
        auto digest = accumulator_.finalize();
        if (!digest.has_value()) {
            good_ = false;
        }
        return digest;
    }

private:
    PackageSink& sink_;
    util::Sha256Accumulator accumulator_;
    std::uint64_t expected_body_bytes_ = 0;
    std::uint64_t body_bytes_ = 0;
    bool expected_size_exceeded_ = false;
    bool good_ = false;
};

} // namespace emission_detail

[[nodiscard]] DistributedSieveWorkPackagePrepareResultV1
prepare_distributed_sieve_work_package_v1(const DistributedSieveWorkIdentityV1& identity) noexcept;

template <DistributedSieveWorkPackageByteSinkV1 Sink>
[[nodiscard]] DistributedSieveWorkPackageEmitResultV1
emit_distributed_sieve_work_package_v1(const DistributedSievePreparedWorkPackageV1& prepared,
                                       const DistributedSieveWorkIdentityV1& identity,
                                       Sink& sink) noexcept {
    using namespace emission_detail;

    if (const auto status = validate_distributed_sieve_work_identity(identity); !status) {
        return {std::nullopt, status, 0, false};
    }

    std::uint64_t expected_total = 0;
    std::uint64_t header_and_body = 0;
    if (prepared.body_bytes > distributed_sieve_work_identity_codec_detail::
                                  DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES ||
        !checked_add(DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1, prepared.body_bytes,
                     header_and_body) ||
        !checked_add(header_and_body, DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1,
                     expected_total) ||
        expected_total > DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1) {
        return {std::nullopt,
                failure(DistributedSieveProtocolError::declared_size_mismatch,
                        DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1),
                0, false};
    }
    if (expected_total != prepared.total_bytes) {
        return {std::nullopt,
                failure(DistributedSieveProtocolError::declared_size_mismatch,
                        DISTRIBUTED_SIEVE_WORK_PACKAGE_TOTAL_BYTES_OFFSET_V1),
                0, false};
    }
    if (!sink.good()) {
        return {std::nullopt, failure(DistributedSieveProtocolError::output_too_large, 0), 0,
                false};
    }

    try {
        const auto header = make_header(prepared);
        PackageHashingOutputSink<Sink> package_output(sink);
        package_output.put_bytes(header);
        if (!package_output.good()) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::output_too_large,
                            package_output.bytes_emitted()),
                    package_output.bytes_emitted(), false};
        }

        WorkHashingBodySink<PackageHashingOutputSink<Sink>> body_output(package_output,
                                                                        prepared.body_bytes);
        CanonicalIdentityByteSinkAdapter<WorkHashingBodySink<PackageHashingOutputSink<Sink>>>
            canonical_body(body_output);
        const bool emitted =
            distributed_sieve_work_identity_codec_detail::emit_distributed_sieve_work_identity_v1(
                canonical_body, identity);
        if (body_output.expected_size_exceeded()) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::declared_size_mismatch,
                            DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1),
                    package_output.bytes_emitted(), false};
        }
        if (!emitted || !canonical_body.good() || !body_output.good() || !package_output.good()) {
            const auto error = !sink.good() ? DistributedSieveProtocolError::output_too_large
                                            : DistributedSieveProtocolError::digest_unavailable;
            return {std::nullopt, failure(error, package_output.bytes_emitted()),
                    package_output.bytes_emitted(), false};
        }

        auto second_work_digest = body_output.finish();
        if (!second_work_digest.has_value()) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::digest_unavailable,
                            package_output.bytes_emitted()),
                    package_output.bytes_emitted(), false};
        }
        if (body_output.body_bytes() != prepared.body_bytes ||
            package_output.bytes_emitted() != header_and_body) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::declared_size_mismatch,
                            DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1),
                    package_output.bytes_emitted(), false};
        }
        if (*second_work_digest != prepared.work_sha256) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::digest_mismatch,
                            DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SHA256_OFFSET_V1),
                    package_output.bytes_emitted(), false};
        }

        auto package_digest = package_output.finish();
        if (!package_digest.has_value()) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::digest_unavailable,
                            package_output.bytes_emitted()),
                    package_output.bytes_emitted(), false};
        }

        sink.put_bytes(package_digest->bytes);
        if (!sink.good()) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::output_too_large,
                            package_output.bytes_emitted()),
                    package_output.bytes_emitted(), false};
        }

        const DistributedSieveWorkPackageWitnessV1 witness{
            .body_bytes = prepared.body_bytes,
            .total_bytes = prepared.total_bytes,
            .work_sha256 = prepared.work_sha256,
            .package_sha256 = *package_digest,
        };
        return {witness, {}, prepared.total_bytes, true};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted), 0, false};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted), 0, false};
    }
}

[[nodiscard]] DistributedSieveWorkPackageEncodeResultV1
encode_distributed_sieve_work_package_v1(const DistributedSieveWorkIdentityV1& identity) noexcept;

[[nodiscard]] DistributedSieveWorkPackageDecodeResultV1
decode_distributed_sieve_work_package_v1(std::span<const std::byte> bytes) noexcept;

} // namespace gnfs::sieve::distributed_sieve_work_package_codec_detail
