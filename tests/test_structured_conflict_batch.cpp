#include "gnfs/relation/structured_batch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::plan_conflict_free_batch;
using gnfs::relation::select_conflict_free_batch;
using gnfs::relation::SequentialStructuredReducer;
using gnfs::relation::SourceCombination;
using gnfs::relation::StructuredBatchCandidate;
using gnfs::relation::StructuredConflictFreeBatchPlan;
using gnfs::relation::StructuredIncidenceSnapshotId;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredRowId;
using gnfs::relation::TreeBasisMergePlan;
using gnfs::relation::TreeBasisPlanner;
using gnfs::relation::TwoWayMergePlan;

namespace {

int checks = 0;
int failures = 0;
std::string_view current_test;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::cerr << "CHECK failed in " << current_test << " at " << __FILE__ << ':'           \
                      << __LINE__ << ": " << #condition << '\n';                                   \
        }                                                                                          \
    } while (false)

[[nodiscard]] constexpr LargePrimeKey rational_key(uint64_t prime) noexcept {
    return LargePrimeKey{prime, 0, false};
}

[[nodiscard]] Relation make_relation(int64_t a, std::initializer_list<LargePrimeKey> lp_keys) {
    Relation relation(a, 1);
    for (const auto& key : lp_keys) {
        relation.rational_large_prime.emplace_back(key.prime, uint8_t{1});
    }
    return relation;
}

[[nodiscard]] bool relation_equal(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

[[nodiscard]] StructuredIncidenceSnapshotId
snapshot_id(const SequentialStructuredReducer& reducer) {
    return StructuredIncidenceSnapshotId{reducer.corpus().generation(), reducer.incidence_epoch()};
}

[[nodiscard]] std::vector<StructuredBatchCandidate>
all_candidates(const SequentialStructuredReducer& reducer) {
    const auto two_way = reducer.plan_two_way_merges();
    const auto trees = reducer.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
    std::vector<StructuredBatchCandidate> result;
    result.reserve(two_way.size() + trees.size());
    for (const auto& plan : two_way) {
        result.emplace_back(plan);
    }
    for (const auto& plan : trees) {
        result.emplace_back(plan);
    }
    return result;
}

enum class CandidateKind : uint8_t {
    TwoWay,
    Tree,
};

struct CandidateIdentity final {
    CandidateKind kind = CandidateKind::TwoWay;
    LargePrimeKey key{};
    std::vector<uint64_t> members;

    [[nodiscard]] bool operator==(const CandidateIdentity&) const noexcept = default;
};

[[nodiscard]] CandidateIdentity identity(const StructuredBatchCandidate& candidate) {
    CandidateIdentity result;
    if (const auto* two_way = std::get_if<TwoWayMergePlan>(&candidate)) {
        result.kind = CandidateKind::TwoWay;
        result.key = two_way->witness;
        for (const auto member : two_way->members) {
            result.members.push_back(member.value);
        }
    } else {
        const auto& tree = std::get<TreeBasisMergePlan>(candidate);
        result.kind = CandidateKind::Tree;
        result.key = tree.pivot;
        for (const auto member : tree.members) {
            result.members.push_back(member.value);
        }
    }
    return result;
}

[[nodiscard]] CandidateIdentity expected_identity(CandidateKind kind, uint64_t prime,
                                                  std::initializer_list<uint64_t> members) {
    return CandidateIdentity{kind, rational_key(prime), std::vector<uint64_t>(members)};
}

[[nodiscard]] std::vector<CandidateIdentity>
identities(const std::vector<StructuredBatchCandidate>& candidates) {
    std::vector<CandidateIdentity> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        result.push_back(identity(candidate));
    }
    return result;
}

[[nodiscard]] bool local_key_less(const LargePrimeKey& lhs, const LargePrimeKey& rhs) noexcept {
    if (lhs.prime != rhs.prime) {
        return lhs.prime < rhs.prime;
    }
    if (lhs.root != rhs.root) {
        return lhs.root < rhs.root;
    }
    return lhs.is_algebraic < rhs.is_algebraic;
}

