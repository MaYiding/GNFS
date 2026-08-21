#include "gnfs/relation/structured_reduction.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::PreparedTreeBasisMerge;
using gnfs::relation::SequentialStructuredReducer;
using gnfs::relation::SourceCombination;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionStopReason;
using gnfs::relation::StructuredRowId;
using gnfs::relation::TreeBasisEdgePlan;
using gnfs::relation::TreeBasisMergePlan;
using gnfs::relation::TreeBasisPlanner;

namespace {

using Mask = uint64_t;
using RowPair = std::pair<uint64_t, uint64_t>;

constexpr size_t kMaxOracleRows = 16;
constexpr size_t kFactorBits = 16;
constexpr size_t kLpBitOffset = kFactorBits;

int checks = 0;
int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " << #condition   \
                      << '\n';                                                                     \
        }                                                                                          \
    } while (false)

void oracle_require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] constexpr LargePrimeKey rational_key(uint64_t prime) noexcept {
    return LargePrimeKey{prime, 0, false};
}

void add_large_prime(Relation& relation, const LargePrimeKey& key) {
    if (key.is_algebraic) {
        relation.algebraic_large_prime.emplace_back(key.prime, key.root, uint8_t{1});
    } else {
        relation.rational_large_prime.emplace_back(key.prime, uint8_t{1});
    }
}

[[nodiscard]] Relation make_relation(std::initializer_list<LargePrimeKey> lp_keys,
                                     std::initializer_list<uint32_t> factors = {}) {
    // The independent AB canary used by the neighbouring structured test maps
    // this pair to zero. Duplicate primary pairs are valid distinct sources.
    Relation relation(0, 16);
    relation.rational_factors.assign(factors.begin(), factors.end());
    for (const auto& key : lp_keys) {
        add_large_prime(relation, key);
    }
    return relation;
}

[[nodiscard]] std::vector<LargePrimeKey> exact_lp_support(const Relation& relation) {
    std::vector<LargePrimeKey> contributions;
    contributions.reserve(relation.rational_large_prime.size() +
                          relation.algebraic_large_prime.size());
    for (const auto& prime_power : relation.rational_large_prime) {
        if ((prime_power.e & 1U) != 0U) {
            contributions.push_back(rational_key(prime_power.p));
        }
    }
    for (const auto& prime_power : relation.algebraic_large_prime) {
        if ((prime_power.e & 1U) != 0U) {
            contributions.push_back(LargePrimeKey{prime_power.p, prime_power.r, true});
        }
    }
    std::sort(contributions.begin(), contributions.end());

    std::vector<LargePrimeKey> support;
    for (size_t begin = 0; begin < contributions.size();) {
        size_t end = begin + 1;
        while (end < contributions.size() && contributions[end] == contributions[begin]) {
            ++end;
        }
        if (((end - begin) & 1U) != 0U) {
            support.push_back(contributions[begin]);
        }
        begin = end;
    }
    return support;
}

[[nodiscard]] std::vector<LargePrimeKey> independent_lp_xor(std::span<const LargePrimeKey> lhs,
                                                            std::span<const LargePrimeKey> rhs) {
    std::vector<LargePrimeKey> result;
    result.reserve(lhs.size() + rhs.size());
    size_t left = 0;
    size_t right = 0;
    while (left < lhs.size() && right < rhs.size()) {
        if (lhs[left] < rhs[right]) {
            result.push_back(lhs[left++]);
        } else if (rhs[right] < lhs[left]) {
            result.push_back(rhs[right++]);
        } else {
            ++left;
            ++right;
        }
    }
    result.insert(result.end(), lhs.begin() + static_cast<std::ptrdiff_t>(left), lhs.end());
    result.insert(result.end(), rhs.begin() + static_cast<std::ptrdiff_t>(right), rhs.end());
    return result;
}

struct GoldenUniverse final {
    std::vector<LargePrimeKey> lp_keys;
};

[[nodiscard]] Mask factor_bit(size_t index) {
    oracle_require(index < kFactorBits, "golden factor bit is out of range");
    return Mask{1} << index;
}

[[nodiscard]] Mask lp_bit(const GoldenUniverse& universe, const LargePrimeKey& key) {
    const auto it = std::find(universe.lp_keys.begin(), universe.lp_keys.end(), key);
    oracle_require(it != universe.lp_keys.end(), "golden LP key is absent from universe");
    const size_t index = static_cast<size_t>(it - universe.lp_keys.begin());
    oracle_require(kLpBitOffset + index < std::numeric_limits<Mask>::digits,
                   "golden LP bit is out of range");
    return Mask{1} << (kLpBitOffset + index);
}

[[nodiscard]] Mask decode_relation(const Relation& relation, const GoldenUniverse& universe) {
    Mask row = 0;
    for (const uint32_t factor : relation.rational_factors) {
        oracle_require(factor < kFactorBits, "relation factor exceeds golden width");
        row ^= factor_bit(factor);
    }
    oracle_require(relation.algebraic_factors.empty(),
                   "tree-basis fixture unexpectedly has algebraic factors");
    for (const auto& key : exact_lp_support(relation)) {
        row ^= lp_bit(universe, key);
    }
    return row;
}

