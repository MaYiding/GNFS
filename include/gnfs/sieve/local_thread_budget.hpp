#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace gnfs::sieve {

// A non-empty special-Q batch consumes the complete local sieve compute-lane
// budget. The lanes are divided as evenly as possible across the active outer
// workers, so no pair of workers differs by more than one lane.
struct LocalSieveThreadPlan {
    std::vector<size_t> threads_per_worker;
    size_t assigned_threads = 0;
    size_t peak_worker_threads = 0;
};

[[nodiscard]] inline LocalSieveThreadPlan
plan_local_sieve_threads(size_t thread_budget, size_t max_outer_workers, size_t batch_size) {
    if (thread_budget == 0) {
        throw std::invalid_argument("local sieve thread budget must be positive");
    }
    if (max_outer_workers == 0) {
        throw std::invalid_argument("local sieve outer worker limit must be positive");
    }

    LocalSieveThreadPlan plan;
    if (batch_size == 0) {
        return plan;
    }

    const size_t worker_count = std::min({thread_budget, max_outer_workers, batch_size});
    const size_t base_threads = thread_budget / worker_count;
    const size_t remainder_threads = thread_budget % worker_count;
    plan.threads_per_worker.reserve(worker_count);

    for (size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        const size_t worker_threads = base_threads + (worker_index < remainder_threads ? 1 : 0);
        plan.threads_per_worker.push_back(worker_threads);
        plan.assigned_threads += worker_threads;
        plan.peak_worker_threads = std::max(plan.peak_worker_threads, worker_threads);
    }

    return plan;
}

} // namespace gnfs::sieve
