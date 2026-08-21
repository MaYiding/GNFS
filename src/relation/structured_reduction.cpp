#include "gnfs/relation/structured_reduction.hpp"
#include "gnfs/relation/relation_sink.hpp"
#include "gnfs/relation/structured_batch.hpp"
#include "gnfs/relation/structured_incidence_builder.hpp"
#include "gnfs/util/ordered_parallel_map.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <set>
#include <type_traits>
#include <utility>

namespace gnfs::relation {
namespace {

using core::PrimePower;
using core::Relation;

#if defined(GNFS_STRUCTURED_REDUCTION_TEST_HOOKS)
using StructuredReductionTestEvent = structured_reduction_testing::Event;
using StructuredReductionTestHook = structured_reduction_testing::Hook;

thread_local const StructuredReductionTestHook* active_structured_reduction_test_hook = nullptr;

class ScopedStructuredReductionTestHook final {
public:
    explicit ScopedStructuredReductionTestHook(const StructuredReductionTestHook& hook) noexcept
        : previous_(std::exchange(active_structured_reduction_test_hook, &hook)) {}

    ~ScopedStructuredReductionTestHook() {
        active_structured_reduction_test_hook = previous_;
    }

    ScopedStructuredReductionTestHook(const ScopedStructuredReductionTestHook&) = delete;
    ScopedStructuredReductionTestHook& operator=(const ScopedStructuredReductionTestHook&) = delete;

private:
    const StructuredReductionTestHook* previous_ = nullptr;
};

void invoke_structured_reduction_test_hook(const StructuredReductionTestHook* hook,
                                           StructuredReductionTestEvent event,
                                           size_t slot = structured_reduction_testing::no_slot) {
    if (hook != nullptr && hook->callback != nullptr)
        hook->callback(event, slot, hook->context);
}
#endif

[[noreturn]] void fail(StructuredReductionErrorCode code, const char* message) {
    throw StructuredReductionError(code, message);
}

size_t checked_resource_add(size_t lhs, size_t rhs, const char* message) {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        fail(StructuredReductionErrorCode::ResourceLimit, message);
    }
    return lhs + rhs;
}

void validate_source_combination(const SourceCombination& combination, bool allow_empty) {
    if (combination.generation() == 0) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "source combination has generation zero");
    }
    if (!allow_empty && combination.empty()) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "logical source combination is empty");
    }

    const auto sources = combination.sources();
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].generation != combination.generation()) {
            fail(StructuredReductionErrorCode::InvalidSourceCombination,
                 "source ID belongs to a different generation");
        }
        if (i != 0 && !(sources[i - 1] < sources[i])) {
            fail(StructuredReductionErrorCode::InvalidSourceCombination,
                 "source combination is not strictly canonical");
        }
    }
}

void validate_lp_key(const LargePrimeKey& key) {
    if (key.prime < 2) {
        fail(StructuredReductionErrorCode::InvariantViolation, "large-prime key has p < 2");
    }
    if (!key.is_algebraic && key.root != 0) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "rational large-prime key has a nonzero root");
    }
    constexpr uint64_t projective_root = std::numeric_limits<uint32_t>::max();
    if (key.is_algebraic && key.root != projective_root && key.root >= key.prime) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "algebraic large-prime key has an invalid root");
    }
}

void validate_canonical_lp_keys(std::span<const LargePrimeKey> keys) {
    for (size_t i = 0; i < keys.size(); ++i) {
        validate_lp_key(keys[i]);
        if (i != 0 && !(keys[i - 1] < keys[i])) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "large-prime support is not strictly canonical");
        }
    }
}

std::vector<LargePrimeKey> symmetric_difference_lp_keys(std::span<const LargePrimeKey> lhs,
                                                        std::span<const LargePrimeKey> rhs) {
    validate_canonical_lp_keys(lhs);
    validate_canonical_lp_keys(rhs);

    std::vector<LargePrimeKey> result;
    if (lhs.size() > result.max_size() - rhs.size()) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "large-prime symmetric difference is too large");
    }
    result.reserve(lhs.size() + rhs.size());

    size_t left = 0;
    size_t right = 0;
    while (left < lhs.size() && right < rhs.size()) {
        if (lhs[left] < rhs[right]) {
            result.push_back(lhs[left++]);
        } else if (rhs[right] < lhs[left]) {
            result.push_back(rhs[right++]);
        } else {
            ++left;
            ++right;
        }
    }
    result.insert(result.end(), lhs.begin() + static_cast<std::ptrdiff_t>(left), lhs.end());
    result.insert(result.end(), rhs.begin() + static_cast<std::ptrdiff_t>(right), rhs.end());
    return result;
}

void validate_full_source_rank(size_t corpus_size,
                               std::span<const SourceCombination* const> transforms) {
    constexpr size_t no_basis_row = std::numeric_limits<size_t>::max();
    std::vector<size_t> pivot_to_basis(corpus_size, no_basis_row);
    std::vector<SourceCombination> basis;
    basis.reserve(transforms.size());

    for (const SourceCombination* transform : transforms) {
        if (transform == nullptr) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "source transform pointer is null");
        }
        validate_source_combination(*transform, false);
        SourceCombination candidate = *transform;
        bool inserted = false;
        while (!candidate.empty()) {
            const SourceId pivot = candidate.sources().back();
            if (pivot.ordinal >= pivot_to_basis.size()) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "source transform has an invalid pivot");
            }
            const size_t basis_index = pivot_to_basis[static_cast<size_t>(pivot.ordinal)];
            if (basis_index == no_basis_row) {
                pivot_to_basis[static_cast<size_t>(pivot.ordinal)] = basis.size();
                basis.push_back(std::move(candidate));
                inserted = true;
                break;
            }
            candidate = SourceCombination::symmetric_difference(candidate, basis[basis_index]);
        }
        if (!inserted) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "source transforms are linearly dependent");
        }
    }
    if (basis.size() != transforms.size()) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "source transform rank is inconsistent");
    }
}

bool contains_lp_key(std::span<const LargePrimeKey> keys, const LargePrimeKey& key) {
    return std::binary_search(keys.begin(), keys.end(), key);
}

bool relations_equal(const Relation& lhs, const Relation& rhs) {
    return lhs.a == rhs.a && lhs.b == rhs.b && lhs.rational_factors == rhs.rational_factors &&
           lhs.algebraic_factors == rhs.algebraic_factors &&
           lhs.rational_large_prime == rhs.rational_large_prime &&
           lhs.algebraic_large_prime == rhs.algebraic_large_prime &&
           lhs.extra_ab_pairs == rhs.extra_ab_pairs;
}

void validate_input_large_prime(const PrimePower& lp, bool algebraic) {
    if (lp.p < 2 || lp.e == 0) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "source relation contains an invalid large-prime power");
    }
    constexpr uint64_t projective_root = std::numeric_limits<uint32_t>::max();
    if (algebraic && lp.r != projective_root && lp.r >= lp.p) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "algebraic large-prime power has an invalid root");
    }
}

void validate_input_relation(const Relation& relation) {
    if (relation.b == 0) {
        fail(StructuredReductionErrorCode::InvalidInput, "source relation has b == 0");
    }
    for (const auto& [a, b] : relation.extra_ab_pairs) {
        (void)a;
        if (b == 0) {
            fail(StructuredReductionErrorCode::InvalidInput,
                 "source relation has an extra pair with b == 0");
        }
    }
    for (const auto& lp : relation.rational_large_prime) {
        validate_input_large_prime(lp, false);
    }
    for (const auto& lp : relation.algebraic_large_prime) {
        validate_input_large_prime(lp, true);
    }

    try {
        relation.validate_persistence_limits();
    } catch (const std::length_error&) {
        fail(StructuredReductionErrorCode::PersistenceLimit,
             "source relation exceeds persistence limits");
    }
}

RelationCorpus make_in_memory_source_corpus(uint64_t generation, std::vector<Relation> relations) {
    if (generation == 0) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "source corpus generation must be nonzero");
    }
    return RelationCorpus::from_in_memory(generation, std::move(relations));
}

size_t checked_persisted_count(size_t current, size_t increment, size_t limit,
                               const char* message) {
    if (increment > limit || current > limit - increment) {
        fail(StructuredReductionErrorCode::PersistenceLimit, message);
    }
    return current + increment;
}

struct LargePrimeTerm final {
    LargePrimeKey key;
    uint8_t exponent = 0;
};

struct LargePrimeAggregate final {
    LargePrimeKey key;
    uint64_t exponent = 0;
};

uint64_t persisted_chunk_count(uint64_t exponent) noexcept {
    constexpr uint64_t chunk_max = std::numeric_limits<uint8_t>::max();
    return exponent / chunk_max + (exponent % chunk_max != 0);
}

using PersistenceFailureKey = std::vector<std::vector<uint64_t>>;

PersistenceFailureKey
make_persistence_failure_key(std::span<const SourceCombination* const> combinations) {
    PersistenceFailureKey key;
    key.reserve(combinations.size());
    for (const SourceCombination* combination : combinations) {
        if (combination == nullptr) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "persistence cache received a null source combination");
        }
        validate_source_combination(*combination, false);
        std::vector<uint64_t> ordinals;
        ordinals.reserve(combination->size());
        for (const SourceId source : combination->sources())
            ordinals.push_back(source.ordinal);
        key.push_back(std::move(ordinals));
    }
    return key;
}

void validate_budget(const StructuredReductionBudget& budget) {
    constexpr size_t max_pairs = static_cast<size_t>(Relation::MAX_SERIALIZED_EXTRA_AB_PAIRS) + 1;
    constexpr size_t max_lp_keys = static_cast<size_t>(Relation::MAX_SERIALIZED_LARGE_PRIMES) * 2;
    constexpr size_t max_tree_output_lp_nnz = 7 * max_lp_keys;

    if (budget.max_pivot_weight > 8) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "structured reduction pivot budget exceeds the weight-eight reference");
    }
    if (budget.max_source_atoms_per_output > max_pairs) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "structured reduction source budget exceeds the relation format boundary");
    }
    if (budget.max_odd_lp_keys_per_output > max_lp_keys ||
        budget.max_output_lp_nnz_per_commit > max_tree_output_lp_nnz) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "structured reduction LP budget exceeds the relation format boundary");
    }
    if (budget.max_materialized_pairs_per_output > max_pairs ||
        budget.max_factor_entries_per_side > Relation::MAX_SERIALIZED_FACTORS ||
        budget.max_persisted_lp_entries_per_side > Relation::MAX_SERIALIZED_LARGE_PRIMES) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "structured reduction materialization budget exceeds the relation format boundary");
    }
}

size_t validate_prebuilt_incidence_stats(const std::vector<std::vector<LargePrimeKey>>& row_lp_keys,
                                         const StructuredIncidenceBuildStats& stats) {
    if (stats.requested_worker_count == 0) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "prebuilt incidence reports no requested worker");
    }

    const size_t row_count = row_lp_keys.size();
    if (row_count == 0) {
        if (stats.shard_count != 0 || stats.peak_shard_rows != 0 ||
            stats.peak_shard_incidence_entries != 0 || stats.total_incidence_entries != 0 ||
            stats.peak_worker_count != 0) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "empty prebuilt incidence has nonempty build statistics");
        }
        return 0;
    }

    if (stats.peak_shard_rows == 0 || stats.peak_shard_rows > row_count) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "prebuilt incidence peak shard row count is invalid");
    }
    const size_t expected_shard_count =
        row_count / stats.peak_shard_rows + (row_count % stats.peak_shard_rows != 0);
    if (stats.shard_count != expected_shard_count) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "prebuilt incidence shard count is inconsistent");
    }

    const size_t expected_peak_workers =
        std::min<size_t>(stats.requested_worker_count, stats.peak_shard_rows);
    if (stats.peak_worker_count != expected_peak_workers) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "prebuilt incidence worker statistics are inconsistent");
    }

    size_t total_entries = 0;
    size_t peak_shard_entries = 0;
    for (size_t shard_begin = 0; shard_begin < row_count;) {
        const size_t shard_rows = std::min(stats.peak_shard_rows, row_count - shard_begin);
        size_t shard_entries = 0;
        for (size_t local_row = 0; local_row < shard_rows; ++local_row) {
            const size_t row_index = shard_begin + local_row;
            shard_entries = checked_resource_add(shard_entries, row_lp_keys[row_index].size(),
                                                 "prebuilt incidence shard entry count overflows");
        }
        total_entries = checked_resource_add(total_entries, shard_entries,
                                             "prebuilt incidence total entry count overflows");
        peak_shard_entries = std::max(peak_shard_entries, shard_entries);
        shard_begin += shard_rows;
    }
    if (stats.total_incidence_entries != total_entries ||
        stats.peak_shard_incidence_entries != peak_shard_entries) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "prebuilt incidence entry statistics are inconsistent");
    }
    return total_entries;
}

void validate_prebuilt_incidence(const SourceCorpus& corpus,
                                 const StructuredIncidenceBuildResult& incidence) {
    if (!incidence.valid()) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "prebuilt incidence receipt is invalid or already consumed");
    }
    if (incidence.generation() != corpus.generation()) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "prebuilt incidence belongs to a different source generation");
    }
    if (incidence.row_count() != corpus.size()) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "prebuilt incidence row count differs from the source corpus");
    }

    const auto& row_lp_keys = incidence.row_lp_keys();
    if (row_lp_keys.size() != incidence.row_count()) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "prebuilt incidence row support count is inconsistent");
    }
    if (!std::in_range<uint64_t>(row_lp_keys.size())) {
        fail(StructuredReductionErrorCode::ResourceLimit,
             "prebuilt incidence row count exceeds the row ID representation");
    }
    constexpr size_t max_lp_keys = static_cast<size_t>(Relation::MAX_SERIALIZED_LARGE_PRIMES) * 2;
    for (const auto& keys : row_lp_keys) {
        if (keys.size() > max_lp_keys) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "prebuilt incidence row support exceeds relation persistence limits");
        }
        validate_canonical_lp_keys(keys);
    }

    const size_t expected_entries =
        validate_prebuilt_incidence_stats(row_lp_keys, incidence.stats());
    const auto& built_buckets = incidence.buckets();
    size_t bucket_entries = 0;
    for (size_t bucket_index = 0; bucket_index < built_buckets.size(); ++bucket_index) {
        const auto& bucket = built_buckets[bucket_index];
        validate_lp_key(bucket.key);
        if (bucket_index != 0 && !(built_buckets[bucket_index - 1].key < bucket.key)) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "prebuilt incidence buckets are not strictly canonical");
        }
        if (bucket.adjacency.empty()) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "prebuilt incidence contains an empty bucket");
        }

        StructuredRowId previous{};
        bool have_previous = false;
        for (const StructuredRowId row_id : bucket.adjacency) {
            if (row_id.value >= row_lp_keys.size()) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "prebuilt incidence bucket contains an out-of-range row");
            }
            if (have_previous && !(previous < row_id)) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "prebuilt incidence bucket adjacency is not strictly canonical");
            }
            previous = row_id;
            have_previous = true;

            const size_t row_index = static_cast<size_t>(row_id.value);
            if (!std::binary_search(row_lp_keys[row_index].begin(), row_lp_keys[row_index].end(),
                                    bucket.key)) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "prebuilt incidence bucket is not symmetric with its row support");
            }
        }
        bucket_entries = checked_resource_add(bucket_entries, bucket.adjacency.size(),
                                              "prebuilt incidence bucket entry count overflows");
    }
    if (bucket_entries != expected_entries) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "prebuilt incidence bucket entries differ from row supports");
    }
    // Canonical row supports and strictly unique bucket adjacency make both
    // sides sets of (key, row) pairs. Every bucket pair was proven present in
    // its row support above, and equal cardinality therefore proves the reverse
    // inclusion without an O(E log B) second lookup pass.
}

} // namespace

StructuredReductionError::StructuredReductionError(StructuredReductionErrorCode code,
                                                   std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

StructuredReductionErrorCode StructuredReductionError::code() const noexcept {
    return code_;
}

SourceCombination::SourceCombination(uint64_t generation, std::vector<SourceId> sources) noexcept
    : generation_(generation), sources_(std::move(sources)) {}

SourceCombination SourceCombination::canonical(uint64_t generation, std::vector<SourceId> sources) {
    if (generation == 0) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "source combination generation must be nonzero");
    }
    for (const auto source : sources) {
        if (source.generation != generation) {
            fail(StructuredReductionErrorCode::InvalidGeneration,
                 "source ID belongs to a different generation");
        }
    }

    std::sort(sources.begin(), sources.end());
    if (std::adjacent_find(sources.begin(), sources.end()) != sources.end()) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "source combination contains duplicate IDs");
    }
    return SourceCombination(generation, std::move(sources));
}