[[nodiscard]] Mask source_mask(const SourceCombination& combination,
                               const SequentialStructuredReducer& reducer) {
    oracle_require(combination.generation() == reducer.corpus().generation(),
                   "source combination generation mismatch");
    oracle_require(reducer.corpus().size() <= kMaxOracleRows,
                   "source corpus exceeds oracle capacity");
    Mask result = 0;
    uint64_t previous = 0;
    bool have_previous = false;
    for (const auto source : combination.sources()) {
        oracle_require(source.generation == reducer.corpus().generation(),
                       "source ID generation mismatch");
        oracle_require(source.ordinal < reducer.corpus().size(),
                       "source ID ordinal is out of range");
        oracle_require(!have_previous || previous < source.ordinal,
                       "source combination is not canonical");
        result |= Mask{1} << source.ordinal;
        previous = source.ordinal;
        have_previous = true;
    }
    oracle_require(result != 0, "logical source combination is empty");
    return result;
}

[[nodiscard]] Mask xor_selected(Mask selection, std::span<const Mask> rows) {
    Mask value = 0;
    for (size_t index = 0; index < rows.size(); ++index) {
        if (((selection >> index) & Mask{1}) != 0) {
            value ^= rows[index];
        }
    }
    return value;
}

[[nodiscard]] Mask subset_count(size_t row_count) {
    oracle_require(row_count <= kMaxOracleRows, "exhaustive oracle row bound exceeded");
    oracle_require(row_count < std::numeric_limits<Mask>::digits,
                   "exhaustive oracle shift bound exceeded");
    return Mask{1} << row_count;
}

[[nodiscard]] std::vector<Mask> exact_span(std::span<const Mask> rows) {
    std::vector<Mask> span;
    for (Mask selection = 0; selection < subset_count(rows.size()); ++selection) {
        span.push_back(xor_selected(selection, rows));
    }
    std::sort(span.begin(), span.end());
    span.erase(std::unique(span.begin(), span.end()), span.end());
    return span;
}

[[nodiscard]] std::vector<Mask> exact_left_kernel(std::span<const Mask> rows) {
    std::vector<Mask> kernel;
    for (Mask selection = 0; selection < subset_count(rows.size()); ++selection) {
        if (xor_selected(selection, rows) == 0) {
            kernel.push_back(selection);
        }
    }
    return kernel;
}

void check_exact_dependency_bijection(const SequentialStructuredReducer& reducer,
                                      std::span<const Mask> golden_rows,
                                      const GoldenUniverse& universe) {
    oracle_require(golden_rows.size() == reducer.corpus().size(),
                   "golden matrix row count mismatch");
    const auto active = reducer.active_row_ids();
    oracle_require(active.size() <= kMaxOracleRows, "active rows exceed oracle capacity");

    std::vector<Mask> reduced_rows;
    std::vector<Mask> transforms;
    for (const auto row : active) {
        const Mask transform = source_mask(reducer.sources(row), reducer);
        const Mask decoded = decode_relation(reducer.materialize(row), universe);
        CHECK(decoded == xor_selected(transform, golden_rows));
        transforms.push_back(transform);
        reduced_rows.push_back(decoded);
    }

    // Overlap between transform masks is expected for a tree basis. Full row
    // rank, not disjoint support, is the invariant.
    CHECK(exact_span(transforms).size() == subset_count(transforms.size()));

    const auto original_kernel = exact_left_kernel(golden_rows);
    const auto reduced_kernel = exact_left_kernel(reduced_rows);
    std::vector<Mask> mapped;
    for (const Mask dependency : reduced_kernel) {
        mapped.push_back(xor_selected(dependency, transforms));
    }
    std::sort(mapped.begin(), mapped.end());
    const auto unique_end = std::unique(mapped.begin(), mapped.end());
    CHECK(unique_end == mapped.end());
    mapped.erase(unique_end, mapped.end());
    CHECK(mapped == original_kernel);
}

struct StatsSnapshot final {
    size_t input_rows = 0;
    size_t singleton_rows_removed = 0;
    size_t two_way_merges = 0;
    size_t tree_basis_batches = 0;
    size_t tree_basis_rows_consumed = 0;
    size_t tree_basis_rows_emitted = 0;
    size_t persistence_limited_plans = 0;
    size_t output_rows = 0;
    StructuredReductionStopReason stop_reason = StructuredReductionStopReason::NotStarted;

    [[nodiscard]] bool operator==(const StatsSnapshot&) const noexcept = default;
};

struct ReducerSnapshot final {
    size_t total_rows = 0;
    size_t active_rows = 0;
    std::vector<uint64_t> active_ids;
    std::vector<Mask> sources;
    std::vector<std::vector<LargePrimeKey>> lp_keys;
    StatsSnapshot stats;

    [[nodiscard]] bool operator==(const ReducerSnapshot&) const noexcept = default;
};

