// test_siqs_2lp_materializer.cpp - SIQS 2LP cycle materialization contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/two_large_prime_materializer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using gnfs::core::Integer;
using gnfs::siqs::IndexedTwoLargePrimeCycleSources;
using gnfs::siqs::materialize_two_large_prime_cycle;
using gnfs::siqs::MaterializedTwoLargePrimeCycle;
using gnfs::siqs::TwoLargePrimeCycleSource;

static_assert(!std::is_default_constructible_v<IndexedTwoLargePrimeCycleSources>);
static_assert(!std::is_copy_constructible_v<IndexedTwoLargePrimeCycleSources>);
static_assert(std::is_nothrow_move_constructible_v<IndexedTwoLargePrimeCycleSources>);
static_assert(!std::is_constructible_v<IndexedTwoLargePrimeCycleSources,
                                       std::span<const TwoLargePrimeCycleSource>>);
static_assert(!std::is_constructible_v<IndexedTwoLargePrimeCycleSources,
                                       std::vector<TwoLargePrimeCycleSource>>);

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (condition) {                                                        \
            ++checks_passed;                                                    \
        } else {                                                                \
            ++checks_failed;                                                    \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':'       \
                      << __LINE__ << '\n';                                      \
        }                                                                       \
    } while (false)

[[nodiscard]] TwoLargePrimeCycleSource make_source(
        size_t relation_index,
        int64_t value,
        bool negative,
        std::vector<uint32_t> factor_base_exponents,
        uint64_t p,
        uint64_t q) {
    return TwoLargePrimeCycleSource{
        relation_index,
        Integer(value),
        negative,
        std::move(factor_base_exponents),
        p,
        q,
    };
}

[[nodiscard]] std::optional<MaterializedTwoLargePrimeCycle> materialize(
        const std::vector<TwoLargePrimeCycleSource>& sources,
        const std::vector<size_t>& cycle_relation_indices,
        const Integer& modulus) {
    return materialize_two_large_prime_cycle(
        std::span<const TwoLargePrimeCycleSource>(sources.data(), sources.size()),
        std::span<const size_t>(cycle_relation_indices.data(),
                                cycle_relation_indices.size()),
        modulus);
}

[[nodiscard]] std::optional<MaterializedTwoLargePrimeCycle>
materialize_indexed(const IndexedTwoLargePrimeCycleSources& sources,
                    const std::vector<size_t>& sorted_cycle_relation_indices,
                    const Integer& modulus) {
    return materialize_two_large_prime_cycle(
        sources,
        std::span<const size_t>(sorted_cycle_relation_indices.data(),
                                sorted_cycle_relation_indices.size()),
        modulus);
}

void check_materialized(
        const std::optional<MaterializedTwoLargePrimeCycle>& result,
        const Integer& value_modulus,
        bool negative,
        const std::vector<uint32_t>& factor_base_exponents,
        const std::vector<uint64_t>& large_prime_square_roots,
        const std::vector<size_t>& relation_indices) {
    CHECK(result.has_value());
    if (!result) {
        return;
    }

    CHECK(result->value_modulus == value_modulus);
    CHECK(result->negative == negative);
    CHECK(result->factor_base_exponents == factor_base_exponents);
    CHECK(result->large_prime_square_roots == large_prime_square_roots);
    CHECK(result->relation_indices == relation_indices);
}

[[nodiscard]] bool same_materialized(
        const MaterializedTwoLargePrimeCycle& lhs,
        const MaterializedTwoLargePrimeCycle& rhs) {
    return lhs.value_modulus == rhs.value_modulus &&
           lhs.negative == rhs.negative &&
           lhs.factor_base_exponents == rhs.factor_base_exponents &&
           lhs.large_prime_square_roots == rhs.large_prime_square_roots &&
           lhs.relation_indices == rhs.relation_indices;
}

[[nodiscard]] std::vector<TwoLargePrimeCycleSource> one_lp_parallel_sources() {
    std::vector<TwoLargePrimeCycleSource> sources;
    sources.push_back(make_source(10, 20, false, {1, 2, 3}, 0, 101));
    sources.push_back(make_source(11, 30, true, {4, 5, 6}, 0, 101));
    return sources;
}

