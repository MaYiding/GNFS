#include "gnfs/api/detail/solver_handoff.hpp"
#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/reduction_engine.hpp"
#include "gnfs/relation/structured_filter_profile.hpp"
#include "gnfs/util/process.hpp"
#include "gnfs/util/temp_path.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

using gnfs::api::detail::handoff_after_collection;
using gnfs::api::detail::SolverHandoffInfo;
using gnfs::core::Relation;
using gnfs::relation::CollectorConfig;
using gnfs::relation::CollectorUniqueOOCPrefixSource;
using gnfs::relation::corpus_digest;
using gnfs::relation::CorpusDigest;
using gnfs::relation::RawRelationSnapshot;
using gnfs::relation::ReductionStrategy;
using gnfs::relation::RelationCollector;
using gnfs::relation::RelationReductionConfig;
using gnfs::relation::RelationReductionEngine;
using gnfs::relation::RelationReductionResult;
using gnfs::relation::RelationReductionStats;
using gnfs::relation::RelationSourceCombination;
using gnfs::relation::RelationSourceCombinationHash;
using gnfs::relation::select_reduction_strategy;
using gnfs::relation::StructuredFilterSelection;
using gnfs::relation::StructuredReductionBudget;
using gnfs::relation::StructuredReductionError;
using gnfs::relation::StructuredReductionErrorCode;
using gnfs::relation::StructuredReductionStopReason;

using PreparedBorrowedStructuredInput = RelationReductionEngine::PreparedBorrowedStructuredInput;

static_assert(!std::is_copy_constructible_v<RawRelationSnapshot>);
static_assert(!std::is_copy_assignable_v<RawRelationSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<RawRelationSnapshot>);
static_assert(!std::is_copy_constructible_v<RelationReductionResult>);
static_assert(!std::is_copy_assignable_v<RelationReductionResult>);
static_assert(std::is_nothrow_move_constructible_v<RelationReductionResult>);
static_assert(!std::is_same_v<RawRelationSnapshot, RelationReductionResult>);
static_assert(!std::is_copy_constructible_v<PreparedBorrowedStructuredInput>);
static_assert(!std::is_copy_assignable_v<PreparedBorrowedStructuredInput>);
static_assert(std::is_nothrow_move_constructible_v<PreparedBorrowedStructuredInput>);

namespace {

int failures = 0;

struct OOCArtifacts final {
    explicit OOCArtifacts(std::string artifact_base) : base(std::move(artifact_base)) {}

    ~OOCArtifacts() {
        std::error_code ignored;
        std::filesystem::remove(base + ".relidx", ignored);
        ignored.clear();
        std::filesystem::remove(base + ".reldata", ignored);
        ignored.clear();
        std::filesystem::remove(base + ".gnfs-ooc-cleanup-v1.lock", ignored);
        ignored.clear();
        std::filesystem::remove_all(base + ".gnfs-sink-lease", ignored);
        ignored.clear();
        std::filesystem::remove(base + ".gnfs-sink-lease.gnfs-ooc-cleanup-v1.lock", ignored);
    }

    std::string base;
};

class BorrowedVectorRelationSource final {
public:
    explicit BorrowedVectorRelationSource(const std::vector<Relation>& relations,
                                          std::optional<size_t> failing_ordinal = std::nullopt)
        : relations_(&relations), failing_ordinal_(failing_ordinal) {}

    BorrowedVectorRelationSource(const BorrowedVectorRelationSource&) = delete;
    BorrowedVectorRelationSource& operator=(const BorrowedVectorRelationSource&) = delete;
    BorrowedVectorRelationSource(BorrowedVectorRelationSource&&) = delete;
    BorrowedVectorRelationSource& operator=(BorrowedVectorRelationSource&&) = delete;

    [[nodiscard]] size_t count() const noexcept {
        ++count_calls_;
        return relations_->size();
    }

    [[nodiscard]] Relation read(size_t ordinal) const {
        ++read_calls_;
        if (failing_ordinal_ == ordinal) {
            throw std::runtime_error("injected borrowed relation source read failure");
        }
        return relations_->at(ordinal);
    }

    [[nodiscard]] size_t count_calls() const noexcept {
        return count_calls_;
    }

    [[nodiscard]] size_t read_calls() const noexcept {
        return read_calls_;
    }

private:
    const std::vector<Relation>* relations_;
    std::optional<size_t> failing_ordinal_;
    mutable size_t count_calls_ = 0;
    mutable size_t read_calls_ = 0;
};

static_assert(gnfs::relation::RelationSource<BorrowedVectorRelationSource>);
static_assert(!std::is_copy_constructible_v<BorrowedVectorRelationSource>);

class SpoofedUniqueRelationSource final {
public:
    static constexpr bool provides_unique_relations = true;

    [[nodiscard]] size_t count() const noexcept {
        return 0;
    }

