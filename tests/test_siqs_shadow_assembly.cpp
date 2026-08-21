// test_siqs_shadow_assembly.cpp - deterministic SIQS shadow assembly contracts

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/raw_relation_corpus_view.hpp>
#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/shadow_assembly.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using std::int64_t;
using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

using gnfs::core::Integer;
using gnfs::siqs::assemble_siqs_shadow_rows;
using gnfs::siqs::assemble_siqs_shadow_rows_bounded;
using gnfs::siqs::build_two_large_prime_cycle_basis;
using gnfs::siqs::IndexedTwoLargePrimeCycleSources;
using gnfs::siqs::materialize_two_large_prime_cycle;
using gnfs::siqs::MaterializedTwoLargePrimeCycle;
using gnfs::siqs::prepare_two_large_prime_corpus;
using gnfs::siqs::SIQSFactorPower;
using gnfs::siqs::SIQSPostMergeRow;
using gnfs::siqs::SIQSRawRelationCorpusView;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::SIQSShadowAssembly;
using gnfs::siqs::SIQSShadowAssemblyLimits;
using gnfs::siqs::SIQSShadowAssemblyOptions;
using gnfs::siqs::SIQSShadowAssemblyResult;
using gnfs::siqs::SIQSShadowAssemblyStats;
using gnfs::siqs::SIQSShadowAssemblyStatus;
using gnfs::siqs::SIQSShadowFingerprint;
using gnfs::siqs::SIQSShadowRow;
using gnfs::siqs::SIQSShadowRowOrigin;
using gnfs::siqs::SIQSSourceId;
using gnfs::siqs::TwoLargePrimeCycleBasisLimits;
using gnfs::siqs::TwoLargePrimeMaterializationStatus;
using gnfs::siqs::shadow_assembly_detail::cycle_slot_status_for_materialization;
using gnfs::siqs::shadow_assembly_detail::CycleSlot;
using gnfs::siqs::shadow_assembly_detail::CycleSlotStatus;
using gnfs::siqs::shadow_assembly_detail::reduce_cycle_slot_statuses;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

const Integer relation_modulus(91);
const std::vector<uint32_t> factor_base_primes{0, 2, 3, 5};

[[nodiscard]] SIQSRelation make_relation(int64_t value, bool negative,
                                         std::vector<uint8_t> exponents, uint64_t large_prime = 0,
                                         uint64_t large_prime2 = 0) {
    SIQSRelation relation;
    relation.value = Integer(value);
    relation.negative = negative;
    relation.exponents = std::move(exponents);
    for (size_t i = 1; i < relation.exponents.size(); ++i) {
        if (relation.exponents[i] != 0) {
            relation.fb_indices.push_back(static_cast<uint32_t>(i));
        }
    }
    relation.large_prime = large_prime;
    relation.large_prime2 = large_prime2;
    return relation;
}

struct OracleSplitter {
    bool reverse = false;

    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t cofactor) const noexcept {
        std::pair<uint64_t, uint64_t> factors{0, 0};
        switch (cofactor) {
        case 121:
            factors = {11, 11};
            break;
        case 319:
            factors = {11, 29};
            break;
        case 451:
            factors = {11, 41};
            break;
        case 1189:
            factors = {29, 41};
            break;
        default:
            break;
        }
        if (reverse) {
            std::swap(factors.first, factors.second);
        }
        return factors;
    }
};

[[nodiscard]] std::vector<SIQSRelation> make_main_corpus() {
    std::vector<SIQSRelation> relations;

    // Canonical full source 0. The exact duplicate must not consume an ID.
    relations.push_back(make_relation(1, true, {0, 1, 2, 1}));
    relations.push_back(relations.back());
    // A different raw source with the same canonical x and arithmetic row.
    relations.push_back(make_relation(92, true, {0, 1, 2, 1}));
    // Same matrix parity as source 0, but a distinct full exponent vector.
    relations.push_back(make_relation(1, true, {0, 13, 2, 1}));
    relations.push_back(make_relation(4, true, {0, 0, 1, 2}));
    relations.push_back(make_relation(8, true, {0, 0, 3, 0}));
    relations.push_back(make_relation(9, true, {0, 1, 0, 1}));
    relations.push_back(make_relation(10, false, {0, 0, 2, 0}));

    // 1LP parallel cycle. Added powers are multiples of lambda(91)=12.
    relations.push_back(make_relation(2, true, {0, 252, 1, 0}, 29, 0));
    relations.push_back(relations.back());
    relations.push_back(make_relation(31, false, {0, 253, 1, 1}, 29, 0));

    // 2LP triangle (11,29), (11,41), (29,41).
    relations.push_back(make_relation(379, false, {0, 1, 2, 2}, 319, 1));
    relations.push_back(make_relation(38, false, {0, 24, 1, 0}, 451, 1));
    relations.push_back(make_relation(85, false, {0, 1, 1, 0}, 1189, 1));

    // A p^2 self-loop.
    relations.push_back(make_relation(11, false, {0, 0, 0, 0}, 121, 1));
    return relations;
}

[[nodiscard]] SIQSShadowAssemblyResult assemble(const std::vector<SIQSRelation>& relations,
                                                SIQSShadowAssemblyOptions options = {3, 1},
                                                OracleSplitter splitter = {}) {
    return assemble_siqs_shadow_rows(
        std::span<const SIQSRelation>(relations.data(), relations.size()),
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()),
        relation_modulus, 41, options, splitter);
}

