#pragma once

/// @file shadow_assembly.hpp
/// @brief Deterministic, parallel assembly of staged SIQS two-large-prime rows.

#include <gnfs/siqs/post_merge_row.hpp>
#include <gnfs/siqs/two_large_prime_adapter.hpp>
#include <gnfs/siqs/two_large_prime_graph.hpp>
#include <gnfs/siqs/two_large_prime_materializer.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using std::size_t;

enum class SIQSShadowRowOrigin : uint8_t {
    raw_full = 0,
    large_prime_cycle = 1,
};

struct SIQSShadowRow {
    SIQSShadowRowOrigin origin;
    SIQSPostMergeRow row;
};

struct SIQSShadowFingerprint {
    uint64_t low = 0;
    uint64_t high = 0;

    [[nodiscard]] friend constexpr bool operator==(const SIQSShadowFingerprint&,
                                                   const SIQSShadowFingerprint&) = default;
};

/// Stable ID layout for the accepted canonical source corpus.
///
/// Canonical source descriptors are bound into source_catalog's fingerprint
/// but are not retained here. Rows remain arithmetically self-contained. A
/// future runtime audit path may add an owning ID-to-descriptor catalog without
/// changing this assembly boundary.
struct SIQSShadowSourceCatalog {
    /// Canonical full-source ordinal to global source ID.
    std::vector<SIQSSourceId> full_source_ids;
    /// Adapter-local partial relation index to global source ID.
    std::vector<SIQSSourceId> partial_source_ids;
};

struct SIQSShadowAssemblyOptions {
    size_t trim_excess_rows = 100;
    uint32_t materialization_workers = 1;

    [[nodiscard]] friend constexpr bool operator==(const SIQSShadowAssemblyOptions&,
                                                   const SIQSShadowAssemblyOptions&) = default;
};

struct SIQSShadowAssemblyStats {
    size_t input_relations = 0;
    size_t encoded_full_relations = 0;
    size_t valid_full_relations = 0;
    size_t rejected_full_relations = 0;
    size_t full_sources = 0;
    size_t duplicate_full_sources = 0;
    TwoLargePrimeAdapterStats adapter;
    size_t partial_sources = 0;
    size_t graph_edges = 0;
    size_t graph_cycles = 0;
    size_t valid_cycle_rows = 0;
    size_t rejected_cycle_rows = 0;
    size_t rows_before_dedup = 0;
    size_t arithmetic_duplicates_removed = 0;
    size_t pretrim_rows = 0;
    size_t selected_rows = 0;
    size_t selected_full_rows = 0;
    size_t selected_cycle_rows = 0;
    size_t trimmed_rows = 0;

    [[nodiscard]] friend constexpr bool operator==(const SIQSShadowAssemblyStats&,
                                                   const SIQSShadowAssemblyStats&) = default;
};

struct SIQSShadowAssemblyFingerprints {
    SIQSShadowFingerprint source_catalog;
    SIQSShadowFingerprint pretrim_rows;
    SIQSShadowFingerprint selected_rows;

    [[nodiscard]] friend constexpr bool operator==(const SIQSShadowAssemblyFingerprints&,
                                                   const SIQSShadowAssemblyFingerprints&) = default;
};

struct SIQSShadowAssembly {
    SIQSShadowSourceCatalog sources;
    std::vector<SIQSShadowRow> rows;
    SIQSShadowAssemblyStats stats;
    SIQSShadowAssemblyFingerprints fingerprints;
};

enum class SIQSShadowAssemblyStatus : uint8_t {
    valid,
    invalid_modulus,
    invalid_factor_base,
    invalid_large_prime_bound,
    invalid_options,
    size_overflow,
    source_id_overflow,
    adapter_failure,
    graph_failure,
    worker_failure,
    internal_invariant_failure,
    resource_exhausted,
    exception_failure,
};

namespace shadow_assembly_detail {
struct SIQSShadowAssemblyResultFactory;
}

/// Invariant-safe result: an assembly is present exactly when status() is valid.
class SIQSShadowAssemblyResult {
public:
    SIQSShadowAssemblyResult(const SIQSShadowAssemblyResult&) = default;
    SIQSShadowAssemblyResult& operator=(const SIQSShadowAssemblyResult&) = default;

    SIQSShadowAssemblyResult(SIQSShadowAssemblyResult&& other) noexcept
        : status_(other.status_), assembly_(std::move(other.assembly_)) {
        other.status_ = SIQSShadowAssemblyStatus::internal_invariant_failure;
        other.assembly_.reset();
    }