    [[nodiscard]] Relation read(size_t) const {
        throw std::out_of_range("spoofed source is empty");
    }
};

template <typename Source>
concept DirectBorrowedStructuredSource =
    requires(uint64_t generation, const Source& source, const RelationReductionConfig& config) {
        {
            RelationReductionEngine::reduce_direct_borrowed_structured(generation, source, config)
        } -> std::same_as<RelationReductionResult>;
    };

static_assert(gnfs::relation::RelationSource<SpoofedUniqueRelationSource>);
static_assert(SpoofedUniqueRelationSource::provides_unique_relations);
static_assert(!DirectBorrowedStructuredSource<BorrowedVectorRelationSource>);
static_assert(!DirectBorrowedStructuredSource<SpoofedUniqueRelationSource>);
static_assert(DirectBorrowedStructuredSource<CollectorUniqueOOCPrefixSource>);

[[nodiscard]] std::string unique_ooc_base(const char* label) {
    static uint64_t sequence = 0;
    return gnfs::util::temp_path("gnfs_reduction_" + std::string(label) + "_" +
                                 std::to_string(gnfs::util::process_id()) + "_" +
                                 std::to_string(sequence++));
}

[[nodiscard]] std::string private_sink_base(const std::string& requested_base) {
    return (std::filesystem::path(requested_base + ".gnfs-sink-lease") / "corpus").string();
}

[[nodiscard]] bool private_sink_exists(const std::string& requested_base) {
    const std::string base = private_sink_base(requested_base);
    return std::filesystem::exists(base + ".relidx") &&
           std::filesystem::exists(base + ".reldata") &&
           std::filesystem::is_directory(requested_base + ".gnfs-sink-lease");
}

[[nodiscard]] bool private_sink_absent(const std::string& requested_base) {
    return !std::filesystem::exists(requested_base + ".gnfs-sink-lease");
}

template <typename Value>
[[nodiscard]] bool overwrite_binary_value(const std::string& path, std::streamoff offset,
                                          const Value& value) {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.seekp(offset);
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    stream.flush();
    return static_cast<bool>(stream);
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " << #condition   \
                      << '\n';                                                                     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

Relation make_full(int64_t a) {
    Relation relation(a, 1);
    relation.rational_factors.push_back(static_cast<uint32_t>(a));
    return relation;
}

Relation make_partial(int64_t a, std::initializer_list<uint64_t> large_primes) {
    Relation relation(a, 1);
    relation.rational_factors.push_back(static_cast<uint32_t>(a));
    for (uint64_t prime : large_primes) {
        relation.rational_large_prime.emplace_back(prime, uint8_t{1});
    }
    return relation;
}

std::vector<Relation> make_shared_primary_corpus() {
    // h is shared by A/B/C, while u1/u2/u3 connect each hub row to a leaf.
    // Standard V0 materializes A+D, B+E and C+F. V3 also materializes the
    // distinct A+B+D+E source combination with the same primary A.
    constexpr uint64_t h = 101;
    constexpr uint64_t u1 = 103;
    constexpr uint64_t u2 = 107;
    constexpr uint64_t u3 = 109;
    return {
        make_partial(10, {h, u1}), make_partial(20, {h, u2}), make_partial(30, {h, u3}),
        make_partial(40, {u1}),    make_partial(50, {u2}),    make_partial(60, {u3}),
    };
}

std::vector<Relation> make_weight_stratified_incidence_corpus() {
    // Across these six rows, 101 has weight 1, 103 has weight 2, 107 has
    // weight 3, and 109 has weight 4. The final row's persisted even exponent
    // cancels canonically, leaving empty GF(2) LP support.
    constexpr uint64_t weight_1 = 101;
    constexpr uint64_t weight_2 = 103;
    constexpr uint64_t weight_3 = 107;
    constexpr uint64_t weight_4 = 109;
    Relation canonical_empty = make_full(60);
    canonical_empty.rational_large_prime.emplace_back(113, uint8_t{2});
    return {
        make_partial(10, {weight_1, weight_4}), make_partial(20, {weight_2, weight_3, weight_4}),
        make_partial(30, {weight_2, weight_4}), make_partial(40, {weight_3, weight_4}),
        make_partial(50, {weight_3}),           std::move(canonical_empty),
    };
}

std::vector<Relation> make_rich_digest_corpus() {
    Relation first(-17, 19);
    first.rational_factors = {2, 3};
    first.algebraic_factors = {5, 7};
    first.rational_large_prime = {
        gnfs::core::PrimePower(101, 11, uint8_t{1}),
        gnfs::core::PrimePower(103, 13, uint8_t{2}),
    };
    first.algebraic_large_prime = {
        gnfs::core::PrimePower(107, 17, uint8_t{3}),
        gnfs::core::PrimePower(109, 19, uint8_t{4}),
    };
    first.extra_ab_pairs = {{-23, 29}, {31, 37}};

    Relation second(41, 43);
    second.rational_factors = {11, 13};
    second.algebraic_factors = {17, 19};
    second.rational_large_prime = {gnfs::core::PrimePower(127, 23, uint8_t{5})};
    second.algebraic_large_prime = {gnfs::core::PrimePower(131, 29, uint8_t{6})};
    second.extra_ab_pairs = {{47, 53}};
    return {std::move(first), std::move(second)};
}

RelationReductionConfig lp_config(ReductionStrategy strategy) {
    RelationReductionConfig config;
    config.large_primes_enabled = true;
    config.merge_rounds = 10;
    config.strategy = strategy;
    return config;
}

RelationReductionConfig structured_config(uint32_t workers = 1, size_t batch_width = 4) {
    RelationReductionConfig config;
    config.large_primes_enabled = true;
    config.merge_rounds = 0;
    config.strategy = ReductionStrategy::Structured;

    StructuredReductionBudget budget(128, 128, 0, 2048);
    budget.max_commits = 128;
    budget.max_source_atoms_per_output = 64;
    budget.max_odd_lp_keys_per_output =
        static_cast<size_t>(Relation::MAX_SERIALIZED_LARGE_PRIMES) * 2;
    budget.max_materialized_pairs_per_output = 64;

    RelationReductionConfig::StructuredExecutionConfig structured{
        std::move(budget),
        {.max_batch_candidates = batch_width, .worker_count = workers},
        {.max_rows_per_shard = 3, .worker_count = workers},
        gnfs::relation::TreeBasisPlanner::DeterministicMst,
        {},
        gnfs::relation::OOCCleanupPolicy::RemoveArtifacts,
        {},
    };
    config.structured = std::move(structured);
    return config;
}

RelationReductionConfig experimental_profile_config(size_t input_rows, uint32_t workers) {
    RelationReductionConfig config;
    config.large_primes_enabled = true;
    config.merge_rounds = 0;
    config.strategy = ReductionStrategy::Structured;
    config.structured =
        gnfs::relation::make_structured_filter_experimental_config(input_rows, workers);
    return config;
}

bool equal_relation(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

bool equal_corpus(const std::vector<Relation>& lhs, const std::vector<Relation>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!equal_relation(lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

bool equal_collector_stats(const gnfs::relation::CollectorStats& lhs,
                           const gnfs::relation::CollectorStats& rhs) {
    return lhs.total_relations == rhs.total_relations && lhs.full_relations == rhs.full_relations &&
           lhs.partial_1lp == rhs.partial_1lp && lhs.partial_2lp == rhs.partial_2lp &&
           lhs.duplicates_rejected == rhs.duplicates_rejected &&
           lhs.invalid_rejected == rhs.invalid_rejected &&
           lhs.n_divisible_rejected == rhs.n_divisible_rejected;
}

void add_relations(RelationCollector& collector, const std::vector<Relation>& relations) {
    for (const auto& relation : relations) {
        Relation copy = relation;
        CHECK(collector.add(std::move(copy)));
    }
}

[[nodiscard]] gnfs::relation::RelationCorpus
make_owned_ooc_corpus(uint64_t generation, const std::string& base,
                      const std::vector<Relation>& relations) {
    gnfs::relation::OOCRelationWriter writer(base);
    for (const auto& relation : relations) {
        (void)writer.write(relation);
    }
    return gnfs::relation::RelationCorpus::from_owned_finalized_ooc(
        generation, writer, gnfs::relation::OOCCleanupPolicy::RemoveArtifacts);
}

template <typename Fn> bool throws_invalid_argument(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

template <typename Fn> bool throws_logic_error(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::logic_error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

template <typename Fn> bool throws_runtime_error(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::runtime_error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

template <typename Fn>
bool throws_structured_error(StructuredReductionErrorCode expected, Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const StructuredReductionError& error) {
        return error.code() == expected;
    } catch (...) {
        return false;
    }
    return false;
}

void check_common_stats(const RelationReductionResult& result) {
    CHECK(result.stats.input_relations == 6);
    CHECK(result.stats.raw_duplicates_removed == 0);
    CHECK(result.stats.filter.input_relations == 6);
    CHECK(result.stats.filter.output_relations == 6);
    CHECK(result.stats.filter.singletons_removed == 0);
    CHECK(result.stats.pre_merge_lp_histogram.unique_keys == 4);
    CHECK(result.stats.pre_merge_lp_histogram.weight_1 == 0);
    CHECK(result.stats.pre_merge_lp_histogram.weight_2 == 3);
    CHECK(result.stats.pre_merge_lp_histogram.weight_3 == 1);
    CHECK(result.stats.pre_merge_lp_histogram.weight_4plus == 0);
    CHECK(result.stats.separated_full_relations == 0);
    CHECK(result.stats.separated_partial_relations == 6);
    CHECK(result.stats.output_relations == result.size());
    CHECK(result.stats.output_lp_columns ==
          gnfs::relation::count_unique_lp_keys(result.materialize_relations()));
    CHECK(result.stats.output_digest == corpus_digest(result.relation_corpus()));
}

void test_generation_and_no_large_primes() {
    {
        std::vector<Relation> input{make_full(1), make_full(2)};
        auto result = RelationReductionEngine::reduce(RawRelationSnapshot(47, std::move(input)),
                                                      RelationReductionConfig{});

        CHECK(result.generation == 47);
        CHECK(result.size() == 2);
        CHECK(result.stats.strategy == ReductionStrategy::NoLargePrimes);
        CHECK(result.stats.input_relations == 2);
        CHECK(result.stats.raw_duplicates_removed == 0);
        CHECK(result.stats.filter.input_relations == 2);
        CHECK(result.stats.filter.output_relations == 2);
        CHECK(result.stats.output_relations == 2);
        CHECK(result.stats.output_lp_columns == 0);
        CHECK(result.stats.merged_relations == 0);
        CHECK(result.stats.output_digest == corpus_digest(result.relation_corpus()));
    }

    // Disabling ordinary LP admission does not erase Special-Q ideal columns
    // already present in relations. A shared key survives singleton filtering
    // and must remain part of every effective matrix-column estimate.
    {
        std::vector<Relation> input{
            make_partial(3, {101}),
            make_partial(5, {101}),
        };
        auto result = RelationReductionEngine::reduce(RawRelationSnapshot(48, std::move(input)),
                                                      RelationReductionConfig{});

        CHECK(result.generation == 48);
        CHECK(result.size() == 2);
        CHECK(result.stats.strategy == ReductionStrategy::NoLargePrimes);
        CHECK(result.stats.output_lp_columns == 1);
        CHECK(!gnfs::relation::has_effective_column_excess(3, 2, result.stats.output_lp_columns));
        CHECK(gnfs::relation::has_effective_column_excess(4, 2, result.stats.output_lp_columns));
    }
}

void test_structured_policy_maps_to_named_legacy_strategy() {
    constexpr std::array legacy_strategies{
        ReductionStrategy::NoLargePrimes, ReductionStrategy::FilterOnly,
        ReductionStrategy::StandardV0,    ReductionStrategy::StandardV0WithV3,
        ReductionStrategy::CliqueV0,
    };
    const auto unset = gnfs::relation::decide_structured_filter_policy(
        gnfs::relation::parse_structured_filter_mode(nullptr), true, false);
    const auto explicit_off = gnfs::relation::decide_structured_filter_policy(
        gnfs::relation::parse_structured_filter_mode("0"), true, false);
    for (const ReductionStrategy legacy : legacy_strategies) {
        CHECK(select_reduction_strategy(unset, legacy) == legacy);
        CHECK(select_reduction_strategy(explicit_off, legacy) == legacy);
    }

    const auto forced_on = gnfs::relation::decide_structured_filter_policy(
        gnfs::relation::parse_structured_filter_mode("1"), true, false);
    CHECK(forced_on.selection == StructuredFilterSelection::Structured);
    CHECK(select_reduction_strategy(forced_on, ReductionStrategy::StandardV0) ==
          ReductionStrategy::Structured);
    CHECK(throws_invalid_argument(
        [&] { (void)select_reduction_strategy(forced_on, ReductionStrategy::Structured); }));
}

void test_illegal_combinations_fail_closed() {
    CHECK(throws_invalid_argument([] {
        RawRelationSnapshot invalid(0, {});
        (void)invalid;
    }));

    auto no_lp_with_standard = lp_config(ReductionStrategy::StandardV0);
    no_lp_with_standard.large_primes_enabled = false;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), no_lp_with_standard);
    }));

    auto no_lp_with_filter_only = lp_config(ReductionStrategy::FilterOnly);
    no_lp_with_filter_only.large_primes_enabled = false;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), no_lp_with_filter_only);
    }));

    RelationReductionConfig lp_with_no_strategy;
    lp_with_no_strategy.large_primes_enabled = true;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), lp_with_no_strategy);
    }));

    auto zero_rounds = lp_config(ReductionStrategy::CliqueV0);
    zero_rounds.merge_rounds = 0;
    CHECK(throws_invalid_argument(
        [&] { (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), zero_rounds); }));

    auto unknown_strategy = lp_config(static_cast<ReductionStrategy>(255));
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(1, {}), unknown_strategy);
    }));

    RawRelationSnapshot mutated_generation(1, {});
    mutated_generation.generation = 0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(std::move(mutated_generation),
                                              RelationReductionConfig{});
    }));

    RawRelationSnapshot mismatched_generation(8, {});
    mismatched_generation.generation = 9;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(std::move(mismatched_generation),
                                              RelationReductionConfig{});
    }));

    auto missing_structured_config = lp_config(ReductionStrategy::Structured);
    missing_structured_config.merge_rounds = 0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(2, {}),
                                              missing_structured_config);
    }));

    auto structured_without_lp = structured_config();
    structured_without_lp.large_primes_enabled = false;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(3, {}), structured_without_lp);
    }));

    auto legacy_with_structured_config = structured_config();
    legacy_with_structured_config.strategy = ReductionStrategy::StandardV0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(4, {}),
                                              legacy_with_structured_config);
    }));

    auto invalid_parallel = structured_config();
    invalid_parallel.structured->parallel.worker_count = 0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(5, {}), invalid_parallel);
    }));

    auto invalid_incidence = structured_config();
    invalid_incidence.structured->incidence.max_rows_per_shard = 0;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(6, {}), invalid_incidence);
    }));

    auto invalid_budget = structured_config();
    invalid_budget.structured->budget.max_pivot_weight = 9;
    RawRelationSnapshot preserved_snapshot(7, {make_partial(1, {101})});
    CHECK(throws_structured_error(StructuredReductionErrorCode::InvalidInput, [&] {
        (void)RelationReductionEngine::reduce(std::move(preserved_snapshot), invalid_budget);
    }));
    CHECK(preserved_snapshot.size() == 1);
    CHECK(preserved_snapshot.read(0).a == 1);
}

