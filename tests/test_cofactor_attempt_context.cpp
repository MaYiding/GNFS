// test_cofactor_attempt_context.cpp - canonical cofactor identity and seed stream contracts

#include <gnfs/cofactor/attempt_context.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using gnfs::cofactor::CofactorAttemptContext;
using gnfs::cofactor::CofactorAttemptCoordinates;
using gnfs::cofactor::CofactorSeed256;
using gnfs::cofactor::CofactorSide;
using gnfs::cofactor::Seed256Stream;
using gnfs::core::Integer;
using gnfs::util::Sha256Digest;

[[noreturn]] void fail(std::string_view expression, const char* file, int line) {
    throw std::runtime_error(std::string("CHECK failed: ") + std::string(expression) + " at " +
                             file + ":" + std::to_string(line));
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fail(#condition, __FILE__, __LINE__);                                                  \
        }                                                                                          \
    } while (false)

constexpr std::string_view INPUT_DOMAIN = "GNFS-COFACTOR-INPUT-V1";
constexpr std::string_view STREAM_DOMAIN = "GNFS-COFACTOR-RANDOM-STREAM-V1";
constexpr char HEX_DIGITS[] = "0123456789abcdef";

constexpr std::string_view RATIONAL_ZERO_PREIMAGE_HEX =
    "474e46532d434f464143544f522d494e5055542d5631000000000000000000";
constexpr std::string_view RATIONAL_010203_PREIMAGE_HEX =
    "474e46532d434f464143544f522d494e5055542d5631000000000000000003010203";
constexpr std::string_view ALGEBRAIC_010203_PREIMAGE_HEX =
    "474e46532d434f464143544f522d494e5055542d5631010000000000000003010203";

constexpr std::string_view RATIONAL_ZERO_DIGEST =
    "b4a7407d07a242d8eaf303b882b59e05bbb99f953510ab6e13a25dde053290f0";
constexpr std::string_view ALGEBRAIC_ZERO_DIGEST =
    "d72d729c2b9142bdd5e0afd980a4663dc2f928c472b4b82267b6862bfa6e5aec";
constexpr std::string_view RATIONAL_010203_DIGEST =
    "8e62046fd5a63aa33372f2bddbd14667cba3fac16fd92e2d4f1ac763291f014b";
constexpr std::string_view ALGEBRAIC_010203_DIGEST =
    "9d8e0dd015ed419f3033d2fcb637aa6127bfd5ec2860279085ffc6e77c3788d4";
constexpr std::string_view RATIONAL_0100_DIGEST =
    "55be8c60a7c3fb771d909937dd467248b54f59e3279df2759dd90c42a51ffff4";

struct StreamGolden final {
    std::uint64_t ordinal;
    std::string_view block_hex;
    std::uint64_t first_u64_be;
};

constexpr std::array<StreamGolden, 6> STREAM_GOLDENS{{
    {0, "bb30277292bceabc118df2093186213858fde6ff91fef4d4edc2f4f201fc56f9", 0xbb30277292bceabcULL},
    {1, "0d1818962ae4bcd0f03590a2cb0593e37f886daf808e47c2357672a5aec8d6c9", 0x0d1818962ae4bcd0ULL},
    {2, "90ee07e6971254c3defd63174fa040a061318b8e6a9618f547aa2d22500fc95f", 0x90ee07e6971254c3ULL},
    {3, "b1a5f6c80173d4751aace76e571de4394027cf27966cd08e2a876f7d7b961949", 0xb1a5f6c80173d475ULL},
    {4, "81fbb9147f8ce9186941f5a152e007684599482cb404cf0ce88492cb5a9dfc6b", 0x81fbb9147f8ce918ULL},
    {std::numeric_limits<std::uint64_t>::max(),
     "c474e0f68ad6990d2a702f0884e700691e0ea60b931a41a8bbfd84fba9a07159", 0xc474e0f68ad6990dULL},
}};

