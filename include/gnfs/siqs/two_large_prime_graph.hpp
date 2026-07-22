#pragma once

/// @file two_large_prime_graph.hpp
/// @brief Deterministic cycle-basis construction for SIQS 1LP/2LP relations.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gnfs::siqs {

/// An undirected multigraph edge contributed by one partial relation.
/// Endpoint zero is reserved for a one-large-prime relation.
struct TwoLargePrimeEdge {
    uint64_t p;
    uint64_t q;
    size_t relation_index;

    [[nodiscard]] friend constexpr bool operator==(
        const TwoLargePrimeEdge&,
        const TwoLargePrimeEdge&) = default;
};

/// A deterministic fundamental-cycle basis of the partial-relation graph.
struct TwoLargePrimeCycleBasis {
    size_t vertex_count;
    size_t edge_count;
    size_t component_count;
    std::vector<std::vector<size_t>> cycles;
};

namespace two_large_prime_graph_detail {

struct CanonicalEdge {
    uint64_t p;
    uint64_t q;
    size_t relation_index;
};

struct IndexedEdge {
    size_t p;
    size_t q;
    size_t relation_index;
};

class DisjointSet {
public:
    explicit DisjointSet(size_t size)
        : parent_(size), rank_(size, 0) {
        std::iota(parent_.begin(), parent_.end(), size_t{0});
    }

    [[nodiscard]] size_t find(size_t vertex) {
        size_t root = vertex;
        while (parent_[root] != root) {
            root = parent_[root];
        }
        while (parent_[vertex] != vertex) {
            const size_t next = parent_[vertex];
            parent_[vertex] = root;
            vertex = next;
        }
        return root;
    }

    [[nodiscard]] bool unite(size_t lhs, size_t rhs) {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs == rhs) {
            return false;
        }

        if (rank_[lhs] < rank_[rhs] ||
            (rank_[lhs] == rank_[rhs] && lhs > rhs)) {
            std::swap(lhs, rhs);
        }
        parent_[rhs] = lhs;
        if (rank_[lhs] == rank_[rhs]) {
            ++rank_[lhs];
        }
        return true;
    }

private:
    std::vector<size_t> parent_;
    std::vector<unsigned char> rank_;
};

/// Sort fixed-width relation identifiers in linear time in the cycle length.
inline void radix_sort_relation_indices(std::vector<size_t>& values) {
    if (values.size() < 2) {
        return;
    }

    constexpr size_t radix = 256;
    constexpr size_t mask = radix - 1;
    constexpr size_t bits_per_pass = 8;
    constexpr size_t bit_count = std::numeric_limits<size_t>::digits;

    std::vector<size_t> scratch(values.size());
    for (size_t shift = 0; shift < bit_count; shift += bits_per_pass) {
        std::array<size_t, radix> offsets{};
        for (const size_t value : values) {
            ++offsets[(value >> shift) & mask];
        }

        size_t next = 0;
        for (size_t& offset : offsets) {
            const size_t count = offset;
            offset = next;
            next += count;
        }
        for (const size_t value : values) {
            scratch[offsets[(value >> shift) & mask]++] = value;
        }
        values.swap(scratch);
    }
}

} // namespace two_large_prime_graph_detail

