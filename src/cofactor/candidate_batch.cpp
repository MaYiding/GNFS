#include "gnfs/cofactor/candidate_batch.hpp"

#include "gnfs/cofactor/candidate_chunk_plan.hpp"
#include "gnfs/util/ordered_parallel_map.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gnfs::cofactor {
namespace {

CandidateBatchResult verify_candidate_batch_impl(const core::PolynomialContext& ctx,
                                                 const factor_base::FactorBase& fb,
                                                 const CofactorizerConfig& config,
                                                 std::span<const sieve::SieveResult> sieve_results,
                                                 const CofactorSeedProvider* seed_provider,
                                                 const CandidateBatchOptions& options) {
    if (options.max_workers == 0) {
        throw std::invalid_argument("candidate batch requires at least one worker");
    }

    std::vector<size_t> candidate_counts;
    candidate_counts.reserve(sieve_results.size());
    for (const auto& sieve_result : sieve_results) {
        candidate_counts.push_back(sieve_result.candidates.size());
    }

    const CandidateChunkPlan plan =
        plan_candidate_chunks(candidate_counts, options.max_candidates_per_chunk);

    CandidateBatchResult result;
    result.relations_by_special_q.resize(sieve_results.size());
    result.total_candidates = plan.total_candidates;
    result.planned_chunks = plan.chunks.size();
    if (plan.chunks.empty()) {
        return result;
    }

    const size_t active_workers_wide = std::min<size_t>(options.max_workers, plan.chunks.size());
    if (active_workers_wide > std::numeric_limits<uint32_t>::max()) {
        throw std::overflow_error("candidate batch worker count exceeds uint32_t");
    }
    const uint32_t active_workers = static_cast<uint32_t>(active_workers_wide);
    result.workers_used = active_workers_wide;

    auto chunk_outputs = util::ordered_work_stealing_map<std::vector<core::Relation>>(
        plan.chunks.size(), active_workers,
        [&](size_t) { return std::make_unique<Cofactorizer>(ctx, fb, config); },
        [&](std::unique_ptr<Cofactorizer>& cofactorizer, size_t chunk_ordinal) {
            const CandidateChunk& chunk = plan.chunks[chunk_ordinal];
            const auto& sieve_result = sieve_results[chunk.special_q_index];
            std::vector<core::Relation> chunk_relations;
            chunk_relations.reserve((chunk.end - chunk.begin) / 4);
            for (size_t candidate_index = chunk.begin; candidate_index < chunk.end;
                 ++candidate_index) {
                std::optional<core::Relation> relation;
                if (seed_provider == nullptr) {
                    relation =
                        cofactorizer->verify(sieve_result.candidates[candidate_index],
                                             sieve_result.special_q.q, sieve_result.special_q.r);
                } else {
                    const CofactorAttemptCoordinates coordinates =
                        candidate_attempt_coordinates_v1(sieve_result, candidate_index);
                    relation = cofactorizer->verify(
                        sieve_result.candidates[candidate_index], sieve_result.special_q.q,
                        sieve_result.special_q.r, coordinates, *seed_provider);
                }
                if (relation) {
                    chunk_relations.push_back(std::move(*relation));
                }
            }
            return chunk_relations;
        });

    std::vector<size_t> relation_counts(sieve_results.size(), 0);
    for (size_t chunk_ordinal = 0; chunk_ordinal < plan.chunks.size(); ++chunk_ordinal) {
        const size_t special_q_index = plan.chunks[chunk_ordinal].special_q_index;
        const size_t chunk_relation_count = chunk_outputs[chunk_ordinal].size();
        if (chunk_relation_count >
            std::numeric_limits<size_t>::max() - relation_counts[special_q_index]) {
            throw std::overflow_error("candidate batch relation count exceeds size_t");
        }
        relation_counts[special_q_index] += chunk_relation_count;
    }
    for (size_t special_q_index = 0; special_q_index < sieve_results.size(); ++special_q_index) {
        result.relations_by_special_q[special_q_index].reserve(relation_counts[special_q_index]);
    }

    for (size_t chunk_ordinal = 0; chunk_ordinal < plan.chunks.size(); ++chunk_ordinal) {
        const size_t special_q_index = plan.chunks[chunk_ordinal].special_q_index;
        auto& source = chunk_outputs[chunk_ordinal];
        auto& destination = result.relations_by_special_q[special_q_index];
        destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                           std::make_move_iterator(source.end()));
    }

    return result;
}

} // namespace

CofactorAttemptCoordinates candidate_attempt_coordinates_v1(const sieve::SieveResult& sieve_result,
                                                            size_t candidate_index) {
    static_assert(std::numeric_limits<size_t>::digits <=
                  std::numeric_limits<std::uint64_t>::digits);
    if (candidate_index >= sieve_result.candidates.size()) {
        throw std::out_of_range("candidate attempt coordinate index is out of range");
    }
    return CofactorAttemptCoordinates{
        .special_q_index = static_cast<std::uint64_t>(sieve_result.special_q.index),
        .candidate_ordinal = static_cast<std::uint64_t>(candidate_index),
    };
}

CandidateBatchResult verify_candidate_batch(const core::PolynomialContext& ctx,
                                            const factor_base::FactorBase& fb,
                                            const CofactorizerConfig& config,
                                            std::span<const sieve::SieveResult> sieve_results,
                                            const CandidateBatchOptions& options) {
    return verify_candidate_batch_impl(ctx, fb, config, sieve_results, nullptr, options);
}

CandidateBatchResult verify_candidate_batch(const core::PolynomialContext& ctx,
                                            const factor_base::FactorBase& fb,
                                            const CofactorizerConfig& config,
                                            std::span<const sieve::SieveResult> sieve_results,
                                            const CofactorSeedProvider& seed_provider,
                                            const CandidateBatchOptions& options) {
    return verify_candidate_batch_impl(ctx, fb, config, sieve_results, &seed_provider, options);
}

} // namespace gnfs::cofactor