void check_result(const SIQSShadowAssemblyResult& result,
                  SIQSShadowAssemblyStatus expected_status) {
    CHECK(result.status() == expected_status);
    CHECK(result.assembly().has_value() == (expected_status == SIQSShadowAssemblyStatus::valid));
    CHECK(result.is_valid() == (expected_status == SIQSShadowAssemblyStatus::valid));
    const bool is_row_limit = expected_status == SIQSShadowAssemblyStatus::row_candidate_limit ||
                              expected_status == SIQSShadowAssemblyStatus::pretrim_row_limit;
    CHECK(result.limit_evidence().has_value() == is_row_limit);
}

[[nodiscard]] bool same_post_merge_row(const SIQSPostMergeRow& lhs, const SIQSPostMergeRow& rhs) {
    return lhs.x_modulus == rhs.x_modulus && lhs.q_negative == rhs.q_negative &&
           lhs.factor_powers == rhs.factor_powers &&
           lhs.large_prime_sqrt_factors == rhs.large_prime_sqrt_factors &&
           lhs.source_ids == rhs.source_ids;
}

[[nodiscard]] bool same_materialized_cycle(const MaterializedTwoLargePrimeCycle& lhs,
                                           const MaterializedTwoLargePrimeCycle& rhs) {
    return lhs.value_modulus == rhs.value_modulus && lhs.negative == rhs.negative &&
           lhs.factor_base_exponents == rhs.factor_base_exponents &&
           lhs.large_prime_square_roots == rhs.large_prime_square_roots &&
           lhs.relation_indices == rhs.relation_indices;
}

[[nodiscard]] bool same_shadow_row(const SIQSShadowRow& lhs, const SIQSShadowRow& rhs) {
    return lhs.origin == rhs.origin && same_post_merge_row(lhs.row, rhs.row);
}

