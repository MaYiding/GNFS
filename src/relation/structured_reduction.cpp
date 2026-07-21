#include "gnfs/relation/structured_reduction.hpp"

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

[[noreturn]] void fail(StructuredReductionErrorCode code, const char* message) {
    throw StructuredReductionError(code, message);
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

SourceCorpus::SourceCorpus(uint64_t generation, std::vector<Relation> relations)
    : generation_(generation), relations_(std::move(relations)) {
    if (generation_ == 0) {
        fail(StructuredReductionErrorCode::InvalidGeneration,
             "source corpus generation must be nonzero");
    }

    for (const auto& relation : relations_) {
        validate_input_relation(relation);
    }
}

uint64_t SourceCorpus::generation() const noexcept {
    return generation_;
}

size_t SourceCorpus::size() const noexcept {
    return relations_.size();
}

SourceId SourceCorpus::source_id(size_t ordinal) const {
    if (ordinal >= relations_.size()) {
        fail(StructuredReductionErrorCode::InvalidInput, "source ordinal is out of range");
    }
    return SourceId{generation_, static_cast<uint64_t>(ordinal)};
}

const Relation& SourceCorpus::at(SourceId source) const {
    if (source.generation != generation_) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "source ID belongs to a different corpus generation");
    }
    if (source.ordinal >= relations_.size()) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "source ID ordinal is out of range");
    }
    return relations_[static_cast<size_t>(source.ordinal)];
}

