#include "gnfs/relation/structured_reduction.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

using gnfs::core::Relation;
using gnfs::relation::LargePrimeKey;
using gnfs::relation::PreparedTwoWayMerge;
using gnfs::relation::SequentialStructuredReducer;
using gnfs::relation::SourceCombination;
using gnfs::relation::SourceCorpus;
using gnfs::relation::SourceId;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionStopReason;
using gnfs::relation::StructuredRowId;
using gnfs::relation::TwoWayMergePlan;

namespace {

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

using Mask = uint64_t;

constexpr size_t kMaxOracleRows = 16;
constexpr size_t kRationalPayloadBits = 8;
constexpr size_t kAlgebraicPayloadBits = 8;
constexpr size_t kAbPayloadBits = 8;
constexpr size_t kAbBitOffset = kRationalPayloadBits + kAlgebraicPayloadBits;
constexpr size_t kLpBitOffset = kAbBitOffset + kAbPayloadBits;
constexpr size_t kMaskBits = std::numeric_limits<Mask>::digits;

void oracle_require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] Mask subset_count(size_t row_count) {
    oracle_require(row_count <= kMaxOracleRows, "oracle row count exceeds exhaustive bound");
    oracle_require(row_count < kMaskBits, "oracle row count exceeds mask shift bound");
    return Mask{1} << row_count;
}

/// Independent linear canary for persisted `(a,b)` provenance. This is not a
/// Schirokauer or sign-column implementation: it deliberately projects low
/// bits from both fields so primary/extra loss changes the semantic row.
[[nodiscard]] constexpr uint8_t ab_pair_payload(int64_t a, uint64_t b) noexcept {
    const auto low_a = static_cast<uint8_t>(static_cast<uint64_t>(a) & UINT64_C(0x0f));
    const auto low_b = static_cast<uint8_t>(b & UINT64_C(0x0f));
    return static_cast<uint8_t>(low_a | static_cast<uint8_t>(low_b << 4U));
}

[[nodiscard]] constexpr LargePrimeKey rational_key(uint64_t prime) noexcept {
    return LargePrimeKey{prime, 0, false};
}

[[nodiscard]] constexpr LargePrimeKey algebraic_key(uint64_t prime, uint64_t root) noexcept {
    return LargePrimeKey{prime, root, true};
}

void add_large_prime(Relation& relation, const LargePrimeKey& key) {
    if (key.is_algebraic) {
        relation.algebraic_large_prime.emplace_back(key.prime, key.root, uint8_t{1});
    } else {
        relation.rational_large_prime.emplace_back(key.prime, uint8_t{1});
    }
}

[[nodiscard]] Relation make_relation(int64_t a, std::initializer_list<LargePrimeKey> lp_keys,
                                     std::initializer_list<uint32_t> payload = {}) {
    Relation relation(a, 1);
    relation.rational_factors.assign(payload.begin(), payload.end());
    for (const auto& key : lp_keys) {
        add_large_prime(relation, key);
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

[[nodiscard]] bool corpus_equal(const std::vector<Relation>& lhs,
                                const std::vector<Relation>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!relation_equal(lhs[index], rhs[index])) {
            return false;
        }
    }
    return true;
}

