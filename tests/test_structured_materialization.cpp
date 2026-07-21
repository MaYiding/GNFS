#include "gnfs/relation/structured_reduction.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using gnfs::core::AlgebraicPrime;
using gnfs::core::PrimePower;
using gnfs::core::Relation;
using gnfs::relation::SourceCombination;
using gnfs::relation::SourceCorpus;
using gnfs::relation::SourceId;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;

namespace {

size_t g_checks = 0;

[[noreturn]] void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed at line ") + std::to_string(line) + ": " +
                             expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            check_failed(#condition, __LINE__);                                                    \
        }                                                                                          \
    } while (false)

template <typename Action>
void expect_structured_error(StructuredReductionErrorCode expected, Action&& action) {
    bool rejected = false;
    try {
        std::forward<Action>(action)();
    } catch (const StructuredReductionError& error) {
        rejected = true;
        CHECK(error.code() == expected);
    }
    CHECK(rejected);
}

Relation make_relation(int64_t a, uint64_t b) {
    return Relation(a, b);
}

bool same_relation(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

void check_ordinals(const SourceCombination& combination,
                    std::initializer_list<uint64_t> expected) {
    const auto sources = combination.sources();
    CHECK(sources.size() == expected.size());
    size_t i = 0;
    for (const uint64_t ordinal : expected) {
        CHECK(sources[i].generation == combination.generation());
        CHECK(sources[i].ordinal == ordinal);
        ++i;
    }
}

void test_source_combination_canonical_and_xor() {
    constexpr uint64_t generation = 17;
    const SourceCombination canonical = SourceCombination::canonical(
        generation, {SourceId{generation, 5}, SourceId{generation, 1}, SourceId{generation, 3}});
    CHECK(canonical.generation() == generation);
    CHECK(!canonical.empty());
    CHECK(canonical.size() == 3);
    check_ordinals(canonical, {1, 3, 5});

    expect_structured_error(StructuredReductionErrorCode::InvalidGeneration,
                            [] { (void)SourceCombination::canonical(0, {SourceId{0, 0}}); });
    expect_structured_error(StructuredReductionErrorCode::InvalidGeneration, [=] {
        (void)SourceCombination::canonical(generation,
                                           {SourceId{generation, 1}, SourceId{generation + 1, 2}});
    });
    expect_structured_error(StructuredReductionErrorCode::InvalidSourceCombination, [=] {
        (void)SourceCombination::canonical(generation,
                                           {SourceId{generation, 2}, SourceId{generation, 2}});
    });

    const SourceCombination lhs = SourceCombination::canonical(
        generation, {SourceId{generation, 4}, SourceId{generation, 0}, SourceId{generation, 2}});
    const SourceCombination rhs = SourceCombination::canonical(
        generation, {SourceId{generation, 4}, SourceId{generation, 3}, SourceId{generation, 2}});
    const SourceCombination overlap = SourceCombination::symmetric_difference(lhs, rhs);
    check_ordinals(overlap, {0, 3});

    const SourceCombination zero = SourceCombination::symmetric_difference(lhs, lhs);
    CHECK(zero.generation() == generation);
    CHECK(zero.empty());
    CHECK(zero.sources().empty());

    const SourceCombination other_generation =
        SourceCombination::singleton(SourceId{generation + 1, 0});
    expect_structured_error(StructuredReductionErrorCode::InvalidGeneration, [&] {
        (void)SourceCombination::symmetric_difference(lhs, other_generation);
    });
}

void test_source_corpus_rejects_invalid_input() {
    {
        SourceCorpus corpus(7, {make_relation(1, 1), make_relation(2, 1)});
        const SourceId source = corpus.source_id(1);
        const Relation* borrowed = corpus.try_borrow(source);
        CHECK(borrowed != nullptr);
        CHECK(borrowed->a == 2);
        CHECK(corpus.at(source).a == 2);
        expect_structured_error(StructuredReductionErrorCode::InvalidSourceCombination,
                                [&] { (void)corpus.try_borrow(SourceId{8, 1}); });
        expect_structured_error(StructuredReductionErrorCode::InvalidSourceCombination,
                                [&] { (void)corpus.try_borrow(SourceId{7, 2}); });
    }

    expect_structured_error(StructuredReductionErrorCode::InvalidGeneration, [] {
        SourceCorpus corpus(0, {make_relation(1, 1)});
        (void)corpus;
    });

    expect_structured_error(StructuredReductionErrorCode::InvalidInput, [] {
        SourceCorpus corpus(1, {make_relation(1, 0)});
        (void)corpus;
    });

    expect_structured_error(StructuredReductionErrorCode::InvalidInput, [] {
        Relation invalid = make_relation(1, 1);
        invalid.extra_ab_pairs.emplace_back(2, 0);
        SourceCorpus corpus(1, {std::move(invalid)});
        (void)corpus;
    });

    expect_structured_error(StructuredReductionErrorCode::InvalidInput, [] {
        Relation invalid = make_relation(1, 1);
        invalid.rational_large_prime.emplace_back(1, 0, 1);
        SourceCorpus corpus(1, {std::move(invalid)});
        (void)corpus;
    });

    expect_structured_error(StructuredReductionErrorCode::InvalidInput, [] {
        Relation invalid = make_relation(1, 1);
        invalid.algebraic_large_prime.emplace_back(101, 7, 0);
        SourceCorpus corpus(1, {std::move(invalid)});
        (void)corpus;
    });

    expect_structured_error(StructuredReductionErrorCode::InvalidInput, [] {
        Relation invalid = make_relation(1, 1);
        invalid.algebraic_large_prime.emplace_back(101, 102, 1);
        SourceCorpus corpus(1, {std::move(invalid)});
        (void)corpus;
    });
}

void test_nested_flatten_and_factor_order() {
    constexpr uint64_t generation = 23;
    Relation first = make_relation(11, 2);
    first.extra_ab_pairs = {{41, 5}, {13, 3}};
    first.rational_factors = {9, 2, 9};
    first.algebraic_factors = {8, 1};

    Relation second = make_relation(41, 5);
    second.extra_ab_pairs = {{-7, 11}, {41, 5}};
    second.rational_factors = {4, 2};
    second.algebraic_factors = {7, 8, 6};

    SourceCorpus corpus(generation, {std::move(first), std::move(second)});
    const SourceCombination combination =
        SourceCombination::canonical(generation, {corpus.source_id(1), corpus.source_id(0)});
    const Relation materialized = corpus.materialize(combination);

    CHECK(materialized.a == 11);
    CHECK(materialized.b == 2);
    const std::vector<std::pair<int64_t, uint64_t>> expected_pairs{
        {41, 5}, {13, 3}, {41, 5}, {-7, 11}, {41, 5}};
    CHECK(materialized.extra_ab_pairs == expected_pairs);
    CHECK(materialized.rational_factors == std::vector<uint32_t>({9, 2, 9, 4, 2}));
    CHECK(materialized.algebraic_factors == std::vector<uint32_t>({8, 1, 7, 8, 6}));

    const SourceCombination zero =
        SourceCombination::symmetric_difference(combination, combination);
    expect_structured_error(StructuredReductionErrorCode::InvalidSourceCombination,
                            [&] { (void)corpus.materialize(zero); });
}

void test_full_width_large_prime_aggregation() {
    constexpr uint64_t generation = 29;
    constexpr uint64_t rational_prime = (UINT64_C(1) << 48U) + 59;
    constexpr uint64_t algebraic_prime = (UINT64_C(1) << 52U) + 111;
    constexpr uint64_t algebraic_root = (UINT64_C(1) << 40U) + 17;
    constexpr uint64_t projective_root = static_cast<uint64_t>(AlgebraicPrime::PROJECTIVE_ROOT);

    Relation first = make_relation(17, 3);
    first.rational_large_prime = {
        PrimePower{rational_prime, std::numeric_limits<uint64_t>::max(), 200},
        PrimePower{1009, 91, 1},
    };
    first.algebraic_large_prime = {
        PrimePower{algebraic_prime, algebraic_root + 1, 3},
        PrimePower{101, projective_root, 100},
        PrimePower{algebraic_prime, algebraic_root, 200},
    };

    Relation second = make_relation(19, 5);
    second.rational_large_prime = {PrimePower{rational_prime, 12345, 54}};
    second.algebraic_large_prime = {
        PrimePower{algebraic_prime, algebraic_root, 55},
        PrimePower{101, projective_root, 155},
        PrimePower{algebraic_prime, algebraic_root + 1, 4},
    };

    SourceCorpus corpus(generation, {std::move(first), std::move(second)});
    const Relation materialized = corpus.materialize(
        SourceCombination::canonical(generation, {corpus.source_id(0), corpus.source_id(1)}));

    const std::vector<PrimePower> expected_rational{
        PrimePower{1009, 0, 1},
        PrimePower{rational_prime, 0, 254},
    };
    const std::vector<PrimePower> expected_algebraic{
        PrimePower{101, projective_root, 255},
        PrimePower{algebraic_prime, algebraic_root, 255},
        PrimePower{algebraic_prime, algebraic_root + 1, 7},
    };
    CHECK(materialized.rational_large_prime == expected_rational);
    CHECK(materialized.algebraic_large_prime == expected_algebraic);
}

void test_exponent_chunk_boundaries() {
    constexpr uint64_t generation = 31;
    Relation first = make_relation(1, 1);
    first.rational_large_prime = {
        PrimePower{101, 0, 254}, PrimePower{103, 0, 255}, PrimePower{107, 0, 255},
        PrimePower{109, 0, 255}, PrimePower{113, 0, 255},
    };

    Relation second = make_relation(2, 1);
    second.rational_large_prime = {
        PrimePower{107, 0, 1},
        PrimePower{109, 0, 255},
        PrimePower{113, 0, 255},
    };

    Relation third = make_relation(3, 1);
    third.rational_large_prime = {PrimePower{113, 0, 1}};

    SourceCorpus corpus(generation, {std::move(first), std::move(second), std::move(third)});
    const Relation materialized = corpus.materialize(SourceCombination::canonical(
        generation, {corpus.source_id(2), corpus.source_id(0), corpus.source_id(1)}));

    const std::vector<PrimePower> expected{
        PrimePower{101, 0, 254}, PrimePower{103, 0, 255}, PrimePower{107, 0, 255},
        PrimePower{107, 0, 1},   PrimePower{109, 0, 255}, PrimePower{109, 0, 255},
        PrimePower{113, 0, 255}, PrimePower{113, 0, 255}, PrimePower{113, 0, 1},
    };
    CHECK(materialized.rational_large_prime == expected);
}

Relation make_unique_lp_relation(int64_t a, uint64_t first_prime, size_t count) {
    Relation relation(a, 1);
    relation.rational_large_prime.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        relation.rational_large_prime.emplace_back(first_prime + i, 0, 1);
    }
    return relation;
}

