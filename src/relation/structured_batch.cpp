#include "gnfs/relation/structured_batch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::relation {
namespace {

[[noreturn]] void fail(StructuredReductionErrorCode code, const char* message) {
    throw StructuredReductionError(code, message);
}

size_t checked_resource_add(size_t lhs, size_t rhs, const char* message) {
    if (rhs > std::numeric_limits<size_t>::max() - lhs)
        fail(StructuredReductionErrorCode::ResourceLimit, message);
    return lhs + rhs;
}

void validate_lp_key(const LargePrimeKey& key) {
    if (key.prime < 2)
        fail(StructuredReductionErrorCode::InvalidPlan, "batch candidate has an invalid LP prime");
    if (!key.is_algebraic && key.root != 0) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "batch candidate has a rational LP with a nonzero root");
    }
    constexpr uint64_t projective_root = std::numeric_limits<uint32_t>::max();
    if (key.is_algebraic && key.root != projective_root && key.root >= key.prime) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "batch candidate has an invalid algebraic LP root");
    }
}

void validate_lp_support(std::span<const LargePrimeKey> keys) {
    for (size_t i = 0; i < keys.size(); ++i) {
        validate_lp_key(keys[i]);
        if (i != 0 && !(keys[i - 1] < keys[i])) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "batch candidate LP support is not strictly canonical");
        }
    }
}

void validate_source_output(const SourceCombination& output, uint64_t generation) {
    if (output.empty() || output.generation() != generation) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "batch candidate source output has the wrong generation or is empty");
    }
    const auto sources = output.sources();
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].generation != generation || (i != 0 && !(sources[i - 1] < sources[i]))) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "batch candidate source output is not strictly canonical");
        }
    }
}

int planner_rank(TreeBasisPlanner planner) {
    switch (planner) {
    case TreeBasisPlanner::ReferenceStar:
        return 0;
    case TreeBasisPlanner::DeterministicMst:
        return 1;
    }
    fail(StructuredReductionErrorCode::InvalidPlan, "batch candidate has an unknown tree planner");
}

std::span<const StructuredRowId> candidate_members(const StructuredBatchCandidate& candidate) {
    if (candidate.valueless_by_exception())
        fail(StructuredReductionErrorCode::InvalidPlan, "batch candidate variant is valueless");
    return std::visit(
        [](const auto& plan) -> std::span<const StructuredRowId> {
            return std::span<const StructuredRowId>(plan.members.data(), plan.members.size());
        },
        candidate);
}

const LargePrimeKey& candidate_pivot(const StructuredBatchCandidate& candidate) {
    return std::visit(
        [](const auto& plan) -> const LargePrimeKey& {
            using Plan = std::remove_cvref_t<decltype(plan)>;
            if constexpr (std::is_same_v<Plan, TwoWayMergePlan>)
                return plan.witness;
            else
                return plan.pivot;
        },
        candidate);
}

int candidate_kind_rank(const StructuredBatchCandidate& candidate) {
    if (candidate.valueless_by_exception())
        fail(StructuredReductionErrorCode::InvalidPlan, "batch candidate variant is valueless");
    return std::holds_alternative<TwoWayMergePlan>(candidate) ? 0 : 1;
}

int candidate_planner_rank(const StructuredBatchCandidate& candidate) {
    if (const auto* tree = std::get_if<TreeBasisMergePlan>(&candidate))
        return planner_rank(tree->planner);
    return 0;
}

void validate_members(std::span<const StructuredRowId> members, size_t expected_min,
                      size_t expected_max, size_t total_row_count) {
    static_assert(std::numeric_limits<size_t>::digits <= std::numeric_limits<uint64_t>::digits);
    if (members.size() < expected_min || members.size() > expected_max) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "batch candidate has an invalid member count");
    }
    for (size_t i = 0; i < members.size(); ++i) {
        if (members[i].value > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
            static_cast<size_t>(members[i].value) >= total_row_count) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "batch candidate member is outside the row snapshot");
        }
        if (i != 0 && !(members[i - 1] < members[i])) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "batch candidate members are not strictly canonical");
        }
    }
}

struct ValidatedCandidate final {
    StructuredBatchCandidate candidate;
    size_t output_lp_nnz = 0;
    size_t total_source_atoms = 0;
};

