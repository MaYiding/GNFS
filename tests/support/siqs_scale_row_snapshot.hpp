#pragma once

#include "../fixtures/siqs_multi_a_scale_profile_v3.hpp"

#include <gnfs/siqs/post_merge_row_wire.hpp>
#include <gnfs/siqs/shadow_assembly.hpp>
#include <gnfs/util/native_random_access_file.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace gnfs::tests::siqs_scale_row_snapshot {

inline constexpr std::size_t MAX_FILE_BYTES = 4 * 1024 * 1024;

inline void append_le(std::vector<std::byte>& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        bytes.push_back(static_cast<std::byte>(value & 0xffU));
        value >>= 8;
    }
}

// The fixed V3 consumer regenerates the complete expected header. No allocation,
// table size, modulus, or corpus identity is taken from unverified input.
[[nodiscard]] inline std::vector<std::byte> header(std::span<const std::uint32_t> primes,
                                                   const core::Integer& square_modulus,
                                                   std::size_t trim_excess) {
    const auto& fixture = SIQS_MULTI_A_SCALE_FIXTURE_V3;
    const auto& golden = fixture.golden;
    std::vector<std::byte> bytes;
    const auto line = [&bytes](std::string_view text) {
        const auto data = std::as_bytes(std::span(text));
        bytes.insert(bytes.end(), data.begin(), data.end());
        bytes.push_back(std::byte{'\n'});
    };
    line("GNFS-SIQS-SELECTED-ROWS-V1");
    line(fixture.profile_id);
    line(fixture.modulus);
    line(square_modulus.to_string());
    for (const auto value :
         {golden.plan_digest_low, golden.plan_digest_high, golden.raw_digest_low,
          golden.raw_digest_high, golden.source_fingerprint_low, golden.source_fingerprint_high,
          golden.pretrim_fingerprint_low, golden.pretrim_fingerprint_high,
          golden.selected_fingerprint_low, golden.selected_fingerprint_high}) {
        append_le(bytes, value, 8);
    }
    append_le(bytes, trim_excess, 4);
    append_le(bytes, golden.selected_rows, 4);
    append_le(bytes, primes.size(), 4);
    for (const auto prime : primes) {
        append_le(bytes, prime, 4);
    }
    return bytes;
}

[[nodiscard]] inline std::vector<std::byte> encode(std::span<const siqs::SIQSShadowRow> rows,
                                                   std::span<const std::uint32_t> primes,
                                                   const core::Integer& square_modulus,
                                                   std::size_t trim_excess) {
    if (rows.size() != SIQS_MULTI_A_SCALE_FIXTURE_V3.golden.selected_rows) {
        throw std::runtime_error("unexpected V3 selected row count");
    }
    auto bytes = header(primes, square_modulus, trim_excess);
    for (const auto& row : rows) {
        const auto encoded = siqs::encode_siqs_post_merge_row(row.row);
        if (!encoded) {
            throw std::runtime_error("cannot encode selected V3 row");
        }
        constexpr std::size_t frame_prefix = 5;
        if (bytes.size() > MAX_FILE_BYTES - frame_prefix ||
            encoded.bytes->size() > MAX_FILE_BYTES - frame_prefix - bytes.size()) {
            throw std::length_error("V3 row snapshot exceeds byte limit");
        }
        bytes.push_back(static_cast<std::byte>(row.origin));
        append_le(bytes, encoded.bytes->size(), 4);
        bytes.insert(bytes.end(), encoded.bytes->begin(), encoded.bytes->end());
    }
    return bytes;
}

[[nodiscard]] inline std::vector<siqs::SIQSShadowRow> decode(std::span<const std::byte> bytes,
                                                             std::span<const std::uint32_t> primes,
                                                             const core::Integer& square_modulus,
                                                             std::size_t trim_excess) {
    const auto prefix = header(primes, square_modulus, trim_excess);
    if (bytes.size() > MAX_FILE_BYTES || bytes.size() < prefix.size() ||
        !std::equal(prefix.begin(), prefix.end(), bytes.begin())) {
        throw std::runtime_error("V3 row snapshot header or factor base mismatch");
    }
    bytes = bytes.subspan(prefix.size());
    const auto& golden = SIQS_MULTI_A_SCALE_FIXTURE_V3.golden;
    std::vector<siqs::SIQSShadowRow> rows;
    rows.reserve(golden.selected_rows);
    for (std::size_t i = 0; i < golden.selected_rows; ++i) {
        if (bytes.size() < 5) {
            throw std::runtime_error("truncated V3 row frame");
        }
        const auto origin = static_cast<siqs::SIQSShadowRowOrigin>(bytes.front());
        if (origin != siqs::SIQSShadowRowOrigin::raw_full &&
            origin != siqs::SIQSShadowRowOrigin::large_prime_cycle) {
            throw std::runtime_error("invalid V3 row origin");
        }
        std::uint32_t size = 0;
        for (unsigned j = 0; j < 4; ++j) {
            size |= std::to_integer<std::uint32_t>(bytes[j + 1]) << (8 * j);
        }
        bytes = bytes.subspan(5);
        if (size > bytes.size()) {
            throw std::runtime_error("truncated V3 row payload");
        }
        auto decoded = siqs::decode_siqs_post_merge_row(bytes.first(size));
        if (!decoded ||
            siqs::check_siqs_post_merge_row_identity(*decoded.row, primes, square_modulus) !=
                siqs::SIQSPostMergeRowStatus::valid) {
            throw std::runtime_error("invalid V3 row encoding or arithmetic identity");
        }
        rows.push_back({origin, std::move(*decoded.row)});
        bytes = bytes.subspan(size);
    }
    if (!bytes.empty()) {
        throw std::runtime_error("trailing data after V3 selected rows");
    }
    const auto selected = siqs::shadow_assembly_detail::fingerprint_selected_rows(
        {golden.source_fingerprint_low, golden.source_fingerprint_high},
        {golden.pretrim_fingerprint_low, golden.pretrim_fingerprint_high}, primes.size(),
        trim_excess, rows);
    if (selected != siqs::SIQSShadowFingerprint{golden.selected_fingerprint_low,
                                                golden.selected_fingerprint_high}) {
        throw std::runtime_error("selected V3 rows differ from the frozen corpus");
    }
    return rows;
}

[[nodiscard]] inline std::vector<std::byte> read(const std::filesystem::path& file) {
    auto input = util::NativeRandomAccessFile::open_read_only(file);
    const auto size = input.size();
    if (size > MAX_FILE_BYTES) {
        throw std::length_error("V3 row snapshot exceeds byte limit");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read_exact_at(0, bytes);
    return bytes;
}

} // namespace gnfs::tests::siqs_scale_row_snapshot