void test_exact_abpair_dedup_preserves_old_collision() {
    Relation first(0, 1);
    first.rational_factors = {101};
    Relation second(static_cast<int64_t>(UINT64_C(3) << 32U), 2);
    second.rational_factors = {103};

    std::vector<Relation> input{first, second};
    const CorpusDigest raw_digest = corpus_digest(input);
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(501, std::move(input)),
                                                  RelationReductionConfig{});

    CHECK(result.stats.input_relations == 2);
    CHECK(result.stats.raw_duplicates_removed == 0);
    CHECK(result.stats.raw_input_digest == raw_digest);
    CHECK(result.size() == 2);
    CHECK(result.read(0).ab() == first.ab());
    CHECK(result.read(1).ab() == second.ab());
}

void test_exact_abpair_dedup_keeps_first_occurrence() {
    Relation first(17, 19);
    first.rational_factors = {101};
    Relation duplicate(17, 19);
    duplicate.rational_factors = {103};
    Relation tail(23, 29);
    tail.rational_factors = {107};

    std::vector<Relation> input{first, duplicate, tail};
    const CorpusDigest raw_digest = corpus_digest(input);
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(502, std::move(input)),
                                                  RelationReductionConfig{});

    CHECK(result.stats.input_relations == 3);
    CHECK(result.stats.raw_duplicates_removed == 1);
    CHECK(result.stats.raw_input_digest == raw_digest);
    CHECK(result.stats.filter.input_relations == 2);
    CHECK(result.size() == 2);
    CHECK(result.read(0).ab() == first.ab());
    CHECK(result.read(0).rational_factors == first.rational_factors);
    CHECK(result.read(1).ab() == tail.ab());
}

void test_merged_input_fails_before_dedup() {
    Relation raw(31, 37);
    Relation merged_duplicate(31, 37);
    merged_duplicate.extra_ab_pairs.emplace_back(41, 43);

    std::vector<Relation> input{raw, merged_duplicate};
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(503, std::move(input)),
                                              RelationReductionConfig{});
    }));
}

void test_digest_covers_every_field_and_order() {
    const auto baseline = make_rich_digest_corpus();
    const CorpusDigest expected = corpus_digest(baseline);

    gnfs::relation::CorpusDigestAccumulator streamed(baseline.size());
    for (const auto& relation : baseline) {
        streamed.append(relation);
    }
    CHECK(streamed.finish() == expected);
    CHECK(streamed.finish() == expected);
    CHECK(throws_logic_error([&] { streamed.append(baseline.front()); }));

    gnfs::relation::CorpusDigestAccumulator short_stream(baseline.size());
    short_stream.append(baseline.front());
    CHECK(throws_logic_error([&] { (void)short_stream.finish(); }));

    auto expect_change = [&](auto mutate) {
        auto changed = baseline;
        mutate(changed);
        CHECK(!(corpus_digest(changed) == expected));
    };

    expect_change([](auto& corpus) { ++corpus[0].a; });
    expect_change([](auto& corpus) { ++corpus[0].b; });
    expect_change([](auto& corpus) { ++corpus[0].rational_factors[0]; });
    expect_change([](auto& corpus) {
        std::swap(corpus[0].rational_factors[0], corpus[0].rational_factors[1]);
    });
    expect_change([](auto& corpus) { ++corpus[0].algebraic_factors[0]; });
    expect_change([](auto& corpus) {
        std::swap(corpus[0].algebraic_factors[0], corpus[0].algebraic_factors[1]);
    });
    expect_change([](auto& corpus) { ++corpus[0].rational_large_prime[0].p; });
    expect_change([](auto& corpus) { ++corpus[0].rational_large_prime[0].r; });
    expect_change([](auto& corpus) { ++corpus[0].rational_large_prime[0].e; });
    expect_change([](auto& corpus) {
        std::swap(corpus[0].rational_large_prime[0], corpus[0].rational_large_prime[1]);
    });
    expect_change([](auto& corpus) { ++corpus[0].algebraic_large_prime[0].p; });
    expect_change([](auto& corpus) { ++corpus[0].algebraic_large_prime[0].r; });
    expect_change([](auto& corpus) { ++corpus[0].algebraic_large_prime[0].e; });
    expect_change([](auto& corpus) {
        std::swap(corpus[0].algebraic_large_prime[0], corpus[0].algebraic_large_prime[1]);
    });
    expect_change([](auto& corpus) { ++corpus[0].extra_ab_pairs[0].first; });
    expect_change([](auto& corpus) { ++corpus[0].extra_ab_pairs[0].second; });
    expect_change(
        [](auto& corpus) { std::swap(corpus[0].extra_ab_pairs[0], corpus[0].extra_ab_pairs[1]); });
    expect_change([](auto& corpus) { corpus[0].rational_factors.push_back(23); });
    expect_change([](auto& corpus) { std::swap(corpus[0], corpus[1]); });
}

void test_filter_only_preserves_filtered_partials() {
    std::vector<Relation> input{
        make_partial(1, {101}),
        make_partial(2, {101}),
        make_partial(3, {103}),
    };
    auto config = lp_config(ReductionStrategy::FilterOnly);
    config.merge_rounds = 0;
    auto result =
        RelationReductionEngine::reduce(RawRelationSnapshot(504, std::move(input)), config);

    CHECK(result.stats.strategy == ReductionStrategy::FilterOnly);
    CHECK(result.stats.filter.input_relations == 3);
    CHECK(result.stats.filter.singletons_removed == 1);
    CHECK(result.stats.pre_merge_lp_histogram.unique_keys == 1);
    CHECK(result.stats.pre_merge_lp_histogram.weight_2 == 1);
    CHECK(result.stats.separated_full_relations == 0);
    CHECK(result.stats.separated_partial_relations == 0);
    CHECK(result.stats.merged_relations == 0);
    CHECK(result.stats.standard_v0.output_relations == 0);
    CHECK(result.stats.clique_v0.input_relations == 0);
    CHECK(result.stats.output_relations == 2);
    CHECK(result.stats.output_lp_columns == 1);
    CHECK(result.size() == 2);
    CHECK(!result.read(0).is_full());
    CHECK(!result.read(1).is_full());
    CHECK(result.read(0).a == 1);
    CHECK(result.read(1).a == 2);
}

void test_fixed_digest_golden() {
    std::vector<Relation> input{
        make_partial(1, {101}),
        make_partial(2, {101}),
        make_full(3),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(505, std::move(input)),
                                                  lp_config(ReductionStrategy::StandardV0));

    constexpr CorpusDigest expected_raw{0xc164e37a1f9065fdULL, 0x50d03aafd1a8c057ULL};
    constexpr CorpusDigest expected_output{0x1b01daeb3e04f2dbULL, 0x704af9c872e1e7e7ULL};
    CHECK(result.stats.raw_input_digest == expected_raw);
    CHECK(result.stats.output_digest == expected_output);
}

void test_singleton_purge_precedes_merge() {
    std::vector<Relation> input{
        make_partial(1, {101}),
        make_partial(2, {101}),
        make_partial(3, {103}),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(88, std::move(input)),
                                                  lp_config(ReductionStrategy::StandardV0));

    CHECK(result.generation == 88);
    CHECK(result.stats.input_relations == 3);
    CHECK(result.stats.filter.input_relations == 3);
    CHECK(result.stats.filter.output_relations == 2);
    CHECK(result.stats.filter.singletons_removed == 1);
    CHECK(result.stats.pre_merge_lp_histogram.unique_keys == 1);
    CHECK(result.stats.pre_merge_lp_histogram.weight_2 == 1);
    CHECK(result.stats.separated_full_relations == 0);
    CHECK(result.stats.separated_partial_relations == 2);
    CHECK(result.stats.standard_v0.input_1lp == 2);
    CHECK(result.stats.standard_v0.full_produced == 1);
    CHECK(result.stats.merged_relations == 1);
    CHECK(result.stats.output_relations == 1);
    CHECK(result.size() == 1);
    CHECK(result.stats.output_lp_columns == 0);
}

void test_standard_v0_and_explicit_off_unset_equivalence() {
    const auto config = lp_config(ReductionStrategy::StandardV0);

    unsetenv("GNFS_V0_BFS");
    unsetenv("GNFS_CASCADE_V3");
    auto unset_result = RelationReductionEngine::reduce(
        RawRelationSnapshot(101, make_shared_primary_corpus()), config);

    setenv("GNFS_V0_BFS", "0", 1);
    setenv("GNFS_CASCADE_V3", "0", 1);
    auto off_result = RelationReductionEngine::reduce(
        RawRelationSnapshot(101, make_shared_primary_corpus()), config);
    unsetenv("GNFS_V0_BFS");
    unsetenv("GNFS_CASCADE_V3");

    CHECK(equal_corpus(unset_result.materialize_relations(), off_result.materialize_relations()));
    CHECK(unset_result.generation == 101);
    CHECK(unset_result.stats.strategy == ReductionStrategy::StandardV0);
    CHECK(unset_result.stats.standard_v0.output_relations == unset_result.stats.merged_relations);
    CHECK(unset_result.stats.v3_relations_added == 0);
    CHECK(unset_result.stats.clique_v0.input_relations == 0);
    check_common_stats(unset_result);
}

void test_standard_v0_with_v3_exact_dedup() {
    auto result =
        RelationReductionEngine::reduce(RawRelationSnapshot(202, make_shared_primary_corpus()),
                                        lp_config(ReductionStrategy::StandardV0WithV3));

    CHECK(result.generation == 202);
    CHECK(result.stats.strategy == ReductionStrategy::StandardV0WithV3);
    CHECK(result.stats.v3.input_relations == 6);
    CHECK(result.stats.v3_relations_added > 0);
    CHECK(result.stats.v3_duplicates_skipped > 0);
    CHECK(result.stats.merged_relations ==
          result.stats.standard_v0.output_relations + result.stats.v3_relations_added);
    check_common_stats(result);

    std::unordered_set<RelationSourceCombination, RelationSourceCombinationHash>
        primary_a_combinations;
    for (const auto& relation : result.materialize_relations()) {
        if (relation.a == 10 && relation.is_merged()) {
            primary_a_combinations.insert(gnfs::relation::relation_source_combination(relation));
        }
    }
    CHECK(primary_a_combinations.size() >= 2);
}

void test_clique_v0() {
    auto result =
        RelationReductionEngine::reduce(RawRelationSnapshot(303, make_shared_primary_corpus()),
                                        lp_config(ReductionStrategy::CliqueV0));

    CHECK(result.generation == 303);
    CHECK(result.stats.strategy == ReductionStrategy::CliqueV0);
    CHECK(result.stats.clique_v0.input_relations == 6);
    CHECK(result.stats.standard_v0.output_relations == 0);
    CHECK(result.stats.v3.input_relations == 0);
    CHECK(result.stats.merged_relations == result.size());
    check_common_stats(result);
}

