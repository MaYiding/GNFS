#include "gnfs/relation/structured_batch.hpp"
#include "gnfs/relation/structured_reduction.hpp"

#if !defined(GNFS_STRUCTURED_REDUCTION_TEST_HOOKS)
#error "test_structured_parallel_failures requires GNFS_STRUCTURED_REDUCTION_TEST_HOOKS"
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::plan_conflict_free_batch;
using gnfs::relation::prepare_conflict_free_batch;
using gnfs::relation::SequentialStructuredReducer;
using gnfs::relation::SourceCombination;
using gnfs::relation::StructuredParallelReductionOptions;
using gnfs::relation::StructuredReductionBudget;
using gnfs::relation::StructuredReductionRunResult;
using gnfs::relation::StructuredReductionStats;
using gnfs::relation::StructuredReductionStopReason;
using gnfs::relation::StructuredRowId;
using gnfs::relation::structured_reduction_testing::commit_prepared_batch_with_hook;
using gnfs::relation::structured_reduction_testing::Event;
using gnfs::relation::structured_reduction_testing::Hook;
using gnfs::relation::structured_reduction_testing::no_slot;
using gnfs::relation::structured_reduction_testing::reduce_budgeted_parallel_with_hook;

namespace {

size_t checks = 0;
size_t failures = 0;
std::string_view current_test;

[[noreturn]] void check_failed(const char* expression, int line) {
    throw std::runtime_error(std::string("CHECK failed in ") + std::string(current_test) +
                             " at line " + std::to_string(line) + ": " + expression);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition))                                                                          \
            check_failed(#condition, __LINE__);                                                    \
    } while (false)

[[nodiscard]] constexpr LargePrimeKey rational_key(uint64_t prime) noexcept {
    return LargePrimeKey{prime, 0, false};
}

[[nodiscard]] Relation make_relation(int64_t a, std::initializer_list<LargePrimeKey> lp_keys) {
    Relation relation(a, 1);
    for (const auto& key : lp_keys)
        relation.rational_large_prime.emplace_back(key.prime, uint8_t{1});
    return relation;
}

[[nodiscard]] Relation persistence_heavy_relation(int64_t a, const LargePrimeKey& pivot) {
    Relation relation(a, 1);
    for (size_t i = 0; i < 9; ++i)
        relation.rational_large_prime.emplace_back(pivot.prime, 0, uint8_t{255});
    return relation;
}

[[nodiscard]] bool relation_equal(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

[[nodiscard]] bool relations_equal(std::span<const Relation> lhs, std::span<const Relation> rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!relation_equal(lhs[i], rhs[i]))
            return false;
    }
    return true;
}

[[nodiscard]] bool stats_equal(const StructuredReductionStats& lhs,
                               const StructuredReductionStats& rhs) noexcept {
    return lhs.input_rows == rhs.input_rows &&
           lhs.singleton_rows_removed == rhs.singleton_rows_removed &&
           lhs.two_way_merges == rhs.two_way_merges &&
           lhs.tree_basis_batches == rhs.tree_basis_batches &&
           lhs.tree_basis_rows_consumed == rhs.tree_basis_rows_consumed &&
           lhs.tree_basis_rows_emitted == rhs.tree_basis_rows_emitted &&
           lhs.persistence_limited_plans == rhs.persistence_limited_plans &&
           lhs.persistence_cache_hits == rhs.persistence_cache_hits &&
           lhs.budgeted_runs == rhs.budgeted_runs && lhs.planning_passes == rhs.planning_passes &&
           lhs.candidate_plans_considered == rhs.candidate_plans_considered &&
           lhs.budget_limited_plans == rhs.budget_limited_plans &&
           lhs.candidate_limit_stops == rhs.candidate_limit_stops &&
           lhs.commit_limit_stops == rhs.commit_limit_stops &&
           lhs.budget_limit_stops == rhs.budget_limit_stops &&
           lhs.peak_prepared_payload_entries == rhs.peak_prepared_payload_entries &&
           lhs.accepted_lp_fill_growth == rhs.accepted_lp_fill_growth &&
           lhs.budget_rejections.pivot_weight_limit == rhs.budget_rejections.pivot_weight_limit &&
           lhs.budget_rejections.source_limit == rhs.budget_rejections.source_limit &&
           lhs.budget_rejections.output_lp_limit == rhs.budget_rejections.output_lp_limit &&
           lhs.budget_rejections.fill_limit == rhs.budget_rejections.fill_limit &&
           lhs.budget_rejections.emitted_row_limit == rhs.budget_rejections.emitted_row_limit &&
           lhs.budget_rejections.materialization_limit ==
               rhs.budget_rejections.materialization_limit &&
           lhs.output_rows == rhs.output_rows && lhs.stop_reason == rhs.stop_reason;
}