Relation SourceCorpus::materialize(const SourceCombination& combination) const {
    validate_source_combination(combination, false);
    if (combination.generation() != generation_) {
        fail(StructuredReductionErrorCode::InvalidSourceCombination,
             "source combination belongs to a different corpus generation");
    }

    size_t rational_factor_count = 0;
    size_t algebraic_factor_count = 0;
    size_t persisted_pair_count = 0;
    size_t raw_lp_count = 0;
    for (const auto source : combination.sources()) {
        const auto& relation = at(source);
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
    for (const auto source : combination.sources()) {
        const auto& relation = at(source);
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
    for (const auto source : combination.sources()) {
        const auto& relation = at(source);
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

struct SequentialStructuredReducer::Impl final {
    struct Row final {
        SourceCombination sources;
        std::vector<LargePrimeKey> lp_keys;
        std::vector<size_t> bucket_ids;
        bool active = true;
    };

    struct Bucket final {
        LargePrimeKey key;
        std::vector<StructuredRowId> adjacency;
        size_t active_degree = 0;
    };

    struct Incidence final {
        LargePrimeKey key;
        StructuredRowId row;
    };

    struct PreparedData final {
        TwoWayMergePlan plan;
        Relation materialized;
    };

    explicit Impl(SourceCorpus source_corpus) : corpus(std::move(source_corpus)) {
        rows.reserve(corpus.size());
        size_t incidence_count = 0;
        for (size_t ordinal = 0; ordinal < corpus.size(); ++ordinal) {
            const SourceId source = corpus.source_id(ordinal);
            auto keys = odd_large_prime_keys(corpus.at(source));
            validate_canonical_lp_keys(keys);
            if (keys.size() > std::numeric_limits<size_t>::max() - incidence_count) {
                fail(StructuredReductionErrorCode::InvalidInput,
                     "source incidence count is too large");
            }
            incidence_count += keys.size();
            rows.push_back(Row{SourceCombination::singleton(source), std::move(keys), {}, true});
        }

        std::vector<Incidence> incidences;
        incidences.reserve(incidence_count);
        for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
            for (const auto& key : rows[row_index].lp_keys) {
                incidences.push_back(Incidence{key, StructuredRowId{row_index}});
            }
        }
        std::sort(incidences.begin(), incidences.end(),
                  [](const Incidence& lhs, const Incidence& rhs) {
                      if (lhs.key < rhs.key)
                          return true;
                      if (rhs.key < lhs.key)
                          return false;
                      return lhs.row < rhs.row;
                  });

        for (size_t begin = 0; begin < incidences.size();) {
            size_t end = begin + 1;
            while (end < incidences.size() && incidences[end].key == incidences[begin].key) {
                ++end;
            }
            const size_t bucket_id = buckets.size();
            Bucket bucket;
            bucket.key = incidences[begin].key;
            bucket.adjacency.reserve(end - begin);
            bucket.active_degree = end - begin;
            for (size_t i = begin; i < end; ++i) {
                bucket.adjacency.push_back(incidences[i].row);
                rows[static_cast<size_t>(incidences[i].row.value)].bucket_ids.push_back(bucket_id);
            }
            buckets.push_back(std::move(bucket));
            begin = end;
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
        const auto it = std::lower_bound(buckets.begin(), buckets.end(), key,
                                         [](const Bucket& bucket, const LargePrimeKey& candidate) {
                                             return bucket.key < candidate;
                                         });
        if (it == buckets.end() || !(it->key == key)) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "logical row references an unknown LP bucket");
        }
        return static_cast<size_t>(it - buckets.begin());
    }

    void validate_state() const {
        size_t counted_active = 0;
        for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
            const auto& row = rows[row_index];
            validate_source_combination(row.sources, false);
            if (row.sources.generation() != corpus.generation()) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "logical row belongs to a different generation");
            }
            for (const auto source : row.sources.sources()) {
                (void)corpus.at(source);
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
            }
        }
        if (counted_active != active_rows) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "active row count is inconsistent");
        }

        // Deterministic sparse GF(2) elimination over immutable source IDs.
        // Active source supports may overlap, but their transform rows must be
        // independent. RowId order and the greatest ordinal pivot make the
        // reference audit reproducible.
        constexpr size_t no_basis_row = std::numeric_limits<size_t>::max();
        std::vector<size_t> pivot_to_basis(corpus.size(), no_basis_row);
        std::vector<SourceCombination> source_basis;
        source_basis.reserve(counted_active);
        for (const auto& row : rows) {
            if (!row.active)
                continue;

            SourceCombination candidate = row.sources;
            bool inserted_basis_row = false;
            while (!candidate.empty()) {
                const SourceId pivot = candidate.sources().back();
                if (pivot.ordinal >= pivot_to_basis.size()) {
                    fail(StructuredReductionErrorCode::InvariantViolation,
                         "active source transform has an invalid pivot");
                }
                const size_t basis_index = pivot_to_basis[static_cast<size_t>(pivot.ordinal)];
                if (basis_index == no_basis_row) {
                    pivot_to_basis[static_cast<size_t>(pivot.ordinal)] = source_basis.size();
                    source_basis.push_back(std::move(candidate));
                    inserted_basis_row = true;
                    break;
                }
                candidate =
                    SourceCombination::symmetric_difference(candidate, source_basis[basis_index]);
            }
            if (!inserted_basis_row) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "active source transforms are linearly dependent");
            }
        }
        if (source_basis.size() != counted_active) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "active source transform rank is inconsistent");
        }

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
            if (degree != bucket.active_degree) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "LP bucket degree is inconsistent");
            }
        }
    }

    [[nodiscard]] std::array<StructuredRowId, 2> active_pair(const Bucket& bucket) const {
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

    [[nodiscard]] TwoWayMergePlan validate_plan(const TwoWayMergePlan& plan) const {
        validate_state();
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
        const auto& witness_bucket = buckets[find_bucket(plan.witness)];
        if (witness_bucket.active_degree != 2 || active_pair(witness_bucket) != plan.members) {
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

    [[nodiscard]] PreparedData prepare(const TwoWayMergePlan& plan) const {
        TwoWayMergePlan validated = validate_plan(plan);
        Relation materialized = corpus.materialize(validated.expected_sources);
        if (odd_large_prime_keys(materialized) != validated.expected_lp_keys) {
            fail(StructuredReductionErrorCode::InvariantViolation,
                 "materialized LP support differs from logical LP support");
        }
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

        std::vector<size_t> output_bucket_ids;
        output_bucket_ids.reserve(validated.expected_lp_keys.size());
        for (const auto& key : validated.expected_lp_keys) {
            output_bucket_ids.push_back(find_bucket(key));
        }

        if (rows.size() >= rows.max_size() || rows.size() == std::numeric_limits<uint64_t>::max()) {
            fail(StructuredReductionErrorCode::PersistenceLimit,
                 "structured row ID space is exhausted");
        }
        const StructuredRowId output_id{static_cast<uint64_t>(rows.size())};

        // Every potentially throwing allocation precedes the first logical
        // state mutation. LP XOR cannot introduce a new bucket.
        rows.reserve(rows.size() + 1);
        for (const size_t bucket_id : output_bucket_ids) {
            auto& adjacency = buckets[bucket_id].adjacency;
            if (adjacency.size() >= adjacency.max_size()) {
                fail(StructuredReductionErrorCode::PersistenceLimit,
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
                --buckets[bucket_id].active_degree;
            }
        }
        for (const size_t bucket_id : rows.back().bucket_ids) {
            buckets[bucket_id].adjacency.push_back(output_id);
            ++buckets[bucket_id].active_degree;
        }

        --active_rows;
        ++statistics.two_way_merges;
        statistics.output_rows = active_rows;
        ++incidence_epoch;
        return output_id;
    }

    SourceCorpus corpus;
    std::vector<Row> rows;
    std::vector<Bucket> buckets;
    size_t active_rows = 0;
    uint64_t incidence_epoch = 1;
    StructuredReductionStats statistics;
    std::set<std::pair<uint64_t, uint64_t>> persistence_limited_pairs;
};

SequentialStructuredReducer::SequentialStructuredReducer(SourceCorpus corpus)
    : impl_(std::make_unique<Impl>(std::move(corpus))) {}

SequentialStructuredReducer::SequentialStructuredReducer(uint64_t generation,
                                                         std::vector<Relation> relations)
    : SequentialStructuredReducer(SourceCorpus(generation, std::move(relations))) {}

SequentialStructuredReducer::~SequentialStructuredReducer() = default;

SequentialStructuredReducer::SequentialStructuredReducer(SequentialStructuredReducer&&) noexcept =
    default;

SequentialStructuredReducer&
SequentialStructuredReducer::operator=(SequentialStructuredReducer&&) noexcept = default;

const SourceCorpus& SequentialStructuredReducer::corpus() const noexcept {
    return impl_->corpus;
}

size_t SequentialStructuredReducer::total_row_count() const noexcept {
    return impl_->rows.size();
}

size_t SequentialStructuredReducer::active_row_count() const noexcept {
    return impl_->active_rows;
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
        if (impl_->buckets[bucket_id].active_degree == 1) {
            pending.push(bucket_id);
        }
    }
    if (!pending.empty() && impl_->incidence_epoch == std::numeric_limits<uint64_t>::max()) {
        fail(StructuredReductionErrorCode::InvariantViolation,
             "incidence epoch would overflow during singleton peeling");
    }

    size_t removed = 0;
    while (!pending.empty()) {
        const size_t bucket_id = pending.top();
        pending.pop();
        auto& bucket = impl_->buckets[bucket_id];
        if (bucket.active_degree != 1)
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
            auto& degree = impl_->buckets[affected_bucket].active_degree;
            if (degree == 0) {
                fail(StructuredReductionErrorCode::InvariantViolation,
                     "LP bucket degree underflow during singleton peeling");
            }
            --degree;
            if (degree == 1)
                pending.push(affected_bucket);
        }
    }

    impl_->statistics.singleton_rows_removed += removed;
    impl_->statistics.output_rows = impl_->active_rows;
    if (removed != 0)
        ++impl_->incidence_epoch;
    return removed;
}

std::vector<TwoWayMergePlan> SequentialStructuredReducer::plan_two_way_merges() const {
    impl_->validate_state();
    std::vector<TwoWayMergePlan> plans;
    for (const auto& bucket : impl_->buckets) {
        if (bucket.active_degree != 2)
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
            const auto pair = std::pair{plan.members[0].value, plan.members[1].value};
            if (impl_->persistence_limited_pairs.contains(pair))
                continue;

            try {
                auto prepared = prepare(plan);
                (void)commit(std::move(prepared));
                committed = true;
                break;
            } catch (const StructuredReductionError& error) {
                if (error.code() != StructuredReductionErrorCode::PersistenceLimit) {
                    throw;
                }
                if (impl_->persistence_limited_pairs.insert(pair).second) {
                    ++impl_->statistics.persistence_limited_plans;
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

const StructuredReductionStats& SequentialStructuredReducer::stats() const noexcept {
    return impl_->statistics;
}

} // namespace gnfs::relation
