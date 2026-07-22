#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace gnfs::cofactor {

struct CandidateChunk {
    size_t special_q_index = 0;
    size_t begin = 0;
    size_t end = 0;

    bool operator==(const CandidateChunk&) const = default;
};

struct CandidateChunkPlan {
    std::vector<CandidateChunk> chunks;
    size_t total_candidates = 0;
};

// Partition every special-Q candidate sequence into canonical lexicographic
// chunks. Chunks never cross a special-Q boundary, and empty sequences emit no
// chunk. The stable ordinals let parallel executors restore exact input order.
[[nodiscard]] inline CandidateChunkPlan
plan_candidate_chunks(std::span<const size_t> candidate_counts, size_t max_candidates_per_chunk) {
    if (max_candidates_per_chunk == 0) {
        throw std::invalid_argument("candidate chunk size must be positive");
    }

    CandidateChunkPlan plan;
    for (size_t special_q_index = 0; special_q_index < candidate_counts.size(); ++special_q_index) {
        const size_t candidate_count = candidate_counts[special_q_index];
        if (candidate_count > std::numeric_limits<size_t>::max() - plan.total_candidates) {
            throw std::overflow_error("candidate count total exceeds size_t");
        }
        plan.total_candidates += candidate_count;

        for (size_t begin = 0; begin < candidate_count;) {
            const size_t end = begin + std::min(max_candidates_per_chunk, candidate_count - begin);
            plan.chunks.push_back(CandidateChunk{special_q_index, begin, end});
            begin = end;
        }
    }
    return plan;
}

} // namespace gnfs::cofactor
