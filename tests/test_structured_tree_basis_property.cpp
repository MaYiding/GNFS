#include "gnfs/relation/structured_reduction.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::SequentialStructuredReducer;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionStopReason;
using gnfs::relation::StructuredRowId;
using gnfs::relation::TreeBasisMergePlan;
using gnfs::relation::TreeBasisPlanner;

namespace {

using Mask = uint64_t;

constexpr uint64_t kPivotPrime = 1'000'003;
constexpr std::array<uint64_t, 6> kExtraPrimes{
    1'000'033, 1'000'037, 1'000'081, 1'000'099, 1'000'117, 1'000'121,
};
constexpr size_t kFactorColumns = 8;
constexpr size_t kPivotColumn = kFactorColumns;
constexpr uint64_t kSeed = 0x6a09e667f3bcc909ULL;

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

[[nodiscard]] constexpr LargePrimeKey rational_key(uint64_t prime) noexcept {
    return LargePrimeKey{prime, 0, false};
}

void add_large_prime(Relation& relation, uint64_t prime) {
    relation.rational_large_prime.emplace_back(prime, uint8_t{1});
}

[[nodiscard]] size_t lp_column(const LargePrimeKey& key) {
    if (key == rational_key(kPivotPrime)) {
        return kPivotColumn;
    }
    const auto it = std::find(kExtraPrimes.begin(), kExtraPrimes.end(), key.prime);
    CHECK(!key.is_algebraic);
    CHECK(key.root == 0);
    CHECK(it != kExtraPrimes.end());
    if (it == kExtraPrimes.end()) {
        return kPivotColumn;
    }
    return kPivotColumn + 1 + static_cast<size_t>(it - kExtraPrimes.begin());
}

[[nodiscard]] Mask encode_relation(const Relation& relation) {
    Mask encoded = 0;
    for (const uint32_t factor : relation.rational_factors) {
        CHECK(factor < kFactorColumns);
        if (factor < kFactorColumns) {
            encoded ^= Mask{1} << factor;
        }
    }
    CHECK(relation.algebraic_factors.empty());
    for (const auto& key : gnfs::relation::odd_large_prime_keys(relation)) {
        const size_t column = lp_column(key);
        CHECK(column < std::numeric_limits<Mask>::digits);
        encoded ^= Mask{1} << column;
    }
    return encoded;
}

[[nodiscard]] Mask source_mask(const SequentialStructuredReducer& reducer, StructuredRowId row) {
    Mask result = 0;
    for (const auto source : reducer.sources(row).sources()) {
        CHECK(source.generation == reducer.corpus().generation());
        CHECK(source.ordinal < std::numeric_limits<Mask>::digits);
        result ^= Mask{1} << source.ordinal;
    }
    CHECK(result != 0);
    return result;
}

[[nodiscard]] Mask xor_selected(Mask selection, std::span<const Mask> rows) {
    Mask result = 0;
    for (size_t index = 0; index < rows.size(); ++index) {
        if (((selection >> index) & Mask{1}) != 0) {
            result ^= rows[index];
        }
    }
    return result;
}

[[nodiscard]] Mask subset_count(size_t rows) {
    CHECK(rows < std::numeric_limits<Mask>::digits);
    return Mask{1} << rows;
}

[[nodiscard]] std::vector<Mask> exact_kernel(std::span<const Mask> rows) {
    std::vector<Mask> result;
    for (Mask selection = 0; selection < subset_count(rows.size()); ++selection) {
        if (xor_selected(selection, rows) == 0) {
            result.push_back(selection);
        }
    }
    return result;
}

[[nodiscard]] std::vector<Mask> exact_span(std::span<const Mask> rows) {
    std::vector<Mask> result;
    for (Mask selection = 0; selection < subset_count(rows.size()); ++selection) {
        result.push_back(xor_selected(selection, rows));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
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
    CHECK(it != plan.members.end());
    return it == plan.members.end() ? 0 : static_cast<size_t>(it - plan.members.begin());
}

void check_tree_span(const TreeBasisMergePlan& plan) {
    std::vector<Mask> edge_rows;
    for (const auto& edge : plan.edges) {
        const size_t lhs = member_index(plan, edge.endpoints[0]);
        const size_t rhs = member_index(plan, edge.endpoints[1]);
        CHECK(lhs != rhs);
        edge_rows.push_back((Mask{1} << lhs) | (Mask{1} << rhs));
    }

    std::vector<Mask> even_space;
    for (Mask value = 0; value < subset_count(plan.members.size()); ++value) {
        if ((std::popcount(value) & 1) == 0) {
            even_space.push_back(value);
        }
    }
    CHECK(exact_span(edge_rows) == even_space);
}

void check_dependency_mapping(const SequentialStructuredReducer& reducer,
                              std::span<const Mask> original_rows) {
    const auto active = reducer.active_row_ids();
    std::vector<Mask> reduced_rows;
    std::vector<Mask> transforms;
    for (const auto row : active) {
        const Mask transform = source_mask(reducer, row);
        const Mask materialized = encode_relation(reducer.materialize(row));
        CHECK(materialized == xor_selected(transform, original_rows));
        transforms.push_back(transform);
        reduced_rows.push_back(materialized);
    }

    CHECK(exact_span(transforms).size() == subset_count(transforms.size()));
    std::vector<Mask> mapped_kernel;
    for (const Mask dependency : exact_kernel(reduced_rows)) {
        mapped_kernel.push_back(xor_selected(dependency, transforms));
    }
    std::sort(mapped_kernel.begin(), mapped_kernel.end());
    CHECK(std::adjacent_find(mapped_kernel.begin(), mapped_kernel.end()) == mapped_kernel.end());
    CHECK(mapped_kernel == exact_kernel(original_rows));
}

[[nodiscard]] std::vector<Relation> random_fixture(std::mt19937_64& rng, size_t weight) {
    std::vector<Relation> relations;
    relations.reserve(weight);
    for (size_t row = 0; row < weight; ++row) {
        Relation relation(static_cast<int64_t>(row + 1), 1);
        add_large_prime(relation, kPivotPrime);
        for (const uint64_t prime : kExtraPrimes) {
            if ((rng() & 1U) != 0U) {
                add_large_prime(relation, prime);
            }
        }
        for (uint32_t factor = 0; factor < kFactorColumns; ++factor) {
            if ((rng() & 3U) == 0U) {
                relation.rational_factors.push_back(factor);
            }
        }
        relations.push_back(std::move(relation));
    }
    return relations;
}

void test_fixed_seed_randomized_oracle() {
    std::mt19937_64 rng(kSeed);
    uint64_t generation = 10'000;
    for (size_t weight = 3; weight <= 8; ++weight) {
        for (size_t sample = 0; sample < 20; ++sample) {
            const auto relations = random_fixture(rng, weight);
            std::vector<Mask> original_rows;
            original_rows.reserve(relations.size());
            for (const auto& relation : relations) {
                original_rows.push_back(encode_relation(relation));
            }

            SequentialStructuredReducer lhs(generation, relations);
            SequentialStructuredReducer rhs(generation, relations);
            const auto lhs_plans = lhs.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
            const auto rhs_plans = rhs.plan_tree_basis_merges(TreeBasisPlanner::DeterministicMst);
            CHECK(lhs_plans == rhs_plans);
            const auto* lhs_found = find_plan(lhs_plans, rational_key(kPivotPrime));
            const auto* rhs_found = find_plan(rhs_plans, rational_key(kPivotPrime));
            CHECK(lhs_found != nullptr);
            CHECK(rhs_found != nullptr);
            if (lhs_found == nullptr || rhs_found == nullptr) {
                ++generation;
                continue;
            }

            const TreeBasisMergePlan lhs_plan = *lhs_found;
            const TreeBasisMergePlan rhs_plan = *rhs_found;
            CHECK(lhs_plan == rhs_plan);
            CHECK(lhs_plan.members.size() == weight);
            CHECK(lhs_plan.edges.size() + 1 == weight);
            check_tree_span(lhs_plan);

            const auto lhs_outputs = lhs.commit(lhs.prepare(lhs_plan));
            const auto rhs_outputs = rhs.commit(rhs.prepare(rhs_plan));
            CHECK(lhs_outputs == rhs_outputs);
            CHECK(lhs.active_row_ids() == rhs.active_row_ids());
            CHECK(lhs.stats().tree_basis_batches == 1);
            CHECK(lhs.stats().tree_basis_rows_consumed == weight);
            CHECK(lhs.stats().tree_basis_rows_emitted + 1 == weight);
            for (const auto output : lhs_outputs) {
                const auto keys = lhs.lp_keys(output);
                CHECK(std::find(keys.begin(), keys.end(), rational_key(kPivotPrime)) == keys.end());
                CHECK(source_mask(lhs, output) == source_mask(rhs, output));
            }
            check_dependency_mapping(lhs, original_rows);
            check_dependency_mapping(rhs, original_rows);
            ++generation;
        }
    }
}

struct ReducerSnapshot final {
    size_t total_rows = 0;
    size_t active_rows = 0;
    std::vector<StructuredRowId> active_ids;
    std::vector<Mask> sources;
    std::vector<std::vector<LargePrimeKey>> lp_keys;
    std::array<size_t, 8> stats{};
    StructuredReductionStopReason stop_reason = StructuredReductionStopReason::NotStarted;

    [[nodiscard]] bool operator==(const ReducerSnapshot&) const noexcept = default;
};

[[nodiscard]] ReducerSnapshot snapshot(const SequentialStructuredReducer& reducer) {
    ReducerSnapshot result;
    result.total_rows = reducer.total_row_count();
    result.active_rows = reducer.active_row_count();
    result.active_ids = reducer.active_row_ids();
    for (const auto row : result.active_ids) {
        result.sources.push_back(source_mask(reducer, row));
        const auto keys = reducer.lp_keys(row);
        result.lp_keys.emplace_back(keys.begin(), keys.end());
    }
    const auto& stats = reducer.stats();
    result.stats = {stats.input_rows,
                    stats.singleton_rows_removed,
                    stats.two_way_merges,
                    stats.tree_basis_batches,
                    stats.tree_basis_rows_consumed,
                    stats.tree_basis_rows_emitted,
                    stats.persistence_limited_plans,
                    stats.output_rows};
    result.stop_reason = stats.stop_reason;
    return result;
}

void test_batch_persistence_failure_is_atomic() {
    static_assert(Relation::MAX_SERIALIZED_EXTRA_AB_PAIRS >= 2);
    constexpr size_t payload_pairs =
        (static_cast<size_t>(Relation::MAX_SERIALIZED_EXTRA_AB_PAIRS) + 1) / 2;
    const auto pivot = rational_key(2'000'003);
    std::vector<Relation> relations;
    for (size_t row = 0; row < 3; ++row) {
        Relation relation(static_cast<int64_t>(row + 1), 1);
        add_large_prime(relation, pivot.prime);
        if (row != 1) {
            relation.extra_ab_pairs.assign(payload_pairs, {static_cast<int64_t>(row + 10), 1});
        }
        relations.push_back(std::move(relation));
    }

    SequentialStructuredReducer reducer(20'001, std::move(relations));
    const auto plans = reducer.plan_tree_basis_merges(TreeBasisPlanner::ReferenceStar);
    const auto* found = find_plan(plans, pivot);
    CHECK(found != nullptr);
    if (found == nullptr) {
        return;
    }
    CHECK(found->edges.size() == 2);
    const ReducerSnapshot before = snapshot(reducer);

    bool caught = false;
    try {
        (void)reducer.prepare(*found);
    } catch (const StructuredReductionError& error) {
        caught = true;
        CHECK(error.code() == StructuredReductionErrorCode::PersistenceLimit);
    }
    CHECK(caught);
    CHECK(snapshot(reducer) == before);
}

} // namespace

int main() {
    test_fixed_seed_randomized_oracle();
    test_batch_persistence_failure_is_atomic();

    if (failures != 0) {
        std::cerr << failures << " of " << checks
                  << " structured tree-basis property checks failed\n";
        return 1;
    }
    std::cout << "All " << checks << " structured tree-basis property checks passed\n";
    return 0;
}