    SIQSShadowAssemblyResult& operator=(SIQSShadowAssemblyResult&& other) noexcept {
        if (this != &other) {
            status_ = other.status_;
            assembly_ = std::move(other.assembly_);
            other.status_ = SIQSShadowAssemblyStatus::internal_invariant_failure;
            other.assembly_.reset();
        }
        return *this;
    }

    [[nodiscard]] SIQSShadowAssemblyStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<SIQSShadowAssembly>& assembly() const noexcept {
        return assembly_;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return status_ == SIQSShadowAssemblyStatus::valid && assembly_.has_value();
    }

private:
    friend struct shadow_assembly_detail::SIQSShadowAssemblyResultFactory;

    SIQSShadowAssemblyResult(SIQSShadowAssemblyStatus status,
                             std::optional<SIQSShadowAssembly> assembly)
        : status_(status), assembly_(std::move(assembly)) {}

    SIQSShadowAssemblyStatus status_;
    std::optional<SIQSShadowAssembly> assembly_;
};

inline constexpr uint32_t SIQS_SHADOW_FINGERPRINT_SCHEMA_VERSION = 1;
inline constexpr uint32_t SIQS_SHADOW_TRIM_POLICY_VERSION = 1;

namespace shadow_assembly_detail {

struct SIQSShadowAssemblyResultFactory {
    [[nodiscard]] static SIQSShadowAssemblyResult failure(SIQSShadowAssemblyStatus status) {
        if (status == SIQSShadowAssemblyStatus::valid) {
            status = SIQSShadowAssemblyStatus::internal_invariant_failure;
        }
        return SIQSShadowAssemblyResult(status, std::nullopt);
    }

    [[nodiscard]] static SIQSShadowAssemblyResult success(SIQSShadowAssembly assembly) {
        return SIQSShadowAssemblyResult(SIQSShadowAssemblyStatus::valid, std::move(assembly));
    }
};

[[nodiscard]] inline bool checked_add_size(size_t lhs, size_t rhs, size_t& result) noexcept {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] inline bool size_to_u64(size_t value, uint64_t& result) noexcept {
    if constexpr (std::numeric_limits<size_t>::max() > std::numeric_limits<uint64_t>::max()) {
        if (value > static_cast<size_t>(std::numeric_limits<uint64_t>::max())) {
            return false;
        }
    }
    result = static_cast<uint64_t>(value);
    return true;
}

[[nodiscard]] inline bool is_encoded_full(const SIQSRelation& relation) noexcept {
    return relation.large_prime == 0 && relation.large_prime2 == 0;
}

struct CanonicalFullSource {
    const SIQSRelation* relation;
    SIQSPostMergeRow row;
};

[[nodiscard]] inline bool full_source_less(const CanonicalFullSource& lhs,
                                           const CanonicalFullSource& rhs) {
    const int value_order = lhs.relation->value.compare(rhs.relation->value);
    if (value_order != 0) {
        return value_order < 0;
    }
    if (lhs.relation->negative != rhs.relation->negative) {
        return !lhs.relation->negative;
    }
    return std::lexicographical_compare(
        lhs.relation->exponents.begin(), lhs.relation->exponents.end(),
        rhs.relation->exponents.begin(), rhs.relation->exponents.end());
}

[[nodiscard]] inline bool full_source_equal(const CanonicalFullSource& lhs,
                                            const CanonicalFullSource& rhs) {
    return lhs.relation->value == rhs.relation->value &&
           lhs.relation->negative == rhs.relation->negative &&
           lhs.relation->exponents == rhs.relation->exponents;
}

[[nodiscard]] inline bool factor_power_less(const SIQSFactorPower& lhs,
                                            const SIQSFactorPower& rhs) noexcept {
    if (lhs.factor_base_index != rhs.factor_base_index) {
        return lhs.factor_base_index < rhs.factor_base_index;
    }
    return lhs.exponent < rhs.exponent;
}

[[nodiscard]] inline bool shadow_row_less(const SIQSShadowRow& lhs, const SIQSShadowRow& rhs) {
    const int x_order = lhs.row.x_modulus.compare(rhs.row.x_modulus);
    if (x_order != 0) {
        return x_order < 0;
    }
    if (lhs.row.q_negative != rhs.row.q_negative) {
        return !lhs.row.q_negative;
    }
    if (lhs.row.factor_powers != rhs.row.factor_powers) {
        return std::lexicographical_compare(
            lhs.row.factor_powers.begin(), lhs.row.factor_powers.end(),
            rhs.row.factor_powers.begin(), rhs.row.factor_powers.end(), factor_power_less);
    }
    if (lhs.row.large_prime_sqrt_factors != rhs.row.large_prime_sqrt_factors) {
        return std::lexicographical_compare(
            lhs.row.large_prime_sqrt_factors.begin(), lhs.row.large_prime_sqrt_factors.end(),
            rhs.row.large_prime_sqrt_factors.begin(), rhs.row.large_prime_sqrt_factors.end());
    }
    if (lhs.origin != rhs.origin) {
        return static_cast<uint8_t>(lhs.origin) < static_cast<uint8_t>(rhs.origin);
    }
    return std::lexicographical_compare(lhs.row.source_ids.begin(), lhs.row.source_ids.end(),
                                        rhs.row.source_ids.begin(), rhs.row.source_ids.end());
}

[[nodiscard]] inline bool same_arithmetic_row(const SIQSShadowRow& lhs, const SIQSShadowRow& rhs) {
    return lhs.row.x_modulus == rhs.row.x_modulus && lhs.row.q_negative == rhs.row.q_negative &&
           lhs.row.factor_powers == rhs.row.factor_powers &&
           lhs.row.large_prime_sqrt_factors == rhs.row.large_prime_sqrt_factors;
}

/// Byte-compatible with sieve_run_identity_detail::StableFingerprint. This
/// private copy avoids coupling SIQS to a sieve implementation-detail header.
/// Every domain starts with a u64-length-prefixed ASCII name and a u32 schema.
/// Sizes and ordinals use u64 little-endian, fixed-width integers use explicit
/// little-endian bytes, and Integer values use length-prefixed decimal ASCII.
class StableFingerprint {
public:
    void add_u8(uint8_t value) noexcept {
        low_ ^= value;
        low_ *= 1099511628211ULL;

        high_ ^= static_cast<uint64_t>(value) + byte_index_ * 0x9e3779b97f4a7c15ULL;
        high_ = std::rotl(high_, 27);
        high_ *= 0x94d049bb133111ebULL;
        high_ += 0x2545f4914f6cdd1dULL;
        ++byte_index_;
    }

