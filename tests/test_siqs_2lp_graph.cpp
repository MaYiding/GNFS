// test_siqs_2lp_graph.cpp - SIQS large-prime graph cycle-basis contracts

#include <gnfs/siqs/two_large_prime_graph.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using gnfs::siqs::build_two_large_prime_cycle_basis;
using gnfs::siqs::TwoLargePrimeCycleBasis;
using gnfs::siqs::TwoLargePrimeCycleBasisLimits;
using gnfs::siqs::TwoLargePrimeCycleBasisResult;
using gnfs::siqs::TwoLargePrimeCycleBasisStatus;
using gnfs::siqs::TwoLargePrimeEdge;

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

class ReferenceDisjointSet {
public:
    explicit ReferenceDisjointSet(size_t size)
        : parent_(size) {
        for (size_t i = 0; i < size; ++i) {
            parent_[i] = i;
        }
    }

    [[nodiscard]] size_t find(size_t vertex) {
        while (parent_[vertex] != vertex) {
            parent_[vertex] = parent_[parent_[vertex]];
            vertex = parent_[vertex];
        }
        return vertex;
    }

    [[nodiscard]] bool unite(size_t lhs, size_t rhs) {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs == rhs) {
            return false;
        }
        parent_[rhs] = lhs;
        return true;
    }

private:
    std::vector<size_t> parent_;
};

struct ReferenceGraphStats {
    size_t vertex_count;
    size_t component_count;
    size_t cycle_rank;
};

[[nodiscard]] ReferenceGraphStats reference_graph_stats(
        const std::vector<TwoLargePrimeEdge>& edges) {
    std::vector<uint64_t> vertices;
    vertices.reserve(edges.size() * 2);
    for (const auto& edge : edges) {
        vertices.push_back(edge.p);
        vertices.push_back(edge.q);
    }
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());

    ReferenceDisjointSet components(vertices.size());
    size_t component_count = vertices.size();
    for (const auto& edge : edges) {
        const auto p = std::lower_bound(vertices.begin(), vertices.end(), edge.p);
        const auto q = std::lower_bound(vertices.begin(), vertices.end(), edge.q);
        const size_t p_index = static_cast<size_t>(p - vertices.begin());
        const size_t q_index = static_cast<size_t>(q - vertices.begin());
        if (components.unite(p_index, q_index)) {
            --component_count;
        }
    }

    return ReferenceGraphStats{
        vertices.size(),
        component_count,
        edges.size() - vertices.size() + component_count,
    };
}