void test_structured_strategy_runs_exactly_once() {
    std::vector<Relation> input{
        make_partial(1, {101}),
        make_partial(2, {101}),
        make_full(3),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(701, std::move(input)),
                                                  structured_config(2, 4));

    CHECK(result.generation == 701);
    CHECK(result.stats.strategy == ReductionStrategy::Structured);
    CHECK(result.stats.input_relations == 3);
    CHECK(result.stats.raw_duplicates_removed == 0);
    CHECK(result.stats.filter.input_relations == 0);
    CHECK(result.stats.filter.output_relations == 0);
    CHECK(result.stats.standard_v0.input_1lp == 0);
    CHECK(result.stats.standard_v0.input_2lp == 0);
    CHECK(result.stats.clique_v0.input_relations == 0);
    CHECK(result.stats.v3.input_relations == 0);
    CHECK(result.stats.structured.budgeted_runs == 1);
    CHECK(result.stats.structured.input_rows == 3);
    CHECK(result.stats.structured_run.commits == 1);
    CHECK(result.stats.structured_run.emitted_rows == 1);
    CHECK(result.stats.structured_run.stop_reason == StructuredReductionStopReason::NoCandidates);
    CHECK(result.stats.singleton_rows_removed == 0);
    CHECK(result.stats.merged_relations == 1);
    CHECK(result.stats.deduplicated_input_lp_histogram.unique_keys == 1);
    CHECK(result.stats.pre_merge_lp_histogram.unique_keys == 0);
    CHECK(result.stats.output_relations == 2);
    CHECK(result.stats.output_lp_columns == 0);
    CHECK(result.stats.output_digest == corpus_digest(result.relation_corpus()));
    CHECK(result.size() == 2);
    CHECK(result.read(0).a == 3);
    CHECK(result.read(1).is_merged());
}

void test_structured_no_candidates_is_success() {
    const std::vector<Relation> input{make_full(11), make_full(13)};
    auto result =
        RelationReductionEngine::reduce(RawRelationSnapshot(702, input), structured_config(1, 1));

    CHECK(result.stats.structured.budgeted_runs == 1);
    CHECK(result.stats.structured_run.commits == 0);
    CHECK(result.stats.structured_run.stop_reason == StructuredReductionStopReason::NoCandidates);
    CHECK(result.stats.merged_relations == 0);
    CHECK(result.stats.output_relations == input.size());
    CHECK(equal_corpus(result.materialize_relations(), input));
}

void test_structured_owns_singleton_policy() {
    std::vector<Relation> input{
        make_partial(21, {101}),
        make_full(23),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(703, std::move(input)),
                                                  structured_config(1, 2));

    CHECK(result.stats.filter.input_relations == 0);
    CHECK(result.stats.filter.singletons_removed == 0);
    CHECK(result.stats.structured_run.singleton_rows_removed == 1);
    CHECK(result.stats.singleton_rows_removed == 1);
    CHECK(result.stats.output_relations == 1);
    CHECK(result.read(0).a == 23);
}

void test_structured_deduplicates_before_source_ids() {
    Relation first = make_partial(31, {101});
    Relation duplicate = first;
    duplicate.rational_factors = {999};
    std::vector<Relation> input{first, duplicate, make_partial(37, {101})};

    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(704, std::move(input)),
                                                  structured_config(2, 2));
    CHECK(result.stats.input_relations == 3);
    CHECK(result.stats.raw_duplicates_removed == 1);
    CHECK(result.stats.structured.input_rows == 2);
    CHECK(result.stats.output_relations == 1);
    CHECK(result.read(0).rational_factors.front() == first.rational_factors.front());
}

void test_structured_final_merge_count_excludes_consumed_intermediates() {
    std::vector<Relation> input{
        make_partial(51, {101}),
        make_partial(53, {101, 103}),
        make_partial(59, {103}),
    };
    auto result = RelationReductionEngine::reduce(RawRelationSnapshot(707, std::move(input)),
                                                  structured_config(2, 2));

    CHECK(result.stats.structured_run.emitted_rows == 2);
    CHECK(result.stats.merged_relations == 1);
    CHECK(result.stats.output_relations == 1);
    CHECK(result.read(0).is_merged());
}

void test_structured_thread_equivalence() {
    std::optional<RelationReductionResult> baseline;
    for (uint32_t workers : {1U, 2U, 4U}) {
        auto result = RelationReductionEngine::reduce(
            RawRelationSnapshot(705, make_shared_primary_corpus()), structured_config(workers, 3));
        CHECK(result.stats.structured.budgeted_runs == 1);
        CHECK(result.stats.structured_incidence.requested_worker_count == workers);
        CHECK(result.stats.output_digest == corpus_digest(result.relation_corpus()));
        if (!baseline.has_value()) {
            baseline.emplace(std::move(result));
            continue;
        }
        CHECK(equal_corpus(result.materialize_relations(), baseline->materialize_relations()));
        CHECK(result.stats.output_digest == baseline->stats.output_digest);
        CHECK(result.stats.output_relations == baseline->stats.output_relations);
        CHECK(result.stats.output_lp_columns == baseline->stats.output_lp_columns);
        CHECK(result.stats.singleton_rows_removed == baseline->stats.singleton_rows_removed);
        CHECK(result.stats.merged_relations == baseline->stats.merged_relations);
        CHECK(result.stats.structured_run == baseline->stats.structured_run);
        CHECK(result.stats.structured.two_way_merges == baseline->stats.structured.two_way_merges);
        CHECK(result.stats.structured.tree_basis_batches ==
              baseline->stats.structured.tree_basis_batches);
        CHECK(result.stats.structured.output_rows == baseline->stats.structured.output_rows);
        CHECK(result.stats.structured.stop_reason == baseline->stats.structured.stop_reason);
    }
}

void test_structured_experimental_profile_thread_equivalence() {
    std::optional<RelationReductionResult> baseline;
    for (uint32_t workers : {1U, 2U, 4U}) {
        auto corpus = make_shared_primary_corpus();
        const size_t input_rows = corpus.size();
        auto result =
            RelationReductionEngine::reduce(RawRelationSnapshot(708, std::move(corpus)),
                                            experimental_profile_config(input_rows, workers));
        CHECK(result.stats.structured.budgeted_runs == 1);
        CHECK(result.stats.structured_incidence.requested_worker_count == workers);
        if (!baseline) {
            baseline.emplace(std::move(result));
            continue;
        }
        CHECK(equal_corpus(result.materialize_relations(), baseline->materialize_relations()));
        CHECK(result.stats.output_digest == baseline->stats.output_digest);
        CHECK(result.stats.structured_run == baseline->stats.structured_run);
        CHECK(result.stats.merged_relations == baseline->stats.merged_relations);
    }
}

void test_structured_ooc_source_and_sink_match_memory() {
    constexpr uint64_t generation = 709;
    auto input = make_shared_primary_corpus();
    input.push_back(input[1]);
    auto memory_result = RelationReductionEngine::reduce(RawRelationSnapshot(generation, input),
                                                         structured_config(2, 3));
    const auto expected = memory_result.materialize_relations();
    CHECK(memory_result.stats.raw_duplicates_removed == 1);

    {
        OOCArtifacts output_artifacts(unique_ooc_base("memory_source_ooc_sink"));
        auto config = structured_config(2, 3);
        config.structured->output_ooc_base_path = output_artifacts.base;
        {
            auto result =
                RelationReductionEngine::reduce(RawRelationSnapshot(generation, input), config);
            CHECK(result.storage_kind() == gnfs::relation::RelationStorageKind::FinalizedOOC);
            CHECK(result.stats == memory_result.stats);
            CHECK(equal_corpus(result.materialize_relations(), expected));
            CHECK(private_sink_exists(output_artifacts.base));
        }
        CHECK(private_sink_absent(output_artifacts.base));
    }

    {
        OOCArtifacts input_artifacts(unique_ooc_base("ooc_source_memory_sink"));
        OOCArtifacts working_artifacts(unique_ooc_base("ooc_source_memory_work"));
        RawRelationSnapshot snapshot(
            make_owned_ooc_corpus(generation, input_artifacts.base, input));
        auto missing_work = structured_config(2, 3);
        CHECK(throws_invalid_argument(
            [&] { (void)RelationReductionEngine::reduce(std::move(snapshot), missing_work); }));
        CHECK(snapshot.corpus.valid());

        auto aliased_paths = structured_config(2, 3);
        aliased_paths.structured->deduplicated_ooc_base_path = working_artifacts.base;
        aliased_paths.structured->output_ooc_base_path = working_artifacts.base;
        CHECK(throws_invalid_argument(
            [&] { (void)RelationReductionEngine::reduce(std::move(snapshot), aliased_paths); }));
        CHECK(snapshot.corpus.valid());

        auto config = structured_config(2, 3);
        config.structured->deduplicated_ooc_base_path = working_artifacts.base;

        auto result = RelationReductionEngine::reduce(std::move(snapshot), config);
        CHECK(result.storage_kind() == gnfs::relation::RelationStorageKind::InMemory);
        CHECK(result.stats == memory_result.stats);
        CHECK(equal_corpus(result.materialize_relations(), expected));
        CHECK(!snapshot.corpus.valid());
        CHECK(!std::filesystem::exists(input_artifacts.base + ".relidx"));
        CHECK(!std::filesystem::exists(input_artifacts.base + ".reldata"));
        CHECK(private_sink_absent(working_artifacts.base));
    }

    {
        OOCArtifacts input_artifacts(unique_ooc_base("ooc_source_ooc_sink"));
        OOCArtifacts working_artifacts(unique_ooc_base("ooc_source_ooc_work"));
        OOCArtifacts output_artifacts(unique_ooc_base("ooc_source_ooc_output"));
        RawRelationSnapshot snapshot(
            make_owned_ooc_corpus(generation, input_artifacts.base, input));
        auto config = structured_config(2, 3);
        config.structured->deduplicated_ooc_base_path = working_artifacts.base;
        config.structured->output_ooc_base_path = output_artifacts.base;

        {
            auto result = RelationReductionEngine::reduce(std::move(snapshot), config);
            CHECK(result.storage_kind() == gnfs::relation::RelationStorageKind::FinalizedOOC);
            CHECK(result.generation == generation);
            CHECK(result.stats == memory_result.stats);
            CHECK(equal_corpus(result.materialize_relations(), expected));
            CHECK(private_sink_exists(output_artifacts.base));
            CHECK(!snapshot.corpus.valid());
            CHECK(!std::filesystem::exists(input_artifacts.base + ".relidx"));
            CHECK(!std::filesystem::exists(input_artifacts.base + ".reldata"));
            CHECK(private_sink_absent(working_artifacts.base));
        }
        CHECK(private_sink_absent(output_artifacts.base));
    }
}