SourceCombination SourceCombination::singleton(SourceId source) {
    if (source.generation == 0) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "source ID generation must be nonzero");
    }
    return SourceCombination(source.generation, std::vector<SourceId>{source});
}

SourceCombination SourceCombination::symmetric_difference(const SourceCombination& lhs,
                                                          const SourceCombination& rhs) {
    validate_source_combination(lhs, true);
    validate_source_combination(rhs, true);
    if (lhs.generation() != rhs.generation()) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "cannot combine different source generations");
    }

    std::vector<SourceId> result;
    if (lhs.size() > result.max_size() - rhs.size()) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "source symmetric difference is too large");
    }
    result.reserve(lhs.size() + rhs.size());

    const auto left_sources = lhs.sources();
    const auto right_sources = rhs.sources();
    size_t left = 0;
    size_t right = 0;
    while (left < left_sources.size() && right < right_sources.size()) {
        if (left_sources[left] < right_sources[right]) {
            result.push_back(left_sources[left++]);
        } else if (right_sources[right] < left_sources[left]) {
            result.push_back(right_sources[right++]);
        } else {
            ++left;
            ++right;
        }
    }
    result.insert(result.end(), left_sources.begin() + static_cast<std::ptrdiff_t>(left),
                  left_sources.end());
    result.insert(result.end(), right_sources.begin() + static_cast<std::ptrdiff_t>(right),
                  right_sources.end());
    return SourceCombination(lhs.generation(), std::move(result));
}

uint64_t SourceCombination::generation() const noexcept {
    return generation_;
}

bool SourceCombination::empty() const noexcept {
    return sources_.empty();
}

size_t SourceCombination::size() const noexcept {
    return sources_.size();
}

std::span<const SourceId> SourceCombination::sources() const noexcept {
    return sources_;
}

SourceCorpus::SourceCorpus(RelationCorpus corpus)
    : generation_(corpus.logical_generation()), storage_(std::move(corpus)) {
    std::get<RelationCorpus>(storage_).for_each(
        [](const Relation& relation, size_t) { validate_input_relation(relation); });
}

SourceCorpus::SourceCorpus(uint64_t generation, std::vector<Relation> relations)
    : SourceCorpus(make_in_memory_source_corpus(generation, std::move(relations))) {}

SourceCorpus::SourceCorpus(uint64_t generation, BorrowedStorage borrowed, ValidatedBorrowedTag)
    : generation_(generation), storage_(borrowed) {
    if (generation == 0) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "borrowed source corpus generation must be nonzero");
    }
    if (borrowed.source == nullptr || borrowed.read == nullptr) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "borrowed source corpus has an invalid erased source");
    }
}

uint64_t SourceCorpus::generation() const noexcept {
    return generation_;
}

size_t SourceCorpus::size() const {
    if (const auto* corpus = std::get_if<RelationCorpus>(&storage_)) {
        return corpus->count();
    }
    return std::get<BorrowedStorage>(storage_).count;
}

SourceId SourceCorpus::source_id(size_t ordinal) const {
    if (ordinal >= size()) {
        fail(StructuredReductionErrorCode::InvalidInput, "source ordinal is out of range");
    }
    return SourceId{generation_, static_cast<uint64_t>(ordinal)};
}

const Relation* SourceCorpus::try_borrow(SourceId source) const {
    if (source.generation != generation_) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "source ID belongs to a different corpus generation");
    }
    if (source.ordinal >= size()) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "source ID ordinal is out of range");
    }
    if (const auto* corpus = std::get_if<RelationCorpus>(&storage_)) {
        return corpus->try_borrow_in_memory(static_cast<size_t>(source.ordinal));
    }
    return nullptr;
}

Relation SourceCorpus::at(SourceId source) const {
    if (const Relation* relation = try_borrow(source)) {
        return *relation;
    }
    const size_t ordinal = static_cast<size_t>(source.ordinal);
    if (const auto* corpus = std::get_if<RelationCorpus>(&storage_)) {
        return corpus->read(ordinal);
    }
    const auto& borrowed = std::get<BorrowedStorage>(storage_);
    return borrowed.read(borrowed.source, ordinal);
}

Relation SourceCorpus::materialize(const SourceCombination& combination) const {
    validate_source_combination(combination, false);
    if (combination.generation() != generation_) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "source combination belongs to a different corpus generation");
    }

    // OOC atoms are deserialized exactly once for this materialization. All
    // sizing, aggregation, and output passes below reuse these local values.
    std::vector<Relation> owned_atoms;
    std::vector<const Relation*> atoms;
    owned_atoms.reserve(combination.size());
    atoms.reserve(combination.size());
    for (const SourceId source : combination.sources()) {
        if (const Relation* borrowed = try_borrow(source)) {
            atoms.push_back(borrowed);
            continue;
        }
        owned_atoms.push_back(at(source));
        atoms.push_back(&owned_atoms.back());
    }

    size_t rational_factor_count = 0;
    size_t algebraic_factor_count = 0;
    size_t persisted_pair_count = 0;
    size_t raw_lp_count = 0;
    for (const Relation* relation_ptr : atoms) {
        const Relation& relation = *relation_ptr;
        rational_factor_count =
            checked_persisted_count(rational_factor_count, relation.rational_factors.size(),
                                    Relation::MAX_SERIALIZED_FACTORS,
                                    "materialized rational factors exceed persistence limit");
        algebraic_factor_count =
            checked_persisted_count(algebraic_factor_count, relation.algebraic_factors.size(),
                                    Relation::MAX_SERIALIZED_FACTORS,
                                    "materialized algebraic factors exceed persistence limit");
        persisted_pair_count = checked_persisted_count(
            persisted_pair_count, relation.extra_ab_pairs.size() + 1,
            static_cast<size_t>(Relation::MAX_SERIALIZED_EXTRA_AB_PAIRS) + 1,
            "materialized provenance exceeds persistence limit");
        if (relation.rational_large_prime.size() >
            std::numeric_limits<size_t>::max() - raw_lp_count) {
            fail(StructuredReductionErrorCode::PersistenceLimit,
                 "materialized large-prime input count is too large");
        }
        raw_lp_count += relation.rational_large_prime.size();
        if (relation.algebraic_large_prime.size() >
            std::numeric_limits<size_t>::max() - raw_lp_count) {
            fail(StructuredReductionErrorCode::PersistenceLimit,
                 "materialized large-prime input count is too large");
        }
        raw_lp_count += relation.algebraic_large_prime.size();
    }

    std::vector<LargePrimeTerm> terms;
    terms.reserve(raw_lp_count);
    for (const Relation* relation_ptr : atoms) {
        const Relation& relation = *relation_ptr;
        for (const auto& lp : relation.rational_large_prime) {
            terms.push_back(LargePrimeTerm{rational_large_prime_key(lp), lp.e});
        }
        for (const auto& lp : relation.algebraic_large_prime) {
            terms.push_back(LargePrimeTerm{algebraic_large_prime_key(lp), lp.e});
        }
    }
    std::sort(terms.begin(), terms.end(), [](const LargePrimeTerm& lhs, const LargePrimeTerm& rhs) {
        return lhs.key < rhs.key;
    });

    std::vector<LargePrimeAggregate> aggregates;
    aggregates.reserve(terms.size());
    for (const auto& term : terms) {
        if (aggregates.empty() || !(aggregates.back().key == term.key)) {
            aggregates.push_back(LargePrimeAggregate{term.key, term.exponent});
            continue;
        }
        if (aggregates.back().exponent > std::numeric_limits<uint64_t>::max() - term.exponent) {
            fail(StructuredReductionErrorCode::ExponentOverflow,
                 "large-prime exponent sum overflows uint64_t");
        }
        aggregates.back().exponent += term.exponent;
    }

    size_t rational_lp_count = 0;
    size_t algebraic_lp_count = 0;
    for (const auto& aggregate : aggregates) {
        const uint64_t chunks_wide = persisted_chunk_count(aggregate.exponent);
        const size_t side_limit = Relation::MAX_SERIALIZED_LARGE_PRIMES;
        if (chunks_wide > side_limit) {
            fail(StructuredReductionErrorCode::PersistenceLimit,
                 aggregate.key.is_algebraic
                     ? "materialized algebraic large primes exceed persistence limit"
                     : "materialized rational large primes exceed persistence limit");
        }
        const size_t chunks = static_cast<size_t>(chunks_wide);
        if (aggregate.key.is_algebraic) {
            algebraic_lp_count = checked_persisted_count(
                algebraic_lp_count, chunks, side_limit,
                "materialized algebraic large primes exceed persistence limit");
        } else {
            rational_lp_count = checked_persisted_count(
                rational_lp_count, chunks, side_limit,
                "materialized rational large primes exceed persistence limit");
        }
    }

    Relation materialized;
    materialized.rational_factors.reserve(rational_factor_count);
    materialized.algebraic_factors.reserve(algebraic_factor_count);
    materialized.rational_large_prime.reserve(rational_lp_count);
    materialized.algebraic_large_prime.reserve(algebraic_lp_count);
    materialized.extra_ab_pairs.reserve(persisted_pair_count - 1);

    bool have_primary = false;
    for (const Relation* relation_ptr : atoms) {
        const Relation& relation = *relation_ptr;
        if (!have_primary) {
            materialized.a = relation.a;
            materialized.b = relation.b;
            materialized.extra_ab_pairs.insert(materialized.extra_ab_pairs.end(),
                                               relation.extra_ab_pairs.begin(),
                                               relation.extra_ab_pairs.end());
            have_primary = true;
        } else {
            materialized.extra_ab_pairs.emplace_back(relation.a, relation.b);
            materialized.extra_ab_pairs.insert(materialized.extra_ab_pairs.end(),
                                               relation.extra_ab_pairs.begin(),
                                               relation.extra_ab_pairs.end());
        }
        materialized.rational_factors.insert(materialized.rational_factors.end(),
                                             relation.rational_factors.begin(),
                                             relation.rational_factors.end());
        materialized.algebraic_factors.insert(materialized.algebraic_factors.end(),
                                              relation.algebraic_factors.begin(),
                                              relation.algebraic_factors.end());
    }

    constexpr uint64_t chunk_max = std::numeric_limits<uint8_t>::max();
    for (const auto& aggregate : aggregates) {
        uint64_t remaining = aggregate.exponent;
        auto& output = aggregate.key.is_algebraic ? materialized.algebraic_large_prime
                                                  : materialized.rational_large_prime;
        while (remaining != 0) {
            const uint8_t chunk = static_cast<uint8_t>(std::min<uint64_t>(remaining, chunk_max));
            output.emplace_back(aggregate.key.prime, aggregate.key.root, chunk);
            remaining -= chunk;
        }
    }

    try {
        materialized.validate_persistence_limits();
    } catch (const std::length_error&) {
        fail(StructuredReductionErrorCode::PersistenceLimit,
             "materialized relation exceeds persistence limits");
    }
    return materialized;
}

PreparedTwoWayMerge::PreparedTwoWayMerge(TwoWayMergePlan plan, Relation materialized) noexcept
    : plan_(std::move(plan)), materialized_(std::move(materialized)) {}

const TwoWayMergePlan& PreparedTwoWayMerge::plan() const noexcept {
    return plan_;
}

const Relation& PreparedTwoWayMerge::materialized_relation() const noexcept {
    return materialized_;
}

PreparedTreeBasisMerge::PreparedTreeBasisMerge(TreeBasisMergePlan plan,
                                               std::vector<Relation> materialized) noexcept
    : plan_(std::move(plan)), materialized_(std::move(materialized)) {}

const TreeBasisMergePlan& PreparedTreeBasisMerge::plan() const noexcept {
    return plan_;
}

std::span<const Relation> PreparedTreeBasisMerge::materialized_relations() const noexcept {
    return materialized_;
}

StructuredReductionBudget::StructuredReductionBudget(
    size_t candidate_examinations_per_pass, size_t emitted_rows, size_t total_lp_fill_growth,
    size_t accepted_materialized_payload_entries_per_commit) noexcept
    : max_candidate_examinations_per_pass(candidate_examinations_per_pass),
      max_emitted_rows(emitted_rows), max_total_lp_fill_growth(total_lp_fill_growth),
      max_accepted_materialized_payload_entries_per_commit(
          accepted_materialized_payload_entries_per_commit),
      max_commits(emitted_rows) {}

void validate_structured_reduction_budget(const StructuredReductionBudget& budget) {
    validate_budget(budget);
}

struct SequentialStructuredReducer::Impl final {
    struct Row final {
        SourceCombination sources;
        std::vector<LargePrimeKey> lp_keys;
        std::vector<size_t> bucket_ids;
        bool active = true;
    };

    struct PreparedData final {
        TwoWayMergePlan plan;
        Relation materialized;
    };

    struct PreparedTreeData final {
        TreeBasisMergePlan plan;
        std::vector<Relation> materialized;
    };