[[nodiscard]] size_t gf2_rank(
        const std::vector<std::vector<size_t>>& cycles,
        const std::vector<TwoLargePrimeEdge>& edges) {
    std::vector<size_t> relation_indices;
    relation_indices.reserve(edges.size());
    for (const auto& edge : edges) {
        relation_indices.push_back(edge.relation_index);
    }
    std::sort(relation_indices.begin(), relation_indices.end());

    const size_t word_count = (relation_indices.size() + 63) / 64;
    std::vector<std::vector<uint64_t>> rows(
        cycles.size(), std::vector<uint64_t>(word_count, 0));
    for (size_t row = 0; row < cycles.size(); ++row) {
        for (const size_t relation_index : cycles[row]) {
            const auto position = std::lower_bound(
                relation_indices.begin(), relation_indices.end(), relation_index);
            if (position == relation_indices.end() || *position != relation_index) {
                continue;
            }
            const size_t column =
                static_cast<size_t>(position - relation_indices.begin());
            rows[row][column / 64] ^= UINT64_C(1) << (column % 64);
        }
    }

    size_t rank = 0;
    for (size_t column = 0;
         column < relation_indices.size() && rank < rows.size();
         ++column) {
        size_t pivot = rank;
        while (pivot < rows.size() &&
               (rows[pivot][column / 64] &
                (UINT64_C(1) << (column % 64))) == 0) {
            ++pivot;
        }
        if (pivot == rows.size()) {
            continue;
        }

        std::swap(rows[rank], rows[pivot]);
        for (size_t row = rank + 1; row < rows.size(); ++row) {
            if ((rows[row][column / 64] &
                 (UINT64_C(1) << (column % 64))) == 0) {
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

void check_cycle_invariants(const std::vector<TwoLargePrimeEdge>& edges,
                            const TwoLargePrimeCycleBasis& basis) {
    const ReferenceGraphStats stats = reference_graph_stats(edges);
    CHECK(basis.vertex_count == stats.vertex_count);
    CHECK(basis.edge_count == edges.size());
    CHECK(basis.component_count == stats.component_count);
    CHECK(basis.cycles.size() == stats.cycle_rank);

    std::unordered_map<size_t, const TwoLargePrimeEdge*> edge_by_relation;
    edge_by_relation.reserve(edges.size());
    for (const auto& edge : edges) {
        edge_by_relation.emplace(edge.relation_index, &edge);
    }

    size_t total_cycle_incidences = 0;
    size_t max_cycle_length = 0;
    for (const auto& cycle : basis.cycles) {
        CHECK(!cycle.empty());
        CHECK(std::is_sorted(cycle.begin(), cycle.end()));
        CHECK(cycle.size() <= std::numeric_limits<size_t>::max() - total_cycle_incidences);
        total_cycle_incidences += cycle.size();
        max_cycle_length = std::max(max_cycle_length, cycle.size());

        std::unordered_set<size_t> unique_relations;
        std::unordered_map<uint64_t, size_t> degree;
        for (const size_t relation_index : cycle) {
            CHECK(unique_relations.insert(relation_index).second);

            const auto edge = edge_by_relation.find(relation_index);
            CHECK(edge != edge_by_relation.end());
            if (edge == edge_by_relation.end()) {
                continue;
            }
            ++degree[edge->second->p];
            ++degree[edge->second->q];
        }
        for (const auto& [vertex, vertex_degree] : degree) {
            (void)vertex;
            CHECK(vertex_degree % 2 == 0);
        }
    }

    CHECK(basis.total_cycle_incidences == total_cycle_incidences);
    CHECK(basis.max_cycle_length == max_cycle_length);
    CHECK((basis.cycles.empty() && basis.total_cycle_incidences == 0 &&
           basis.max_cycle_length == 0) ||
          (!basis.cycles.empty() && basis.max_cycle_length > 0 &&
           basis.max_cycle_length <= basis.total_cycle_incidences));
    CHECK(gf2_rank(basis.cycles, edges) == basis.cycles.size());
}

[[nodiscard]] constexpr TwoLargePrimeCycleBasisLimits unlimited_limits() noexcept {
    return {
        std::numeric_limits<size_t>::max(),
        std::numeric_limits<size_t>::max(),
        std::numeric_limits<size_t>::max(),
    };
}

void check_typed_status(const std::vector<TwoLargePrimeEdge>& edges,
                        const TwoLargePrimeCycleBasisLimits& limits,
                        TwoLargePrimeCycleBasisStatus expected_status) {
    const auto result = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(edges.data(), edges.size()), limits);
    CHECK(result.status() == expected_status);
    CHECK(result.is_valid() == (expected_status == TwoLargePrimeCycleBasisStatus::valid));
    CHECK(result.basis().has_value() == (expected_status == TwoLargePrimeCycleBasisStatus::valid));
    if (result.basis()) {
        check_cycle_invariants(edges, *result.basis());
    }
}

void check_result_state(const TwoLargePrimeCycleBasisResult& result,
                        TwoLargePrimeCycleBasisStatus expected_status) {
    CHECK(result.status() == expected_status);
    CHECK(result.is_valid() == (expected_status == TwoLargePrimeCycleBasisStatus::valid));
    CHECK(result.basis().has_value() == (expected_status == TwoLargePrimeCycleBasisStatus::valid));
}

template <class Result> void self_copy_assign(Result& result) {
    const Result* alias = &result;
    result = *alias;
}

template <class Result> void self_move_assign(Result& result) {
    Result* alias = &result;
    result = std::move(*alias);
}

[[nodiscard]] std::optional<TwoLargePrimeCycleBasis> build_checked(
        const std::vector<TwoLargePrimeEdge>& edges) {
    const auto result = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(edges.data(), edges.size()));
    CHECK(result.has_value());
    if (result) {
        check_cycle_invariants(edges, *result);
    }
    return result;
}

void check_expected(const TwoLargePrimeCycleBasis& basis,
                    size_t vertex_count,
                    size_t edge_count,
                    size_t component_count,
                    const std::vector<std::vector<size_t>>& cycles) {
    CHECK(basis.vertex_count == vertex_count);
    CHECK(basis.edge_count == edge_count);
    CHECK(basis.component_count == component_count);
    CHECK(basis.cycles == cycles);
}

[[nodiscard]] bool same_basis(const TwoLargePrimeCycleBasis& lhs,
                              const TwoLargePrimeCycleBasis& rhs) {
    return lhs.vertex_count == rhs.vertex_count && lhs.edge_count == rhs.edge_count &&
           lhs.component_count == rhs.component_count &&
           lhs.total_cycle_incidences == rhs.total_cycle_incidences &&
           lhs.max_cycle_length == rhs.max_cycle_length && lhs.cycles == rhs.cycles;
}

void test_empty_graph_and_tree() {
    {
        const std::vector<TwoLargePrimeEdge> edges;
        const auto basis = build_checked(edges);
        if (basis) {
            check_expected(*basis, 0, 0, 0, {});
        }
    }

    {
        const std::vector<TwoLargePrimeEdge> edges{
            {0, 101, 0},
            {101, 103, 1},
            {103, 107, 2},
        };
        const auto basis = build_checked(edges);
        if (basis) {
            check_expected(*basis, 4, 3, 1, {});
        }
    }
}

void test_parallel_edges_and_self_loop() {
    {
        const std::vector<TwoLargePrimeEdge> edges{
            {0, 101, 10},
            {0, 101, 11},
        };
        const std::vector<std::vector<size_t>> expected{{10, 11}};
        const auto basis = build_checked(edges);
        if (basis) {
            check_expected(*basis, 2, 2, 1, expected);
        }
    }

    {
        const std::vector<TwoLargePrimeEdge> edges{
            {101, 103, 20},
            {103, 101, 21},
        };
        const std::vector<std::vector<size_t>> expected{{20, 21}};
        const auto basis = build_checked(edges);
        if (basis) {
            check_expected(*basis, 2, 2, 1, expected);
        }
    }

    {
        const std::vector<TwoLargePrimeEdge> edges{{107, 107, 30}};
        const std::vector<std::vector<size_t>> expected{{30}};
        const auto basis = build_checked(edges);
        if (basis) {
            check_expected(*basis, 1, 1, 1, expected);
        }
    }
}

void test_triangle_and_mixed_one_lp_cycle() {
    {
        const std::vector<TwoLargePrimeEdge> edges{
            {101, 103, 40},
            {103, 107, 41},
            {107, 101, 42},
        };
        const std::vector<std::vector<size_t>> expected{{40, 41, 42}};
        const auto basis = build_checked(edges);
        if (basis) {
            check_expected(*basis, 3, 3, 1, expected);
        }
    }

    {
        const std::vector<TwoLargePrimeEdge> edges{
            {0, 109, 50},
            {109, 113, 51},
            {0, 113, 52},
        };
        const std::vector<std::vector<size_t>> expected{{50, 51, 52}};
        const auto basis = build_checked(edges);
        if (basis) {
            check_expected(*basis, 3, 3, 1, expected);
        }
    }
}

void test_disconnected_components_and_cycle_rank() {
    const std::vector<TwoLargePrimeEdge> edges{
        {101, 103, 100},
        {103, 107, 101},
        {107, 101, 102},
        {109, 113, 110},
        {113, 109, 111},
        {127, 131, 120},
        {137, 137, 130},
    };
    const std::vector<std::vector<size_t>> expected{
        {100, 101, 102},
        {110, 111},
        {130},
    };
    const auto basis = build_checked(edges);
    if (basis) {
        check_expected(*basis, 8, 7, 4, expected);
        CHECK(basis->cycles.size() ==
              basis->edge_count - basis->vertex_count + basis->component_count);
    }
}

void test_input_order_and_endpoint_direction_are_irrelevant() {
    const std::vector<TwoLargePrimeEdge> edges{
        {103, 107, 409},
        {0, 101, 405},
        {101, 107, 407},
        {107, 0, 403},
        {101, 103, 401},
    };
    const auto baseline = build_checked(edges);

    auto reordered = edges;
    std::reverse(reordered.begin(), reordered.end());
    for (auto& edge : reordered) {
        std::swap(edge.p, edge.q);
    }
    const auto reversed = build_checked(reordered);
    const auto repeated = build_checked(edges);

    if (baseline && reversed && repeated) {
        CHECK(same_basis(*baseline, *reversed));
        CHECK(same_basis(*baseline, *repeated));
    }

    constexpr TwoLargePrimeCycleBasisLimits exact_limits{5, 2, 7};
    const auto typed_baseline = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(edges.data(), edges.size()), exact_limits);
    const auto typed_reversed = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(reordered.data(), reordered.size()), exact_limits);
    CHECK(typed_baseline.status() == typed_reversed.status());
    CHECK(typed_baseline.is_valid() && typed_reversed.is_valid());
    if (typed_baseline.basis() && typed_reversed.basis()) {
        CHECK(same_basis(*typed_baseline.basis(), *typed_reversed.basis()));
    }

    constexpr TwoLargePrimeCycleBasisLimits incidence_short{5, 2, 6};
    const auto limited_baseline = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(edges.data(), edges.size()), incidence_short);
    const auto limited_reversed = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(reordered.data(), reordered.size()), incidence_short);
    CHECK(limited_baseline.status() == TwoLargePrimeCycleBasisStatus::incidence_limit);
    CHECK(limited_reversed.status() == limited_baseline.status());
    CHECK(!limited_baseline.basis().has_value());
    CHECK(!limited_reversed.basis().has_value());
}