void test_structured_ooc_failure_preserves_authoritative_input() {
    constexpr uint64_t generation = 711;
    auto input = make_shared_primary_corpus();
    input[2].extra_ab_pairs = {{999, 1}};

    OOCArtifacts input_artifacts(unique_ooc_base("failure_source"));
    OOCArtifacts working_artifacts(unique_ooc_base("failure_work"));
    OOCArtifacts output_artifacts(unique_ooc_base("failure_output"));
    {
        RawRelationSnapshot snapshot(
            make_owned_ooc_corpus(generation, input_artifacts.base, input));
        auto config = structured_config(2, 3);
        config.structured->deduplicated_ooc_base_path = working_artifacts.base;
        config.structured->output_ooc_base_path = output_artifacts.base;

        CHECK(throws_invalid_argument(
            [&] { (void)RelationReductionEngine::reduce(std::move(snapshot), config); }));
        CHECK(snapshot.corpus.valid());
        CHECK(snapshot.size() == input.size());
        CHECK(equal_relation(snapshot.read(2), input[2]));
        CHECK(std::filesystem::exists(input_artifacts.base + ".relidx"));
        CHECK(std::filesystem::exists(input_artifacts.base + ".reldata"));
        CHECK(private_sink_absent(working_artifacts.base));
        CHECK(private_sink_absent(output_artifacts.base));
    }

    CHECK(!std::filesystem::exists(input_artifacts.base + ".relidx"));
    CHECK(!std::filesystem::exists(input_artifacts.base + ".reldata"));
}

void test_structured_ooc_post_prepare_failure_preserves_authoritative_input() {
    constexpr uint64_t generation = 712;
    auto input = make_shared_primary_corpus();
    input[2].b = 0;

    OOCArtifacts input_artifacts(unique_ooc_base("post_prepare_failure_source"));
    OOCArtifacts working_artifacts(unique_ooc_base("post_prepare_failure_work"));
    OOCArtifacts output_artifacts(unique_ooc_base("post_prepare_failure_output"));
    {
        RawRelationSnapshot snapshot(
            make_owned_ooc_corpus(generation, input_artifacts.base, input));
        auto config = structured_config(2, 3);
        config.structured->deduplicated_ooc_base_path = working_artifacts.base;
        config.structured->output_ooc_base_path = output_artifacts.base;

        CHECK(throws_structured_error(StructuredReductionErrorCode::InvalidInput, [&] {
            (void)RelationReductionEngine::reduce(std::move(snapshot), config);
        }));
        CHECK(snapshot.corpus.valid());
        CHECK(snapshot.size() == input.size());
        CHECK(equal_relation(snapshot.read(2), input[2]));
        CHECK(std::filesystem::exists(input_artifacts.base + ".relidx"));
        CHECK(std::filesystem::exists(input_artifacts.base + ".reldata"));
        CHECK(private_sink_absent(working_artifacts.base));
        CHECK(private_sink_absent(output_artifacts.base));
    }

    CHECK(!std::filesystem::exists(input_artifacts.base + ".relidx"));
    CHECK(!std::filesystem::exists(input_artifacts.base + ".reldata"));
}

void test_structured_ooc_rejects_overlapping_artifact_scopes() {
    constexpr uint64_t generation = 713;
    const auto input = make_shared_primary_corpus();
    OOCArtifacts raw_artifacts(unique_ooc_base("scoped_raw"));
    OOCArtifacts working_artifacts(unique_ooc_base("scoped_work"));

    {
        auto raw_sink = gnfs::relation::RelationSink::out_of_core(
            generation, raw_artifacts.base, gnfs::relation::OOCCleanupPolicy::RemoveArtifacts);
        for (const auto& relation : input) {
            (void)raw_sink.append(relation);
        }
        auto raw_corpus = raw_sink.finalize();
        const auto raw_scope = raw_corpus.ooc_artifact_scope();
        CHECK(raw_scope.has_value());
        CHECK(!raw_scope->cleanup_directory.empty());
        CHECK(std::filesystem::equivalent(raw_scope->cleanup_directory,
                                          raw_artifacts.base + ".gnfs-sink-lease"));

        RawRelationSnapshot snapshot(std::move(raw_corpus));
        auto config = structured_config(2, 3);
        config.structured->deduplicated_ooc_base_path = working_artifacts.base;
        config.structured->output_ooc_base_path =
            (std::filesystem::path(raw_scope->cleanup_directory) / "nested-output").string();

        CHECK(throws_invalid_argument(
            [&] { (void)RelationReductionEngine::reduce(std::move(snapshot), config); }));
        CHECK(snapshot.corpus.valid());
        CHECK(snapshot.size() == input.size());
        CHECK(private_sink_absent(working_artifacts.base));
        CHECK(
            !std::filesystem::exists(config.structured->output_ooc_base_path + ".gnfs-sink-lease"));

        config.structured->output_ooc_base_path.clear();
        config.structured->deduplicated_ooc_base_path =
            (std::filesystem::path(raw_scope->cleanup_directory) / "nested-work").string();
        CHECK(throws_invalid_argument(
            [&] { (void)RelationReductionEngine::reduce(std::move(snapshot), config); }));
        CHECK(snapshot.corpus.valid());
    }

    CHECK(private_sink_absent(raw_artifacts.base));
    CHECK(private_sink_absent(working_artifacts.base));
}

void test_structured_borrowed_source_matches_owning_routes() {
    constexpr uint64_t generation = 714;
    auto input = make_shared_primary_corpus();
    input.push_back(input[1]);
    const auto original_input = input;

    auto memory_result = RelationReductionEngine::reduce(RawRelationSnapshot(generation, input),
                                                         structured_config(4, 3));
    const auto expected_rows = memory_result.materialize_relations();
    CHECK(memory_result.stats.raw_duplicates_removed == 1);

    OOCArtifacts owning_input(unique_ooc_base("borrowed_equivalence_owning_input"));
    OOCArtifacts owning_work(unique_ooc_base("borrowed_equivalence_owning_work"));
    OOCArtifacts owning_output(unique_ooc_base("borrowed_equivalence_owning_output"));
    auto owning_config = structured_config(4, 3);
    owning_config.structured->deduplicated_ooc_base_path = owning_work.base;
    owning_config.structured->output_ooc_base_path = owning_output.base;
    RawRelationSnapshot owning_snapshot(
        make_owned_ooc_corpus(generation, owning_input.base, input));

    {
        auto owning_result =
            RelationReductionEngine::reduce(std::move(owning_snapshot), owning_config);
        CHECK(owning_result.storage_kind() == gnfs::relation::RelationStorageKind::FinalizedOOC);
        CHECK(owning_result.stats == memory_result.stats);
        CHECK(owning_result.stats.raw_input_digest == memory_result.stats.raw_input_digest);
        CHECK(owning_result.stats.output_digest == memory_result.stats.output_digest);
        CHECK(equal_corpus(owning_result.materialize_relations(), expected_rows));
        CHECK(private_sink_absent(owning_work.base));

        OOCArtifacts borrowed_work(unique_ooc_base("borrowed_equivalence_work"));
        OOCArtifacts borrowed_output(unique_ooc_base("borrowed_equivalence_output"));
        auto borrowed_config = structured_config(4, 3);
        borrowed_config.structured->deduplicated_ooc_base_path = borrowed_work.base;
        borrowed_config.structured->output_ooc_base_path = borrowed_output.base;

        {
            auto prepared = [&] {
                BorrowedVectorRelationSource callback_scoped_source(input);
                auto token = RelationReductionEngine::prepare_borrowed_structured(
                    generation, callback_scoped_source, borrowed_config);
                CHECK(token.valid());
                CHECK(token.generation() == generation);
                CHECK(token.input_relations() == input.size());
                CHECK(callback_scoped_source.count_calls() == 1);
                CHECK(callback_scoped_source.read_calls() == input.size());
                CHECK(private_sink_exists(borrowed_work.base));
                return token;
            }();

            // The callback-scoped source above is already destroyed. This
            // phase can only use the owning working corpus sealed in the token.
            borrowed_config.structured->output_ooc_base_path = borrowed_work.base;
            borrowed_config.structured->parallel.worker_count = 1;
            auto borrowed_result =
                RelationReductionEngine::reduce_prepared_structured(std::move(prepared));
            CHECK(borrowed_result.storage_kind() ==
                  gnfs::relation::RelationStorageKind::FinalizedOOC);
            CHECK(borrowed_result.generation == generation);
            CHECK(borrowed_result.stats == memory_result.stats);
            CHECK(borrowed_result.stats == owning_result.stats);
            CHECK(borrowed_result.stats.raw_input_digest == memory_result.stats.raw_input_digest);
            CHECK(borrowed_result.stats.output_digest == memory_result.stats.output_digest);
            CHECK(equal_corpus(borrowed_result.materialize_relations(), expected_rows));
            CHECK(private_sink_absent(borrowed_work.base));
            CHECK(private_sink_exists(borrowed_output.base));
            CHECK(!prepared.valid());
            CHECK(throws_logic_error([&] {
                (void)RelationReductionEngine::reduce_prepared_structured(std::move(prepared));
            }));
            CHECK(equal_corpus(input, original_input));
        }
        CHECK(private_sink_absent(borrowed_output.base));
    }
    CHECK(private_sink_absent(owning_output.base));
}

