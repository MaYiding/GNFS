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
    size_t row_count = 0;
    std::vector<std::vector<LargePrimeKey>> row_lp_keys;
    std::vector<Incidence> incidences;
};

[[nodiscard]] WorkerIncidenceShard build_worker_shard(const SourceCorpus& corpus,
                                                      size_t shard_begin, size_t local_begin,
                                                      size_t row_count) {
    WorkerIncidenceShard result;
    result.local_begin = local_begin;
    result.row_count = row_count;
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

[[nodiscard]] WorkerIncidenceShard
build_worker_shard_from_row_supports(const std::vector<std::vector<LargePrimeKey>>& row_lp_keys,
                                     size_t shard_begin, size_t local_begin, size_t row_count) {
    WorkerIncidenceShard result;
    result.local_begin = local_begin;
    result.row_count = row_count;
    for (size_t local_offset = 0; local_offset < row_count; ++local_offset) {
        const size_t ordinal = shard_begin + local_begin + local_offset;
        const StructuredRowId row{static_cast<uint64_t>(ordinal)};
        for (const LargePrimeKey& key : row_lp_keys[ordinal])
            result.incidences.push_back(Incidence{key, row});
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

void validate_build_request(uint64_t generation, size_t row_count,
                            const StructuredIncidenceBuildOptions& options) {
    if (generation == 0) {
        throw StructuredReductionError(StructuredReductionErrorCode::InvalidGeneration,
                                       "structured incidence generation must be nonzero");
    }
    if (options.max_rows_per_shard == 0 || options.worker_count == 0) {
        throw StructuredReductionError(
            StructuredReductionErrorCode::InvalidInput,
            "structured incidence construction requires nonzero shard rows and worker count");
    }
    if (!std::in_range<uint64_t>(row_count)) {
        throw StructuredReductionError(
            StructuredReductionErrorCode::ResourceLimit,
            "structured incidence row count exceeds the row ID representation");
    }
}

void validate_row_support_key(const LargePrimeKey& key) {
    if (key.prime < 2) {
        throw StructuredReductionError(
            StructuredReductionErrorCode::InvalidInput,
            "structured incidence row support contains a large-prime key with p < 2");
    }
    if (!key.is_algebraic && key.root != 0) {
        throw StructuredReductionError(
            StructuredReductionErrorCode::InvalidInput,
            "structured incidence row support contains a rational key with a nonzero root");
    }
    constexpr uint64_t projective_root = std::numeric_limits<uint32_t>::max();
    if (key.is_algebraic && key.root != projective_root && key.root >= key.prime) {
        throw StructuredReductionError(
            StructuredReductionErrorCode::InvalidInput,
            "structured incidence row support contains an invalid algebraic root");
    }
}

void validate_row_supports(const std::vector<std::vector<LargePrimeKey>>& row_lp_keys) {
    constexpr size_t max_lp_keys =
        static_cast<size_t>(core::Relation::MAX_SERIALIZED_LARGE_PRIMES) * 2;
    for (const auto& keys : row_lp_keys) {
        if (keys.size() > max_lp_keys) {
            throw StructuredReductionError(
                StructuredReductionErrorCode::PersistenceLimit,
                "structured incidence row support exceeds relation persistence limits");
        }
        for (size_t index = 0; index < keys.size(); ++index) {
            validate_row_support_key(keys[index]);
            if (index != 0 && !(keys[index - 1] < keys[index])) {
                throw StructuredReductionError(
                    StructuredReductionErrorCode::InvalidInput,
                    "structured incidence row support is not strictly canonical");
            }
        }
    }
}

struct IncidenceAssembly final {
    std::vector<StructuredIncidenceBucket> buckets;
    StructuredIncidenceBuildStats stats;
};

template <typename WorkerBuilder>
[[nodiscard]] IncidenceAssembly
build_incidence_shards(size_t row_count, const StructuredIncidenceBuildOptions& options,
                       std::vector<std::vector<LargePrimeKey>>* extracted_row_lp_keys,
                       WorkerBuilder&& build_worker) {
    IncidenceAssembly result;
    result.stats.requested_worker_count = options.worker_count;

    BucketRows bucket_rows;
    for (size_t shard_begin = 0; shard_begin < row_count;) {
        const size_t remaining = row_count - shard_begin;
        const size_t shard_rows = std::min(options.max_rows_per_shard, remaining);
        const size_t worker_count = std::min<size_t>(options.worker_count, shard_rows);
        auto worker_shards = gnfs::util::ordered_parallel_map<WorkerIncidenceShard>(
            worker_count, options.worker_count, [&](size_t worker) {
                const size_t rows_per_worker = shard_rows / worker_count;
                const size_t extra_rows = shard_rows % worker_count;
                const size_t local_begin = worker * rows_per_worker + std::min(worker, extra_rows);
                const size_t local_row_count = rows_per_worker + (worker < extra_rows ? 1 : 0);
                return build_worker(shard_begin, local_begin, local_row_count);
            });

        size_t shard_incidence_entries = 0;
        size_t next_local_row = 0;
        for (auto& worker_shard : worker_shards) {
            if (worker_shard.local_begin != next_local_row) {
                throw StructuredReductionError(
                    StructuredReductionErrorCode::InvariantViolation,
                    "structured incidence worker shards are not contiguous");
            }
            if (next_local_row > shard_rows ||
                worker_shard.row_count > shard_rows - next_local_row) {
                throw StructuredReductionError(
                    StructuredReductionErrorCode::InvariantViolation,
                    "structured incidence worker shard exceeds its row range");
            }
            shard_incidence_entries =
                checked_add(shard_incidence_entries, worker_shard.incidences.size(),
                            "structured incidence shard entry count overflows");
            if (extracted_row_lp_keys != nullptr) {
                if (worker_shard.row_lp_keys.size() != worker_shard.row_count) {
                    throw StructuredReductionError(
                        StructuredReductionErrorCode::InvariantViolation,
                        "structured incidence worker shard row support count is inconsistent");
                }
                for (auto& keys : worker_shard.row_lp_keys) {
                    (*extracted_row_lp_keys)[shard_begin + next_local_row] = std::move(keys);
                    ++next_local_row;
                }
            } else {
                if (!worker_shard.row_lp_keys.empty()) {
                    throw StructuredReductionError(
                        StructuredReductionErrorCode::InvariantViolation,
                        "prebuilt structured incidence worker unexpectedly returned row supports");
                }
                next_local_row += worker_shard.row_count;
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
    for (auto& [key, adjacency] : bucket_rows)
        result.buckets.push_back(StructuredIncidenceBucket{key, std::move(adjacency)});
    return result;
}

} // namespace

StructuredIncidenceBuildResult
build_structured_incidence_shards(const SourceCorpus& corpus,
                                  const StructuredIncidenceBuildOptions& options) {
    const uint64_t generation = corpus.generation();
    const size_t row_count = corpus.size();
    validate_build_request(generation, row_count, options);

    std::vector<std::vector<LargePrimeKey>> row_lp_keys(row_count);
    IncidenceAssembly incidence = build_incidence_shards(
        row_count, options, &row_lp_keys,
        [&](size_t shard_begin, size_t local_begin, size_t local_row_count) {
            return build_worker_shard(corpus, shard_begin, local_begin, local_row_count);
        });
    validate_row_supports(row_lp_keys);
    return StructuredIncidenceBuildResult(generation, std::move(row_lp_keys),
                                          std::move(incidence.buckets), incidence.stats);
}

StructuredIncidenceBuildResult
build_structured_incidence_from_row_supports(uint64_t generation,
                                             std::vector<std::vector<LargePrimeKey>> row_lp_keys,
                                             const StructuredIncidenceBuildOptions& options) {
    const size_t row_count = row_lp_keys.size();
    validate_build_request(generation, row_count, options);
    validate_row_supports(row_lp_keys);

    IncidenceAssembly incidence =
        build_incidence_shards(row_count, options, nullptr,
                               [&](size_t shard_begin, size_t local_begin, size_t local_row_count) {
                                   return build_worker_shard_from_row_supports(
                                       row_lp_keys, shard_begin, local_begin, local_row_count);
                               });
    return StructuredIncidenceBuildResult(generation, std::move(row_lp_keys),
                                          std::move(incidence.buckets), incidence.stats);
}

} // namespace gnfs::relation