[[nodiscard]] bool same_assembly(const SIQSShadowAssembly& lhs, const SIQSShadowAssembly& rhs) {
    if (lhs.sources.full_source_ids != rhs.sources.full_source_ids ||
        lhs.sources.partial_source_ids != rhs.sources.partial_source_ids ||
        lhs.stats != rhs.stats || lhs.fingerprints != rhs.fingerprints ||
        lhs.rows.size() != rhs.rows.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.rows.size(); ++i) {
        if (!same_shadow_row(lhs.rows[i], rhs.rows[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const SIQSShadowRow* find_row(const SIQSShadowAssembly& assembly, int64_t x_modulus,
                                            SIQSShadowRowOrigin origin) {
    const auto position = std::find_if(
        assembly.rows.begin(), assembly.rows.end(), [x_modulus, origin](const SIQSShadowRow& row) {
            return row.origin == origin && row.row.x_modulus == Integer(x_modulus);
        });
    return position == assembly.rows.end() ? nullptr : &*position;
}

void check_main_stats(const SIQSShadowAssembly& assembly) {
    const auto& stats = assembly.stats;
    CHECK(stats.input_relations == 15);
    CHECK(stats.encoded_full_relations == 8);
    CHECK(stats.valid_full_relations == 8);
    CHECK(stats.rejected_full_relations == 0);
    CHECK(stats.full_sources == 7);
    CHECK(stats.duplicate_full_sources == 1);
    CHECK(stats.adapter.input_relations == 15);
    CHECK(stats.adapter.full_relations == 8);
    CHECK(stats.adapter.accepted_one_lp == 2);
    CHECK(stats.adapter.accepted_two_lp == 4);
    CHECK(stats.adapter.rejected_relations == 1);
    CHECK(stats.partial_sources == 6);
    CHECK(stats.graph_edges == 6);
    CHECK(stats.graph_cycles == 3);
    CHECK(stats.valid_cycle_rows == 3);
    CHECK(stats.rejected_cycle_rows == 0);
    CHECK(stats.rows_before_dedup == 10);
    CHECK(stats.arithmetic_duplicates_removed == 1);
    CHECK(stats.pretrim_rows == 9);
    CHECK(stats.selected_rows == 7);
    CHECK(stats.selected_full_rows == 4);
    CHECK(stats.selected_cycle_rows == 3);
    CHECK(stats.trimmed_rows == 2);

    CHECK(stats.valid_full_relations + stats.rejected_full_relations ==
          stats.encoded_full_relations);
    CHECK(stats.full_sources + stats.duplicate_full_sources == stats.valid_full_relations);
    CHECK(stats.valid_cycle_rows + stats.rejected_cycle_rows == stats.graph_cycles);
    CHECK(stats.pretrim_rows + stats.arithmetic_duplicates_removed == stats.rows_before_dedup);
    CHECK(stats.selected_rows + stats.trimmed_rows == stats.pretrim_rows);
    CHECK(stats.selected_full_rows + stats.selected_cycle_rows == stats.selected_rows);
}

void test_catalog_provenance_dedup_and_trim() {
    const auto result = assemble(make_main_corpus());
    check_result(result, SIQSShadowAssemblyStatus::valid);
    if (!result.assembly()) {
        return;
    }
    const auto& assembly = *result.assembly();
    check_main_stats(assembly);

    const std::vector<SIQSSourceId> expected_full_ids{{0}, {1}, {2}, {3}, {4}, {5}, {6}};
    const std::vector<SIQSSourceId> expected_partial_ids{{7}, {8}, {9}, {10}, {11}, {12}};
    CHECK(assembly.sources.full_source_ids == expected_full_ids);
    CHECK(assembly.sources.partial_source_ids == expected_partial_ids);

    // The raw sources x=1 and x=92 both exist, but their identical arithmetic
    // row is retained once with the smaller canonical source ID.
    const auto* first_x1 = find_row(assembly, 1, SIQSShadowRowOrigin::raw_full);
    CHECK(first_x1 != nullptr);
    if (first_x1 != nullptr) {
        CHECK(first_x1->row.source_ids == std::vector<SIQSSourceId>({{0}}));
        CHECK(first_x1->row.factor_powers ==
              std::vector<SIQSFactorPower>({{1, 1}, {2, 2}, {3, 1}}));
    }
    const size_t x1_count = static_cast<size_t>(
        std::count_if(assembly.rows.begin(), assembly.rows.end(), [](const SIQSShadowRow& row) {
            return row.origin == SIQSShadowRowOrigin::raw_full && row.row.x_modulus == Integer(1);
        }));
    CHECK(x1_count == 2);
    const bool kept_same_parity_wide_row =
        std::any_of(assembly.rows.begin(), assembly.rows.end(), [](const SIQSShadowRow& row) {
            return row.origin == SIQSShadowRowOrigin::raw_full && row.row.x_modulus == Integer(1) &&
                   row.row.factor_powers ==
                       std::vector<SIQSFactorPower>({{1, 13}, {2, 2}, {3, 1}}) &&
                   row.row.source_ids == std::vector<SIQSSourceId>({{1}});
        });
    CHECK(kept_same_parity_wide_row);
    CHECK(std::none_of(assembly.rows.begin(), assembly.rows.end(), [](const SIQSShadowRow& row) {
        return row.row.source_ids == std::vector<SIQSSourceId>({{6}});
    }));

    const auto* square_cycle = find_row(assembly, 11, SIQSShadowRowOrigin::large_prime_cycle);
    const auto* triangle_cycle = find_row(assembly, 38, SIQSShadowRowOrigin::large_prime_cycle);
    const auto* one_lp_cycle = find_row(assembly, 62, SIQSShadowRowOrigin::large_prime_cycle);
    CHECK(square_cycle != nullptr);
    CHECK(triangle_cycle != nullptr);
    CHECK(one_lp_cycle != nullptr);
    if (square_cycle != nullptr) {
        CHECK(square_cycle->row.factor_powers.empty());
        CHECK(square_cycle->row.large_prime_sqrt_factors == std::vector<uint64_t>({11}));
        CHECK(square_cycle->row.source_ids == std::vector<SIQSSourceId>({{9}}));
    }
    if (triangle_cycle != nullptr) {
        CHECK(triangle_cycle->row.factor_powers ==
              std::vector<SIQSFactorPower>({{1, 26}, {2, 4}, {3, 2}}));
        CHECK(triangle_cycle->row.large_prime_sqrt_factors == std::vector<uint64_t>({11, 29, 41}));
        CHECK(triangle_cycle->row.source_ids == std::vector<SIQSSourceId>({{10}, {11}, {12}}));
    }
    if (one_lp_cycle != nullptr) {
        CHECK(one_lp_cycle->row.q_negative);
        CHECK(one_lp_cycle->row.factor_powers ==
              std::vector<SIQSFactorPower>({{1, 505}, {2, 2}, {3, 1}}));
        CHECK(one_lp_cycle->row.large_prime_sqrt_factors == std::vector<uint64_t>({29}));
        CHECK(one_lp_cycle->row.source_ids == std::vector<SIQSSourceId>({{7}, {8}}));
    }

    // The full prefix is capped at FB size while trim excess is reserved for
    // every available cycle row.
    CHECK(std::count_if(assembly.rows.begin(), assembly.rows.end(), [](const SIQSShadowRow& row) {
              return row.origin == SIQSShadowRowOrigin::large_prime_cycle;
          }) == 3);
}

void test_permutation_split_order_and_worker_determinism() {
    const auto baseline = assemble(make_main_corpus(), {3, 1}, OracleSplitter{false});
    check_result(baseline, SIQSShadowAssemblyStatus::valid);
    if (!baseline.assembly()) {
        return;
    }

    auto reversed_relations = make_main_corpus();
    std::reverse(reversed_relations.begin(), reversed_relations.end());
    const auto reversed = assemble(reversed_relations, {3, 1}, OracleSplitter{true});
    check_result(reversed, SIQSShadowAssemblyStatus::valid);
    if (reversed.assembly()) {
        CHECK(same_assembly(*baseline.assembly(), *reversed.assembly()));
    }

    for (const uint32_t workers : {1U, 2U, 4U}) {
        const auto candidate = assemble(make_main_corpus(), {3, workers});
        check_result(candidate, SIQSShadowAssemblyStatus::valid);
        if (candidate.assembly()) {
            CHECK(same_assembly(*baseline.assembly(), *candidate.assembly()));
        }
    }
}

void test_segmented_corpus_matches_flattened_across_duplicates_limits_and_exceptions() {
    const auto relations = make_main_corpus();
    const auto baseline = assemble(relations, {3, 1});
    check_result(baseline, SIQSShadowAssemblyStatus::valid);
    if (!baseline.assembly()) {
        return;
    }

    const auto relation_span = std::span<const SIQSRelation>(relations.data(), relations.size());
    const auto factor_base_span =
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size());
    for (size_t split = 0; split <= relations.size(); ++split) {
        const auto view = SIQSRawRelationCorpusView::try_create(relation_span.first(split),
                                                                relation_span.subspan(split));
        CHECK(view.has_value());
        if (!view) {
            continue;
        }
        const auto segmented =
            assemble_siqs_shadow_rows(*view, factor_base_span, relation_modulus, 41,
                                      SIQSShadowAssemblyOptions{3, 1}, OracleSplitter{});
        check_result(segmented, SIQSShadowAssemblyStatus::valid);
        if (segmented.assembly()) {
            CHECK(same_assembly(*segmented.assembly(), *baseline.assembly()));
        }
    }

    // The duplicate 1LP records at ordinals 8 and 9 straddle this boundary and
    // live in independent vectors. Canonical IDs, statistics, and fingerprints
    // must remain identical for every worker count.
    const std::vector<SIQSRelation> first(relations.begin(), relations.begin() + 9);
    const std::vector<SIQSRelation> second(relations.begin() + 9, relations.end());
    const auto independent = SIQSRawRelationCorpusView::try_create(
        std::span<const SIQSRelation>(first.data(), first.size()),
        std::span<const SIQSRelation>(second.data(), second.size()));
    CHECK(independent.has_value());
    if (!independent) {
        return;
    }
    for (const uint32_t workers : {1U, 2U, 4U}) {
        const auto segmented =
            assemble_siqs_shadow_rows(*independent, factor_base_span, relation_modulus, 41,
                                      SIQSShadowAssemblyOptions{3, workers}, OracleSplitter{});
        check_result(segmented, SIQSShadowAssemblyStatus::valid);
        if (segmented.assembly()) {
            CHECK(same_assembly(*segmented.assembly(), *baseline.assembly()));
        }
    }

    const SIQSShadowAssemblyLimits candidate_short{
        TwoLargePrimeCycleBasisLimits{6, 3, 6},
        9,
        9,
    };
    const auto bounded = assemble_siqs_shadow_rows_bounded(
        *independent, factor_base_span, relation_modulus, 41, SIQSShadowAssemblyOptions{3, 1},
        candidate_short, OracleSplitter{});
    check_result(bounded, SIQSShadowAssemblyStatus::row_candidate_limit);
    CHECK(bounded.limit_evidence() == (gnfs::siqs::SIQSShadowAssemblyLimitEvidence{10, 9}));

    const auto throwing = assemble_siqs_shadow_rows(
        *independent, factor_base_span, relation_modulus, 41, SIQSShadowAssemblyOptions{3, 1},
        [](uint64_t) -> std::pair<uint64_t, uint64_t> { throw 7; });
    check_result(throwing, SIQSShadowAssemblyStatus::exception_failure);

    const auto exhausted = assemble_siqs_shadow_rows(
        *independent, factor_base_span, relation_modulus, 41, SIQSShadowAssemblyOptions{3, 1},
        [](uint64_t) -> std::pair<uint64_t, uint64_t> { throw std::bad_alloc(); });
    check_result(exhausted, SIQSShadowAssemblyStatus::resource_exhausted);
}

void test_adapter_graph_cycles_match_generic_and_indexed_materializers() {
    const auto relations = make_main_corpus();
    auto corpus = prepare_two_large_prime_corpus(
        std::span<const SIQSRelation>(relations.data(), relations.size()),
        factor_base_primes.size(), 41, OracleSplitter{});
    CHECK(corpus.has_value());
    if (!corpus) {
        return;
    }

    const auto basis = build_two_large_prime_cycle_basis(corpus->edges);
    CHECK(basis.has_value());
    if (!basis) {
        return;
    }
    CHECK(basis->cycles.size() == 3);

    std::vector<std::optional<MaterializedTwoLargePrimeCycle>> generic_results;
    generic_results.reserve(basis->cycles.size());
    for (const auto& cycle : basis->cycles) {
        generic_results.push_back(
            materialize_two_large_prime_cycle(corpus->sources, cycle, relation_modulus));
    }

    // This mirrors assembly: one immutable, move-only validated corpus is
    // shared by every sorted graph cycle, including concurrent workers.
    auto indexed_sources = IndexedTwoLargePrimeCycleSources::try_create(std::move(corpus->sources));
    CHECK(indexed_sources.has_value());
    if (!indexed_sources) {
        return;
    }

    for (size_t cycle_ordinal = 0; cycle_ordinal < basis->cycles.size(); ++cycle_ordinal) {
        const auto indexed = materialize_two_large_prime_cycle(
            *indexed_sources, basis->cycles[cycle_ordinal], relation_modulus);
        const auto& generic = generic_results[cycle_ordinal];
        CHECK(generic.has_value());
        CHECK(indexed.has_value());
        if (generic && indexed) {
            CHECK(same_materialized_cycle(*generic, *indexed));
        }
    }
}

void test_materialization_failures_map_fail_closed() {
    CHECK(cycle_slot_status_for_materialization(TwoLargePrimeMaterializationStatus::valid) ==
          CycleSlotStatus::valid);
    CHECK(cycle_slot_status_for_materialization(
              TwoLargePrimeMaterializationStatus::size_overflow) == CycleSlotStatus::size_overflow);
    CHECK(cycle_slot_status_for_materialization(
              TwoLargePrimeMaterializationStatus::exponent_overflow) ==
          CycleSlotStatus::size_overflow);

    for (const auto status : {
             TwoLargePrimeMaterializationStatus::invalid_modulus,
             TwoLargePrimeMaterializationStatus::invalid_source_catalog,
             TwoLargePrimeMaterializationStatus::invalid_cycle_support,
             TwoLargePrimeMaterializationStatus::invalid_source_shape,
             TwoLargePrimeMaterializationStatus::odd_large_prime_degree,
             TwoLargePrimeMaterializationStatus::internal_invariant_failure,
         }) {
        CHECK(cycle_slot_status_for_materialization(status) ==
              CycleSlotStatus::internal_invariant_failure);
    }

    CHECK(reduce_cycle_slot_statuses(std::span<const CycleSlot>{}) ==
          SIQSShadowAssemblyStatus::valid);

    const std::vector<CycleSlot> rejection_only{
        {CycleSlotStatus::row_identity_rejected, std::nullopt},
    };
    CHECK(reduce_cycle_slot_statuses(rejection_only) == SIQSShadowAssemblyStatus::valid);

    const std::vector<CycleSlot> invalid_valid_slot{
        {CycleSlotStatus::valid, std::nullopt},
    };
    CHECK(reduce_cycle_slot_statuses(invalid_valid_slot) ==
          SIQSShadowAssemblyStatus::internal_invariant_failure);

    const std::vector<CycleSlot> size_before_resource{
        {CycleSlotStatus::row_identity_rejected, std::nullopt},
        {CycleSlotStatus::size_overflow, std::nullopt},
        {CycleSlotStatus::resource_exhausted, std::nullopt},
    };
    CHECK(reduce_cycle_slot_statuses(size_before_resource) ==
          SIQSShadowAssemblyStatus::size_overflow);

    const std::vector<CycleSlot> resource_before_size{
        {CycleSlotStatus::row_identity_rejected, std::nullopt},
        {CycleSlotStatus::resource_exhausted, std::nullopt},
        {CycleSlotStatus::size_overflow, std::nullopt},
    };
    CHECK(reduce_cycle_slot_statuses(resource_before_size) ==
          SIQSShadowAssemblyStatus::resource_exhausted);

    const std::vector<CycleSlot> terminal_before_pending{
        {CycleSlotStatus::size_overflow, std::nullopt},
        {CycleSlotStatus::pending, std::nullopt},
    };
    CHECK(reduce_cycle_slot_statuses(terminal_before_pending) ==
          SIQSShadowAssemblyStatus::size_overflow);

    const std::vector<CycleSlot> pending_before_terminal{
        {CycleSlotStatus::pending, std::nullopt},
        {CycleSlotStatus::exception_failure, std::nullopt},
    };
    CHECK(reduce_cycle_slot_statuses(pending_before_terminal) ==
          SIQSShadowAssemblyStatus::internal_invariant_failure);
}

void test_invalid_configuration_and_result_moves() {
    const auto corpus = make_main_corpus();
    const SIQSShadowAssemblyOptions options{3, 1};
    const auto call = [&](const std::vector<uint32_t>& primes, const Integer& modulus,
                          uint64_t lp_bound, SIQSShadowAssemblyOptions selected_options) {
        return assemble_siqs_shadow_rows(
            std::span<const SIQSRelation>(corpus.data(), corpus.size()),
            std::span<const uint32_t>(primes.data(), primes.size()), modulus, lp_bound,
            selected_options, OracleSplitter{});
    };

    check_result(call(factor_base_primes, Integer(0), 41, options),
                 SIQSShadowAssemblyStatus::invalid_modulus);
    check_result(call(factor_base_primes, Integer(1), 41, options),
                 SIQSShadowAssemblyStatus::invalid_modulus);
    check_result(call({}, relation_modulus, 41, options),
                 SIQSShadowAssemblyStatus::invalid_factor_base);
    check_result(call({0, 3, 2, 5}, relation_modulus, 41, options),
                 SIQSShadowAssemblyStatus::invalid_factor_base);
    check_result(call(factor_base_primes, relation_modulus, 1, options),
                 SIQSShadowAssemblyStatus::invalid_large_prime_bound);
    check_result(call(factor_base_primes, relation_modulus, 41, {3, 0}),
                 SIQSShadowAssemblyStatus::invalid_options);
    check_result(
        call(factor_base_primes, relation_modulus, 41, {std::numeric_limits<size_t>::max(), 1}),
        SIQSShadowAssemblyStatus::size_overflow);

    const auto throwing_splitter = assemble_siqs_shadow_rows(
        std::span<const SIQSRelation>(corpus.data(), corpus.size()),
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()),
        relation_modulus, 41, options, [](uint64_t) -> std::pair<uint64_t, uint64_t> { throw 7; });
    check_result(throwing_splitter, SIQSShadowAssemblyStatus::exception_failure);

    const auto exhausted_splitter = assemble_siqs_shadow_rows(
        std::span<const SIQSRelation>(corpus.data(), corpus.size()),
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()),
        relation_modulus, 41, options,
        [](uint64_t) -> std::pair<uint64_t, uint64_t> { throw std::bad_alloc(); });
    check_result(exhausted_splitter, SIQSShadowAssemblyStatus::resource_exhausted);

    auto valid = assemble(corpus);
    check_result(valid, SIQSShadowAssemblyStatus::valid);
    SIQSShadowAssemblyResult moved = std::move(valid);
    check_result(moved, SIQSShadowAssemblyStatus::valid);
    check_result(valid, SIQSShadowAssemblyStatus::internal_invariant_failure);

    auto assignment_target = call(factor_base_primes, Integer(1), 41, options);
    assignment_target = std::move(moved);
    check_result(assignment_target, SIQSShadowAssemblyStatus::valid);
    check_result(moved, SIQSShadowAssemblyStatus::internal_invariant_failure);
}

void test_rejection_stats_remain_partitioned() {
    std::vector<SIQSRelation> relations;
    relations.push_back(make_relation(1, true, {0, 1, 2, 1}));
    auto malformed_full = make_relation(1, true, {0, 1, 2, 1});
    malformed_full.exponents[0] = 1;
    malformed_full.fb_indices.insert(malformed_full.fb_indices.begin(), 0);
    relations.push_back(std::move(malformed_full));

    // The adapter accepts these two partials structurally, but value=3 makes
    // their materialized arithmetic identity invalid.
    relations.push_back(make_relation(3, true, {0, 252, 1, 0}, 29, 0));
    relations.push_back(make_relation(31, false, {0, 253, 1, 1}, 29, 0));

    const auto baseline = assemble(relations, {3, 1});
    check_result(baseline, SIQSShadowAssemblyStatus::valid);
    if (!baseline.assembly()) {
        return;
    }
    const auto& stats = baseline.assembly()->stats;
    CHECK(stats.input_relations == 4);
    CHECK(stats.encoded_full_relations == 2);
    CHECK(stats.valid_full_relations == 1);
    CHECK(stats.rejected_full_relations == 1);
    CHECK(stats.full_sources == 1);
    CHECK(stats.duplicate_full_sources == 0);
    CHECK(stats.adapter.full_relations == 1);
    CHECK(stats.adapter.accepted_one_lp == 2);
    CHECK(stats.adapter.accepted_two_lp == 0);
    CHECK(stats.adapter.rejected_relations == 1);
    CHECK(stats.graph_cycles == 1);
    CHECK(stats.valid_cycle_rows == 0);
    CHECK(stats.rejected_cycle_rows == 1);
    CHECK(stats.rows_before_dedup == 1);
    CHECK(stats.pretrim_rows == 1);
    CHECK(stats.selected_rows == 1);
    CHECK(stats.selected_full_rows == 1);
    CHECK(stats.selected_cycle_rows == 0);
    CHECK(stats.trimmed_rows == 0);

    CHECK(stats.valid_full_relations + stats.rejected_full_relations ==
          stats.encoded_full_relations);
    CHECK(stats.valid_cycle_rows + stats.rejected_cycle_rows == stats.graph_cycles);
    CHECK(stats.selected_full_rows + stats.selected_cycle_rows == stats.selected_rows);

    for (const uint32_t workers : {2U, 4U}) {
        const auto candidate = assemble(relations, {3, workers});
        check_result(candidate, SIQSShadowAssemblyStatus::valid);
        if (candidate.assembly()) {
            CHECK(same_assembly(*baseline.assembly(), *candidate.assembly()));
        }
    }
}

void test_parallel_rejection_slots_are_deterministic() {
    auto relations = make_main_corpus();
    // Preserve the duplicate relation while invalidating only the 1LP cycle's
    // exact arithmetic identity. The triangle and self-loop remain valid, so
    // requested worker counts 2 and 4 resolve to real parallel execution over
    // three independent cycle slots.
    relations[8].value = Integer(3);
    relations[9].value = Integer(3);

    const auto baseline = assemble(relations, {3, 1});
    check_result(baseline, SIQSShadowAssemblyStatus::valid);
    if (!baseline.assembly()) {
        return;
    }
    CHECK(baseline.assembly()->stats.graph_cycles == 3);
    CHECK(baseline.assembly()->stats.valid_cycle_rows == 2);
    CHECK(baseline.assembly()->stats.rejected_cycle_rows == 1);

    for (const uint32_t workers : {2U, 4U}) {
        const auto candidate = assemble(relations, {3, workers});
        check_result(candidate, SIQSShadowAssemblyStatus::valid);
        if (candidate.assembly()) {
            CHECK(same_assembly(*baseline.assembly(), *candidate.assembly()));
        }
    }
}

void test_bounded_graph_and_row_limits_are_inclusive() {
    const auto relations = make_main_corpus();
    const auto call = [&](SIQSShadowAssemblyLimits limits) {
        return assemble_siqs_shadow_rows_bounded(
            std::span<const SIQSRelation>(relations.data(), relations.size()),
            std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()),
            relation_modulus, 41, SIQSShadowAssemblyOptions{3, 4}, limits, OracleSplitter{});
    };

    const SIQSShadowAssemblyLimits exact{
        TwoLargePrimeCycleBasisLimits{6, 3, 6},
        10,
        9,
    };
    const auto bounded = call(exact);
    check_result(bounded, SIQSShadowAssemblyStatus::valid);
    const auto unlimited = assemble(relations, {3, 4});
    check_result(unlimited, SIQSShadowAssemblyStatus::valid);
    if (bounded.assembly() && unlimited.assembly()) {
        CHECK(same_assembly(*bounded.assembly(), *unlimited.assembly()));
    }

    auto edge_short = exact;
    edge_short.graph.max_edges = 5;
    check_result(call(edge_short), SIQSShadowAssemblyStatus::graph_edge_limit);

    auto cycle_short = exact;
    cycle_short.graph.max_cycles = 2;
    check_result(call(cycle_short), SIQSShadowAssemblyStatus::graph_cycle_limit);

    auto incidence_short = exact;
    incidence_short.graph.max_cycle_incidences = 5;
    check_result(call(incidence_short), SIQSShadowAssemblyStatus::graph_incidence_limit);

    auto candidate_short = exact;
    candidate_short.max_row_candidates = 9;
    const auto candidate_result = call(candidate_short);
    check_result(candidate_result, SIQSShadowAssemblyStatus::row_candidate_limit);
    if (candidate_result.limit_evidence()) {
        CHECK(candidate_result.limit_evidence()->observed == 10);
        CHECK(candidate_result.limit_evidence()->maximum == 9);
    }

    auto pretrim_short = exact;
    pretrim_short.max_pretrim_rows = 8;
    const auto pretrim_result = call(pretrim_short);
    check_result(pretrim_result, SIQSShadowAssemblyStatus::pretrim_row_limit);
    if (pretrim_result.limit_evidence()) {
        CHECK(pretrim_result.limit_evidence()->observed == 9);
        CHECK(pretrim_result.limit_evidence()->maximum == 8);
    }

    SIQSShadowAssemblyResult copied_limit(candidate_result);
    check_result(copied_limit, SIQSShadowAssemblyStatus::row_candidate_limit);
    CHECK(copied_limit.limit_evidence() == candidate_result.limit_evidence());
    auto assigned_limit = assemble(relations);
    assigned_limit = pretrim_result;
    check_result(assigned_limit, SIQSShadowAssemblyStatus::pretrim_row_limit);
    CHECK(assigned_limit.limit_evidence() == pretrim_result.limit_evidence());

    const std::vector<SIQSRelation> empty;
    const auto empty_result = assemble_siqs_shadow_rows_bounded(
        std::span<const SIQSRelation>(empty.data(), empty.size()),
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()),
        relation_modulus, 41, SIQSShadowAssemblyOptions{0, 1},
        SIQSShadowAssemblyLimits{TwoLargePrimeCycleBasisLimits{0, 0, 0}, 0, 0}, OracleSplitter{});
    check_result(empty_result, SIQSShadowAssemblyStatus::valid);
    if (empty_result.assembly()) {
        CHECK(empty_result.assembly()->rows.empty());
    }
}

