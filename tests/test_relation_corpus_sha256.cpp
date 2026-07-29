#include <gnfs/relation/relation_corpus_sha256.hpp>

#include <gnfs/core/relation.hpp>
#include <gnfs/util/sha256.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using gnfs::core::PrimePower;
using gnfs::core::Relation;
using gnfs::relation::relation_corpus_sha256_v1;
using gnfs::relation::RELATION_CORPUS_SHA256_VERSION_V1;
using gnfs::relation::RelationCorpusSha256AccumulatorV1;
using gnfs::util::decode_sha256_hex;
using gnfs::util::Sha256Digest;

static_assert(RELATION_CORPUS_SHA256_VERSION_V1 == 1);
static_assert(noexcept(RelationCorpusSha256AccumulatorV1{}));
static_assert(noexcept(
    std::declval<RelationCorpusSha256AccumulatorV1&>().append(std::declval<const Relation&>())));
static_assert(noexcept(std::declval<RelationCorpusSha256AccumulatorV1&>().finalize()));
static_assert(noexcept(std::declval<const RelationCorpusSha256AccumulatorV1&>().count()));
static_assert(noexcept(std::declval<const RelationCorpusSha256AccumulatorV1&>().failed()));
static_assert(noexcept(std::declval<const RelationCorpusSha256AccumulatorV1&>().finalized()));
static_assert(noexcept(relation_corpus_sha256_v1(std::span<const Relation>{})));
static_assert(sizeof(RelationCorpusSha256AccumulatorV1) <= 256);

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

[[nodiscard]] Relation first_relation() {
    Relation relation(-1, UINT64_C(0x0123456789abcdef));
    relation.rational_factors = {0, UINT32_C(0x11223344), UINT32_MAX};
    relation.algebraic_factors = {7, 3};
    relation.rational_large_prime = {
        PrimePower{1009, 0, 1},
        PrimePower{1013, 17, 2},
    };
    relation.algebraic_large_prime = {
        PrimePower{2003, 19, 3},
    };
    relation.extra_ab_pairs = {
        {std::numeric_limits<std::int64_t>::min(), UINT64_MAX},
        {42, 9},
    };
    return relation;
}

[[nodiscard]] Relation second_relation() {
    Relation relation(123456789, 97);
    relation.rational_factors = {5};
    relation.algebraic_large_prime = {
        PrimePower{3001, 41, 1},
        PrimePower{3001, 42, 2},
    };
    return relation;
}

[[nodiscard]] std::optional<Sha256Digest> digest(std::span<const Relation> relations) {
    return relation_corpus_sha256_v1(relations);
}

void expect_digest(std::span<const Relation> relations, std::string_view expected_hex) {
    const auto expected = decode_sha256_hex(expected_hex);
    EXPECT(expected.has_value());
    EXPECT(digest(relations) == expected);
}

void test_fixed_v1_vectors() {
    const std::vector<Relation> empty;
    expect_digest(empty, "a694e89470cc8595fa46418a1b86ddc54689ef0b10efea28684edcc01fa03d6d");

    const std::vector<Relation> one{first_relation()};
    expect_digest(one, "f9a47a00d616c3359cbc9ab0634474729fbed0d504ae53bb1207d5a5efc3f192");

    const std::vector<Relation> two{first_relation(), second_relation()};
    expect_digest(two, "fe04d586ba8575a8ec28c92c1cc2fc333a25dcd7d1b33769feabf07a2c808a9f");
}

void test_streaming_and_terminal_state() {
    const Relation first = first_relation();
    const Relation second = second_relation();
    const std::vector<Relation> expected_rows{first, second};

    RelationCorpusSha256AccumulatorV1 accumulator;
    EXPECT(accumulator.count() == 0);
    EXPECT(!accumulator.failed());
    EXPECT(!accumulator.finalized());
    EXPECT(accumulator.append(first));
    EXPECT(accumulator.count() == 1);
    EXPECT(accumulator.append(second));
    EXPECT(accumulator.count() == 2);
    EXPECT(accumulator.finalize() == digest(expected_rows));
    EXPECT(accumulator.finalized());
    EXPECT(!accumulator.failed());

    EXPECT(!accumulator.append(first));
    EXPECT(accumulator.count() == 2);
    EXPECT(!accumulator.finalize().has_value());
    EXPECT(!accumulator.failed());
}