void test_structured_direct_borrowed_source_matches_owning_and_two_stage() {
    constexpr uint64_t generation = 719;
    const auto input = make_weight_stratified_incidence_corpus();
    struct IncidenceExecutionShape final {
        size_t shard_rows;
        uint32_t workers;
    };
    const std::array<IncidenceExecutionShape, 4> execution_shapes{{
        {1, 4},                // Worker count clamps to the one-row shard.
        {3, 2},                // Two uniform three-row shards.
        {4, 2},                // A full shard followed by a two-row tail.
        {input.size() + 1, 1}, // One oversized, single-worker shard.
    }};

    for (const auto& [shard_rows, workers] : execution_shapes) {
        const auto make_config = [&] {
            auto config = structured_config(workers, 3);
            config.structured->incidence.max_rows_per_shard = shard_rows;
            return config;
        };

        auto owning_result =
            RelationReductionEngine::reduce(RawRelationSnapshot(generation, input), make_config());
        const auto expected_rows = owning_result.materialize_relations();
        const auto& expected_histogram = owning_result.stats.deduplicated_input_lp_histogram;
        CHECK(owning_result.storage_kind() == gnfs::relation::RelationStorageKind::InMemory);
        CHECK(owning_result.stats.raw_duplicates_removed == 0);
        CHECK(owning_result.stats.raw_input_digest == corpus_digest(input));
        CHECK(owning_result.stats.output_digest == corpus_digest(owning_result.relation_corpus()));
        CHECK(expected_histogram.unique_keys == 4);
        CHECK(expected_histogram.weight_1 == 1);
        CHECK(expected_histogram.weight_2 == 1);
        CHECK(expected_histogram.weight_3 == 1);
        CHECK(expected_histogram.weight_4plus == 1);

        const size_t expected_peak_shard_rows = std::min(shard_rows, input.size());
        const size_t expected_shard_count = (input.size() + shard_rows - 1) / shard_rows;
        CHECK(owning_result.stats.structured_incidence.shard_count == expected_shard_count);
        CHECK(owning_result.stats.structured_incidence.peak_shard_rows == expected_peak_shard_rows);
        CHECK(owning_result.stats.structured_incidence.total_incidence_entries == 10);
        CHECK(owning_result.stats.structured_incidence.requested_worker_count == workers);
        CHECK(owning_result.stats.structured_incidence.peak_worker_count ==
              std::min<size_t>(workers, expected_peak_shard_rows));

        OOCArtifacts two_stage_work(unique_ooc_base("direct_equivalence_two_stage_work"));
        OOCArtifacts two_stage_output(unique_ooc_base("direct_equivalence_two_stage_output"));
        auto two_stage_config = make_config();
        two_stage_config.structured->deduplicated_ooc_base_path = two_stage_work.base;
        two_stage_config.structured->output_ooc_base_path = two_stage_output.base;
        BorrowedVectorRelationSource two_stage_source(input);

        auto two_stage_result = RelationReductionEngine::reduce_borrowed_structured(
            generation, two_stage_source, two_stage_config);
        CHECK(two_stage_result.storage_kind() == gnfs::relation::RelationStorageKind::FinalizedOOC);
        CHECK(two_stage_result.stats == owning_result.stats);
        CHECK(two_stage_result.stats.raw_input_digest == owning_result.stats.raw_input_digest);
        CHECK(two_stage_result.stats.output_digest == owning_result.stats.output_digest);
        CHECK(two_stage_result.stats.deduplicated_input_lp_histogram == expected_histogram);
        CHECK(two_stage_result.stats.pre_merge_lp_histogram ==
              owning_result.stats.pre_merge_lp_histogram);
        CHECK(equal_corpus(two_stage_result.materialize_relations(), expected_rows));
        CHECK(private_sink_absent(two_stage_work.base));
        CHECK(private_sink_exists(two_stage_output.base));

        OOCArtifacts raw(unique_ooc_base("direct_equivalence_raw"));
        OOCArtifacts direct_output(unique_ooc_base("direct_equivalence_output"));
        std::optional<RelationReductionResult> direct_result;
        {
            CollectorConfig collector_config;
            collector_config.ooc_enabled = true;
            collector_config.ooc_base_path = raw.base;
            collector_config.check_duplicates = true;
            RelationCollector collector(collector_config);
            add_relations(collector, input);

            auto direct_config = make_config();
            direct_config.structured->output_ooc_base_path = direct_output.base;
            direct_result.emplace(
                collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
                    CHECK(source.ab_pairs_unique());
                    CHECK(source.count() == input.size());
                    CHECK(source.descriptor().count == input.size());
                    return RelationReductionEngine::reduce_direct_borrowed_structured(
                        generation, source, direct_config);
                }));

            CHECK(direct_result->storage_kind() ==
                  gnfs::relation::RelationStorageKind::FinalizedOOC);
            CHECK(private_sink_exists(direct_output.base));

            // The returned corpus must not retain the callback-scoped source.
            Relation tail = make_full(static_cast<int64_t>(900 + workers + shard_rows * 10));
            CHECK(collector.add(std::move(tail)));
            CHECK(collector.size() == input.size() + 1);
        }

        CHECK(direct_result.has_value());
        CHECK(direct_result->generation == generation);
        CHECK(direct_result->stats == owning_result.stats);
        CHECK(direct_result->stats == two_stage_result.stats);
        CHECK(direct_result->stats.raw_input_digest == owning_result.stats.raw_input_digest);
        CHECK(direct_result->stats.output_digest == owning_result.stats.output_digest);
        CHECK(direct_result->stats.deduplicated_input_lp_histogram == expected_histogram);
        CHECK(direct_result->stats.pre_merge_lp_histogram ==
              owning_result.stats.pre_merge_lp_histogram);
        CHECK(equal_corpus(direct_result->materialize_relations(), expected_rows));
        direct_result.reset();
        CHECK(private_sink_absent(direct_output.base));
    }
}

void test_structured_direct_borrowed_contract_and_unique_capability() {
    constexpr uint64_t generation = 720;
    const auto input = make_shared_primary_corpus();
    OOCArtifacts raw(unique_ooc_base("direct_contract_raw"));
    OOCArtifacts working(unique_ooc_base("direct_contract_work"));
    OOCArtifacts output(unique_ooc_base("direct_contract_output"));

    CollectorConfig collector_config;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = raw.base;
    collector_config.check_duplicates = true;
    RelationCollector collector(collector_config);
    add_relations(collector, input);

    auto invoke = [&](uint64_t requested_generation, const RelationReductionConfig& config) {
        return collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
            return RelationReductionEngine::reduce_direct_borrowed_structured(requested_generation,
                                                                              source, config);
        });
    };

    auto valid = structured_config(2, 3);
    valid.structured->output_ooc_base_path = output.base;
    CHECK(throws_invalid_argument([&] { (void)invoke(0, valid); }));

    CHECK(throws_invalid_argument([&] { (void)invoke(generation, RelationReductionConfig{}); }));

    auto working_rejected = valid;
    working_rejected.structured->deduplicated_ooc_base_path = working.base;
    CHECK(throws_invalid_argument([&] { (void)invoke(generation, working_rejected); }));

    auto missing_output = structured_config(2, 3);
    CHECK(throws_invalid_argument([&] { (void)invoke(generation, missing_output); }));

    auto preserved_output = valid;
    preserved_output.structured->output_ooc_cleanup = gnfs::relation::OOCCleanupPolicy::Preserve;
    CHECK(throws_invalid_argument([&] { (void)invoke(generation, preserved_output); }));

    CHECK(private_sink_absent(working.base));
    CHECK(private_sink_absent(output.base));
    Relation tail = make_full(951);
    CHECK(collector.add(std::move(tail)));

    OOCArtifacts nonunique_raw(unique_ooc_base("direct_contract_nonunique_raw"));
    CollectorConfig nonunique_config;
    nonunique_config.ooc_enabled = true;
    nonunique_config.ooc_base_path = nonunique_raw.base;
    nonunique_config.check_duplicates = false;
    RelationCollector nonunique_collector(nonunique_config);
    Relation relation = make_full(953);
    CHECK(nonunique_collector.add(std::move(relation)));
    bool callback_called = false;
    CHECK(throws_logic_error([&] {
        nonunique_collector.with_unique_ooc_prefix(
            [&](const CollectorUniqueOOCPrefixSource&) { callback_called = true; });
    }));
    CHECK(!callback_called);
}

void test_structured_direct_borrowed_output_failure_is_retryable() {
    constexpr uint64_t generation = 721;
    auto input = make_shared_primary_corpus();
    OOCArtifacts raw(unique_ooc_base("direct_output_retry_raw"));
    OOCArtifacts output(unique_ooc_base("direct_output_retry_output"));

    CollectorConfig collector_config;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = raw.base;
    collector_config.check_duplicates = true;
    RelationCollector collector(collector_config);
    add_relations(collector, input);

    auto config = structured_config(2, 3);
    config.structured->output_ooc_base_path = output.base;
    std::error_code error;
    CHECK(std::filesystem::create_directory(output.base + ".gnfs-sink-lease", error));
    CHECK(!error);

    CHECK(throws_runtime_error([&] {
        (void)collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
            return RelationReductionEngine::reduce_direct_borrowed_structured(generation, source,
                                                                              config);
        });
    }));
    CHECK(std::filesystem::is_directory(output.base + ".gnfs-sink-lease"));

    Relation tail = make_full(955);
    input.push_back(tail);
    CHECK(collector.add(std::move(tail)));
    error.clear();
    CHECK(std::filesystem::remove(output.base + ".gnfs-sink-lease", error));
    CHECK(!error);

    auto oracle = RelationReductionEngine::reduce(RawRelationSnapshot(generation, input),
                                                  structured_config(2, 3));
    {
        auto retry =
            collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
                return RelationReductionEngine::reduce_direct_borrowed_structured(generation,
                                                                                  source, config);
            });
        CHECK(retry.stats == oracle.stats);
        CHECK(equal_corpus(retry.materialize_relations(), oracle.materialize_relations()));
        CHECK(private_sink_exists(output.base));
    }
    CHECK(private_sink_absent(output.base));
}