ValidatedCandidate validate_candidate(StructuredBatchCandidate candidate,
                                      StructuredIncidenceSnapshotId snapshot,
                                      size_t total_row_count) {
    if (candidate.valueless_by_exception())
        fail(StructuredReductionErrorCode::InvalidPlan, "batch candidate variant is valueless");

    ValidatedCandidate result;
    if (const auto* two_way = std::get_if<TwoWayMergePlan>(&candidate)) {
        if (two_way->generation != snapshot.generation)
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "two-way batch candidate belongs to a different generation");
        if (two_way->incidence_epoch != snapshot.incidence_epoch)
            fail(StructuredReductionErrorCode::StalePlan,
                 "two-way batch candidate belongs to a stale incidence epoch");
        validate_members(two_way->members, 2, 2, total_row_count);
        validate_lp_key(two_way->witness);
        validate_source_output(two_way->expected_sources, snapshot.generation);
        validate_lp_support(two_way->expected_lp_keys);
        if (std::binary_search(two_way->expected_lp_keys.begin(), two_way->expected_lp_keys.end(),
                               two_way->witness)) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "two-way batch candidate retains its witness");
        }
        result.output_lp_nnz = two_way->expected_lp_keys.size();
        result.total_source_atoms = two_way->expected_sources.size();
    } else {
        const auto& tree = std::get<TreeBasisMergePlan>(candidate);
        if (tree.generation != snapshot.generation)
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "tree batch candidate belongs to a different generation");
        if (tree.incidence_epoch != snapshot.incidence_epoch)
            fail(StructuredReductionErrorCode::StalePlan,
                 "tree batch candidate belongs to a stale incidence epoch");
        (void)planner_rank(tree.planner);
        validate_lp_key(tree.pivot);
        validate_members(tree.members, 3, 8, total_row_count);
        if (tree.edges.size() != tree.members.size() - 1) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "tree batch candidate has an invalid edge count");
        }

        std::array<size_t, 8> parent{};
        for (size_t i = 0; i < tree.members.size(); ++i)
            parent[i] = i;
        auto find_root = [&](size_t node) {
            while (parent[node] != node) {
                parent[node] = parent[parent[node]];
                node = parent[node];
            }
            return node;
        };

        for (const auto& edge : tree.edges) {
            if (!(edge.endpoints[0] < edge.endpoints[1])) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "tree batch candidate edge endpoints are not ordered");
            }
            const auto lhs =
                std::lower_bound(tree.members.begin(), tree.members.end(), edge.endpoints[0]);
            const auto rhs =
                std::lower_bound(tree.members.begin(), tree.members.end(), edge.endpoints[1]);
            if (lhs == tree.members.end() || *lhs != edge.endpoints[0] ||
                rhs == tree.members.end() || *rhs != edge.endpoints[1]) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "tree batch candidate edge endpoint is not a member");
            }
            size_t lhs_root = find_root(static_cast<size_t>(lhs - tree.members.begin()));
            size_t rhs_root = find_root(static_cast<size_t>(rhs - tree.members.begin()));
            if (lhs_root == rhs_root) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "tree batch candidate edges contain a cycle");
            }
            if (rhs_root < lhs_root)
                std::swap(lhs_root, rhs_root);
            parent[rhs_root] = lhs_root;

            validate_source_output(edge.expected_sources, snapshot.generation);
            validate_lp_support(edge.expected_lp_keys);
            if (std::binary_search(edge.expected_lp_keys.begin(), edge.expected_lp_keys.end(),
                                   tree.pivot)) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "tree batch candidate output retains its pivot");
            }
            result.output_lp_nnz =
                checked_resource_add(result.output_lp_nnz, edge.expected_lp_keys.size(),
                                     "tree batch output LP metric overflows");
            result.total_source_atoms =
                checked_resource_add(result.total_source_atoms, edge.expected_sources.size(),
                                     "tree batch source metric overflows");
        }
        const size_t root = find_root(0);
        for (size_t i = 1; i < tree.members.size(); ++i) {
            if (find_root(i) != root) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "tree batch candidate edges are disconnected");
            }
        }
        if (result.output_lp_nnz != tree.output_lp_nnz) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "tree batch candidate output LP metric is inconsistent");
        }
        const size_t expected_growth = tree.output_lp_nnz > tree.input_nonpivot_lp_nnz
                                           ? tree.output_lp_nnz - tree.input_nonpivot_lp_nnz
                                           : 0;
        if (tree.lp_fill_growth != expected_growth) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "tree batch candidate fill metric is inconsistent");
        }
    }
    result.candidate = std::move(candidate);
    return result;
}

