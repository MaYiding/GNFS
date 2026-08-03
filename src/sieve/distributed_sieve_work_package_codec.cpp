#include "distributed_sieve_work_package_codec_internal.hpp"

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

namespace gnfs::sieve::distributed_sieve_work_package_codec_detail {
namespace {

using emission_detail::checked_add;
using emission_detail::failure;

class WorkDigestCountingByteSink final {
public:
    WorkDigestCountingByteSink() noexcept {
        constexpr std::array<std::byte, 1> separator{std::byte{0}};
        good_ = accumulator_.update(DISTRIBUTED_SIEVE_WORK_DIGEST_DOMAIN_V1) &&
                accumulator_.update(separator);
    }

    void put_bytes(std::span<const std::byte> bytes) noexcept {
        if (!good()) {
            return;
        }
        constexpr std::uint64_t max_body_bytes = distributed_sieve_work_identity_codec_detail::
            DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES;
        if (static_cast<std::uint64_t>(bytes.size()) > max_body_bytes ||
            body_bytes_ > max_body_bytes - static_cast<std::uint64_t>(bytes.size()) ||
            !accumulator_.update(bytes)) {
            good_ = false;
            return;
        }
        body_bytes_ += static_cast<std::uint64_t>(bytes.size());
    }

    [[nodiscard]] bool good() const noexcept {
        return good_;
    }

    [[nodiscard]] std::uint64_t body_bytes() const noexcept {
        return body_bytes_;
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
    util::Sha256Accumulator accumulator_;
    std::uint64_t body_bytes_ = 0;
    bool good_ = false;
};

class VectorByteSink final {
public:
    explicit VectorByteSink(std::size_t capacity) {
        bytes_.reserve(capacity);
    }

    void put_bytes(std::span<const std::byte> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] constexpr bool good() const noexcept {
        return true;
    }

    [[nodiscard]] std::vector<std::byte> take_bytes() noexcept {
        return std::move(bytes_);
    }

private:
    std::vector<std::byte> bytes_;
};

[[nodiscard]] std::uint32_t load_u32(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset++]))
                 << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t load_u64(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset++]))
                 << shift;
    }
    return value;
}

[[nodiscard]] std::optional<util::Sha256Digest>
hash_domain_and_bytes(std::string_view domain, std::span<const std::byte> bytes) noexcept {
    util::Sha256Accumulator accumulator;
    constexpr std::array<std::byte, 1> separator{std::byte{0}};
    if (!accumulator.update(domain) || !accumulator.update(separator) ||
        !accumulator.update(bytes)) {
        return std::nullopt;
    }
    return accumulator.finalize();
}

[[nodiscard]] DistributedSieveProtocolStatus
adjust_body_status(DistributedSieveProtocolStatus status) noexcept {
    if (status.byte_offset == DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET) {
        return status;
    }
    std::uint64_t adjusted = 0;
    if (!checked_add(DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1, status.byte_offset,
                     adjusted)) {
        return failure(DistributedSieveProtocolError::integer_out_of_range,
                       DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1, status.element_index);
    }
    status.byte_offset = adjusted;
    return status;
}

} // namespace

