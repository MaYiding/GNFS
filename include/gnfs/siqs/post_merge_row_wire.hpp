#pragma once

/// @file post_merge_row_wire.hpp
/// @brief Bounded, canonical wire codec for SIQS sparse-wide rows.

#include <gnfs/siqs/post_merge_row.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

namespace gnfs::siqs {

inline constexpr uint32_t SIQS_POST_MERGE_ROW_WIRE_VERSION = 1;
inline constexpr std::size_t SIQS_POST_MERGE_ROW_WIRE_HEADER_SIZE = 40;
inline constexpr std::size_t SIQS_POST_MERGE_ROW_WIRE_MAX_BYTES = std::size_t{64} * 1024 * 1024;
inline constexpr std::size_t SIQS_POST_MERGE_ROW_WIRE_NO_ERROR_OFFSET =
    std::numeric_limits<std::size_t>::max();

enum class SIQSPostMergeRowWireError : uint8_t {
    none,
    truncated,
    trailing_bytes,
    invalid_magic,
    unsupported_wire_version,
    declared_size_mismatch,
    nonzero_reserved,
    invalid_boolean,
    invalid_row,
    invalid_source_ids,
    invalid_factor_powers,
    invalid_large_prime_roots,
    noncanonical_integer,
    size_overflow,
    resource_limit,
    allocation_failure,
};

[[nodiscard]] constexpr std::string_view
siqs_post_merge_row_wire_error_name(SIQSPostMergeRowWireError error) noexcept {
    switch (error) {
    case SIQSPostMergeRowWireError::none:
        return "none";
    case SIQSPostMergeRowWireError::truncated:
        return "truncated";
    case SIQSPostMergeRowWireError::trailing_bytes:
        return "trailing_bytes";
    case SIQSPostMergeRowWireError::invalid_magic:
        return "invalid_magic";
    case SIQSPostMergeRowWireError::unsupported_wire_version:
        return "unsupported_wire_version";
    case SIQSPostMergeRowWireError::declared_size_mismatch:
        return "declared_size_mismatch";
    case SIQSPostMergeRowWireError::nonzero_reserved:
        return "nonzero_reserved";
    case SIQSPostMergeRowWireError::invalid_boolean:
        return "invalid_boolean";
    case SIQSPostMergeRowWireError::invalid_row:
        return "invalid_row";
    case SIQSPostMergeRowWireError::invalid_source_ids:
        return "invalid_source_ids";
    case SIQSPostMergeRowWireError::invalid_factor_powers:
        return "invalid_factor_powers";
    case SIQSPostMergeRowWireError::invalid_large_prime_roots:
        return "invalid_large_prime_roots";
    case SIQSPostMergeRowWireError::noncanonical_integer:
        return "noncanonical_integer";
    case SIQSPostMergeRowWireError::size_overflow:
        return "size_overflow";
    case SIQSPostMergeRowWireError::resource_limit:
        return "resource_limit";
    case SIQSPostMergeRowWireError::allocation_failure:
        return "allocation_failure";
    }
    return "unknown";
}

struct SIQSPostMergeRowWireEncodeResult final {
    std::optional<std::vector<std::byte>> bytes;
    SIQSPostMergeRowWireError error = SIQSPostMergeRowWireError::none;
    std::size_t error_offset = SIQS_POST_MERGE_ROW_WIRE_NO_ERROR_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return bytes.has_value() && error == SIQSPostMergeRowWireError::none;
    }
};

struct SIQSPostMergeRowWireDecodeResult final {
    std::optional<SIQSPostMergeRow> row;
    SIQSPostMergeRowWireError error = SIQSPostMergeRowWireError::none;
    std::size_t error_offset = SIQS_POST_MERGE_ROW_WIRE_NO_ERROR_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return row.has_value() && error == SIQSPostMergeRowWireError::none;
    }
};

