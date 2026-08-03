// test_siqs_2lp_adapter.cpp - SIQS raw-relation to 2LP graph adapter contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/two_large_prime_adapter.hpp>
#include <gnfs/siqs/two_large_prime_graph.hpp>
#include <gnfs/siqs/two_large_prime_materializer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using std::int64_t;
using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

using gnfs::core::Integer;
using gnfs::siqs::build_two_large_prime_cycle_basis;
using gnfs::siqs::materialize_two_large_prime_cycle;
using gnfs::siqs::MaterializedTwoLargePrimeCycle;
using gnfs::siqs::prepare_two_large_prime_corpus;
using gnfs::siqs::PreparedTwoLargePrimeCorpus;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::TwoLargePrimeAdapterStats;
using gnfs::siqs::TwoLargePrimeCycleBasis;
using gnfs::siqs::TwoLargePrimeCycleSource;
using gnfs::siqs::TwoLargePrimeEdge;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

[[nodiscard]] SIQSRelation make_relation(int64_t value, std::vector<uint8_t> exponents,
                                         uint64_t large_prime, uint64_t large_prime2,
                                         bool negative = false) {
    SIQSRelation relation;
    relation.value = Integer(value);
    relation.exponents = std::move(exponents);
    for (size_t i = 0; i < relation.exponents.size(); ++i) {
        if (relation.exponents[i] != 0) {
            relation.fb_indices.push_back(static_cast<uint32_t>(i));
        }
    }
    relation.large_prime = large_prime;
    relation.large_prime2 = large_prime2;
    relation.negative = negative;
    return relation;
}

template <class Splitter>
[[nodiscard]] std::optional<PreparedTwoLargePrimeCorpus>
prepare(const std::vector<SIQSRelation>& relations, size_t factor_base_size,
        uint64_t large_prime_bound, Splitter&& splitter) {
    return prepare_two_large_prime_corpus(
        std::span<const SIQSRelation>(relations.data(), relations.size()), factor_base_size,
        large_prime_bound, std::forward<Splitter>(splitter));
}

void check_stats(const TwoLargePrimeAdapterStats& stats, size_t input_relations,
                 size_t full_relations, size_t accepted_one_lp, size_t accepted_two_lp,
                 size_t rejected_relations) {
    CHECK(stats.input_relations == input_relations);
    CHECK(stats.full_relations == full_relations);
    CHECK(stats.accepted_one_lp == accepted_one_lp);
    CHECK(stats.accepted_two_lp == accepted_two_lp);
    CHECK(stats.rejected_relations == rejected_relations);
    CHECK(stats.input_relations == stats.full_relations + stats.accepted_one_lp +
                                       stats.accepted_two_lp + stats.rejected_relations);
    CHECK(stats.typed_rejections() == stats.rejected_relations);
}

void check_rejection_stats(const TwoLargePrimeAdapterStats& stats, size_t malformed_source_shape,
                           size_t unsupported_encoding, size_t invalid_one_large_prime,
                           size_t invalid_two_large_prime_split, size_t exact_duplicate) {
    CHECK(stats.malformed_source_shape == malformed_source_shape);
    CHECK(stats.unsupported_encoding == unsupported_encoding);
    CHECK(stats.invalid_one_large_prime == invalid_one_large_prime);
    CHECK(stats.invalid_two_large_prime_split == invalid_two_large_prime_split);
    CHECK(stats.exact_duplicate == exact_duplicate);
    CHECK(stats.typed_rejections() == malformed_source_shape + unsupported_encoding +
                                          invalid_one_large_prime + invalid_two_large_prime_split +
                                          exact_duplicate);
}

[[nodiscard]] bool same_source(const TwoLargePrimeCycleSource& lhs,
                               const TwoLargePrimeCycleSource& rhs) {
    return lhs.relation_index == rhs.relation_index && lhs.value == rhs.value &&
           lhs.negative == rhs.negative && lhs.factor_base_exponents == rhs.factor_base_exponents &&
           lhs.p == rhs.p && lhs.q == rhs.q;
}