void test_exact_and_one_over_limits() {
    const std::vector<TwoLargePrimeEdge> edges{
        {101, 103, 10},
        {103, 107, 11},
        {107, 101, 12},
        {109, 109, 13},
    };
    constexpr TwoLargePrimeCycleBasisLimits exact{4, 2, 4};
    const auto exact_result = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(edges.data(), edges.size()), exact);
    CHECK(exact_result.status() == TwoLargePrimeCycleBasisStatus::valid);
    CHECK(exact_result.is_valid());
    CHECK(exact_result.basis().has_value());
    if (exact_result.basis()) {
        check_cycle_invariants(edges, *exact_result.basis());
        CHECK(exact_result.basis()->cycles.size() == exact.max_cycles);
        CHECK(exact_result.basis()->total_cycle_incidences == exact.max_cycle_incidences);
        CHECK(exact_result.basis()->max_cycle_length == 3);
    }

    check_typed_status(edges, {3, 2, 4}, TwoLargePrimeCycleBasisStatus::edge_limit);
    check_typed_status(edges, {4, 1, 4}, TwoLargePrimeCycleBasisStatus::cycle_limit);
    check_typed_status(edges, {4, 2, 3}, TwoLargePrimeCycleBasisStatus::incidence_limit);
}

void test_zero_limits() {
    const std::vector<TwoLargePrimeEdge> empty;
    const auto empty_result = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(empty.data(), empty.size()), {0, 0, 0});
    CHECK(empty_result.status() == TwoLargePrimeCycleBasisStatus::valid);
    CHECK(empty_result.is_valid());
    CHECK(empty_result.basis().has_value());
    if (empty_result.basis()) {
        check_cycle_invariants(empty, *empty_result.basis());
        check_expected(*empty_result.basis(), 0, 0, 0, {});
    }

    const std::vector<TwoLargePrimeEdge> tree{
        {0, 101, 20},
        {101, 103, 21},
        {103, 107, 22},
    };
    check_typed_status(tree, {3, 0, 0}, TwoLargePrimeCycleBasisStatus::valid);
    check_typed_status(tree, {0, 0, 0}, TwoLargePrimeCycleBasisStatus::edge_limit);

    const std::vector<TwoLargePrimeEdge> triangle{
        {101, 103, 30},
        {103, 107, 31},
        {107, 101, 32},
    };
    check_typed_status(triangle, {3, 0, 0}, TwoLargePrimeCycleBasisStatus::cycle_limit);
    check_typed_status(triangle, {3, 1, 0}, TwoLargePrimeCycleBasisStatus::incidence_limit);
}