bool members_less(std::span<const StructuredRowId> lhs, std::span<const StructuredRowId> rhs) {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

bool identity_less(const ValidatedCandidate& lhs, const ValidatedCandidate& rhs) {
    const int lhs_kind = candidate_kind_rank(lhs.candidate);
    const int rhs_kind = candidate_kind_rank(rhs.candidate);
    if (lhs_kind != rhs_kind)
        return lhs_kind < rhs_kind;
    const int lhs_planner = candidate_planner_rank(lhs.candidate);
    const int rhs_planner = candidate_planner_rank(rhs.candidate);
    if (lhs_planner != rhs_planner)
        return lhs_planner < rhs_planner;
    return members_less(candidate_members(lhs.candidate), candidate_members(rhs.candidate));
}

bool identity_equal(const ValidatedCandidate& lhs, const ValidatedCandidate& rhs) {
    return !identity_less(lhs, rhs) && !identity_less(rhs, lhs);
}

bool global_candidate_less(const ValidatedCandidate& lhs, const ValidatedCandidate& rhs) {
    const int lhs_kind = candidate_kind_rank(lhs.candidate);
    const int rhs_kind = candidate_kind_rank(rhs.candidate);
    if (lhs_kind != rhs_kind)
        return lhs_kind < rhs_kind;
    if (lhs.output_lp_nnz != rhs.output_lp_nnz)
        return lhs.output_lp_nnz < rhs.output_lp_nnz;
    if (lhs.total_source_atoms != rhs.total_source_atoms)
        return lhs.total_source_atoms < rhs.total_source_atoms;
    const auto& lhs_pivot = candidate_pivot(lhs.candidate);
    const auto& rhs_pivot = candidate_pivot(rhs.candidate);
    if (lhs_pivot < rhs_pivot)
        return true;
    if (rhs_pivot < lhs_pivot)
        return false;
    const auto lhs_members = candidate_members(lhs.candidate);
    const auto rhs_members = candidate_members(rhs.candidate);
    if (members_less(lhs_members, rhs_members))
        return true;
    if (members_less(rhs_members, lhs_members))
        return false;
    return candidate_planner_rank(lhs.candidate) < candidate_planner_rank(rhs.candidate);
}

bool duplicate_payload_equal(const StructuredBatchCandidate& lhs,
                             const StructuredBatchCandidate& rhs) {
    if (lhs.index() != rhs.index())
        return false;
    if (const auto* lhs_two_way = std::get_if<TwoWayMergePlan>(&lhs)) {
        const auto& rhs_two_way = std::get<TwoWayMergePlan>(rhs);
        return lhs_two_way->generation == rhs_two_way.generation &&
               lhs_two_way->incidence_epoch == rhs_two_way.incidence_epoch &&
               lhs_two_way->members == rhs_two_way.members &&
               lhs_two_way->expected_sources == rhs_two_way.expected_sources &&
               lhs_two_way->expected_lp_keys == rhs_two_way.expected_lp_keys;
    }
    const auto& lhs_tree = std::get<TreeBasisMergePlan>(lhs);
    const auto& rhs_tree = std::get<TreeBasisMergePlan>(rhs);
    return lhs_tree.generation == rhs_tree.generation &&
           lhs_tree.incidence_epoch == rhs_tree.incidence_epoch &&
           lhs_tree.planner == rhs_tree.planner && lhs_tree.members == rhs_tree.members &&
           lhs_tree.edges == rhs_tree.edges &&
           lhs_tree.input_nonpivot_lp_nnz == rhs_tree.input_nonpivot_lp_nnz &&
           lhs_tree.output_lp_nnz == rhs_tree.output_lp_nnz &&
           lhs_tree.lp_fill_growth == rhs_tree.lp_fill_growth;
}

} // namespace

