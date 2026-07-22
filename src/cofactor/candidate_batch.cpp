#include "gnfs/cofactor/candidate_batch.hpp"

#include "gnfs/cofactor/candidate_chunk_plan.hpp"
#include "gnfs/util/ordered_parallel_map.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gnfs::cofactor {

CandidateBatchResult verify_candidate_batch(const core::PolynomialContext& ctx,
                                            const factor_base::FactorBase& fb,
                                            const CofactorizerConfig& config,
                                            std::span<const sieve::SieveResult> sieve_results,
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

    std::vector<std::optional<std::vector<core::Relation>>> chunk_outputs(plan.chunks.size());
    std::vector<std::exception_ptr> chunk_errors(plan.chunks.size());
    std::atomic<size_t> next_chunk{0};

    const auto worker_summaries =
        util::ordered_parallel_map<size_t>(active_workers_wide, active_workers, [&](size_t) {
            Cofactorizer cofactorizer(ctx, fb, config);
            size_t chunks_processed = 0;

            while (true) {
                const size_t chunk_ordinal = next_chunk.fetch_add(1, std::memory_order_relaxed);
                if (chunk_ordinal >= plan.chunks.size()) {
                    break;
                }

                const CandidateChunk& chunk = plan.chunks[chunk_ordinal];
                try {
                    const auto& sieve_result = sieve_results[chunk.special_q_index];
                    std::vector<core::Relation> chunk_relations;
                    chunk_relations.reserve((chunk.end - chunk.begin) / 4);
                    for (size_t candidate_index = chunk.begin; candidate_index < chunk.end;
                         ++candidate_index) {
                        auto relation =
                            cofactorizer.verify(sieve_result.candidates[candidate_index],
                                                sieve_result.special_q.q, sieve_result.special_q.r);
                        if (relation) {
                            chunk_relations.push_back(std::move(*relation));
                        }
                    }
                    chunk_outputs[chunk_ordinal].emplace(std::move(chunk_relations));
                } catch (...) {
                    chunk_errors[chunk_ordinal] = std::current_exception();
                }
                ++chunks_processed;
            }

            return chunks_processed;
        });

    const size_t completed_chunks =
        std::accumulate(worker_summaries.begin(), worker_summaries.end(), size_t{0});
    if (completed_chunks != plan.chunks.size()) {
        throw std::logic_error("candidate batch did not claim every planned chunk");
    }

    // Every worker drains its claimed chunks before the lowest canonical chunk
    // failure is rethrown. Completion timing therefore cannot select the error.
    for (const auto& error : chunk_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    std::vector<size_t> relation_counts(sieve_results.size(), 0);
    for (size_t chunk_ordinal = 0; chunk_ordinal < plan.chunks.size(); ++chunk_ordinal) {
        if (!chunk_outputs[chunk_ordinal]) {
            throw std::logic_error("candidate batch completed without a chunk output");
        }
        const size_t special_q_index = plan.chunks[chunk_ordinal].special_q_index;
        const size_t chunk_relation_count = chunk_outputs[chunk_ordinal]->size();
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
        auto& source = *chunk_outputs[chunk_ordinal];
        auto& destination = result.relations_by_special_q[special_q_index];
        destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                           std::make_move_iterator(source.end()));
    }

    return result;
}

} // namespace gnfs::cofactor