void test_structured_direct_borrowed_source_failure_aborts_output() {
    constexpr uint64_t generation = 722;
    OOCArtifacts raw(unique_ooc_base("direct_source_failure_raw"));
    OOCArtifacts output(unique_ooc_base("direct_source_failure_output"));

    CollectorConfig collector_config;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = raw.base;
    collector_config.check_duplicates = true;
    RelationCollector collector(collector_config);
    Relation relation = make_partial(1, {101});
    CHECK(collector.add(std::move(relation)));
    const auto stats_before_failure = collector.stats();

    const auto descriptor = collector.checkpoint_ooc();
    {
        std::fstream data(raw.base + ".reldata", std::ios::in | std::ios::out | std::ios::binary);
        CHECK(static_cast<bool>(data));
        const uint32_t corrupt_count = std::numeric_limits<uint32_t>::max();
        data.seekp(
            static_cast<std::streamoff>(gnfs::relation::OOCRelationWriter::DATA_HEADER_BYTES + 16));
        data.write(reinterpret_cast<const char*>(&corrupt_count), sizeof(corrupt_count));
        data.flush();
        CHECK(static_cast<bool>(data));
    }
    collector.resume_ooc(descriptor);

    auto config = structured_config(2, 3);
    config.structured->output_ooc_base_path = output.base;
    CHECK(throws_runtime_error([&] {
        (void)collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
            return RelationReductionEngine::reduce_direct_borrowed_structured(generation, source,
                                                                              config);
        });
    }));
    CHECK(private_sink_absent(output.base));
    CHECK(equal_collector_stats(collector.stats(), stats_before_failure));
    CHECK(throws_logic_error([&] {
        Relation tail = make_full(957);
        (void)collector.add(std::move(tail));
    }));
}

void test_structured_direct_borrowed_rejects_pre_scan_ab_proof_drift() {
    constexpr uint64_t generation = 725;
    OOCArtifacts raw(unique_ooc_base("direct_ab_proof_drift_raw"));
    OOCArtifacts output(unique_ooc_base("direct_ab_proof_drift_output"));

    CollectorConfig collector_config;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = raw.base;
    collector_config.check_duplicates = true;
    RelationCollector collector(collector_config);
    Relation first = make_full(100);
    Relation second = make_full(200);
    CHECK(collector.add(std::move(first)));
    CHECK(collector.add(std::move(second)));
    const auto stats_before_failure = collector.stats();

    const auto descriptor = collector.checkpoint_ooc();
    constexpr int64_t replacement_a = 300;
    CHECK(overwrite_binary_value(
        raw.base + ".reldata",
        static_cast<std::streamoff>(gnfs::relation::OOCRelationWriter::DATA_HEADER_BYTES),
        replacement_a));
    collector.resume_ooc(descriptor);

    auto config = structured_config(2, 3);
    config.structured->output_ooc_base_path = output.base;
    CHECK(throws_runtime_error([&] {
        (void)collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
            return RelationReductionEngine::reduce_direct_borrowed_structured(generation, source,
                                                                              config);
        });
    }));
    CHECK(private_sink_absent(output.base));
    CHECK(equal_collector_stats(collector.stats(), stats_before_failure));
    CHECK(throws_logic_error([&] {
        Relation tail = make_full(400);
        (void)collector.add(std::move(tail));
    }));
}

void test_structured_direct_borrowed_rejects_pre_scan_factor_drift() {
    constexpr uint64_t generation = 726;
    OOCArtifacts raw(unique_ooc_base("direct_factor_receipt_drift_raw"));
    OOCArtifacts output(unique_ooc_base("direct_factor_receipt_drift_output"));

    CollectorConfig collector_config;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = raw.base;
    collector_config.check_duplicates = true;
    RelationCollector collector(collector_config);
    Relation first = make_full(100);
    Relation second = make_full(200);
    CHECK(collector.add(std::move(first)));
    CHECK(collector.add(std::move(second)));
    const auto stats_before_failure = collector.stats();

    const auto descriptor = collector.checkpoint_ooc();
    constexpr uint32_t replacement_factor = 777;
    constexpr std::streamoff first_rational_factor_offset =
        static_cast<std::streamoff>(gnfs::relation::OOCRelationWriter::DATA_HEADER_BYTES) +
        static_cast<std::streamoff>(sizeof(int64_t) + sizeof(uint64_t) + sizeof(uint32_t));
    CHECK(overwrite_binary_value(raw.base + ".reldata", first_rational_factor_offset,
                                 replacement_factor));
    collector.resume_ooc(descriptor);

    auto config = structured_config(2, 3);
    config.structured->output_ooc_base_path = output.base;
    CHECK(throws_runtime_error([&] {
        (void)collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
            return RelationReductionEngine::reduce_direct_borrowed_structured(generation, source,
                                                                              config);
        });
    }));
    CHECK(private_sink_absent(output.base));
    CHECK(equal_collector_stats(collector.stats(), stats_before_failure));
    CHECK(throws_logic_error([&] {
        Relation tail = make_full(300);
        (void)collector.add(std::move(tail));
    }));
}

void test_structured_direct_borrowed_detects_post_scan_payload_drift() {
    constexpr uint64_t generation = 723;
    constexpr size_t raw_row_count = 100'000;
    OOCArtifacts raw(unique_ooc_base("direct_payload_drift_raw"));
    OOCArtifacts output(unique_ooc_base("direct_payload_drift_output"));

    CollectorConfig collector_config;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = raw.base;
    collector_config.check_duplicates = true;
    RelationCollector collector(collector_config);
    for (size_t ordinal = 0; ordinal < raw_row_count; ++ordinal) {
        Relation relation = make_full(static_cast<int64_t>(ordinal + 1'000));
        CHECK(collector.add(std::move(relation)));
    }
    const auto stats_before_failure = collector.stats();

    auto config = experimental_profile_config(raw_row_count, 4);
    config.structured->output_ooc_base_path = output.base;
    const std::string output_data = private_sink_base(output.base) + ".reldata";
    std::atomic<bool> stop_modifier{false};
    std::atomic<bool> payload_modified{false};
    std::atomic<bool> modifier_failed{false};
    std::thread modifier([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!stop_modifier.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::error_code error;
            const uintmax_t size = std::filesystem::file_size(output_data, error);
            if (!error && size > gnfs::relation::OOCRelationWriter::DATA_HEADER_BYTES) {
                // Changing only `a` preserves record size and all structural
                // validity rules, but changes both the AB identity and V1 row
                // fingerprint after the engine's authoritative first scan.
                constexpr int64_t replacement_a = -7'230'001;
                const bool overwritten = overwrite_binary_value(
                    raw.base + ".reldata",
                    static_cast<std::streamoff>(
                        gnfs::relation::OOCRelationWriter::DATA_HEADER_BYTES),
                    replacement_a);
                modifier_failed.store(!overwritten, std::memory_order_release);
                payload_modified.store(overwritten, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
        if (!stop_modifier.load(std::memory_order_acquire)) {
            modifier_failed.store(true, std::memory_order_release);
        }
    });

    std::optional<RelationReductionResult> unexpected_result;
    std::exception_ptr reduction_failure;
    try {
        unexpected_result.emplace(
            collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
                return RelationReductionEngine::reduce_direct_borrowed_structured(generation,
                                                                                  source, config);
            }));
    } catch (...) {
        reduction_failure = std::current_exception();
    }
    stop_modifier.store(true, std::memory_order_release);
    modifier.join();

    bool runtime_failure = false;
    if (reduction_failure) {
        try {
            std::rethrow_exception(reduction_failure);
        } catch (const std::runtime_error&) {
            runtime_failure = true;
        } catch (...) {
        }
    }
    CHECK(payload_modified.load(std::memory_order_acquire));
    CHECK(!modifier_failed.load(std::memory_order_acquire));
    CHECK(runtime_failure);
    unexpected_result.reset();
    CHECK(private_sink_absent(output.base));
    CHECK(equal_collector_stats(collector.stats(), stats_before_failure));
    CHECK(throws_logic_error([&] {
        Relation tail = make_full(7'230'002);
        (void)collector.add(std::move(tail));
    }));
}

void test_structured_direct_borrowed_resume_failure_cleans_finalized_output() {
    constexpr uint64_t generation = 724;
    const auto input = make_shared_primary_corpus();
    OOCArtifacts raw(unique_ooc_base("direct_resume_failure_raw"));
    OOCArtifacts output(unique_ooc_base("direct_resume_failure_output"));

    CollectorConfig collector_config;
    collector_config.ooc_enabled = true;
    collector_config.ooc_base_path = raw.base;
    collector_config.check_duplicates = true;
    RelationCollector collector(collector_config);
    add_relations(collector, input);
    const auto stats_before_failure = collector.stats();

    auto config = structured_config(2, 3);
    config.structured->output_ooc_base_path = output.base;
    bool raw_corrupted_after_output = false;
    bool resume_failure = false;
    try {
        (void)collector.with_unique_ooc_prefix([&](const CollectorUniqueOOCPrefixSource& source) {
            auto result = RelationReductionEngine::reduce_direct_borrowed_structured(
                generation, source, config);
            CHECK(private_sink_exists(output.base));

            constexpr uint64_t corrupt_data_magic = 0;
            raw_corrupted_after_output =
                overwrite_binary_value(raw.base + ".reldata", 0, corrupt_data_magic);
            return result;
        });
    } catch (const std::runtime_error&) {
        resume_failure = true;
    } catch (...) {
    }

    CHECK(raw_corrupted_after_output);
    CHECK(resume_failure);
    CHECK(private_sink_absent(output.base));
    CHECK(equal_collector_stats(collector.stats(), stats_before_failure));
    CHECK(throws_logic_error([&] {
        Relation tail = make_full(7'240'001);
        (void)collector.add(std::move(tail));
    }));
}

void test_structured_borrowed_source_contract_rejects_invalid_routes() {
    const auto input = make_shared_primary_corpus();
    BorrowedVectorRelationSource source(input);
    OOCArtifacts working(unique_ooc_base("borrowed_contract_work"));
    OOCArtifacts output(unique_ooc_base("borrowed_contract_output"));

    auto valid = structured_config(2, 3);
    valid.structured->deduplicated_ooc_base_path = working.base;
    valid.structured->output_ooc_base_path = output.base;
    CHECK(throws_invalid_argument(
        [&] { (void)RelationReductionEngine::reduce_borrowed_structured(0, source, valid); }));

    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce_borrowed_structured(715, source,
                                                                  RelationReductionConfig{});
    }));

    auto missing_execution = valid;
    missing_execution.structured.reset();
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce_borrowed_structured(715, source, missing_execution);
    }));

    auto missing_working = structured_config(2, 3);
    missing_working.structured->output_ooc_base_path = output.base;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce_borrowed_structured(715, source, missing_working);
    }));

    auto missing_output = structured_config(2, 3);
    missing_output.structured->deduplicated_ooc_base_path = working.base;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce_borrowed_structured(715, source, missing_output);
    }));

    auto overlapping = structured_config(2, 3);
    overlapping.structured->deduplicated_ooc_base_path = working.base;
    overlapping.structured->output_ooc_base_path = working.base;
    CHECK(throws_invalid_argument([&] {
        (void)RelationReductionEngine::reduce_borrowed_structured(715, source, overlapping);
    }));

    CHECK(source.count_calls() == 0);
    CHECK(source.read_calls() == 0);
    CHECK(private_sink_absent(working.base));
    CHECK(private_sink_absent(output.base));
}