    explicit Impl(SourceCorpus source_corpus, std::vector<std::vector<LargePrimeKey>> row_lp_keys,
                  std::vector<StructuredIncidenceBucket> built_buckets,
                  StructuredIncidenceBuildStats build_stats)
        : corpus(std::move(source_corpus)), buckets(std::move(built_buckets)),
          incidence_build_statistics(build_stats) {
        rows.reserve(corpus.size());
        for (size_t ordinal = 0; ordinal < corpus.size(); ++ordinal) {
            const SourceId source = corpus.source_id(ordinal);
            auto keys = std::move(row_lp_keys[ordinal]);
            rows.push_back(Row{SourceCombination::singleton(source), std::move(keys), {}, true});
        }
        std::vector<std::vector<LargePrimeKey>>{}.swap(row_lp_keys);

        bucket_active_degrees.reserve(buckets.size());
        for (size_t bucket_id = 0; bucket_id < buckets.size(); ++bucket_id) {
            const auto& bucket = buckets[bucket_id];
            bucket_active_degrees.push_back(bucket.adjacency.size());
            for (const StructuredRowId row : bucket.adjacency) {
                if (row.value >= rows.size()) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "prebuilt incidence bucket contains an out-of-range row");
                }
                rows[static_cast<size_t>(row.value)].bucket_ids.push_back(bucket_id);
            }
        }

        active_rows = rows.size();
        statistics.input_rows = rows.size();
        statistics.output_rows = active_rows;
        validate_state();
    }

    [[nodiscard]] const Row& row_at(StructuredRowId id) const {
        if (id.value >= rows.size()) {
            fail(StructuredReductionErrorCode::InvalidPlan, "structured row ID is out of range");
        }
        return rows[static_cast<size_t>(id.value)];
    }

    [[nodiscard]] Row& row_at(StructuredRowId id) {
        if (id.value >= rows.size()) {
            fail(StructuredReductionErrorCode::InvalidPlan, "structured row ID is out of range");
        }
        return rows[static_cast<size_t>(id.value)];
    }

    [[nodiscard]] size_t find_bucket(const LargePrimeKey& key) const {
        const auto it =
            std::lower_bound(buckets.begin(), buckets.end(), key,
                             [](const StructuredIncidenceBucket& bucket,
                                const LargePrimeKey& candidate) { return bucket.key < candidate; });
        if (it == buckets.end() || !(it->key == key)) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "logical row references an unknown LP bucket");
        }
        return static_cast<size_t>(it - buckets.begin());
    }

    void validate_state() const {
        if (bucket_active_degrees.size() != buckets.size()) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "LP bucket degree storage size is inconsistent");
        }
        size_t counted_active = 0;
        std::vector<const SourceCombination*> active_transforms;
        active_transforms.reserve(active_rows);
        for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
            const auto& row = rows[row_index];
            validate_source_combination(row.sources, false);
            if (row.sources.generation() != corpus.generation()) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "logical row belongs to a different generation");
            }
            for (const SourceId source : row.sources.sources()) {
                if (source.ordinal >= corpus.size()) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "logical row references an out-of-range source ordinal");
                }
            }
            validate_canonical_lp_keys(row.lp_keys);
            if (row.bucket_ids.size() != row.lp_keys.size()) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "logical row incidence size is inconsistent");
            }
            for (size_t i = 0; i < row.bucket_ids.size(); ++i) {
                if (row.bucket_ids[i] >= buckets.size() ||
                    !(buckets[row.bucket_ids[i]].key == row.lp_keys[i])) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "logical row incidence points to the wrong bucket");
                }
            }
            if (row.active) {
                ++counted_active;
                active_transforms.push_back(&row.sources);
            }
        }
        if (counted_active != active_rows) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "active row count is inconsistent");
        }

        validate_full_source_rank(corpus.size(), active_transforms);

        for (size_t bucket_index = 0; bucket_index < buckets.size(); ++bucket_index) {
            const auto& bucket = buckets[bucket_index];
            validate_lp_key(bucket.key);
            if (bucket_index != 0 && !(buckets[bucket_index - 1].key < bucket.key)) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "LP buckets are not strictly sorted");
            }

            size_t degree = 0;
            StructuredRowId previous{};
            bool have_previous = false;
            for (const auto row_id : bucket.adjacency) {
                if (row_id.value >= rows.size()) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "LP bucket contains an invalid row ID");
                }
                if (have_previous && !(previous < row_id)) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "LP bucket adjacency is not append-canonical");
                }
                previous = row_id;
                have_previous = true;

                const auto& row = rows[static_cast<size_t>(row_id.value)];
                if (!std::binary_search(row.bucket_ids.begin(), row.bucket_ids.end(),
                                        bucket_index)) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "LP bucket adjacency is not symmetric");
                }
                if (row.active)
                    ++degree;
            }
            if (degree != bucket_active_degrees[bucket_index]) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "LP bucket degree is inconsistent");
            }
        }
    }

    [[nodiscard]] std::array<StructuredRowId, 2>
    active_pair(const StructuredIncidenceBucket& bucket) const {
        std::array<StructuredRowId, 2> pair{};
        size_t count = 0;
        for (const auto row_id : bucket.adjacency) {
            if (!rows[static_cast<size_t>(row_id.value)].active)
                continue;
            if (count >= pair.size()) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "degree-two bucket contains more than two active rows");
            }
            pair[count++] = row_id;
        }
        if (count != 2) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "degree-two bucket does not contain two active rows");
        }
        if (pair[1] < pair[0])
            std::swap(pair[0], pair[1]);
        return pair;
    }

    [[nodiscard]] std::vector<StructuredRowId> active_members(size_t bucket_id) const {
        const auto& bucket = buckets[bucket_id];
        const size_t active_degree = bucket_active_degrees[bucket_id];
        std::vector<StructuredRowId> members;
        members.reserve(active_degree);
        for (const auto row_id : bucket.adjacency) {
            if (rows[static_cast<size_t>(row_id.value)].active)
                members.push_back(row_id);
        }
        if (members.size() != active_degree) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "LP bucket active member count is inconsistent");
        }
        return members;
    }

    [[nodiscard]] TreeBasisEdgePlan make_tree_edge(StructuredRowId lhs, StructuredRowId rhs,
                                                   const LargePrimeKey& pivot) const {
        if (rhs < lhs)
            std::swap(lhs, rhs);
        const auto& left = row_at(lhs);
        const auto& right = row_at(rhs);
        auto sources = SourceCombination::symmetric_difference(left.sources, right.sources);
        if (sources.empty()) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "tree edge has identical source transforms");
        }
        auto lp_keys = symmetric_difference_lp_keys(left.lp_keys, right.lp_keys);
        if (contains_lp_key(lp_keys, pivot)) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "tree edge failed to eliminate its pivot");
        }
        return TreeBasisEdgePlan{{lhs, rhs}, std::move(sources), std::move(lp_keys)};
    }

    [[nodiscard]] TreeBasisMergePlan build_tree_plan(size_t bucket_id,
                                                     TreeBasisPlanner planner) const {
        const auto& bucket = buckets[bucket_id];
        const size_t active_degree = bucket_active_degrees[bucket_id];
        if (active_degree < 3 || active_degree > 8) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "tree-basis pivot weight is outside [3,8]");
        }
        if (planner != TreeBasisPlanner::ReferenceStar &&
            planner != TreeBasisPlanner::DeterministicMst) {
            fail(StructuredReductionErrorCode::InvalidPlan, "unknown tree-basis planner");
        }

        TreeBasisMergePlan plan;
        plan.generation = corpus.generation();
        plan.incidence_epoch = incidence_epoch;
        plan.planner = planner;
        plan.pivot = bucket.key;
        plan.members = active_members(bucket_id);

        for (const auto member : plan.members) {
            const auto& keys = row_at(member).lp_keys;
            if (!contains_lp_key(keys, plan.pivot) || keys.empty()) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "tree-basis member does not contain its pivot");
            }
            plan.input_nonpivot_lp_nnz =
                checked_resource_add(plan.input_nonpivot_lp_nnz, keys.size() - 1,
                                     "tree-basis input LP metric overflows");
        }

        if (planner == TreeBasisPlanner::ReferenceStar) {
            plan.edges.reserve(plan.members.size() - 1);
            const StructuredRowId root = plan.members.front();
            for (size_t i = 1; i < plan.members.size(); ++i) {
                plan.edges.push_back(make_tree_edge(root, plan.members[i], plan.pivot));
            }
        } else {
            struct Candidate final {
                size_t lhs_index = 0;
                size_t rhs_index = 0;
                TreeBasisEdgePlan edge;
            };

            std::vector<Candidate> candidates;
            const size_t member_count = plan.members.size();
            candidates.reserve(member_count * (member_count - 1) / 2);
            for (size_t lhs = 0; lhs < member_count; ++lhs) {
                for (size_t rhs = lhs + 1; rhs < member_count; ++rhs) {
                    candidates.push_back(Candidate{
                        lhs, rhs,
                        make_tree_edge(plan.members[lhs], plan.members[rhs], plan.pivot)});
                }
            }
            std::sort(
                candidates.begin(), candidates.end(),
                [](const Candidate& lhs, const Candidate& rhs) {
                    if (lhs.edge.expected_lp_keys.size() != rhs.edge.expected_lp_keys.size()) {
                        return lhs.edge.expected_lp_keys.size() < rhs.edge.expected_lp_keys.size();
                    }
                    if (lhs.edge.expected_sources.size() != rhs.edge.expected_sources.size()) {
                        return lhs.edge.expected_sources.size() < rhs.edge.expected_sources.size();
                    }
                    if (lhs.edge.endpoints[0] != rhs.edge.endpoints[0])
                        return lhs.edge.endpoints[0] < rhs.edge.endpoints[0];
                    return lhs.edge.endpoints[1] < rhs.edge.endpoints[1];
                });

            std::array<size_t, 8> parent{};
            for (size_t i = 0; i < member_count; ++i)
                parent[i] = i;
            auto find_root = [&](size_t node) {
                while (parent[node] != node) {
                    parent[node] = parent[parent[node]];
                    node = parent[node];
                }
                return node;
            };

            plan.edges.reserve(member_count - 1);
            for (auto& candidate : candidates) {
                size_t lhs_root = find_root(candidate.lhs_index);
                size_t rhs_root = find_root(candidate.rhs_index);
                if (lhs_root == rhs_root)
                    continue;
                if (rhs_root < lhs_root)
                    std::swap(lhs_root, rhs_root);
                parent[rhs_root] = lhs_root;
                plan.edges.push_back(std::move(candidate.edge));
                if (plan.edges.size() == member_count - 1)
                    break;
            }
            if (plan.edges.size() != member_count - 1) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "deterministic MST did not span all pivot members");
            }
        }

        for (const auto& edge : plan.edges) {
            plan.output_lp_nnz =
                checked_resource_add(plan.output_lp_nnz, edge.expected_lp_keys.size(),
                                     "tree-basis output LP metric overflows");
        }
        if (plan.output_lp_nnz > plan.input_nonpivot_lp_nnz) {
            plan.lp_fill_growth = plan.output_lp_nnz - plan.input_nonpivot_lp_nnz;
        }
        return plan;
    }

    void validate_tree_shape(const TreeBasisMergePlan& plan) const {
        const size_t member_count = plan.members.size();
        if (member_count < 3 || member_count > 8 || plan.edges.size() != member_count - 1) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "tree-basis plan has invalid dimensions");
        }

        std::array<size_t, 8> parent{};
        for (size_t i = 0; i < member_count; ++i)
            parent[i] = i;
        auto find_root = [&](size_t node) {
            while (parent[node] != node) {
                parent[node] = parent[parent[node]];
                node = parent[node];
            }
            return node;
        };

        for (const auto& edge : plan.edges) {
            if (!(edge.endpoints[0] < edge.endpoints[1])) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "tree-basis edge endpoints are not ordered");
            }
            const auto lhs =
                std::lower_bound(plan.members.begin(), plan.members.end(), edge.endpoints[0]);
            const auto rhs =
                std::lower_bound(plan.members.begin(), plan.members.end(), edge.endpoints[1]);
            if (lhs == plan.members.end() || *lhs != edge.endpoints[0] ||
                rhs == plan.members.end() || *rhs != edge.endpoints[1]) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "tree-basis edge endpoint is not a member");
            }
            size_t lhs_root = find_root(static_cast<size_t>(lhs - plan.members.begin()));
            size_t rhs_root = find_root(static_cast<size_t>(rhs - plan.members.begin()));
            if (lhs_root == rhs_root) {
                fail(StructuredReductionErrorCode::InvalidPlan, "tree-basis edges contain a cycle");
            }
            if (rhs_root < lhs_root)
                std::swap(lhs_root, rhs_root);
            parent[rhs_root] = lhs_root;
        }
        const size_t root = find_root(0);
        for (size_t i = 1; i < member_count; ++i) {
            if (find_root(i) != root) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "tree-basis edges are disconnected");
            }
        }
    }

    [[nodiscard]] TreeBasisMergePlan
    validate_tree_plan_after_state_validation(const TreeBasisMergePlan& plan) const {
        if (plan.generation == 0 || plan.generation != corpus.generation()) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "tree-basis plan belongs to a different generation");
        }
        if (plan.incidence_epoch != incidence_epoch) {
            fail(StructuredReductionErrorCode::StalePlan,
                 "tree-basis plan belongs to a stale incidence epoch");
        }
        if (plan.planner != TreeBasisPlanner::ReferenceStar &&
            plan.planner != TreeBasisPlanner::DeterministicMst) {
            fail(StructuredReductionErrorCode::InvalidPlan, "unknown tree-basis planner");
        }
        try {
            validate_lp_key(plan.pivot);
        } catch (const StructuredReductionError&) {
            fail(StructuredReductionErrorCode::InvalidPlan, "tree-basis pivot is invalid");
        }
        const auto bucket_it =
            std::lower_bound(buckets.begin(), buckets.end(), plan.pivot,
                             [](const StructuredIncidenceBucket& bucket, const LargePrimeKey& key) {
                                 return bucket.key < key;
                             });
        const size_t bucket_id = static_cast<size_t>(bucket_it - buckets.begin());
        if (bucket_it == buckets.end() || !(bucket_it->key == plan.pivot) ||
            bucket_active_degrees[bucket_id] < 3 || bucket_active_degrees[bucket_id] > 8) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "tree-basis pivot is not an active weight-[3,8] bucket");
        }

        TreeBasisMergePlan expected = build_tree_plan(bucket_id, plan.planner);
        if (!(plan == expected)) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "tree-basis plan is not the exact current deterministic plan");
        }
        validate_tree_shape(expected);

        std::vector<const SourceCombination*> prospective;
        prospective.reserve(active_rows - 1);
        for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
            const StructuredRowId row_id{static_cast<uint64_t>(row_index)};
            const auto& row = rows[row_index];
            if (!row.active ||
                std::binary_search(expected.members.begin(), expected.members.end(), row_id)) {
                continue;
            }
            prospective.push_back(&row.sources);
        }
        for (const auto& edge : expected.edges)
            prospective.push_back(&edge.expected_sources);
        if (prospective.size() != active_rows - 1) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "prospective tree-basis row count is inconsistent");
        }
        validate_full_source_rank(corpus.size(), prospective);
        return expected;
    }

    [[nodiscard]] TreeBasisMergePlan validate_tree_plan(const TreeBasisMergePlan& plan) const {
        validate_state();
        return validate_tree_plan_after_state_validation(plan);
    }

    [[nodiscard]] TwoWayMergePlan
    validate_plan_after_state_validation(const TwoWayMergePlan& plan) const {
        if (plan.generation == 0 || plan.generation != corpus.generation()) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "merge plan belongs to a different generation");
        }
        if (plan.incidence_epoch != incidence_epoch) {
            fail(StructuredReductionErrorCode::StalePlan,
                 "merge plan belongs to a stale incidence epoch");
        }
        if (!(plan.members[0] < plan.members[1])) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "merge plan members are not strictly ordered");
        }
        if (plan.members[0].value >= rows.size() || plan.members[1].value >= rows.size()) {
            fail(StructuredReductionErrorCode::InvalidPlan, "merge plan references an invalid row");
        }

        const auto& lhs = row_at(plan.members[0]);
        const auto& rhs = row_at(plan.members[1]);
        if (!lhs.active || !rhs.active) {
            fail(StructuredReductionErrorCode::StalePlan, "merge plan references an inactive row");
        }
        try {
            validate_lp_key(plan.witness);
        } catch (const StructuredReductionError&) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "merge witness is not a valid large-prime key");
        }
        if (!contains_lp_key(lhs.lp_keys, plan.witness) ||
            !contains_lp_key(rhs.lp_keys, plan.witness)) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "merge witness is not shared by both rows");
        }
        const size_t witness_bucket_id = find_bucket(plan.witness);
        const auto& witness_bucket = buckets[witness_bucket_id];
        if (bucket_active_degrees[witness_bucket_id] != 2 ||
            active_pair(witness_bucket) != plan.members) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "merge witness does not identify this exact degree-two pair");
        }

        const auto expected_sources =
            SourceCombination::symmetric_difference(lhs.sources, rhs.sources);
        if (expected_sources.empty()) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "active merge members have identical source transforms");
        }
        try {
            validate_source_combination(plan.expected_sources, false);
        } catch (const StructuredReductionError&) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "merge plan source combination is not canonical");
        }
        if (!(plan.expected_sources == expected_sources)) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "merge plan source combination is not exact");
        }

        const auto expected_lp_keys = symmetric_difference_lp_keys(lhs.lp_keys, rhs.lp_keys);
        try {
            validate_canonical_lp_keys(plan.expected_lp_keys);
        } catch (const StructuredReductionError&) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "merge plan LP support is not canonical");
        }
        if (plan.expected_lp_keys != expected_lp_keys) {
            fail(StructuredReductionErrorCode::InvalidPlan, "merge plan LP support is not exact");
        }
        return plan;
    }

    [[nodiscard]] TwoWayMergePlan validate_plan(const TwoWayMergePlan& plan) const {
        validate_state();
        return validate_plan_after_state_validation(plan);
    }

    [[nodiscard]] Relation materialize_validated_plan(const TwoWayMergePlan& validated) const {
        Relation materialized = corpus.materialize(validated.expected_sources);
        if (odd_large_prime_keys(materialized) != validated.expected_lp_keys) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "materialized LP support differs from logical LP support");
        }
        return materialized;
    }

    [[nodiscard]] PreparedData prepare(const TwoWayMergePlan& plan) const {
        TwoWayMergePlan validated = validate_plan(plan);
        Relation materialized = materialize_validated_plan(validated);
        return PreparedData{std::move(validated), std::move(materialized)};
    }

    [[nodiscard]] StructuredRowId commit(TwoWayMergePlan plan, Relation materialized) {
        TwoWayMergePlan validated = validate_plan(plan);
        Relation expected = corpus.materialize(validated.expected_sources);
        if (!relations_equal(expected, materialized)) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "prepared materialization does not match this corpus");
        }
        if (odd_large_prime_keys(materialized) != validated.expected_lp_keys) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "prepared materialization has the wrong LP support");
        }
        if (incidence_epoch == std::numeric_limits<uint64_t>::max()) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "incidence epoch would overflow during commit");
        }
        const size_t next_two_way_merges =
            checked_resource_add(statistics.two_way_merges, 1, "two-way merge statistics overflow");

        std::vector<size_t> output_bucket_ids;
        output_bucket_ids.reserve(validated.expected_lp_keys.size());
        for (const auto& key : validated.expected_lp_keys) {
            output_bucket_ids.push_back(find_bucket(key));
        }

        if (rows.size() >= rows.max_size() || rows.size() == std::numeric_limits<uint64_t>::max()) {
            fail(StructuredReductionErrorCode::ResourceLimit,
                 "structured row ID space is exhausted");
        }
        const StructuredRowId output_id{static_cast<uint64_t>(rows.size())};

        // Every potentially throwing allocation precedes the first logical
        // state mutation. LP XOR cannot introduce a new bucket.
        rows.reserve(rows.size() + 1);
        for (const size_t bucket_id : output_bucket_ids) {
            auto& adjacency = buckets[bucket_id].adjacency;
            if (adjacency.size() >= adjacency.max_size()) {
                fail(StructuredReductionErrorCode::ResourceLimit,
                     "LP bucket adjacency exceeds vector limits");
            }
            adjacency.reserve(adjacency.size() + 1);
        }

        Row output{std::move(validated.expected_sources), std::move(validated.expected_lp_keys),
                   std::move(output_bucket_ids), true};
        static_assert(std::is_nothrow_move_constructible_v<Row>);
        const auto members = validated.members;

        // From this point onward all operations are no-throw: capacities were
        // reserved and vector moves use the default allocator.
        rows.push_back(std::move(output));
        for (const auto member : members) {
            auto& input = rows[static_cast<size_t>(member.value)];
            input.active = false;
            for (const size_t bucket_id : input.bucket_ids) {
                --bucket_active_degrees[bucket_id];
            }
        }
        for (const size_t bucket_id : rows.back().bucket_ids) {
            buckets[bucket_id].adjacency.push_back(output_id);
            ++bucket_active_degrees[bucket_id];
        }

        --active_rows;
        statistics.two_way_merges = next_two_way_merges;
        statistics.output_rows = active_rows;
        ++incidence_epoch;
        return output_id;
    }

    [[nodiscard]] std::vector<TreeBasisMergePlan>
    plan_tree_basis_merges(TreeBasisPlanner planner) const {
        validate_state();
        if (planner != TreeBasisPlanner::ReferenceStar &&
            planner != TreeBasisPlanner::DeterministicMst) {
            fail(StructuredReductionErrorCode::InvalidPlan, "unknown tree-basis planner");
        }
        struct ScoredPlan final {
            TreeBasisMergePlan plan;
            size_t source_nnz = 0;
        };

        std::vector<ScoredPlan> scored;
        for (size_t bucket_id = 0; bucket_id < buckets.size(); ++bucket_id) {
            const size_t active_degree = bucket_active_degrees[bucket_id];
            if (active_degree < 3 || active_degree > 8)
                continue;
            TreeBasisMergePlan plan = build_tree_plan(bucket_id, planner);
            size_t source_nnz = 0;
            for (const auto& edge : plan.edges) {
                source_nnz = checked_resource_add(source_nnz, edge.expected_sources.size(),
                                                  "tree-basis source metric overflows");
            }
            scored.push_back(ScoredPlan{std::move(plan), source_nnz});
        }

        std::sort(scored.begin(), scored.end(), [](const ScoredPlan& lhs, const ScoredPlan& rhs) {
            if (lhs.plan.output_lp_nnz != rhs.plan.output_lp_nnz)
                return lhs.plan.output_lp_nnz < rhs.plan.output_lp_nnz;
            if (lhs.source_nnz != rhs.source_nnz)
                return lhs.source_nnz < rhs.source_nnz;
            if (lhs.plan.pivot < rhs.plan.pivot)
                return true;
            if (rhs.plan.pivot < lhs.plan.pivot)
                return false;
            return std::lexicographical_compare(lhs.plan.members.begin(), lhs.plan.members.end(),
                                                rhs.plan.members.begin(), rhs.plan.members.end());
        });

        std::vector<TreeBasisMergePlan> plans;
        plans.reserve(scored.size());
        for (auto& candidate : scored)
            plans.push_back(std::move(candidate.plan));
        return plans;
    }

    [[nodiscard]] std::vector<Relation>
    materialize_validated_tree_plan(const TreeBasisMergePlan& validated) const {
        std::vector<Relation> materialized;
        materialized.reserve(validated.edges.size());
        for (const auto& edge : validated.edges) {
            Relation relation = corpus.materialize(edge.expected_sources);
            if (odd_large_prime_keys(relation) != edge.expected_lp_keys) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "tree-basis materialized LP support is inconsistent");
            }
            if (contains_lp_key(edge.expected_lp_keys, validated.pivot)) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "tree-basis materialization retained its pivot");
            }
            materialized.push_back(std::move(relation));
        }
        return materialized;
    }

    [[nodiscard]] PreparedTreeData prepare_tree(const TreeBasisMergePlan& plan) const {
        TreeBasisMergePlan validated = validate_tree_plan(plan);
        auto materialized = materialize_validated_tree_plan(validated);
        return PreparedTreeData{std::move(validated), std::move(materialized)};
    }

    [[nodiscard]] std::vector<StructuredRowId> commit_tree(TreeBasisMergePlan plan,
                                                           std::vector<Relation> materialized) {
        TreeBasisMergePlan validated = validate_tree_plan(plan);
        if (materialized.size() != validated.edges.size()) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "prepared tree-basis relation count is inconsistent");
        }
        for (size_t i = 0; i < validated.edges.size(); ++i) {
            Relation expected = corpus.materialize(validated.edges[i].expected_sources);
            if (!relations_equal(expected, materialized[i])) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "prepared tree-basis materialization does not match corpus");
            }
            if (odd_large_prime_keys(materialized[i]) != validated.edges[i].expected_lp_keys) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "prepared tree-basis LP support is inconsistent");
            }
        }
        if (incidence_epoch == std::numeric_limits<uint64_t>::max()) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "incidence epoch would overflow during tree-basis commit");
        }

        const size_t input_count = validated.members.size();
        const size_t output_count = validated.edges.size();
        if (active_rows < input_count || output_count + 1 != input_count) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "tree-basis active row accounting is inconsistent");
        }
        if (output_count > rows.max_size() - rows.size() ||
            rows.size() > std::numeric_limits<uint64_t>::max() - output_count) {
            fail(StructuredReductionErrorCode::ResourceLimit,
                 "structured row ID capacity is exhausted");
        }

        const size_t next_batches = checked_resource_add(statistics.tree_basis_batches, 1,
                                                         "tree-basis batch statistics overflow");
        const size_t next_consumed =
            checked_resource_add(statistics.tree_basis_rows_consumed, input_count,
                                 "tree-basis consumed-row statistics overflow");
        const size_t next_emitted =
            checked_resource_add(statistics.tree_basis_rows_emitted, output_count,
                                 "tree-basis emitted-row statistics overflow");

        std::vector<Row> staged_rows;
        staged_rows.reserve(output_count);
        std::vector<size_t> append_counts(buckets.size(), 0);
        for (const auto& edge : validated.edges) {
            std::vector<size_t> bucket_ids;
            bucket_ids.reserve(edge.expected_lp_keys.size());
            for (const auto& key : edge.expected_lp_keys) {
                const size_t bucket_id = find_bucket(key);
                bucket_ids.push_back(bucket_id);
                append_counts[bucket_id] = checked_resource_add(
                    append_counts[bucket_id], 1, "tree-basis bucket append count overflows");
            }
            staged_rows.push_back(
                Row{edge.expected_sources, edge.expected_lp_keys, std::move(bucket_ids), true});
        }

        std::vector<StructuredRowId> output_ids;
        output_ids.reserve(output_count);
        const uint64_t first_output = static_cast<uint64_t>(rows.size());
        for (size_t i = 0; i < output_count; ++i) {
            output_ids.push_back(StructuredRowId{first_output + static_cast<uint64_t>(i)});
        }

        // Every allocation precedes the first logical mutation. Output LP
        // support is a symmetric-difference subset of existing buckets.
        rows.reserve(rows.size() + output_count);
        for (size_t bucket_id = 0; bucket_id < buckets.size(); ++bucket_id) {
            const size_t append_count = append_counts[bucket_id];
            if (append_count == 0)
                continue;
            auto& adjacency = buckets[bucket_id].adjacency;
            if (append_count > adjacency.max_size() - adjacency.size()) {
                fail(StructuredReductionErrorCode::ResourceLimit,
                     "LP bucket adjacency capacity is exhausted");
            }
            adjacency.reserve(adjacency.size() + append_count);
        }

        static_assert(std::is_nothrow_move_constructible_v<Row>);
        for (auto& staged : staged_rows)
            rows.push_back(std::move(staged));
        for (const auto member : validated.members) {
            auto& input = rows[static_cast<size_t>(member.value)];
            input.active = false;
            for (const size_t bucket_id : input.bucket_ids)
                --bucket_active_degrees[bucket_id];
        }
        for (size_t i = 0; i < output_count; ++i) {
            const auto output_id = output_ids[i];
            const auto& output = rows[static_cast<size_t>(first_output) + i];
            for (const size_t bucket_id : output.bucket_ids) {
                buckets[bucket_id].adjacency.push_back(output_id);
                ++bucket_active_degrees[bucket_id];
            }
        }

        --active_rows;
        statistics.tree_basis_batches = next_batches;
        statistics.tree_basis_rows_consumed = next_consumed;
        statistics.tree_basis_rows_emitted = next_emitted;
        statistics.output_rows = active_rows;
        ++incidence_epoch;
        return output_ids;
    }

    [[nodiscard]] StructuredBatchCommitResult
    commit_prepared_batch(StructuredIncidenceSnapshotId snapshot,
                          std::span<const StructuredBatchPrepareOutcome> outcomes,
                          std::span<const uint8_t> publish_mask,
                          size_t expected_prepared_candidates,
                          size_t expected_persistence_limited_candidates) {
        validate_state();
        if (snapshot.generation == 0 || snapshot.incidence_epoch == 0) {
            fail(StructuredReductionErrorCode::InvalidGeneration,
                 "prepared batch snapshot identity contains zero");
        }
        if (snapshot.generation != corpus.generation()) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "prepared batch belongs to a different source generation");
        }
        if (snapshot.incidence_epoch != incidence_epoch) {
            fail(StructuredReductionErrorCode::StalePlan,
                 "prepared batch belongs to a stale incidence epoch");
        }
        if (expected_prepared_candidates > outcomes.size() ||
            expected_persistence_limited_candidates >
                outcomes.size() - expected_prepared_candidates) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "prepared batch cached outcome counts are inconsistent");
        }
        if (expected_prepared_candidates + expected_persistence_limited_candidates !=
            outcomes.size()) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "prepared batch is moved-from or has inconsistent outcome counts");
        }
        if (publish_mask.size() != outcomes.size()) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "prepared batch publication mask has the wrong size");
        }

        size_t actual_prepared_candidates = 0;
        size_t actual_persistence_limited_candidates = 0;
        size_t published_prepared_candidates = 0;
        for (size_t outcome_index = 0; outcome_index < outcomes.size(); ++outcome_index) {
            const auto& outcome = outcomes[outcome_index];
            if (outcome.valueless_by_exception()) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "prepared batch contains a valueless outcome");
            }
            if (publish_mask[outcome_index] > 1) {
                fail(StructuredReductionErrorCode::InvalidPlan,
                     "prepared batch publication mask is not boolean");
            }
            if (std::holds_alternative<StructuredBatchPersistenceLimit>(outcome)) {
                if (publish_mask[outcome_index] != 0) {
                    fail(StructuredReductionErrorCode::InvalidPlan,
                         "prepared batch persistence marker cannot be published");
                }
                ++actual_persistence_limited_candidates;
            } else {
                ++actual_prepared_candidates;
                if (publish_mask[outcome_index] != 0)
                    ++published_prepared_candidates;
            }
        }
        if (actual_prepared_candidates != expected_prepared_candidates ||
            actual_persistence_limited_candidates != expected_persistence_limited_candidates) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "prepared batch cached outcome kinds are inconsistent");
        }

        StructuredBatchCommitResult result;
        if (outcomes.size() >= result.output_offsets.max_size()) {
            fail(StructuredReductionErrorCode::ResourceLimit,
                 "prepared batch slot count exceeds vector limits");
        }
        result.output_offsets.reserve(outcomes.size() + 1);
        result.output_offsets.push_back(0);

        std::vector<uint8_t> selected(rows.size(), 0);
        std::vector<uint8_t> claimed(rows.size(), 0);
        std::vector<StructuredRowId> claimed_members;
        std::vector<Row> staged_rows;
        std::vector<size_t> remove_counts(buckets.size(), 0);
        std::vector<size_t> append_counts(buckets.size(), 0);

        size_t total_consumed = 0;
        size_t two_way_candidates = 0;
        size_t tree_candidates = 0;
        size_t tree_rows_consumed = 0;
        size_t tree_rows_emitted = 0;

        auto register_candidate_members = [&](std::span<const StructuredRowId> members,
                                              bool publish) {
            if (publish) {
                total_consumed = checked_resource_add(
                    total_consumed, members.size(), "prepared batch consumed-row count overflows");
            }
            for (const auto member : members) {
                if (member.value >= rows.size()) {
                    fail(StructuredReductionErrorCode::InvalidPlan,
                         "prepared batch member is outside the current row set");
                }
                const size_t member_index = static_cast<size_t>(member.value);
                if (!rows[member_index].active) {
                    fail(StructuredReductionErrorCode::StalePlan,
                         "prepared batch member is no longer active");
                }
                if (selected[member_index] != 0) {
                    fail(StructuredReductionErrorCode::InvalidPlan,
                         "prepared batch candidates are not member-disjoint");
                }
                selected[member_index] = 1;
                if (!publish)
                    continue;
                claimed[member_index] = 1;
                claimed_members.push_back(member);
                for (const size_t bucket_id : rows[member_index].bucket_ids) {
                    remove_counts[bucket_id] =
                        checked_resource_add(remove_counts[bucket_id], 1,
                                             "prepared batch bucket removal count overflows");
                }
            }
        };

        auto stage_output = [&](const SourceCombination& sources,
                                std::span<const LargePrimeKey> lp_keys) {
            std::vector<size_t> bucket_ids;
            bucket_ids.reserve(lp_keys.size());
            for (const auto& key : lp_keys) {
                const size_t bucket_id = find_bucket(key);
                bucket_ids.push_back(bucket_id);
                append_counts[bucket_id] = checked_resource_add(
                    append_counts[bucket_id], 1, "prepared batch bucket append count overflows");
            }
            staged_rows.push_back(Row{sources,
                                      std::vector<LargePrimeKey>(lp_keys.begin(), lp_keys.end()),
                                      std::move(bucket_ids), true});
        };

        for (size_t outcome_index = 0; outcome_index < outcomes.size(); ++outcome_index) {
            const auto& outcome = outcomes[outcome_index];
            const bool publish = publish_mask[outcome_index] != 0;
            if (const auto* two_way = std::get_if<PreparedTwoWayMerge>(&outcome)) {
                TwoWayMergePlan validated = validate_plan_after_state_validation(two_way->plan());
                Relation expected = materialize_validated_plan(validated);
                if (!relations_equal(expected, two_way->materialized_relation())) {
                    fail(StructuredReductionErrorCode::InvalidPlan,
                         "prepared batch materialization does not match this corpus");
                }
                if (odd_large_prime_keys(two_way->materialized_relation()) !=
                    validated.expected_lp_keys) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "prepared batch materialization has the wrong LP support");
                }

                register_candidate_members(validated.members, publish);
                if (publish) {
                    ++two_way_candidates;
                    size_t input_nonpivot_lp_nnz = 0;
                    for (const auto member : validated.members) {
                        const auto& input = rows[static_cast<size_t>(member.value)];
                        if (input.lp_keys.empty()) {
                            fail(StructuredReductionErrorCode::InvariantViolation,
                                 "prepared batch member has empty pivot support");
                        }
                        input_nonpivot_lp_nnz =
                            checked_resource_add(input_nonpivot_lp_nnz, input.lp_keys.size() - 1,
                                                 "prepared batch input LP metric overflows");
                    }
                    if (validated.expected_lp_keys.size() > input_nonpivot_lp_nnz) {
                        result.lp_fill_growth = checked_resource_add(
                            result.lp_fill_growth,
                            validated.expected_lp_keys.size() - input_nonpivot_lp_nnz,
                            "prepared batch LP fill growth overflows");
                    }
                    stage_output(validated.expected_sources, validated.expected_lp_keys);
                }
            } else if (const auto* tree = std::get_if<PreparedTreeBasisMerge>(&outcome)) {
                TreeBasisMergePlan validated =
                    validate_tree_plan_after_state_validation(tree->plan());
                auto expected = materialize_validated_tree_plan(validated);
                const auto materialized = tree->materialized_relations();
                if (expected.size() != materialized.size()) {
                    fail(StructuredReductionErrorCode::InvalidPlan,
                         "prepared batch tree relation count is inconsistent");
                }
                for (size_t i = 0; i < expected.size(); ++i) {
                    if (!relations_equal(expected[i], materialized[i])) {
                        fail(StructuredReductionErrorCode::InvalidPlan,
                             "prepared batch tree materialization does not match this corpus");
                    }
                    if (odd_large_prime_keys(materialized[i]) !=
                        validated.edges[i].expected_lp_keys) {
                        fail(StructuredReductionErrorCode::InvariantViolation,
                             "prepared batch tree materialization has the wrong LP support");
                    }
                }

                register_candidate_members(validated.members, publish);
                if (publish) {
                    ++tree_candidates;
                    tree_rows_consumed =
                        checked_resource_add(tree_rows_consumed, validated.members.size(),
                                             "prepared batch tree consumed-row count overflows");
                    tree_rows_emitted =
                        checked_resource_add(tree_rows_emitted, validated.edges.size(),
                                             "prepared batch tree emitted-row count overflows");
                    result.lp_fill_growth =
                        checked_resource_add(result.lp_fill_growth, validated.lp_fill_growth,
                                             "prepared batch LP fill growth overflows");
                    for (const auto& edge : validated.edges)
                        stage_output(edge.expected_sources, edge.expected_lp_keys);
                }
            } else {
                const auto& limited = std::get<StructuredBatchPersistenceLimit>(outcome);
                if (limited.candidate.valueless_by_exception()) {
                    fail(StructuredReductionErrorCode::InvalidPlan,
                         "prepared batch persistence marker is valueless");
                }
                bool reproduced_persistence_limit = false;
                try {
                    std::visit(
                        [&](const auto& plan) {
                            using Plan = std::remove_cvref_t<decltype(plan)>;
                            if constexpr (std::is_same_v<Plan, TwoWayMergePlan>) {
                                const auto validated = validate_plan_after_state_validation(plan);
                                register_candidate_members(validated.members, false);
                                (void)materialize_validated_plan(validated);
                            } else {
                                const auto validated =
                                    validate_tree_plan_after_state_validation(plan);
                                register_candidate_members(validated.members, false);
                                (void)materialize_validated_tree_plan(validated);
                            }
                        },
                        limited.candidate);
                } catch (const StructuredReductionError& error) {
                    if (error.code() != StructuredReductionErrorCode::PersistenceLimit)
                        throw;
                    reproduced_persistence_limit = true;
                }
                if (!reproduced_persistence_limit) {
                    fail(StructuredReductionErrorCode::InvalidPlan,
                         "prepared batch persistence marker does not match this corpus");
                }
            }
            result.output_offsets.push_back(staged_rows.size());
        }

        result.committed_candidates =
            checked_resource_add(two_way_candidates, tree_candidates,
                                 "prepared batch committed-candidate count overflows");
        result.persistence_limited_candidates = actual_persistence_limited_candidates;
        result.emitted_rows = staged_rows.size();
        if (result.committed_candidates != published_prepared_candidates) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "prepared batch published success count is inconsistent");
        }
        if (total_consumed < result.emitted_rows ||
            total_consumed - result.emitted_rows != result.committed_candidates) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "prepared batch active-row delta is inconsistent");
        }
        if (active_rows < total_consumed) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "prepared batch consumes more active rows than exist");
        }
        const size_t active_after_consumption = active_rows - total_consumed;
        const size_t next_active_rows =
            checked_resource_add(active_after_consumption, result.emitted_rows,
                                 "prepared batch active-row count overflows");
        if (active_rows < result.committed_candidates ||
            next_active_rows != active_rows - result.committed_candidates) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "prepared batch net active-row count is inconsistent");
        }

        if (result.committed_candidates == 0) {
            static_assert(std::is_nothrow_move_constructible_v<StructuredBatchCommitResult>);
            return result;
        }
        if (incidence_epoch == std::numeric_limits<uint64_t>::max()) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "incidence epoch would overflow during prepared batch commit");
        }

        std::vector<const SourceCombination*> prospective;
        prospective.reserve(next_active_rows);
        for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
            if (rows[row_index].active && claimed[row_index] == 0)
                prospective.push_back(&rows[row_index].sources);
        }
        for (const auto& staged : staged_rows)
            prospective.push_back(&staged.sources);
        if (prospective.size() != next_active_rows) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "prepared batch prospective row count is inconsistent");
        }
        validate_full_source_rank(corpus.size(), prospective);

        const size_t next_two_way_merges =
            checked_resource_add(statistics.two_way_merges, two_way_candidates,
                                 "prepared batch two-way merge statistics overflow");
        const size_t next_tree_batches =
            checked_resource_add(statistics.tree_basis_batches, tree_candidates,
                                 "prepared batch tree statistics overflow");
        const size_t next_tree_consumed =
            checked_resource_add(statistics.tree_basis_rows_consumed, tree_rows_consumed,
                                 "prepared batch tree consumed-row statistics overflow");
        const size_t next_tree_emitted =
            checked_resource_add(statistics.tree_basis_rows_emitted, tree_rows_emitted,
                                 "prepared batch tree emitted-row statistics overflow");

        std::vector<size_t> next_active_degrees;
        next_active_degrees.reserve(buckets.size());
        for (size_t bucket_id = 0; bucket_id < buckets.size(); ++bucket_id) {
            const size_t active_degree = bucket_active_degrees[bucket_id];
            if (remove_counts[bucket_id] > active_degree) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "prepared batch bucket removal exceeds active degree");
            }
            const size_t residual_degree = active_degree - remove_counts[bucket_id];
            next_active_degrees.push_back(
                checked_resource_add(residual_degree, append_counts[bucket_id],
                                     "prepared batch bucket degree overflows"));
        }

        if (result.emitted_rows > rows.max_size() - rows.size() ||
            rows.size() > std::numeric_limits<uint64_t>::max() - result.emitted_rows) {
            fail(StructuredReductionErrorCode::ResourceLimit,
                 "prepared batch structured row capacity is exhausted");
        }
        result.output_rows.reserve(result.emitted_rows);
        const uint64_t first_output = static_cast<uint64_t>(rows.size());
        for (size_t i = 0; i < result.emitted_rows; ++i) {
            result.output_rows.push_back(StructuredRowId{first_output + static_cast<uint64_t>(i)});
        }

        // Every potentially throwing operation precedes the logical publish.
        // Existing inactive adjacency entries remain as append-only history.
        rows.reserve(rows.size() + result.emitted_rows);
        for (size_t bucket_id = 0; bucket_id < buckets.size(); ++bucket_id) {
            const size_t append_count = append_counts[bucket_id];
            if (append_count == 0)
                continue;
            auto& adjacency = buckets[bucket_id].adjacency;
            if (append_count > adjacency.max_size() - adjacency.size()) {
                fail(StructuredReductionErrorCode::ResourceLimit,
                     "prepared batch bucket adjacency capacity is exhausted");
            }
            adjacency.reserve(adjacency.size() + append_count);
        }

        static_assert(std::is_nothrow_move_constructible_v<Row>);
        static_assert(std::is_nothrow_move_constructible_v<StructuredRowId>);
        static_assert(std::is_nothrow_move_constructible_v<StructuredBatchCommitResult>);