[[nodiscard]] size_t local_output_lp_nnz(const StructuredBatchCandidate& candidate) {
    if (const auto* two_way = std::get_if<TwoWayMergePlan>(&candidate)) {
        return two_way->expected_lp_keys.size();
    }
    return std::get<TreeBasisMergePlan>(candidate).output_lp_nnz;
}

[[nodiscard]] size_t local_source_nnz(const StructuredBatchCandidate& candidate) {
    if (const auto* two_way = std::get_if<TwoWayMergePlan>(&candidate)) {
        return two_way->expected_sources.size();
    }
    size_t result = 0;
    for (const auto& edge : std::get<TreeBasisMergePlan>(candidate).edges) {
        result += edge.expected_sources.size();
    }
    return result;
}

[[nodiscard]] int local_planner_rank(const StructuredBatchCandidate& candidate) {
    const auto* tree = std::get_if<TreeBasisMergePlan>(&candidate);
    if (tree == nullptr) {
        return 0;
    }
    return tree->planner == TreeBasisPlanner::ReferenceStar ? 0 : 1;
}

[[nodiscard]] bool local_candidate_less(const StructuredBatchCandidate& lhs,
                                        const StructuredBatchCandidate& rhs) {
    const bool lhs_tree = std::holds_alternative<TreeBasisMergePlan>(lhs);
    const bool rhs_tree = std::holds_alternative<TreeBasisMergePlan>(rhs);
    if (lhs_tree != rhs_tree) {
        return !lhs_tree;
    }
    if (local_output_lp_nnz(lhs) != local_output_lp_nnz(rhs)) {
        return local_output_lp_nnz(lhs) < local_output_lp_nnz(rhs);
    }
    if (local_source_nnz(lhs) != local_source_nnz(rhs)) {
        return local_source_nnz(lhs) < local_source_nnz(rhs);
    }
    const auto lhs_identity = identity(lhs);
    const auto rhs_identity = identity(rhs);
    if (local_key_less(lhs_identity.key, rhs_identity.key)) {
        return true;
    }
    if (local_key_less(rhs_identity.key, lhs_identity.key)) {
        return false;
    }
    if (lhs_identity.members != rhs_identity.members) {
        return std::lexicographical_compare(
            lhs_identity.members.begin(), lhs_identity.members.end(), rhs_identity.members.begin(),
            rhs_identity.members.end());
    }
    return local_planner_rank(lhs) < local_planner_rank(rhs);
}

[[nodiscard]] bool local_conflicts(const StructuredBatchCandidate& candidate,
                                   const std::vector<bool>& claimed) {
    for (const uint64_t member : identity(candidate).members) {
        if (claimed[static_cast<size_t>(member)]) {
            return true;
        }
    }
    return false;
}

struct LocalSelection final {
    std::vector<StructuredBatchCandidate> candidates;
    size_t conflict_count = 0;
    size_t capacity_count = 0;
};

[[nodiscard]] LocalSelection local_select(std::vector<StructuredBatchCandidate> candidates,
                                          size_t row_count, size_t width) {
    std::sort(candidates.begin(), candidates.end(), local_candidate_less);
    LocalSelection result;
    std::vector<bool> claimed(row_count, false);
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (result.candidates.size() == width) {
            result.capacity_count = candidates.size() - index;
            break;
        }
        const auto& candidate = candidates[index];
        if (local_conflicts(candidate, claimed)) {
            ++result.conflict_count;
            continue;
        }
        result.candidates.push_back(candidate);
        for (const uint64_t member : identity(candidate).members) {
            claimed[static_cast<size_t>(member)] = true;
        }
    }
    return result;
}

