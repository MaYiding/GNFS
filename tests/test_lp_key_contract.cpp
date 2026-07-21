// Cross-module contract tests for canonical large-prime GF(2) incidence.

#include <gnfs/factor_base/factor_base.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/relation_source.hpp>
#include <gnfs/relation/clique_merger.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/relation/large_prime_key.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

using gnfs::core::PrimePower;
using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::core::Relation;
using gnfs::factor_base::FactorBase;
using gnfs::linalg::BitVector;
using gnfs::linalg::MatrixBuilder;
using gnfs::linalg::MatrixBuilderConfig;
using gnfs::linalg::OOCRelationSource;
using gnfs::linalg::PrimeIdealKey;
using gnfs::linalg::VectorRelationSource;
using gnfs::relation::CliqueRelationMerger;
using gnfs::relation::CliqueStats;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::LargePrimeKeyHash;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::PartialRelationMerger;
using gnfs::relation::RelationFilter;
using gnfs::relation::count_lp_key_weights;
using gnfs::relation::count_odd_large_prime_keys;
using gnfs::relation::count_unique_lp_keys;
using gnfs::relation::for_each_odd_large_prime_key;
using gnfs::relation::odd_large_prime_keys;
using gnfs::relation::odd_large_prime_keys_empty;
using gnfs::relation::separate_relations;
using gnfs::sqrt::verify_algebraic_ideal_powers;