void test_fingerprint_layers() {
    const auto baseline = assemble(make_main_corpus(), {3, 1});
    check_result(baseline, SIQSShadowAssemblyStatus::valid);
    if (!baseline.assembly()) {
        return;
    }

    auto source_mutation = make_main_corpus();
    const auto source = std::find_if(
        source_mutation.begin(), source_mutation.end(), [](const SIQSRelation& relation) {
            return relation.value == Integer(92) && relation.large_prime == 0;
        });
    CHECK(source != source_mutation.end());
    if (source != source_mutation.end()) {
        source->value = Integer(183);
    }
    const auto mutated = assemble(source_mutation, {3, 1});
    check_result(mutated, SIQSShadowAssemblyStatus::valid);
    if (mutated.assembly()) {
        CHECK(mutated.assembly()->rows.size() == baseline.assembly()->rows.size());
        CHECK(mutated.assembly()->fingerprints.source_catalog !=
              baseline.assembly()->fingerprints.source_catalog);
        CHECK(mutated.assembly()->fingerprints.pretrim_rows !=
              baseline.assembly()->fingerprints.pretrim_rows);
        CHECK(mutated.assembly()->fingerprints.selected_rows !=
              baseline.assembly()->fingerprints.selected_rows);
    }

    const auto trim_mutation = assemble(make_main_corpus(), {4, 1});
    check_result(trim_mutation, SIQSShadowAssemblyStatus::valid);
    if (trim_mutation.assembly()) {
        CHECK(trim_mutation.assembly()->fingerprints.source_catalog ==
              baseline.assembly()->fingerprints.source_catalog);
        CHECK(trim_mutation.assembly()->fingerprints.pretrim_rows ==
              baseline.assembly()->fingerprints.pretrim_rows);
        CHECK(trim_mutation.assembly()->fingerprints.selected_rows !=
              baseline.assembly()->fingerprints.selected_rows);
        CHECK(trim_mutation.assembly()->stats.selected_rows == 8);
    }

    const SIQSShadowFingerprint zero{};
    CHECK(baseline.assembly()->fingerprints.source_catalog != zero);
    CHECK(baseline.assembly()->fingerprints.pretrim_rows != zero);
    CHECK(baseline.assembly()->fingerprints.selected_rows != zero);
}