namespace post_merge_row_wire_detail {

inline constexpr std::size_t declared_size_offset = 12;
inline constexpr std::size_t flags_offset = 20;
inline constexpr std::size_t x_size_offset = 24;
inline constexpr std::size_t factor_count_offset = 28;
inline constexpr std::size_t large_prime_count_offset = 32;
inline constexpr std::size_t source_count_offset = 36;
inline constexpr std::size_t payload_offset = SIQS_POST_MERGE_ROW_WIRE_HEADER_SIZE;
inline constexpr std::size_t factor_power_wire_size = 8;
inline constexpr std::size_t large_prime_wire_size = 8;
inline constexpr std::size_t source_id_wire_size = 8;

inline constexpr char magic[] = "GNFSQROW";

struct Status final {
    SIQSPostMergeRowWireError error = SIQSPostMergeRowWireError::none;
    std::size_t offset = SIQS_POST_MERGE_ROW_WIRE_NO_ERROR_OFFSET;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return error == SIQSPostMergeRowWireError::none;
    }
};

[[nodiscard]] constexpr Status failure(SIQSPostMergeRowWireError error,
                                       std::size_t offset) noexcept {
    return {error, offset};
}

[[nodiscard]] constexpr bool checked_add(std::size_t lhs, std::size_t rhs,
                                         std::size_t& result) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] constexpr bool checked_mul(std::size_t lhs, std::size_t rhs,
                                         std::size_t& result) noexcept {
    if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

[[nodiscard]] constexpr bool fits_size(uint64_t value) noexcept {
    if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
        return value <= static_cast<uint64_t>(std::numeric_limits<std::size_t>::max());
    }
    return true;
}

constexpr void write_u32(std::vector<std::byte>& bytes, std::size_t offset,
                         uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<std::byte>(static_cast<uint8_t>(value >> shift));
    }
}

constexpr void write_u64(std::vector<std::byte>& bytes, std::size_t offset,
                         uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes[offset++] = static_cast<std::byte>(static_cast<uint8_t>(value >> shift));
    }
}

[[nodiscard]] constexpr uint8_t read_u8(std::span<const std::byte> bytes,
                                        std::size_t offset) noexcept {
    return std::to_integer<uint8_t>(bytes[offset]);
}

[[nodiscard]] constexpr uint32_t read_u32(std::span<const std::byte> bytes,
                                          std::size_t offset) noexcept {
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        result |= static_cast<uint32_t>(read_u8(bytes, offset++)) << shift;
    }
    return result;
}

[[nodiscard]] constexpr uint64_t read_u64(std::span<const std::byte> bytes,
                                          std::size_t offset) noexcept {
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        result |= static_cast<uint64_t>(read_u8(bytes, offset++)) << shift;
    }
    return result;
}

[[nodiscard]] inline std::vector<std::byte> export_integer(const core::Integer& value) {
    const std::size_t bit_count = mpz_sizeinbase(value.get_mpz(), 2);
    if (bit_count > std::numeric_limits<std::size_t>::max() - 7) {
        throw std::length_error("SIQS row integer size overflows byte conversion");
    }
    const std::size_t byte_count = bit_count == 0 ? 1 : (bit_count + 7) / 8;
    if (byte_count > SIQS_POST_MERGE_ROW_WIRE_MAX_BYTES - payload_offset) {
        throw std::length_error("SIQS row integer exceeds wire resource limit");
    }
    std::vector<std::byte> result(byte_count, std::byte{0});
    if (value.is_zero()) {
        return result;
    }
    // mpz_export with order -1 and one-byte words is explicitly least-significant
    // byte first and therefore independent of host word endianness.
    std::vector<unsigned char> raw(byte_count, 0);
    std::size_t exported = 0;
    mpz_export(raw.data(), &exported, -1, 1, 1, 0, value.get_mpz());
    if (exported != byte_count) {
        throw std::runtime_error("SIQS row integer export size mismatch");
    }
    for (std::size_t i = 0; i < byte_count; ++i) {
        result[i] = static_cast<std::byte>(raw[i]);
    }
    return result;
}