    void add_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            add_u8(static_cast<uint8_t>((value >> shift) & 0xffU));
        }
    }

    void add_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            add_u8(static_cast<uint8_t>((value >> shift) & 0xffULL));
        }
    }

    [[nodiscard]] bool add_size(size_t value) noexcept {
        uint64_t encoded = 0;
        if (!size_to_u64(value, encoded)) {
            return false;
        }
        add_u64(encoded);
        return true;
    }

    [[nodiscard]] bool add_string(std::string_view value) noexcept {
        if (!add_size(value.size())) {
            return false;
        }
        for (const char byte : value) {
            add_u8(static_cast<uint8_t>(static_cast<unsigned char>(byte)));
        }
        return true;
    }

    [[nodiscard]] SIQSShadowFingerprint finish() const noexcept {
        uint64_t low = avalanche(low_ ^ byte_index_);
        uint64_t high = avalanche(high_ ^ std::rotl(byte_index_, 17));
        if (low == 0) {
            low = 0x6a09e667f3bcc909ULL;
        }
        if (high == 0) {
            high = 0xbb67ae8584caa73bULL;
        }
        return SIQSShadowFingerprint{low, high};
    }

private:
    [[nodiscard]] static uint64_t avalanche(uint64_t value) noexcept {
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31;
        return value;
    }

    uint64_t low_ = 14695981039346656037ULL;
    uint64_t high_ = 0x243f6a8885a308d3ULL;
    uint64_t byte_index_ = 0;
};

[[nodiscard]] inline bool add_integer(StableFingerprint& hash, const core::Integer& value) {
    const std::string encoded = value.to_string(10);
    return hash.add_string(encoded);
}

template <class Exponents>
[[nodiscard]] inline bool add_exponents(StableFingerprint& hash, const Exponents& exponents) {
    if (!hash.add_size(exponents.size())) {
        return false;
    }
    for (size_t i = 0; i < exponents.size(); ++i) {
        if (!hash.add_size(i)) {
            return false;
        }
        hash.add_u32(static_cast<uint32_t>(exponents[i]));
    }
    return true;
}

