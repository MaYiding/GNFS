#pragma once

#include "gnfs/relation/structured_reduction.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace gnfs::relation {

/// Logical metadata cardinality used by the optional incidence preflight.
///
/// These are element counts, not byte or RSS estimates. The total counts the
/// corpus row slots, row-support entries, final bucket slots, and bucket
/// adjacency entries that the vector-backed incidence construction must retain.
/// Callers can use the upper-bound helper when row supports have not yet been
/// read; a conservative bound treats every row as carrying the maximum
/// persistence-allowed LP support and every support entry as a distinct key.
struct StructuredIncidenceMetadataEstimate final {
    size_t row_slots = 0;
    size_t row_support_entries = 0;
    size_t bucket_slots = 0;
    size_t bucket_adjacency_entries = 0;
    size_t total_entries = 0;

    [[nodiscard]] bool
    operator==(const StructuredIncidenceMetadataEstimate&) const noexcept = default;
};

/// Compute a checked metadata estimate from known support cardinalities.
///
/// `unique_lp_keys` must not exceed `row_support_entries`. Arithmetic overflow
/// is reported as a typed `StructuredReductionError(ResourceLimit)` failure;
/// malformed cardinalities are reported as `StructuredReductionError(InvalidInput)`.
[[nodiscard]] StructuredIncidenceMetadataEstimate
estimate_structured_incidence_metadata(size_t row_count, size_t row_support_entries,
                                       size_t unique_lp_keys);

/// Compute a checked conservative estimate before source rows are read.
///
/// Every row is assumed to have at most `max_row_support_entries` effective LP
/// keys, and every such key is assumed distinct. This is intentionally a
/// logical upper bound rather than a prediction of the eventual bucket count.
[[nodiscard]] StructuredIncidenceMetadataEstimate
estimate_structured_incidence_metadata_upper_bound(size_t row_count,
                                                   size_t max_row_support_entries);

/// Optional resource limits for deterministic incidence construction.
///
/// `max_rows_per_shard` bounds every temporary ordered-parallel work set. The
/// accumulating distinct-key index, final row supports, and bucket adjacency
/// still scale with the corpus and remain owned by the returned result. This
/// is a bounded-shard construction contract, not an OOC memory bound. When
/// `max_metadata_entries` is set, the checked logical estimate must be no
/// greater than that value before worker or source-row construction begins.
struct StructuredIncidenceBuildOptions final {
    size_t max_rows_per_shard = 4096;
    uint32_t worker_count = 1;
    std::optional<size_t> max_metadata_entries = std::nullopt;

    [[nodiscard]] bool operator==(const StructuredIncidenceBuildOptions&) const noexcept = default;
};

struct StructuredIncidenceBuildStats final {
    size_t shard_count = 0;
    size_t peak_shard_rows = 0;
    size_t peak_shard_incidence_entries = 0;
    size_t total_incidence_entries = 0;
    /// Configured upper bound, including for an empty corpus.
    uint32_t requested_worker_count = 0;
    /// Maximum execution slots used by a nonempty shard. Serial work counts as one.
    uint32_t peak_worker_count = 0;

    [[nodiscard]] bool operator==(const StructuredIncidenceBuildStats&) const noexcept = default;
};

/// Reject an estimate that exceeds the configured optional cap.
///
/// A cap rejection or checked arithmetic overflow is a `StructuredReductionError`
/// with code `ResourceLimit`.
void preflight_structured_incidence_metadata(const StructuredIncidenceMetadataEstimate& estimate,
                                             const StructuredIncidenceBuildOptions& options);

struct StructuredIncidenceBucket final {
    LargePrimeKey key;
    std::vector<StructuredRowId> adjacency;

    [[nodiscard]] bool operator==(const StructuredIncidenceBucket&) const noexcept = default;
};

/// Generation-bound canonical LP incidence assembled through bounded row shards.
///
/// Row support preserves source-ordinal order. Buckets and each adjacency list
/// are strictly sorted, so shard width and worker count cannot affect the
/// `row_lp_keys` or `buckets` payload. Statistics intentionally record the
/// requested execution shape and therefore may differ between equivalent
/// payloads. The result is a move-only receipt: only approved builders can mint
/// it, and moving it invalidates the source receipt so it cannot be consumed
/// twice by a structured reducer. It binds logical generation and row count,
/// not a corpus digest; engine consumers must retain the authoritative source
/// proof for the same synchronous reduction scope.
class StructuredIncidenceBuildResult final {
public:
    StructuredIncidenceBuildResult(const StructuredIncidenceBuildResult&) = delete;
    StructuredIncidenceBuildResult& operator=(const StructuredIncidenceBuildResult&) = delete;