[[nodiscard]] inline Status validate_row_shape(const SIQSPostMergeRow& row) noexcept {
    if (row.x_modulus.is_negative()) {
        return failure(SIQSPostMergeRowWireError::invalid_row, 0);
    }
    if (row.source_ids.empty()) {
        return failure(SIQSPostMergeRowWireError::invalid_source_ids, source_count_offset);
    }
    for (std::size_t i = 1; i < row.source_ids.size(); ++i) {
        if (row.source_ids[i - 1] >= row.source_ids[i]) {
            return failure(SIQSPostMergeRowWireError::invalid_source_ids,
                           payload_offset + i * source_id_wire_size);
        }
    }
    uint32_t previous_index = 0;
    for (const SIQSFactorPower& power : row.factor_powers) {
        if (power.factor_base_index == 0 || power.exponent == 0 ||
            power.factor_base_index <= previous_index) {
            return failure(SIQSPostMergeRowWireError::invalid_factor_powers, factor_count_offset);
        }
        previous_index = power.factor_base_index;
    }
    for (std::size_t i = 0; i < row.large_prime_sqrt_factors.size(); ++i) {
        if (row.large_prime_sqrt_factors[i] < 2 ||
            (i != 0 && row.large_prime_sqrt_factors[i - 1] > row.large_prime_sqrt_factors[i])) {
            return failure(SIQSPostMergeRowWireError::invalid_large_prime_roots,
                           large_prime_count_offset);
        }
    }
    return {};
}

[[nodiscard]] inline Status checked_wire_size(const SIQSPostMergeRow& row, std::size_t x_size,
                                              std::size_t& total) noexcept {
    if (row.factor_powers.size() > std::numeric_limits<uint32_t>::max() ||
        row.large_prime_sqrt_factors.size() > std::numeric_limits<uint32_t>::max() ||
        row.source_ids.size() > std::numeric_limits<uint32_t>::max()) {
        return failure(SIQSPostMergeRowWireError::resource_limit, 0);
    }
    total = payload_offset;
    if (!checked_add(total, x_size, total)) {
        return failure(SIQSPostMergeRowWireError::size_overflow, x_size_offset);
    }
    for (const auto [count, width, offset] :
         {std::tuple<std::size_t, std::size_t, std::size_t>{
              row.factor_powers.size(), factor_power_wire_size, factor_count_offset},
          std::tuple<std::size_t, std::size_t, std::size_t>{
              row.large_prime_sqrt_factors.size(), large_prime_wire_size, large_prime_count_offset},
          std::tuple<std::size_t, std::size_t, std::size_t>{
              row.source_ids.size(), source_id_wire_size, source_count_offset}}) {
        std::size_t bytes = 0;
        if (!checked_mul(count, width, bytes) || !checked_add(total, bytes, total)) {
            return failure(SIQSPostMergeRowWireError::size_overflow, offset);
        }
    }
    if (total > SIQS_POST_MERGE_ROW_WIRE_MAX_BYTES) {
        return failure(SIQSPostMergeRowWireError::resource_limit, total);
    }
    return {};
}