[[nodiscard]] inline bool add_post_merge_row(StableFingerprint& hash,
                                             const SIQSShadowRow& shadow_row) {
    hash.add_u8(static_cast<uint8_t>(shadow_row.origin));
    const SIQSPostMergeRow& row = shadow_row.row;
    if (!add_integer(hash, row.x_modulus)) {
        return false;
    }
    hash.add_u8(row.q_negative ? uint8_t{1} : uint8_t{0});

    if (!hash.add_size(row.factor_powers.size())) {
        return false;
    }
    for (size_t i = 0; i < row.factor_powers.size(); ++i) {
        if (!hash.add_size(i)) {
            return false;
        }
        hash.add_u32(row.factor_powers[i].factor_base_index);
        hash.add_u32(row.factor_powers[i].exponent);
    }

    if (!hash.add_size(row.large_prime_sqrt_factors.size())) {
        return false;
    }
    for (size_t i = 0; i < row.large_prime_sqrt_factors.size(); ++i) {
        if (!hash.add_size(i)) {
            return false;
        }
        hash.add_u64(row.large_prime_sqrt_factors[i]);
    }

    if (!hash.add_size(row.source_ids.size())) {
        return false;
    }
    for (size_t i = 0; i < row.source_ids.size(); ++i) {
        if (!hash.add_size(i)) {
            return false;
        }
        hash.add_u64(row.source_ids[i].value);
    }
    return true;
}

[[nodiscard]] inline std::optional<SIQSShadowFingerprint>
fingerprint_source_catalog(const std::vector<CanonicalFullSource>& full_sources,
                           const PreparedTwoLargePrimeCorpus& partial_corpus,
                           std::span<const uint32_t> factor_base_primes,
                           const core::Integer& modulus, uint64_t large_prime_bound) {
    StableFingerprint hash;
    if (!hash.add_string("GNFS-SIQS-SOURCE-CATALOG")) {
        return std::nullopt;
    }
    hash.add_u32(SIQS_SHADOW_FINGERPRINT_SCHEMA_VERSION);
    if (!add_integer(hash, modulus) || !hash.add_size(factor_base_primes.size())) {
        return std::nullopt;
    }
    for (size_t i = 0; i < factor_base_primes.size(); ++i) {
        if (!hash.add_size(i)) {
            return std::nullopt;
        }
        hash.add_u32(factor_base_primes[i]);
    }
    hash.add_u64(large_prime_bound);
    if (!hash.add_size(full_sources.size()) || !hash.add_size(partial_corpus.sources.size())) {
        return std::nullopt;
    }

    for (size_t i = 0; i < full_sources.size(); ++i) {
        uint64_t source_id = 0;
        if (!size_to_u64(i, source_id)) {
            return std::nullopt;
        }
        hash.add_u64(source_id);
        hash.add_u8(static_cast<uint8_t>(SIQSShadowRowOrigin::raw_full));
        hash.add_u64(0);
        hash.add_u64(0);
        if (!add_integer(hash, full_sources[i].relation->value)) {
            return std::nullopt;
        }
        hash.add_u8(full_sources[i].relation->negative ? uint8_t{1} : uint8_t{0});
        if (!add_exponents(hash, full_sources[i].relation->exponents)) {
            return std::nullopt;
        }
    }

    uint64_t full_count = 0;
    if (!size_to_u64(full_sources.size(), full_count)) {
        return std::nullopt;
    }
    for (size_t i = 0; i < partial_corpus.sources.size(); ++i) {
        uint64_t partial_index = 0;
        if (!size_to_u64(i, partial_index) ||
            partial_index > std::numeric_limits<uint64_t>::max() - full_count) {
            return std::nullopt;
        }
        const TwoLargePrimeCycleSource& source = partial_corpus.sources[i];
        hash.add_u64(full_count + partial_index);
        hash.add_u8(static_cast<uint8_t>(SIQSShadowRowOrigin::large_prime_cycle));
        hash.add_u64(source.p);
        hash.add_u64(source.q);
        if (!add_integer(hash, source.value)) {
            return std::nullopt;
        }
        hash.add_u8(source.negative ? uint8_t{1} : uint8_t{0});
        if (!add_exponents(hash, source.factor_base_exponents)) {
            return std::nullopt;
        }
    }
    return hash.finish();
}