constexpr std::array<StreamGolden, 2> RANGE_SEED_GOLDENS{{
    {0, "7660a28380ac29cdac466103e74159262873faf1cdff1a2c98c91f5a392501f1", 0x7660a28380ac29cdULL},
    {std::numeric_limits<std::uint64_t>::max(),
     "4d6a0707dc009b6ff61f1cb00e9142150be7f1fd311c7a9cc473f71b8bd29f17", 0x4d6a0707dc009b6fULL},
}};

using Bytes = std::vector<std::byte>;

void append_ascii(Bytes& output, std::string_view input) {
    output.reserve(output.size() + input.size());
    for (const char character : input) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

void append_u64_be(Bytes& output, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const unsigned shift = static_cast<unsigned>((sizeof(value) - 1 - index) * 8);
        output.push_back(static_cast<std::byte>(value >> shift));
    }
}

[[nodiscard]] Bytes make_input_preimage(std::uint8_t side, std::span<const std::byte> magnitude) {
    Bytes preimage;
    append_ascii(preimage, INPUT_DOMAIN);
    preimage.push_back(static_cast<std::byte>(side));
    append_u64_be(preimage, static_cast<std::uint64_t>(magnitude.size()));
    preimage.insert(preimage.end(), magnitude.begin(), magnitude.end());
    return preimage;
}

[[nodiscard]] Bytes make_stream_preimage(const CofactorSeed256& seed, std::uint64_t ordinal) {
    Bytes preimage;
    append_ascii(preimage, STREAM_DOMAIN);
    preimage.insert(preimage.end(), seed.digest.bytes.begin(), seed.digest.bytes.end());
    append_u64_be(preimage, ordinal);
    return preimage;
}

[[nodiscard]] std::string bytes_hex(std::span<const std::byte> bytes) {
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        encoded.push_back(HEX_DIGITS[value >> 4U]);
        encoded.push_back(HEX_DIGITS[value & 0x0fU]);
    }
    return encoded;
}

[[nodiscard]] std::string digest_hex(const Sha256Digest& digest) {
    const auto encoded = gnfs::util::encode_sha256_hex(digest);
    return {encoded.begin(), encoded.end()};
}

[[nodiscard]] Sha256Digest hash_preimage(const Bytes& preimage) {
    const auto digest =
        gnfs::util::sha256(std::span<const std::byte>(preimage.data(), preimage.size()));
    CHECK(digest.has_value());
    return *digest;
}

[[nodiscard]] std::uint64_t first_u64_be(const Sha256Digest& digest) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(digest.bytes[index]);
    }
    return value;
}

