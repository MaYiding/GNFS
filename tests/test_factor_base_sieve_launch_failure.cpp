#include <gnfs/factor_base/builder.hpp>

#include "support/test_check.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t disarmed = std::numeric_limits<std::size_t>::max();
// Only main arms its budget. Every sieve worker starts with injection disabled.
thread_local std::size_t allocations_before_failure = disarmed;
thread_local bool injected_failure = false;
thread_local std::size_t triggered_faults = 0;

} // namespace

// Match the replacement-new convention used by test_factor_base_add_rollback.
// Keep these symbols visible in Release builds with LTO.
#if defined(__GNUC__) || defined(__clang__)
#define GNFS_TEST_ALLOCATION_REPLACEMENT __attribute__((used))
#else
#define GNFS_TEST_ALLOCATION_REPLACEMENT
#endif

GNFS_TEST_ALLOCATION_REPLACEMENT void* operator new(std::size_t size) {
    if (allocations_before_failure != disarmed) {
        if (allocations_before_failure == 0) {
            allocations_before_failure = disarmed;
            injected_failure = true;
            ++triggered_faults;
            throw std::bad_alloc{};
        }
        --allocations_before_failure;
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

int main() {
    using gnfs::factor_base::FactorBaseBuilder;
    constexpr uint32_t bound = 5'000'000;
    constexpr std::size_t max_fault_ordinals = 512;
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    std::cout << "Factor-base sieve launch failure: hardware_concurrency=" << hardware_threads
              << ", bound=" << bound << '\n';
    if (hardware_threads == 1) {
        std::cout << "Single hardware thread: this run cannot establish partial worker-launch "
                     "coverage. Allocation failure and retry checks still apply.\n";
    } else if (hardware_threads == 0) {
        std::cout << "Hardware concurrency is unknown; the sieve uses its configured fallback.\n";
    }

    // Compute the independent serial reference once, before arming injection.
    const auto reference = FactorBaseBuilder::build_eratosthenes_sieve(bound, false);
    GNFS_TEST_CHECK(reference.size() == static_cast<std::size_t>(bound) + 1);
    std::size_t caught_faults = 0;
    bool completed_without_fault = false;
    for (std::size_t ordinal = 0; ordinal < max_fault_ordinals; ++ordinal) {
        // Flush while disarmed so a terminating old implementation leaves the
        // exact failing ordinal in its log. Do not assume a library-specific
        // number of allocations per std::thread construction.
        std::cout << "Injecting main-thread allocation fault ordinal=" << ordinal << std::endl;
        std::vector<bool> result;
        bool failed = false;
        injected_failure = false;
        allocations_before_failure = ordinal;
        try {
            result = FactorBaseBuilder::build_eratosthenes_sieve(bound, true);
        } catch (const std::bad_alloc&) {
            failed = true;
        } catch (...) {
            allocations_before_failure = disarmed;
            throw;
        }
        // Assertions, diagnostics, and retry allocations must not consume the
        // injected budget or turn a test failure into a false allocation hit.
        allocations_before_failure = disarmed;
        if (failed) {
            GNFS_TEST_CHECK(injected_failure);
            ++caught_faults;
            GNFS_TEST_CHECK(triggered_faults == caught_faults);
            const auto retry = FactorBaseBuilder::build_eratosthenes_sieve(bound, true);
            GNFS_TEST_CHECK(retry == reference);
            std::cout << "Caught fault ordinal=" << ordinal
                      << ", triggered_faults=" << triggered_faults
                      << ", caught_faults=" << caught_faults << "; retry matched all "
                      << reference.size() << " entries\n";
            continue;
        }

        GNFS_TEST_CHECK(!injected_failure);
        GNFS_TEST_CHECK(triggered_faults == caught_faults);
        GNFS_TEST_CHECK(result == reference);
        GNFS_TEST_CHECK(caught_faults > 0);
        completed_without_fault = true;
        std::cout << "First no-fault success ordinal=" << ordinal
                  << ", triggered_faults=" << triggered_faults
                  << ", caught_faults=" << caught_faults << "; matched all " << reference.size()
                  << " entries\n";
        break;
    }
    if (!completed_without_fault) {
        std::cerr << "Allocation sweep exhausted " << max_fault_ordinals
                  << " ordinals without a no-fault success; triggered_faults=" << triggered_faults
                  << ", caught_faults=" << caught_faults << '\n';
    }
    GNFS_TEST_CHECK(completed_without_fault);
}