#if defined(GNFS_STRUCTURED_REDUCTION_TEST_HOOKS)
        invoke_structured_reduction_test_hook(
            active_structured_reduction_test_hook,
            StructuredReductionTestEvent::MaskedBatchCommitBeforePublish);
#endif

        for (auto& staged : staged_rows)
            rows.push_back(std::move(staged));
        for (const auto member : claimed_members)
            rows[static_cast<size_t>(member.value)].active = false;
        for (size_t i = 0; i < result.output_rows.size(); ++i) {
            const auto output_id = result.output_rows[i];
            const auto& output = rows[static_cast<size_t>(output_id.value)];
            for (const size_t bucket_id : output.bucket_ids)
                buckets[bucket_id].adjacency.push_back(output_id);
        }
        for (size_t bucket_id = 0; bucket_id < buckets.size(); ++bucket_id)
            bucket_active_degrees[bucket_id] = next_active_degrees[bucket_id];

        active_rows = next_active_rows;
        statistics.two_way_merges = next_two_way_merges;
        statistics.tree_basis_batches = next_tree_batches;
        statistics.tree_basis_rows_consumed = next_tree_consumed;
        statistics.tree_basis_rows_emitted = next_tree_emitted;
        statistics.output_rows = next_active_rows;
        ++incidence_epoch;
        return result;
    }

    SourceCorpus corpus;
    std::vector<Row> rows;
    std::vector<StructuredIncidenceBucket> buckets;
    std::vector<size_t> bucket_active_degrees;
    size_t active_rows = 0;
    uint64_t incidence_epoch = 1;
    StructuredReductionStats statistics;
    StructuredIncidenceBuildStats incidence_build_statistics;
    std::set<PersistenceFailureKey> persistence_limited_plans;
};