DistributedSieveWorkPackagePrepareResultV1
prepare_distributed_sieve_work_package_v1(const DistributedSieveWorkIdentityV1& identity) noexcept {
    if (const auto status = validate_distributed_sieve_work_identity(identity); !status) {
        return {std::nullopt, status};
    }

    try {
        WorkDigestCountingByteSink counter;
        emission_detail::CanonicalIdentityByteSinkAdapter<WorkDigestCountingByteSink>
            canonical_body(counter);
        const bool emitted =
            distributed_sieve_work_identity_codec_detail::emit_distributed_sieve_work_identity_v1(
                canonical_body, identity);
        if (!emitted || !canonical_body.good() || !counter.good()) {
            return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable,
                                          counter.body_bytes())};
        }
        auto work_digest = counter.finish();
        if (!work_digest.has_value()) {
            return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable,
                                          counter.body_bytes())};
        }

        std::uint64_t header_and_body = 0;
        std::uint64_t total_bytes = 0;
        if (!checked_add(DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1, counter.body_bytes(),
                         header_and_body) ||
            !checked_add(header_and_body, DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1,
                         total_bytes) ||
            total_bytes > DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::output_too_large, counter.body_bytes())};
        }

        return {
            DistributedSievePreparedWorkPackageV1{
                .body_bytes = counter.body_bytes(),
                .total_bytes = total_bytes,
                .work_sha256 = *work_digest,
            },
            {},
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

DistributedSieveWorkPackageEncodeResultV1
encode_distributed_sieve_work_package_v1(const DistributedSieveWorkIdentityV1& identity) noexcept {
    const auto prepared = prepare_distributed_sieve_work_package_v1(identity);
    if (!prepared) {
        return {std::nullopt, prepared.status};
    }

    try {
        if (prepared.prepared->total_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return {std::nullopt, failure(DistributedSieveProtocolError::output_too_large,
                                          prepared.prepared->total_bytes)};
        }
        VectorByteSink sink(static_cast<std::size_t>(prepared.prepared->total_bytes));
        const auto emitted =
            emit_distributed_sieve_work_package_v1(*prepared.prepared, identity, sink);
        if (!emitted) {
            return {std::nullopt, emitted.status};
        }
        auto bytes = sink.take_bytes();
        if (bytes.size() != prepared.prepared->total_bytes) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::declared_size_mismatch, bytes.size())};
        }
        return {
            DistributedSieveEncodedWorkPackageV1{
                .bytes = std::move(bytes),
                .witness = *emitted.witness,
            },
            {},
        };
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

