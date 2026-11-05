#include <gnfs/siqs/siqs.hpp>

#include "support/scoped_environment_stderr.hpp"
#include "support/test_check.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <thread>

namespace {
std::atomic<bool> observing{false};
std::atomic<size_t> caller_requests{0};
std::atomic<size_t> worker_requests{0};
std::thread::id caller;
size_t caller_bytes = 0;
size_t worker_bytes = 0;

void* allocate(size_t bytes) {
    if (observing.load(std::memory_order_relaxed)) {
        if (std::this_thread::get_id() == caller) {
            if (bytes == caller_bytes)
                caller_requests.fetch_add(1, std::memory_order_relaxed);
        } else if (bytes == worker_bytes) {
            worker_requests.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (void* pointer = std::malloc(bytes == 0 ? 1 : bytes))
        return pointer;
    throw std::bad_alloc{};
}

class ScopedObserver final {
public:
    ScopedObserver() noexcept {
        caller = std::this_thread::get_id();
        caller_requests.store(0, std::memory_order_relaxed);
        worker_requests.store(0, std::memory_order_relaxed);
        observing.store(true, std::memory_order_relaxed);
    }
    ~ScopedObserver() {
        observing.store(false, std::memory_order_relaxed);
    }
    ScopedObserver(const ScopedObserver&) = delete;
    ScopedObserver& operator=(const ScopedObserver&) = delete;
};

size_t occurrences(std::string_view text, std::string_view token) {
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(token, offset)) != std::string_view::npos) {
        ++count;
        offset += token.size();
    }
    return count;
}
} // namespace

// Keep replacement symbols under Release LTO. This isolated executable only
// observes requests; it never injects an allocation failure into native workers.
#if defined(__GNUC__) || defined(__clang__)
#define GNFS_TEST_ALLOCATION_REPLACEMENT __attribute__((used))
#else
#define GNFS_TEST_ALLOCATION_REPLACEMENT
#endif
GNFS_TEST_ALLOCATION_REPLACEMENT void* operator new(size_t size) {
    return allocate(size);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void* operator new[](size_t size) {
    return allocate(size);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}
#undef GNFS_TEST_ALLOCATION_REPLACEMENT

int main() {
    using namespace gnfs::siqs;
    using namespace gnfs::siqs::siqs_factor_detail;
    using gnfs::tests::support::ScopedStderrCapture;
    using Clock = std::chrono::steady_clock;

    const Integer n(143u);
    const auto params = select_params(3);
    // Match the native initial relation reserve and each worker's sieve buffer.
    // The unexpired native control below verifies both observations still fire.
    caller_bytes = (static_cast<size_t>(params.fb_size) + 1 + 50) * 10 * sizeof(SIQSRelation);
    worker_bytes = static_cast<size_t>(params.sieve_half) * 2;
    {
        ScopedObserver observer;
        void* memory = ::operator new(caller_bytes);
        ::operator delete(memory);
        std::thread worker([] {
            void* buffer = ::operator new(worker_bytes);
            ::operator delete(buffer);
        });
        worker.join();
    }
    GNFS_TEST_CHECK(caller_requests.load() > 0 && worker_requests.load() > 0);

    for (const size_t seconds : {size_t(0), size_t(1)}) {
        for (const auto mode : {SIQSShadowProofMode::off, SIQSShadowProofMode::observe,
                                SIQSShadowProofMode::prefer}) {
            const FactorAttemptContext context{Clock::now() - std::chrono::seconds(60), seconds,
                                               false, 3, params};
            ScopedStderrCapture capture;
            FactorAttemptOutcome outcome;
            {
                ScopedObserver observer;
                outcome = factor_once(n, 1, mode, context);
            }
            const auto record = capture.finish();
            GNFS_TEST_CHECK(caller_requests.load() == 0);
            GNFS_TEST_CHECK(worker_requests.load() == 0);
            GNFS_TEST_CHECK(!outcome.result);
            GNFS_TEST_CHECK(outcome.terminal == FactorAttemptTerminal::insufficient_relations);
            GNFS_TEST_CHECK(outcome.observe_record_committed ==
                            (mode == SIQSShadowProofMode::observe));
            GNFS_TEST_CHECK(occurrences(record, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) ==
                            (mode == SIQSShadowProofMode::observe ? 1u : 0u));
            GNFS_TEST_CHECK(occurrences(record, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) ==
                            (mode == SIQSShadowProofMode::prefer ? 1u : 0u));
        }
    }

    // Exercise the real unexpired path rather than trusting only synthetic
    // allocation controls. No exact hardware-worker or allocation count is pinned.
    const FactorAttemptContext context{Clock::now(), 30, false, 3, params};
    FactorAttemptOutcome outcome;
    {
        ScopedObserver observer;
        outcome = factor_once(n, select_multiplier(n), SIQSShadowProofMode::off, context);
    }
    GNFS_TEST_CHECK(caller_requests.load() > 0 && worker_requests.load() > 0);
    GNFS_TEST_CHECK(outcome.result.has_value());
    GNFS_TEST_CHECK(outcome.terminal == FactorAttemptTerminal::factor_found);
    GNFS_TEST_CHECK(outcome.result->factor1 > Integer(1u) && outcome.result->factor2 > Integer(1u));
    GNFS_TEST_CHECK(outcome.result->factor1 * outcome.result->factor2 == n);
    GNFS_TEST_CHECK(outcome.result->relations_found > 0 && outcome.result->polynomials_used > 0);
    GNFS_TEST_CHECK(outcome.result->resolved_sieve_workers ==
                    resolve_siqs_sieve_workers(std::thread::hardware_concurrency()));
    GNFS_TEST_CHECK(!outcome.observe_record_committed &&
                    !outcome.result->shadow_proof_observe_record_committed);
    std::puts(
        "SIQS sieve budget admission: PASS (expired buffers, shadow terminals, native control)");
}
