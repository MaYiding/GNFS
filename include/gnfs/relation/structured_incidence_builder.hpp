#pragma once

#include "gnfs/relation/structured_reduction.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace gnfs::relation {

/// Resource limits for deterministic incidence construction.
///
/// `max_rows_per_shard` bounds every temporary ordered-parallel work set. The
/// accumulating distinct-key index, final row supports, and bucket adjacency
/// still scale with the corpus and remain owned by the returned result. This
/// is a bounded-shard construction contract, not an OOC memory bound.
struct StructuredIncidenceBuildOptions final {
    size_t max_rows_per_shard = 4096;
    uint32_t worker_count = 1;

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