template <typename Fn> void check_error(StructuredReductionErrorCode expected, Fn&& fn) {
    bool caught = false;
    try {
        std::forward<Fn>(fn)();
    } catch (const StructuredReductionError& error) {
        caught = true;
        if (error.code() != expected) {
            std::cerr << "unexpected structured error code: expected=" << static_cast<int>(expected)
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

template <typename Fn>
void check_error_one_of(std::initializer_list<StructuredReductionErrorCode> expected, Fn&& fn) {
    bool caught = false;
    try {
        std::forward<Fn>(fn)();
    } catch (const StructuredReductionError& error) {
        caught = true;
        CHECK(std::find(expected.begin(), expected.end(), error.code()) != expected.end());
    } catch (...) {
        caught = true;
        CHECK(false);
    }
    CHECK(caught);
}

/// Independently fold raw PrimePower entries into their exact GF(2) support.
/// This intentionally does not call odd_large_prime_keys(), so the test oracle
/// does not share the reducer's LP-normalization implementation.
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
            contributions.push_back(algebraic_key(prime_power.p, prime_power.r));
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

struct SemanticUniverse final {
    std::vector<LargePrimeKey> lp_keys;
};

[[nodiscard]] SemanticUniverse build_universe(const SourceCorpus& corpus) {
    SemanticUniverse universe;
    for (size_t ordinal = 0; ordinal < corpus.size(); ++ordinal) {
        auto keys = exact_lp_support(corpus.at(corpus.source_id(ordinal)));
        universe.lp_keys.insert(universe.lp_keys.end(), keys.begin(), keys.end());
    }
    std::sort(universe.lp_keys.begin(), universe.lp_keys.end());
    universe.lp_keys.erase(std::unique(universe.lp_keys.begin(), universe.lp_keys.end()),
                           universe.lp_keys.end());
    oracle_require(kLpBitOffset <= kMaskBits, "oracle payload offsets exceed mask width");
    oracle_require(universe.lp_keys.size() <= kMaskBits - kLpBitOffset,
                   "oracle LP universe exceeds mask width");
    return universe;
}

/// Exact factor + AB-provenance + LP projection used by the M2 reducer tests.
/// It intentionally is not a complete MatrixBuilder oracle: sign and
/// Schirokauer columns require a PolynomialContext and are tested elsewhere.
[[nodiscard]] Mask semantic_row(const Relation& relation, const SemanticUniverse& universe) {
    Mask row = 0;
    for (uint32_t factor : relation.rational_factors) {
        oracle_require(factor < kRationalPayloadBits,
                       "rational factor exceeds oracle payload width");
        row ^= Mask{1} << factor;
    }
    for (uint32_t factor : relation.algebraic_factors) {
        oracle_require(factor < kAlgebraicPayloadBits,
                       "algebraic factor exceeds oracle payload width");
        row ^= Mask{1} << (kRationalPayloadBits + factor);
    }

    auto add_ab_pair = [&](int64_t a, uint64_t b) {
        row ^= static_cast<Mask>(ab_pair_payload(a, b)) << kAbBitOffset;
    };
    add_ab_pair(relation.a, relation.b);
    for (const auto& [a, b] : relation.extra_ab_pairs) {
        add_ab_pair(a, b);
    }

    for (const auto& key : exact_lp_support(relation)) {
        const auto it = std::lower_bound(universe.lp_keys.begin(), universe.lp_keys.end(), key);
        oracle_require(it != universe.lp_keys.end() && *it == key,
                       "materialized LP key is absent from source universe");
        const size_t index = static_cast<size_t>(it - universe.lp_keys.begin());
        oracle_require(kLpBitOffset + index < kMaskBits,
                       "oracle LP column exceeds mask shift bound");
        row ^= Mask{1} << (kLpBitOffset + index);
    }
    return row;
}

[[nodiscard]] Mask source_mask(const SourceCombination& combination, const SourceCorpus& corpus) {
    Mask mask = 0;
    oracle_require(combination.generation() == corpus.generation(),
                   "source combination has the wrong generation");
    oracle_require(corpus.size() <= kMaxOracleRows, "source corpus exceeds oracle mask capacity");
    SourceId previous{};
    bool have_previous = false;
    for (const SourceId source : combination.sources()) {
        oracle_require(source.generation == corpus.generation(),
                       "source ID has the wrong generation");
        oracle_require(source.ordinal < corpus.size(), "source ID is outside the corpus");
        oracle_require(source.ordinal < kMaskBits, "source ID exceeds mask shift bound");
        oracle_require(!have_previous || previous < source,
                       "source combination is not strictly canonical");
        mask |= Mask{1} << source.ordinal;
        previous = source;
        have_previous = true;
    }
    oracle_require(mask != 0, "active source combination is empty");
    return mask;
}

[[nodiscard]] Mask xor_selected(Mask selection, std::span<const Mask> rows) {
    oracle_require(rows.size() <= kMaxOracleRows,
                   "selected row set exceeds exhaustive oracle bound");
    oracle_require(rows.size() < kMaskBits, "selected row set exceeds mask shift bound");
    Mask value = 0;
    for (size_t index = 0; index < rows.size(); ++index) {
        if (((selection >> index) & Mask{1}) != 0) {
            value ^= rows[index];
        }
    }
    return value;
}

[[nodiscard]] std::vector<Mask> exact_left_kernel(std::span<const Mask> rows) {
    const Mask limit = subset_count(rows.size());
    std::vector<Mask> kernel;
    for (Mask selection = 0; selection < limit; ++selection) {
        if (xor_selected(selection, rows) == 0) {
            kernel.push_back(selection);
        }
    }
    return kernel;
}

[[nodiscard]] std::vector<Mask> exact_span(std::span<const Mask> rows) {
    const Mask limit = subset_count(rows.size());
    std::vector<Mask> span;
    span.reserve(static_cast<size_t>(limit));
    for (Mask selection = 0; selection < limit; ++selection) {
        span.push_back(xor_selected(selection, rows));
    }
    std::sort(span.begin(), span.end());
    span.erase(std::unique(span.begin(), span.end()), span.end());
    return span;
}

void check_exact_dependency_oracle(const SequentialStructuredReducer& reducer) {
    const SourceCorpus& corpus = reducer.corpus();
    oracle_require(corpus.size() <= kMaxOracleRows,
                   "source corpus exceeds exhaustive oracle bound");
    const auto active = reducer.active_row_ids();
    oracle_require(active.size() <= kMaxOracleRows, "active rows exceed exhaustive oracle bound");

    const SemanticUniverse universe = build_universe(corpus);
    std::vector<Mask> original_rows;
    original_rows.reserve(corpus.size());
    for (size_t ordinal = 0; ordinal < corpus.size(); ++ordinal) {
        original_rows.push_back(semantic_row(corpus.at(corpus.source_id(ordinal)), universe));
    }

    std::vector<Mask> reduced_rows;
    std::vector<Mask> transform_rows;
    reduced_rows.reserve(active.size());
    transform_rows.reserve(active.size());
    for (const StructuredRowId row : active) {
        reduced_rows.push_back(semantic_row(reducer.materialize(row), universe));
        transform_rows.push_back(source_mask(reducer.sources(row), corpus));
    }

    Mask active_sources = 0;
    for (const Mask transform : transform_rows) {
        oracle_require((active_sources & transform) == 0, "active source combinations overlap");
        active_sources |= transform;
    }

    CHECK(reduced_rows.size() == transform_rows.size());
    for (size_t index = 0; index < reduced_rows.size(); ++index) {
        CHECK(reduced_rows[index] == xor_selected(transform_rows[index], original_rows));
    }

    const auto transform_span = exact_span(transform_rows);
    CHECK(transform_span.size() == subset_count(transform_rows.size()));

    const auto original_kernel = exact_left_kernel(original_rows);
    const auto reduced_kernel = exact_left_kernel(reduced_rows);
    std::vector<Mask> mapped_kernel;
    mapped_kernel.reserve(reduced_kernel.size());
    for (const Mask dependency : reduced_kernel) {
        mapped_kernel.push_back(xor_selected(dependency, transform_rows));
    }
    std::sort(mapped_kernel.begin(), mapped_kernel.end());
    const auto unique_end = std::unique(mapped_kernel.begin(), mapped_kernel.end());
    CHECK(unique_end == mapped_kernel.end());
    mapped_kernel.erase(unique_end, mapped_kernel.end());
    CHECK(mapped_kernel == original_kernel);
}

struct StatsSnapshot final {
    size_t input_rows = 0;
    size_t singleton_rows_removed = 0;
    size_t two_way_merges = 0;
    size_t persistence_limited_plans = 0;
    size_t output_rows = 0;
    StructuredReductionStopReason stop_reason = StructuredReductionStopReason::NotStarted;

    [[nodiscard]] bool operator==(const StatsSnapshot&) const noexcept = default;
};

struct ReducerSnapshot final {
    size_t total_rows = 0;
    size_t active_rows = 0;
    std::vector<uint64_t> active_ids;
    std::vector<Mask> source_masks;
    std::vector<std::vector<LargePrimeKey>> lp_keys;
    StatsSnapshot stats{};

    [[nodiscard]] bool operator==(const ReducerSnapshot&) const noexcept = default;
};

[[nodiscard]] ReducerSnapshot snapshot_state(const SequentialStructuredReducer& reducer) {
    ReducerSnapshot snapshot;
    snapshot.total_rows = reducer.total_row_count();
    snapshot.active_rows = reducer.active_row_count();
    for (const StructuredRowId row : reducer.active_row_ids()) {
        snapshot.active_ids.push_back(row.value);
        snapshot.source_masks.push_back(source_mask(reducer.sources(row), reducer.corpus()));
        const auto keys = reducer.lp_keys(row);
        snapshot.lp_keys.emplace_back(keys.begin(), keys.end());
    }
    const auto& stats = reducer.stats();
    snapshot.stats = StatsSnapshot{stats.input_rows,     stats.singleton_rows_removed,
                                   stats.two_way_merges, stats.persistence_limited_plans,
                                   stats.output_rows,    stats.stop_reason};
    return snapshot;
}

[[nodiscard]] std::vector<Relation> singleton_simple_fixture() {
    return {
        make_relation(1, {rational_key(101)}, {0}),
        make_relation(2, {}, {1}),
    };
}

void test_singleton_simple() {
    SequentialStructuredReducer reducer(11, singleton_simple_fixture());
    CHECK(reducer.peel_singletons() == 1);
    CHECK(reducer.active_row_count() == 1);
    CHECK(reducer.stats().singleton_rows_removed == 1);
    const auto active = reducer.active_row_ids();
    CHECK(active.size() == 1);
    CHECK(reducer.lp_keys(active.front()).empty());
    CHECK(source_mask(reducer.sources(active.front()), reducer.corpus()) == (Mask{1} << 1U));
    check_exact_dependency_oracle(reducer);
}

void test_singleton_cascade() {
    const auto p = rational_key(101);
    const auto q = rational_key(103);
    const auto r = rational_key(107);
    std::vector<Relation> relations{
        make_relation(10, {p, q}, {0}),
        make_relation(20, {q, r}, {1}),
        make_relation(30, {r}, {0, 1}),
    };
    SequentialStructuredReducer reducer(12, std::move(relations));
    CHECK(reducer.peel_singletons() == 3);
    CHECK(reducer.active_row_count() == 0);
    CHECK(reducer.stats().singleton_rows_removed == 3);
    CHECK(reducer.peel_singletons() == 0);
    check_exact_dependency_oracle(reducer);
}

void test_singleton_cycle_survives() {
    const auto p = rational_key(101);
    const auto q = rational_key(103);
    const auto r = rational_key(107);
    std::vector<Relation> relations{
        make_relation(40, {p, q}, {0}),
        make_relation(50, {q, r}, {1}),
        make_relation(60, {r, p}, {0, 1}),
    };
    SequentialStructuredReducer reducer(13, std::move(relations));
    const ReducerSnapshot before = snapshot_state(reducer);
    CHECK(reducer.peel_singletons() == 0);
    CHECK(snapshot_state(reducer) == before);
    CHECK(reducer.active_row_count() == 3);
    check_exact_dependency_oracle(reducer);
}

[[nodiscard]] std::vector<Relation> two_way_fixture(uint64_t prime = 101) {
    const auto p = rational_key(prime);
    std::vector<Relation> relations{
        make_relation(70, {p}, {0}),
        make_relation(80, {p}, {1}),
        make_relation(90, {}, {0, 1}),
    };
    relations[0].algebraic_factors = {0};
    relations[1].algebraic_factors = {1};
    relations[2].algebraic_factors = {0, 1};
    return relations;
}

void test_two_way_plan_prepare_commit() {
    SequentialStructuredReducer reducer(21, two_way_fixture());
    const ReducerSnapshot initial = snapshot_state(reducer);
    const auto plans = reducer.plan_two_way_merges();
    CHECK(plans.size() == 1);
    CHECK(snapshot_state(reducer) == initial);
    if (plans.empty()) {
        return;
    }

    const TwoWayMergePlan& plan = plans.front();
    CHECK(plan.generation == 21);
    CHECK(plan.members[0] < plan.members[1]);
    CHECK(plan.witness == rational_key(101));
    CHECK(plan.expected_sources.size() == 2);
    CHECK(plan.expected_lp_keys.empty());

    PreparedTwoWayMerge prepared = reducer.prepare(plan);
    CHECK(prepared.plan() == plan);
    CHECK(exact_lp_support(prepared.materialized_relation()).empty());
    CHECK(snapshot_state(reducer) == initial);

    const StructuredRowId output = reducer.commit(std::move(prepared));
    CHECK(reducer.is_active(output));
    CHECK(!reducer.is_active(plan.members[0]));
    CHECK(!reducer.is_active(plan.members[1]));
    CHECK(reducer.total_row_count() == 4);
    CHECK(reducer.active_row_count() == 2);
    CHECK(reducer.stats().two_way_merges == 1);
    CHECK(reducer.stats().output_rows == 2);
    CHECK(source_mask(reducer.sources(output), reducer.corpus()) == Mask{0b011});

    const std::vector<Mask> parent_masks{Mask{0b001}, Mask{0b010}};
    const std::vector<Mask> output_masks{source_mask(reducer.sources(output), reducer.corpus())};
    std::vector<Mask> even_parent_span{0, parent_masks[0] ^ parent_masks[1]};
    std::sort(even_parent_span.begin(), even_parent_span.end());
    CHECK(exact_span(output_masks) == even_parent_span);
    check_exact_dependency_oracle(reducer);
}

void test_residual_lp_symmetric_difference() {
    const auto p = rational_key(101);
    const auto q = rational_key(103);
    const auto r = algebraic_key(107, 19);
    std::vector<Relation> relations{
        make_relation(100, {p, q}, {0}),
        make_relation(110, {p, r}, {1}),
        make_relation(120, {}, {0, 1}),
    };
    SequentialStructuredReducer reducer(22, std::move(relations));
    const auto plans = reducer.plan_two_way_merges();
    const auto it = std::find_if(plans.begin(), plans.end(),
                                 [&](const TwoWayMergePlan& plan) { return plan.witness == p; });
    CHECK(it != plans.end());
    if (it == plans.end()) {
        return;
    }
    const std::vector<LargePrimeKey> expected{q, r};
    CHECK(it->expected_lp_keys == expected);
    auto prepared = reducer.prepare(*it);
    CHECK(exact_lp_support(prepared.materialized_relation()) == expected);
    const StructuredRowId output = reducer.commit(std::move(prepared));
    const auto output_keys = reducer.lp_keys(output);
    CHECK(std::vector<LargePrimeKey>(output_keys.begin(), output_keys.end()) == expected);
    check_exact_dependency_oracle(reducer);
}

void test_full_row_survives_unchanged() {
    std::vector<Relation> relations{make_relation(130, {}, {0, 2})};
    SequentialStructuredReducer reducer(23, std::move(relations));
    const auto before = reducer.materialize_active();
    CHECK(reducer.peel_singletons() == 0);
    CHECK(reducer.plan_two_way_merges().empty());
    reducer.reduce_two_way();
    CHECK(reducer.active_row_count() == 1);
    CHECK(reducer.stats().stop_reason == StructuredReductionStopReason::NoCandidates);
    CHECK(corpus_equal(reducer.materialize_active(), before));
    check_exact_dependency_oracle(reducer);
}

void check_prepare_failure_without_mutation(SequentialStructuredReducer& reducer,
                                            const TwoWayMergePlan& plan,
                                            StructuredReductionErrorCode code) {
    const ReducerSnapshot before = snapshot_state(reducer);
    check_error(code, [&] { (void)reducer.prepare(plan); });
    CHECK(snapshot_state(reducer) == before);
}

void test_invalid_plans_fail_closed() {
    SequentialStructuredReducer reducer(31, two_way_fixture());
    const auto plans = reducer.plan_two_way_merges();
    CHECK(plans.size() == 1);
    if (plans.empty()) {
        return;
    }
    const TwoWayMergePlan valid = plans.front();

    TwoWayMergePlan duplicate_members = valid;
    duplicate_members.members[1] = duplicate_members.members[0];
    check_prepare_failure_without_mutation(reducer, duplicate_members,
                                           StructuredReductionErrorCode::InvalidPlan);

    TwoWayMergePlan fake_witness = valid;
    fake_witness.witness = rational_key(997);
    check_prepare_failure_without_mutation(reducer, fake_witness,
                                           StructuredReductionErrorCode::InvalidPlan);

    TwoWayMergePlan fake_sources = valid;
    fake_sources.expected_sources = SourceCombination::singleton(reducer.corpus().source_id(2));
    check_prepare_failure_without_mutation(reducer, fake_sources,
                                           StructuredReductionErrorCode::InvalidPlan);

    TwoWayMergePlan fake_residual = valid;
    fake_residual.expected_lp_keys.push_back(rational_key(991));
    check_prepare_failure_without_mutation(reducer, fake_residual,
                                           StructuredReductionErrorCode::InvalidPlan);

    TwoWayMergePlan wrong_generation = valid;
    wrong_generation.generation = 32;
    check_prepare_failure_without_mutation(reducer, wrong_generation,
                                           StructuredReductionErrorCode::InvalidPlan);

    SequentialStructuredReducer other_generation(32, two_way_fixture());
    check_prepare_failure_without_mutation(other_generation, valid,
                                           StructuredReductionErrorCode::InvalidPlan);

    check_error_one_of({StructuredReductionErrorCode::InvalidGeneration,
                        StructuredReductionErrorCode::InvalidSourceCombination},
                       [] {
                           (void)SourceCombination::canonical(
                               31, std::vector<SourceId>{SourceId{31, 0}, SourceId{32, 1}});
                       });
}

void test_degree_three_witness_forgery_fails_closed() {
    const auto p = rational_key(101);
    const auto q = rational_key(103);
    std::vector<Relation> relations{
        make_relation(140, {p}, {0}), make_relation(150, {p}, {1}), make_relation(160, {p}, {2}),
        make_relation(170, {q}, {3}), make_relation(180, {q}, {4}),
    };
    SequentialStructuredReducer reducer(32, std::move(relations));

    const auto plans = reducer.plan_two_way_merges();
    CHECK(plans.size() == 1);
    if (plans.size() != 1) {
        return;
    }
    CHECK(plans.front().witness == q);

    std::vector<StructuredRowId> p_rows;
    for (const StructuredRowId row : reducer.active_row_ids()) {
        const auto keys = reducer.lp_keys(row);
        if (std::find(keys.begin(), keys.end(), p) != keys.end()) {
            p_rows.push_back(row);
        }
    }
    CHECK(p_rows.size() == 3);
    if (p_rows.size() != 3) {
        return;
    }

    // Reuse a valid plan's generation and epoch, and make every derived field
    // exact for two rows selected from a degree-three witness bucket. The
    // witness degree is the only invalid property.
    TwoWayMergePlan forged = plans.front();
    forged.members = {p_rows[0], p_rows[1]};
    forged.witness = p;
    forged.expected_sources = SourceCombination::symmetric_difference(reducer.sources(p_rows[0]),
                                                                      reducer.sources(p_rows[1]));
    forged.expected_lp_keys.clear();

    check_prepare_failure_without_mutation(reducer, forged,
                                           StructuredReductionErrorCode::InvalidPlan);
    check_exact_dependency_oracle(reducer);
}

void test_stale_plan_and_prepared_merge_fail_closed() {
    SequentialStructuredReducer reducer(33, two_way_fixture());
    const auto plans = reducer.plan_two_way_merges();
    CHECK(plans.size() == 1);
    if (plans.empty()) {
        return;
    }
    const TwoWayMergePlan plan = plans.front();
    auto first = reducer.prepare(plan);
    auto stale_prepared = reducer.prepare(plan);
    (void)reducer.commit(std::move(first));

    const ReducerSnapshot committed = snapshot_state(reducer);
    check_error(StructuredReductionErrorCode::StalePlan,
                [&] { (void)reducer.commit(std::move(stale_prepared)); });
    CHECK(snapshot_state(reducer) == committed);

    check_error(StructuredReductionErrorCode::StalePlan, [&] { (void)reducer.prepare(plan); });
    CHECK(snapshot_state(reducer) == committed);
    check_exact_dependency_oracle(reducer);
}

void test_disjoint_commit_invalidates_prepared_epoch() {
    const auto p = rational_key(101);
    const auto q = rational_key(103);
    std::vector<Relation> relations{
        make_relation(190, {p}, {0}),
        make_relation(200, {p}, {1}),
        make_relation(210, {q}, {2}),
        make_relation(220, {q}, {3}),
    };
    SequentialStructuredReducer reducer(34, std::move(relations));

    const auto plans = reducer.plan_two_way_merges();
    CHECK(plans.size() == 2);
    const auto a = std::find_if(plans.begin(), plans.end(),
                                [&](const auto& plan) { return plan.witness == p; });
    const auto b = std::find_if(plans.begin(), plans.end(),
                                [&](const auto& plan) { return plan.witness == q; });
    CHECK(a != plans.end());
    CHECK(b != plans.end());
    if (a == plans.end() || b == plans.end()) {
        return;
    }
    CHECK(a->incidence_epoch == b->incidence_epoch);

    auto prepared_a = reducer.prepare(*a);
    auto prepared_b = reducer.prepare(*b);
    (void)reducer.commit(std::move(prepared_b));

    const ReducerSnapshot after_b = snapshot_state(reducer);
    check_error(StructuredReductionErrorCode::StalePlan,
                [&] { (void)reducer.commit(std::move(prepared_a)); });
    CHECK(snapshot_state(reducer) == after_b);
    check_exact_dependency_oracle(reducer);
}

void test_duplicate_primary_sources_remain_distinct() {
    Relation first = make_relation(42, {}, {0});
    first.b = 7;
    first.extra_ab_pairs = {{9, 2}};

    Relation second = make_relation(42, {}, {1});
    second.b = 7;
    second.extra_ab_pairs = {{11, 3}};

    SourceCorpus corpus(35, {std::move(first), std::move(second)});
    CHECK(!(corpus.source_id(0) == corpus.source_id(1)));
    const auto combination = SourceCombination::canonical(
        corpus.generation(), {corpus.source_id(1), corpus.source_id(0)});
    const Relation materialized = corpus.materialize(combination);

    CHECK(materialized.a == 42);
    CHECK(materialized.b == 7);
    const std::vector<std::pair<int64_t, uint64_t>> expected_pairs{{9, 2}, {42, 7}, {11, 3}};
    CHECK(materialized.extra_ab_pairs == expected_pairs);
    CHECK(materialized.rational_factors == std::vector<uint32_t>({0, 1}));

    const auto universe = build_universe(corpus);
    const Mask expected = semantic_row(corpus.at(corpus.source_id(0)), universe) ^
                          semantic_row(corpus.at(corpus.source_id(1)), universe);
    CHECK(semantic_row(materialized, universe) == expected);
}

void test_nested_residual_merge_preserves_two_dimensional_kernel() {
    const auto p = rational_key(101);
    const auto q = rational_key(103);
    const auto r = algebraic_key(107, 19);

    Relation first = make_relation(1, {p, q}, {0});
    first.algebraic_factors = {0};
    first.extra_ab_pairs = {{33, 1}};
    Relation second = make_relation(2, {p, r}, {1});
    second.algebraic_factors = {1};
    Relation third = make_relation(4, {q, r}, {0, 1});
    third.algebraic_factors = {0, 1};

    Relation matching_full = make_relation(7, {});
    matching_full.extra_ab_pairs = {{33, 1}};
    Relation duplicate_full_a = make_relation(8, {}, {2});
    duplicate_full_a.algebraic_factors = {2};
    Relation duplicate_full_b = make_relation(24, {}, {2});
    duplicate_full_b.algebraic_factors = {2};

    SequentialStructuredReducer reducer(36, {std::move(first), std::move(second), std::move(third),
                                             std::move(matching_full), std::move(duplicate_full_a),
                                             std::move(duplicate_full_b)});

    const auto universe = build_universe(reducer.corpus());
    std::vector<Mask> original_rows;
    for (size_t ordinal = 0; ordinal < reducer.corpus().size(); ++ordinal) {
        original_rows.push_back(
            semantic_row(reducer.corpus().at(reducer.corpus().source_id(ordinal)), universe));
    }
    CHECK(exact_left_kernel(original_rows).size() == 4);

    const auto first_plans = reducer.plan_two_way_merges();
    const auto first_plan =
        std::find_if(first_plans.begin(), first_plans.end(), [&](const TwoWayMergePlan& plan) {
            return plan.witness == p && plan.members[0].value == 0 && plan.members[1].value == 1;
        });
    CHECK(first_plan != first_plans.end());
    if (first_plan == first_plans.end()) {
        return;
    }
    const StructuredRowId residual = reducer.commit(reducer.prepare(*first_plan));
    CHECK(std::vector<LargePrimeKey>(reducer.lp_keys(residual).begin(),
                                     reducer.lp_keys(residual).end()) ==
          std::vector<LargePrimeKey>({q, r}));

    const auto second_plans = reducer.plan_two_way_merges();
    const auto second_plan =
        std::find_if(second_plans.begin(), second_plans.end(), [&](const TwoWayMergePlan& plan) {
            return plan.witness == q &&
                   ((plan.members[0] == StructuredRowId{2} && plan.members[1] == residual) ||
                    (plan.members[1] == StructuredRowId{2} && plan.members[0] == residual));
        });
    CHECK(second_plan != second_plans.end());
    if (second_plan == second_plans.end()) {
        return;
    }
    const StructuredRowId nested = reducer.commit(reducer.prepare(*second_plan));
    CHECK(reducer.lp_keys(nested).empty());
    CHECK(source_mask(reducer.sources(nested), reducer.corpus()) == Mask{0b000111});

    const Relation materialized = reducer.materialize(nested);
    const std::vector<std::pair<int64_t, uint64_t>> expected_pairs{{33, 1}, {2, 1}, {4, 1}};
    CHECK(materialized.extra_ab_pairs == expected_pairs);
    check_exact_dependency_oracle(reducer);
}

void test_candidate_result_size_precedes_witness_order() {
    {
        const auto small_witness = rational_key(101);
        const auto large_witness = rational_key(103);
        SequentialStructuredReducer reducer(
            37, {make_relation(1, {small_witness, rational_key(107)}),
                 make_relation(2, {small_witness, rational_key(109)}),
                 make_relation(3, {large_witness}), make_relation(4, {large_witness})});
        const auto plans = reducer.plan_two_way_merges();
        CHECK(plans.size() == 2);
        if (plans.size() == 2) {
            CHECK(plans[0].witness == large_witness);
            CHECK(plans[0].expected_lp_keys.empty());
            CHECK(plans[1].witness == small_witness);
            CHECK(plans[1].expected_lp_keys.size() == 2);
        }
    }

    {
        const auto small_witness = rational_key(101);
        const auto large_witness = rational_key(103);
        const auto seed = rational_key(107);
        SequentialStructuredReducer reducer(
            38, {make_relation(11, {seed, small_witness}), make_relation(12, {seed}),
                 make_relation(13, {small_witness}), make_relation(14, {large_witness}),
                 make_relation(15, {large_witness})});

        const auto initial = reducer.plan_two_way_merges();
        const auto seed_plan = std::find_if(initial.begin(), initial.end(),
                                            [&](const auto& plan) { return plan.witness == seed; });
        CHECK(seed_plan != initial.end());
        if (seed_plan == initial.end()) {
            return;
        }
        (void)reducer.commit(reducer.prepare(*seed_plan));

        const auto plans = reducer.plan_two_way_merges();
        CHECK(plans.size() == 2);
        if (plans.size() == 2) {
            CHECK(plans[0].witness == large_witness);
            CHECK(plans[0].expected_sources.size() == 2);
            CHECK(plans[1].witness == small_witness);
            CHECK(plans[1].expected_sources.size() == 3);
            CHECK(plans[0].expected_lp_keys.size() == plans[1].expected_lp_keys.size());
        }
    }
}

void test_persistence_limited_plan_is_skipped_once() {
    const auto p = rational_key(1009);
    Relation first = make_relation(31, {});
    Relation second = make_relation(32, {});
    for (size_t i = 0; i < 9; ++i) {
        first.rational_large_prime.emplace_back(p.prime, 0, uint8_t{255});
        second.rational_large_prime.emplace_back(p.prime, 0, uint8_t{255});
    }

    SequentialStructuredReducer reducer(39, {std::move(first), std::move(second)});
    const ReducerSnapshot before = snapshot_state(reducer);
    CHECK(reducer.plan_two_way_merges().size() == 1);
    reducer.reduce_two_way();
    const ReducerSnapshot after = snapshot_state(reducer);

    CHECK(after.total_rows == before.total_rows);
    CHECK(after.active_rows == before.active_rows);
    CHECK(after.active_ids == before.active_ids);
    CHECK(after.source_masks == before.source_masks);
    CHECK(after.lp_keys == before.lp_keys);
    CHECK(reducer.stats().two_way_merges == 0);
    CHECK(reducer.stats().persistence_limited_plans == 1);
    CHECK(reducer.stats().stop_reason == StructuredReductionStopReason::PersistenceLimit);
    CHECK(reducer.active_row_count() == 2);
}

[[nodiscard]] std::vector<Relation> ordered_candidate_fixture() {
    const std::vector<LargePrimeKey> keys{
        rational_key(103),
        algebraic_key(101, 7),
        rational_key(101),
        algebraic_key(101, 0),
    };
    std::vector<Relation> relations;
    relations.reserve(keys.size() * 2);
    int64_t a = 200;
    uint32_t payload = 0;
    for (const auto& key : keys) {
        relations.push_back(make_relation(a++, {key}, {payload++}));
        relations.push_back(make_relation(a++, {key}, {payload++}));
    }
    return relations;
}

[[nodiscard]] bool plan_less(const TwoWayMergePlan& lhs, const TwoWayMergePlan& rhs) {
    if (!(lhs.witness == rhs.witness)) {
        return lhs.witness < rhs.witness;
    }
    if (!(lhs.members[0] == rhs.members[0])) {
        return lhs.members[0] < rhs.members[0];
    }
    return lhs.members[1] < rhs.members[1];
}

void test_candidate_total_order_and_repeatability() {
    SequentialStructuredReducer first(41, ordered_candidate_fixture());
    SequentialStructuredReducer second(41, ordered_candidate_fixture());
    const auto first_plans = first.plan_two_way_merges();
    const auto second_plans = second.plan_two_way_merges();
    CHECK(first_plans == second_plans);
    CHECK(first_plans.size() == 4);
    for (size_t index = 1; index < first_plans.size(); ++index) {
        CHECK(plan_less(first_plans[index - 1], first_plans[index]));
    }
    const std::vector<LargePrimeKey> expected_witnesses{
        rational_key(101),
        algebraic_key(101, 0),
        algebraic_key(101, 7),
        rational_key(103),
    };
    if (first_plans.size() == expected_witnesses.size()) {
        for (size_t index = 0; index < expected_witnesses.size(); ++index) {
            CHECK(first_plans[index].witness == expected_witnesses[index]);
        }
    }

    first.reduce_two_way();
    second.reduce_two_way();
    CHECK(snapshot_state(first) == snapshot_state(second));
    CHECK(corpus_equal(first.materialize_active(), second.materialize_active()));
    CHECK(first.stats().two_way_merges == 4);
    CHECK(first.active_row_count() == 4);
    check_exact_dependency_oracle(first);
    check_exact_dependency_oracle(second);
}

} // namespace

int main() {
    test_singleton_simple();
    test_singleton_cascade();
    test_singleton_cycle_survives();
    test_two_way_plan_prepare_commit();
    test_residual_lp_symmetric_difference();
    test_full_row_survives_unchanged();
    test_invalid_plans_fail_closed();
    test_degree_three_witness_forgery_fails_closed();
    test_stale_plan_and_prepared_merge_fail_closed();
    test_disjoint_commit_invalidates_prepared_epoch();
    test_duplicate_primary_sources_remain_distinct();
    test_nested_residual_merge_preserves_two_dimensional_kernel();
    test_candidate_result_size_precedes_witness_order();
    test_persistence_limited_plan_is_skipped_once();
    test_candidate_total_order_and_repeatability();

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " structured-filter checks failed\n";
        return 1;
    }
    std::cout << "All " << checks << " structured-filter checks passed\n";
    return 0;
}