[[nodiscard]] inline Status check_frame(std::span<const std::byte> bytes, std::size_t& x_size,
                                        uint32_t& factor_count, uint32_t& large_prime_count,
                                        uint32_t& source_count, std::size_t& total_size) noexcept {
    if (bytes.size() < payload_offset) {
        return failure(SIQSPostMergeRowWireError::truncated, bytes.size());
    }
    for (std::size_t i = 0; i < 8; ++i) {
        if (read_u8(bytes, i) != static_cast<uint8_t>(magic[i])) {
            return failure(SIQSPostMergeRowWireError::invalid_magic, i);
        }
    }
    if (read_u32(bytes, 8) != SIQS_POST_MERGE_ROW_WIRE_VERSION) {
        return failure(SIQSPostMergeRowWireError::unsupported_wire_version, 8);
    }
    const uint64_t declared_size = read_u64(bytes, declared_size_offset);
    if (!fits_size(declared_size) || declared_size > SIQS_POST_MERGE_ROW_WIRE_MAX_BYTES) {
        return failure(SIQSPostMergeRowWireError::resource_limit, declared_size_offset);
    }
    total_size = static_cast<std::size_t>(declared_size);
    if (total_size != bytes.size()) {
        return failure(total_size < bytes.size() ? SIQSPostMergeRowWireError::trailing_bytes
                                                 : SIQSPostMergeRowWireError::truncated,
                       total_size < bytes.size() ? total_size : bytes.size());
    }
    if (read_u8(bytes, flags_offset) > 1) {
        return failure(SIQSPostMergeRowWireError::invalid_boolean, flags_offset);
    }
    for (std::size_t i = flags_offset + 1; i < x_size_offset; ++i) {
        if (read_u8(bytes, i) != 0) {
            return failure(SIQSPostMergeRowWireError::nonzero_reserved, i);
        }
    }
    x_size = read_u32(bytes, x_size_offset);
    factor_count = read_u32(bytes, factor_count_offset);
    large_prime_count = read_u32(bytes, large_prime_count_offset);
    source_count = read_u32(bytes, source_count_offset);
    std::size_t expected = payload_offset;
    if (!checked_add(expected, x_size, expected)) {
        return failure(SIQSPostMergeRowWireError::size_overflow, x_size_offset);
    }
    for (const auto [count, width, offset] :
         {std::tuple<std::size_t, std::size_t, std::size_t>{factor_count, factor_power_wire_size,
                                                            factor_count_offset},
          std::tuple<std::size_t, std::size_t, std::size_t>{
              large_prime_count, large_prime_wire_size, large_prime_count_offset},
          std::tuple<std::size_t, std::size_t, std::size_t>{source_count, source_id_wire_size,
                                                            source_count_offset}}) {
        std::size_t bytes_for_field = 0;
        if (!checked_mul(count, width, bytes_for_field) ||
            !checked_add(expected, bytes_for_field, expected)) {
            return failure(SIQSPostMergeRowWireError::size_overflow, offset);
        }
    }
    if (expected != total_size) {
        return failure(SIQSPostMergeRowWireError::declared_size_mismatch, declared_size_offset);
    }
    if (x_size == 0) {
        return failure(SIQSPostMergeRowWireError::noncanonical_integer, x_size_offset);
    }
    if (x_size > 1 && read_u8(bytes, payload_offset + x_size - 1) == 0) {
        return failure(SIQSPostMergeRowWireError::noncanonical_integer,
                       payload_offset + x_size - 1);
    }
    if (source_count == 0) {
        return failure(SIQSPostMergeRowWireError::invalid_source_ids, source_count_offset);
    }
    return {};
}

} // namespace post_merge_row_wire_detail

/// Encode a canonical sparse-wide row. The result is independent of host
/// endianness and contains no pointers or implementation-defined padding.
[[nodiscard]] inline SIQSPostMergeRowWireEncodeResult
encode_siqs_post_merge_row(const SIQSPostMergeRow& row) noexcept {
    using namespace post_merge_row_wire_detail;
    const Status shape = validate_row_shape(row);
    if (!shape) {
        return {std::nullopt, shape.error, shape.offset};
    }
    try {
        const auto x_bytes = export_integer(row.x_modulus);
        std::size_t total_size = 0;
        const Status size_status = checked_wire_size(row, x_bytes.size(), total_size);
        if (!size_status) {
            return {std::nullopt, size_status.error, size_status.offset};
        }
        std::vector<std::byte> bytes(total_size, std::byte{0});
        for (std::size_t i = 0; i < 8; ++i) {
            bytes[i] = static_cast<std::byte>(static_cast<uint8_t>(magic[i]));
        }
        write_u32(bytes, 8, SIQS_POST_MERGE_ROW_WIRE_VERSION);
        write_u64(bytes, declared_size_offset, static_cast<uint64_t>(total_size));
        bytes[flags_offset] = static_cast<std::byte>(row.q_negative ? 1 : 0);
        write_u32(bytes, x_size_offset, static_cast<uint32_t>(x_bytes.size()));
        write_u32(bytes, factor_count_offset, static_cast<uint32_t>(row.factor_powers.size()));
        write_u32(bytes, large_prime_count_offset,
                  static_cast<uint32_t>(row.large_prime_sqrt_factors.size()));
        write_u32(bytes, source_count_offset, static_cast<uint32_t>(row.source_ids.size()));

        std::size_t cursor = payload_offset;
        for (const std::byte byte : x_bytes) {
            bytes[cursor++] = byte;
        }
        for (const SIQSFactorPower& power : row.factor_powers) {
            write_u32(bytes, cursor, power.factor_base_index);
            cursor += 4;
            write_u32(bytes, cursor, power.exponent);
            cursor += 4;
        }
        for (const uint64_t root : row.large_prime_sqrt_factors) {
            write_u64(bytes, cursor, root);
            cursor += 8;
        }
        for (const SIQSSourceId source_id : row.source_ids) {
            write_u64(bytes, cursor, source_id.value);
            cursor += 8;
        }
        return {std::move(bytes), SIQSPostMergeRowWireError::none,
                SIQS_POST_MERGE_ROW_WIRE_NO_ERROR_OFFSET};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, SIQSPostMergeRowWireError::allocation_failure, 0};
    } catch (const std::length_error&) {
        return {std::nullopt, SIQSPostMergeRowWireError::size_overflow, 0};
    } catch (...) {
        return {std::nullopt, SIQSPostMergeRowWireError::invalid_row, 0};
    }
}

