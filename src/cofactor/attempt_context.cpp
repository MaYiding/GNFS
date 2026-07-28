#include "gnfs/cofactor/attempt_context.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace gnfs::cofactor {
namespace {

constexpr std::string_view INPUT_DOMAIN = "GNFS-COFACTOR-INPUT-V1";
constexpr std::string_view RANDOM_STREAM_DOMAIN = "GNFS-COFACTOR-RANDOM-STREAM-V1";

static_assert(std::numeric_limits<std::size_t>::digits <=
              std::numeric_limits<std::uint64_t>::digits);

[[nodiscard]] constexpr std::array<std::byte, 8> encode_u64_be(std::uint64_t value) noexcept {
    std::array<std::byte, 8> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const unsigned shift = static_cast<unsigned>((encoded.size() - 1 - index) * 8);
        encoded[index] = static_cast<std::byte>(value >> shift);
    }
    return encoded;
}

[[nodiscard]] constexpr std::uint8_t side_tag(CofactorSide side) {
    switch (side) {
    case CofactorSide::rational:
        return 0;
    case CofactorSide::algebraic:
        return 1;
    }
    throw std::invalid_argument("unknown cofactor side");
}

void append_or_throw(util::Sha256Accumulator& accumulator, std::string_view bytes) {
    if (!accumulator.update(bytes)) {
        throw std::length_error("cofactor SHA-256 input exceeds the supported length");
    }
}

void append_or_throw(util::Sha256Accumulator& accumulator, std::span<const std::byte> bytes) {
    if (!accumulator.update(bytes)) {
        throw std::length_error("cofactor SHA-256 input exceeds the supported length");
    }
}

[[nodiscard]] util::Sha256Digest finalize_or_throw(util::Sha256Accumulator& accumulator) {
    auto digest = accumulator.finalize();
    if (!digest) {
        throw std::logic_error("cofactor SHA-256 finalization failed");
    }
    return *digest;
}

[[nodiscard]] std::vector<std::byte> export_unsigned_magnitude(const core::Integer& value) {
    if (value.is_zero()) {
        return {};
    }

    const std::size_t bit_count = mpz_sizeinbase(value.get_mpz(), 2);
    const std::size_t byte_count = bit_count / 8 + static_cast<std::size_t>(bit_count % 8 != 0);
    std::vector<std::byte> magnitude(byte_count);

    std::size_t written = 0;
    mpz_export(magnitude.data(), &written, /*order=*/1, /*size=*/1, /*endian=*/1,
               /*nails=*/0, value.get_mpz());
    if (written > magnitude.size()) {
        throw std::logic_error("GMP exported an oversized cofactor magnitude");
    }
    magnitude.resize(written);
    return magnitude;
}

[[nodiscard]] std::uint64_t first_u64_be(const util::Sha256Digest& digest) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(digest.bytes[index]);
    }
    return value;
}

} // namespace

util::Sha256Digest canonical_cofactor_input_digest(const core::Integer& cofactor,
                                                   CofactorSide side) {
    const std::uint8_t tag = side_tag(side);
    const std::vector<std::byte> magnitude = export_unsigned_magnitude(cofactor);
    const auto magnitude_length = encode_u64_be(static_cast<std::uint64_t>(magnitude.size()));
    const std::array<std::byte, 1> encoded_side{static_cast<std::byte>(tag)};

    util::Sha256Accumulator accumulator;
    append_or_throw(accumulator, INPUT_DOMAIN);
    append_or_throw(accumulator, encoded_side);
    append_or_throw(accumulator, magnitude_length);
    append_or_throw(accumulator, magnitude);
    return finalize_or_throw(accumulator);
}

util::Sha256Digest cofactor_random_block(const CofactorSeed256& seed, std::uint64_t draw_ordinal) {
    const auto encoded_ordinal = encode_u64_be(draw_ordinal);

    util::Sha256Accumulator accumulator;
    append_or_throw(accumulator, RANDOM_STREAM_DOMAIN);
    append_or_throw(accumulator, seed.digest.bytes);
    append_or_throw(accumulator, encoded_ordinal);
    return finalize_or_throw(accumulator);
}

std::uint64_t cofactor_random_u64(const CofactorSeed256& seed, std::uint64_t draw_ordinal) {
    return first_u64_be(cofactor_random_block(seed, draw_ordinal));
}

Seed256Stream::Seed256Stream(CofactorSeed256 seed, std::uint64_t initial_draw_ordinal) noexcept
    : seed_(seed), draw_ordinal_(initial_draw_ordinal) {}

std::optional<util::Sha256Digest> Seed256Stream::next_block() {
    if (exhausted_) {
        return std::nullopt;
    }

    util::Sha256Digest block = cofactor_random_block(seed_, draw_ordinal_);
    advance();
    return block;
}

std::optional<std::uint64_t> Seed256Stream::next_u64() {
    if (exhausted_) {
        return std::nullopt;
    }

    const std::uint64_t value = cofactor_random_u64(seed_, draw_ordinal_);
    advance();
    return value;
}

bool Seed256Stream::exhausted() const noexcept {
    return exhausted_;
}

void Seed256Stream::advance() noexcept {
    if (draw_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
        exhausted_ = true;
        return;
    }
    ++draw_ordinal_;
}

} // namespace gnfs::cofactor