void test_independent_golden_fingerprints() {
    const std::vector<SIQSRelation> relations{
        make_relation(1, true, {0, 1, 2, 1}),
        make_relation(5, false, {0, 2, 0, 0}, 29, 0),
        make_relation(22, false, {0, 0, 0, 0}, 29, 0),
    };
    const auto result = assemble_siqs_shadow_rows(
        std::span<const SIQSRelation>(relations.data(), relations.size()),
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()),
        relation_modulus, 47, SIQSShadowAssemblyOptions{0, 1}, OracleSplitter{});
    check_result(result, SIQSShadowAssemblyStatus::valid);
    if (!result.assembly()) {
        return;
    }

    const auto& assembly = *result.assembly();
    CHECK(assembly.sources.full_source_ids == std::vector<SIQSSourceId>({{0}}));
    CHECK(assembly.sources.partial_source_ids == std::vector<SIQSSourceId>({{1}, {2}}));
    CHECK(assembly.rows.size() == 2);

    const auto* cycle = find_row(assembly, 19, SIQSShadowRowOrigin::large_prime_cycle);
    CHECK(cycle != nullptr);
    if (cycle != nullptr) {
        CHECK(!cycle->row.q_negative);
        CHECK(cycle->row.factor_powers == std::vector<SIQSFactorPower>({{1, 2}}));
        CHECK(cycle->row.large_prime_sqrt_factors == std::vector<uint64_t>({29}));
        CHECK(cycle->row.source_ids == std::vector<SIQSSourceId>({{1}, {2}}));
    }

    const SIQSShadowFingerprint expected_catalog{0x4e893e9e29866b06ULL, 0x051575fa0890dc08ULL};
    const SIQSShadowFingerprint expected_pretrim{0x20e6b7eb37d00757ULL, 0xbb2f1303298174ddULL};
    const SIQSShadowFingerprint expected_selected{0xb97d9cf80b7796baULL, 0x55354c6507cbc9c5ULL};
    CHECK(assembly.fingerprints.source_catalog == expected_catalog);
    CHECK(assembly.fingerprints.pretrim_rows == expected_pretrim);
    CHECK(assembly.fingerprints.selected_rows == expected_selected);

    auto noisy_relations = relations;
    noisy_relations.push_back(relations.front());
    auto invalid_full = relations.front();
    invalid_full.exponents[0] = 1;
    invalid_full.fb_indices.insert(invalid_full.fb_indices.begin(), 0);
    noisy_relations.push_back(std::move(invalid_full));
    const auto noisy_result = assemble_siqs_shadow_rows(
        std::span<const SIQSRelation>(noisy_relations.data(), noisy_relations.size()),
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()),
        relation_modulus, 47, SIQSShadowAssemblyOptions{0, 1}, OracleSplitter{});
    check_result(noisy_result, SIQSShadowAssemblyStatus::valid);
    if (!noisy_result.assembly()) {
        return;
    }

    const auto& noisy_assembly = *noisy_result.assembly();
    CHECK(noisy_assembly.sources.full_source_ids == assembly.sources.full_source_ids);
    CHECK(noisy_assembly.sources.partial_source_ids == assembly.sources.partial_source_ids);
    CHECK(noisy_assembly.rows.size() == assembly.rows.size());
    if (noisy_assembly.rows.size() == assembly.rows.size()) {
        for (size_t i = 0; i < assembly.rows.size(); ++i) {
            CHECK(same_shadow_row(noisy_assembly.rows[i], assembly.rows[i]));
        }
    }
    CHECK(noisy_assembly.fingerprints == assembly.fingerprints);
    CHECK(noisy_assembly.stats.input_relations == assembly.stats.input_relations + 2);
    CHECK(noisy_assembly.stats.rejected_full_relations ==
          assembly.stats.rejected_full_relations + 1);
    CHECK(noisy_assembly.stats.duplicate_full_sources == assembly.stats.duplicate_full_sources + 1);
    CHECK(noisy_assembly.stats.valid_full_relations == assembly.stats.valid_full_relations + 1);
    CHECK(noisy_assembly.stats.full_sources == assembly.stats.full_sources);
    CHECK(noisy_assembly.stats.valid_full_relations +
              noisy_assembly.stats.rejected_full_relations ==
          noisy_assembly.stats.encoded_full_relations);
    CHECK(noisy_assembly.stats.full_sources + noisy_assembly.stats.duplicate_full_sources ==
          noisy_assembly.stats.valid_full_relations);
}

