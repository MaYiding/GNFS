#include "gnfs/relation/structured_incidence_builder.hpp"

#include "gnfs/util/ordered_parallel_map.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <utility>

namespace gnfs::relation {
namespace {

size_t checked_add(size_t lhs, size_t rhs, const char* message) {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        throw StructuredReductionError(StructuredReductionErrorCode::ResourceLimit, message);
    }
    return lhs + rhs;
}

struct Incidence final {
    LargePrimeKey key;
    StructuredRowId row;
};

[[nodiscard]] bool incidence_less(const Incidence& lhs, const Incidence& rhs) noexcept {
    if (lhs.key < rhs.key)
        return true;
    if (rhs.key < lhs.key)
        return false;
    return lhs.row < rhs.row;
}

struct WorkerIncidenceShard final {
    size_t local_begin = 0;
    std::vector<std::vector<LargePrimeKey>> row_lp_keys;
    std::vector<Incidence> incidences;
};

[[nodiscard]] WorkerIncidenceShard build_worker_shard(const SourceCorpus& corpus,
                                                      size_t shard_begin, size_t local_begin,
                                                      size_t row_count) {
    WorkerIncidenceShard result;
    result.local_begin = local_begin;
    result.row_lp_keys.reserve(row_count);
    for (size_t local_offset = 0; local_offset < row_count; ++local_offset) {
        const size_t ordinal = shard_begin + local_begin + local_offset;
        const SourceId source = corpus.source_id(ordinal);
        std::vector<LargePrimeKey> keys;
        if (const core::Relation* relation = corpus.try_borrow(source)) {
            keys = odd_large_prime_keys(*relation);
        } else {
            keys = odd_large_prime_keys(corpus.at(source));
        }
        const StructuredRowId row{static_cast<uint64_t>(ordinal)};
        for (const LargePrimeKey& key : keys)
            result.incidences.push_back(Incidence{key, row});
        result.row_lp_keys.push_back(std::move(keys));
    }
    std::sort(result.incidences.begin(), result.incidences.end(), incidence_less);
    return result;
}

struct MergeCursor final {
    size_t worker = 0;
    size_t incidence = 0;
};

struct MergeCursorGreater final {
    const std::vector<WorkerIncidenceShard>* shards = nullptr;

    [[nodiscard]] bool operator()(const MergeCursor& lhs, const MergeCursor& rhs) const noexcept {
        const Incidence& left = (*shards)[lhs.worker].incidences[lhs.incidence];
        const Incidence& right = (*shards)[rhs.worker].incidences[rhs.incidence];
        if (incidence_less(right, left))
            return true;
        if (incidence_less(left, right))
            return false;
        if (lhs.worker != rhs.worker)
            return lhs.worker > rhs.worker;
        return lhs.incidence > rhs.incidence;
    }
};

using BucketRows = std::map<LargePrimeKey, std::vector<StructuredRowId>>;

void append_sorted_worker_shards(const std::vector<WorkerIncidenceShard>& shards,
                                 BucketRows& bucket_rows) {
    std::priority_queue<MergeCursor, std::vector<MergeCursor>, MergeCursorGreater> queue(
        MergeCursorGreater{&shards});
    for (size_t worker = 0; worker < shards.size(); ++worker) {
        if (!shards[worker].incidences.empty())
            queue.push(MergeCursor{worker, 0});
    }

    while (!queue.empty()) {
        MergeCursor cursor = queue.top();
        queue.pop();
        const Incidence& incidence = shards[cursor.worker].incidences[cursor.incidence];
        bucket_rows[incidence.key].push_back(incidence.row);
        ++cursor.incidence;
        if (cursor.incidence < shards[cursor.worker].incidences.size())
            queue.push(cursor);
    }
}

} // namespace

StructuredIncidenceBuildResult
build_structured_incidence_shards(const SourceCorpus& corpus,
                                  const StructuredIncidenceBuildOptions& options) {
    if (options.max_rows_per_shard == 0 || options.worker_count == 0) {
        throw StructuredReductionError(
            StructuredReductionErrorCode::InvalidInput,
            "structured incidence construction requires nonzero shard rows and worker count");
    }

    StructuredIncidenceBuildResult result;
    result.row_lp_keys.resize(corpus.size());
    result.stats.requested_worker_count = options.worker_count;

    BucketRows bucket_rows;
    for (size_t shard_begin = 0; shard_begin < corpus.size();) {
        const size_t remaining = corpus.size() - shard_begin;
        const size_t shard_rows = std::min(options.max_rows_per_shard, remaining);
        const size_t worker_count = std::min<size_t>(options.worker_count, shard_rows);
        auto worker_shards = gnfs::util::ordered_parallel_map<WorkerIncidenceShard>(
            worker_count, options.worker_count, [&](size_t worker) {
                const size_t rows_per_worker = shard_rows / worker_count;
                const size_t extra_rows = shard_rows % worker_count;
                const size_t local_begin = worker * rows_per_worker + std::min(worker, extra_rows);
                const size_t row_count = rows_per_worker + (worker < extra_rows ? 1 : 0);
                return build_worker_shard(corpus, shard_begin, local_begin, row_count);
            });

        size_t shard_incidence_entries = 0;
        size_t next_local_row = 0;
        for (auto& worker_shard : worker_shards) {
            if (worker_shard.local_begin != next_local_row) {
                throw StructuredReductionError(
                    StructuredReductionErrorCode::InvariantViolation,
                    "structured incidence worker shards are not contiguous");
            }
            shard_incidence_entries =
                checked_add(shard_incidence_entries, worker_shard.incidences.size(),
                            "structured incidence shard entry count overflows");
            for (auto& keys : worker_shard.row_lp_keys) {
                result.row_lp_keys[shard_begin + next_local_row] = std::move(keys);
                ++next_local_row;
            }
        }
        if (next_local_row != shard_rows) {
            throw StructuredReductionError(
                StructuredReductionErrorCode::InvariantViolation,
                "structured incidence worker shards do not cover the row shard");
        }
        append_sorted_worker_shards(worker_shards, bucket_rows);

        result.stats.shard_count =
            checked_add(result.stats.shard_count, 1, "structured incidence shard count overflows");
        result.stats.peak_shard_rows = std::max(result.stats.peak_shard_rows, shard_rows);
        result.stats.peak_shard_incidence_entries =
            std::max(result.stats.peak_shard_incidence_entries, shard_incidence_entries);
        result.stats.total_incidence_entries =
            checked_add(result.stats.total_incidence_entries, shard_incidence_entries,
                        "structured incidence total entry count overflows");
        result.stats.peak_worker_count =
            std::max(result.stats.peak_worker_count, static_cast<uint32_t>(worker_count));
        shard_begin += shard_rows;
    }

    result.buckets.reserve(bucket_rows.size());
    for (auto& [key, adjacency] : bucket_rows) {
        result.buckets.push_back(StructuredIncidenceBucket{key, std::move(adjacency)});
    }
    return result;
}

} // namespace gnfs::relation