namespace {

[[noreturn]] void fail_check(
        const char* expression,
        const char* file,
        int line,
        std::string_view message = {}) {
    std::cerr << "CHECK failed: " << expression << " at " << file << ':' << line;
    if (!message.empty()) std::cerr << " (" << message << ')';
    std::cerr << '\n';
    std::exit(EXIT_FAILURE);
}

#define CHECK(expr)                                                           \
    do {                                                                      \
        if (!(expr)) fail_check(#expr, __FILE__, __LINE__);                  \
    } while (false)

#define CHECK_MSG(expr, msg)                                                  \
    do {                                                                      \
        if (!(expr)) fail_check(#expr, __FILE__, __LINE__, (msg));           \
    } while (false)

Relation make_relation(int64_t a) {
    return Relation(a, 1);
}

Relation make_rational_lp_relation(int64_t a, uint64_t p, uint8_t e = 1) {
    Relation relation = make_relation(a);
    relation.rational_large_prime.push_back(PrimePower{p, 0, e});
    return relation;
}

Relation make_algebraic_lp_relation(
        int64_t a,
        uint64_t p,
        uint64_t root,
        uint8_t e = 1) {
    Relation relation = make_relation(a);
    relation.algebraic_large_prime.push_back(PrimePower{p, root, e});
    return relation;
}

struct TempOOCStore {
    std::string base = gnfs::util::temp_path(
        "gnfs_lp_key_contract_" +
        std::to_string(gnfs::util::process_id()));

    ~TempOOCStore() {
        std::remove((base + ".reldata").c_str());
        std::remove((base + ".relidx").c_str());
    }
};

void check_mapping_equal(
        const gnfs::linalg::ColumnMapping& lhs,
        const gnfs::linalg::ColumnMapping& rhs) {
    CHECK(lhs.num_rational_fb == rhs.num_rational_fb);
    CHECK(lhs.num_algebraic_fb == rhs.num_algebraic_fb);
    CHECK(lhs.num_large_primes_rat == rhs.num_large_primes_rat);
    CHECK(lhs.num_large_primes_alg == rhs.num_large_primes_alg);
    CHECK(lhs.num_qc_columns == rhs.num_qc_columns);
    CHECK(lhs.num_class_group_columns == rhs.num_class_group_columns);
    CHECK(lhs.num_schirokauer_columns == rhs.num_schirokauer_columns);
    CHECK(lhs.has_sign_column == rhs.has_sign_column);
    CHECK(lhs.rat_lp_to_col == rhs.rat_lp_to_col);
    CHECK(lhs.alg_lp_to_col == rhs.alg_lp_to_col);
}

void check_matrix_equal(
        const gnfs::linalg::SparseMatrix& lhs,
        const gnfs::linalg::SparseMatrix& rhs) {
    CHECK(lhs.num_rows() == rhs.num_rows());
    CHECK(lhs.num_cols() == rhs.num_cols());
    for (size_t i = 0; i < lhs.num_rows(); ++i) {
        CHECK(lhs.row(i).indices() == rhs.row(i).indices());
    }
}

void test_full_width_identity_and_side() {
    constexpr uint64_t high = uint64_t{1} << 32;
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();

    Relation relation = make_relation(1);
    relation.rational_large_prime.push_back({17, 0, 1});
    relation.rational_large_prime.push_back({max, 0, 1});
    relation.algebraic_large_prime.push_back({17, 0, 1});
    relation.algebraic_large_prime.push_back({17, 19, 1});
    relation.algebraic_large_prime.push_back({high + 17, 19, 1});
    relation.algebraic_large_prime.push_back({17, high + 19, 1});
    relation.algebraic_large_prime.push_back({max, max, 1});

    const auto keys = odd_large_prime_keys(relation);
    CHECK(keys.size() == 7);
    CHECK(std::is_sorted(keys.begin(), keys.end()));
    CHECK(count_odd_large_prime_keys(relation) == keys.size());

    std::vector<LargePrimeKey> visited;
    for_each_odd_large_prime_key(relation, [&](const LargePrimeKey& key) {
        visited.push_back(key);
    });
    CHECK(visited == keys);

    CHECK(std::find(keys.begin(), keys.end(), LargePrimeKey{17, 0, false}) !=
          keys.end());
    CHECK(std::find(keys.begin(), keys.end(), LargePrimeKey{17, 0, true}) !=
          keys.end());
    CHECK(std::find(keys.begin(), keys.end(), LargePrimeKey{17, 19, true}) !=
          keys.end());
    CHECK(std::find(keys.begin(), keys.end(),
                    LargePrimeKey{high + 17, 19, true}) != keys.end());
    CHECK(std::find(keys.begin(), keys.end(),
                    LargePrimeKey{17, high + 19, true}) != keys.end());
    CHECK(std::find(keys.begin(), keys.end(),
                    LargePrimeKey{max, 0, false}) != keys.end());
    CHECK(std::find(keys.begin(), keys.end(),
                    LargePrimeKey{max, max, true}) != keys.end());

    // A hash collision would be legal, but structural equality must never
    // collapse either high field or the rational/algebraic side.
    std::unordered_set<LargePrimeKey, LargePrimeKeyHash> exact(
        keys.begin(), keys.end());
    CHECK(exact.size() == keys.size());
}

void test_exponent_parity_and_deep_fallback() {
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    Relation relation = make_relation(2);
    relation.rational_large_prime.push_back({97, 0, 0});        // zero: absent
    relation.rational_large_prime.push_back({101, 0, 2});       // even: absent
    relation.rational_large_prime.push_back({103, 0, 2});
    relation.rational_large_prime.push_back({103, 0, 1});       // total 3: present
    relation.algebraic_large_prime.push_back({107, 7, 1});
    relation.algebraic_large_prime.push_back({107, 7, 1});      // total 2: absent
    relation.algebraic_large_prime.push_back({109, 9, 255});    // odd: present
    relation.algebraic_large_prime.push_back({113, 11, 254});   // even: absent
    relation.algebraic_large_prime.push_back({max, max, 0});    // zero: absent
    relation.algebraic_large_prime.push_back({127, 13, 255});
    relation.algebraic_large_prime.push_back({127, 13, 1});     // total 256: absent

    const std::vector<LargePrimeKey> expected{
        LargePrimeKey{103, 0, false},
        LargePrimeKey{109, 9, true},
    };
    const auto parity_keys = odd_large_prime_keys(relation);
    CHECK(parity_keys == expected);
    CHECK(!odd_large_prime_keys_empty(relation));
    CHECK(std::find(parity_keys.begin(), parity_keys.end(),
                    LargePrimeKey{97, 0, false}) == parity_keys.end());
    CHECK(std::find(parity_keys.begin(), parity_keys.end(),
                    LargePrimeKey{127, 13, true}) == parity_keys.end());
    CHECK(std::find(parity_keys.begin(), parity_keys.end(),
                    LargePrimeKey{max, max, true}) == parity_keys.end());

    Relation empty = make_relation(3);
    CHECK(odd_large_prime_keys_empty(empty));
    CHECK(count_odd_large_prime_keys(empty) == 0);

    // Exercise the >8-entry hash fallback. p=1005 occurs twice and cancels.
    Relation deep = make_relation(4);
    for (uint64_t p = 1000; p < 1012; ++p) {
        deep.rational_large_prime.push_back({p, 0, 1});
    }
    deep.rational_large_prime.push_back({1005, 0, 1});
    deep.algebraic_large_prime.push_back({5000, 3000, 2});
    auto deep_keys = odd_large_prime_keys(deep);
    CHECK(deep_keys.size() == 11);
    CHECK(std::is_sorted(deep_keys.begin(), deep_keys.end()));
    CHECK(std::find(deep_keys.begin(), deep_keys.end(),
                    LargePrimeKey{1005, 0, false}) == deep_keys.end());
}

void test_merge_xor_homomorphism() {
    Relation lhs = make_relation(5);
    lhs.rational_large_prime.push_back({101, 0, 1});
    lhs.algebraic_large_prime.push_back({211, 17, 1});
    lhs.algebraic_large_prime.push_back({223, 19, 2});

    Relation rhs = make_relation(6);
    rhs.rational_large_prime.push_back({101, 0, 3});
    rhs.rational_large_prime.push_back({103, 0, 1});
    rhs.algebraic_large_prime.push_back({227, 23, 1});

    const auto lhs_keys = odd_large_prime_keys(lhs);
    const auto rhs_keys = odd_large_prime_keys(rhs);
    std::vector<LargePrimeKey> expected;
    std::set_symmetric_difference(
        lhs_keys.begin(), lhs_keys.end(),
        rhs_keys.begin(), rhs_keys.end(),
        std::back_inserter(expected));

    Relation merged = PartialRelationMerger::merge_two(lhs, rhs);
    CHECK(odd_large_prime_keys(merged) == expected);
}

void test_metrics_filtering_and_separation() {
    Relation odd_a = make_rational_lp_relation(10, 131, 1);
    Relation odd_b = make_rational_lp_relation(11, 131, 3);
    Relation even_entry = make_rational_lp_relation(12, 131, 2);
    Relation even_duplicates = make_relation(13);
    even_duplicates.rational_large_prime.push_back({131, 0, 1});
    even_duplicates.rational_large_prime.push_back({131, 0, 1});

    std::vector<Relation> relations{
        odd_a, odd_b, even_entry, even_duplicates,
    };

    const LargePrimeKey key{131, 0, false};
    auto weights = RelationFilter::count_large_primes(relations);
    CHECK(weights.size() == 1);
    CHECK(weights.at(key) == 2);
    CHECK(RelationFilter::get_unique_large_primes(relations) ==
          std::vector<LargePrimeKey>{key});
    CHECK(count_unique_lp_keys(relations) == 1);

    auto histogram = count_lp_key_weights(relations);
    CHECK(histogram.unique_keys == 1);
    CHECK(histogram.weight_1 == 0);
    CHECK(histogram.weight_2 == 1);
    CHECK(histogram.weight_3 == 0);
    CHECK(histogram.weight_4plus == 0);

    // Preserve the raw-storage API while classifying by effective GF(2) LPs.
    CHECK(!even_entry.is_full());
    CHECK(even_entry.num_large_primes() == 1);
    CHECK(PartialRelationMerger::is_effectively_full(even_entry));

    std::vector<Relation> to_separate{even_entry, odd_a};
    auto separated = separate_relations(std::move(to_separate));
    CHECK(separated.full.size() == 1);
    CHECK(separated.partial.size() == 1);
    CHECK(separated.full.front().rational_large_prime.size() == 1);
    CHECK(separated.full.front().rational_large_prime.front().e == 2);

    // The even-exponent row has no singleton column and must survive filtering.
    std::vector<Relation> filter_input{
        even_entry,
        make_rational_lp_relation(14, 137, 1),
    };
    RelationFilter filter;
    auto filtered = filter.filter(std::move(filter_input));
    CHECK(filtered.size() == 1);
    CHECK(filtered.front().a == even_entry.a);
}

void test_v0_v3_effective_classification() {
    Relation even = make_rational_lp_relation(20, 149, 2);

    PartialRelationMerger::MergeStats v0_even_stats;
    auto v0_even = PartialRelationMerger::merge_all(
        std::vector<Relation>{even}, 2, &v0_even_stats);
    CHECK(v0_even.size() == 1);
    CHECK(PartialRelationMerger::is_effectively_full(v0_even.front()));
    CHECK(v0_even.front().rational_large_prime.front().e == 2);
    CHECK(v0_even_stats.input_1lp == 0);
    CHECK(v0_even_stats.input_2lp == 0);
    CHECK(v0_even_stats.full_produced == 1);
    CHECK(v0_even_stats.residual_emitted == 0);
    CHECK(v0_even_stats.residual_dropped == 0);
    CHECK(v0_even_stats.output_relations == 1);

    CliqueStats v3_even_stats;
    auto v3_even = CliqueRelationMerger::merge_cliques(
        std::vector<Relation>{even}, &v3_even_stats);
    CHECK(v3_even.size() == 1);
    CHECK(v3_even_stats.full_produced == 1);
    CHECK(v3_even_stats.input_1lp == 0);
    CHECK(v3_even_stats.input_2lp == 0);

    // Both rows are effectively 1LP although the first stores two entries.
    Relation first = make_relation(21);
    first.rational_large_prime.push_back({151, 0, 2});
    first.rational_large_prime.push_back({151, 0, 1});
    Relation second = make_rational_lp_relation(22, 151, 3);

    PartialRelationMerger::MergeStats v0_pair_stats;
    auto v0_pair = PartialRelationMerger::merge_all(
        std::vector<Relation>{first, second}, 2, &v0_pair_stats);
    CHECK(v0_pair.size() == 1);
    CHECK(PartialRelationMerger::is_effectively_full(v0_pair.front()));
    CHECK(v0_pair_stats.input_1lp == 2);
    CHECK(v0_pair_stats.weight2_merges == 1);
    CHECK(v0_pair_stats.full_produced == 1);
    CHECK(v0_pair_stats.residual_emitted == 0);
    CHECK(v0_pair_stats.output_relations == 1);

    auto legacy_pair = PartialRelationMerger::merge(
        std::vector<Relation>{first, second});
    CHECK(legacy_pair.size() == 1);
    CHECK(PartialRelationMerger::is_effectively_full(legacy_pair.front()));

    CliqueStats v3_pair_stats;
    auto v3_pair = CliqueRelationMerger::merge_cliques(
        std::vector<Relation>{first, second}, &v3_pair_stats);
    CHECK(v3_pair.size() == 1);
    CHECK(PartialRelationMerger::is_effectively_full(v3_pair.front()));
    CHECK(v3_pair_stats.input_1lp == 2);

    // One V0 round merges the shared middle key but leaves the two endpoint
    // keys. The returned row is a residual output, never a full relation.
    Relation residual_left = make_relation(26);
    residual_left.rational_large_prime.push_back({167, 0, 1});
    residual_left.rational_large_prime.push_back({173, 0, 1});
    Relation residual_right = make_relation(27);
    residual_right.rational_large_prime.push_back({173, 0, 1});
    residual_right.rational_large_prime.push_back({179, 0, 1});
    PartialRelationMerger::MergeStats residual_stats;
    auto residual_output = PartialRelationMerger::merge_all(
        std::vector<Relation>{residual_left, residual_right}, 1,
        &residual_stats);
    CHECK(residual_output.size() == 1);
    CHECK(!PartialRelationMerger::is_effectively_full(
        residual_output.front()));
    CHECK(residual_stats.full_produced == 0);
    CHECK(residual_stats.residual_emitted == 1);
    CHECK(residual_stats.residual_dropped == 0);
    CHECK(residual_stats.output_relations == 1);
    CHECK(residual_stats.output_relations ==
          residual_stats.full_produced + residual_stats.residual_emitted);

    // Regression: after one pair becomes full, the third relation belongs to
    // the same component but accepts no neighbour of its own. Component-global
    // visited.size() must not make that unmerged singleton look like a residual.
    Relation third_a = make_rational_lp_relation(23, 163, 1);
    Relation third_b = make_rational_lp_relation(24, 163, 1);
    Relation unmerged = make_rational_lp_relation(25, 163, 1);
    CliqueStats local_accounting_stats;
    auto local_accounting = CliqueRelationMerger::merge_cliques(
        std::vector<Relation>{third_a, third_b, unmerged},
        &local_accounting_stats);
    CHECK(local_accounting.size() == 1);
    CHECK(PartialRelationMerger::is_effectively_full(
        local_accounting.front()));
    CHECK(local_accounting_stats.full_produced == 1);
    CHECK(local_accounting_stats.residual_emitted == 0);
    CHECK(local_accounting_stats.singletons_removed == 1);
    CHECK(local_accounting_stats.input_1lp == 3);
    const size_t emitted_source_count =
        1 + local_accounting.front().extra_ab_pairs.size();
    CHECK(emitted_source_count == 2);
    CHECK(emitted_source_count + local_accounting_stats.singletons_removed ==
          local_accounting_stats.input_relations);
}

void test_matrix_builder_uses_canonical_support() {
    constexpr uint64_t high = uint64_t{1} << 32;

    Relation first = make_relation(30);
    first.rational_large_prime.push_back({17, 0, 1});
    first.algebraic_large_prime.push_back({17, 19, 1});

    Relation high_prime = make_algebraic_lp_relation(31, high + 17, 19, 1);
    Relation high_root = make_algebraic_lp_relation(32, 17, high + 19, 1);

    Relation mixed = make_relation(33);
    mixed.rational_large_prime.push_back({23, 0, 2});
    mixed.algebraic_large_prime.push_back({29, 7, 2});
    mixed.algebraic_large_prime.push_back({29, 7, 1});

    std::vector<Relation> relations{
        first, high_prime, high_root, mixed,
    };

    FactorBase empty_factor_base;
    MatrixBuilder builder;
    auto built = builder.build(relations, empty_factor_base);

    const size_t mapped_lp_columns = built.mapping.num_large_primes_rat +
                                     built.mapping.num_large_primes_alg;
    CHECK(mapped_lp_columns == 5);
    CHECK(mapped_lp_columns == count_unique_lp_keys(relations));
    CHECK(built.mapping.num_large_primes_rat == 1);
    CHECK(built.mapping.num_large_primes_alg == 4);

    for (size_t row_index = 0; row_index < relations.size(); ++row_index) {
        auto keys = odd_large_prime_keys(relations[row_index]);
        const auto& row = built.matrix.row(row_index);
        CHECK(row.weight() == keys.size());

        for (const auto& key : keys) {
            uint32_t column = 0;
            if (key.is_algebraic) {
                auto it = built.mapping.alg_lp_to_col.find(
                    PrimeIdealKey{key.prime, key.root});
                CHECK(it != built.mapping.alg_lp_to_col.end());
                column = it->second;
            } else {
                auto it = built.mapping.rat_lp_to_col.find(key.prime);
                CHECK(it != built.mapping.rat_lp_to_col.end());
                column = it->second;
            }
            CHECK(row.test(column));
        }
    }

    // Column IDs are a pure sorted-key function, independent of relation order.
    auto reversed_relations = relations;
    std::reverse(reversed_relations.begin(), reversed_relations.end());
    auto reversed = builder.build(reversed_relations, empty_factor_base);
    check_mapping_equal(built.mapping, reversed.mapping);

    // The vector and RelationSource paths share both canonical collection and
    // deterministic column assignment.
    MatrixBuilderConfig streaming_config;
    streaming_config.include_sign_column = false;
    streaming_config.include_qc_columns = false;
    streaming_config.include_class_group = false;
    streaming_config.include_schirokauer = false;
    MatrixBuilder streaming_builder(streaming_config);

    std::vector<Integer> polynomial_coefficients;
    polynomial_coefficients.emplace_back(-10);
    polynomial_coefficients.emplace_back(1);
    PolynomialContext context(
        Integer(91), std::move(polynomial_coefficients), Integer(10));

    auto vector_result = streaming_builder.build_with_qc(
        relations, empty_factor_base, context);
    VectorRelationSource source(relations);
    auto streaming_result = streaming_builder.build_with_qc_streaming(
        source, empty_factor_base, context);
    check_mapping_equal(vector_result.mapping, streaming_result.mapping);
    check_matrix_equal(vector_result.matrix, streaming_result.matrix);
}

void test_ooc_relation_source_preserves_canonical_contract() {
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();

    // Deliberately keep the raw LP entries unsorted and mix full-width values,
    // zero exponents, and totals of 256. The effective global keys are:
    // rational {3, 5, UINT64_MAX}, algebraic {(5,0), (5,MAX), (MAX,MAX)}.
    Relation first = make_relation(50);
    first.rational_large_prime.push_back({max, 0, 1});
    first.rational_large_prime.push_back({7, 0, 0});
    first.rational_large_prime.push_back({5, 0, 1});
    first.algebraic_large_prime.push_back({max, max, 1});
    first.algebraic_large_prime.push_back({5, 0, 1});

    Relation second = make_relation(51);
    second.rational_large_prime.push_back({11, 0, 255});
    second.rational_large_prime.push_back({3, 0, 1});
    second.rational_large_prime.push_back({11, 0, 1});
    second.algebraic_large_prime.push_back({5, max, 1});

    Relation third = make_relation(52);
    third.rational_large_prime.push_back({max, 0, 1});
    third.rational_large_prime.push_back({max, 0, 1});
    third.algebraic_large_prime.push_back({5, 0, 255});
    third.algebraic_large_prime.push_back({5, 0, 1});

    std::vector<Relation> corpus{first, second, third};

    MatrixBuilderConfig config;
    config.include_sign_column = false;
    config.include_qc_columns = false;
    config.include_class_group = false;
    config.include_schirokauer = false;
    MatrixBuilder builder(config);
    FactorBase empty_factor_base;

    std::vector<Integer> polynomial_coefficients;
    polynomial_coefficients.emplace_back(-10);
    polynomial_coefficients.emplace_back(1);
    PolynomialContext context(
        Integer(91), std::move(polynomial_coefficients), Integer(10));

    auto vector_result = builder.build_with_qc(
        corpus, empty_factor_base, context);

    TempOOCStore store;
    {
        OOCRelationWriter writer(store.base);
        for (const auto& relation : corpus) writer.write(relation);
        writer.close();
    }
    OOCRelationReader reader(store.base);
    CHECK(reader.count() == corpus.size());
    OOCRelationSource source(reader);
    auto ooc_result = builder.build_with_qc_streaming(
        source, empty_factor_base, context);

    check_mapping_equal(vector_result.mapping, ooc_result.mapping);
    check_matrix_equal(vector_result.matrix, ooc_result.matrix);

    const auto& mapping = ooc_result.mapping;
    CHECK(mapping.num_large_primes_rat == 3);
    CHECK(mapping.num_large_primes_alg == 3);
    CHECK(mapping.rat_lp_to_col.at(3) == 0);
    CHECK(mapping.rat_lp_to_col.at(5) == 1);
    CHECK(mapping.rat_lp_to_col.at(max) == 2);
    CHECK(mapping.alg_lp_to_col.at(PrimeIdealKey{5, 0}) == 3);
    CHECK(mapping.alg_lp_to_col.at(PrimeIdealKey{5, max}) == 4);
    CHECK(mapping.alg_lp_to_col.at(PrimeIdealKey{max, max}) == 5);

    const std::vector<std::vector<uint32_t>> expected_rows{
        {1, 2, 3, 5},
        {0, 4},
        {},
    };
    CHECK(ooc_result.matrix.num_rows() == expected_rows.size());
    for (size_t i = 0; i < expected_rows.size(); ++i) {
        CHECK(ooc_result.matrix.row(i).indices() == expected_rows[i]);
    }
}

void test_algebraic_square_verifier_uses_full_width_keys() {
    constexpr uint64_t high = uint64_t{1} << 32;
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();

    Relation base = make_algebraic_lp_relation(40, 17, 19, 1);
    Relation high_prime = make_algebraic_lp_relation(41, high + 17, 19, 1);
    Relation high_root = make_algebraic_lp_relation(42, 17, high + 19, 1);
    Relation same = make_algebraic_lp_relation(43, 17, 19, 1);

    BitVector pair(2);
    pair.set(0);
    pair.set(1);

    CHECK_MSG(!verify_algebraic_ideal_powers(
                  pair, std::vector<Relation>{base, high_prime}),
              "p values with identical low 32 bits are distinct ideals");
    CHECK_MSG(!verify_algebraic_ideal_powers(
                  pair, std::vector<Relation>{base, high_root}),
              "roots with identical low 32 bits are distinct ideals");
    CHECK(verify_algebraic_ideal_powers(
        pair, std::vector<Relation>{base, same}));

    // Per-row normalization must XOR repeated exponents before dependency
    // aggregation. 255 + 1 = 256 is even despite overflowing uint8_t if summed
    // in the storage type.
    Relation inline_even = make_relation(44);
    inline_even.algebraic_large_prime.push_back({max, max, 255});
    inline_even.algebraic_large_prime.push_back({max, max, 1});
    BitVector one_row(1);
    one_row.set(0);
    CHECK(verify_algebraic_ideal_powers(
        one_row, std::vector<Relation>{inline_even}));

    // The same XOR law applies across selected dependency rows.
    Relation cross_left = make_algebraic_lp_relation(45, max, max, 255);
    Relation cross_right = make_algebraic_lp_relation(46, max, max, 1);
    CHECK(verify_algebraic_ideal_powers(
        pair, std::vector<Relation>{cross_left, cross_right}));
    CHECK(!verify_algebraic_ideal_powers(
        one_row, std::vector<Relation>{cross_left}));
}

}  // namespace

int main() {
    std::cout << "=== Canonical LP Key Contract Tests ===\n";

    test_full_width_identity_and_side();
    test_exponent_parity_and_deep_fallback();
    test_merge_xor_homomorphism();
    test_metrics_filtering_and_separation();
    test_v0_v3_effective_classification();
    test_matrix_builder_uses_canonical_support();
    test_ooc_relation_source_preserves_canonical_contract();
    test_algebraic_square_verifier_uses_full_width_keys();

    std::cout << "All canonical LP key contract tests passed.\n";
    return EXIT_SUCCESS;
}