[[nodiscard]] std::vector<TwoLargePrimeCycleSource> triangle_sources() {
    std::vector<TwoLargePrimeCycleSource> sources;
    sources.push_back(make_source(30, 2, false, {100, 1}, 101, 103));
    sources.push_back(make_source(31, 3, true, {100, 2}, 103, 107));
    sources.push_back(make_source(32, 5, true, {100, 3}, 107, 101));
    return sources;
}

[[nodiscard]] std::vector<TwoLargePrimeCycleSource> contiguous_triangle_sources() {
    auto sources = triangle_sources();
    for (size_t relation_index = 0; relation_index < sources.size(); ++relation_index) {
        sources[relation_index].relation_index = relation_index;
    }
    return sources;
}

void test_parallel_one_lp_materialization() {
    const auto sources = one_lp_parallel_sources();
    const std::vector<size_t> cycle{10, 11};
    const auto result = materialize(sources, cycle, Integer(97));

    check_materialized(result,
                       Integer(18),
                       true,
                       {5, 7, 9},
                       {101},
                       {10, 11});
}

void test_negative_values_use_canonical_modulus() {
    auto sources = one_lp_parallel_sources();
    sources[0].value = Integer(-20);
    sources[0].negative = false;
    sources[1].negative = false;
    const std::vector<size_t> cycle{10, 11};
    const auto result = materialize(sources, cycle, Integer(97));

    check_materialized(result,
                       Integer(79),
                       false,
                       {5, 7, 9},
                       {101},
                       {10, 11});
}

void test_square_self_loop_materialization() {
    const std::vector<TwoLargePrimeCycleSource> sources{
        make_source(20, 23, true, {7, 8, 9}, 103, 103),
    };
    const std::vector<size_t> cycle{20};
    const auto result = materialize(sources, cycle, Integer(97));

    check_materialized(result,
                       Integer(23),
                       true,
                       {7, 8, 9},
                       {103},
                       {20});
}

void test_triangle_and_wide_exponent_sum() {
    const auto sources = triangle_sources();
    const std::vector<size_t> cycle{30, 31, 32};
    const auto result = materialize(sources, cycle, Integer(97));

    check_materialized(result,
                       Integer(30),
                       false,
                       {300, 6},
                       {101, 103, 107},
                       {30, 31, 32});
}

void test_degree_four_vertex_repeats_square_root() {
    std::vector<TwoLargePrimeCycleSource> sources;
    sources.push_back(make_source(40, 2, false, {1, 0}, 101, 103));
    sources.push_back(make_source(41, 3, false, {2, 1}, 103, 101));
    sources.push_back(make_source(42, 5, false, {3, 1}, 101, 107));
    sources.push_back(make_source(43, 7, false, {4, 2}, 107, 101));

    const std::vector<size_t> cycle{40, 41, 42, 43};
    const auto result = materialize(sources, cycle, Integer(97));
    check_materialized(result,
                       Integer(16),
                       false,
                       {10, 4},
                       {101, 101, 103, 107},
                       {40, 41, 42, 43});
}

void test_endpoint_and_cycle_order_are_deterministic() {
    const auto sources = triangle_sources();
    const std::vector<size_t> ordered_cycle{30, 31, 32};
    const auto baseline = materialize(sources, ordered_cycle, Integer(97));

    auto reversed_sources = sources;
    std::reverse(reversed_sources.begin(), reversed_sources.end());
    for (auto& source : reversed_sources) {
        std::swap(source.p, source.q);
    }
    const std::vector<size_t> shuffled_cycle{32, 30, 31};
    const auto reordered = materialize(
        reversed_sources, shuffled_cycle, Integer(97));

    CHECK(baseline.has_value());
    CHECK(reordered.has_value());
    if (baseline && reordered) {
        CHECK(same_materialized(*baseline, *reordered));
        const std::vector<size_t> expected_indices{30, 31, 32};
        CHECK(reordered->relation_indices == expected_indices);
    }
}