DistributedSieveWorkPackageDecodeResultV1
decode_distributed_sieve_work_package_v1(std::span<const std::byte> bytes) noexcept {
    if (static_cast<std::uint64_t>(bytes.size()) >
        DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1) {
        return {std::nullopt,
                failure(DistributedSieveProtocolError::input_too_large, bytes.size())};
    }
    if (bytes.size() < DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1) {
        return {std::nullopt, failure(DistributedSieveProtocolError::truncated, bytes.size())};
    }

    for (std::size_t index = 0; index < DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_V1.size(); ++index) {
        const auto expected = static_cast<std::byte>(static_cast<std::uint8_t>(
            static_cast<unsigned char>(DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_V1[index])));
        const auto offset = DISTRIBUTED_SIEVE_WORK_PACKAGE_MAGIC_OFFSET_V1 + index;
        if (bytes[offset] != expected) {
            return {std::nullopt, failure(DistributedSieveProtocolError::invalid_magic, offset)};
        }
    }

    const std::uint32_t wire_version =
        load_u32(bytes, DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_OFFSET_V1);
    const std::uint32_t work_schema =
        load_u32(bytes, DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SCHEMA_OFFSET_V1);
    const std::uint32_t header_bytes =
        load_u32(bytes, DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_OFFSET_V1);
    const std::uint32_t trailer_bytes =
        load_u32(bytes, DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_OFFSET_V1);
    const std::uint64_t body_bytes =
        load_u64(bytes, DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1);
    const std::uint64_t declared_total =
        load_u64(bytes, DISTRIBUTED_SIEVE_WORK_PACKAGE_TOTAL_BYTES_OFFSET_V1);
    const std::uint64_t reserved =
        load_u64(bytes, DISTRIBUTED_SIEVE_WORK_PACKAGE_RESERVED_OFFSET_V1);

    if (wire_version != DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_V1) {
        return {std::nullopt, failure(DistributedSieveProtocolError::unsupported_wire_version,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_WIRE_VERSION_OFFSET_V1)};
    }
    if (work_schema != DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1) {
        return {std::nullopt, failure(DistributedSieveProtocolError::unsupported_schema_version,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SCHEMA_OFFSET_V1)};
    }
    if (header_bytes != DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1) {
        return {std::nullopt, failure(DistributedSieveProtocolError::invalid_value,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_OFFSET_V1)};
    }
    if (trailer_bytes != DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1) {
        return {std::nullopt, failure(DistributedSieveProtocolError::invalid_value,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_OFFSET_V1)};
    }
    if (reserved != 0) {
        return {std::nullopt, failure(DistributedSieveProtocolError::invalid_value,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_RESERVED_OFFSET_V1)};
    }
    if (body_bytes > distributed_sieve_work_identity_codec_detail::
                         DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES) {
        return {std::nullopt, failure(DistributedSieveProtocolError::input_too_large,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1)};
    }

    std::uint64_t trailer_offset = 0;
    std::uint64_t computed_total = 0;
    if (!checked_add(DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1, body_bytes, trailer_offset) ||
        !checked_add(trailer_offset, DISTRIBUTED_SIEVE_WORK_PACKAGE_TRAILER_BYTES_V1,
                     computed_total) ||
        computed_total > DISTRIBUTED_SIEVE_WORK_PACKAGE_VALID_MAX_BYTES_V1) {
        return {std::nullopt, failure(DistributedSieveProtocolError::declared_size_mismatch,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_BODY_BYTES_OFFSET_V1)};
    }
    if (declared_total != computed_total) {
        return {std::nullopt, failure(DistributedSieveProtocolError::declared_size_mismatch,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_TOTAL_BYTES_OFFSET_V1)};
    }
    if (static_cast<std::uint64_t>(bytes.size()) < declared_total) {
        return {std::nullopt, failure(DistributedSieveProtocolError::truncated, bytes.size())};
    }
    if (static_cast<std::uint64_t>(bytes.size()) > declared_total) {
        return {std::nullopt,
                failure(DistributedSieveProtocolError::trailing_bytes, declared_total)};
    }

    util::Sha256Digest stored_work_digest;
    std::copy_n(bytes.begin() + DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SHA256_OFFSET_V1,
                stored_work_digest.bytes.size(), stored_work_digest.bytes.begin());
    util::Sha256Digest stored_package_digest;
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(trailer_offset),
                stored_package_digest.bytes.size(), stored_package_digest.bytes.begin());

    const auto package_preimage = bytes.first(static_cast<std::size_t>(trailer_offset));
    const auto computed_package_digest =
        hash_domain_and_bytes(DISTRIBUTED_SIEVE_WORK_PACKAGE_DIGEST_DOMAIN_V1, package_preimage);
    if (!computed_package_digest.has_value()) {
        return {std::nullopt,
                failure(DistributedSieveProtocolError::digest_unavailable, trailer_offset)};
    }
    if (*computed_package_digest != stored_package_digest) {
        return {std::nullopt,
                failure(DistributedSieveProtocolError::digest_mismatch, trailer_offset)};
    }

    const auto body = bytes.subspan(DISTRIBUTED_SIEVE_WORK_PACKAGE_HEADER_BYTES_V1,
                                    static_cast<std::size_t>(body_bytes));
    const auto computed_work_digest =
        hash_domain_and_bytes(DISTRIBUTED_SIEVE_WORK_DIGEST_DOMAIN_V1, body);
    if (!computed_work_digest.has_value()) {
        return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SHA256_OFFSET_V1)};
    }
    if (*computed_work_digest != stored_work_digest) {
        return {std::nullopt, failure(DistributedSieveProtocolError::digest_mismatch,
                                      DISTRIBUTED_SIEVE_WORK_PACKAGE_WORK_SHA256_OFFSET_V1)};
    }

    // Integrity binding is complete before the owning identity decoder can
    // allocate strings or vectors. No source-authenticity claim is made.
    auto decoded_identity =
        distributed_sieve_work_identity_codec_detail::decode_distributed_sieve_work_identity_v1(
            body);
    if (!decoded_identity) {
        return {std::nullopt, adjust_body_status(decoded_identity.status)};
    }

    return {
        DistributedSieveDecodedWorkPackageV1{
            .identity = std::move(*decoded_identity.identity),
            .witness =
                {
                    .body_bytes = body_bytes,
                    .total_bytes = declared_total,
                    .work_sha256 = stored_work_digest,
                    .package_sha256 = stored_package_digest,
                },
        },
        {},
    };
}

} // namespace gnfs::sieve::distributed_sieve_work_package_codec_detail
