// Canonical sparse-wide SIQS row wire and independent arithmetic oracle.

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/post_merge_row.hpp>
#include <gnfs/siqs/post_merge_row_wire.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

using gnfs::core::Integer;
using gnfs::siqs::check_siqs_post_merge_row_identity;
using gnfs::siqs::decode_siqs_post_merge_row;
using gnfs::siqs::encode_siqs_post_merge_row;
using gnfs::siqs::SIQSFactorPower;
using gnfs::siqs::SIQSPostMergeRow;
using gnfs::siqs::SIQSPostMergeRowStatus;
using gnfs::siqs::SIQSPostMergeRowWireDecodeResult;
using gnfs::siqs::SIQSPostMergeRowWireError;
using gnfs::siqs::SIQSSourceId;
using gnfs::siqs::visit_siqs_post_merge_odd_columns;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition << " at " << __FILE__ << ':' << __LINE__ << '\n';     \
        }                                                                                          \
    } while (false)

constexpr std::array<uint32_t, 5> factor_base{0, 2, 3, 5, 7};
constexpr std::array<uint64_t, 2> wide_roots{4'294'967'311ULL, 4'294'967'357ULL};

void put_u32(std::vector<std::byte>& bytes, std::size_t offset, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<std::byte>(static_cast<uint8_t>(value >> shift));
    }
}

void put_u64(std::vector<std::byte>& bytes, std::size_t offset, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes[offset++] = static_cast<std::byte>(static_cast<uint8_t>(value >> shift));
    }
}

void multiply_mod(Integer& product, const Integer& factor, const Integer& modulus) {
    mpz_mul(product.get_mpz(), product.get_mpz(), factor.get_mpz());
    mpz_mod(product.get_mpz(), product.get_mpz(), modulus.get_mpz());
}

[[nodiscard]] SIQSPostMergeRow make_identity_row(const Integer& modulus) {
    SIQSPostMergeRow row{
        Integer(1), false, {{1, 4}, {2, 6}, {3, 2}}, {wide_roots[0], wide_roots[1]}, {{10}, {20}}};

    // Construct one square root of the independent RHS with GMP directly.
    for (const SIQSFactorPower& power : row.factor_powers) {
        Integer factor;
        mpz_powm_ui(factor.get_mpz(),
                    Integer(static_cast<uint64_t>(factor_base[power.factor_base_index])).get_mpz(),
                    static_cast<unsigned long>(power.exponent / 2), modulus.get_mpz());
        multiply_mod(row.x_modulus, factor, modulus);
    }
    for (const uint64_t root : row.large_prime_sqrt_factors) {
        multiply_mod(row.x_modulus, Integer(root), modulus);
    }
    return row;
}

[[nodiscard]] Integer independent_rhs(const SIQSPostMergeRow& row, const Integer& modulus) {
    Integer rhs(1);
    for (const SIQSFactorPower& power : row.factor_powers) {
        Integer factor;
        mpz_powm_ui(factor.get_mpz(),
                    Integer(static_cast<uint64_t>(factor_base[power.factor_base_index])).get_mpz(),
                    static_cast<unsigned long>(power.exponent), modulus.get_mpz());
        multiply_mod(rhs, factor, modulus);
    }
    for (const uint64_t root : row.large_prime_sqrt_factors) {
        Integer factor(root);
        mpz_mul(factor.get_mpz(), factor.get_mpz(), factor.get_mpz());
        mpz_mod(factor.get_mpz(), factor.get_mpz(), modulus.get_mpz());
        multiply_mod(rhs, factor, modulus);
    }
    return rhs;
}

[[nodiscard]] bool same_row(const SIQSPostMergeRow& lhs, const SIQSPostMergeRow& rhs) {
    return lhs.x_modulus == rhs.x_modulus && lhs.q_negative == rhs.q_negative &&
           lhs.factor_powers == rhs.factor_powers &&
           lhs.large_prime_sqrt_factors == rhs.large_prime_sqrt_factors &&
           lhs.source_ids == rhs.source_ids;
}

template <class Result> void expect_error(const Result& result, SIQSPostMergeRowWireError error) {
    CHECK(!result);
    CHECK(!result.row.has_value());
    CHECK(result.error == error);
}

void test_golden_wire() {
    // x=0 is represented by one zero byte. This frame is also an independent
    // little-endian layout oracle for every scalar field and array boundary.
    SIQSPostMergeRow row{Integer(0), true, {{1, 1}}, {wide_roots[0]}, {{7}}};
    const auto encoded = encode_siqs_post_merge_row(row);
    CHECK(encoded);
    if (!encoded.bytes) {
        return;
    }

    std::vector<std::byte> expected(65, std::byte{0});
    const std::array<char, 8> magic{'G', 'N', 'F', 'S', 'Q', 'R', 'O', 'W'};
    for (std::size_t i = 0; i < magic.size(); ++i) {
        expected[i] = static_cast<std::byte>(static_cast<uint8_t>(magic[i]));
    }
    put_u32(expected, 8, 1);
    put_u64(expected, 12, expected.size());
    expected[20] = std::byte{1};
    put_u32(expected, 24, 1);
    put_u32(expected, 28, 1);
    put_u32(expected, 32, 1);
    put_u32(expected, 36, 1);
    put_u32(expected, 41, 1);
    put_u32(expected, 45, 1);
    put_u64(expected, 49, wide_roots[0]);
    put_u64(expected, 57, 7);
    CHECK(*encoded.bytes == expected);

    const auto decoded = decode_siqs_post_merge_row(*encoded.bytes);
    CHECK(decoded);
    if (decoded.row) {
        CHECK(same_row(*decoded.row, row));
    }
}

