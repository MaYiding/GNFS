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
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::siqs {

/// An undirected multigraph edge contributed by one partial relation.
/// Endpoint zero is reserved for a one-large-prime relation.
struct TwoLargePrimeEdge {
    uint64_t p;
    uint64_t q;
    size_t relation_index;

    [[nodiscard]] friend constexpr bool operator==(const TwoLargePrimeEdge&,
                                                   const TwoLargePrimeEdge&) = default;
};

/// A deterministic fundamental-cycle basis of the partial-relation graph.
struct TwoLargePrimeCycleBasis {
    size_t vertex_count;
    size_t edge_count;
    size_t component_count;
    size_t total_cycle_incidences;
    size_t max_cycle_length;
    std::vector<std::vector<size_t>> cycles;
};

/// Hard construction limits for a bounded cycle basis.
///
/// Every value is an inclusive maximum. In particular, zero permits no object
/// of the corresponding kind; it never means unlimited.
struct TwoLargePrimeCycleBasisLimits {
    size_t max_edges;
    size_t max_cycles;
    size_t max_cycle_incidences;
};

enum class TwoLargePrimeCycleBasisStatus : uint8_t {
    valid,
    edge_limit,
    invalid_edge,
    duplicate_relation_index,
    size_overflow,
    cycle_limit,
    incidence_limit,
    internal_invariant_failure,
};

namespace two_large_prime_graph_detail {
struct TwoLargePrimeCycleBasisResultFactory;
}

/// Invariant-safe result: a basis is present exactly when status() is valid.
class TwoLargePrimeCycleBasisResult {
public:
    TwoLargePrimeCycleBasisResult(const TwoLargePrimeCycleBasisResult&) = default;
    TwoLargePrimeCycleBasisResult& operator=(const TwoLargePrimeCycleBasisResult& other) {
        if (this != &other) {
            TwoLargePrimeCycleBasisResult replacement(other);
            swap(replacement);
        }
        return *this;
    }

    TwoLargePrimeCycleBasisResult(TwoLargePrimeCycleBasisResult&& other) noexcept
        : status_(other.status_), basis_(std::move(other.basis_)) {
        other.status_ = TwoLargePrimeCycleBasisStatus::internal_invariant_failure;
        other.basis_.reset();
    }

    TwoLargePrimeCycleBasisResult& operator=(TwoLargePrimeCycleBasisResult&& other) noexcept {
        if (this != &other) {
            status_ = other.status_;
            basis_ = std::move(other.basis_);
            other.status_ = TwoLargePrimeCycleBasisStatus::internal_invariant_failure;
            other.basis_.reset();
        }
        return *this;
    }

    [[nodiscard]] TwoLargePrimeCycleBasisStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<TwoLargePrimeCycleBasis>& basis() const noexcept {
        return basis_;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return status_ == TwoLargePrimeCycleBasisStatus::valid && basis_.has_value();
    }

private:
    friend struct two_large_prime_graph_detail::TwoLargePrimeCycleBasisResultFactory;

    TwoLargePrimeCycleBasisResult(TwoLargePrimeCycleBasisStatus status,
                                  std::optional<TwoLargePrimeCycleBasis> basis)
        : status_(status), basis_(std::move(basis)) {}

    void swap(TwoLargePrimeCycleBasisResult& other) noexcept {
        static_assert(std::is_nothrow_swappable_v<TwoLargePrimeCycleBasisStatus>);
        static_assert(std::is_nothrow_swappable_v<std::optional<TwoLargePrimeCycleBasis>>);
        using std::swap;
        swap(status_, other.status_);
        swap(basis_, other.basis_);
    }

    TwoLargePrimeCycleBasisStatus status_;
    std::optional<TwoLargePrimeCycleBasis> basis_;
};

namespace two_large_prime_graph_detail {

struct TwoLargePrimeCycleBasisResultFactory {
    [[nodiscard]] static TwoLargePrimeCycleBasisResult
    failure(TwoLargePrimeCycleBasisStatus status) {
        if (status == TwoLargePrimeCycleBasisStatus::valid) {
            status = TwoLargePrimeCycleBasisStatus::internal_invariant_failure;
        }
        return TwoLargePrimeCycleBasisResult(status, std::nullopt);
    }

    [[nodiscard]] static TwoLargePrimeCycleBasisResult success(TwoLargePrimeCycleBasis basis) {
        return TwoLargePrimeCycleBasisResult(TwoLargePrimeCycleBasisStatus::valid,
                                             std::move(basis));
    }

    [[nodiscard]] static std::optional<TwoLargePrimeCycleBasis>
    release(TwoLargePrimeCycleBasisResult&& result) {
        if (!result.is_valid()) {
            return std::nullopt;
        }
        auto basis = std::move(result.basis_);
        result.status_ = TwoLargePrimeCycleBasisStatus::internal_invariant_failure;
        result.basis_.reset();
        return basis;
    }
};

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

template <class T> [[nodiscard]] inline bool vector_size_is_representable(size_t count) noexcept {
    return count <= std::vector<T>{}.max_size();
}

class DisjointSet {
public:
    explicit DisjointSet(size_t size) : parent_(size), rank_(size, 0) {
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

        if (rank_[lhs] < rank_[rhs] || (rank_[lhs] == rank_[rhs] && lhs > rhs)) {
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
/// Returns false rather than relying on vector::length_error when scratch size
/// is not representable.
[[nodiscard]] inline bool radix_sort_relation_indices(std::vector<size_t>& values) {
    if (values.size() < 2) {
        return true;
    }
    if (!vector_size_is_representable<size_t>(values.size())) {
        return false;
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
    return true;
}

} // namespace two_large_prime_graph_detail

/// Build a deterministic fundamental-cycle basis for an undirected multigraph.
///
/// Edges are first normalized and ordered by (min endpoint, max endpoint,
/// relation index). That order selects the spanning forest, so reversing edge
/// endpoints or permuting the input cannot change the result. Parallel edges
/// remain distinct, and a self-loop is a one-edge cycle.
///
/// Each returned cycle contains relation indices in ascending order. This
/// graph-only boundary does not repeat primality or LP-bound checks; production
/// callers must validate those endpoints before constructing edges.
///
/// The edge limit is checked before any allocation proportional to E. The
/// cycle and minimum-incidence limits are checked once cycle rank is known and
/// before allocating the cycle vector. Total incidence is checked before every
/// relation identifier is appended to a cycle.
[[nodiscard]] inline TwoLargePrimeCycleBasisResult
build_two_large_prime_cycle_basis(std::span<const TwoLargePrimeEdge> edges,
                                  const TwoLargePrimeCycleBasisLimits& limits) {
    using namespace two_large_prime_graph_detail;

    if (edges.size() > limits.max_edges) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::edge_limit);
    }
    if (!vector_size_is_representable<CanonicalEdge>(edges.size())) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::size_overflow);
    }

    std::vector<CanonicalEdge> canonical_edges;
    canonical_edges.reserve(edges.size());
    for (const auto& edge : edges) {
        const uint64_t p = std::min(edge.p, edge.q);
        const uint64_t q = std::max(edge.p, edge.q);

        // Zero is valid only as the virtual endpoint of a 1LP edge. Endpoint
        // one is never a valid large-prime graph vertex.
        if ((p == 0 && q < 2) || (p != 0 && p < 2)) {
            return TwoLargePrimeCycleBasisResultFactory::failure(
                TwoLargePrimeCycleBasisStatus::invalid_edge);
        }
        canonical_edges.push_back(CanonicalEdge{p, q, edge.relation_index});
    }

    std::sort(canonical_edges.begin(), canonical_edges.end(),
              [](const CanonicalEdge& lhs, const CanonicalEdge& rhs) {
                  if (lhs.p != rhs.p)
                      return lhs.p < rhs.p;
                  if (lhs.q != rhs.q)
                      return lhs.q < rhs.q;
                  return lhs.relation_index < rhs.relation_index;
              });

    if (!vector_size_is_representable<size_t>(canonical_edges.size())) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::size_overflow);
    }
    std::vector<size_t> relation_indices;
    relation_indices.reserve(canonical_edges.size());
    for (const auto& edge : canonical_edges) {
        relation_indices.push_back(edge.relation_index);
    }
    std::sort(relation_indices.begin(), relation_indices.end());
    if (std::adjacent_find(relation_indices.begin(), relation_indices.end()) !=
        relation_indices.end()) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::duplicate_relation_index);
    }

    std::vector<uint64_t> vertices;
    if (canonical_edges.size() > vertices.max_size() / 2) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::size_overflow);
    }
    vertices.reserve(canonical_edges.size() * 2);
    for (const auto& edge : canonical_edges) {
        vertices.push_back(edge.p);
        vertices.push_back(edge.q);
    }
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());

    using ForestAdjacency = std::vector<std::pair<size_t, size_t>>;
    if (!vector_size_is_representable<ForestAdjacency>(vertices.size()) ||
        !vector_size_is_representable<std::pair<size_t, size_t>>(canonical_edges.size()) ||
        !vector_size_is_representable<IndexedEdge>(canonical_edges.size()) ||
        !vector_size_is_representable<size_t>(vertices.size()) ||
        !vector_size_is_representable<unsigned char>(vertices.size())) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::size_overflow);
    }

    const auto vertex_index = [&vertices](uint64_t vertex) {
        return static_cast<size_t>(std::lower_bound(vertices.begin(), vertices.end(), vertex) -
                                   vertices.begin());
    };

    std::vector<ForestAdjacency> forest(vertices.size());
    std::vector<IndexedEdge> chords;
    chords.reserve(canonical_edges.size());
    DisjointSet components(vertices.size());
    size_t component_count = vertices.size();
    size_t tree_edge_count = 0;

    for (const auto& edge : canonical_edges) {
        const size_t p = vertex_index(edge.p);
        const size_t q = vertex_index(edge.q);
        if (p != q && components.unite(p, q)) {
            if (tree_edge_count == std::numeric_limits<size_t>::max() || component_count == 0 ||
                forest[p].size() == forest[p].max_size() ||
                forest[q].size() == forest[q].max_size()) {
                return TwoLargePrimeCycleBasisResultFactory::failure(
                    TwoLargePrimeCycleBasisStatus::size_overflow);
            }
            forest[p].emplace_back(q, edge.relation_index);
            forest[q].emplace_back(p, edge.relation_index);
            ++tree_edge_count;
            --component_count;
        } else {
            if (chords.size() == chords.max_size()) {
                return TwoLargePrimeCycleBasisResultFactory::failure(
                    TwoLargePrimeCycleBasisStatus::size_overflow);
            }
            chords.push_back(IndexedEdge{p, q, edge.relation_index});
        }
    }

    if (canonical_edges.size() < tree_edge_count ||
        chords.size() != canonical_edges.size() - tree_edge_count) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
    }
    if (chords.size() > limits.max_cycles) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::cycle_limit);
    }
    // Every fundamental cycle contains its chord, so this lower bound can
    // reject an impossible incidence budget before cycles.reserve().
    if (chords.size() > limits.max_cycle_incidences) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::incidence_limit);
    }
    if (!vector_size_is_representable<std::vector<size_t>>(chords.size())) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::size_overflow);
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
        if (rooted_component_count == std::numeric_limits<size_t>::max()) {
            return TwoLargePrimeCycleBasisResultFactory::failure(
                TwoLargePrimeCycleBasisStatus::size_overflow);
        }
        ++rooted_component_count;
        parent[root] = root;
        if (stack.size() == stack.max_size()) {
            return TwoLargePrimeCycleBasisResultFactory::failure(
                TwoLargePrimeCycleBasisStatus::size_overflow);
        }
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
                if (depth[vertex] == std::numeric_limits<size_t>::max()) {
                    return TwoLargePrimeCycleBasisResultFactory::failure(
                        TwoLargePrimeCycleBasisStatus::size_overflow);
                }
                depth[neighbor] = depth[vertex] + 1;
                if (stack.size() == stack.max_size()) {
                    return TwoLargePrimeCycleBasisResultFactory::failure(
                        TwoLargePrimeCycleBasisStatus::size_overflow);
                }
                stack.push_back(neighbor);
            }
        }
    }

    if (rooted_component_count != component_count || component_count > vertices.size() ||
        tree_edge_count != vertices.size() - component_count) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
    }

    std::vector<std::vector<size_t>> cycles;
    cycles.reserve(chords.size());
    size_t total_cycle_incidences = 0;
    size_t max_cycle_length = 0;
    const auto append_cycle_relation =
        [&limits, &total_cycle_incidences](
            std::vector<size_t>& cycle,
            size_t relation_index) -> std::optional<TwoLargePrimeCycleBasisStatus> {
        if (total_cycle_incidences == std::numeric_limits<size_t>::max() ||
            cycle.size() == cycle.max_size()) {
            return TwoLargePrimeCycleBasisStatus::size_overflow;
        }
        const size_t next_total = total_cycle_incidences + 1;
        if (next_total > limits.max_cycle_incidences) {
            return TwoLargePrimeCycleBasisStatus::incidence_limit;
        }
        cycle.push_back(relation_index);
        total_cycle_incidences = next_total;
        return std::nullopt;
    };

    for (const auto& chord : chords) {
        std::vector<size_t> cycle;
        if (const auto status = append_cycle_relation(cycle, chord.relation_index)) {
            return TwoLargePrimeCycleBasisResultFactory::failure(*status);
        }
        size_t lhs = chord.p;
        size_t rhs = chord.q;

        while (depth[lhs] > depth[rhs]) {
            if (parent[lhs] == no_index || parent[lhs] == lhs) {
                return TwoLargePrimeCycleBasisResultFactory::failure(
                    TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
            }
            if (const auto status = append_cycle_relation(cycle, parent_edge[lhs])) {
                return TwoLargePrimeCycleBasisResultFactory::failure(*status);
            }
            lhs = parent[lhs];
        }
        while (depth[rhs] > depth[lhs]) {
            if (parent[rhs] == no_index || parent[rhs] == rhs) {
                return TwoLargePrimeCycleBasisResultFactory::failure(
                    TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
            }
            if (const auto status = append_cycle_relation(cycle, parent_edge[rhs])) {
                return TwoLargePrimeCycleBasisResultFactory::failure(*status);
            }
            rhs = parent[rhs];
        }
        while (lhs != rhs) {
            if (parent[lhs] == no_index || parent[rhs] == no_index || parent[lhs] == lhs ||
                parent[rhs] == rhs) {
                return TwoLargePrimeCycleBasisResultFactory::failure(
                    TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
            }
            if (const auto status = append_cycle_relation(cycle, parent_edge[lhs])) {
                return TwoLargePrimeCycleBasisResultFactory::failure(*status);
            }
            if (const auto status = append_cycle_relation(cycle, parent_edge[rhs])) {
                return TwoLargePrimeCycleBasisResultFactory::failure(*status);
            }
            lhs = parent[lhs];
            rhs = parent[rhs];
        }

        if (!radix_sort_relation_indices(cycle)) {
            return TwoLargePrimeCycleBasisResultFactory::failure(
                TwoLargePrimeCycleBasisStatus::size_overflow);
        }
        max_cycle_length = std::max(max_cycle_length, cycle.size());
        if (cycles.size() == cycles.max_size()) {
            return TwoLargePrimeCycleBasisResultFactory::failure(
                TwoLargePrimeCycleBasisStatus::size_overflow);
        }
        cycles.push_back(std::move(cycle));
    }

    // A forest has V-C tree edges, hence every remaining edge contributes one
    // fundamental cycle and the cycle rank is exactly E-V+C.
    if (cycles.size() != chords.size() ||
        cycles.size() != canonical_edges.size() - tree_edge_count) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
    }

    size_t verified_total_cycle_incidences = 0;
    size_t verified_max_cycle_length = 0;
    for (const auto& cycle : cycles) {
        if (cycle.empty()) {
            return TwoLargePrimeCycleBasisResultFactory::failure(
                TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
        }
        if (cycle.size() > std::numeric_limits<size_t>::max() - verified_total_cycle_incidences) {
            return TwoLargePrimeCycleBasisResultFactory::failure(
                TwoLargePrimeCycleBasisStatus::size_overflow);
        }
        verified_total_cycle_incidences += cycle.size();
        verified_max_cycle_length = std::max(verified_max_cycle_length, cycle.size());
    }
    if (verified_total_cycle_incidences != total_cycle_incidences ||
        verified_max_cycle_length != max_cycle_length ||
        total_cycle_incidences > limits.max_cycle_incidences) {
        return TwoLargePrimeCycleBasisResultFactory::failure(
            TwoLargePrimeCycleBasisStatus::internal_invariant_failure);
    }

    return TwoLargePrimeCycleBasisResultFactory::success(
        TwoLargePrimeCycleBasis{vertices.size(), canonical_edges.size(), component_count,
                                total_cycle_incidences, max_cycle_length, std::move(cycles)});
}

/// Source-compatible unlimited wrapper for the legacy optional API.
[[nodiscard]] inline std::optional<TwoLargePrimeCycleBasis>
build_two_large_prime_cycle_basis(std::span<const TwoLargePrimeEdge> edges) {
    const size_t unlimited = std::numeric_limits<size_t>::max();
    auto result = build_two_large_prime_cycle_basis(
        edges, TwoLargePrimeCycleBasisLimits{unlimited, unlimited, unlimited});
    return two_large_prime_graph_detail::TwoLargePrimeCycleBasisResultFactory::release(
        std::move(result));
}

} // namespace gnfs::siqs