/// Decode one complete canonical row frame. Arithmetic identity validation is
/// intentionally separate: call check_siqs_post_merge_row_identity() with the
/// run's modulus and ordered factor base after decoding.
[[nodiscard]] inline SIQSPostMergeRowWireDecodeResult
decode_siqs_post_merge_row(std::span<const std::byte> bytes) noexcept {
    using namespace post_merge_row_wire_detail;
    try {
        std::size_t x_size = 0;
        uint32_t factor_count = 0;
        uint32_t large_prime_count = 0;
        uint32_t source_count = 0;
        std::size_t total_size = 0;
        const Status frame =
            check_frame(bytes, x_size, factor_count, large_prime_count, source_count, total_size);
        if (!frame) {
            return {std::nullopt, frame.error, frame.offset};
        }

        SIQSPostMergeRow row;
        row.q_negative = read_u8(bytes, flags_offset) != 0;
        row.x_modulus = core::Integer(0);
        mpz_import(row.x_modulus.get_mpz(), x_size, -1, 1, 1, 0,
                   bytes.data() + static_cast<std::ptrdiff_t>(payload_offset));

        std::size_t cursor = payload_offset + x_size;
        row.factor_powers.reserve(factor_count);
        for (uint32_t i = 0; i < factor_count; ++i) {
            const uint32_t index = read_u32(bytes, cursor);
            cursor += 4;
            const uint32_t exponent = read_u32(bytes, cursor);
            cursor += 4;
            if (index == 0 || exponent == 0 ||
                (!row.factor_powers.empty() &&
                 row.factor_powers.back().factor_base_index >= index)) {
                return {std::nullopt, SIQSPostMergeRowWireError::invalid_factor_powers,
                        factor_count_offset};
            }
            row.factor_powers.push_back({index, exponent});
        }

        row.large_prime_sqrt_factors.reserve(large_prime_count);
        for (uint32_t i = 0; i < large_prime_count; ++i) {
            const uint64_t root = read_u64(bytes, cursor);
            cursor += 8;
            if (root < 2 || (!row.large_prime_sqrt_factors.empty() &&
                             row.large_prime_sqrt_factors.back() > root)) {
                return {std::nullopt, SIQSPostMergeRowWireError::invalid_large_prime_roots,
                        large_prime_count_offset};
            }
            row.large_prime_sqrt_factors.push_back(root);
        }

        row.source_ids.reserve(source_count);
        for (uint32_t i = 0; i < source_count; ++i) {
            const SIQSSourceId source_id{read_u64(bytes, cursor)};
            cursor += 8;
            if (!row.source_ids.empty() && row.source_ids.back() >= source_id) {
                return {std::nullopt, SIQSPostMergeRowWireError::invalid_source_ids,
                        source_count_offset};
            }
            row.source_ids.push_back(source_id);
        }
        if (cursor != total_size) {
            return {std::nullopt, SIQSPostMergeRowWireError::declared_size_mismatch,
                    declared_size_offset};
        }
        return {std::move(row), SIQSPostMergeRowWireError::none,
                SIQS_POST_MERGE_ROW_WIRE_NO_ERROR_OFFSET};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, SIQSPostMergeRowWireError::allocation_failure, 0};
    } catch (...) {
        return {std::nullopt, SIQSPostMergeRowWireError::invalid_row, 0};
    }
}

} // namespace gnfs::siqs