struct RowSnapshot final {
    StructuredRowId id{};
    SourceCombination sources;
    std::vector<LargePrimeKey> lp_keys;
    Relation materialized;
};

struct ReducerSnapshot final {
    uint64_t generation = 0;
    uint64_t incidence_epoch = 0;
    size_t total_rows = 0;
    size_t active_rows = 0;
    std::vector<RowSnapshot> rows;
    std::vector<Relation> materialized_active;
    StructuredReductionStats stats;
};

[[nodiscard]] ReducerSnapshot capture_state(const SequentialStructuredReducer& reducer) {
    ReducerSnapshot result;
    result.generation = reducer.corpus().generation();
    result.incidence_epoch = reducer.incidence_epoch();
    result.total_rows = reducer.total_row_count();
    result.active_rows = reducer.active_row_count();
    for (const StructuredRowId id : reducer.active_row_ids()) {
        const auto keys = reducer.lp_keys(id);
        result.rows.push_back(RowSnapshot{id, reducer.sources(id),
                                          std::vector<LargePrimeKey>(keys.begin(), keys.end()),
                                          reducer.materialize(id)});
    }
    result.materialized_active = reducer.materialize_active();
    result.stats = reducer.stats();
    return result;
}

[[nodiscard]] bool logical_equal(const ReducerSnapshot& lhs, const ReducerSnapshot& rhs) {
    if (lhs.generation != rhs.generation || lhs.incidence_epoch != rhs.incidence_epoch ||
        lhs.total_rows != rhs.total_rows || lhs.active_rows != rhs.active_rows ||
        lhs.rows.size() != rhs.rows.size() ||
        !relations_equal(lhs.materialized_active, rhs.materialized_active)) {
        return false;
    }
    for (size_t i = 0; i < lhs.rows.size(); ++i) {
        const auto& left = lhs.rows[i];
        const auto& right = rhs.rows[i];
        if (left.id != right.id || left.sources != right.sources || left.lp_keys != right.lp_keys ||
            !relation_equal(left.materialized, right.materialized)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool state_equal(const ReducerSnapshot& lhs, const ReducerSnapshot& rhs) {
    return logical_equal(lhs, rhs) && stats_equal(lhs.stats, rhs.stats);
}

[[nodiscard]] StructuredReductionBudget generous_budget() {
    StructuredReductionBudget budget(64, 64, 64, 1024);
    budget.max_commits = 64;
    budget.max_source_atoms_per_output = 16;
    budget.max_odd_lp_keys_per_output = 32;
    budget.max_materialized_pairs_per_output = 32;
    budget.max_factor_entries_per_side = 32;
    return budget;
}

[[nodiscard]] StructuredParallelReductionOptions parallel_options(size_t width, uint32_t workers) {
    StructuredParallelReductionOptions options;
    options.max_batch_candidates = width;
    options.worker_count = workers;
    return options;
}

class InjectedFailure final : public std::runtime_error {
public:
    InjectedFailure(Event event, size_t slot)
        : std::runtime_error("injected structured-reduction failure"), event(event), slot(slot) {}

    Event event;
    size_t slot;
};

struct FailureControl final {
    Event failure_event = Event::ParallelPrepareSlotStarted;
    size_t failure_slot = no_slot;
    bool secondary_failure_enabled = false;
    Event secondary_failure_event = Event::ParallelPrepareSlotCompleted;
    size_t secondary_failure_slot = no_slot;
    std::atomic<uint64_t> started_mask{0};
    std::atomic<uint64_t> completed_mask{0};
    std::atomic<size_t> prepublish_events{0};
    std::atomic<size_t> prepeel_events{0};
};

void failure_hook(Event event, size_t slot, void* raw_context) {
    auto& control = *static_cast<FailureControl*>(raw_context);
    switch (event) {
    case Event::ParallelPrepareSlotStarted:
        control.started_mask.fetch_or(uint64_t{1} << static_cast<unsigned>(slot),
                                      std::memory_order_relaxed);
        break;
    case Event::ParallelPrepareSlotCompleted:
        control.completed_mask.fetch_or(uint64_t{1} << static_cast<unsigned>(slot),
                                        std::memory_order_relaxed);
        break;
    case Event::MaskedBatchCommitBeforePublish:
        control.prepublish_events.fetch_add(1, std::memory_order_relaxed);
        break;
    case Event::ParallelDriverBeforePostCommitPeel:
        control.prepeel_events.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    if (event == control.failure_event &&
        (control.failure_slot == no_slot || control.failure_slot == slot)) {
        throw InjectedFailure(event, slot);
    }
    if (control.secondary_failure_enabled && event == control.secondary_failure_event &&
        (control.secondary_failure_slot == no_slot || control.secondary_failure_slot == slot)) {
        throw InjectedFailure(event, slot);
    }
}

template <typename Action> void expect_injected_failure(Event event, size_t slot, Action&& action) {
    bool caught = false;
    try {
        std::forward<Action>(action)();
    } catch (const InjectedFailure& failure) {
        caught = true;
        CHECK(failure.event == event);
        CHECK(failure.slot == slot);
    }
    CHECK(caught);
}

[[nodiscard]] std::vector<Relation> prepare_failure_fixture() {
    const auto ready_first = rational_key(601);
    const auto persistence = rational_key(607);
    const auto ready_tail = rational_key(613);
    return {
        make_relation(10, {ready_first}),
        make_relation(11, {ready_first}),
        persistence_heavy_relation(12, persistence),
        persistence_heavy_relation(13, persistence),
        make_relation(14, {ready_tail}),
        make_relation(15, {ready_tail}),
    };
}

void test_lowest_prepare_failure_drains_tail_and_discards_round_state() {
    SequentialStructuredReducer reducer(51'001, prepare_failure_fixture());
    const ReducerSnapshot before = capture_state(reducer);
    FailureControl control;
    control.failure_event = Event::ParallelPrepareSlotStarted;
    control.failure_slot = 0;
    control.secondary_failure_enabled = true;
    control.secondary_failure_event = Event::ParallelPrepareSlotCompleted;
    control.secondary_failure_slot = 2;
    const Hook hook{failure_hook, &control};

    // Slot 2 also fails after completing its materialization. The ordered
    // barrier must drain that tail failure and still rethrow slot 0.
    expect_injected_failure(Event::ParallelPrepareSlotStarted, 0, [&] {
        (void)reduce_budgeted_parallel_with_hook(reducer, generous_budget(), parallel_options(3, 3),
                                                 hook);
    });

    const ReducerSnapshot after = capture_state(reducer);
    CHECK(logical_equal(after, before));
    StructuredReductionStats expected_stats = before.stats;
    ++expected_stats.budgeted_runs;
    expected_stats.stop_reason = StructuredReductionStopReason::NotStarted;
    CHECK(stats_equal(after.stats, expected_stats));
    CHECK(control.started_mask.load(std::memory_order_relaxed) == uint64_t{0b111});
    CHECK(control.completed_mask.load(std::memory_order_relaxed) == uint64_t{0b110});

    // A clean retry must discover the drained persistence outcome afresh. If
    // the failed round had leaked its staged cache, this would report cache
    // hits without recording the intrinsic persistence failure.
    const auto retry = reducer.reduce_budgeted_parallel(generous_budget(), parallel_options(3, 3));
    CHECK((retry == StructuredReductionRunResult{0, 2, 2, 0,
                                                 StructuredReductionStopReason::PersistenceLimit}));
    CHECK(reducer.stats().persistence_limited_plans == 1);
    CHECK(reducer.stats().persistence_cache_hits == 1);
    CHECK(reducer.stats().candidate_plans_considered == 3);
    CHECK(reducer.stats().planning_passes == 2);
}

void test_masked_commit_prepublication_failure_preserves_complete_snapshot() {
    const auto ready = rational_key(701);
    const auto persistence = rational_key(709);
    SequentialStructuredReducer reducer(51'002,
                                        {make_relation(20, {ready}), make_relation(21, {ready}),
                                         persistence_heavy_relation(22, persistence),
                                         persistence_heavy_relation(23, persistence)});

    auto batch = plan_conflict_free_batch(reducer, 2);
    auto prepared = prepare_conflict_free_batch(reducer, batch, 2);
    CHECK(prepared.prepared_candidate_count() == 1);
    CHECK(prepared.persistence_limited_candidate_count() == 1);
    const ReducerSnapshot before = capture_state(reducer);

    FailureControl control;
    control.failure_event = Event::MaskedBatchCommitBeforePublish;
    const Hook hook{failure_hook, &control};
    expect_injected_failure(Event::MaskedBatchCommitBeforePublish, no_slot, [&] {
        (void)commit_prepared_batch_with_hook(reducer, std::move(prepared), hook);
    });

    CHECK(state_equal(capture_state(reducer), before));
    CHECK(control.prepublish_events.load(std::memory_order_relaxed) == 1);

    batch = plan_conflict_free_batch(reducer, 2);
    prepared = prepare_conflict_free_batch(reducer, batch, 2);
    const auto committed = reducer.commit(std::move(prepared));
    CHECK(committed.committed_candidates == 1);
    CHECK(committed.persistence_limited_candidates == 1);
    CHECK(committed.emitted_rows == 1);
    CHECK(reducer.incidence_epoch() == before.incidence_epoch + 1);
}

void test_driver_prepublication_failure_discards_staged_round_state() {
    SequentialStructuredReducer reducer(51'004, prepare_failure_fixture());
    const ReducerSnapshot before = capture_state(reducer);
    FailureControl control;
    control.failure_event = Event::MaskedBatchCommitBeforePublish;
    const Hook hook{failure_hook, &control};

    expect_injected_failure(Event::MaskedBatchCommitBeforePublish, no_slot, [&] {
        (void)reduce_budgeted_parallel_with_hook(reducer, generous_budget(), parallel_options(3, 3),
                                                 hook);
    });

    const ReducerSnapshot after = capture_state(reducer);
    CHECK(logical_equal(after, before));
    StructuredReductionStats expected_stats = before.stats;
    ++expected_stats.budgeted_runs;
    expected_stats.stop_reason = StructuredReductionStopReason::NotStarted;
    CHECK(stats_equal(after.stats, expected_stats));
    CHECK(control.prepublish_events.load(std::memory_order_relaxed) == 1);

    // The failed scheduler had already staged a persistence marker and round
    // statistics. A clean retry must rediscover them rather than observe a
    // leaked cache entry or counter delta.
    const auto retry = reducer.reduce_budgeted_parallel(generous_budget(), parallel_options(3, 3));
    CHECK((retry == StructuredReductionRunResult{0, 2, 2, 0,
                                                 StructuredReductionStopReason::PersistenceLimit}));
    CHECK(reducer.stats().budgeted_runs == 2);
    CHECK(reducer.stats().persistence_limited_plans == 1);
    CHECK(reducer.stats().persistence_cache_hits == 1);
    CHECK(reducer.stats().candidate_plans_considered == 3);
    CHECK(reducer.stats().planning_passes == 2);
}

void test_postpublication_peel_failure_retains_commit_granule() {
    const auto p = rational_key(809);
    const auto q = rational_key(811);
    SequentialStructuredReducer reducer(
        51'003, {make_relation(30, {p, q}), make_relation(31, {p, q}), make_relation(32, {q})});
    const ReducerSnapshot before = capture_state(reducer);

    FailureControl control;
    control.failure_event = Event::ParallelDriverBeforePostCommitPeel;
    const Hook hook{failure_hook, &control};
    expect_injected_failure(Event::ParallelDriverBeforePostCommitPeel, no_slot, [&] {
        (void)reduce_budgeted_parallel_with_hook(reducer, generous_budget(), parallel_options(1, 2),
                                                 hook);
    });

    CHECK(control.prepeel_events.load(std::memory_order_relaxed) == 1);
    CHECK(reducer.incidence_epoch() == before.incidence_epoch + 1);
    CHECK(reducer.total_row_count() == before.total_rows + 1);
    CHECK(reducer.active_row_count() == 2);
    CHECK((reducer.active_row_ids() ==
           std::vector<StructuredRowId>{StructuredRowId{2}, StructuredRowId{3}}));
    CHECK((std::vector<LargePrimeKey>(reducer.lp_keys(StructuredRowId{2}).begin(),
                                      reducer.lp_keys(StructuredRowId{2}).end()) ==
           std::vector<LargePrimeKey>{q}));
    CHECK(reducer.lp_keys(StructuredRowId{3}).empty());

    const auto& stats = reducer.stats();
    CHECK(stats.budgeted_runs == before.stats.budgeted_runs + 1);
    CHECK(stats.planning_passes == before.stats.planning_passes + 1);
    CHECK(stats.candidate_plans_considered == before.stats.candidate_plans_considered + 1);
    CHECK(stats.two_way_merges == before.stats.two_way_merges + 1);
    CHECK(stats.singleton_rows_removed == before.stats.singleton_rows_removed);
    CHECK(stats.peak_prepared_payload_entries == 4);
    CHECK(stats.output_rows == 2);
    CHECK(stats.stop_reason == StructuredReductionStopReason::NotStarted);

    CHECK(reducer.peel_singletons() == 1);
    CHECK(reducer.incidence_epoch() == before.incidence_epoch + 2);
    CHECK((reducer.active_row_ids() == std::vector<StructuredRowId>{StructuredRowId{3}}));
    CHECK(reducer.stats().singleton_rows_removed == before.stats.singleton_rows_removed + 1);
    CHECK(reducer.stats().output_rows == 1);
}

template <typename Action> void run_test(std::string_view name, Action&& action) {
    current_test = name;
    try {
        std::forward<Action>(action)();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "test failed: " << current_test << ": " << error.what() << '\n';
    } catch (...) {
        ++failures;
        std::cerr << "test failed with non-standard exception: " << current_test << '\n';
    }
}

} // namespace

int main() {
    run_test("lowest prepare failure drains tail",
             test_lowest_prepare_failure_drains_tail_and_discards_round_state);
    run_test("masked commit prepublication rollback",
             test_masked_commit_prepublication_failure_preserves_complete_snapshot);
    run_test("driver prepublication rollback",
             test_driver_prepublication_failure_discards_staged_round_state);
    run_test("postpublication peel commit granule",
             test_postpublication_peel_failure_retains_commit_granule);

    if (failures != 0) {
        std::cerr << failures << " tests failed after " << checks << " checks\n";
        return 1;
    }
    std::cout << "structured parallel failures: " << checks << " checks passed\n";
    return 0;
}