[[nodiscard]] ReducerSnapshot snapshot_state(const SequentialStructuredReducer& reducer) {
    ReducerSnapshot snapshot;
    snapshot.total_rows = reducer.total_row_count();
    snapshot.active_rows = reducer.active_row_count();
    for (const auto row : reducer.active_row_ids()) {
        snapshot.active_ids.push_back(row.value);
        snapshot.sources.push_back(source_mask(reducer.sources(row), reducer));
        const auto keys = reducer.lp_keys(row);
        snapshot.lp_keys.emplace_back(keys.begin(), keys.end());
    }
    const auto& stats = reducer.stats();
    snapshot.stats = StatsSnapshot{
        stats.input_rows,
        stats.singleton_rows_removed,
        stats.two_way_merges,
        stats.tree_basis_batches,
        stats.tree_basis_rows_consumed,
        stats.tree_basis_rows_emitted,
        stats.persistence_limited_plans,
        stats.output_rows,
        stats.stop_reason,
    };
    return snapshot;
}

template <typename Action>
void check_error(StructuredReductionErrorCode expected, Action&& action) {
    bool caught = false;
    try {
        std::forward<Action>(action)();
    } catch (const StructuredReductionError& error) {
        caught = true;
        if (error.code() != expected) {
            std::cerr << "unexpected structured error: expected=" << static_cast<int>(expected)
                      << " actual=" << static_cast<int>(error.code()) << " message=" << error.what()
                      << '\n';
        }
        CHECK(error.code() == expected);
    } catch (const std::exception& error) {
        caught = true;
        CHECK(false);
        std::cerr << "unexpected exception: " << error.what() << '\n';
    } catch (...) {
        caught = true;
        CHECK(false);
    }
    CHECK(caught);
}

[[nodiscard]] RowPair canonical_pair(StructuredRowId lhs, StructuredRowId rhs) {
    if (rhs < lhs) {
        std::swap(lhs, rhs);
    }
    return {lhs.value, rhs.value};
}

[[nodiscard]] std::vector<RowPair> edge_pairs(const TreeBasisMergePlan& plan) {
    std::vector<RowPair> result;
    for (const auto& edge : plan.edges) {
        result.push_back(canonical_pair(edge.endpoints[0], edge.endpoints[1]));
    }
    return result;
}

[[nodiscard]] std::vector<RowPair> sorted_edge_pairs(const TreeBasisMergePlan& plan) {
    auto result = edge_pairs(plan);
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] const TreeBasisMergePlan* find_plan(std::span<const TreeBasisMergePlan> plans,
                                                  const LargePrimeKey& pivot) {
    const auto it = std::find_if(plans.begin(), plans.end(),
                                 [&](const auto& plan) { return plan.pivot == pivot; });
    return it == plans.end() ? nullptr : &*it;
}

[[nodiscard]] size_t member_index(const TreeBasisMergePlan& plan, StructuredRowId row) {
    const auto it = std::find(plan.members.begin(), plan.members.end(), row);
    oracle_require(it != plan.members.end(), "tree edge endpoint is not a member");
    return static_cast<size_t>(it - plan.members.begin());
}

void check_independent_tree_rank(const TreeBasisMergePlan& plan) {
    oracle_require(plan.members.size() <= 8, "tree-rank fixture exceeds weight bound");
    CHECK(plan.edges.size() + 1 == plan.members.size());

    std::vector<Mask> incidence_rows;
    std::vector<RowPair> pairs;
    for (const auto& edge : plan.edges) {
        CHECK(edge.endpoints[0] < edge.endpoints[1]);
        const size_t lhs = member_index(plan, edge.endpoints[0]);
        const size_t rhs = member_index(plan, edge.endpoints[1]);
        CHECK(lhs != rhs);
        incidence_rows.push_back((Mask{1} << lhs) | (Mask{1} << rhs));
        pairs.push_back(canonical_pair(edge.endpoints[0], edge.endpoints[1]));
    }
    std::sort(pairs.begin(), pairs.end());
    CHECK(std::adjacent_find(pairs.begin(), pairs.end()) == pairs.end());

    for (Mask selection = 1; selection < subset_count(incidence_rows.size()); ++selection) {
        CHECK(xor_selected(selection, incidence_rows) != 0);
    }

    std::vector<Mask> expected_even_space;
    for (Mask value = 0; value < subset_count(plan.members.size()); ++value) {
        if ((std::popcount(value) & 1) == 0) {
            expected_even_space.push_back(value);
        }
    }
    CHECK(exact_span(incidence_rows) == expected_even_space);
}