[[nodiscard]] inline std::optional<SIQSShadowFingerprint>
fingerprint_rows(std::string_view domain, const SIQSShadowFingerprint& source_catalog,
                 std::span<const SIQSShadowRow> rows) {
    StableFingerprint hash;
    if (!hash.add_string(domain)) {
        return std::nullopt;
    }
    hash.add_u32(SIQS_SHADOW_FINGERPRINT_SCHEMA_VERSION);
    hash.add_u64(source_catalog.low);
    hash.add_u64(source_catalog.high);
    if (!hash.add_size(rows.size())) {
        return std::nullopt;
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!hash.add_size(i) || !add_post_merge_row(hash, rows[i])) {
            return std::nullopt;
        }
    }
    return hash.finish();
}

[[nodiscard]] inline std::optional<SIQSShadowFingerprint>
fingerprint_selected_rows(const SIQSShadowFingerprint& source_catalog,
                          const SIQSShadowFingerprint& pretrim, size_t factor_base_size,
                          size_t trim_excess_rows, std::span<const SIQSShadowRow> rows) {
    StableFingerprint hash;
    if (!hash.add_string("GNFS-SIQS-SELECTED-ROWS")) {
        return std::nullopt;
    }
    hash.add_u32(SIQS_SHADOW_FINGERPRINT_SCHEMA_VERSION);
    hash.add_u32(SIQS_SHADOW_TRIM_POLICY_VERSION);
    hash.add_u64(source_catalog.low);
    hash.add_u64(source_catalog.high);
    hash.add_u64(pretrim.low);
    hash.add_u64(pretrim.high);
    if (!hash.add_size(factor_base_size) || !hash.add_size(trim_excess_rows) ||
        !hash.add_size(rows.size())) {
        return std::nullopt;
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!hash.add_size(i) || !add_post_merge_row(hash, rows[i])) {
            return std::nullopt;
        }
    }
    return hash.finish();
}

struct CycleSlot {
    bool rejected = false;
    std::optional<SIQSShadowRow> row;
};

[[nodiscard]] inline bool stats_are_consistent(const SIQSShadowAssemblyStats& stats) noexcept {
    size_t sum = 0;
    if (stats.adapter.input_relations != stats.input_relations ||
        !checked_add_size(stats.adapter.full_relations, stats.adapter.accepted_one_lp, sum) ||
        !checked_add_size(sum, stats.adapter.accepted_two_lp, sum) ||
        !checked_add_size(sum, stats.adapter.rejected_relations, sum) ||
        sum != stats.adapter.input_relations) {
        return false;
    }
    if (!checked_add_size(stats.valid_full_relations, stats.rejected_full_relations, sum) ||
        sum != stats.encoded_full_relations) {
        return false;
    }
    if (!checked_add_size(stats.full_sources, stats.duplicate_full_sources, sum) ||
        sum != stats.valid_full_relations) {
        return false;
    }
    if (!checked_add_size(stats.adapter.accepted_one_lp, stats.adapter.accepted_two_lp, sum) ||
        sum != stats.partial_sources || stats.graph_edges != stats.partial_sources) {
        return false;
    }
    if (!checked_add_size(stats.valid_cycle_rows, stats.rejected_cycle_rows, sum) ||
        sum != stats.graph_cycles) {
        return false;
    }
    if (!checked_add_size(stats.full_sources, stats.valid_cycle_rows, sum) ||
        sum != stats.rows_before_dedup) {
        return false;
    }
    if (!checked_add_size(stats.pretrim_rows, stats.arithmetic_duplicates_removed, sum) ||
        sum != stats.rows_before_dedup) {
        return false;
    }
    if (!checked_add_size(stats.selected_rows, stats.trimmed_rows, sum) ||
        sum != stats.pretrim_rows) {
        return false;
    }
    return checked_add_size(stats.selected_full_rows, stats.selected_cycle_rows, sum) &&
           sum == stats.selected_rows;
}

} // namespace shadow_assembly_detail

