#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace gnfs::util {
namespace {

constexpr std::array<std::uint32_t, 8> INITIAL_STATE = {
    UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85), UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
    UINT32_C(0x510e527f), UINT32_C(0x9b05688c), UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19),
};

constexpr std::array<std::uint32_t, 64> ROUND_CONSTANTS = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
    UINT32_C(0x3956c25b), UINT32_C(0x59f111f1), UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
    UINT32_C(0xd807aa98), UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
    UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786), UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
    UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
    UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147), UINT32_C(0x06ca6351), UINT32_C(0x14292967),
    UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
    UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b), UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
    UINT32_C(0xd192e819), UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
    UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a), UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
    UINT32_C(0x748f82ee), UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2),
};

constexpr std::uint64_t MAX_MESSAGE_BYTES = std::numeric_limits<std::uint64_t>::max() / 8;

static_assert(std::numeric_limits<std::size_t>::digits <=
              std::numeric_limits<std::uint64_t>::digits);

[[nodiscard]] constexpr std::uint8_t byte_value(std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] std::uint32_t load_be_u32(const std::byte* bytes) noexcept {
    return (static_cast<std::uint32_t>(byte_value(bytes[0])) << 24U) |
           (static_cast<std::uint32_t>(byte_value(bytes[1])) << 16U) |
           (static_cast<std::uint32_t>(byte_value(bytes[2])) << 8U) |
           static_cast<std::uint32_t>(byte_value(bytes[3]));
}

void store_be_u32(std::uint32_t value, std::byte* bytes) noexcept {
    bytes[0] = static_cast<std::byte>(value >> 24U);
    bytes[1] = static_cast<std::byte>(value >> 16U);
    bytes[2] = static_cast<std::byte>(value >> 8U);
    bytes[3] = static_cast<std::byte>(value);
}

[[nodiscard]] constexpr int lowercase_hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

} // namespace

Sha256Accumulator::Sha256Accumulator() noexcept {
    reset();
}

void Sha256Accumulator::reset() noexcept {
    state_ = INITIAL_STATE;
    buffer_.fill(std::byte{0});
    total_bytes_ = 0;
    buffered_bytes_ = 0;
    failed_ = false;
    finalized_ = false;
}

bool Sha256Accumulator::update(std::span<const std::byte> bytes) noexcept {
    if (failed_ || finalized_) {
        return false;
    }

    const std::uint64_t byte_count = static_cast<std::uint64_t>(bytes.size());
    if (byte_count > MAX_MESSAGE_BYTES - total_bytes_) {
        failed_ = true;
        return false;
    }
    total_bytes_ += byte_count;

    if (buffered_bytes_ != 0) {
        const std::size_t copied = std::min(buffer_.size() - buffered_bytes_, bytes.size());
        if (copied != 0) {
            std::memcpy(buffer_.data() + buffered_bytes_, bytes.data(), copied);
            buffered_bytes_ += copied;
            bytes = bytes.subspan(copied);
        }
        if (buffered_bytes_ == buffer_.size()) {
            compress_block(buffer_.data());
            buffered_bytes_ = 0;
        }
    }

    while (bytes.size() >= buffer_.size()) {
        compress_block(bytes.data());
        bytes = bytes.subspan(buffer_.size());
    }

    if (!bytes.empty()) {
        std::memcpy(buffer_.data(), bytes.data(), bytes.size());
        buffered_bytes_ = bytes.size();
    }
    return true;
}

bool Sha256Accumulator::update(std::string_view bytes) noexcept {
    return update(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
}

std::optional<Sha256Digest> Sha256Accumulator::finalize() noexcept {
    if (failed_ || finalized_) {
        return std::nullopt;
    }
    finalized_ = true;

    const std::uint64_t bit_length = total_bytes_ * 8;
    buffer_[buffered_bytes_++] = std::byte{0x80};
    if (buffered_bytes_ > 56) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_bytes_), buffer_.end(),
                  std::byte{0});
        compress_block(buffer_.data());
        buffered_bytes_ = 0;
    }

    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_bytes_), buffer_.begin() + 56,
              std::byte{0});
    for (std::size_t index = 0; index < 8; ++index) {
        const unsigned shift = static_cast<unsigned>((7 - index) * 8);
        buffer_[56 + index] = static_cast<std::byte>(bit_length >> shift);
    }
    compress_block(buffer_.data());

    Sha256Digest digest;
    for (std::size_t index = 0; index < state_.size(); ++index) {
        store_be_u32(state_[index], digest.bytes.data() + index * 4);
    }
    return digest;
}

bool Sha256Accumulator::failed() const noexcept {
    return failed_;
}

bool Sha256Accumulator::finalized() const noexcept {
    return finalized_;
}

void Sha256Accumulator::compress_block(const std::byte* block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
        schedule[index] = load_be_u32(block + index * 4);
    }
    for (std::size_t index = 16; index < schedule.size(); ++index) {
        const std::uint32_t s0 = std::rotr(schedule[index - 15], 7) ^
                                 std::rotr(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3U);
        const std::uint32_t s1 = std::rotr(schedule[index - 2], 17) ^
                                 std::rotr(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10U);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0; index < schedule.size(); ++index) {
        const std::uint32_t upper_sigma_1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temporary_1 =
            h + upper_sigma_1 + choose + ROUND_CONSTANTS[index] + schedule[index];
        const std::uint32_t upper_sigma_0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary_2 = upper_sigma_0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary_1;
        d = c;
        c = b;
        b = a;
        a = temporary_1 + temporary_2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

std::optional<Sha256Digest> sha256(std::span<const std::byte> bytes) noexcept {
    Sha256Accumulator accumulator;
    if (!accumulator.update(bytes)) {
        return std::nullopt;
    }
    return accumulator.finalize();
}

std::optional<Sha256Digest> sha256(std::string_view bytes) noexcept {
    Sha256Accumulator accumulator;
    if (!accumulator.update(bytes)) {
        return std::nullopt;
    }
    return accumulator.finalize();
}

Sha256Hex encode_sha256_hex(const Sha256Digest& digest) noexcept {
    constexpr std::string_view HEX = "0123456789abcdef";
    Sha256Hex encoded{};
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        const std::uint8_t value = byte_value(digest.bytes[index]);
        encoded[index * 2] = HEX[static_cast<std::size_t>(value >> 4U)];
        encoded[index * 2 + 1] = HEX[static_cast<std::size_t>(value & UINT8_C(0x0f))];
    }
    return encoded;
}

std::optional<Sha256Digest> decode_sha256_hex(std::string_view encoded) noexcept {
    if (encoded.size() != SHA256_HEX_CHARACTERS) {
        return std::nullopt;
    }

    Sha256Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        const int upper = lowercase_hex_value(encoded[index * 2]);
        const int lower = lowercase_hex_value(encoded[index * 2 + 1]);
        if (upper < 0 || lower < 0) {
            return std::nullopt;
        }
        digest.bytes[index] = static_cast<std::byte>(static_cast<unsigned>((upper << 4) | lower));
    }
    return digest;
}

} // namespace gnfs::util