void check_plan_payloads(const SequentialStructuredReducer& reducer,
                         const TreeBasisMergePlan& plan) {
    size_t input_nonpivot_lp_nnz = 0;
    for (const auto member : plan.members) {
        const auto keys = reducer.lp_keys(member);
        CHECK(std::find(keys.begin(), keys.end(), plan.pivot) != keys.end());
        input_nonpivot_lp_nnz += keys.size() - 1;
    }

    size_t output_lp_nnz = 0;
    for (const auto& edge : plan.edges) {
        const Mask expected_sources = source_mask(reducer.sources(edge.endpoints[0]), reducer) ^
                                      source_mask(reducer.sources(edge.endpoints[1]), reducer);
        CHECK(source_mask(edge.expected_sources, reducer) == expected_sources);
        const auto expected_lp = independent_lp_xor(reducer.lp_keys(edge.endpoints[0]),
                                                    reducer.lp_keys(edge.endpoints[1]));
        CHECK(edge.expected_lp_keys == expected_lp);
        CHECK(std::find(edge.expected_lp_keys.begin(), edge.expected_lp_keys.end(), plan.pivot) ==
              edge.expected_lp_keys.end());
        output_lp_nnz += edge.expected_lp_keys.size();
    }

    CHECK(plan.input_nonpivot_lp_nnz == input_nonpivot_lp_nnz);
    CHECK(plan.output_lp_nnz == output_lp_nnz);
    CHECK(plan.lp_fill_growth ==
          (output_lp_nnz > input_nonpivot_lp_nnz ? output_lp_nnz - input_nonpivot_lp_nnz : 0));
}

[[nodiscard]] std::vector<RowPair> star_edges(size_t weight, uint64_t first_row = 0) {
    std::vector<RowPair> result;
    for (size_t index = 1; index < weight; ++index) {
        result.emplace_back(first_row, first_row + index);
    }
    return result;
}

void check_contiguous_ids(std::span<const StructuredRowId> ids, uint64_t first) {
    for (size_t index = 0; index < ids.size(); ++index) {
        CHECK(ids[index].value == first + index);
    }
}

void test_weight_three_through_eight() {
    const auto pivot = rational_key(1009);
    const GoldenUniverse universe{{pivot}};

    for (size_t weight = 3; weight <= 8; ++weight) {
        std::vector<Relation> relations;
        std::vector<Mask> golden_rows;
        for (size_t index = 0; index < weight; ++index) {
            Relation relation(0, 16);
            relation.rational_factors.push_back(static_cast<uint32_t>(index));
            add_large_prime(relation, pivot);
            relations.push_back(std::move(relation));
            golden_rows.push_back(lp_bit(universe, pivot) ^ factor_bit(index));
        }
        relations.push_back(make_relation({}, {0, 1}));
        golden_rows.push_back(factor_bit(0) ^ factor_bit(1));

        SequentialStructuredReducer reducer(100 + weight, std::move(relations));
        const ReducerSnapshot before = snapshot_state(reducer);
        const auto plans = reducer.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
        const auto* found = find_plan(plans, pivot);
        CHECK(found != nullptr);
        if (found == nullptr) {
            continue;
        }
        const TreeBasisMergePlan plan = *found;
        CHECK(snapshot_state(reducer) == before);
        CHECK(plan.generation == 100 + weight);
        CHECK(plan.planner == TreeBasisPlanner::DeterministicMst);
        CHECK(plan.members.size() == weight);
        CHECK(plan.edges.size() == weight - 1);
        CHECK(edge_pairs(plan) == star_edges(weight));
        for (size_t index = 0; index < weight; ++index) {
            CHECK(plan.members[index] == StructuredRowId{index});
        }

        check_independent_tree_rank(plan);
        check_plan_payloads(reducer, plan);

        PreparedTreeBasisMerge prepared = reducer.prepare(plan);
        auto stale_prepared = reducer.prepare(plan);
        CHECK(snapshot_state(reducer) == before);
        CHECK(prepared.plan() == plan);
        const auto materialized = prepared.materialized_relations();
        CHECK(materialized.size() == plan.edges.size());
        for (size_t index = 0; index < materialized.size(); ++index) {
            const auto& edge = plan.edges[index];
            const Mask expected =
                golden_rows[edge.endpoints[0].value] ^ golden_rows[edge.endpoints[1].value];
            CHECK(decode_relation(materialized[index], universe) == expected);
            const auto support = exact_lp_support(materialized[index]);
            CHECK(std::find(support.begin(), support.end(), pivot) == support.end());
        }

        const uint64_t first_output = reducer.total_row_count();
        const auto outputs = reducer.commit(std::move(prepared));
        CHECK(outputs.size() == weight - 1);
        check_contiguous_ids(outputs, first_output);
        for (size_t index = 0; index < outputs.size(); ++index) {
            CHECK(reducer.is_active(outputs[index]));
            CHECK(source_mask(reducer.sources(outputs[index]), reducer) ==
                  source_mask(plan.edges[index].expected_sources, reducer));
            CHECK(std::find(reducer.lp_keys(outputs[index]).begin(),
                            reducer.lp_keys(outputs[index]).end(),
                            pivot) == reducer.lp_keys(outputs[index]).end());
        }
        for (const auto member : plan.members) {
            CHECK(!reducer.is_active(member));
        }
        CHECK(reducer.active_row_count() == weight);
        CHECK(reducer.stats().tree_basis_batches == 1);
        CHECK(reducer.stats().tree_basis_rows_consumed == weight);
        CHECK(reducer.stats().tree_basis_rows_emitted == weight - 1);
        CHECK(reducer.stats().two_way_merges == 0);
        CHECK(reducer.stats().output_rows == weight);
        check_exact_dependency_bijection(reducer, golden_rows, universe);

        const ReducerSnapshot committed = snapshot_state(reducer);
        check_error(StructuredReductionErrorCode::StalePlan,
                    [&] { (void)reducer.commit(std::move(stale_prepared)); });
        CHECK(snapshot_state(reducer) == committed);
        check_error(StructuredReductionErrorCode::StalePlan, [&] { (void)reducer.prepare(plan); });
        CHECK(snapshot_state(reducer) == committed);
    }
}

