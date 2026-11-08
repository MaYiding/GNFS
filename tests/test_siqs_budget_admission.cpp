#include <gnfs/siqs/siqs.hpp>

#include "support/scoped_environment_stderr.hpp"
#include "support/test_check.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
std::atomic<bool> observing{false};
std::atomic<size_t> caller_requests{0};
std::atomic<size_t> worker_requests{0};
std::atomic<size_t> failing_bytes{0};
std::atomic<bool> allocation_failure_triggered{false};
std::thread::id caller;
bool fail_on_caller = false;
std::chrono::steady_clock::time_point pause_caller_until{};
std::chrono::steady_clock::time_point pause_failure_until{};
size_t caller_bytes = 0; // Zero observes all caller allocations.
size_t worker_bytes = 0;

void* allocate(size_t bytes) {
    if (observing.load(std::memory_order_relaxed)) {
        const bool on_caller = std::this_thread::get_id() == caller;
        if (on_caller) {
            if (caller_bytes == 0 || bytes == caller_bytes) {
                caller_requests.fetch_add(1, std::memory_order_relaxed);
                if (pause_caller_until != std::chrono::steady_clock::time_point{})
                    std::this_thread::sleep_until(pause_caller_until);
            }
        } else {
            if (bytes == worker_bytes)
                worker_requests.fetch_add(1, std::memory_order_relaxed);
        }
        if (on_caller == fail_on_caller) {
            size_t expected = bytes;
            if (bytes != 0 &&
                failing_bytes.compare_exchange_strong(expected, 0, std::memory_order_relaxed)) {
                if (pause_failure_until != std::chrono::steady_clock::time_point{})
                    std::this_thread::sleep_until(pause_failure_until);
                allocation_failure_triggered.store(true, std::memory_order_relaxed);
                throw std::bad_alloc{};
            }
        }
    }
    if (void* pointer = std::malloc(bytes == 0 ? 1 : bytes))
        return pointer;
    throw std::bad_alloc{};
}