void test_invalid_modulus_and_cycle_identifiers_fail_closed() {
    const auto sources = one_lp_parallel_sources();
    const std::vector<size_t> valid_cycle{10, 11};

    CHECK(!materialize(sources, valid_cycle, Integer(1)).has_value());
    CHECK(!materialize(sources, valid_cycle, Integer(0)).has_value());
    CHECK(!materialize(sources, valid_cycle, Integer(-7)).has_value());

    const std::vector<size_t> empty_cycle;
    CHECK(!materialize(sources, empty_cycle, Integer(97)).has_value());

    const std::vector<size_t> duplicate_cycle_id{10, 10};
    CHECK(!materialize(sources, duplicate_cycle_id, Integer(97)).has_value());

    const std::vector<size_t> unknown_cycle_id{10, 999, 11};
    CHECK(!materialize(sources, unknown_cycle_id, Integer(97)).has_value());
}

void test_duplicate_sources_and_invalid_endpoints_fail_closed() {
    {
        auto sources = one_lp_parallel_sources();
        sources.push_back(make_source(10, 7, false, {0}, 109, 113));
        const std::vector<size_t> cycle{10, 11};
        CHECK(!materialize(sources, cycle, Integer(97)).has_value());
    }

    {
        auto sources = one_lp_parallel_sources();
        sources.push_back(make_source(99, 7, false, {0}, 109, 113));
        sources.push_back(make_source(99, 11, false, {0}, 127, 131));
        const std::vector<size_t> cycle{10, 11};
        CHECK(!materialize(sources, cycle, Integer(97)).has_value());
    }

    {
        const std::vector<TwoLargePrimeCycleSource> sources{
            make_source(70, 13, false, {1}, 0, 0),
        };
        const std::vector<size_t> cycle{70};
        CHECK(!materialize(sources, cycle, Integer(97)).has_value());
    }

    {
        const std::vector<TwoLargePrimeCycleSource> sources{
            make_source(80, 13, false, {1}, 1, 109),
            make_source(81, 17, false, {2}, 109, 1),
        };
        const std::vector<size_t> cycle{80, 81};
        CHECK(!materialize(sources, cycle, Integer(97)).has_value());
    }
}

void test_shape_parity_and_overflow_fail_closed() {
    {
        std::vector<TwoLargePrimeCycleSource> sources;
        sources.push_back(make_source(90, 2, false, {1, 2}, 0, 127));
        sources.push_back(make_source(91, 3, false, {3}, 0, 127));
        const std::vector<size_t> cycle{90, 91};
        CHECK(!materialize(sources, cycle, Integer(97)).has_value());
    }

    {
        const std::vector<TwoLargePrimeCycleSource> sources{
            make_source(92, 2, false, {1}, 0, 127),
        };
        const std::vector<size_t> cycle{92};
        CHECK(!materialize(sources, cycle, Integer(97)).has_value());
    }

    {
        std::vector<TwoLargePrimeCycleSource> sources;
        sources.push_back(make_source(
            93,
            2,
            false,
            {std::numeric_limits<uint32_t>::max()},
            0,
            127));
        sources.push_back(make_source(94, 3, false, {1}, 0, 127));
        const std::vector<size_t> cycle{93, 94};
        CHECK(!materialize(sources, cycle, Integer(97)).has_value());
    }
}

void test_unselected_exponent_shape_is_irrelevant() {
    auto sources = one_lp_parallel_sources();
    // Only source IDs are global lookup state. Other fields of an unselected
    // source are outside this materialization operation's validation scope.
    sources.push_back(make_source(99, 42, true, {9}, 1, 0));
    const std::vector<size_t> cycle{11, 10};
    const auto result = materialize(sources, cycle, Integer(97));

    check_materialized(result,
                       Integer(18),
                       true,
                       {5, 7, 9},
                       {101},
                       {10, 11});
}