    StructuredIncidenceBuildResult(StructuredIncidenceBuildResult&& other) noexcept
        : generation_(std::exchange(other.generation_, uint64_t{0})),
          source_rows_(std::exchange(other.source_rows_, size_t{0})),
          row_lp_keys_(std::move(other.row_lp_keys_)), buckets_(std::move(other.buckets_)),
          stats_(std::exchange(other.stats_, StructuredIncidenceBuildStats{})) {}

    StructuredIncidenceBuildResult& operator=(StructuredIncidenceBuildResult&& other) noexcept {
        if (this != &other) {
            generation_ = std::exchange(other.generation_, uint64_t{0});
            source_rows_ = std::exchange(other.source_rows_, size_t{0});
            row_lp_keys_ = std::move(other.row_lp_keys_);
            buckets_ = std::move(other.buckets_);
            stats_ = std::exchange(other.stats_, StructuredIncidenceBuildStats{});
        }
        return *this;
    }

    ~StructuredIncidenceBuildResult() = default;

    [[nodiscard]] bool valid() const noexcept {
        return generation_ != 0 && source_rows_ == row_lp_keys_.size();
    }

    [[nodiscard]] uint64_t generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] size_t row_count() const noexcept {
        return source_rows_;
    }

    [[nodiscard]] const std::vector<std::vector<LargePrimeKey>>& row_lp_keys() const& noexcept {
        return row_lp_keys_;
    }
    [[nodiscard]] const std::vector<std::vector<LargePrimeKey>>& row_lp_keys() const&& = delete;

    [[nodiscard]] const std::vector<StructuredIncidenceBucket>& buckets() const& noexcept {
        return buckets_;
    }
    [[nodiscard]] const std::vector<StructuredIncidenceBucket>& buckets() const&& = delete;

    [[nodiscard]] const StructuredIncidenceBuildStats& stats() const& noexcept {
        return stats_;
    }
    [[nodiscard]] const StructuredIncidenceBuildStats& stats() const&& = delete;

    [[nodiscard]] bool operator==(const StructuredIncidenceBuildResult&) const noexcept = default;

private:
    StructuredIncidenceBuildResult(uint64_t generation,
                                   std::vector<std::vector<LargePrimeKey>> row_lp_keys,
                                   std::vector<StructuredIncidenceBucket> buckets,
                                   StructuredIncidenceBuildStats stats) noexcept
        : generation_(generation), source_rows_(row_lp_keys.size()),
          row_lp_keys_(std::move(row_lp_keys)), buckets_(std::move(buckets)), stats_(stats) {}

    uint64_t generation_ = 0;
    size_t source_rows_ = 0;
    std::vector<std::vector<LargePrimeKey>> row_lp_keys_;
    std::vector<StructuredIncidenceBucket> buckets_;
    StructuredIncidenceBuildStats stats_;

    friend StructuredIncidenceBuildResult
    build_structured_incidence_shards(const SourceCorpus& corpus,
                                      const StructuredIncidenceBuildOptions& options);
    friend StructuredIncidenceBuildResult build_structured_incidence_from_row_supports(
        uint64_t generation, std::vector<std::vector<LargePrimeKey>> row_lp_keys,
        const StructuredIncidenceBuildOptions& options);
    friend class SequentialStructuredReducer;
};

[[nodiscard]] StructuredIncidenceBuildResult
build_structured_incidence_shards(const SourceCorpus& corpus,
                                  const StructuredIncidenceBuildOptions& options);

/// Build canonical bucket adjacency from source-ordinal row supports that were
/// extracted during an earlier authoritative relation scan.
///
/// Every row support must already be a strictly sorted, duplicate-free list of
/// valid large-prime keys and must not exceed twice
/// `core::Relation::MAX_SERIALIZED_LARGE_PRIMES`. The function validates that
/// contract before launching worker shards, consumes the row-support vector,
/// and never reads a relation source. Its payload and execution statistics
/// match the source-backed builder for the same generation, supports, and
/// options.
[[nodiscard]] StructuredIncidenceBuildResult
build_structured_incidence_from_row_supports(uint64_t generation,
                                             std::vector<std::vector<LargePrimeKey>> row_lp_keys,
                                             const StructuredIncidenceBuildOptions& options);

} // namespace gnfs::relation