class ScopedObserver final {
public:
    explicit ScopedObserver(size_t fail_bytes = 0, bool fail_caller = false) noexcept {
        fail_on_caller = fail_caller;
        caller = std::this_thread::get_id();
        caller_requests.store(0, std::memory_order_relaxed);
        worker_requests.store(0, std::memory_order_relaxed);
        failing_bytes.store(fail_bytes, std::memory_order_relaxed);
        allocation_failure_triggered.store(false, std::memory_order_relaxed);
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

// Keep replacement symbols under Release LTO. Faults are one-shot and target
// native worker buffer sizes, never a platform-specific allocation ordinal.
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
    // Count all caller allocations before selecting individual buffers below.
    {
        ScopedObserver observer;
        void* (*volatile forced_allocate)(size_t) = &::operator new;
        ::operator delete(forced_allocate(sizeof(uint32_t)));
    }
    GNFS_TEST_CHECK(caller_requests.load() == 1);

    using Stop = MultiplierPortfolioStop;
    constexpr struct {
        int elapsed_seconds;
        size_t budget;
        size_t limit;
        Stop expected;
    } closed[] = {{0, 60, 0, Stop::attempt_limit},
                  {0, 0, 0, Stop::attempt_limit},
                  {0, 0, 3, Stop::budget_exhausted},
                  {60, 1, 3, Stop::budget_exhausted},
                  {-60, 1, 3, Stop::budget_exhausted}};
    for (const auto& test : closed) {
        const FactorAttemptContext context{Clock::now() -
                                               std::chrono::seconds(test.elapsed_seconds),
                                           test.budget, false, 3, params};
        const auto stopped = [&] {
            ScopedObserver observer;
            return factor_multiplier_portfolio(n, SIQSShadowProofMode::prefer, context, test.limit);
        }();
        GNFS_TEST_CHECK(caller_requests.load() == 0);
        GNFS_TEST_CHECK(stopped.stop == test.expected && stopped.attempts_started == 0);
        GNFS_TEST_CHECK(!stopped.result && !stopped.failure &&
                        stopped.any_observe_record_committed == false);
    }

    const FactorAttemptContext zero_budget{Clock::now(), 0, false, 3, params};
    for (const bool invalid_modulus : {true, false}) {
        bool rejected = false;
        try {
            const auto mode =
                invalid_modulus ? SIQSShadowProofMode::off : static_cast<SIQSShadowProofMode>(255);
            (void)factor_multiplier_portfolio(invalid_modulus ? Integer(16u) : n, mode, zero_budget,
                                              0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        GNFS_TEST_CHECK(rejected);
    }

    // A worker failure still takes precedence if its join crosses the deadline.
    const size_t fb_size = static_cast<size_t>(params.fb_size) + 1;
    worker_bytes = static_cast<size_t>(params.sieve_half) * 2;
    for (const bool fail_caller : {true, false}) {
        const FactorAttemptContext context{Clock::now(), 1, false, 3, params};
        if (!fail_caller)
            pause_failure_until = context.started + std::chrono::seconds(context.max_seconds);
        const auto outcome = [&] {
            ScopedObserver observer(fail_caller ? fb_size * sizeof(FBPrime) : worker_bytes,
                                    fail_caller);
            return factor_multiplier_portfolio(n, SIQSShadowProofMode::off, context, 3);
        }();
        pause_failure_until = {};
        GNFS_TEST_CHECK(allocation_failure_triggered.load());
        GNFS_TEST_CHECK(outcome.attempts_started == 1 && outcome.stop == Stop::attempt_exception &&
                        !outcome.result && outcome.failure && !outcome.attempts[0].terminal);
        bool bad_alloc = false;
        try {
            std::rethrow_exception(outcome.failure);
        } catch (const std::bad_alloc&) {
            bad_alloc = true;
        }
        GNFS_TEST_CHECK(bad_alloc);
    }

    // Hold a real caller allocation in each stage until the shared deadline.
    // The injected delay starts after the portfolio admits the attempt.
    const struct {
        size_t allocation;
        FactorAttemptTerminal terminal;
        bool observe_committed;
    } stages[] = {
        {fb_size * sizeof(FBPrime), FactorAttemptTerminal::factor_base_budget_exhausted, false},
        {(fb_size + 50) * 10 * sizeof(SIQSRelation), FactorAttemptTerminal::sieve_budget_exhausted,
         false},
        {fb_size * sizeof(uint32_t), FactorAttemptTerminal::shadow_budget_exhausted, true}};
    for (const auto& stage : stages) {
        caller_bytes = stage.allocation;
        ScopedStderrCapture capture;
        const FactorAttemptContext context{Clock::now(), 1, false, 3, params};
        pause_caller_until = context.started + std::chrono::seconds(context.max_seconds);
        const auto outcome = [&] {
            ScopedObserver observer;
            return factor_multiplier_portfolio(n, SIQSShadowProofMode::observe, context, 1);
        }();
        pause_caller_until = {};
        const auto log = capture.finish();
        GNFS_TEST_CHECK(caller_requests.load() >= 1 && outcome.attempts_started == 1);
        GNFS_TEST_CHECK(outcome.attempts[0].terminal == stage.terminal);
        GNFS_TEST_CHECK(outcome.stop == Stop::budget_exhausted && !outcome.result &&
                        !outcome.failure);
        GNFS_TEST_CHECK(outcome.attempts[0].observe_record_committed == stage.observe_committed &&
                        outcome.any_observe_record_committed == stage.observe_committed);
        GNFS_TEST_CHECK(occurrences(log, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) ==
                        static_cast<size_t>(stage.observe_committed));
    }

    // Match the native initial relation reserve and each worker's sieve buffer.
    // The unexpired native control below verifies both observations still fire.
    caller_bytes = (fb_size + 50) * 10 * sizeof(SIQSRelation);
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

    // Prove the failure injector independently of native code or allocation
    // elision, then require each real worker fault below to trigger as well.
    std::atomic<bool> self_check_caught{false};
    {
        ScopedObserver observer(worker_bytes);
        std::thread worker([&] {
            void* (*volatile forced_allocate)(size_t) = &::operator new;
            try {
                void* memory = forced_allocate(worker_bytes);
                ::operator delete(memory);
            } catch (const std::bad_alloc&) {
                self_check_caught.store(true, std::memory_order_relaxed);
            }
        });
        worker.join();
    }
    GNFS_TEST_CHECK(self_check_caught.load() && allocation_failure_triggered.load());

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

    // The independent test process must catch actual native worker allocation
    // failures before any observe/prefer terminal record is committed. Each
    // size is derived from the fixture; no hardware worker count is assumed.
    for (const size_t failure_bytes :
         {(static_cast<size_t>(params.fb_size) + 1 + 50) * sizeof(SIQSRelation), worker_bytes,
          static_cast<size_t>(params.fb_size) + 100}) {
        for (const auto mode : {SIQSShadowProofMode::off, SIQSShadowProofMode::observe,
                                SIQSShadowProofMode::prefer}) {
            const FactorAttemptContext context{Clock::now(), 30, false, 3, params};
            ScopedStderrCapture capture;
            bool caught = false;
            {
                ScopedObserver observer(failure_bytes);
                try {
                    (void)factor_once(n, 1, mode, context);
                } catch (const std::bad_alloc&) {
                    caught = true;
                }
            }
            const auto record = capture.finish();
            GNFS_TEST_CHECK(caught && allocation_failure_triggered.load());
            GNFS_TEST_CHECK(occurrences(record, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0);
            GNFS_TEST_CHECK(occurrences(record, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 0);
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
        "SIQS sieve budget admission: PASS (expired buffers, worker failures, shadow terminals, "
        "native control)");
}
