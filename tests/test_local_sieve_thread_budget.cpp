#include <gnfs/sieve/local_thread_budget.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using gnfs::sieve::LocalSieveThreadPlan;
using gnfs::sieve::plan_local_sieve_threads;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void require_plan(size_t budget, size_t outer_limit, size_t batch_size,
                  const std::vector<size_t>& expected) {
    const LocalSieveThreadPlan plan = plan_local_sieve_threads(budget, outer_limit, batch_size);
    require(plan.threads_per_worker == expected, "unexpected per-worker thread allocation");

    const size_t expected_sum = std::accumulate(expected.begin(), expected.end(), size_t{0});
    const size_t expected_peak =
        expected.empty() ? 0 : *std::max_element(expected.begin(), expected.end());
    require(plan.assigned_threads == expected_sum, "assigned thread total is inconsistent");
    require(plan.peak_worker_threads == expected_peak, "peak worker thread count is inconsistent");
}

void test_exact_allocations() {
    require_plan(10, 4, 4, {3, 3, 2, 2});
    require_plan(10, 4, 1, {10});
    require_plan(2, 4, 4, {1, 1});
    require_plan(4, 2, 4, {2, 2});
    require_plan(7, 3, 2, {4, 3});
    require_plan(4, 4, 0, {});
}

void test_invalid_limits() {
    for (const auto [budget, outer_limit, batch_size] :
         std::array<std::array<size_t, 3>, 3>{{{0, 1, 1}, {1, 0, 1}, {0, 0, 0}}}) {
        bool threw = false;
        try {
            (void)plan_local_sieve_threads(budget, outer_limit, batch_size);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "zero budget or outer limit must be rejected");
    }
}

void test_property_grid() {
    for (size_t budget = 1; budget <= 32; ++budget) {
        for (size_t outer_limit = 1; outer_limit <= 8; ++outer_limit) {
            for (size_t batch_size = 0; batch_size <= 8; ++batch_size) {
                const LocalSieveThreadPlan plan =
                    plan_local_sieve_threads(budget, outer_limit, batch_size);
                const size_t expected_workers =
                    batch_size == 0 ? 0 : std::min({budget, outer_limit, batch_size});
                require(plan.threads_per_worker.size() == expected_workers,
                        "worker count violates its caps");

                if (batch_size == 0) {
                    require(plan.assigned_threads == 0 && plan.peak_worker_threads == 0,
                            "empty batch must produce an empty plan");
                    continue;
                }

                require(plan.assigned_threads == budget,
                        "non-empty batch must consume the full compute-lane budget");
                require(std::all_of(plan.threads_per_worker.begin(), plan.threads_per_worker.end(),
                                    [](size_t count) { return count > 0; }),
                        "every active worker must receive at least one compute lane");

                const auto [minimum, maximum] = std::minmax_element(plan.threads_per_worker.begin(),
                                                                    plan.threads_per_worker.end());
                require(*maximum - *minimum <= 1, "worker allocations must be balanced");
                require(plan.peak_worker_threads == *maximum,
                        "reported peak must match the allocation vector");
            }
        }
    }
}

} // namespace

int main() {
    try {
        test_exact_allocations();
        test_invalid_limits();
        test_property_grid();
        std::cout << "All local sieve thread budget tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Local sieve thread budget test failed: " << error.what() << '\n';
        return 1;
    }
}