void test_indexed_and_generic_materialization_match_exactly() {
    auto sources = contiguous_triangle_sources();
    const std::vector<size_t> shuffled_cycle{2, 0, 1};
    const std::vector<size_t> sorted_cycle{0, 1, 2};
    const auto generic = materialize(sources, shuffled_cycle, Integer(97));

    auto indexed_sources = IndexedTwoLargePrimeCycleSources::try_create(std::move(sources));
    CHECK(indexed_sources.has_value());
    if (!indexed_sources) {
        return;
    }
    const auto indexed = materialize_indexed(*indexed_sources, sorted_cycle, Integer(97));

    CHECK(generic.has_value());
    CHECK(indexed.has_value());
    if (generic && indexed) {
        CHECK(same_materialized(*generic, *indexed));
    }
}

void test_indexed_corpus_owns_and_validates_source_storage() {
    // The temporary vector is destroyed at the end of this statement. The
    // successful later lookup proves the validated corpus owns its storage.
    auto owned_temporary =
        IndexedTwoLargePrimeCycleSources::try_create(contiguous_triangle_sources());
    CHECK(owned_temporary.has_value());
    if (owned_temporary) {
        const std::vector<size_t> cycle{0, 1, 2};
        CHECK(materialize_indexed(*owned_temporary, cycle, Integer(97)).has_value());
    }

    {
        auto sources = contiguous_triangle_sources();
        sources[1].relation_index = 2;
        CHECK(!IndexedTwoLargePrimeCycleSources::try_create(std::move(sources)).has_value());
    }
    {
        auto sources = contiguous_triangle_sources();
        sources[1].relation_index = 0;
        CHECK(!IndexedTwoLargePrimeCycleSources::try_create(std::move(sources)).has_value());
    }
    {
        auto sources = contiguous_triangle_sources();
        std::swap(sources[0], sources[1]);
        CHECK(!IndexedTwoLargePrimeCycleSources::try_create(std::move(sources)).has_value());
    }
}

void test_indexed_cycle_contract_fails_closed() {
    auto indexed_sources =
        IndexedTwoLargePrimeCycleSources::try_create(contiguous_triangle_sources());
    CHECK(indexed_sources.has_value());
    if (!indexed_sources) {
        return;
    }

    CHECK(!materialize_indexed(*indexed_sources, {1, 0}, Integer(97)).has_value());
    CHECK(!materialize_indexed(*indexed_sources, {0, 2, 1}, Integer(97)).has_value());
    CHECK(!materialize_indexed(*indexed_sources, {0, 0}, Integer(97)).has_value());
    CHECK(!materialize_indexed(*indexed_sources, {0, 3}, Integer(97)).has_value());
    CHECK(!materialize_indexed(*indexed_sources, {}, Integer(97)).has_value());
}

void test_indexed_overflow_and_unselected_source_policy_match_generic() {
    {
        std::vector<TwoLargePrimeCycleSource> sources;
        sources.push_back(make_source(0, 2, false, {std::numeric_limits<uint32_t>::max()}, 0, 127));
        sources.push_back(make_source(1, 3, false, {1}, 0, 127));
        const std::vector<size_t> cycle{0, 1};
        const auto generic = materialize(sources, cycle, Integer(97));
        auto indexed_sources = IndexedTwoLargePrimeCycleSources::try_create(std::move(sources));
        CHECK(indexed_sources.has_value());
        if (indexed_sources) {
            const auto indexed = materialize_indexed(*indexed_sources, cycle, Integer(97));
            CHECK(!generic.has_value());
            CHECK(!indexed.has_value());
        }
    }

    {
        auto sources = one_lp_parallel_sources();
        sources[0].relation_index = 0;
        sources[1].relation_index = 1;
        // Identity is corpus-wide, but endpoint and exponent-shape checks stay
        // scoped to selected sources exactly as in the generic API.
        sources.push_back(make_source(2, 42, true, {9}, 1, 0));
        const std::vector<size_t> cycle{0, 1};
        const auto generic = materialize(sources, cycle, Integer(97));
        auto indexed_sources = IndexedTwoLargePrimeCycleSources::try_create(std::move(sources));
        CHECK(indexed_sources.has_value());
        if (!indexed_sources) {
            return;
        }
        const auto indexed = materialize_indexed(*indexed_sources, cycle, Integer(97));
        CHECK(generic.has_value());
        CHECK(indexed.has_value());
        if (generic && indexed) {
            CHECK(same_materialized(*generic, *indexed));
        }
    }
}