SequentialStructuredReducer::SequentialStructuredReducer(SourceCorpus corpus)
    : SequentialStructuredReducer(std::move(corpus), StructuredIncidenceBuildOptions{}) {}

SequentialStructuredReducer::SequentialStructuredReducer(
    SourceCorpus corpus, const StructuredIncidenceBuildOptions& build_options) {
    auto incidence = build_structured_incidence_shards(corpus, build_options);
    impl_ = consume_prebuilt_incidence(std::move(corpus), std::move(incidence));
}

SequentialStructuredReducer::SequentialStructuredReducer(SourceCorpus corpus,
                                                         StructuredIncidenceBuildResult&& incidence)
    : impl_(consume_prebuilt_incidence(std::move(corpus), std::move(incidence))) {}

std::unique_ptr<SequentialStructuredReducer::Impl>
SequentialStructuredReducer::consume_prebuilt_incidence(
    SourceCorpus corpus, StructuredIncidenceBuildResult&& incidence) {
    validate_prebuilt_incidence(corpus, incidence);

    StructuredIncidenceBuildResult consumed(std::move(incidence));
    return std::make_unique<Impl>(std::move(corpus), std::move(consumed.row_lp_keys_),
                                  std::move(consumed.buckets_), consumed.stats_);
}

SequentialStructuredReducer::SequentialStructuredReducer(uint64_t generation,
                                                         std::vector<Relation> relations)
    : SequentialStructuredReducer(SourceCorpus(generation, std::move(relations))) {}

SequentialStructuredReducer::SequentialStructuredReducer(
    uint64_t generation, std::vector<Relation> relations,
    const StructuredIncidenceBuildOptions& build_options)
    : SequentialStructuredReducer(SourceCorpus(generation, std::move(relations)), build_options) {}

SequentialStructuredReducer::~SequentialStructuredReducer() = default;

SequentialStructuredReducer::SequentialStructuredReducer(SequentialStructuredReducer&&) noexcept =
    default;

SequentialStructuredReducer&
SequentialStructuredReducer::operator=(SequentialStructuredReducer&&) noexcept = default;

const SourceCorpus& SequentialStructuredReducer::corpus() const noexcept {
    return impl_->corpus;
}

uint64_t SequentialStructuredReducer::incidence_epoch() const noexcept {
    return impl_->incidence_epoch;
}

size_t SequentialStructuredReducer::total_row_count() const noexcept {
    return impl_->rows.size();
}

size_t SequentialStructuredReducer::active_row_count() const noexcept {
    return impl_->active_rows;
}

size_t SequentialStructuredReducer::active_lp_column_count() const noexcept {
    return static_cast<size_t>(
        std::count_if(impl_->bucket_active_degrees.begin(), impl_->bucket_active_degrees.end(),
                      [](size_t active_degree) { return active_degree != 0; }));
}

bool SequentialStructuredReducer::is_active(StructuredRowId row) const {
    return impl_->row_at(row).active;
}

const SourceCombination& SequentialStructuredReducer::sources(StructuredRowId row) const {
    return impl_->row_at(row).sources;
}

std::span<const LargePrimeKey> SequentialStructuredReducer::lp_keys(StructuredRowId row) const {
    return impl_->row_at(row).lp_keys;
}

std::vector<StructuredRowId> SequentialStructuredReducer::active_row_ids() const {
    std::vector<StructuredRowId> result;
    result.reserve(impl_->active_rows);
    for (size_t i = 0; i < impl_->rows.size(); ++i) {
        if (impl_->rows[i].active) {
            result.push_back(StructuredRowId{static_cast<uint64_t>(i)});
        }
    }
    return result;
}

size_t SequentialStructuredReducer::peel_singletons() {
    impl_->validate_state();
    std::vector<size_t> pending_storage;
    pending_storage.reserve(impl_->buckets.size());
    std::priority_queue<size_t, std::vector<size_t>, std::greater<size_t>> pending(
        std::greater<size_t>{}, std::move(pending_storage));
    for (size_t bucket_id = 0; bucket_id < impl_->buckets.size(); ++bucket_id) {
        if (impl_->bucket_active_degrees[bucket_id] == 1) {
            pending.push(bucket_id);
        }
    }
    if (!pending.empty()) {
        if (impl_->incidence_epoch == std::numeric_limits<uint64_t>::max()) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "incidence epoch would overflow during singleton peeling");
        }
        (void)checked_resource_add(impl_->statistics.singleton_rows_removed, impl_->active_rows,
                                   "singleton-removal statistics overflow");
    }

    size_t removed = 0;
    while (!pending.empty()) {
        const size_t bucket_id = pending.top();
        pending.pop();
        auto& bucket = impl_->buckets[bucket_id];
        if (impl_->bucket_active_degrees[bucket_id] != 1)
            continue;

        StructuredRowId singleton{};
        bool found = false;
        for (const auto row_id : bucket.adjacency) {
            if (!impl_->rows[static_cast<size_t>(row_id.value)].active)
                continue;
            if (found) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "singleton bucket has multiple active rows");
            }
            singleton = row_id;
            found = true;
        }
        if (!found) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "singleton bucket has no active row");
        }

        auto& row = impl_->row_at(singleton);
        row.active = false;
        --impl_->active_rows;
        ++removed;
        for (const size_t affected_bucket : row.bucket_ids) {
            auto& degree = impl_->bucket_active_degrees[affected_bucket];
            if (degree == 0) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "LP bucket degree underflow during singleton peeling");
            }
            --degree;
            if (degree == 1)
                pending.push(affected_bucket);
        }
    }

    impl_->statistics.singleton_rows_removed = checked_resource_add(
        impl_->statistics.singleton_rows_removed, removed, "singleton-removal statistics overflow");
    impl_->statistics.output_rows = impl_->active_rows;
    if (removed != 0)
        ++impl_->incidence_epoch;
    return removed;
}

std::vector<TwoWayMergePlan> SequentialStructuredReducer::plan_two_way_merges() const {
    impl_->validate_state();
    std::vector<TwoWayMergePlan> plans;
    for (size_t bucket_id = 0; bucket_id < impl_->buckets.size(); ++bucket_id) {
        const auto& bucket = impl_->buckets[bucket_id];
        if (impl_->bucket_active_degrees[bucket_id] != 2)
            continue;
        const auto members = impl_->active_pair(bucket);
        const auto& lhs = impl_->row_at(members[0]);
        const auto& rhs = impl_->row_at(members[1]);
        auto sources = SourceCombination::symmetric_difference(lhs.sources, rhs.sources);
        if (sources.empty()) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "active degree-two rows have identical source transforms");
        }
        auto keys = symmetric_difference_lp_keys(lhs.lp_keys, rhs.lp_keys);
        plans.push_back(TwoWayMergePlan{impl_->corpus.generation(), impl_->incidence_epoch, members,
                                        bucket.key, std::move(sources), std::move(keys)});
    }

    std::sort(plans.begin(), plans.end(),
              [](const TwoWayMergePlan& lhs, const TwoWayMergePlan& rhs) {
                  if (lhs.expected_lp_keys.size() != rhs.expected_lp_keys.size()) {
                      return lhs.expected_lp_keys.size() < rhs.expected_lp_keys.size();
                  }
                  if (lhs.expected_sources.size() != rhs.expected_sources.size()) {
                      return lhs.expected_sources.size() < rhs.expected_sources.size();
                  }
                  if (lhs.witness < rhs.witness)
                      return true;
                  if (rhs.witness < lhs.witness)
                      return false;
                  if (lhs.members[0] < rhs.members[0])
                      return true;
                  if (rhs.members[0] < lhs.members[0])
                      return false;
                  return lhs.members[1] < rhs.members[1];
              });
    return plans;
}