/// Assemble a deterministic sparse-wide shadow corpus without changing SIQS.
///
/// The operation has no environment, logging, matrix, extraction, or factor()
/// side effects. Input order and materialization worker count do not influence
/// the canonical source IDs, selected rows, statistics, or fingerprints.
/// `splitter` must satisfy the adapter's deterministic pure-function contract.
/// Malformed or arithmetically invalid source rows contribute rejection stats;
/// invalid configuration, worker exceptions, and breached internal invariants
/// fail the whole result closed.
template <class Splitter>
[[nodiscard]] SIQSShadowAssemblyResult
assemble_siqs_shadow_rows(std::span<const SIQSRelation> raw_relations,
                          std::span<const uint32_t> factor_base_primes,
                          const core::Integer& modulus, uint64_t large_prime_bound,
                          const SIQSShadowAssemblyOptions& options, Splitter&& splitter) {
    using namespace shadow_assembly_detail;

    if (!post_merge_row_detail::has_valid_modulus(modulus)) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::invalid_modulus);
    }
    if (!post_merge_row_detail::has_valid_factor_base(factor_base_primes)) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::invalid_factor_base);
    }
    if (large_prime_bound < 2) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::invalid_large_prime_bound);
    }
    if (options.materialization_workers == 0) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::invalid_options);
    }
    size_t row_capacity = 0;
    if (!checked_add_size(factor_base_primes.size(), options.trim_excess_rows, row_capacity)) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::size_overflow);
    }
    uint64_t input_size = 0;
    if (!size_to_u64(raw_relations.size(), input_size)) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::source_id_overflow);
    }
    (void)input_size;

    SIQSShadowAssembly assembly;
    SIQSShadowAssemblyStats& stats = assembly.stats;
    stats.input_relations = raw_relations.size();

    std::vector<CanonicalFullSource> full_sources;
    for (const SIQSRelation& relation : raw_relations) {
        if (!is_encoded_full(relation)) {
            continue;
        }
        ++stats.encoded_full_relations;
        const auto result =
            make_full_post_merge_row(relation, SIQSSourceId{0}, factor_base_primes, modulus);
        if (!result.is_valid()) {
            if (result.status() != SIQSPostMergeRowStatus::invalid_source_relation &&
                result.status() != SIQSPostMergeRowStatus::row_identity_mismatch) {
                return SIQSShadowAssemblyResultFactory::failure(
                    SIQSShadowAssemblyStatus::internal_invariant_failure);
            }
            ++stats.rejected_full_relations;
            continue;
        }
        ++stats.valid_full_relations;
        full_sources.push_back(CanonicalFullSource{&relation, *result.row()});
    }

    std::sort(full_sources.begin(), full_sources.end(), full_source_less);
    const auto unique_full_end =
        std::unique(full_sources.begin(), full_sources.end(), full_source_equal);
    full_sources.erase(unique_full_end, full_sources.end());
    stats.full_sources = full_sources.size();
    stats.duplicate_full_sources = stats.valid_full_relations - stats.full_sources;

    std::optional<PreparedTwoLargePrimeCorpus> partial_corpus;
    try {
        partial_corpus =
            prepare_two_large_prime_corpus(raw_relations, factor_base_primes.size(),
                                           large_prime_bound, std::forward<Splitter>(splitter));
    } catch (const std::bad_alloc&) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::resource_exhausted);
    } catch (...) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::exception_failure);
    }
    if (!partial_corpus) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::adapter_failure);
    }
    stats.adapter = partial_corpus->stats;
    if (!checked_add_size(stats.adapter.accepted_one_lp, stats.adapter.accepted_two_lp,
                          stats.partial_sources)) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::size_overflow);
    }
    stats.graph_edges = partial_corpus->edges.size();
    if (stats.graph_edges != partial_corpus->sources.size() ||
        stats.partial_sources != stats.graph_edges) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::internal_invariant_failure);
    }
    for (size_t i = 0; i < partial_corpus->sources.size(); ++i) {
        const TwoLargePrimeCycleSource& source = partial_corpus->sources[i];
        const TwoLargePrimeEdge& edge = partial_corpus->edges[i];
        if (edge.relation_index != i || source.p != edge.p || source.q != edge.q) {
            return SIQSShadowAssemblyResultFactory::failure(
                SIQSShadowAssemblyStatus::internal_invariant_failure);
        }
    }

    uint64_t full_count = 0;
    uint64_t partial_count = 0;
    if (!size_to_u64(full_sources.size(), full_count) ||
        !size_to_u64(partial_corpus->sources.size(), partial_count) ||
        partial_count > std::numeric_limits<uint64_t>::max() - full_count) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::source_id_overflow);
    }

    assembly.sources.full_source_ids.reserve(full_sources.size());
    for (size_t i = 0; i < full_sources.size(); ++i) {
        uint64_t source_id = 0;
        if (!size_to_u64(i, source_id)) {
            return SIQSShadowAssemblyResultFactory::failure(
                SIQSShadowAssemblyStatus::source_id_overflow);
        }
        full_sources[i].row.source_ids.front().value = source_id;
        assembly.sources.full_source_ids.push_back(SIQSSourceId{source_id});
    }

    assembly.sources.partial_source_ids.reserve(partial_corpus->sources.size());
    for (size_t i = 0; i < partial_corpus->sources.size(); ++i) {
        uint64_t partial_index = 0;
        if (!size_to_u64(i, partial_index) ||
            partial_index > std::numeric_limits<uint64_t>::max() - full_count) {
            return SIQSShadowAssemblyResultFactory::failure(
                SIQSShadowAssemblyStatus::source_id_overflow);
        }
        assembly.sources.partial_source_ids.push_back(SIQSSourceId{full_count + partial_index});
    }

    const auto catalog_fingerprint = fingerprint_source_catalog(
        full_sources, *partial_corpus, factor_base_primes, modulus, large_prime_bound);
    if (!catalog_fingerprint) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::size_overflow);
    }
    assembly.fingerprints.source_catalog = *catalog_fingerprint;

    // Validate the adapter's contiguous relation identity exactly once, then
    // transfer ownership so worker lookups cannot observe mutation or dangling
    // source storage. The immutable move-only corpus is shared by every worker;
    // each graph cycle can now select its sources in O(L).
    const auto indexed_sources =
        IndexedTwoLargePrimeCycleSources::try_create(std::move(partial_corpus->sources));
    if (!indexed_sources) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::internal_invariant_failure);
    }

    const auto basis = build_two_large_prime_cycle_basis(partial_corpus->edges);
    if (!basis) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::graph_failure);
    }
    stats.graph_cycles = basis->cycles.size();

    std::vector<CycleSlot> cycle_slots(basis->cycles.size());
    std::atomic<bool> worker_resource_exhausted{false};
    std::atomic<bool> worker_exception{false};
    std::atomic<bool> internal_failure{false};
    const auto materialize_worker = [&](size_t begin_cycle, size_t end_cycle) noexcept {
        try {
            for (size_t cycle_ordinal = begin_cycle; cycle_ordinal < end_cycle; ++cycle_ordinal) {
                CycleSlot& slot = cycle_slots[cycle_ordinal];
                const auto& support = basis->cycles[cycle_ordinal];
                auto materialized =
                    materialize_two_large_prime_cycle(*indexed_sources, support, modulus);
                if (!materialized) {
                    // The current materializer uses nullopt for both checked
                    // arithmetic exhaustion and structural rejection. Upstream
                    // adapter/graph invariants exclude the structural cases;
                    // retain fail-closed row rejection until that API exposes
                    // a typed status before production integration.
                    slot.rejected = true;
                    continue;
                }

                std::vector<SIQSSourceId> mapped_source_ids;
                mapped_source_ids.reserve(materialized->relation_indices.size());
                bool invalid_index = false;
                for (const size_t local_id : materialized->relation_indices) {
                    if (local_id >= assembly.sources.partial_source_ids.size()) {
                        invalid_index = true;
                        break;
                    }
                    mapped_source_ids.push_back(assembly.sources.partial_source_ids[local_id]);
                }
                if (invalid_index) {
                    internal_failure.store(true, std::memory_order_relaxed);
                    return;
                }

                const auto converted = make_cycle_post_merge_row(
                    std::move(*materialized), mapped_source_ids, factor_base_primes, modulus);
                if (!converted.is_valid()) {
                    if (converted.status() == SIQSPostMergeRowStatus::row_identity_mismatch) {
                        slot.rejected = true;
                        continue;
                    }
                    internal_failure.store(true, std::memory_order_relaxed);
                    return;
                }
                slot.row = SIQSShadowRow{SIQSShadowRowOrigin::large_prime_cycle, *converted.row()};
            }
        } catch (const std::bad_alloc&) {
            worker_resource_exhausted.store(true, std::memory_order_relaxed);
        } catch (...) {
            worker_exception.store(true, std::memory_order_relaxed);
        }
    };

    if (!cycle_slots.empty()) {
        const size_t worker_count =
            std::min(cycle_slots.size(), static_cast<size_t>(options.materialization_workers));
        try {
            std::vector<std::jthread> workers;
            workers.reserve(worker_count);
            const size_t cycles_per_worker = cycle_slots.size() / worker_count;
            const size_t extra_cycles = cycle_slots.size() % worker_count;
            for (size_t i = 0; i < worker_count; ++i) {
                const size_t begin_cycle = i * cycles_per_worker + std::min(i, extra_cycles);
                const size_t cycle_count =
                    cycles_per_worker + (i < extra_cycles ? size_t{1} : size_t{0});
                workers.emplace_back(materialize_worker, begin_cycle, begin_cycle + cycle_count);
            }
        } catch (const std::bad_alloc&) {
            return SIQSShadowAssemblyResultFactory::failure(
                SIQSShadowAssemblyStatus::resource_exhausted);
        } catch (...) {
            return SIQSShadowAssemblyResultFactory::failure(
                SIQSShadowAssemblyStatus::exception_failure);
        }
    }
    if (worker_resource_exhausted.load(std::memory_order_relaxed)) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::resource_exhausted);
    }
    if (worker_exception.load(std::memory_order_relaxed)) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::exception_failure);
    }
    if (internal_failure.load(std::memory_order_relaxed)) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::internal_invariant_failure);
    }

    size_t maximum_rows = 0;
    if (!checked_add_size(full_sources.size(), cycle_slots.size(), maximum_rows)) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::size_overflow);
    }
    std::vector<SIQSShadowRow> rows;
    rows.reserve(maximum_rows);
    for (CanonicalFullSource& source : full_sources) {
        rows.push_back(SIQSShadowRow{SIQSShadowRowOrigin::raw_full, std::move(source.row)});
    }
    for (CycleSlot& slot : cycle_slots) {
        if (slot.row) {
            ++stats.valid_cycle_rows;
            rows.push_back(std::move(*slot.row));
        } else {
            ++stats.rejected_cycle_rows;
        }
    }
    stats.rows_before_dedup = rows.size();

    std::sort(rows.begin(), rows.end(), shadow_row_less);
    const auto unique_row_end = std::unique(rows.begin(), rows.end(), same_arithmetic_row);
    rows.erase(unique_row_end, rows.end());
    stats.pretrim_rows = rows.size();
    stats.arithmetic_duplicates_removed = stats.rows_before_dedup - stats.pretrim_rows;

    const auto pretrim_fingerprint =
        fingerprint_rows("GNFS-SIQS-PRETRIM-ROWS", assembly.fingerprints.source_catalog,
                         std::span<const SIQSShadowRow>(rows.data(), rows.size()));
    if (!pretrim_fingerprint) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::size_overflow);
    }
    assembly.fingerprints.pretrim_rows = *pretrim_fingerprint;

    std::vector<uint8_t> selected_mask(rows.size(), uint8_t{0});
    size_t selected_count = 0;
    size_t selected_full = 0;
    for (size_t i = 0; i < rows.size() && selected_full < factor_base_primes.size() &&
                       selected_count < row_capacity;
         ++i) {
        if (rows[i].origin == SIQSShadowRowOrigin::raw_full) {
            selected_mask[i] = 1;
            ++selected_full;
            ++selected_count;
        }
    }
    for (size_t i = 0; i < rows.size() && selected_count < row_capacity; ++i) {
        if (rows[i].origin == SIQSShadowRowOrigin::large_prime_cycle) {
            selected_mask[i] = 1;
            ++selected_count;
        }
    }
    for (size_t i = 0; i < rows.size() && selected_count < row_capacity; ++i) {
        if (rows[i].origin == SIQSShadowRowOrigin::raw_full && selected_mask[i] == 0) {
            selected_mask[i] = 1;
            ++selected_full;
            ++selected_count;
        }
    }

    assembly.rows.reserve(selected_count);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (selected_mask[i] != 0) {
            assembly.rows.push_back(std::move(rows[i]));
        }
    }
    std::sort(assembly.rows.begin(), assembly.rows.end(), shadow_row_less);
    stats.selected_rows = assembly.rows.size();
    stats.selected_full_rows = selected_full;
    stats.selected_cycle_rows = stats.selected_rows - stats.selected_full_rows;
    stats.trimmed_rows = stats.pretrim_rows - stats.selected_rows;

    const auto selected_fingerprint = fingerprint_selected_rows(
        assembly.fingerprints.source_catalog, assembly.fingerprints.pretrim_rows,
        factor_base_primes.size(), options.trim_excess_rows,
        std::span<const SIQSShadowRow>(assembly.rows.data(), assembly.rows.size()));
    if (!selected_fingerprint) {
        return SIQSShadowAssemblyResultFactory::failure(SIQSShadowAssemblyStatus::size_overflow);
    }
    assembly.fingerprints.selected_rows = *selected_fingerprint;

    if (!stats_are_consistent(stats)) {
        return SIQSShadowAssemblyResultFactory::failure(
            SIQSShadowAssemblyStatus::internal_invariant_failure);
    }
    return SIQSShadowAssemblyResultFactory::success(std::move(assembly));
}

} // namespace gnfs::siqs