void test_indexed_selected_source_validation_matches_generic() {
    {
        std::vector<TwoLargePrimeCycleSource> sources;
        sources.push_back(make_source(0, 2, false, {1, 2}, 0, 127));
        sources.push_back(make_source(1, 3, false, {3}, 0, 127));
        const std::vector<size_t> cycle{0, 1};
        const auto generic = materialize(sources, cycle, Integer(97));
        auto indexed_sources = IndexedTwoLargePrimeCycleSources::try_create(std::move(sources));
        CHECK(indexed_sources.has_value());
        if (indexed_sources) {
            CHECK(!generic.has_value());
            CHECK(!materialize_indexed(*indexed_sources, cycle, Integer(97)).has_value());
        }
    }

    {
        std::vector<TwoLargePrimeCycleSource> sources;
        sources.push_back(make_source(0, 2, false, {1}, 1, 109));
        sources.push_back(make_source(1, 3, false, {1}, 109, 1));
        const std::vector<size_t> cycle{0, 1};
        const auto generic = materialize(sources, cycle, Integer(97));
        auto indexed_sources = IndexedTwoLargePrimeCycleSources::try_create(std::move(sources));
        CHECK(indexed_sources.has_value());
        if (indexed_sources) {
            CHECK(!generic.has_value());
            CHECK(!materialize_indexed(*indexed_sources, cycle, Integer(97)).has_value());
        }
    }
}

void test_large_indexed_corpus_reuses_one_corpus_for_small_cycles() {
    constexpr size_t source_count = 8192;
    constexpr size_t cycle_count = 256;
    std::vector<TwoLargePrimeCycleSource> sources;
    sources.reserve(source_count);
    for (size_t relation_index = 0; relation_index < source_count; ++relation_index) {
        const uint64_t endpoint = 101 + static_cast<uint64_t>(relation_index / 2);
        sources.push_back(make_source(relation_index, 2, false, {1}, 0, endpoint));
    }

    auto indexed_sources = IndexedTwoLargePrimeCycleSources::try_create(std::move(sources));
    CHECK(indexed_sources.has_value());
    if (!indexed_sources) {
        return;
    }

    // One O(E) validation is shared by many O(L) selections with L == 2.
    for (size_t cycle_ordinal = 0; cycle_ordinal < cycle_count; ++cycle_ordinal) {
        const std::vector<size_t> cycle{cycle_ordinal * 2, cycle_ordinal * 2 + 1};
        const auto result = materialize_indexed(*indexed_sources, cycle, Integer(97));
        CHECK(result.has_value());
        if (result) {
            CHECK(result->relation_indices == cycle);
        }
    }
}

} // namespace

int main() {
    test_parallel_one_lp_materialization();
    test_negative_values_use_canonical_modulus();
    test_square_self_loop_materialization();
    test_triangle_and_wide_exponent_sum();
    test_degree_four_vertex_repeats_square_root();
    test_endpoint_and_cycle_order_are_deterministic();
    test_invalid_modulus_and_cycle_identifiers_fail_closed();
    test_duplicate_sources_and_invalid_endpoints_fail_closed();
    test_shape_parity_and_overflow_fail_closed();
    test_unselected_exponent_shape_is_irrelevant();
    test_indexed_and_generic_materialization_match_exactly();
    test_indexed_corpus_owns_and_validates_source_storage();
    test_indexed_cycle_contract_fails_closed();
    test_indexed_overflow_and_unselected_source_policy_match_generic();
    test_indexed_selected_source_validation_matches_generic();
    test_large_indexed_corpus_reuses_one_corpus_for_small_cycles();

    std::cout << checks_passed << " checks passed, " << checks_failed
              << " checks failed\n";
    return checks_failed == 0 ? 0 : 1;
}
