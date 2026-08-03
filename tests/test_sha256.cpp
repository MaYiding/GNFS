#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using gnfs::util::decode_sha256_hex;
using gnfs::util::encode_sha256_hex;
using gnfs::util::sha256;
using gnfs::util::SHA256_DIGEST_BYTES;
using gnfs::util::SHA256_HEX_CHARACTERS;
using gnfs::util::Sha256Accumulator;
using gnfs::util::Sha256Digest;
using gnfs::util::Sha256Hex;

static_assert(SHA256_DIGEST_BYTES == 32);
static_assert(SHA256_HEX_CHARACTERS == 64);
static_assert(sizeof(Sha256Digest) == 32);
static_assert(std::is_trivially_copyable_v<Sha256Digest>);
static_assert(noexcept(Sha256Accumulator{}));
static_assert(noexcept(std::declval<Sha256Accumulator&>().update(std::string_view{})));
static_assert(noexcept(std::declval<Sha256Accumulator&>().finalize()));
static_assert(noexcept(sha256(std::string_view{})));
static_assert(noexcept(encode_sha256_hex(Sha256Digest{})));
static_assert(noexcept(decode_sha256_hex(std::string_view{})));

int checks_passed = 0;
int checks_failed = 0;

void expect(bool condition, const char* expression, int line) {
    if (condition) {
        ++checks_passed;
        return;
    }
    ++checks_failed;
    std::cerr << "FAIL: " << expression << " at " << __FILE__ << ':' << line << '\n';
}

#define EXPECT(condition) expect(static_cast<bool>(condition), #condition, __LINE__)

[[nodiscard]] std::string_view view(const Sha256Hex& encoded) noexcept {
    return {encoded.data(), encoded.size()};
}

[[nodiscard]] std::optional<Sha256Digest> digest_of(std::string_view message) noexcept {
    return sha256(message);
}

void expect_vector(std::string_view message, std::string_view expected) {
    const auto digest = digest_of(message);
    EXPECT(digest.has_value());
    if (!digest.has_value()) {
        return;
    }
    const Sha256Hex encoded = encode_sha256_hex(*digest);
    EXPECT(view(encoded) == expected);

    const auto decoded = decode_sha256_hex(expected);
    EXPECT(decoded.has_value());
    EXPECT(decoded == digest);
}

void test_standard_vectors() {
    expect_vector("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect_vector("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const std::string million_a(1'000'000, 'a');
    expect_vector(million_a, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void test_padding_boundaries() {
    struct BoundaryVector final {
        std::size_t size;
        std::string_view digest;
    };
    constexpr std::array<BoundaryVector, 5> VECTORS = {{
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    }};

    for (const auto& vector : VECTORS) {
        expect_vector(std::string(vector.size, 'a'), vector.digest);
    }
}

void test_chunking_invariance() {
    std::string message(4097, '\0');
    for (std::size_t index = 0; index < message.size(); ++index) {
        message[index] = static_cast<char>('!' + (index % 90));
    }

    const auto expected = sha256(message);
    EXPECT(expected.has_value());

    constexpr std::array<std::size_t, 10> CHUNKS = {1, 2, 3, 7, 55, 56, 63, 64, 65, 257};
    for (const std::size_t chunk_size : CHUNKS) {
        Sha256Accumulator accumulator;
        EXPECT(accumulator.update(std::string_view{}));
        for (std::size_t offset = 0; offset < message.size(); offset += chunk_size) {
            const std::size_t count = std::min(chunk_size, message.size() - offset);
            EXPECT(accumulator.update(std::string_view(message).substr(offset, count)));
        }
        EXPECT(accumulator.finalize() == expected);
    }

    Sha256Accumulator mixed;
    EXPECT(mixed.update(std::string_view(message).substr(0, 17)));
    const auto bytes =
        std::as_bytes(std::span<const char>(message.data() + 17, message.size() - 17));
    EXPECT(mixed.update(bytes));
    EXPECT(mixed.finalize() == expected);
}

void test_accumulator_state_machine() {
    Sha256Accumulator accumulator;
    EXPECT(!accumulator.failed());
    EXPECT(!accumulator.finalized());
    EXPECT(accumulator.update("abc"));

    const auto first = accumulator.finalize();
    EXPECT(first.has_value());
    EXPECT(accumulator.finalized());
    EXPECT(!accumulator.failed());
    EXPECT(!accumulator.update("more"));
    EXPECT(!accumulator.finalize().has_value());

    accumulator.reset();
    EXPECT(!accumulator.failed());
    EXPECT(!accumulator.finalized());
    EXPECT(accumulator.update("abc"));
    EXPECT(accumulator.finalize() == first);
}

void test_canonical_hex_codec() {
    constexpr std::string_view ABC =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const auto decoded = decode_sha256_hex(ABC);
    EXPECT(decoded.has_value());
    if (decoded.has_value()) {
        EXPECT(view(encode_sha256_hex(*decoded)) == ABC);
    }

    EXPECT(!decode_sha256_hex("").has_value());
    EXPECT(!decode_sha256_hex(ABC.substr(0, 63)).has_value());
    EXPECT(!decode_sha256_hex(std::string(ABC) + "0").has_value());

    std::string invalid(ABC);
    invalid[0] = 'B';
    EXPECT(!decode_sha256_hex(invalid).has_value());
    invalid = ABC;
    invalid[17] = 'g';
    EXPECT(!decode_sha256_hex(invalid).has_value());
    invalid = ABC;
    invalid[42] = ' ';
    EXPECT(!decode_sha256_hex(invalid).has_value());
    invalid = ABC;
    invalid[31] = '\0';
    EXPECT(!decode_sha256_hex(std::string_view(invalid.data(), invalid.size())).has_value());

    const std::string zeros(SHA256_HEX_CHARACTERS, '0');
    const auto zero_digest = decode_sha256_hex(zeros);
    EXPECT(zero_digest.has_value());
    if (zero_digest.has_value()) {
        EXPECT(view(encode_sha256_hex(*zero_digest)) == zeros);
    }
}

} // namespace

int main() {
    test_standard_vectors();
    test_padding_boundaries();
    test_chunking_invariance();
    test_accumulator_state_machine();
    test_canonical_hex_codec();

    std::cout << "SHA-256 checks: " << checks_passed << " passed, " << checks_failed << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