PreparedTwoWayMerge SequentialStructuredReducer::prepare(const TwoWayMergePlan& plan) const {
    auto prepared = impl_->prepare(plan);
    return PreparedTwoWayMerge(std::move(prepared.plan), std::move(prepared.materialized));
}

StructuredRowId SequentialStructuredReducer::commit(PreparedTwoWayMerge&& prepared) {
    return impl_->commit(std::move(prepared.plan_), std::move(prepared.materialized_));
}

void SequentialStructuredReducer::reduce_two_way() {
    impl_->statistics.stop_reason = StructuredReductionStopReason::NotStarted;
    peel_singletons();
    while (true) {
        const auto plans = plan_two_way_merges();
        if (plans.empty()) {
            impl_->statistics.stop_reason = StructuredReductionStopReason::NoCandidates;
            break;
        }
        bool committed = false;
        for (const auto& plan : plans) {
            const std::array<const SourceCombination*, 1> outputs{&plan.expected_sources};
            const PersistenceFailureKey persistence_key = make_persistence_failure_key(outputs);
            if (impl_->persistence_limited_plans.contains(persistence_key)) {
                impl_->statistics.persistence_cache_hits =
                    checked_resource_add(impl_->statistics.persistence_cache_hits, 1,
                                         "persistence-cache hit statistics overflow");
                continue;
            }

            try {
                auto prepared = prepare(plan);
                (void)commit(std::move(prepared));
                committed = true;
                break;
            } catch (const StructuredReductionError& error) {
                if (error.code() != StructuredReductionErrorCode::PersistenceLimit) {
                    throw;
                }
                const size_t next_persistence_limited =
                    checked_resource_add(impl_->statistics.persistence_limited_plans, 1,
                                         "persistence-limited plan statistics overflow");
                if (impl_->persistence_limited_plans.insert(persistence_key).second) {
                    impl_->statistics.persistence_limited_plans = next_persistence_limited;
                }
            }
        }
        if (!committed) {
            impl_->statistics.stop_reason = StructuredReductionStopReason::PersistenceLimit;
            break;
        }
        peel_singletons();
    }
    impl_->statistics.output_rows = impl_->active_rows;
}

std::vector<TreeBasisMergePlan>
SequentialStructuredReducer::plan_tree_basis_merges(TreeBasisPlanner planner) const {
    return impl_->plan_tree_basis_merges(planner);
}

PreparedTreeBasisMerge SequentialStructuredReducer::prepare(const TreeBasisMergePlan& plan) const {
    auto prepared = impl_->prepare_tree(plan);
    return PreparedTreeBasisMerge(std::move(prepared.plan), std::move(prepared.materialized));
}

std::vector<StructuredRowId>
SequentialStructuredReducer::commit(PreparedTreeBasisMerge&& prepared) {
    return impl_->commit_tree(std::move(prepared.plan_), std::move(prepared.materialized_));
}

StructuredBatchCommitResult SequentialStructuredReducer::commit(StructuredPreparedBatch prepared) {
    if (!prepared.valid_) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "prepared batch handle is moved-from or already consumed");
    }
    std::vector<uint8_t> publish_mask;
    publish_mask.reserve(prepared.outcomes_.size());
    for (const auto& outcome : prepared.outcomes_) {
        publish_mask.push_back(
            std::holds_alternative<StructuredBatchPersistenceLimit>(outcome) ? 0 : 1);
    }
    return impl_->commit_prepared_batch(prepared.snapshot_, prepared.outcomes_, publish_mask,
                                        prepared.prepared_candidate_count_,
                                        prepared.persistence_limited_candidate_count_);
}

StructuredPreparedBatch prepare_conflict_free_batch(const SequentialStructuredReducer& reducer,
                                                    const StructuredConflictFreeBatchPlan& batch,
                                                    uint32_t worker_count) {
    if (worker_count == 0) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "structured batch preparation requires at least one worker");
    }

    const auto& impl = *reducer.impl_;
    impl.validate_state();
    if (batch.snapshot.generation == 0 || batch.snapshot.incidence_epoch == 0) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "structured batch snapshot identity contains zero");
    }
    if (batch.snapshot.generation != impl.corpus.generation()) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "structured batch belongs to a different source generation");
    }
    if (batch.snapshot.incidence_epoch != impl.incidence_epoch) {
        fail(StructuredReductionErrorCode::StalePlan,
             "structured batch belongs to a stale incidence epoch");
    }

    size_t accounted_candidates = batch.candidates.size();
    accounted_candidates =
        checked_resource_add(accounted_candidates, batch.duplicate_candidate_count,
                             "structured batch accounting overflows");
    accounted_candidates = checked_resource_add(accounted_candidates, batch.conflict_deferred_count,
                                                "structured batch accounting overflows");
    accounted_candidates = checked_resource_add(accounted_candidates, batch.capacity_deferred_count,
                                                "structured batch accounting overflows");
    if (accounted_candidates != batch.raw_candidate_count) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "structured batch candidate accounting is inconsistent");
    }

    // StructuredConflictFreeBatchPlan is publicly constructible. Re-run the
    // pure selector over the selected subset to prove canonical global order,
    // duplicate freedom, and member disjointness before exact state checks.
    auto canonical = select_conflict_free_batch(batch.snapshot, reducer.total_row_count(),
                                                batch.candidates, batch.candidates.size());
    if (canonical.duplicate_candidate_count != 0 || canonical.conflict_deferred_count != 0 ||
        canonical.capacity_deferred_count != 0 || canonical.candidates != batch.candidates) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "structured batch candidates are not a canonical conflict-free selection");
    }

    // Seal the immutable reducer state once, then validate every exact plan in
    // candidate order without repeating the whole-state rank/integrity pass.
    std::vector<StructuredBatchCandidate> validated;
    validated.reserve(batch.candidates.size());
    for (const auto& candidate : batch.candidates) {
        validated.push_back(std::visit(
            [&](const auto& plan) -> StructuredBatchCandidate {
                using Plan = std::remove_cvref_t<decltype(plan)>;
                if constexpr (std::is_same_v<Plan, TwoWayMergePlan>) {
                    return impl.validate_plan_after_state_validation(plan);
                } else {
                    return impl.validate_tree_plan_after_state_validation(plan);
                }
            },
            candidate));
    }

#if defined(GNFS_STRUCTURED_REDUCTION_TEST_HOOKS)
    const StructuredReductionTestHook* const test_hook = active_structured_reduction_test_hook;
#endif

    auto outcomes = gnfs::util::ordered_parallel_map<StructuredBatchPrepareOutcome>(
        validated.size(), worker_count, [&](size_t index) -> StructuredBatchPrepareOutcome {
#if defined(GNFS_STRUCTURED_REDUCTION_TEST_HOOKS)
            invoke_structured_reduction_test_hook(
                test_hook, StructuredReductionTestEvent::ParallelPrepareSlotStarted, index);
#endif
            auto& candidate = validated[index];
            auto outcome = [&]() -> StructuredBatchPrepareOutcome {
                try {
                    return std::visit(
                        [&](auto& plan) -> StructuredBatchPrepareOutcome {
                            using Plan = std::remove_cvref_t<decltype(plan)>;
                            if constexpr (std::is_same_v<Plan, TwoWayMergePlan>) {
                                Relation materialized = impl.materialize_validated_plan(plan);
                                return PreparedTwoWayMerge(std::move(plan),
                                                           std::move(materialized));
                            } else {
                                auto materialized = impl.materialize_validated_tree_plan(plan);
                                return PreparedTreeBasisMerge(std::move(plan),
                                                              std::move(materialized));
                            }
                        },
                        candidate);
                } catch (const StructuredReductionError& error) {
                    if (error.code() != StructuredReductionErrorCode::PersistenceLimit) {
                        throw;
                    }
                    return StructuredBatchPersistenceLimit{std::move(candidate)};
                }
            }();
#if defined(GNFS_STRUCTURED_REDUCTION_TEST_HOOKS)
            invoke_structured_reduction_test_hook(
                test_hook, StructuredReductionTestEvent::ParallelPrepareSlotCompleted, index);
#endif
            return outcome;
        });

    return StructuredPreparedBatch(batch.snapshot, std::move(outcomes));
}

StructuredReductionRunResult
SequentialStructuredReducer::reduce_budgeted(const StructuredReductionBudget& budget,
                                             TreeBasisPlanner planner) {
    validate_budget(budget);
    if (planner != TreeBasisPlanner::ReferenceStar &&
        planner != TreeBasisPlanner::DeterministicMst) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "unknown budgeted structured-reduction planner");
    }

    enum class BudgetRejection {
        None,
        PivotWeight,
        Source,
        OutputLp,
        Fill,
        EmittedRows,
        Materialization,
    };

    struct CandidateMetadata final {
        size_t pivot_weight = 0;
        size_t emitted_rows = 0;
        size_t output_lp_nnz = 0;
        size_t lp_fill_growth = 0;
        std::vector<const SourceCombination*> outputs;
        std::vector<size_t> output_lp_nnz_by_row;
    };

    auto candidate_metadata = [&](const auto& plan) {
        using Plan = std::remove_cvref_t<decltype(plan)>;
        CandidateMetadata metadata;
        if constexpr (std::is_same_v<Plan, TwoWayMergePlan>) {
            metadata.pivot_weight = plan.members.size();
            metadata.emitted_rows = 1;
            metadata.output_lp_nnz = plan.expected_lp_keys.size();
            metadata.outputs.push_back(&plan.expected_sources);
            metadata.output_lp_nnz_by_row.push_back(plan.expected_lp_keys.size());

            size_t input_nonpivot_lp_nnz = 0;
            for (const StructuredRowId member : plan.members) {
                const auto& input = impl_->row_at(member);
                if (input.lp_keys.empty() || !contains_lp_key(input.lp_keys, plan.witness)) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "two-way budget metadata does not contain its pivot");
                }
                input_nonpivot_lp_nnz =
                    checked_resource_add(input_nonpivot_lp_nnz, input.lp_keys.size() - 1,
                                         "two-way input LP budget metric overflows");
            }
            if (metadata.output_lp_nnz > input_nonpivot_lp_nnz)
                metadata.lp_fill_growth = metadata.output_lp_nnz - input_nonpivot_lp_nnz;
        } else {
            static_assert(std::is_same_v<Plan, TreeBasisMergePlan>);
            metadata.pivot_weight = plan.members.size();
            metadata.emitted_rows = plan.edges.size();
            metadata.outputs.reserve(plan.edges.size());
            metadata.output_lp_nnz_by_row.reserve(plan.edges.size());
            for (const auto& edge : plan.edges) {
                metadata.outputs.push_back(&edge.expected_sources);
                metadata.output_lp_nnz_by_row.push_back(edge.expected_lp_keys.size());
                metadata.output_lp_nnz =
                    checked_resource_add(metadata.output_lp_nnz, edge.expected_lp_keys.size(),
                                         "tree-basis output LP budget metric overflows");
            }
            if (metadata.output_lp_nnz != plan.output_lp_nnz) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "tree-basis output LP budget metric is inconsistent");
            }
            const size_t expected_growth = metadata.output_lp_nnz > plan.input_nonpivot_lp_nnz
                                               ? metadata.output_lp_nnz - plan.input_nonpivot_lp_nnz
                                               : 0;
            if (expected_growth != plan.lp_fill_growth) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "tree-basis fill budget metric is inconsistent");
            }
            metadata.lp_fill_growth = expected_growth;
        }
        return metadata;
    };

    StructuredReductionRunResult result;
    const size_t next_budgeted_runs = checked_resource_add(impl_->statistics.budgeted_runs, 1,
                                                           "budgeted-run statistics overflow");
    impl_->statistics.stop_reason = StructuredReductionStopReason::NotStarted;
    impl_->statistics.budgeted_runs = next_budgeted_runs;

    auto add_run_singletons = [&](size_t removed) {
        result.singleton_rows_removed =
            checked_resource_add(result.singleton_rows_removed, removed,
                                 "budgeted-run singleton-removal count overflows");
    };

    auto record_candidate = [&] {
        impl_->statistics.candidate_plans_considered =
            checked_resource_add(impl_->statistics.candidate_plans_considered, 1,
                                 "considered-candidate statistics overflow");
    };

    auto record_budget_rejection = [&](BudgetRejection rejection) {
        if (rejection == BudgetRejection::None) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "attempted to record an empty budget rejection");
        }
        const size_t next_limited = checked_resource_add(impl_->statistics.budget_limited_plans, 1,
                                                         "budget-limited plan statistics overflow");

        size_t* counter = nullptr;
        switch (rejection) {
        case BudgetRejection::PivotWeight:
            counter = &impl_->statistics.budget_rejections.pivot_weight_limit;
            break;
        case BudgetRejection::Source:
            counter = &impl_->statistics.budget_rejections.source_limit;
            break;
        case BudgetRejection::OutputLp:
            counter = &impl_->statistics.budget_rejections.output_lp_limit;
            break;
        case BudgetRejection::Fill:
            counter = &impl_->statistics.budget_rejections.fill_limit;
            break;
        case BudgetRejection::EmittedRows:
            counter = &impl_->statistics.budget_rejections.emitted_row_limit;
            break;
        case BudgetRejection::Materialization:
            counter = &impl_->statistics.budget_rejections.materialization_limit;
            break;
        case BudgetRejection::None:
            break;
        }
        if (counter == nullptr) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "unknown structured-reduction budget rejection");
        }
        const size_t next_counter =
            checked_resource_add(*counter, 1, "budget-rejection statistics overflow");
        impl_->statistics.budget_limited_plans = next_limited;
        *counter = next_counter;
    };

    auto metadata_rejection = [&](const CandidateMetadata& metadata) {
        if (metadata.pivot_weight > budget.max_pivot_weight)
            return BudgetRejection::PivotWeight;
        for (const SourceCombination* output : metadata.outputs) {
            if (output == nullptr) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "budget metadata contains a null source output");
            }
            if (output->size() > budget.max_source_atoms_per_output)
                return BudgetRejection::Source;
        }
        for (const size_t output_lp_nnz : metadata.output_lp_nnz_by_row) {
            if (output_lp_nnz > budget.max_odd_lp_keys_per_output)
                return BudgetRejection::OutputLp;
        }
        if (metadata.output_lp_nnz > budget.max_output_lp_nnz_per_commit)
            return BudgetRejection::OutputLp;
        const size_t next_fill =
            checked_resource_add(result.lp_fill_growth, metadata.lp_fill_growth,
                                 "budgeted-run LP fill growth overflows");
        if (next_fill > budget.max_total_lp_fill_growth)
            return BudgetRejection::Fill;
        const size_t next_emitted = checked_resource_add(
            result.emitted_rows, metadata.emitted_rows, "budgeted-run emitted-row count overflows");
        if (next_emitted > budget.max_emitted_rows)
            return BudgetRejection::EmittedRows;
        return BudgetRejection::None;
    };

    struct PreparedPayload final {
        size_t entries = 0;
        bool limited = false;
    };
    auto inspect_prepared_payload = [&](std::span<const Relation> relations) {
        PreparedPayload payload;
        for (const Relation& relation : relations) {
            const size_t pairs = checked_resource_add(relation.extra_ab_pairs.size(), 1,
                                                      "prepared materialized pair count overflows");
            if (pairs > budget.max_materialized_pairs_per_output ||
                relation.rational_factors.size() > budget.max_factor_entries_per_side ||
                relation.algebraic_factors.size() > budget.max_factor_entries_per_side ||
                relation.rational_large_prime.size() > budget.max_persisted_lp_entries_per_side ||
                relation.algebraic_large_prime.size() > budget.max_persisted_lp_entries_per_side) {
                payload.limited = true;
            }
            payload.entries = checked_resource_add(payload.entries, pairs,
                                                   "prepared payload entry count overflows");
            payload.entries =
                checked_resource_add(payload.entries, relation.rational_factors.size(),
                                     "prepared payload entry count overflows");
            payload.entries =
                checked_resource_add(payload.entries, relation.algebraic_factors.size(),
                                     "prepared payload entry count overflows");
            payload.entries =
                checked_resource_add(payload.entries, relation.rational_large_prime.size(),
                                     "prepared payload entry count overflows");
            payload.entries =
                checked_resource_add(payload.entries, relation.algebraic_large_prime.size(),
                                     "prepared payload entry count overflows");
        }
        if (payload.entries > budget.max_accepted_materialized_payload_entries_per_commit) {
            payload.limited = true;
        }
        return payload;
    };

    auto finish = [&](StructuredReductionStopReason reason, bool candidate_limit_stop,
                      bool commit_limit_stop) {
        size_t next_candidate_stops = impl_->statistics.candidate_limit_stops;
        size_t next_commit_stops = impl_->statistics.commit_limit_stops;
        size_t next_budget_stops = impl_->statistics.budget_limit_stops;
        if (candidate_limit_stop) {
            next_candidate_stops = checked_resource_add(next_candidate_stops, 1,
                                                        "candidate-limit stop statistics overflow");
        }
        if (commit_limit_stop) {
            next_commit_stops =
                checked_resource_add(next_commit_stops, 1, "commit-limit stop statistics overflow");
        }
        if (reason == StructuredReductionStopReason::BudgetLimit) {
            next_budget_stops =
                checked_resource_add(next_budget_stops, 1, "budget-limit stop statistics overflow");
        }
        impl_->statistics.candidate_limit_stops = next_candidate_stops;
        impl_->statistics.commit_limit_stops = next_commit_stops;
        impl_->statistics.budget_limit_stops = next_budget_stops;
        impl_->statistics.output_rows = impl_->active_rows;
        impl_->statistics.stop_reason = reason;
        result.stop_reason = reason;
        return result;
    };

    add_run_singletons(peel_singletons());
    while (true) {
        impl_->statistics.planning_passes =
            checked_resource_add(impl_->statistics.planning_passes, 1,
                                 "structured-reduction planning-pass statistics overflow");
        const auto two_way_plans = plan_two_way_merges();
        const auto tree_plans = plan_tree_basis_merges(planner);
        const size_t raw_candidate_count =
            checked_resource_add(two_way_plans.size(), tree_plans.size(),
                                 "structured-reduction candidate count overflows");
        if (raw_candidate_count == 0)
            return finish(StructuredReductionStopReason::NoCandidates, false, false);
        if (result.commits >= budget.max_commits)
            return finish(StructuredReductionStopReason::BudgetLimit, false, true);

        size_t candidate_examinations_this_pass = 0;
        bool committed = false;
        bool candidate_limit_stop = false;
        bool saw_persistence_limit = false;

        auto consider = [&](const auto& plan) {
            const CandidateMetadata metadata = candidate_metadata(plan);
            const BudgetRejection metadata_limit = metadata_rejection(metadata);
            if (metadata_limit != BudgetRejection::None) {
                if (candidate_examinations_this_pass >=
                    budget.max_candidate_examinations_per_pass) {
                    candidate_limit_stop = true;
                    return;
                }
                ++candidate_examinations_this_pass;
                record_candidate();
                record_budget_rejection(metadata_limit);
                return;
            }

            const auto output_span = std::span<const SourceCombination* const>(
                metadata.outputs.data(), metadata.outputs.size());
            const PersistenceFailureKey persistence_key = make_persistence_failure_key(output_span);
            if (impl_->persistence_limited_plans.contains(persistence_key)) {
                impl_->statistics.persistence_cache_hits =
                    checked_resource_add(impl_->statistics.persistence_cache_hits, 1,
                                         "persistence-cache hit statistics overflow");
                saw_persistence_limit = true;
                return;
            }
            if (candidate_examinations_this_pass >= budget.max_candidate_examinations_per_pass) {
                candidate_limit_stop = true;
                return;
            }
            ++candidate_examinations_this_pass;
            record_candidate();

            try {
                auto prepared = prepare(plan);
                std::span<const Relation> materialized;
                using Plan = std::remove_cvref_t<decltype(plan)>;
                if constexpr (std::is_same_v<Plan, TwoWayMergePlan>) {
                    materialized = std::span<const Relation>(&prepared.materialized_relation(), 1);
                } else {
                    static_assert(std::is_same_v<Plan, TreeBasisMergePlan>);
                    materialized = prepared.materialized_relations();
                }
                if (materialized.size() != metadata.emitted_rows) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "prepared output count differs from budget metadata");
                }

                const PreparedPayload payload = inspect_prepared_payload(materialized);
                impl_->statistics.peak_prepared_payload_entries =
                    std::max(impl_->statistics.peak_prepared_payload_entries, payload.entries);
                if (payload.limited) {
                    record_budget_rejection(BudgetRejection::Materialization);
                    return;
                }

                const size_t next_commits =
                    checked_resource_add(result.commits, 1, "budgeted-run commit count overflows");
                const size_t next_emitted =
                    checked_resource_add(result.emitted_rows, metadata.emitted_rows,
                                         "budgeted-run emitted-row count overflows");
                const size_t next_fill =
                    checked_resource_add(result.lp_fill_growth, metadata.lp_fill_growth,
                                         "budgeted-run LP fill growth overflows");
                const size_t next_accepted_fill = checked_resource_add(
                    impl_->statistics.accepted_lp_fill_growth, metadata.lp_fill_growth,
                    "accepted LP fill-growth statistics overflow");

                (void)commit(std::move(prepared));
                result.commits = next_commits;
                result.emitted_rows = next_emitted;
                result.lp_fill_growth = next_fill;
                impl_->statistics.accepted_lp_fill_growth = next_accepted_fill;
                committed = true;
            } catch (const StructuredReductionError& error) {
                if (error.code() != StructuredReductionErrorCode::PersistenceLimit)
                    throw;
                saw_persistence_limit = true;
                const size_t next_persistence_limited =
                    checked_resource_add(impl_->statistics.persistence_limited_plans, 1,
                                         "persistence-limited plan statistics overflow");
                if (impl_->persistence_limited_plans.insert(persistence_key).second) {
                    impl_->statistics.persistence_limited_plans = next_persistence_limited;
                }
            }
        };

        for (const auto& plan : two_way_plans) {
            consider(plan);
            if (committed || candidate_limit_stop)
                break;
        }
        if (!committed && !candidate_limit_stop) {
            for (const auto& plan : tree_plans) {
                consider(plan);
                if (committed || candidate_limit_stop)
                    break;
            }
        }

        if (committed) {
            add_run_singletons(peel_singletons());
            continue;
        }
        if (candidate_limit_stop)
            return finish(StructuredReductionStopReason::BudgetLimit, true, false);
        if (saw_persistence_limit)
            return finish(StructuredReductionStopReason::PersistenceLimit, false, false);
        return finish(StructuredReductionStopReason::BudgetLimit, false, false);
    }
}

