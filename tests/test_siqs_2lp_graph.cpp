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

    for (const auto& cycle : basis.cycles) {
        CHECK(!cycle.empty());
        CHECK(std::is_sorted(cycle.begin(), cycle.end()));

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

    CHECK(gf2_rank(basis.cycles, edges) == basis.cycles.size());
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
    return lhs.vertex_count == rhs.vertex_count &&
           lhs.edge_count == rhs.edge_count &&
           lhs.component_count == rhs.component_count &&
           lhs.cycles == rhs.cycles;
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

        CHECK(basis.has_value() == reordered_basis.has_value());
        if (basis && reordered_basis) {
            CHECK(same_basis(*basis, *reordered_basis));
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
    test_full_size_t_relation_id_domain();
    test_small_multigraph_exhaustive_oracle();
    test_invalid_inputs_fail_closed();

    std::cout << checks_passed << " checks passed, " << checks_failed
              << " checks failed\n";
    return checks_failed == 0 ? 0 : 1;
}