void test_cross_size_arithmetic_and_parity() {
    const std::array<std::array<const char*, 2>, 3> factors{{
        {"3000000000000000000020161", "4000000000000000000011001"},
        {"30000000000000000000000000000011207", "40000000000000000000000000000001183"},
        {"300000000000000000000000000000000000000044471",
         "400000000000000000000000000000000000000019639"},
    }};
    for (const auto& factor_pair : factors) {
        const Integer modulus = Integer(factor_pair[0]) * Integer(factor_pair[1]);
        const SIQSPostMergeRow row = make_identity_row(modulus);
        const Integer rhs = independent_rhs(row, modulus);
        Integer lhs;
        mpz_mul(lhs.get_mpz(), row.x_modulus.get_mpz(), row.x_modulus.get_mpz());
        mpz_mod(lhs.get_mpz(), lhs.get_mpz(), modulus.get_mpz());
        CHECK(lhs == rhs);
        CHECK(check_siqs_post_merge_row_identity(
                  row, std::span<const uint32_t>(factor_base.data(), factor_base.size()),
                  modulus) == SIQSPostMergeRowStatus::valid);

        const auto encoded = encode_siqs_post_merge_row(row);
        CHECK(encoded);
        if (!encoded.bytes) {
            continue;
        }
        const auto decoded = decode_siqs_post_merge_row(*encoded.bytes);
        CHECK(decoded);
        if (!decoded.row) {
            continue;
        }
        CHECK(same_row(*decoded.row, row));
        CHECK(check_siqs_post_merge_row_identity(
                  *decoded.row, std::span<const uint32_t>(factor_base.data(), factor_base.size()),
                  modulus) == SIQSPostMergeRowStatus::valid);
        CHECK(decoded.row->large_prime_sqrt_factors[0] > std::numeric_limits<uint32_t>::max());

        std::vector<std::size_t> odd_columns;
        const SIQSPostMergeRow parity_row{
            Integer(1), true, {{1, 1}, {2, 2}}, {wide_roots[0]}, {{1}}};
        visit_siqs_post_merge_odd_columns(
            parity_row, [&odd_columns](std::size_t column) { odd_columns.push_back(column); });
        CHECK(odd_columns == std::vector<std::size_t>({0, 1}));
    }
}

void test_rejects_malformed_frames() {
    SIQSPostMergeRow row{Integer(256), false, {{1, 2}}, {wide_roots[0]}, {{7}, {8}}};
    const auto encoded = encode_siqs_post_merge_row(row);
    CHECK(encoded);
    if (!encoded.bytes) {
        return;
    }

    auto truncated = *encoded.bytes;
    truncated.resize(39);
    expect_error(decode_siqs_post_merge_row(truncated), SIQSPostMergeRowWireError::truncated);

    auto trailing = *encoded.bytes;
    trailing.push_back(std::byte{0});
    expect_error(decode_siqs_post_merge_row(trailing), SIQSPostMergeRowWireError::trailing_bytes);

    auto bad_version = *encoded.bytes;
    put_u32(bad_version, 8, 2);
    expect_error(decode_siqs_post_merge_row(bad_version),
                 SIQSPostMergeRowWireError::unsupported_wire_version);

    auto bad_flags = *encoded.bytes;
    bad_flags[20] = std::byte{2};
    expect_error(decode_siqs_post_merge_row(bad_flags), SIQSPostMergeRowWireError::invalid_boolean);

    auto bad_reserved = *encoded.bytes;
    bad_reserved[21] = std::byte{1};
    expect_error(decode_siqs_post_merge_row(bad_reserved),
                 SIQSPostMergeRowWireError::nonzero_reserved);

    auto bad_integer = *encoded.bytes;
    // x=256 is little-endian {0x00, 0x01}; clearing the high byte is noncanonical.
    bad_integer[41] = std::byte{0};
    expect_error(decode_siqs_post_merge_row(bad_integer),
                 SIQSPostMergeRowWireError::noncanonical_integer);

    auto bad_factor = *encoded.bytes;
    put_u32(bad_factor, 42, 0);
    expect_error(decode_siqs_post_merge_row(bad_factor),
                 SIQSPostMergeRowWireError::invalid_factor_powers);

    auto bad_lp = *encoded.bytes;
    put_u64(bad_lp, 50, 1);
    expect_error(decode_siqs_post_merge_row(bad_lp),
                 SIQSPostMergeRowWireError::invalid_large_prime_roots);

    auto bad_sources = *encoded.bytes;
    put_u64(bad_sources, 66, 6);
    expect_error(decode_siqs_post_merge_row(bad_sources),
                 SIQSPostMergeRowWireError::invalid_source_ids);

    auto over_limit = *encoded.bytes;
    put_u64(over_limit, 12,
            static_cast<uint64_t>(gnfs::siqs::SIQS_POST_MERGE_ROW_WIRE_MAX_BYTES) + 1);
    expect_error(decode_siqs_post_merge_row(over_limit), SIQSPostMergeRowWireError::resource_limit);
}

} // namespace

int main() {
    test_golden_wire();
    test_cross_size_arithmetic_and_parity();
    test_rejects_malformed_frames();
    std::cout << "SIQS post-merge row wire: " << checks_passed << " checks passed, "
              << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