struct PlannedReducer final {
    SequentialStructuredReducer reducer;
    TreeBasisMergePlan plan;
};

[[nodiscard]] PlannedReducer make_incidental_reducer(TreeBasisPlanner planner) {
    const auto pivot = rational_key(2003);
    const auto a = rational_key(2011);
    const auto b = rational_key(2017);
    const auto c = rational_key(2027);
    const auto d = rational_key(2029);
    SequentialStructuredReducer reducer(planner == TreeBasisPlanner::DeterministicMst ? 201 : 202,
                                        {make_relation({pivot, a, b}), make_relation({pivot, a, c}),
                                         make_relation({pivot, b, d}),
                                         make_relation({pivot, c, d})});
    const auto plans = reducer.plan_tree_basis_merges(planner);
    const auto* found = find_plan(plans, pivot);
    oracle_require(found != nullptr, "incidental-cancellation pivot was not planned");
    return PlannedReducer{std::move(reducer), *found};
}

void test_incidental_cancellation_and_reference_star() {
    const auto pivot = rational_key(2003);
    const auto a = rational_key(2011);
    const auto b = rational_key(2017);
    const auto c = rational_key(2027);
    const auto d = rational_key(2029);
    const GoldenUniverse universe{{pivot, a, b, c, d}};
    const std::vector<Mask> golden_rows{
        lp_bit(universe, pivot) ^ lp_bit(universe, a) ^ lp_bit(universe, b),
        lp_bit(universe, pivot) ^ lp_bit(universe, a) ^ lp_bit(universe, c),
        lp_bit(universe, pivot) ^ lp_bit(universe, b) ^ lp_bit(universe, d),
        lp_bit(universe, pivot) ^ lp_bit(universe, c) ^ lp_bit(universe, d),
    };

    auto mst = make_incidental_reducer(TreeBasisPlanner::DeterministicMst);
    auto star = make_incidental_reducer(TreeBasisPlanner::ReferenceStar);
    const std::vector<RowPair> expected_mst{{0, 1}, {0, 2}, {1, 3}};
    const std::vector<RowPair> expected_star{{0, 1}, {0, 2}, {0, 3}};
    CHECK(sorted_edge_pairs(mst.plan) == expected_mst);
    CHECK(sorted_edge_pairs(star.plan) == expected_star);
    CHECK(sorted_edge_pairs(mst.plan) != sorted_edge_pairs(star.plan));
    CHECK(mst.plan.output_lp_nnz == 6);
    CHECK(star.plan.output_lp_nnz == 8);
    CHECK(mst.plan.output_lp_nnz < star.plan.output_lp_nnz);
    CHECK(mst.plan.input_nonpivot_lp_nnz == 8);
    CHECK(star.plan.input_nonpivot_lp_nnz == 8);

    check_independent_tree_rank(mst.plan);
    check_independent_tree_rank(star.plan);
    check_plan_payloads(mst.reducer, mst.plan);
    check_plan_payloads(star.reducer, star.plan);
    (void)mst.reducer.commit(mst.reducer.prepare(mst.plan));
    (void)star.reducer.commit(star.reducer.prepare(star.plan));
    CHECK(exact_left_kernel(golden_rows) == std::vector<Mask>({0, Mask{0b1111}}));
    check_exact_dependency_bijection(mst.reducer, golden_rows, universe);
    check_exact_dependency_bijection(star.reducer, golden_rows, universe);
}