template <class Function> void expect_invalid_argument(Function&& function) {
    bool caught = false;
    try {
        std::forward<Function>(function)();
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    CHECK(caught);
}

void test_input_wire_and_digest_goldens() {
    const std::array<std::byte, 3> magnitude{
        std::byte{0x01},
        std::byte{0x02},
        std::byte{0x03},
    };
    const Bytes rational_zero = make_input_preimage(0, {});
    const Bytes rational_value = make_input_preimage(0, magnitude);
    const Bytes algebraic_value = make_input_preimage(1, magnitude);

    CHECK(bytes_hex(rational_zero) == RATIONAL_ZERO_PREIMAGE_HEX);
    CHECK(bytes_hex(rational_value) == RATIONAL_010203_PREIMAGE_HEX);
    CHECK(bytes_hex(algebraic_value) == ALGEBRAIC_010203_PREIMAGE_HEX);
    CHECK(rational_zero.size() == INPUT_DOMAIN.size() + 1 + sizeof(std::uint64_t));
    CHECK(rational_value.size() == rational_zero.size() + magnitude.size());

    const std::size_t side_offset = INPUT_DOMAIN.size();
    const std::size_t length_offset = side_offset + 1;
    CHECK(std::to_integer<std::uint8_t>(rational_value[side_offset]) == 0);
    CHECK(std::to_integer<std::uint8_t>(algebraic_value[side_offset]) == 1);
    CHECK(bytes_hex(std::span<const std::byte>(rational_value).subspan(length_offset, 8)) ==
          "0000000000000003");
    CHECK(bytes_hex(std::span<const std::byte>(rational_zero).subspan(length_offset, 8)) ==
          "0000000000000000");

    CHECK(digest_hex(hash_preimage(rational_zero)) == RATIONAL_ZERO_DIGEST);
    CHECK(digest_hex(hash_preimage(rational_value)) == RATIONAL_010203_DIGEST);
    CHECK(digest_hex(hash_preimage(algebraic_value)) == ALGEBRAIC_010203_DIGEST);

    CHECK(digest_hex(gnfs::cofactor::canonical_cofactor_input_digest(
              Integer(0), CofactorSide::rational)) == RATIONAL_ZERO_DIGEST);
    CHECK(digest_hex(gnfs::cofactor::canonical_cofactor_input_digest(
              Integer(0), CofactorSide::algebraic)) == ALGEBRAIC_ZERO_DIGEST);
    CHECK(digest_hex(gnfs::cofactor::canonical_cofactor_input_digest(
              Integer("66051"), CofactorSide::rational)) == RATIONAL_010203_DIGEST);
    CHECK(digest_hex(gnfs::cofactor::canonical_cofactor_input_digest(
              Integer("66051"), CofactorSide::algebraic)) == ALGEBRAIC_010203_DIGEST);
}

void test_sign_side_and_minimal_magnitude() {
    const Integer positive("66051");
    const Integer negative("-66051");
    const Integer decimal_leading_zeroes("00066051");

    const Sha256Digest rational =
        gnfs::cofactor::canonical_cofactor_input_digest(positive, CofactorSide::rational);
    CHECK(rational ==
          gnfs::cofactor::canonical_cofactor_input_digest(negative, CofactorSide::rational));
    CHECK(rational == gnfs::cofactor::canonical_cofactor_input_digest(decimal_leading_zeroes,
                                                                      CofactorSide::rational));
    CHECK(rational !=
          gnfs::cofactor::canonical_cofactor_input_digest(positive, CofactorSide::algebraic));

    const std::array<std::byte, 2> minimal_0100{std::byte{0x01}, std::byte{0x00}};
    const std::array<std::byte, 3> padded_0100{
        std::byte{0x00},
        std::byte{0x01},
        std::byte{0x00},
    };
    const Bytes minimal_preimage = make_input_preimage(0, minimal_0100);
    const Bytes padded_preimage = make_input_preimage(0, padded_0100);
    const Sha256Digest production_0100 =
        gnfs::cofactor::canonical_cofactor_input_digest(Integer("256"), CofactorSide::rational);

    CHECK(bytes_hex(std::span<const std::byte>(minimal_preimage).last(10)) ==
          "00000000000000020100");
    CHECK(digest_hex(production_0100) == RATIONAL_0100_DIGEST);
    CHECK(production_0100 == hash_preimage(minimal_preimage));
    CHECK(production_0100 != hash_preimage(padded_preimage));

    expect_invalid_argument([&] {
        (void)gnfs::cofactor::canonical_cofactor_input_digest(positive,
                                                              static_cast<CofactorSide>(0xff));
    });
}

void test_zero_seed_ordinal_and_endian_goldens() {
    const CofactorSeed256 zero_seed{};
    CHECK(std::all_of(zero_seed.digest.bytes.begin(), zero_seed.digest.bytes.end(),
                      [](std::byte byte) { return byte == std::byte{0}; }));

    const Bytes ordinal_zero_preimage = make_stream_preimage(zero_seed, 0);
    CHECK(ordinal_zero_preimage.size() ==
          STREAM_DOMAIN.size() + zero_seed.digest.bytes.size() + sizeof(std::uint64_t));
    CHECK(bytes_hex(std::span<const std::byte>(ordinal_zero_preimage).last(8)) ==
          "0000000000000000");
    CHECK(digest_hex(hash_preimage(ordinal_zero_preimage)) == STREAM_GOLDENS[0].block_hex);

    for (const StreamGolden& golden : STREAM_GOLDENS) {
        const Sha256Digest block = gnfs::cofactor::cofactor_random_block(zero_seed, golden.ordinal);
        CHECK(digest_hex(block) == golden.block_hex);
        CHECK(first_u64_be(block) == golden.first_u64_be);
        CHECK(gnfs::cofactor::cofactor_random_u64(zero_seed, golden.ordinal) ==
              golden.first_u64_be);
    }

    const Bytes max_preimage =
        make_stream_preimage(zero_seed, std::numeric_limits<std::uint64_t>::max());
    CHECK(bytes_hex(std::span<const std::byte>(max_preimage).last(8)) == "ffffffffffffffff");
    CHECK(digest_hex(hash_preimage(max_preimage)) == STREAM_GOLDENS.back().block_hex);
}

void test_full_width_seed_goldens_and_last_byte_sensitivity() {
    CofactorSeed256 range_seed{};
    for (std::size_t index = 0; index < range_seed.digest.bytes.size(); ++index) {
        range_seed.digest.bytes[index] = static_cast<std::byte>(index);
    }

    for (const StreamGolden& golden : RANGE_SEED_GOLDENS) {
        const Sha256Digest block =
            gnfs::cofactor::cofactor_random_block(range_seed, golden.ordinal);
        CHECK(digest_hex(block) == golden.block_hex);
        CHECK(first_u64_be(block) == golden.first_u64_be);
        CHECK(gnfs::cofactor::cofactor_random_u64(range_seed, golden.ordinal) ==
              golden.first_u64_be);
    }

    CofactorSeed256 last_byte_flipped = range_seed;
    last_byte_flipped.digest.bytes.back() ^= std::byte{0x01};
    CHECK(gnfs::cofactor::cofactor_random_block(last_byte_flipped, 0) !=
          gnfs::cofactor::cofactor_random_block(range_seed, 0));
    CHECK(gnfs::cofactor::cofactor_random_block(last_byte_flipped,
                                                std::numeric_limits<std::uint64_t>::max()) !=
          gnfs::cofactor::cofactor_random_block(range_seed,
                                                std::numeric_limits<std::uint64_t>::max()));
}

using Observation = std::vector<std::pair<Sha256Digest, std::uint64_t>>;

[[nodiscard]] Observation observe_seed(const CofactorSeed256& seed) {
    Observation observations;
    observations.reserve(STREAM_GOLDENS.size());
    for (const StreamGolden& golden : STREAM_GOLDENS) {
        observations.emplace_back(gnfs::cofactor::cofactor_random_block(seed, golden.ordinal),
                                  gnfs::cofactor::cofactor_random_u64(seed, golden.ordinal));
    }
    return observations;
}

void test_cross_instance_cross_thread_and_ordinal_order() {
    const CofactorSeed256 seed{};
    const Observation baseline = observe_seed(seed);
    CHECK(observe_seed(CofactorSeed256{}) == baseline);

    constexpr std::array<std::size_t, STREAM_GOLDENS.size()> shuffled_indices{5, 2, 0, 4, 1, 3};
    for (const std::size_t index : shuffled_indices) {
        CHECK(gnfs::cofactor::cofactor_random_block(seed, STREAM_GOLDENS[index].ordinal) ==
              baseline[index].first);
        CHECK(gnfs::cofactor::cofactor_random_u64(seed, STREAM_GOLDENS[index].ordinal) ==
              baseline[index].second);
    }

    std::array<std::future<Observation>, 4> futures;
    for (auto& future : futures) {
        future = std::async(std::launch::async, [seed] { return observe_seed(seed); });
    }
    for (auto& future : futures) {
        CHECK(future.get() == baseline);
    }

    Seed256Stream first(seed);
    Seed256Stream second(seed);
    CHECK(!first.exhausted());
    CHECK(!second.exhausted());
    for (std::size_t ordinal = 0; ordinal < 5; ++ordinal) {
        const auto first_block = first.next_block();
        const auto second_block = second.next_block();
        CHECK(first_block.has_value());
        CHECK(second_block.has_value());
        CHECK(*first_block == *second_block);
        CHECK(digest_hex(*first_block) == STREAM_GOLDENS[ordinal].block_hex);
    }

    Seed256Stream mixed(seed);
    const auto block_zero = mixed.next_block();
    const auto u64_one = mixed.next_u64();
    const auto block_two = mixed.next_block();
    CHECK(block_zero.has_value());
    CHECK(u64_one.has_value());
    CHECK(block_two.has_value());
    CHECK(digest_hex(*block_zero) == STREAM_GOLDENS[0].block_hex);
    CHECK(*u64_one == STREAM_GOLDENS[1].first_u64_be);
    CHECK(digest_hex(*block_two) == STREAM_GOLDENS[2].block_hex);

    Seed256Stream terminal(seed, std::numeric_limits<std::uint64_t>::max());
    CHECK(!terminal.exhausted());
    const auto terminal_block = terminal.next_block();
    CHECK(terminal_block.has_value());
    CHECK(digest_hex(*terminal_block) == STREAM_GOLDENS.back().block_hex);
    CHECK(terminal.exhausted());
    CHECK(!terminal.next_block().has_value());
    CHECK(!terminal.next_u64().has_value());
}

void test_coordinate_and_context_boundaries() {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    const CofactorAttemptCoordinates minimum_coordinates{};
    const CofactorAttemptCoordinates maximum_coordinates{maximum, maximum};

    CHECK(minimum_coordinates.special_q_index == 0);
    CHECK(minimum_coordinates.candidate_ordinal == 0);
    CHECK(maximum_coordinates.special_q_index == maximum);
    CHECK(maximum_coordinates.candidate_ordinal == maximum);
    CHECK(minimum_coordinates != maximum_coordinates);
    CHECK(static_cast<std::uint8_t>(CofactorSide::rational) == 0);
    CHECK(static_cast<std::uint8_t>(CofactorSide::algebraic) == 1);

    const CofactorAttemptContext default_context{};
    CHECK(default_context.coordinates == minimum_coordinates);
    CHECK(default_context.side == CofactorSide::rational);
    CHECK(std::all_of(default_context.cofactor_digest.bytes.begin(),
                      default_context.cofactor_digest.bytes.end(),
                      [](std::byte byte) { return byte == std::byte{0}; }));
    CHECK(default_context.seed == CofactorSeed256{});

    CofactorAttemptContext boundary_context{};
    boundary_context.coordinates = maximum_coordinates;
    boundary_context.side = CofactorSide::algebraic;
    boundary_context.cofactor_digest =
        gnfs::cofactor::canonical_cofactor_input_digest(Integer("66051"), CofactorSide::algebraic);
    boundary_context.seed.digest = gnfs::cofactor::cofactor_random_block(CofactorSeed256{}, 0);

    const CofactorAttemptContext copy = boundary_context;
    CHECK(copy == boundary_context);
    CHECK(copy.coordinates.special_q_index == maximum);
    CHECK(copy.coordinates.candidate_ordinal == maximum);
    CHECK(copy.side == CofactorSide::algebraic);
    CHECK(digest_hex(copy.cofactor_digest) == ALGEBRAIC_010203_DIGEST);
    CHECK(copy.seed.digest == gnfs::cofactor::cofactor_random_block(CofactorSeed256{}, 0));
}

} // namespace

int main() {
    try {
        test_input_wire_and_digest_goldens();
        test_sign_side_and_minimal_magnitude();
        test_zero_seed_ordinal_and_endian_goldens();
        test_full_width_seed_goldens_and_last_byte_sensitivity();
        test_cross_instance_cross_thread_and_ordinal_order();
        test_coordinate_and_context_boundaries();
    } catch (const std::exception& error) {
        std::cerr << "test_cofactor_attempt_context: " << error.what() << '\n';
        return 1;
    }

    std::cout << "Cofactor attempt context contract tests passed\n";
    return 0;
}