StructuredConflictFreeBatchPlan
select_conflict_free_batch(StructuredIncidenceSnapshotId snapshot, size_t total_row_count,
                           std::vector<StructuredBatchCandidate> candidates,
                           size_t max_batch_candidates) {
    if (snapshot.generation == 0 || snapshot.incidence_epoch == 0) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "conflict-free batch snapshot identity contains zero");
    }

    StructuredConflictFreeBatchPlan result;
    result.snapshot = snapshot;
    result.raw_candidate_count = candidates.size();

    std::vector<ValidatedCandidate> validated;
    if (candidates.size() > validated.max_size()) {
        fail(StructuredReductionErrorCode::ResourceLimit,
             "validated batch candidate vector exceeds capacity");
    }
    validated.reserve(candidates.size());
    for (auto& candidate : candidates) {
        validated.push_back(validate_candidate(std::move(candidate), snapshot, total_row_count));
    }

    std::sort(validated.begin(), validated.end(),
              [](const ValidatedCandidate& lhs, const ValidatedCandidate& rhs) {
                  if (identity_less(lhs, rhs))
                      return true;
                  if (identity_less(rhs, lhs))
                      return false;
                  return global_candidate_less(lhs, rhs);
              });

    std::vector<ValidatedCandidate> unique;
    if (validated.size() > unique.max_size()) {
        fail(StructuredReductionErrorCode::ResourceLimit,
             "unique batch candidate vector exceeds capacity");
    }
    unique.reserve(validated.size());
    for (size_t begin = 0; begin < validated.size();) {
        size_t end = begin + 1;
        while (end < validated.size() && identity_equal(validated[begin], validated[end])) {
            if (!duplicate_payload_equal(validated[begin].candidate, validated[end].candidate)) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "equivalent batch candidates have inconsistent output payloads");
            }
            result.duplicate_candidate_count =
                checked_resource_add(result.duplicate_candidate_count, 1,
                                     "duplicate batch candidate statistics overflow");
            ++end;
        }
        unique.push_back(std::move(validated[begin]));
        begin = end;
    }

    std::sort(unique.begin(), unique.end(), global_candidate_less);

    if (unique.empty())
        return result;
    if (max_batch_candidates == 0) {
        result.capacity_deferred_count = unique.size();
        return result;
    }

    std::vector<uint8_t> claimed_rows;
    if (total_row_count > claimed_rows.max_size()) {
        fail(StructuredReductionErrorCode::ResourceLimit,
             "batch claimed-row bitmap exceeds capacity");
    }
    claimed_rows.resize(total_row_count, uint8_t{0});

    const size_t selected_capacity = std::min(max_batch_candidates, unique.size());
    if (selected_capacity > result.candidates.max_size()) {
        fail(StructuredReductionErrorCode::ResourceLimit,
             "selected batch candidate vector exceeds capacity");
    }
    result.candidates.reserve(selected_capacity);

    for (size_t i = 0; i < unique.size(); ++i) {
        if (result.candidates.size() == max_batch_candidates) {
            result.capacity_deferred_count = unique.size() - i;
            break;
        }

        const auto members = candidate_members(unique[i].candidate);
        bool conflicts = false;
        for (const StructuredRowId member : members) {
            const size_t row_index = static_cast<size_t>(member.value);
            if (claimed_rows[row_index] != 0) {
                conflicts = true;
                break;
            }
        }
        if (conflicts) {
            result.conflict_deferred_count =
                checked_resource_add(result.conflict_deferred_count, 1,
                                     "conflict-deferred batch candidate statistics overflow");
            continue;
        }

        std::array<size_t, 8> row_indices{};
        size_t member_count = 0;
        for (const StructuredRowId member : members)
            row_indices[member_count++] = static_cast<size_t>(member.value);

        result.candidates.push_back(std::move(unique[i].candidate));
        for (size_t member_index = 0; member_index < member_count; ++member_index)
            claimed_rows[row_indices[member_index]] = uint8_t{1};
    }
    return result;
}

StructuredConflictFreeBatchPlan plan_conflict_free_batch(const SequentialStructuredReducer& reducer,
                                                         size_t max_batch_candidates,
                                                         TreeBasisPlanner planner) {
    const StructuredIncidenceSnapshotId snapshot{reducer.corpus().generation(),
                                                 reducer.incidence_epoch()};
    auto two_way_plans = reducer.plan_two_way_merges();
    auto tree_plans = reducer.plan_tree_basis_merges(planner);
    const size_t candidate_count = checked_resource_add(two_way_plans.size(), tree_plans.size(),
                                                        "combined batch candidate count overflows");

    std::vector<StructuredBatchCandidate> candidates;
    if (candidate_count > candidates.max_size()) {
        fail(StructuredReductionErrorCode::ResourceLimit,
             "combined batch candidate vector exceeds capacity");
    }
    candidates.reserve(candidate_count);
    for (auto& plan : two_way_plans)
        candidates.emplace_back(std::move(plan));
    for (auto& plan : tree_plans)
        candidates.emplace_back(std::move(plan));

    return select_conflict_free_batch(snapshot, reducer.total_row_count(), std::move(candidates),
                                      max_batch_candidates);
}

} // namespace gnfs::relation
