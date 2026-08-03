#include <gnfs/cofactor/candidate_chunk_plan.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using gnfs::cofactor::CandidateChunk;
using gnfs::cofactor::CandidateChunkPlan;
using gnfs::cofactor::plan_candidate_chunks;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void test_exact_plan() {
    const std::vector<size_t> counts{0, 5, 1, 7};
    const CandidateChunkPlan plan = plan_candidate_chunks(counts, 3);
    const std::vector<CandidateChunk> expected{{1, 0, 3}, {1, 3, 5}, {2, 0, 1},
                                               {3, 0, 3}, {3, 3, 6}, {3, 6, 7}};

    require(plan.chunks == expected, "candidate chunks differ from canonical exact plan");
    require(plan.total_candidates == 13, "exact plan candidate total is wrong");
}

void test_empty_and_invalid_plans() {
    const CandidateChunkPlan empty = plan_candidate_chunks({}, 4);
    require(empty.chunks.empty() && empty.total_candidates == 0,
            "empty input must produce an empty plan");

    const std::vector<size_t> zero_counts{0, 0, 0};
    const CandidateChunkPlan all_zero = plan_candidate_chunks(zero_counts, 4);
    require(all_zero.chunks.empty() && all_zero.total_candidates == 0,
            "all-zero counts must produce an empty plan");

    bool zero_chunk_rejected = false;
    try {
        (void)plan_candidate_chunks(zero_counts, 0);
    } catch (const std::invalid_argument&) {
        zero_chunk_rejected = true;
    }
    require(zero_chunk_rejected, "zero candidate chunk size must be rejected");

    const std::vector<size_t> overflowing_counts{std::numeric_limits<size_t>::max(), 1};
    bool overflow_rejected = false;
    try {
        (void)plan_candidate_chunks(overflowing_counts, std::numeric_limits<size_t>::max());
    } catch (const std::overflow_error&) {
        overflow_rejected = true;
    }
    require(overflow_rejected, "overflowing candidate total must be rejected");
}

void verify_plan_properties(const std::vector<size_t>& counts, size_t grain) {
    const CandidateChunkPlan plan = plan_candidate_chunks(counts, grain);
    const size_t expected_total = std::accumulate(counts.begin(), counts.end(), size_t{0});
    require(plan.total_candidates == expected_total, "candidate total does not match input");

    std::vector<std::vector<unsigned char>> coverage;
    coverage.reserve(counts.size());
    for (const size_t count : counts) {
        coverage.emplace_back(count, 0);
    }

    size_t previous_special_q = 0;
    size_t previous_end = 0;
    bool first_chunk = true;
    size_t covered_total = 0;
    for (const CandidateChunk& chunk : plan.chunks) {
        require(chunk.special_q_index < counts.size(), "chunk special-Q index is out of range");
        require(chunk.begin < chunk.end, "chunk must be non-empty");
        require(chunk.end <= counts[chunk.special_q_index], "chunk end exceeds candidate count");
        require(chunk.end - chunk.begin <= grain, "chunk exceeds configured grain");

        if (!first_chunk) {
            require(chunk.special_q_index >= previous_special_q,
                    "chunks are not ordered by special-Q index");
            if (chunk.special_q_index == previous_special_q) {
                require(chunk.begin == previous_end,
                        "chunks within one special-Q are not contiguous");
            } else {
                require(chunk.begin == 0, "a new special-Q must start at candidate zero");
            }
        } else {
            require(chunk.begin == 0, "the first non-empty special-Q must start at zero");
            first_chunk = false;
        }

        for (size_t candidate = chunk.begin; candidate < chunk.end; ++candidate) {
            require(coverage[chunk.special_q_index][candidate] == 0, "candidate coverage overlaps");
            coverage[chunk.special_q_index][candidate] = 1;
            ++covered_total;
        }
        previous_special_q = chunk.special_q_index;
        previous_end = chunk.end;
    }

    require(covered_total == expected_total, "candidate coverage is incomplete");
    for (const auto& per_special_q : coverage) {
        require(std::all_of(per_special_q.begin(), per_special_q.end(),
                            [](unsigned char count) { return count == 1; }),
                "a candidate was not covered exactly once");
    }
}

void test_property_grid() {
    for (size_t grain = 1; grain <= 8; ++grain) {
        for (size_t count = 0; count <= 17; ++count) {
            verify_plan_properties({0, count, (count * 7 + 3) % 18, count / 2, 0}, grain);
        }
    }
}

} // namespace

int main() {
    try {
        test_exact_plan();
        test_empty_and_invalid_plans();
        test_property_grid();
        std::cout << "All candidate chunk plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Candidate chunk plan test failed: " << error.what() << '\n';
        return 1;
    }
}
