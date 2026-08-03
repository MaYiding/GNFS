#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace gnfs::util {

inline constexpr std::size_t SHA256_DIGEST_BYTES = 32;
inline constexpr std::size_t SHA256_HEX_CHARACTERS = SHA256_DIGEST_BYTES * 2;

struct Sha256Digest final {
    std::array<std::byte, SHA256_DIGEST_BYTES> bytes{};

    [[nodiscard]] friend constexpr bool operator==(const Sha256Digest&,
                                                   const Sha256Digest&) noexcept = default;
};

static_assert(sizeof(Sha256Digest) == SHA256_DIGEST_BYTES);

using Sha256Hex = std::array<char, SHA256_HEX_CHARACTERS>;

/// Incremental SHA-256 with an explicit terminal state. update() rejects input
/// after finalize() and messages whose bit length cannot be encoded by SHA-256.
/// finalize() succeeds exactly once; reset() starts a fresh stream.
class Sha256Accumulator final {
public:
    Sha256Accumulator() noexcept;

    void reset() noexcept;

    [[nodiscard]] bool update(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool update(std::string_view bytes) noexcept;

    [[nodiscard]] std::optional<Sha256Digest> finalize() noexcept;

    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] bool finalized() const noexcept;

private:
    void compress_block(const std::byte* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::byte, 64> buffer_{};
    std::uint64_t total_bytes_ = 0;
    std::size_t buffered_bytes_ = 0;
    bool failed_ = false;
    bool finalized_ = false;
};

[[nodiscard]] std::optional<Sha256Digest> sha256(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] std::optional<Sha256Digest> sha256(std::string_view bytes) noexcept;

/// Encode exactly 32 digest bytes as 64 lowercase hexadecimal characters.
[[nodiscard]] Sha256Hex encode_sha256_hex(const Sha256Digest& digest) noexcept;

/// Decode only the canonical lowercase 64-character representation.
[[nodiscard]] std::optional<Sha256Digest> decode_sha256_hex(std::string_view encoded) noexcept;

} // namespace gnfs::util