void test_nested_tree_merges_allow_source_overlap() {
    const auto p = rational_key(3001);
    const auto q = rational_key(3011);
    const GoldenUniverse universe{{p, q}};
    const std::vector<Mask> golden_rows{
        lp_bit(universe, p),
        lp_bit(universe, p) ^ lp_bit(universe, q),
        lp_bit(universe, p) ^ lp_bit(universe, q),
        lp_bit(universe, p) ^ lp_bit(universe, q),
    };
    SequentialStructuredReducer reducer(301, {make_relation({p}), make_relation({p, q}),
                                              make_relation({p, q}), make_relation({p, q})});

    const auto initial = reducer.plan_tree_basis_merges(TreeBasisPlanner::ReferenceStar);
    const auto* p_found = find_plan(initial, p);
    const auto* q_found = find_plan(initial, q);
    CHECK(p_found != nullptr);
    CHECK(q_found != nullptr);
    if (p_found == nullptr || q_found == nullptr) {
        return;
    }
    const TreeBasisMergePlan p_plan = *p_found;
    const TreeBasisMergePlan stale_q = *q_found;
    CHECK(edge_pairs(p_plan) == star_edges(4));
    const auto first_outputs = reducer.commit(reducer.prepare(p_plan));
    CHECK(first_outputs.size() == 3);
    check_contiguous_ids(first_outputs, 4);
    const std::vector<Mask> expected_first_sources{Mask{0b0011}, Mask{0b0101}, Mask{0b1001}};
    for (size_t index = 0; index < first_outputs.size(); ++index) {
        CHECK(source_mask(reducer.sources(first_outputs[index]), reducer) ==
              expected_first_sources[index]);
        const auto keys = reducer.lp_keys(first_outputs[index]);
        CHECK(keys.size() == 1);
        if (keys.size() == 1) {
            CHECK(keys.front() == q);
        }
    }
    CHECK((expected_first_sources[0] & expected_first_sources[1]) != 0);

    const ReducerSnapshot after_first = snapshot_state(reducer);
    check_error(StructuredReductionErrorCode::StalePlan, [&] { (void)reducer.prepare(stale_q); });
    CHECK(snapshot_state(reducer) == after_first);

    const auto second_plans = reducer.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
    const auto* second_found = find_plan(second_plans, q);
    CHECK(second_found != nullptr);
    if (second_found == nullptr) {
        return;
    }
    const TreeBasisMergePlan second = *second_found;
    CHECK(edge_pairs(second) == star_edges(3, 4));
    check_independent_tree_rank(second);
    check_plan_payloads(reducer, second);
    const auto final_outputs = reducer.commit(reducer.prepare(second));
    CHECK(final_outputs.size() == 2);
    check_contiguous_ids(final_outputs, 7);
    CHECK(source_mask(reducer.sources(final_outputs[0]), reducer) == Mask{0b0110});
    CHECK(source_mask(reducer.sources(final_outputs[1]), reducer) == Mask{0b1010});
    CHECK((source_mask(reducer.sources(final_outputs[0]), reducer) &
           source_mask(reducer.sources(final_outputs[1]), reducer)) != 0);

    CHECK(reducer.total_row_count() == 9);
    CHECK(reducer.active_row_count() == 2);
    CHECK(reducer.stats().tree_basis_batches == 2);
    CHECK(reducer.stats().tree_basis_rows_consumed == 7);
    CHECK(reducer.stats().tree_basis_rows_emitted == 5);
    CHECK(reducer.stats().output_rows == 2);
    check_exact_dependency_bijection(reducer, golden_rows, universe);
}

void test_rank_validation_survives_elimination_and_row_reallocation() {
    const auto p = rational_key(3251);
    const auto a = rational_key(3253);
    const auto b = rational_key(3257);
    const GoldenUniverse universe{{p, a, b}};
    const std::vector<Mask> golden_rows{
        lp_bit(universe, p) ^ lp_bit(universe, a),
        lp_bit(universe, p) ^ lp_bit(universe, b),
        lp_bit(universe, p) ^ lp_bit(universe, a) ^ lp_bit(universe, b),
    };
    SequentialStructuredReducer reducer(
        325, {make_relation({p, a}), make_relation({p, b}), make_relation({p, a, b})});

    const auto plans = reducer.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
    const auto* found = find_plan(plans, p);
    CHECK(found != nullptr);
    if (found == nullptr) {
        return;
    }
    const std::vector<RowPair> expected_edges{{0, 2}, {1, 2}};
    CHECK(edge_pairs(*found) == expected_edges);
    check_independent_tree_rank(*found);
    check_plan_payloads(reducer, *found);

    const auto outputs = reducer.commit(reducer.prepare(*found));
    CHECK(outputs == std::vector<StructuredRowId>({StructuredRowId{3}, StructuredRowId{4}}));
    if (outputs.size() != 2) {
        return;
    }
    CHECK(source_mask(reducer.sources(outputs[0]), reducer) == Mask{0b101});
    CHECK(source_mask(reducer.sources(outputs[1]), reducer) == Mask{0b110});
    const auto first_lp_keys = reducer.lp_keys(outputs[0]);
    const auto second_lp_keys = reducer.lp_keys(outputs[1]);
    CHECK(first_lp_keys.size() == 1);
    CHECK(second_lp_keys.size() == 1);
    if (first_lp_keys.size() == 1) {
        CHECK(first_lp_keys.front() == b);
    }
    if (second_lp_keys.size() == 1) {
        CHECK(second_lp_keys.front() == a);
    }

    // Both active source transforms have pivot 2, so full-rank validation must
    // eliminate the second against the first. Planning again after commit also
    // validates after rows grew from three to five entries and may have moved.
    CHECK(reducer.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst).empty());
    CHECK(reducer.plan_two_way_merges().empty());
    check_exact_dependency_bijection(reducer, golden_rows, universe);
}