void test_bounded_status_precedence() {
    const std::vector<TwoLargePrimeEdge> oversized_invalid{
        {0, 0, 40},
        {101, 103, 40},
    };
    check_typed_status(oversized_invalid, {1, 0, 0}, TwoLargePrimeCycleBasisStatus::edge_limit);
    check_typed_status(oversized_invalid, {2, 0, 0}, TwoLargePrimeCycleBasisStatus::invalid_edge);

    auto reordered_invalid = oversized_invalid;
    std::reverse(reordered_invalid.begin(), reordered_invalid.end());
    for (auto& edge : reordered_invalid) {
        std::swap(edge.p, edge.q);
    }
    check_typed_status(reordered_invalid, {2, 0, 0}, TwoLargePrimeCycleBasisStatus::invalid_edge);

    const std::vector<TwoLargePrimeEdge> duplicate_relation_index{
        {0, 101, 50},
        {101, 103, 50},
    };
    check_typed_status(duplicate_relation_index, {2, 0, 0},
                       TwoLargePrimeCycleBasisStatus::duplicate_relation_index);

    const std::vector<TwoLargePrimeEdge> valid_triangle{
        {101, 103, 60},
        {103, 107, 61},
        {107, 101, 62},
    };
    check_typed_status(valid_triangle, {3, 0, 0}, TwoLargePrimeCycleBasisStatus::cycle_limit);
    check_typed_status(valid_triangle, {3, 1, 2}, TwoLargePrimeCycleBasisStatus::incidence_limit);

    const std::vector<TwoLargePrimeEdge> self_loops{
        {101, 101, 70},
        {103, 103, 71},
        {107, 107, 72},
    };
    check_typed_status(self_loops, {3, 2, 0}, TwoLargePrimeCycleBasisStatus::cycle_limit);
    // Every self-loop contributes one cycle incidence. Since cycle_count alone
    // exceeds the incidence budget, this must fail before allocating cycle vectors.
    check_typed_status(self_loops, {3, 3, 2}, TwoLargePrimeCycleBasisStatus::incidence_limit);
}