/// Build a deterministic fundamental-cycle basis for an undirected multigraph.
///
/// Edges are first normalized and ordered by (min endpoint, max endpoint,
/// relation index). That order selects the spanning forest, so reversing edge
/// endpoints or permuting the input cannot change the result. Parallel edges
/// remain distinct, and a self-loop is a one-edge cycle.
///
/// Returns nullopt for an invalid endpoint encoding or a duplicate relation
/// index. Each returned cycle contains relation indices in ascending order.
/// This graph-only boundary does not repeat primality or LP-bound checks;
/// production callers must validate those endpoints before constructing edges.
[[nodiscard]] inline std::optional<TwoLargePrimeCycleBasis>
build_two_large_prime_cycle_basis(std::span<const TwoLargePrimeEdge> edges) {
    using namespace two_large_prime_graph_detail;

    std::vector<CanonicalEdge> canonical_edges;
    canonical_edges.reserve(edges.size());
    for (const auto& edge : edges) {
        const uint64_t p = std::min(edge.p, edge.q);
        const uint64_t q = std::max(edge.p, edge.q);

        // Zero is valid only as the virtual endpoint of a 1LP edge. Endpoint
        // one is never a valid large-prime graph vertex.
        if ((p == 0 && q < 2) || (p != 0 && p < 2)) {
            return std::nullopt;
        }
        canonical_edges.push_back(CanonicalEdge{p, q, edge.relation_index});
    }

    std::sort(canonical_edges.begin(), canonical_edges.end(),
              [](const CanonicalEdge& lhs, const CanonicalEdge& rhs) {
                  if (lhs.p != rhs.p) return lhs.p < rhs.p;
                  if (lhs.q != rhs.q) return lhs.q < rhs.q;
                  return lhs.relation_index < rhs.relation_index;
              });

    std::vector<size_t> relation_indices;
    relation_indices.reserve(canonical_edges.size());
    for (const auto& edge : canonical_edges) {
        relation_indices.push_back(edge.relation_index);
    }
    std::sort(relation_indices.begin(), relation_indices.end());
    if (std::adjacent_find(relation_indices.begin(), relation_indices.end()) !=
        relation_indices.end()) {
        return std::nullopt;
    }

    std::vector<uint64_t> vertices;
    if (canonical_edges.size() > vertices.max_size() / 2) {
        return std::nullopt;
    }
    vertices.reserve(canonical_edges.size() * 2);
    for (const auto& edge : canonical_edges) {
        vertices.push_back(edge.p);
        vertices.push_back(edge.q);
    }
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());

    const auto vertex_index = [&vertices](uint64_t vertex) {
        return static_cast<size_t>(
            std::lower_bound(vertices.begin(), vertices.end(), vertex) - vertices.begin());
    };

    std::vector<std::vector<std::pair<size_t, size_t>>> forest(vertices.size());
    std::vector<IndexedEdge> chords;
    chords.reserve(canonical_edges.size());
    DisjointSet components(vertices.size());
    size_t component_count = vertices.size();
    size_t tree_edge_count = 0;

    for (const auto& edge : canonical_edges) {
        const size_t p = vertex_index(edge.p);
        const size_t q = vertex_index(edge.q);
        if (p != q && components.unite(p, q)) {
            forest[p].emplace_back(q, edge.relation_index);
            forest[q].emplace_back(p, edge.relation_index);
            ++tree_edge_count;
            --component_count;
        } else {
            chords.push_back(IndexedEdge{p, q, edge.relation_index});
        }
    }

    // Root each tree at its smallest vertex. A tree has a unique root path, so
    // adjacency traversal order cannot affect the fundamental cycles.
    const size_t no_index = std::numeric_limits<size_t>::max();
    std::vector<size_t> parent(vertices.size(), no_index);
    // parent[] carries visitation/root state. Keep parent_edge unconstrained so
    // every size_t value remains a valid external relation identifier,
    // including SIZE_MAX.
    std::vector<size_t> parent_edge(vertices.size(), 0);
    std::vector<size_t> depth(vertices.size(), 0);
    std::vector<size_t> stack;
    stack.reserve(vertices.size());
    size_t rooted_component_count = 0;

    for (size_t root = 0; root < vertices.size(); ++root) {
        if (parent[root] != no_index) {
            continue;
        }
        ++rooted_component_count;
        parent[root] = root;
        stack.push_back(root);

        while (!stack.empty()) {
            const size_t vertex = stack.back();
            stack.pop_back();
            for (const auto& [neighbor, relation_index] : forest[vertex]) {
                if (parent[neighbor] != no_index) {
                    continue;
                }
                parent[neighbor] = vertex;
                parent_edge[neighbor] = relation_index;
                depth[neighbor] = depth[vertex] + 1;
                stack.push_back(neighbor);
            }
        }
    }

    if (rooted_component_count != component_count ||
        component_count > vertices.size() ||
        tree_edge_count != vertices.size() - component_count) {
        return std::nullopt;
    }

    std::vector<std::vector<size_t>> cycles;
    cycles.reserve(chords.size());
    for (const auto& chord : chords) {
        std::vector<size_t> cycle{chord.relation_index};
        size_t lhs = chord.p;
        size_t rhs = chord.q;

        while (depth[lhs] > depth[rhs]) {
            if (parent[lhs] == no_index || parent[lhs] == lhs) {
                return std::nullopt;
            }
            cycle.push_back(parent_edge[lhs]);
            lhs = parent[lhs];
        }
        while (depth[rhs] > depth[lhs]) {
            if (parent[rhs] == no_index || parent[rhs] == rhs) {
                return std::nullopt;
            }
            cycle.push_back(parent_edge[rhs]);
            rhs = parent[rhs];
        }
        while (lhs != rhs) {
            if (parent[lhs] == no_index || parent[rhs] == no_index ||
                parent[lhs] == lhs || parent[rhs] == rhs) {
                return std::nullopt;
            }
            cycle.push_back(parent_edge[lhs]);
            cycle.push_back(parent_edge[rhs]);
            lhs = parent[lhs];
            rhs = parent[rhs];
        }

        radix_sort_relation_indices(cycle);
        cycles.push_back(std::move(cycle));
    }

    // A forest has V-C tree edges, hence every remaining edge contributes one
    // fundamental cycle and the cycle rank is exactly E-V+C.
    if (canonical_edges.size() < tree_edge_count ||
        cycles.size() != canonical_edges.size() - tree_edge_count) {
        return std::nullopt;
    }

    return TwoLargePrimeCycleBasis{
        vertices.size(), canonical_edges.size(), component_count, std::move(cycles)};
}

} // namespace gnfs::siqs