void test_relation_and_vector_order_are_bound() {
    const std::vector<Relation> forward{first_relation(), second_relation()};
    const std::vector<Relation> reversed{second_relation(), first_relation()};
    const auto forward_digest = digest(forward);
    const auto reversed_digest = digest(reversed);
    EXPECT(forward_digest.has_value());
    EXPECT(reversed_digest.has_value());
    EXPECT(forward_digest != reversed_digest);
    expect_digest(reversed, "903a52e5aaf3ebd9f7fbd2f8f6eccd19fde6fafb4163f3a8538202c3d3f48869");

    Relation factor_order = first_relation();
    std::swap(factor_order.rational_factors[0], factor_order.rational_factors[1]);
    EXPECT(digest(std::span<const Relation>(&factor_order, 1)) !=
           digest(std::span<const Relation>(&forward.front(), 1)));

    Relation algebraic_factor_order = first_relation();
    std::swap(algebraic_factor_order.algebraic_factors[0],
              algebraic_factor_order.algebraic_factors[1]);
    EXPECT(digest(std::span<const Relation>(&algebraic_factor_order, 1)) !=
           digest(std::span<const Relation>(&forward.front(), 1)));

    Relation large_prime_order = first_relation();
    std::swap(large_prime_order.rational_large_prime[0], large_prime_order.rational_large_prime[1]);
    EXPECT(digest(std::span<const Relation>(&large_prime_order, 1)) !=
           digest(std::span<const Relation>(&forward.front(), 1)));

    Relation algebraic_large_prime_forward = first_relation();
    algebraic_large_prime_forward.algebraic_large_prime.push_back(PrimePower{2011, 23, 1});
    Relation algebraic_large_prime_reversed = algebraic_large_prime_forward;
    std::swap(algebraic_large_prime_reversed.algebraic_large_prime[0],
              algebraic_large_prime_reversed.algebraic_large_prime[1]);
    EXPECT(digest(std::span<const Relation>(&algebraic_large_prime_forward, 1)) !=
           digest(std::span<const Relation>(&algebraic_large_prime_reversed, 1)));

    Relation extra_pair_order = first_relation();
    std::swap(extra_pair_order.extra_ab_pairs[0], extra_pair_order.extra_ab_pairs[1]);
    EXPECT(digest(std::span<const Relation>(&extra_pair_order, 1)) !=
           digest(std::span<const Relation>(&forward.front(), 1)));
}

template <typename Mutation> void expect_field_bound(Mutation&& mutation) {
    const Relation baseline = first_relation();
    Relation changed = baseline;
    std::forward<Mutation>(mutation)(changed);
    EXPECT(digest(std::span<const Relation>(&baseline, 1)) !=
           digest(std::span<const Relation>(&changed, 1)));
}

void test_every_semantic_field_is_bound() {
    expect_field_bound([](Relation& value) { value.a = -2; });
    expect_field_bound([](Relation& value) { ++value.b; });
    expect_field_bound([](Relation& value) { ++value.rational_factors[1]; });
    expect_field_bound([](Relation& value) { ++value.algebraic_factors[0]; });
    expect_field_bound([](Relation& value) { ++value.rational_large_prime[0].p; });
    expect_field_bound([](Relation& value) { ++value.rational_large_prime[0].r; });
    expect_field_bound([](Relation& value) { ++value.rational_large_prime[0].e; });
    expect_field_bound([](Relation& value) { ++value.algebraic_large_prime[0].p; });
    expect_field_bound([](Relation& value) { ++value.algebraic_large_prime[0].r; });
    expect_field_bound([](Relation& value) { ++value.algebraic_large_prime[0].e; });
    expect_field_bound([](Relation& value) { ++value.extra_ab_pairs[0].first; });
    expect_field_bound([](Relation& value) { --value.extra_ab_pairs[0].second; });

    const Relation first = first_relation();
    const Relation second = second_relation();
    const std::vector<Relation> prefix{first};
    const std::vector<Relation> extended{first, second};
    EXPECT(digest(prefix) != digest(extended));
}

void test_invalid_persisted_shape_fails_closed() {
    Relation oversized = first_relation();
    oversized.rational_large_prime.resize(Relation::MAX_SERIALIZED_LARGE_PRIMES + 1U);

    RelationCorpusSha256AccumulatorV1 accumulator;
    EXPECT(!accumulator.append(oversized));
    EXPECT(accumulator.failed());
    EXPECT(!accumulator.finalized());
    EXPECT(accumulator.count() == 0);
    EXPECT(!accumulator.finalize().has_value());

    const std::vector<Relation> rows{oversized};
    EXPECT(!digest(rows).has_value());
}

} // namespace

int main() {
    test_fixed_v1_vectors();
    test_streaming_and_terminal_state();
    test_relation_and_vector_order_are_bound();
    test_every_semantic_field_is_bound();
    test_invalid_persisted_shape_fails_closed();

    std::cout << "Relation corpus SHA-256 checks: " << checks_passed << " passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