StructuredReductionRunResult SequentialStructuredReducer::reduce_budgeted_parallel(
    const StructuredReductionBudget& budget, const StructuredParallelReductionOptions& options,
    TreeBasisPlanner planner) {
    validate_budget(budget);
    if (options.max_batch_candidates == 0 || options.worker_count == 0) {
        fail(StructuredReductionErrorCode::InvalidInput,
             "parallel structured reduction requires nonzero batch width and worker count");
    }
    if (planner != TreeBasisPlanner::ReferenceStar &&
        planner != TreeBasisPlanner::DeterministicMst) {
        fail(StructuredReductionErrorCode::InvalidPlan,
             "unknown parallel structured-reduction planner");
    }

    enum class BudgetRejection {
        None,
        PivotWeight,
        Source,
        OutputLp,
        Fill,
        EmittedRows,
        Materialization,
    };

    struct CandidateMetadata final {
        size_t pivot_weight = 0;
        size_t emitted_rows = 0;
        size_t output_lp_nnz = 0;
        size_t lp_fill_growth = 0;
        std::vector<const SourceCombination*> outputs;
        std::vector<size_t> output_lp_nnz_by_row;
    };

    auto candidate_metadata = [&](const auto& plan) {
        using Plan = std::remove_cvref_t<decltype(plan)>;
        CandidateMetadata metadata;
        if constexpr (std::is_same_v<Plan, TwoWayMergePlan>) {
            metadata.pivot_weight = plan.members.size();
            metadata.emitted_rows = 1;
            metadata.output_lp_nnz = plan.expected_lp_keys.size();
            metadata.outputs.push_back(&plan.expected_sources);
            metadata.output_lp_nnz_by_row.push_back(plan.expected_lp_keys.size());

            size_t input_nonpivot_lp_nnz = 0;
            for (const StructuredRowId member : plan.members) {
                const auto& input = impl_->row_at(member);
                if (input.lp_keys.empty() || !contains_lp_key(input.lp_keys, plan.witness)) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "parallel two-way budget metadata does not contain its pivot");
                }
                input_nonpivot_lp_nnz =
                    checked_resource_add(input_nonpivot_lp_nnz, input.lp_keys.size() - 1,
                                         "parallel two-way input LP budget metric overflows");
            }
            if (metadata.output_lp_nnz > input_nonpivot_lp_nnz)
                metadata.lp_fill_growth = metadata.output_lp_nnz - input_nonpivot_lp_nnz;
        } else {
            static_assert(std::is_same_v<Plan, TreeBasisMergePlan>);
            metadata.pivot_weight = plan.members.size();
            metadata.emitted_rows = plan.edges.size();
            metadata.outputs.reserve(plan.edges.size());
            metadata.output_lp_nnz_by_row.reserve(plan.edges.size());
            for (const auto& edge : plan.edges) {
                metadata.outputs.push_back(&edge.expected_sources);
                metadata.output_lp_nnz_by_row.push_back(edge.expected_lp_keys.size());
                metadata.output_lp_nnz =
                    checked_resource_add(metadata.output_lp_nnz, edge.expected_lp_keys.size(),
                                         "parallel tree-basis output LP budget metric overflows");
            }
            if (metadata.output_lp_nnz != plan.output_lp_nnz) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "parallel tree-basis output LP budget metric is inconsistent");
            }
            const size_t expected_growth = metadata.output_lp_nnz > plan.input_nonpivot_lp_nnz
                                               ? metadata.output_lp_nnz - plan.input_nonpivot_lp_nnz
                                               : 0;
            if (expected_growth != plan.lp_fill_growth) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "parallel tree-basis fill budget metric is inconsistent");
            }
            metadata.lp_fill_growth = expected_growth;
        }
        return metadata;
    };

    auto metadata_for_candidate = [&](const StructuredBatchCandidate& candidate) {
        if (candidate.valueless_by_exception()) {
            fail(StructuredReductionErrorCode::InvalidPlan,
                 "parallel scheduler received a valueless candidate");
        }
        return std::visit([&](const auto& plan) { return candidate_metadata(plan); }, candidate);
    };

    auto metadata_rejection = [&](const CandidateMetadata& metadata, size_t current_fill,
                                  size_t current_emitted) {
        if (metadata.pivot_weight > budget.max_pivot_weight)
            return BudgetRejection::PivotWeight;
        for (const SourceCombination* output : metadata.outputs) {
            if (output == nullptr) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "parallel budget metadata contains a null source output");
            }
            if (output->size() > budget.max_source_atoms_per_output)
                return BudgetRejection::Source;
        }
        for (const size_t output_lp_nnz : metadata.output_lp_nnz_by_row) {
            if (output_lp_nnz > budget.max_odd_lp_keys_per_output)
                return BudgetRejection::OutputLp;
        }
        if (metadata.output_lp_nnz > budget.max_output_lp_nnz_per_commit)
            return BudgetRejection::OutputLp;
        const size_t next_fill =
            checked_resource_add(current_fill, metadata.lp_fill_growth,
                                 "parallel budgeted-run LP fill growth overflows");
        if (next_fill > budget.max_total_lp_fill_growth)
            return BudgetRejection::Fill;
        const size_t next_emitted =
            checked_resource_add(current_emitted, metadata.emitted_rows,
                                 "parallel budgeted-run emitted-row count overflows");
        if (next_emitted > budget.max_emitted_rows)
            return BudgetRejection::EmittedRows;
        return BudgetRejection::None;
    };

    struct PreparedPayload final {
        size_t entries = 0;
        bool limited = false;
    };
    auto inspect_prepared_payload = [&](std::span<const Relation> relations) {
        PreparedPayload payload;
        for (const Relation& relation : relations) {
            const size_t pairs =
                checked_resource_add(relation.extra_ab_pairs.size(), 1,
                                     "parallel prepared materialized pair count overflows");
            if (pairs > budget.max_materialized_pairs_per_output ||
                relation.rational_factors.size() > budget.max_factor_entries_per_side ||
                relation.algebraic_factors.size() > budget.max_factor_entries_per_side ||
                relation.rational_large_prime.size() > budget.max_persisted_lp_entries_per_side ||
                relation.algebraic_large_prime.size() > budget.max_persisted_lp_entries_per_side) {
                payload.limited = true;
            }
            payload.entries = checked_resource_add(
                payload.entries, pairs, "parallel prepared payload entry count overflows");
            payload.entries =
                checked_resource_add(payload.entries, relation.rational_factors.size(),
                                     "parallel prepared payload entry count overflows");
            payload.entries =
                checked_resource_add(payload.entries, relation.algebraic_factors.size(),
                                     "parallel prepared payload entry count overflows");
            payload.entries =
                checked_resource_add(payload.entries, relation.rational_large_prime.size(),
                                     "parallel prepared payload entry count overflows");
            payload.entries =
                checked_resource_add(payload.entries, relation.algebraic_large_prime.size(),
                                     "parallel prepared payload entry count overflows");
        }
        if (payload.entries > budget.max_accepted_materialized_payload_entries_per_commit)
            payload.limited = true;
        return payload;
    };

    struct RoundAccounting final {
        size_t candidate_plans_considered = 0;
        size_t budget_limited_plans = 0;
        size_t persistence_cache_hits = 0;
        size_t persistence_limited_plans = 0;
        size_t peak_prepared_payload_entries = 0;
        StructuredReductionRejectionStats budget_rejections;
    };

    auto record_budget_rejection = [&](RoundAccounting& accounting, BudgetRejection rejection) {
        if (rejection == BudgetRejection::None) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "attempted to record an empty parallel budget rejection");
        }
        accounting.budget_limited_plans = checked_resource_add(
            accounting.budget_limited_plans, 1, "parallel budget-limited plan count overflows");
        size_t* counter = nullptr;
        switch (rejection) {
        case BudgetRejection::PivotWeight:
            counter = &accounting.budget_rejections.pivot_weight_limit;
            break;
        case BudgetRejection::Source:
            counter = &accounting.budget_rejections.source_limit;
            break;
        case BudgetRejection::OutputLp:
            counter = &accounting.budget_rejections.output_lp_limit;
            break;
        case BudgetRejection::Fill:
            counter = &accounting.budget_rejections.fill_limit;
            break;
        case BudgetRejection::EmittedRows:
            counter = &accounting.budget_rejections.emitted_row_limit;
            break;
        case BudgetRejection::Materialization:
            counter = &accounting.budget_rejections.materialization_limit;
            break;
        case BudgetRejection::None:
            break;
        }
        if (counter == nullptr) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "unknown parallel structured-reduction budget rejection");
        }
        *counter = checked_resource_add(*counter, 1, "parallel budget-rejection counter overflows");
    };

    struct NextSchedulerStats final {
        size_t planning_passes = 0;
        size_t candidate_plans_considered = 0;
        size_t budget_limited_plans = 0;
        size_t persistence_cache_hits = 0;
        size_t persistence_limited_plans = 0;
        size_t peak_prepared_payload_entries = 0;
        size_t accepted_lp_fill_growth = 0;
        StructuredReductionRejectionStats budget_rejections;
    };

    auto next_scheduler_stats = [&](const RoundAccounting& accounting,
                                    size_t accepted_fill_growth) {
        NextSchedulerStats next;
        next.planning_passes = checked_resource_add(impl_->statistics.planning_passes, 1,
                                                    "parallel planning-pass statistics overflow");
        next.candidate_plans_considered = checked_resource_add(
            impl_->statistics.candidate_plans_considered, accounting.candidate_plans_considered,
            "parallel considered-candidate statistics overflow");
        next.budget_limited_plans = checked_resource_add(
            impl_->statistics.budget_limited_plans, accounting.budget_limited_plans,
            "parallel budget-limited plan statistics overflow");
        next.persistence_cache_hits = checked_resource_add(
            impl_->statistics.persistence_cache_hits, accounting.persistence_cache_hits,
            "parallel persistence-cache hit statistics overflow");
        next.persistence_limited_plans = checked_resource_add(
            impl_->statistics.persistence_limited_plans, accounting.persistence_limited_plans,
            "parallel persistence-limited plan statistics overflow");
        next.peak_prepared_payload_entries =
            std::max(impl_->statistics.peak_prepared_payload_entries,
                     accounting.peak_prepared_payload_entries);
        next.accepted_lp_fill_growth =
            checked_resource_add(impl_->statistics.accepted_lp_fill_growth, accepted_fill_growth,
                                 "parallel accepted LP fill-growth statistics overflow");

        const auto& current = impl_->statistics.budget_rejections;
        const auto& delta = accounting.budget_rejections;
        next.budget_rejections.pivot_weight_limit =
            checked_resource_add(current.pivot_weight_limit, delta.pivot_weight_limit,
                                 "parallel pivot-weight rejection statistics overflow");
        next.budget_rejections.source_limit =
            checked_resource_add(current.source_limit, delta.source_limit,
                                 "parallel source rejection statistics overflow");
        next.budget_rejections.output_lp_limit =
            checked_resource_add(current.output_lp_limit, delta.output_lp_limit,
                                 "parallel output-LP rejection statistics overflow");
        next.budget_rejections.fill_limit = checked_resource_add(
            current.fill_limit, delta.fill_limit, "parallel fill rejection statistics overflow");
        next.budget_rejections.emitted_row_limit =
            checked_resource_add(current.emitted_row_limit, delta.emitted_row_limit,
                                 "parallel emitted-row rejection statistics overflow");
        next.budget_rejections.materialization_limit =
            checked_resource_add(current.materialization_limit, delta.materialization_limit,
                                 "parallel materialization rejection statistics overflow");
        return next;
    };

    auto apply_scheduler_stats = [&](const NextSchedulerStats& next) noexcept {
        impl_->statistics.planning_passes = next.planning_passes;
        impl_->statistics.candidate_plans_considered = next.candidate_plans_considered;
        impl_->statistics.budget_limited_plans = next.budget_limited_plans;
        impl_->statistics.persistence_cache_hits = next.persistence_cache_hits;
        impl_->statistics.persistence_limited_plans = next.persistence_limited_plans;
        impl_->statistics.peak_prepared_payload_entries = next.peak_prepared_payload_entries;
        impl_->statistics.accepted_lp_fill_growth = next.accepted_lp_fill_growth;
        impl_->statistics.budget_rejections = next.budget_rejections;
    };

    StructuredReductionRunResult result;
    const size_t next_budgeted_runs = checked_resource_add(
        impl_->statistics.budgeted_runs, 1, "parallel budgeted-run statistics overflow");
    impl_->statistics.stop_reason = StructuredReductionStopReason::NotStarted;
    impl_->statistics.budgeted_runs = next_budgeted_runs;

    auto add_run_singletons = [&](size_t removed) {
        result.singleton_rows_removed =
            checked_resource_add(result.singleton_rows_removed, removed,
                                 "parallel budgeted-run singleton-removal count overflows");
    };

    auto finish = [&](StructuredReductionStopReason reason, bool candidate_limit_stop,
                      bool commit_limit_stop, const RoundAccounting& accounting,
                      std::set<PersistenceFailureKey>* staged_persistence_cache) {
        NextSchedulerStats next = next_scheduler_stats(accounting, 0);
        size_t next_candidate_stops = impl_->statistics.candidate_limit_stops;
        size_t next_commit_stops = impl_->statistics.commit_limit_stops;
        size_t next_budget_stops = impl_->statistics.budget_limit_stops;
        if (candidate_limit_stop) {
            next_candidate_stops = checked_resource_add(
                next_candidate_stops, 1, "parallel candidate-limit stop statistics overflow");
        }
        if (commit_limit_stop) {
            next_commit_stops = checked_resource_add(
                next_commit_stops, 1, "parallel commit-limit stop statistics overflow");
        }
        if (reason == StructuredReductionStopReason::BudgetLimit) {
            next_budget_stops = checked_resource_add(
                next_budget_stops, 1, "parallel budget-limit stop statistics overflow");
        }

        if (staged_persistence_cache != nullptr) {
            static_assert(noexcept(std::declval<std::set<PersistenceFailureKey>&>().swap(
                std::declval<std::set<PersistenceFailureKey>&>())));
            impl_->persistence_limited_plans.swap(*staged_persistence_cache);
        }
        apply_scheduler_stats(next);
        impl_->statistics.candidate_limit_stops = next_candidate_stops;
        impl_->statistics.commit_limit_stops = next_commit_stops;
        impl_->statistics.budget_limit_stops = next_budget_stops;
        impl_->statistics.output_rows = impl_->active_rows;
        impl_->statistics.stop_reason = reason;
        result.stop_reason = reason;
        return result;
    };

    add_run_singletons(peel_singletons());
    while (true) {
        RoundAccounting accounting;
        const auto two_way_plans = plan_two_way_merges();
        const auto tree_plans = plan_tree_basis_merges(planner);
        const size_t raw_candidate_count =
            checked_resource_add(two_way_plans.size(), tree_plans.size(),
                                 "parallel structured-reduction candidate count overflows");
        if (raw_candidate_count == 0)
            return finish(StructuredReductionStopReason::NoCandidates, false, false, accounting,
                          nullptr);
        if (result.commits >= budget.max_commits)
            return finish(StructuredReductionStopReason::BudgetLimit, false, true, accounting,
                          nullptr);

        std::vector<StructuredBatchCandidate> remaining_candidates;
        remaining_candidates.reserve(raw_candidate_count);
        for (const auto& plan : two_way_plans)
            remaining_candidates.emplace_back(plan);
        for (const auto& plan : tree_plans)
            remaining_candidates.emplace_back(plan);

        std::set<PersistenceFailureKey> staged_persistence_cache = impl_->persistence_limited_plans;
        bool candidate_limit_stop = false;
        bool saw_persistence_limit = false;
        bool committed = false;

        const size_t remaining_commit_slots = budget.max_commits - result.commits;
        const size_t batch_width = std::min(options.max_batch_candidates, remaining_commit_slots);
        if (batch_width == 0) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "parallel scheduler has no commit capacity after preflight");
        }

        while (!remaining_candidates.empty()) {
            const StructuredIncidenceSnapshotId snapshot{impl_->corpus.generation(),
                                                         impl_->incidence_epoch};
            auto selected = select_conflict_free_batch(snapshot, total_row_count(),
                                                       remaining_candidates, batch_width);
            if (selected.candidates.empty()) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "parallel scheduler failed to select a nonempty wave");
            }

            std::vector<StructuredBatchCandidate> dispatch_candidates;
            dispatch_candidates.reserve(selected.candidates.size());
            size_t processed_candidate_count = 0;
            size_t tentative_fill = result.lp_fill_growth;
            size_t tentative_emitted = result.emitted_rows;
            for (const auto& candidate : selected.candidates) {
                const CandidateMetadata metadata = metadata_for_candidate(candidate);
                const BudgetRejection rejection =
                    metadata_rejection(metadata, tentative_fill, tentative_emitted);
                if (rejection != BudgetRejection::None) {
                    // A prepared prefix may publish. Leave later deterministic
                    // outcomes for the next epoch instead of accounting for
                    // work that the sequential policy would not yet observe.
                    if (!dispatch_candidates.empty())
                        break;
                    if (accounting.candidate_plans_considered >=
                        budget.max_candidate_examinations_per_pass) {
                        candidate_limit_stop = true;
                        break;
                    }
                    accounting.candidate_plans_considered =
                        checked_resource_add(accounting.candidate_plans_considered, 1,
                                             "parallel candidate-examination count overflows");
                    record_budget_rejection(accounting, rejection);
                    ++processed_candidate_count;
                    continue;
                }

                const auto output_span = std::span<const SourceCombination* const>(
                    metadata.outputs.data(), metadata.outputs.size());
                const PersistenceFailureKey persistence_key =
                    make_persistence_failure_key(output_span);
                if (staged_persistence_cache.contains(persistence_key)) {
                    // Preserve metadata-before-cache precedence after any
                    // successful publication changes cumulative run budgets.
                    if (!dispatch_candidates.empty())
                        break;
                    accounting.persistence_cache_hits =
                        checked_resource_add(accounting.persistence_cache_hits, 1,
                                             "parallel persistence-cache hit count overflows");
                    saw_persistence_limit = true;
                    ++processed_candidate_count;
                    continue;
                }
                if (accounting.candidate_plans_considered >=
                    budget.max_candidate_examinations_per_pass) {
                    candidate_limit_stop = true;
                    break;
                }
                accounting.candidate_plans_considered =
                    checked_resource_add(accounting.candidate_plans_considered, 1,
                                         "parallel candidate-examination count overflows");
                dispatch_candidates.push_back(candidate);
                ++processed_candidate_count;
                tentative_fill =
                    checked_resource_add(tentative_fill, metadata.lp_fill_growth,
                                         "parallel tentative LP fill-growth reservation overflows");
                tentative_emitted =
                    checked_resource_add(tentative_emitted, metadata.emitted_rows,
                                         "parallel tentative emitted-row reservation overflows");
            }

            if (dispatch_candidates.empty()) {
                if (candidate_limit_stop)
                    break;
                for (size_t index = 0; index < processed_candidate_count; ++index)
                    std::erase(remaining_candidates, selected.candidates[index]);
                continue;
            }

            auto dispatch = select_conflict_free_batch(snapshot, total_row_count(),
                                                       std::move(dispatch_candidates),
                                                       selected.candidates.size());
            auto prepared = prepare_conflict_free_batch(*this, dispatch, options.worker_count);
            std::vector<uint8_t> publish_mask(prepared.outcomes_.size(), 0);
            size_t next_commits = result.commits;
            size_t next_emitted_rows = result.emitted_rows;
            size_t next_lp_fill_growth = result.lp_fill_growth;

            for (size_t slot = 0; slot < prepared.outcomes_.size(); ++slot) {
                const auto& outcome = prepared.outcomes_[slot];
                const auto& candidate = dispatch.candidates[slot];
                const CandidateMetadata metadata = metadata_for_candidate(candidate);

                const size_t candidate_fill =
                    checked_resource_add(next_lp_fill_growth, metadata.lp_fill_growth,
                                         "parallel admitted LP fill growth overflows");
                if (candidate_fill > budget.max_total_lp_fill_growth) {
                    record_budget_rejection(accounting, BudgetRejection::Fill);
                    continue;
                }
                const size_t candidate_emitted =
                    checked_resource_add(next_emitted_rows, metadata.emitted_rows,
                                         "parallel admitted emitted-row count overflows");
                if (candidate_emitted > budget.max_emitted_rows) {
                    record_budget_rejection(accounting, BudgetRejection::EmittedRows);
                    continue;
                }

                if (std::holds_alternative<StructuredBatchPersistenceLimit>(outcome)) {
                    const auto output_span = std::span<const SourceCombination* const>(
                        metadata.outputs.data(), metadata.outputs.size());
                    PersistenceFailureKey key = make_persistence_failure_key(output_span);
                    if (staged_persistence_cache.insert(std::move(key)).second) {
                        accounting.persistence_limited_plans = checked_resource_add(
                            accounting.persistence_limited_plans, 1,
                            "parallel persistence-limited plan count overflows");
                    }
                    saw_persistence_limit = true;
                    continue;
                }

                std::span<const Relation> materialized;
                if (const auto* two_way = std::get_if<PreparedTwoWayMerge>(&outcome)) {
                    materialized = std::span<const Relation>(&two_way->materialized_relation(), 1);
                } else {
                    materialized =
                        std::get<PreparedTreeBasisMerge>(outcome).materialized_relations();
                }
                if (materialized.size() != metadata.emitted_rows) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "parallel prepared output count differs from budget metadata");
                }
                const PreparedPayload payload = inspect_prepared_payload(materialized);
                accounting.peak_prepared_payload_entries =
                    std::max(accounting.peak_prepared_payload_entries, payload.entries);

                if (payload.limited) {
                    record_budget_rejection(accounting, BudgetRejection::Materialization);
                    continue;
                }

                const size_t candidate_commits = checked_resource_add(
                    next_commits, 1, "parallel admitted commit count overflows");
                if (candidate_commits > budget.max_commits) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "parallel batch exceeded its preflight commit capacity");
                }
                publish_mask[slot] = 1;
                next_commits = candidate_commits;
                next_emitted_rows = candidate_emitted;
                next_lp_fill_growth = candidate_fill;
            }

            const bool publishes_any = std::find(publish_mask.begin(), publish_mask.end(),
                                                 uint8_t{1}) != publish_mask.end();
            if (publishes_any) {
                const size_t accepted_fill_growth = next_lp_fill_growth - result.lp_fill_growth;
                const NextSchedulerStats next_stats =
                    next_scheduler_stats(accounting, accepted_fill_growth);

                (void)impl_->commit_prepared_batch(prepared.snapshot_, prepared.outcomes_,
                                                   publish_mask, prepared.prepared_candidate_count_,
                                                   prepared.persistence_limited_candidate_count_);

                impl_->persistence_limited_plans.swap(staged_persistence_cache);
                apply_scheduler_stats(next_stats);
                result.commits = next_commits;
                result.emitted_rows = next_emitted_rows;
                result.lp_fill_growth = next_lp_fill_growth;
#if defined(GNFS_STRUCTURED_REDUCTION_TEST_HOOKS)
                invoke_structured_reduction_test_hook(
                    active_structured_reduction_test_hook,
                    StructuredReductionTestEvent::ParallelDriverBeforePostCommitPeel);
#endif
                add_run_singletons(peel_singletons());
                committed = true;
                break;
            }

            for (size_t index = 0; index < processed_candidate_count; ++index)
                std::erase(remaining_candidates, selected.candidates[index]);
            if (candidate_limit_stop)
                break;
        }

        if (committed)
            continue;
        if (candidate_limit_stop) {
            return finish(StructuredReductionStopReason::BudgetLimit, true, false, accounting,
                          &staged_persistence_cache);
        }
        if (saw_persistence_limit) {
            return finish(StructuredReductionStopReason::PersistenceLimit, false, false, accounting,
                          &staged_persistence_cache);
        }
        return finish(StructuredReductionStopReason::BudgetLimit, false, false, accounting,
                      &staged_persistence_cache);
    }
}