template <typename Action>
void expect_error(StructuredReductionErrorCode expected, Action&& action) {
    bool caught = false;
    try {
        std::forward<Action>(action)();
    } catch (const StructuredReductionError& error) {
        caught = true;
        CHECK(error.code() == expected);
        if (error.code() != expected) {
            std::cerr << "unexpected structured error code: expected=" << static_cast<int>(expected)
                      << " actual=" << static_cast<int>(error.code()) << " message=" << error.what()
                      << '\n';
        }
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

[[nodiscard]] std::vector<Relation> main_fixture(bool include_singleton = false) {
    const auto y = rational_key(101);
    const auto x = rational_key(103);
    const auto z = rational_key(107);
    const auto u = rational_key(109);
    const auto t = rational_key(113);
    std::vector<Relation> relations{
        make_relation(10, {x, t}), make_relation(11, {x, y}), make_relation(12, {y, u}),
        make_relation(13, {z}),    make_relation(14, {z}),    make_relation(15, {t}),
        make_relation(16, {t}),    make_relation(17, {u}),    make_relation(18, {u}),
    };
    if (include_singleton) {
        relations.push_back(make_relation(19, {rational_key(127)}));
    }
    return relations;
}

[[nodiscard]] std::vector<CandidateIdentity> main_raw_identities() {
    return {
        expected_identity(CandidateKind::TwoWay, 107, {3, 4}),
        expected_identity(CandidateKind::TwoWay, 101, {1, 2}),
        expected_identity(CandidateKind::TwoWay, 103, {0, 1}),
        expected_identity(CandidateKind::Tree, 109, {2, 7, 8}),
        expected_identity(CandidateKind::Tree, 113, {0, 5, 6}),
    };
}

[[nodiscard]] std::vector<CandidateIdentity> main_selected_identities() {
    return {
        expected_identity(CandidateKind::TwoWay, 107, {3, 4}),
        expected_identity(CandidateKind::TwoWay, 101, {1, 2}),
        expected_identity(CandidateKind::Tree, 113, {0, 5, 6}),
    };
}

[[nodiscard]] std::vector<StructuredBatchCandidate>
expected_main_selection(const std::vector<StructuredBatchCandidate>& raw, size_t width) {
    if (width == 0) {
        return {};
    }
    if (width == 1) {
        return {raw[0]};
    }
    if (width == 2) {
        return {raw[0], raw[1]};
    }
    return {raw[0], raw[1], raw[4]};
}

void test_main_fixture_permutations_and_widths() {
    SequentialStructuredReducer reducer(71, main_fixture());
    const auto snapshot = snapshot_id(reducer);
    const auto raw = all_candidates(reducer);
    CHECK(raw.size() == 5);
    if (raw.size() != 5) {
        return;
    }
    CHECK(identities(raw) == main_raw_identities());

    const auto local = local_select(raw, reducer.total_row_count(), 3);
    CHECK(identities(local.candidates) == main_selected_identities());
    CHECK(local.conflict_count == 2);
    CHECK(local.capacity_count == 0);

    StructuredConflictFreeBatchPlan expected;
    expected.snapshot = snapshot;
    expected.candidates = expected_main_selection(raw, 3);
    expected.raw_candidate_count = 5;
    expected.duplicate_candidate_count = 0;
    expected.conflict_deferred_count = 2;
    expected.capacity_deferred_count = 0;

    std::array<size_t, 5> order{0, 1, 2, 3, 4};
    size_t permutation_count = 0;
    do {
        std::vector<StructuredBatchCandidate> permuted;
        permuted.reserve(order.size());
        for (const size_t index : order) {
            permuted.push_back(raw[index]);
        }
        const auto selected =
            select_conflict_free_batch(snapshot, reducer.total_row_count(), std::move(permuted), 3);
        CHECK(selected == expected);
        ++permutation_count;
    } while (std::next_permutation(order.begin(), order.end()));
    CHECK(permutation_count == 120);
    CHECK(plan_conflict_free_batch(reducer, 3) == expected);

    struct WidthExpectation final {
        size_t width;
        size_t selected;
        size_t conflicts;
        size_t capacity;
    };
    constexpr std::array expectations{
        WidthExpectation{0, 0, 0, 5}, WidthExpectation{1, 1, 0, 4}, WidthExpectation{2, 2, 0, 3},
        WidthExpectation{3, 3, 2, 0}, WidthExpectation{4, 3, 2, 0},
    };
    for (const auto expectation : expectations) {
        const auto selected =
            select_conflict_free_batch(snapshot, reducer.total_row_count(), raw, expectation.width);
        CHECK(selected.snapshot == snapshot);
        CHECK(selected.candidates == expected_main_selection(raw, expectation.width));
        CHECK(selected.candidates.size() == expectation.selected);
        CHECK(selected.raw_candidate_count == 5);
        CHECK(selected.duplicate_candidate_count == 0);
        CHECK(selected.conflict_deferred_count == expectation.conflicts);
        CHECK(selected.capacity_deferred_count == expectation.capacity);

        const auto oracle = local_select(raw, reducer.total_row_count(), expectation.width);
        CHECK(oracle.candidates == selected.candidates);
        CHECK(oracle.conflict_count == selected.conflict_deferred_count);
        CHECK(oracle.capacity_count == selected.capacity_deferred_count);
    }

    const auto selected = select_conflict_free_batch(snapshot, reducer.total_row_count(), raw, 3);
    CHECK(selected.candidates.size() == 3);
    if (selected.candidates.size() == 3) {
        const auto* tree = std::get_if<TreeBasisMergePlan>(&selected.candidates[2]);
        CHECK(tree != nullptr);
        if (tree != nullptr) {
            CHECK(tree->edges.size() == 2);
            size_t emitted_rows = 0;
            for (const auto& candidate : selected.candidates) {
                const auto* selected_tree = std::get_if<TreeBasisMergePlan>(&candidate);
                emitted_rows += selected_tree == nullptr ? 1 : selected_tree->edges.size();
            }
            CHECK(emitted_rows == 4);
        }
    }
}

void test_ordered_greedy_is_maximal_not_maximum() {
    const auto a = rational_key(211);
    const auto b = rational_key(223);
    const auto c = rational_key(227);
    std::vector<Relation> relations{
        make_relation(20, {a, b}), make_relation(21, {a, c}), make_relation(22, {b}),
        make_relation(23, {b}),    make_relation(24, {c}),    make_relation(25, {c}),
    };
    SequentialStructuredReducer reducer(72, std::move(relations));
    const auto raw = all_candidates(reducer);
    CHECK(raw.size() == 3);
    if (raw.size() != 3) {
        return;
    }
    const std::vector<CandidateIdentity> expected_raw{
        expected_identity(CandidateKind::TwoWay, 211, {0, 1}),
        expected_identity(CandidateKind::Tree, 223, {0, 2, 3}),
        expected_identity(CandidateKind::Tree, 227, {1, 4, 5}),
    };
    CHECK(identities(raw) == expected_raw);

    const auto b_members = identity(raw[1]).members;
    const auto c_members = identity(raw[2]).members;
    bool b_c_conflict = false;
    for (const uint64_t member : b_members) {
        b_c_conflict = b_c_conflict ||
                       std::find(c_members.begin(), c_members.end(), member) != c_members.end();
    }
    CHECK(!b_c_conflict);

    const auto selected =
        select_conflict_free_batch(snapshot_id(reducer), reducer.total_row_count(), raw, 3);
    CHECK(selected.candidates == std::vector<StructuredBatchCandidate>{raw[0]});
    CHECK(identities(selected.candidates) ==
          std::vector<CandidateIdentity>{expected_identity(CandidateKind::TwoWay, 211, {0, 1})});
    CHECK(selected.raw_candidate_count == 3);
    CHECK(selected.duplicate_candidate_count == 0);
    CHECK(selected.conflict_deferred_count == 2);
    CHECK(selected.capacity_deferred_count == 0);

    const auto oracle = local_select(raw, reducer.total_row_count(), 3);
    CHECK(oracle.candidates == selected.candidates);
    CHECK(oracle.conflict_count == 2);
    CHECK(oracle.capacity_count == 0);
}

void test_two_way_duplicate_identity_keeps_smallest_witness() {
    const auto p = rational_key(307);
    const auto q = rational_key(311);
    SequentialStructuredReducer reducer(73, {make_relation(30, {p, q}), make_relation(31, {p, q})});
    const auto plans = reducer.plan_two_way_merges();
    CHECK(plans.size() == 2);
    if (plans.size() != 2) {
        return;
    }
    CHECK(plans[0].witness == p);
    CHECK(plans[1].witness == q);
    CHECK(plans[0].members == plans[1].members);
    CHECK(plans[0].expected_sources == plans[1].expected_sources);
    CHECK(plans[0].expected_lp_keys == plans[1].expected_lp_keys);

    std::vector<StructuredBatchCandidate> reversed{plans[1], plans[0]};
    const auto selected =
        select_conflict_free_batch(snapshot_id(reducer), reducer.total_row_count(), reversed, 2);
    CHECK(selected.candidates == std::vector<StructuredBatchCandidate>{plans[0]});
    CHECK(selected.raw_candidate_count == 2);
    CHECK(selected.duplicate_candidate_count == 1);
    CHECK(selected.conflict_deferred_count == 0);
    CHECK(selected.capacity_deferred_count == 0);
}

void test_tree_duplicate_identity_keeps_smallest_pivot() {
    const auto p = rational_key(401);
    const auto q = rational_key(409);
    SequentialStructuredReducer reducer(
        74, {make_relation(40, {p, q}), make_relation(41, {p, q}), make_relation(42, {p, q})});
    const auto plans = reducer.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
    CHECK(plans.size() == 2);
    if (plans.size() != 2) {
        return;
    }
    CHECK(plans[0].pivot == p);
    CHECK(plans[1].pivot == q);
    CHECK(plans[0].planner == plans[1].planner);
    CHECK(plans[0].members == plans[1].members);
    CHECK(plans[0].edges == plans[1].edges);
    CHECK(plans[0].input_nonpivot_lp_nnz == plans[1].input_nonpivot_lp_nnz);
    CHECK(plans[0].output_lp_nnz == plans[1].output_lp_nnz);
    CHECK(plans[0].lp_fill_growth == plans[1].lp_fill_growth);

    std::vector<StructuredBatchCandidate> reversed{plans[1], plans[0]};
    const auto selected =
        select_conflict_free_batch(snapshot_id(reducer), reducer.total_row_count(), reversed, 2);
    CHECK(selected.candidates == std::vector<StructuredBatchCandidate>{plans[0]});
    CHECK(selected.raw_candidate_count == 2);
    CHECK(selected.duplicate_candidate_count == 1);
    CHECK(selected.conflict_deferred_count == 0);
    CHECK(selected.capacity_deferred_count == 0);
}

struct RowState final {
    StructuredRowId row{};
    SourceCombination sources;
    std::vector<LargePrimeKey> lp_keys;
    Relation materialized;
};

struct ReducerState final {
    uint64_t epoch = 0;
    size_t total_rows = 0;
    size_t active_rows = 0;
    std::vector<RowState> rows;
};

[[nodiscard]] ReducerState capture_state(const SequentialStructuredReducer& reducer) {
    ReducerState state;
    state.epoch = reducer.incidence_epoch();
    state.total_rows = reducer.total_row_count();
    state.active_rows = reducer.active_row_count();
    for (const auto row : reducer.active_row_ids()) {
        const auto keys = reducer.lp_keys(row);
        state.rows.push_back(RowState{row, reducer.sources(row),
                                      std::vector<LargePrimeKey>(keys.begin(), keys.end()),
                                      reducer.materialize(row)});
    }
    return state;
}

[[nodiscard]] bool state_equal(const ReducerState& lhs, const ReducerState& rhs) {
    if (lhs.epoch != rhs.epoch || lhs.total_rows != rhs.total_rows ||
        lhs.active_rows != rhs.active_rows || lhs.rows.size() != rhs.rows.size()) {
        return false;
    }
    for (size_t index = 0; index < lhs.rows.size(); ++index) {
        const auto& left = lhs.rows[index];
        const auto& right = rhs.rows[index];
        if (left.row != right.row || left.sources != right.sources ||
            left.lp_keys != right.lp_keys ||
            !relation_equal(left.materialized, right.materialized)) {
            return false;
        }
    }
    return true;
}

void test_stale_candidates_after_epoch_advance_fail_closed() {
    SequentialStructuredReducer reducer(75, main_fixture(true));
    const auto old_snapshot = snapshot_id(reducer);
    const auto old_candidates = all_candidates(reducer);
    CHECK(old_candidates.size() == 5);
    CHECK(reducer.peel_singletons() == 1);
    CHECK(reducer.incidence_epoch() == old_snapshot.incidence_epoch + 1);
    const auto current_snapshot = snapshot_id(reducer);
    const auto before = capture_state(reducer);

    expect_error(StructuredReductionErrorCode::StalePlan, [&] {
        (void)select_conflict_free_batch(current_snapshot, reducer.total_row_count(),
                                         old_candidates, 3);
    });
    CHECK(state_equal(capture_state(reducer), before));
}

void test_malformed_candidates_fail_closed() {
    const auto p = rational_key(503);
    const auto q = rational_key(509);
    SequentialStructuredReducer reducer(76, {make_relation(50, {p, q}), make_relation(51, {p, q})});
    const auto plans = reducer.plan_two_way_merges();
    CHECK(plans.size() == 2);
    if (plans.size() != 2) {
        return;
    }
    const auto snapshot = snapshot_id(reducer);

    auto zero_snapshot = snapshot;
    zero_snapshot.generation = 0;
    expect_error(StructuredReductionErrorCode::InvalidGeneration, [&] {
        (void)select_conflict_free_batch(zero_snapshot, reducer.total_row_count(),
                                         std::vector<StructuredBatchCandidate>{plans[0]}, 1);
    });

    TwoWayMergePlan wrong_generation = plans[0];
    ++wrong_generation.generation;
    expect_error(StructuredReductionErrorCode::InvalidPlan, [&] {
        (void)select_conflict_free_batch(snapshot, reducer.total_row_count(),
                                         std::vector<StructuredBatchCandidate>{wrong_generation},
                                         1);
    });

    TwoWayMergePlan out_of_range = plans[0];
    out_of_range.members[1] = StructuredRowId{reducer.total_row_count()};
    expect_error(StructuredReductionErrorCode::InvalidPlan, [&] {
        (void)select_conflict_free_batch(snapshot, reducer.total_row_count(),
                                         std::vector<StructuredBatchCandidate>{out_of_range}, 1);
    });

    TwoWayMergePlan descending = plans[0];
    std::swap(descending.members[0], descending.members[1]);
    expect_error(StructuredReductionErrorCode::InvalidPlan, [&] {
        (void)select_conflict_free_batch(snapshot, reducer.total_row_count(),
                                         std::vector<StructuredBatchCandidate>{descending}, 1);
    });

    TwoWayMergePlan duplicate_member = plans[0];
    duplicate_member.members[1] = duplicate_member.members[0];
    expect_error(StructuredReductionErrorCode::InvalidPlan, [&] {
        (void)select_conflict_free_batch(snapshot, reducer.total_row_count(),
                                         std::vector<StructuredBatchCandidate>{duplicate_member},
                                         1);
    });

    TwoWayMergePlan inconsistent_payload = plans[1];
    inconsistent_payload.expected_lp_keys.push_back(rational_key(997));
    expect_error(StructuredReductionErrorCode::InvariantViolation, [&] {
        (void)select_conflict_free_batch(
            snapshot, reducer.total_row_count(),
            std::vector<StructuredBatchCandidate>{plans[0], inconsistent_payload}, 2);
    });
}

template <typename Action> void run_test(std::string_view name, Action&& action) {
    current_test = name;
    try {
        std::forward<Action>(action)();
    } catch (const std::exception& error) {
        ++checks;
        ++failures;
        std::cerr << "unexpected exception in " << current_test << ": " << error.what() << '\n';
    } catch (...) {
        ++checks;
        ++failures;
        std::cerr << "unexpected non-standard exception in " << current_test << '\n';
    }
}

} // namespace

int main() {
    run_test("main fixture permutations and widths", test_main_fixture_permutations_and_widths);
    run_test("ordered greedy maximal not maximum", test_ordered_greedy_is_maximal_not_maximum);
    run_test("two-way duplicate identity", test_two_way_duplicate_identity_keeps_smallest_witness);
    run_test("tree duplicate identity", test_tree_duplicate_identity_keeps_smallest_pivot);
    run_test("stale epoch", test_stale_candidates_after_epoch_advance_fail_closed);
    run_test("malformed candidates", test_malformed_candidates_fail_closed);

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " checks failed\n";
        return 1;
    }
    std::cout << "structured conflict batch: " << checks << " checks passed\n";
    return 0;
}