[[nodiscard]] bool same_corpus(const PreparedTwoLargePrimeCorpus& lhs,
                               const PreparedTwoLargePrimeCorpus& rhs) {
    if (lhs.stats != rhs.stats || lhs.edges != rhs.edges ||
        lhs.sources.size() != rhs.sources.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.sources.size(); ++i) {
        if (!same_source(lhs.sources[i], rhs.sources[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_basis(const TwoLargePrimeCycleBasis& lhs,
                              const TwoLargePrimeCycleBasis& rhs) {
    return lhs.vertex_count == rhs.vertex_count && lhs.edge_count == rhs.edge_count &&
           lhs.component_count == rhs.component_count && lhs.cycles == rhs.cycles;
}

[[nodiscard]] bool same_materialized(const MaterializedTwoLargePrimeCycle& lhs,
                                     const MaterializedTwoLargePrimeCycle& rhs) {
    return lhs.value_modulus == rhs.value_modulus && lhs.negative == rhs.negative &&
           lhs.factor_base_exponents == rhs.factor_base_exponents &&
           lhs.large_prime_square_roots == rhs.large_prime_square_roots &&
           lhs.relation_indices == rhs.relation_indices;
}

[[nodiscard]] size_t support_rank(const std::vector<std::vector<size_t>>& supports,
                                  size_t column_count) {
    const size_t word_count = (column_count + 63) / 64;
    std::vector<std::vector<uint64_t>> rows(supports.size(), std::vector<uint64_t>(word_count, 0));
    for (size_t row = 0; row < supports.size(); ++row) {
        for (const size_t column : supports[row]) {
            if (column >= column_count) {
                return 0;
            }
            rows[row][column / 64] ^= uint64_t{1} << (column % 64);
        }
    }

    size_t rank = 0;
    for (size_t column = 0; column < column_count && rank < rows.size(); ++column) {
        size_t pivot = rank;
        while (pivot < rows.size() &&
               (rows[pivot][column / 64] & (uint64_t{1} << (column % 64))) == 0) {
            ++pivot;
        }
        if (pivot == rows.size()) {
            continue;
        }
        std::swap(rows[rank], rows[pivot]);
        for (size_t row = rank + 1; row < rows.size(); ++row) {
            if ((rows[row][column / 64] & (uint64_t{1} << (column % 64))) == 0) {
                continue;
            }
            for (size_t word = 0; word < word_count; ++word) {
                rows[row][word] ^= rows[rank][word];
            }
        }
        ++rank;
    }
    return rank;
}

[[nodiscard]] std::pair<uint64_t, uint64_t> split_shadow_cofactor(uint64_t cofactor) {
    if (cofactor == uint64_t{101} * 103) {
        return {103, 101};
    }
    if (cofactor == uint64_t{101} * 107) {
        return {107, 101};
    }
    if (cofactor == uint64_t{103} * 107) {
        return {107, 103};
    }
    if (cofactor == uint64_t{101} * 101) {
        return {101, 101};
    }
    return {0, 0};
}

struct RecordingShadowSplitter {
    size_t* calls;
    std::vector<uint64_t>* inputs;

    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t cofactor) const {
        ++*calls;
        inputs->push_back(cofactor);
        return split_shadow_cofactor(cofactor);
    }
};

void test_invalid_config_and_full_relation_count() {
    const std::vector<SIQSRelation> relations{
        make_relation(11, {0, 2, 0}, 0, 0),
        make_relation(13, {0, 0, 3}, 0, 0, true),
    };
    size_t splitter_calls = 0;
    const auto splitter = [&splitter_calls](uint64_t) {
        ++splitter_calls;
        return std::pair<uint64_t, uint64_t>{0, 0};
    };

    CHECK(!prepare(relations, 0, 200, splitter).has_value());
    CHECK(!prepare(relations, 3, 1, splitter).has_value());
    CHECK(splitter_calls == 0);

    const auto corpus = prepare(relations, 3, 200, splitter);
    CHECK(corpus.has_value());
    if (corpus) {
        check_stats(corpus->stats, 2, 2, 0, 0, 0);
        check_rejection_stats(corpus->stats, 0, 0, 0, 0, 0);
        CHECK(corpus->edges.empty());
        CHECK(corpus->sources.empty());
    }
    CHECK(splitter_calls == 0);
}

void test_raw_encoding_admission_and_splitter_dispatch() {
    const uint64_t valid_cofactor = uint64_t{101} * 103;
    const std::vector<SIQSRelation> relations{
        make_relation(2, {0, 1, 0}, 0, 0),
        make_relation(3, {0, 0, 1}, 101, 0),
        make_relation(5, {0, 1, 1}, 15, 0),
        make_relation(7, {0, 2, 0}, 211, 0),
        make_relation(11, {0, 0, 2}, valid_cofactor, 1),
        make_relation(13, {0, 3, 0}, 101, 103),
    };

    size_t splitter_calls = 0;
    std::vector<uint64_t> splitter_inputs;
    const auto splitter = [&](uint64_t cofactor) {
        ++splitter_calls;
        splitter_inputs.push_back(cofactor);
        return std::pair<uint64_t, uint64_t>{103, 101};
    };

    const auto corpus = prepare(relations, 3, 200, splitter);
    CHECK(corpus.has_value());
    if (corpus) {
        check_stats(corpus->stats, 6, 1, 1, 1, 3);
        check_rejection_stats(corpus->stats, 0, 1, 2, 0, 0);
        const std::vector<TwoLargePrimeEdge> expected_edges{
            {0, 101, 0},
            {101, 103, 1},
        };
        CHECK(corpus->edges == expected_edges);
        CHECK(corpus->sources.size() == 2);
    }
    CHECK(splitter_calls == 1);
    CHECK(splitter_inputs == std::vector<uint64_t>{valid_cofactor});
}

void test_split_normalization_failures_and_prime_square() {
    const uint64_t composite_factor = uint64_t{15} * 17;
    const uint64_t factor_over_bound = uint64_t{101} * 211;
    const uint64_t wrong_product = uint64_t{107} * 109;
    const uint64_t prime_square = uint64_t{101} * 101;
    const std::vector<SIQSRelation> relations{
        make_relation(2, {0, 1, 0}, composite_factor, 1),
        make_relation(3, {0, 0, 1}, factor_over_bound, 1),
        make_relation(5, {0, 2, 0}, wrong_product, 1),
        make_relation(7, {0, 0, 2}, prime_square, 1),
    };

    size_t splitter_calls = 0;
    const auto splitter = [&](uint64_t cofactor) {
        ++splitter_calls;
        if (cofactor == composite_factor) {
            return std::pair<uint64_t, uint64_t>{15, 17};
        }
        if (cofactor == factor_over_bound) {
            return std::pair<uint64_t, uint64_t>{101, 211};
        }
        if (cofactor == wrong_product) {
            return std::pair<uint64_t, uint64_t>{107, 113};
        }
        return std::pair<uint64_t, uint64_t>{101, 101};
    };

    const auto corpus = prepare(relations, 3, 200, splitter);
    CHECK(corpus.has_value());
    if (corpus) {
        check_stats(corpus->stats, 4, 0, 0, 1, 3);
        check_rejection_stats(corpus->stats, 0, 0, 0, 3, 0);
        const std::vector<TwoLargePrimeEdge> expected_edges{{101, 101, 0}};
        CHECK(corpus->edges == expected_edges);
        CHECK(corpus->sources.size() == 1);
        if (!corpus->sources.empty()) {
            CHECK(corpus->sources[0].p == 101);
            CHECK(corpus->sources[0].q == 101);
        }
    }
    CHECK(splitter_calls == 4);
}

void test_structural_rejections_precede_splitter() {
    constexpr size_t factor_base_size = 4;
    const uint64_t sentinel_cofactor = uint64_t{101} * 103;
    std::vector<SIQSRelation> relations;

    auto with_merge_history = make_relation(2, {0, 1, 0, 0}, sentinel_cofactor, 1);
    with_merge_history.merge_lps.push_back(101);
    relations.push_back(std::move(with_merge_history));

    relations.push_back(make_relation(3, {0, 1, 0}, sentinel_cofactor, 1));

    auto duplicate_index = make_relation(5, {0, 1, 0, 0}, sentinel_cofactor, 1);
    duplicate_index.fb_indices = {1, 1};
    relations.push_back(std::move(duplicate_index));

    auto out_of_range_index = make_relation(7, {0, 1, 0, 0}, sentinel_cofactor, 1);
    out_of_range_index.fb_indices = {1, 4};
    relations.push_back(std::move(out_of_range_index));

    auto missing_index = make_relation(11, {0, 1, 2, 0}, sentinel_cofactor, 1);
    missing_index.fb_indices = {1};
    relations.push_back(std::move(missing_index));

    auto spurious_index = make_relation(13, {0, 1, 0, 0}, sentinel_cofactor, 1);
    spurious_index.fb_indices = {1, 2};
    relations.push_back(std::move(spurious_index));

    // Factor-base slot zero is the sign sentinel, not an ordinary exponent.
    relations.push_back(make_relation(17, {1, 1, 0, 0}, sentinel_cofactor, 1));

    auto explicit_sign_index = make_relation(19, {0, 1, 0, 0}, sentinel_cofactor, 1);
    explicit_sign_index.fb_indices = {0, 1};
    relations.push_back(std::move(explicit_sign_index));

    size_t splitter_calls = 0;
    const auto splitter = [&splitter_calls](uint64_t) {
        ++splitter_calls;
        return std::pair<uint64_t, uint64_t>{101, 103};
    };

    const auto corpus = prepare(relations, factor_base_size, 200, splitter);
    CHECK(corpus.has_value());
    if (corpus) {
        check_stats(corpus->stats, relations.size(), 0, 0, 0, relations.size());
        check_rejection_stats(corpus->stats, relations.size(), 0, 0, 0, 0);
        CHECK(corpus->edges.empty());
        CHECK(corpus->sources.empty());
    }
    CHECK(splitter_calls == 0);
}

void test_uint8_exponents_are_widened_exactly() {
    auto relation = make_relation(23, {0, 255, 128, 1, 0}, 101, 0, true);
    relation.fb_indices = {3, 1, 2};
    const std::vector<SIQSRelation> relations{relation};

    size_t splitter_calls = 0;
    const auto corpus = prepare(relations, 5, 200, [&splitter_calls](uint64_t) {
        ++splitter_calls;
        return std::pair<uint64_t, uint64_t>{0, 0};
    });
    CHECK(corpus.has_value());
    if (corpus) {
        check_stats(corpus->stats, 1, 0, 1, 0, 0);
        check_rejection_stats(corpus->stats, 0, 0, 0, 0, 0);
        CHECK(corpus->sources.size() == 1);
        if (!corpus->sources.empty()) {
            const std::vector<uint32_t> expected{0, 255, 128, 1, 0};
            CHECK(corpus->sources[0].factor_base_exponents == expected);
            CHECK(corpus->sources[0].relation_index == 0);
            CHECK(corpus->sources[0].value == Integer(23));
            CHECK(corpus->sources[0].negative);
            CHECK(corpus->sources[0].p == 0);
            CHECK(corpus->sources[0].q == 101);
        }
    }
    CHECK(splitter_calls == 0);
}

void test_exact_duplicates_are_deduplicated_but_parallel_payloads_survive() {
    const SIQSRelation original = make_relation(17, {0, 1, 2}, 109, 0, true);
    SIQSRelation sparse_order_duplicate = original;
    sparse_order_duplicate.fb_indices = {2, 1};
    const std::vector<SIQSRelation> exact_duplicates{
        original,
        sparse_order_duplicate,
    };
    const auto no_split = [](uint64_t) { return std::pair<uint64_t, uint64_t>{0, 0}; };

    const auto deduplicated = prepare(exact_duplicates, 3, 200, no_split);
    CHECK(deduplicated.has_value());
    if (deduplicated) {
        check_stats(deduplicated->stats, 2, 0, 1, 0, 1);
        check_rejection_stats(deduplicated->stats, 0, 0, 0, 0, 1);
        CHECK(deduplicated->edges.size() == 1);
        CHECK(deduplicated->sources.size() == 1);
        if (!deduplicated->edges.empty()) {
            CHECK(deduplicated->edges[0] == (TwoLargePrimeEdge{0, 109, 0}));
        }
        if (!deduplicated->sources.empty()) {
            CHECK(deduplicated->sources[0].relation_index == 0);
        }
    }

    SIQSRelation different_payload = original;
    different_payload.value = Integer(19);
    const std::vector<SIQSRelation> parallel_relations{
        different_payload,
        original,
    };
    const auto parallel = prepare(parallel_relations, 3, 200, no_split);
    CHECK(parallel.has_value());
    if (parallel) {
        check_stats(parallel->stats, 2, 0, 2, 0, 0);
        check_rejection_stats(parallel->stats, 0, 0, 0, 0, 0);
        const std::vector<TwoLargePrimeEdge> expected_edges{
            {0, 109, 0},
            {0, 109, 1},
        };
        CHECK(parallel->edges == expected_edges);
        CHECK(parallel->sources.size() == 2);
        if (parallel->sources.size() == 2) {
            CHECK(parallel->sources[0].relation_index == 0);
            CHECK(parallel->sources[1].relation_index == 1);
            CHECK(parallel->sources[0].value == Integer(17));
            CHECK(parallel->sources[1].value == Integer(19));
        }

        const auto basis = build_two_large_prime_cycle_basis(
            std::span<const TwoLargePrimeEdge>(parallel->edges.data(), parallel->edges.size()));
        CHECK(basis.has_value());
        if (basis) {
            const std::vector<std::vector<size_t>> expected_cycles{{0, 1}};
            CHECK(basis->cycles == expected_cycles);
        }
    }

    SIQSRelation different_sign = original;
    different_sign.negative = false;
    const std::vector<SIQSRelation> sign_distinct_relations{
        original,
        different_sign,
    };
    const auto sign_distinct = prepare(sign_distinct_relations, 3, 200, no_split);
    CHECK(sign_distinct.has_value());
    if (sign_distinct) {
        check_stats(sign_distinct->stats, 2, 0, 2, 0, 0);
        check_rejection_stats(sign_distinct->stats, 0, 0, 0, 0, 0);
        CHECK(sign_distinct->edges.size() == 2);
        CHECK(sign_distinct->sources.size() == 2);
        if (sign_distinct->sources.size() == 2) {
            CHECK(!sign_distinct->sources[0].negative);
            CHECK(sign_distinct->sources[1].negative);
        }
    }

    SIQSRelation different_exponents = original;
    different_exponents.exponents = {0, 2, 1};
    different_exponents.fb_indices = {1, 2};
    const std::vector<SIQSRelation> exponent_distinct_relations{
        different_exponents,
        original,
    };
    const auto exponent_distinct = prepare(exponent_distinct_relations, 3, 200, no_split);
    CHECK(exponent_distinct.has_value());
    if (exponent_distinct) {
        check_stats(exponent_distinct->stats, 2, 0, 2, 0, 0);
        check_rejection_stats(exponent_distinct->stats, 0, 0, 0, 0, 0);
        CHECK(exponent_distinct->edges.size() == 2);
        CHECK(exponent_distinct->sources.size() == 2);
        if (exponent_distinct->sources.size() == 2) {
            const std::vector<uint32_t> expected_first{0, 1, 2};
            const std::vector<uint32_t> expected_second{0, 2, 1};
            CHECK(exponent_distinct->sources[0].factor_base_exponents == expected_first);
            CHECK(exponent_distinct->sources[1].factor_base_exponents == expected_second);
        }
    }
}

[[nodiscard]] std::vector<SIQSRelation> make_shadow_relations() {
    const uint64_t p101_p103 = uint64_t{101} * 103;
    const uint64_t p101_p107 = uint64_t{101} * 107;
    const uint64_t p103_p107 = uint64_t{103} * 107;

    std::vector<SIQSRelation> relations;
    relations.push_back(make_relation(53, {0, 1, 0, 2}, p103_p107, 1, true));
    relations.push_back(make_relation(29, {0, 100, 3, 0}, 109, 0));
    relations.push_back(make_relation(7, {0, 2, 0, 0}, 0, 0));
    relations.push_back(make_relation(31, {0, 1, 1, 0}, p101_p103, 1));
    relations.push_back(make_relation(23, {0, 200, 0, 1}, 109, 0, true));
    relations.push_back(make_relation(37, {0, 0, 1, 2}, p101_p107, 1));
    // The exact duplicate is rejected canonically. It must not create a third
    // parallel edge or perturb IDs when the raw corpus is reversed.
    const SIQSRelation exact_duplicate = relations[4];
    relations.push_back(exact_duplicate);
    return relations;
}

void test_graph_and_materializer_shadow_oracle() {
    const auto relations = make_shadow_relations();
    size_t forward_calls = 0;
    std::vector<uint64_t> forward_inputs;
    const auto forward =
        prepare(relations, 4, 200, RecordingShadowSplitter{&forward_calls, &forward_inputs});
    CHECK(forward.has_value());
    if (!forward) {
        return;
    }

    check_stats(forward->stats, 7, 1, 2, 3, 1);
    check_rejection_stats(forward->stats, 0, 0, 0, 0, 1);
    CHECK(forward_calls == 3);
    const std::vector<TwoLargePrimeEdge> expected_edges{
        {0, 109, 0}, {0, 109, 1}, {101, 103, 2}, {101, 107, 3}, {103, 107, 4},
    };
    CHECK(forward->edges == expected_edges);
    CHECK(forward->sources.size() == expected_edges.size());
    if (forward->edges.size() == forward->sources.size()) {
        for (size_t i = 0; i < forward->edges.size(); ++i) {
            CHECK(forward->edges[i].relation_index == forward->sources[i].relation_index);
            CHECK(forward->edges[i].p == forward->sources[i].p);
            CHECK(forward->edges[i].q == forward->sources[i].q);
        }
    }

    auto reversed_relations = relations;
    std::reverse(reversed_relations.begin(), reversed_relations.end());
    size_t reversed_calls = 0;
    std::vector<uint64_t> reversed_inputs;
    const auto reversed = prepare(reversed_relations, 4, 200,
                                  RecordingShadowSplitter{&reversed_calls, &reversed_inputs});
    CHECK(reversed.has_value());
    if (!reversed) {
        return;
    }

    check_stats(reversed->stats, 7, 1, 2, 3, 1);
    check_rejection_stats(reversed->stats, 0, 0, 0, 0, 1);
    CHECK(reversed_calls == 3);
    std::sort(forward_inputs.begin(), forward_inputs.end());
    std::sort(reversed_inputs.begin(), reversed_inputs.end());
    CHECK(forward_inputs == reversed_inputs);
    CHECK(same_corpus(*forward, *reversed));

    const auto forward_basis = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(forward->edges.data(), forward->edges.size()));
    const auto reversed_basis = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(reversed->edges.data(), reversed->edges.size()));
    CHECK(forward_basis.has_value());
    CHECK(reversed_basis.has_value());
    if (!forward_basis || !reversed_basis) {
        return;
    }

    CHECK(same_basis(*forward_basis, *reversed_basis));
    CHECK(forward_basis->vertex_count == 5);
    CHECK(forward_basis->edge_count == 5);
    CHECK(forward_basis->component_count == 2);
    CHECK(forward_basis->cycles.size() == 2);
    const std::vector<std::vector<size_t>> expected_cycles{
        {0, 1},
        {2, 3, 4},
    };
    CHECK(forward_basis->cycles == expected_cycles);
    CHECK(forward_basis->cycles.size() ==
          forward_basis->edge_count - forward_basis->vertex_count + forward_basis->component_count);
    CHECK(support_rank(forward_basis->cycles, forward_basis->edge_count) == 2);

    const Integer modulus(1'000'003);
    for (size_t i = 0; i < forward_basis->cycles.size(); ++i) {
        const auto& forward_cycle = forward_basis->cycles[i];
        const auto& reversed_cycle = reversed_basis->cycles[i];
        const auto forward_materialized = materialize_two_large_prime_cycle(
            std::span<const TwoLargePrimeCycleSource>(forward->sources.data(),
                                                      forward->sources.size()),
            std::span<const size_t>(forward_cycle.data(), forward_cycle.size()), modulus);
        const auto reversed_materialized = materialize_two_large_prime_cycle(
            std::span<const TwoLargePrimeCycleSource>(reversed->sources.data(),
                                                      reversed->sources.size()),
            std::span<const size_t>(reversed_cycle.data(), reversed_cycle.size()), modulus);
        CHECK(forward_materialized.has_value());
        CHECK(reversed_materialized.has_value());
        if (forward_materialized && reversed_materialized) {
            CHECK(same_materialized(*forward_materialized, *reversed_materialized));
            if (i == 0) {
                const std::vector<uint32_t> expected_wide_sum{0, 300, 3, 1};
                CHECK(forward_materialized->factor_base_exponents == expected_wide_sum);
            }
        }
    }
}

} // namespace

int main() {
    test_invalid_config_and_full_relation_count();
    test_raw_encoding_admission_and_splitter_dispatch();
    test_split_normalization_failures_and_prime_square();
    test_structural_rejections_precede_splitter();
    test_uint8_exponents_are_widened_exactly();
    test_exact_duplicates_are_deduplicated_but_parallel_payloads_survive();
    test_graph_and_materializer_shadow_oracle();

    std::cout << checks_passed << " checks passed, " << checks_failed << " checks failed\n";
    return checks_failed == 0 ? 0 : 1;
}
