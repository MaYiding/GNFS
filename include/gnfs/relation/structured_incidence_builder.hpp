#pragma once

#include "gnfs/relation/structured_reduction.hpp"

#include <cstddef>
#include <cstdint>
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

/// Canonical LP incidence assembled through bounded row shards.
///
/// Row support preserves source-ordinal order. Buckets and each adjacency list
/// are strictly sorted, so shard width and worker count cannot affect the
/// `row_lp_keys` or `buckets` payload. Statistics intentionally record the
/// requested execution shape and therefore may differ between equivalent
/// payloads.
struct StructuredIncidenceBuildResult final {
    std::vector<std::vector<LargePrimeKey>> row_lp_keys;
    std::vector<StructuredIncidenceBucket> buckets;
    StructuredIncidenceBuildStats stats;

    [[nodiscard]] bool operator==(const StructuredIncidenceBuildResult&) const noexcept = default;
};

[[nodiscard]] StructuredIncidenceBuildResult
build_structured_incidence_shards(const SourceCorpus& corpus,
                                  const StructuredIncidenceBuildOptions& options);

} // namespace gnfs::relation
