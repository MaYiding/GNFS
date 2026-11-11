#include <gnfs/core/integer.hpp>
#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/sqrt/hensel_parallel.hpp>
#include <gnfs/sqrt/hensel_sqrt.hpp>
#include <gnfs/sqrt/number_field.hpp>

#include "support/scoped_environment_stderr.hpp"
#include "support/test_check.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

// This binary owns its workers. Keeping the outer Hensel dispatch sequential
// isolates allocations on other threads to the inner product computation.
thread_local bool is_caller = false;
std::atomic<bool> observe_workers{false};
std::atomic<bool> worker_failure_armed{false};
std::atomic<std::size_t> worker_allocations{0};
std::atomic<std::size_t> injected_failures{0};

class InjectedWorkerAllocationFailure final : public std::bad_alloc {};

void disarm() noexcept {
    worker_failure_armed.store(false, std::memory_order_relaxed);
    observe_workers.store(false, std::memory_order_relaxed);
}

} // namespace

// Keep the replacement functions visible in Release builds with LTO.
#if defined(__GNUC__) || defined(__clang__)
#define GNFS_TEST_ALLOCATION_REPLACEMENT __attribute__((used))
#else
#define GNFS_TEST_ALLOCATION_REPLACEMENT
#endif

GNFS_TEST_ALLOCATION_REPLACEMENT void* operator new(std::size_t size) {
    if (!is_caller && observe_workers.load(std::memory_order_relaxed)) {
        worker_allocations.fetch_add(1, std::memory_order_relaxed);
        if (worker_failure_armed.exchange(false, std::memory_order_relaxed)) {
            injected_failures.fetch_add(1, std::memory_order_relaxed);
            // Only one worker throws; allocation during unwinding stays enabled.
            throw InjectedWorkerAllocationFailure{};
        }
    }
    if (void* ptr = std::malloc(size == 0 ? 1 : size))
        return ptr;
    throw std::bad_alloc{};
}

GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

#undef GNFS_TEST_ALLOCATION_REPLACEMENT

namespace {

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;
using gnfs::sqrt::HenselSqrt;
using gnfs::sqrt::NumberField;

void check_square(const std::optional<Integer>& result, const Integer& modulus) {
    GNFS_TEST_CHECK(result.has_value());
    Integer square;
    mpz_powm_ui(square.get_mpz(), result->get_mpz(), 2, modulus.get_mpz());
    // In Q(i), (-alpha)^4=1 and the two remaining factors are (1-alpha)^2.
    // Evaluation at m=8 gives (1-8)^2 = 49 modulo 65.
    GNFS_TEST_CHECK(square == Integer(49));
}

void check_worker_failure() {
    const gnfs::tests::support::ScopedEnvironmentVariable outer_threads("GNFS_SQRT_HENSEL_THREADS",
                                                                        "1");
    gnfs::sqrt::reset_sqrt_hensel_threads_cache();
    GNFS_TEST_CHECK(gnfs::sqrt::sqrt_hensel_threads() == 1);

    const unsigned hw = std::thread::hardware_concurrency();
    const std::size_t hw_safe = hw == 0 ? 4 : static_cast<std::size_t>(hw);
    GNFS_TEST_CHECK(hw_safe <= (std::numeric_limits<std::size_t>::max() - 3) / 50);
    const std::size_t minimum_pairs = std::max(std::size_t{100}, hw_safe * 50);
    std::size_t padding = minimum_pairs - 2;
    padding += (4 - padding % 4) % 4;
    std::vector<std::pair<int64_t, uint64_t>> pairs(padding + 2, {0, 1});
    pairs[0] = {1, 1};
    pairs[1] = {1, 1};

    const PolynomialContext context(Integer(65), {Integer(1), Integer(0), Integer(1)}, Integer(8));
    GNFS_TEST_CHECK(context.verify());
    const NumberField number_field(context);
    HenselSqrt solver;

    worker_allocations.store(0, std::memory_order_relaxed);
    observe_workers.store(true, std::memory_order_relaxed);
    const auto initial = solver.compute(pairs, number_field);
    disarm();
    check_square(initial, number_field.n());
    std::cout << "Hensel product healthy square oracle passed" << std::endl;

    if (hw_safe == 1) {
        std::cout << "SKIP Hensel product worker fault: hardware concurrency is 1; "
                     "only the healthy square oracle was exercised\n";
        return;
    }
    GNFS_TEST_CHECK(worker_allocations.load(std::memory_order_relaxed) > 0);

    bool caught_injected_failure = false;
    worker_allocations.store(0, std::memory_order_relaxed);
    injected_failures.store(0, std::memory_order_relaxed);
    observe_workers.store(true, std::memory_order_relaxed);
    worker_failure_armed.store(true, std::memory_order_relaxed);
    try {
        (void)solver.compute(pairs, number_field);
    } catch (const InjectedWorkerAllocationFailure&) {
        caught_injected_failure = true;
    } catch (...) {
        disarm();
        throw;
    }
    disarm();
    GNFS_TEST_CHECK(caught_injected_failure);
    GNFS_TEST_CHECK(injected_failures.load(std::memory_order_relaxed) == 1);
    GNFS_TEST_CHECK(worker_allocations.load(std::memory_order_relaxed) > 0);

    const auto retry = solver.compute(pairs, number_field);
    check_square(retry, number_field.n());
    GNFS_TEST_CHECK(*retry == *initial);
    std::cout << "Hensel product worker failure propagated once; same-instance retry passed\n";
}

} // namespace

int main() {
    is_caller = true;
    check_worker_failure();
}