void test_structured_borrowed_prepared_token_drop_cleans_working_corpus() {
    constexpr uint64_t generation = 718;
    const auto input = make_shared_primary_corpus();
    BorrowedVectorRelationSource source(input);
    OOCArtifacts working(unique_ooc_base("borrowed_drop_work"));
    OOCArtifacts output(unique_ooc_base("borrowed_drop_output"));
    auto config = structured_config(2, 3);
    config.structured->deduplicated_ooc_base_path = working.base;
    config.structured->output_ooc_base_path = output.base;

    {
        auto prepared =
            RelationReductionEngine::prepare_borrowed_structured(generation, source, config);
        CHECK(prepared.valid());
        CHECK(private_sink_exists(working.base));
        CHECK(private_sink_absent(output.base));
    }

    CHECK(source.count_calls() == 1);
    CHECK(source.read_calls() == input.size());
    CHECK(private_sink_absent(working.base));
    CHECK(private_sink_absent(output.base));
}

void test_structured_borrowed_source_read_failure_rolls_back() {
    constexpr uint64_t generation = 716;
    const auto input = make_shared_primary_corpus();
    const auto original_input = input;
    BorrowedVectorRelationSource source(input, 2);
    OOCArtifacts working(unique_ooc_base("borrowed_read_failure_work"));
    OOCArtifacts output(unique_ooc_base("borrowed_read_failure_output"));
    auto config = structured_config(2, 3);
    config.structured->deduplicated_ooc_base_path = working.base;
    config.structured->output_ooc_base_path = output.base;

    CHECK(throws_runtime_error([&] {
        (void)RelationReductionEngine::reduce_borrowed_structured(generation, source, config);
    }));
    CHECK(source.count_calls() == 1);
    CHECK(source.read_calls() == 3);
    CHECK(equal_corpus(input, original_input));
    CHECK(private_sink_absent(working.base));
    CHECK(private_sink_absent(output.base));
}

void test_structured_borrowed_output_failure_rolls_back_working_corpus() {
    constexpr uint64_t generation = 717;
    const auto input = make_shared_primary_corpus();
    const auto original_input = input;
    BorrowedVectorRelationSource source(input);
    OOCArtifacts working(unique_ooc_base("borrowed_output_failure_work"));
    OOCArtifacts output(unique_ooc_base("borrowed_output_failure_output"));
    auto config = structured_config(2, 3);
    config.structured->deduplicated_ooc_base_path = working.base;
    config.structured->output_ooc_base_path = output.base;

    std::error_code error;
    CHECK(std::filesystem::create_directory(output.base + ".gnfs-sink-lease", error));
    CHECK(!error);
    {
        auto prepared =
            RelationReductionEngine::prepare_borrowed_structured(generation, source, config);
        CHECK(private_sink_exists(working.base));
        CHECK(throws_runtime_error([&] {
            (void)RelationReductionEngine::reduce_prepared_structured(std::move(prepared));
        }));
        // Output reservation happens before the token is consumed, so this
        // failure remains retryable. Dropping the token rolls working storage
        // back without touching the pre-existing output lease.
        CHECK(prepared.valid());
    }
    CHECK(source.count_calls() == 1);
    CHECK(source.read_calls() == input.size());
    CHECK(equal_corpus(input, original_input));
    CHECK(private_sink_absent(working.base));
    CHECK(std::filesystem::is_directory(output.base + ".gnfs-sink-lease"));
    CHECK(!std::filesystem::exists(private_sink_base(output.base) + ".relidx"));
    CHECK(!std::filesystem::exists(private_sink_base(output.base) + ".reldata"));
}

void test_structured_invariant_error_never_falls_back() {
    Relation invalid(41, 0);
    invalid.rational_large_prime.emplace_back(101, uint8_t{1});
    std::vector<Relation> input{invalid, make_partial(43, {101})};

    CHECK(throws_structured_error(StructuredReductionErrorCode::InvalidInput, [&] {
        (void)RelationReductionEngine::reduce(RawRelationSnapshot(706, std::move(input)),
                                              structured_config(2, 2));
    }));
}

void test_structured_sink_preflight_and_observer_failure() {
    constexpr uint64_t generation = 710;
    gnfs::relation::SequentialStructuredReducer reducer(generation, make_shared_primary_corpus());

    {
        auto sink = gnfs::relation::RelationSink::in_memory(generation + 1);
        CHECK(throws_structured_error(StructuredReductionErrorCode::InvalidGeneration,
                                      [&] { (void)reducer.materialize_active_to(sink); }));
        CHECK(sink.state() == gnfs::relation::RelationSinkState::Open);
        CHECK(sink.count() == 0);
    }

    {
        auto sink = gnfs::relation::RelationSink::in_memory(generation);
        (void)sink.append(make_full(91));
        CHECK(throws_structured_error(StructuredReductionErrorCode::InvariantViolation,
                                      [&] { (void)reducer.materialize_active_to(sink); }));
        CHECK(sink.state() == gnfs::relation::RelationSinkState::Open);
        CHECK(sink.count() == 1);
    }

    {
        auto sink = gnfs::relation::RelationSink::in_memory(generation);
        bool observer_failed = false;
        try {
            (void)reducer.materialize_active_to(sink, [](const Relation&) {
                throw std::runtime_error("injected structured output observer failure");
            });
        } catch (const std::runtime_error&) {
            observer_failed = true;
        }
        CHECK(observer_failed);
        CHECK(sink.state() == gnfs::relation::RelationSinkState::Aborted);
        CHECK(sink.count() == 0);
    }
}

void test_solver_handoff_exactly_once() {
    {
        std::vector<Relation> relations{make_full(71), make_full(73)};
        RelationReductionStats stats;
        stats.output_relations = relations.size();
        RelationReductionResult reduction(601, std::move(relations), std::move(stats));

        size_t diagnostic_calls = 0;
        size_t solver_calls = 0;
        const uint64_t generation = handoff_after_collection(
            std::move(reduction), 2,
            [&](const SolverHandoffInfo& info) {
                ++diagnostic_calls;
                CHECK(info.relation_rows == 2);
                CHECK(info.estimated_effective_columns == 2);
                CHECK(info.estimated_underbuilt);
            },
            [&](RelationReductionResult handed_off) {
                ++solver_calls;
                CHECK(handed_off.generation == 601);
                CHECK(handed_off.size() == 2);
                return handed_off.generation;
            });

        CHECK(generation == 601);
        CHECK(diagnostic_calls == 1);
        CHECK(solver_calls == 1);
    }

    {
        std::vector<Relation> relations{make_full(79), make_full(83)};
        RelationReductionStats stats;
        stats.output_relations = relations.size();
        RelationReductionResult reduction(602, std::move(relations), std::move(stats));

        size_t diagnostic_calls = 0;
        size_t solver_calls = 0;
        const uint64_t generation = handoff_after_collection(
            std::move(reduction), 1, [&](const SolverHandoffInfo&) { ++diagnostic_calls; },
            [&](RelationReductionResult handed_off) {
                ++solver_calls;
                CHECK(handed_off.generation == 602);
                CHECK(handed_off.size() == 2);
                return handed_off.generation;
            });

        CHECK(generation == 602);
        CHECK(diagnostic_calls == 0);
        CHECK(solver_calls == 1);
    }
}

} // namespace

int main() {
    // Keep unrelated optional merger policies out of this explicit-strategy test.
    unsetenv("GNFS_3LP");
    unsetenv("GNFS_V0_WEIGHT3");
    unsetenv("GNFS_WEIGHT_CUTOFF");
    unsetenv("GNFS_DROP_RESIDUAL");

    test_generation_and_no_large_primes();
    test_structured_policy_maps_to_named_legacy_strategy();
    test_illegal_combinations_fail_closed();
    test_exact_abpair_dedup_preserves_old_collision();
    test_exact_abpair_dedup_keeps_first_occurrence();
    test_merged_input_fails_before_dedup();
    test_digest_covers_every_field_and_order();
    test_filter_only_preserves_filtered_partials();
    test_fixed_digest_golden();
    test_singleton_purge_precedes_merge();
    test_standard_v0_and_explicit_off_unset_equivalence();
    test_standard_v0_with_v3_exact_dedup();
    test_clique_v0();
    test_structured_strategy_runs_exactly_once();
    test_structured_no_candidates_is_success();
    test_structured_owns_singleton_policy();
    test_structured_deduplicates_before_source_ids();
    test_structured_final_merge_count_excludes_consumed_intermediates();
    test_structured_thread_equivalence();
    test_structured_experimental_profile_thread_equivalence();
    test_structured_ooc_source_and_sink_match_memory();
    test_structured_ooc_failure_preserves_authoritative_input();
    test_structured_ooc_post_prepare_failure_preserves_authoritative_input();
    test_structured_ooc_rejects_overlapping_artifact_scopes();
    test_structured_borrowed_source_matches_owning_routes();
    test_structured_direct_borrowed_source_matches_owning_and_two_stage();
    test_structured_direct_borrowed_contract_and_unique_capability();
    test_structured_direct_borrowed_output_failure_is_retryable();
    test_structured_direct_borrowed_source_failure_aborts_output();
    test_structured_direct_borrowed_rejects_pre_scan_ab_proof_drift();
    test_structured_direct_borrowed_rejects_pre_scan_factor_drift();
    test_structured_direct_borrowed_detects_post_scan_payload_drift();
    test_structured_direct_borrowed_resume_failure_cleans_finalized_output();
    test_structured_borrowed_source_contract_rejects_invalid_routes();
    test_structured_borrowed_prepared_token_drop_cleans_working_corpus();
    test_structured_borrowed_source_read_failure_rolls_back();
    test_structured_borrowed_output_failure_rolls_back_working_corpus();
    test_structured_invariant_error_never_falls_back();
    test_structured_sink_preflight_and_observer_failure();
    test_solver_handoff_exactly_once();

    if (failures != 0) {
        std::cerr << failures << " relation reduction engine checks failed\n";
        return 1;
    }
    std::cout << "relation reduction engine checks passed\n";
    return 0;
}