void test_unlimited_typed_and_legacy_parity() {
    const std::vector<TwoLargePrimeEdge> edges{
        {0, 101, 70},
        {101, 103, 71},
        {103, 0, 72},
        {107, 107, 73},
    };
    const auto legacy = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(edges.data(), edges.size()));
    const auto typed = build_two_large_prime_cycle_basis(
        std::span<const TwoLargePrimeEdge>(edges.data(), edges.size()), unlimited_limits());
    CHECK(legacy.has_value());
    CHECK(typed.status() == TwoLargePrimeCycleBasisStatus::valid);
    CHECK(typed.is_valid());
    CHECK(typed.basis().has_value());
    if (legacy && typed.basis()) {
        CHECK(same_basis(*legacy, *typed.basis()));
    }
}

void test_typed_result_value_semantics() {
    const std::vector<TwoLargePrimeEdge> edges{
        {101, 103, 80},
        {103, 107, 81},
        {107, 101, 82},
    };
    const auto edge_span = std::span<const TwoLargePrimeEdge>(edges.data(), edges.size());
    auto valid = build_two_large_prime_cycle_basis(edge_span, {3, 1, 3});
    auto failure = build_two_large_prime_cycle_basis(edge_span, {2, 1, 3});
    check_result_state(valid, TwoLargePrimeCycleBasisStatus::valid);
    check_result_state(failure, TwoLargePrimeCycleBasisStatus::edge_limit);

    TwoLargePrimeCycleBasisResult copied_valid(valid);
    TwoLargePrimeCycleBasisResult copied_failure(failure);
    check_result_state(copied_valid, TwoLargePrimeCycleBasisStatus::valid);
    check_result_state(copied_failure, TwoLargePrimeCycleBasisStatus::edge_limit);
    if (valid.basis() && copied_valid.basis()) {
        CHECK(same_basis(*valid.basis(), *copied_valid.basis()));
    }
    copied_failure = valid;
    copied_valid = failure;
    check_result_state(copied_failure, TwoLargePrimeCycleBasisStatus::valid);
    check_result_state(copied_valid, TwoLargePrimeCycleBasisStatus::edge_limit);
    self_copy_assign(copied_failure);
    self_copy_assign(copied_valid);
    check_result_state(copied_failure, TwoLargePrimeCycleBasisStatus::valid);
    check_result_state(copied_valid, TwoLargePrimeCycleBasisStatus::edge_limit);

    TwoLargePrimeCycleBasisResult moved_valid(std::move(valid));
    TwoLargePrimeCycleBasisResult moved_failure(std::move(failure));
    check_result_state(moved_valid, TwoLargePrimeCycleBasisStatus::valid);
    check_result_state(moved_failure, TwoLargePrimeCycleBasisStatus::edge_limit);
    check_result_state(valid, TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
    check_result_state(failure, TwoLargePrimeCycleBasisStatus::internal_invariant_failure);

    copied_valid = std::move(moved_valid);
    check_result_state(copied_valid, TwoLargePrimeCycleBasisStatus::valid);
    check_result_state(moved_valid, TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
    self_move_assign(copied_valid);
    self_move_assign(moved_failure);
    check_result_state(copied_valid, TwoLargePrimeCycleBasisStatus::valid);
    check_result_state(moved_failure, TwoLargePrimeCycleBasisStatus::edge_limit);
}

void test_full_size_t_relation_id_domain() {
    const size_t max_id = std::numeric_limits<size_t>::max();
    const std::vector<TwoLargePrimeEdge> edges{
        {0, 101, max_id},
        {0, 103, 1},
        {101, 103, 2},
    };
    const auto basis = build_checked(edges);
    if (basis) {
        check_expected(*basis, 3, 3, 1, {{1, 2, max_id}});
    }
}

void test_small_multigraph_exhaustive_oracle() {
    // Each slot is a distinct relation. Repeated endpoint pairs deliberately
    // create parallel edges, while the loops exercise p^2 relations.
    constexpr std::array<std::pair<uint64_t, uint64_t>, 11> edge_slots{{
        {0, 101},
        {101, 0},
        {0, 103},
        {101, 101},
        {101, 103},
        {103, 101},
        {101, 107},
        {103, 103},
        {103, 107},
        {107, 103},
        {107, 107},
    }};

    const size_t graph_count = size_t{1} << edge_slots.size();
    for (size_t mask = 0; mask < graph_count; ++mask) {
        std::vector<TwoLargePrimeEdge> edges;
        edges.reserve(edge_slots.size());
        for (size_t slot = 0; slot < edge_slots.size(); ++slot) {
            if ((mask & (size_t{1} << slot)) == 0) {
                continue;
            }
            edges.push_back(TwoLargePrimeEdge{
                edge_slots[slot].first,
                edge_slots[slot].second,
                1'000 + slot,
            });
        }

        const auto basis = build_checked(edges);

        auto reordered = edges;
        std::reverse(reordered.begin(), reordered.end());
        for (auto& edge : reordered) {
            std::swap(edge.p, edge.q);
        }
        const auto reordered_basis = build_two_large_prime_cycle_basis(
            std::span<const TwoLargePrimeEdge>(reordered.data(), reordered.size()));
        const auto typed_basis = build_two_large_prime_cycle_basis(
            std::span<const TwoLargePrimeEdge>(edges.data(), edges.size()), unlimited_limits());
        const auto typed_reordered_basis = build_two_large_prime_cycle_basis(
            std::span<const TwoLargePrimeEdge>(reordered.data(), reordered.size()),
            unlimited_limits());

        CHECK(basis.has_value() == reordered_basis.has_value());
        CHECK(typed_basis.status() == TwoLargePrimeCycleBasisStatus::valid);
        CHECK(typed_reordered_basis.status() == typed_basis.status());
        CHECK(typed_basis.basis().has_value() == basis.has_value());
        CHECK(typed_reordered_basis.basis().has_value() == reordered_basis.has_value());
        if (basis && reordered_basis) {
            CHECK(same_basis(*basis, *reordered_basis));
        }
        if (basis && typed_basis.basis() && typed_reordered_basis.basis()) {
            CHECK(same_basis(*basis, *typed_basis.basis()));
            CHECK(same_basis(*basis, *typed_reordered_basis.basis()));
        }
    }
}

void test_invalid_inputs_fail_closed() {
    const std::vector<TwoLargePrimeEdge> duplicate_relation_index{
        {0, 101, 7},
        {101, 103, 7},
    };
    CHECK(!build_two_large_prime_cycle_basis(
               std::span<const TwoLargePrimeEdge>(duplicate_relation_index.data(),
                                                  duplicate_relation_index.size()))
               .has_value());

    const std::vector<TwoLargePrimeEdge> zero_zero{{0, 0, 8}};
    CHECK(!build_two_large_prime_cycle_basis(
               std::span<const TwoLargePrimeEdge>(zero_zero.data(), zero_zero.size()))
               .has_value());

    const std::vector<TwoLargePrimeEdge> endpoint_one{{1, 101, 9}};
    CHECK(!build_two_large_prime_cycle_basis(
               std::span<const TwoLargePrimeEdge>(endpoint_one.data(),
                                                  endpoint_one.size()))
               .has_value());

    const std::vector<TwoLargePrimeEdge> one_lp_endpoint_one{{0, 1, 10}};
    CHECK(!build_two_large_prime_cycle_basis(
               std::span<const TwoLargePrimeEdge>(one_lp_endpoint_one.data(),
                                                  one_lp_endpoint_one.size()))
               .has_value());
}

} // namespace

int main() {
    test_empty_graph_and_tree();
    test_parallel_edges_and_self_loop();
    test_triangle_and_mixed_one_lp_cycle();
    test_disconnected_components_and_cycle_rank();
    test_input_order_and_endpoint_direction_are_irrelevant();
    test_exact_and_one_over_limits();
    test_zero_limits();
    test_bounded_status_precedence();
    test_unlimited_typed_and_legacy_parity();
    test_typed_result_value_semantics();
    test_full_size_t_relation_id_domain();
    test_small_multigraph_exhaustive_oracle();
    test_invalid_inputs_fail_closed();

    std::cout << checks_passed << " checks passed, " << checks_failed
              << " checks failed\n";
    return checks_failed == 0 ? 0 : 1;
}