void test_empty_assembly_is_valid_and_fingerprinted() {
    const std::vector<SIQSRelation> relations;
    const auto result = assemble_siqs_shadow_rows(
        std::span<const SIQSRelation>(relations.data(), relations.size()),
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size()),
        relation_modulus, 47, SIQSShadowAssemblyOptions{0, 1}, OracleSplitter{});
    check_result(result, SIQSShadowAssemblyStatus::valid);
    if (!result.assembly()) {
        return;
    }

    const auto& assembly = *result.assembly();
    CHECK(assembly.sources.full_source_ids.empty());
    CHECK(assembly.sources.partial_source_ids.empty());
    CHECK(assembly.rows.empty());
    CHECK(assembly.stats == SIQSShadowAssemblyStats{});
    CHECK(assembly.fingerprints.source_catalog.low != 0);
    CHECK(assembly.fingerprints.source_catalog.high != 0);
    CHECK(assembly.fingerprints.pretrim_rows.low != 0);
    CHECK(assembly.fingerprints.pretrim_rows.high != 0);
    CHECK(assembly.fingerprints.selected_rows.low != 0);
    CHECK(assembly.fingerprints.selected_rows.high != 0);
}

} // namespace

int main() {
    test_catalog_provenance_dedup_and_trim();
    test_permutation_split_order_and_worker_determinism();
    test_segmented_corpus_matches_flattened_across_duplicates_limits_and_exceptions();
    test_adapter_graph_cycles_match_generic_and_indexed_materializers();
    test_materialization_failures_map_fail_closed();
    test_invalid_configuration_and_result_moves();
    test_rejection_stats_remain_partitioned();
    test_parallel_rejection_slots_are_deterministic();
    test_bounded_graph_and_row_limits_are_inclusive();
    test_fingerprint_layers();
    test_independent_golden_fingerprints();
    test_empty_assembly_is_valid_and_fingerprinted();

    std::cout << "SIQS shadow assembly: " << checks_passed << " checks passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