void test_persistence_limit_rejected_before_publish() {
    constexpr uint64_t generation = 37;
    SourceCorpus corpus(generation,
                        {make_unique_lp_relation(1, 1000, 8), make_unique_lp_relation(2, 2000, 8),
                         make_unique_lp_relation(3, 3000, 1)});

    const SourceCombination exactly_sixteen =
        SourceCombination::canonical(generation, {corpus.source_id(0), corpus.source_id(1)});
    const Relation before = corpus.materialize(exactly_sixteen);
    CHECK(before.rational_large_prime.size() == Relation::MAX_SERIALIZED_LARGE_PRIMES);

    const SourceCombination seventeen = SourceCombination::canonical(
        generation, {corpus.source_id(0), corpus.source_id(1), corpus.source_id(2)});
    expect_structured_error(StructuredReductionErrorCode::PersistenceLimit,
                            [&] { (void)corpus.materialize(seventeen); });

    const Relation after = corpus.materialize(exactly_sixteen);
    CHECK(same_relation(before, after));
}

void test_materialized_stream_roundtrip() {
    constexpr uint64_t generation = 41;
    Relation first = make_relation(-19, 7);
    first.rational_factors = {3, 1};
    first.rational_large_prime = {PrimePower{1009, 0, 255}};

    Relation second = make_relation(23, 11);
    second.extra_ab_pairs = {{29, 13}};
    second.algebraic_factors = {8, 5};
    second.rational_large_prime = {PrimePower{1009, 0, 1}};

    SourceCorpus corpus(generation, {std::move(first), std::move(second)});
    const Relation materialized = corpus.materialize(
        SourceCombination::canonical(generation, {corpus.source_id(0), corpus.source_id(1)}));

    std::ostringstream output(std::ios::binary);
    materialized.serialize(output);
    CHECK(!output.str().empty());
    std::istringstream input(output.str(), std::ios::binary);
    const Relation roundtrip = Relation::deserialize(input);
    CHECK(same_relation(materialized, roundtrip));
}

} // namespace

int main() {
    try {
        test_source_combination_canonical_and_xor();
        test_source_corpus_rejects_invalid_input();
        test_nested_flatten_and_factor_order();
        test_full_width_large_prime_aggregation();
        test_exponent_chunk_boundaries();
        test_persistence_limit_rejected_before_publish();
        test_materialized_stream_roundtrip();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "All " << g_checks << " structured materialization checks passed\n";
    return 0;
}