void test_rank_validation_retains_eliminated_basis_rows() {
    const auto p = rational_key(3301);
    const auto a = rational_key(3307);
    const auto b = rational_key(3313);
    const auto c = rational_key(3319);
    const auto d = rational_key(3323);
    const GoldenUniverse universe{{p, a, b, c, d}};
    const std::vector<Mask> golden_rows{
        lp_bit(universe, p),
        lp_bit(universe, p) ^ lp_bit(universe, a) ^ lp_bit(universe, b) ^ lp_bit(universe, c) ^
            lp_bit(universe, d),
        lp_bit(universe, p) ^ lp_bit(universe, a) ^ lp_bit(universe, b),
        lp_bit(universe, p) ^ lp_bit(universe, a) ^ lp_bit(universe, c),
        lp_bit(universe, p) ^ lp_bit(universe, a),
    };
    SequentialStructuredReducer reducer(331, {make_relation({p}), make_relation({p, a, b, c, d}),
                                              make_relation({p, a, b}), make_relation({p, a, c}),
                                              make_relation({p, a})});

    const auto plans = reducer.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
    const auto* found = find_plan(plans, p);
    CHECK(found != nullptr);
    if (found == nullptr) {
        return;
    }
    const std::vector<RowPair> expected_edges{{0, 4}, {2, 4}, {3, 4}, {1, 2}};
    CHECK(edge_pairs(*found) == expected_edges);
    check_independent_tree_rank(*found);
    check_plan_payloads(reducer, *found);

    const auto outputs = reducer.commit(reducer.prepare(*found));
    CHECK(outputs == std::vector<StructuredRowId>({StructuredRowId{5}, StructuredRowId{6},
                                                   StructuredRowId{7}, StructuredRowId{8}}));
    if (outputs.size() != 4) {
        return;
    }
    const std::vector<Mask> expected_sources{Mask{0b10001}, Mask{0b10100}, Mask{0b11000},
                                             Mask{0b00110}};
    for (size_t index = 0; index < outputs.size(); ++index) {
        CHECK(source_mask(reducer.sources(outputs[index]), reducer) == expected_sources[index]);
    }

    // Validation processes these transforms in output order. The second and
    // third each collide with {0,4} and create distinct owned basis rows; the
    // fourth then collides with the first owned row after another append.
    (void)reducer.plan_two_way_merges();
    check_exact_dependency_bijection(reducer, golden_rows, universe);
}

void test_mst_prefers_sparse_source_xor_after_lp_tie() {
    const auto p = rational_key(3527);
    const auto q = rational_key(3533);
    SequentialStructuredReducer reducer(351, {make_relation({p}), make_relation({p, q}),
                                              make_relation({p, q}), make_relation({q})});

    const auto first_plans = reducer.plan_tree_basis_merges(TreeBasisPlanner::ReferenceStar);
    const auto* p_found = find_plan(first_plans, p);
    CHECK(p_found != nullptr);
    if (p_found == nullptr) {
        return;
    }
    const auto first_outputs = reducer.commit(reducer.prepare(*p_found));
    CHECK(first_outputs == std::vector<StructuredRowId>({StructuredRowId{4}, StructuredRowId{5}}));

    const auto second_plans = reducer.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
    const auto* q_found = find_plan(second_plans, q);
    CHECK(q_found != nullptr);
    if (q_found == nullptr) {
        return;
    }
    CHECK(q_found->members == std::vector<StructuredRowId>(
                                  {StructuredRowId{3}, StructuredRowId{4}, StructuredRowId{5}}));
    CHECK(q_found->edges.size() == 2);
    if (q_found->edges.size() != 2) {
        return;
    }

    // Every candidate cancels q and has zero LP cost. The 4-5 edge wins
    // before the lower RowId edges because its overlapping source transforms
    // XOR to two atoms instead of three.
    CHECK(canonical_pair(q_found->edges[0].endpoints[0], q_found->edges[0].endpoints[1]) ==
          RowPair(4, 5));
    CHECK(q_found->edges[0].expected_lp_keys.empty());
    CHECK(q_found->edges[0].expected_sources.size() == 2);
    CHECK(canonical_pair(q_found->edges[1].endpoints[0], q_found->edges[1].endpoints[1]) ==
          RowPair(3, 4));
    CHECK(q_found->edges[1].expected_lp_keys.empty());
    CHECK(q_found->edges[1].expected_sources.size() == 3);
    check_independent_tree_rank(*q_found);
}

[[nodiscard]] TreeBasisEdgePlan atomic_edge(const SequentialStructuredReducer& reducer,
                                            uint64_t lhs, uint64_t rhs) {
    const auto lhs_id = reducer.corpus().source_id(lhs);
    const auto rhs_id = reducer.corpus().source_id(rhs);
    TreeBasisEdgePlan edge;
    edge.endpoints = {StructuredRowId{std::min(lhs, rhs)}, StructuredRowId{std::max(lhs, rhs)}};
    edge.expected_sources =
        SourceCombination::canonical(reducer.corpus().generation(), std::vector{lhs_id, rhs_id});
    edge.expected_lp_keys =
        independent_lp_xor(reducer.lp_keys(edge.endpoints[0]), reducer.lp_keys(edge.endpoints[1]));
    return edge;
}