#if defined(GNFS_STRUCTURED_REDUCTION_TEST_HOOKS)
namespace structured_reduction_testing {

StructuredReductionRunResult reduce_budgeted_parallel_with_hook(
    SequentialStructuredReducer& reducer, const StructuredReductionBudget& budget,
    const StructuredParallelReductionOptions& options, const Hook& hook, TreeBasisPlanner planner) {
    const ScopedStructuredReductionTestHook scope(hook);
    return reducer.reduce_budgeted_parallel(budget, options, planner);
}

StructuredBatchCommitResult commit_prepared_batch_with_hook(SequentialStructuredReducer& reducer,
                                                            StructuredPreparedBatch prepared,
                                                            const Hook& hook) {
    const ScopedStructuredReductionTestHook scope(hook);
    return reducer.commit(std::move(prepared));
}

} // namespace structured_reduction_testing
#endif

Relation SequentialStructuredReducer::materialize(StructuredRowId row) const {
    return impl_->corpus.materialize(impl_->row_at(row).sources);
}

std::vector<Relation> SequentialStructuredReducer::materialize_active() const {
    std::vector<Relation> result;
    result.reserve(impl_->active_rows);
    for (size_t i = 0; i < impl_->rows.size(); ++i) {
        if (!impl_->rows[i].active)
            continue;
        result.push_back(impl_->corpus.materialize(impl_->rows[i].sources));
    }
    return result;
}

size_t SequentialStructuredReducer::materialize_active_to(
    RelationSink& sink, const std::function<void(const Relation&)>& observer) const {
    if (sink.state() != RelationSinkState::Open) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "structured output sink is not open");
    }
    if (sink.logical_generation() != impl_->corpus.generation()) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "structured output sink belongs to a different generation");
    }
    if (sink.count() != 0) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "structured output sink must be empty before materialization");
    }

    size_t appended = 0;
    try {
        for (const auto& row : impl_->rows) {
            if (!row.active)
                continue;
            if (sink.count() != appended) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "relation sink count changed during materialization");
            }
            Relation relation = impl_->corpus.materialize(row.sources);
            if (observer) {
                observer(relation);
            }
            const size_t ordinal = sink.append(std::move(relation));
            if (ordinal != appended) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "relation sink returned a noncontiguous output ordinal");
            }
            ++appended;
        }
    } catch (...) {
        sink.abort();
        throw;
    }
    return appended;
}

const StructuredReductionStats& SequentialStructuredReducer::stats() const noexcept {
    return impl_->statistics;
}

StructuredIncidenceBuildStats SequentialStructuredReducer::incidence_build_stats() const noexcept {
    return impl_->incidence_build_statistics;
}

} // namespace gnfs::relation