void check_invalid_plan_without_mutation(SequentialStructuredReducer& reducer,
                                         const TreeBasisMergePlan& plan,
                                         StructuredReductionErrorCode code) {
    const ReducerSnapshot before = snapshot_state(reducer);
    check_error(code, [&] { (void)reducer.prepare(plan); });
    CHECK(snapshot_state(reducer) == before);
}

void test_forged_tree_plans_fail_closed() {
    const auto pivot = rational_key(4001);
    SequentialStructuredReducer reducer(401,
                                        {make_relation({pivot}, {0}), make_relation({pivot}, {1}),
                                         make_relation({pivot}, {2}), make_relation({pivot}, {3})});
    const auto plans = reducer.plan_tree_basis_merges();
    const auto* found = find_plan(plans, pivot);
    CHECK(found != nullptr);
    if (found == nullptr) {
        return;
    }
    const TreeBasisMergePlan valid = *found;

    TreeBasisMergePlan cycle = valid;
    cycle.edges = {atomic_edge(reducer, 0, 1), atomic_edge(reducer, 1, 2),
                   atomic_edge(reducer, 0, 2)};
    cycle.output_lp_nnz = 0;
    cycle.lp_fill_growth = 0;
    check_invalid_plan_without_mutation(reducer, cycle, StructuredReductionErrorCode::InvalidPlan);

    TreeBasisMergePlan disconnected = valid;
    disconnected.edges = {atomic_edge(reducer, 0, 1), atomic_edge(reducer, 2, 3)};
    disconnected.output_lp_nnz = 0;
    disconnected.lp_fill_growth = 0;
    check_invalid_plan_without_mutation(reducer, disconnected,
                                        StructuredReductionErrorCode::InvalidPlan);

    TreeBasisMergePlan fake_sources = valid;
    fake_sources.edges.front().expected_sources =
        SourceCombination::singleton(reducer.corpus().source_id(0));
    check_invalid_plan_without_mutation(reducer, fake_sources,
                                        StructuredReductionErrorCode::InvalidPlan);

    TreeBasisMergePlan fake_lp = valid;
    fake_lp.edges.front().expected_lp_keys.push_back(rational_key(4999));
    fake_lp.output_lp_nnz += 1;
    fake_lp.lp_fill_growth = fake_lp.output_lp_nnz > fake_lp.input_nonpivot_lp_nnz
                                 ? fake_lp.output_lp_nnz - fake_lp.input_nonpivot_lp_nnz
                                 : 0;
    check_invalid_plan_without_mutation(reducer, fake_lp,
                                        StructuredReductionErrorCode::InvalidPlan);
}

void test_weight_above_eight_is_rejected() {
    const auto oversized = rational_key(5003);
    const auto legal = rational_key(5009);
    std::vector<Relation> relations;
    for (size_t index = 0; index < 9; ++index) {
        relations.push_back(make_relation({oversized}, {static_cast<uint32_t>(index)}));
    }
    relations.push_back(make_relation({legal}, {9}));
    relations.push_back(make_relation({legal}, {10}));
    relations.push_back(make_relation({legal}, {11}));
    SequentialStructuredReducer reducer(501, std::move(relations));

    const ReducerSnapshot before = snapshot_state(reducer);
    const auto plans = reducer.plan_tree_basis_merges();
    CHECK(find_plan(plans, oversized) == nullptr);
    const auto* legal_found = find_plan(plans, legal);
    CHECK(legal_found != nullptr);
    if (legal_found == nullptr) {
        return;
    }

    TreeBasisMergePlan forged = *legal_found;
    forged.pivot = oversized;
    forged.members.clear();
    forged.edges.clear();
    for (uint64_t index = 0; index < 9; ++index) {
        forged.members.push_back(StructuredRowId{index});
    }
    for (uint64_t index = 1; index < 9; ++index) {
        forged.edges.push_back(atomic_edge(reducer, 0, index));
    }
    forged.input_nonpivot_lp_nnz = 0;
    forged.output_lp_nnz = 0;
    forged.lp_fill_growth = 0;
    check_invalid_plan_without_mutation(reducer, forged, StructuredReductionErrorCode::InvalidPlan);
    CHECK(snapshot_state(reducer) == before);
}

} // namespace

int main() {
    try {
        test_weight_three_through_eight();
        test_incidental_cancellation_and_reference_star();
        test_nested_tree_merges_allow_source_overlap();
        test_rank_validation_survives_elimination_and_row_reallocation();
        test_rank_validation_retains_eliminated_basis_rows();
        test_mst_prefers_sparse_source_xor_after_lp_tie();
        test_forged_tree_plans_fail_closed();
        test_weight_above_eight_is_rejected();
    } catch (const std::exception& error) {
        std::cerr << "fatal tree-basis oracle error: " << error.what() << '\n';
        return 1;
    }

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " structured tree-basis checks failed\n";
        return 1;
    }
    std::cout << "All " << checks << " structured tree-basis checks passed\n";
    return 0;
}
