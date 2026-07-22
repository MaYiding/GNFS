#include "gnfs/util/ordered_parallel_map.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using gnfs::util::ordered_parallel_map;
using gnfs::util::ordered_work_stealing_map;

namespace {

int checks = 0;
int failures = 0;
std::string_view current_test;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::cerr << "CHECK failed in " << current_test << " at " << __FILE__ << ':'           \
                      << __LINE__ << ": " << #condition << '\n';                                   \
        }                                                                                          \
    } while (false)

class MoveOnlyResult final {
public:
    explicit MoveOnlyResult(size_t value) noexcept : value_(value) {}

    MoveOnlyResult() = delete;
    MoveOnlyResult(const MoveOnlyResult&) = delete;
    MoveOnlyResult& operator=(const MoveOnlyResult&) = delete;
    MoveOnlyResult(MoveOnlyResult&&) noexcept = default;
    MoveOnlyResult& operator=(MoveOnlyResult&&) = delete;

    [[nodiscard]] size_t value() const noexcept {
        return value_;
    }

private:
    size_t value_;
};

static_assert(!std::is_default_constructible_v<MoveOnlyResult>);
static_assert(!std::is_copy_constructible_v<MoveOnlyResult>);
static_assert(std::is_nothrow_move_constructible_v<MoveOnlyResult>);

struct WorkerLifetimeCounters final {
    std::atomic_size_t constructed{0};
    std::atomic_size_t destroyed{0};
    std::atomic_size_t live{0};
};

class MoveOnlyWorkerState final {
public:
    MoveOnlyWorkerState(std::shared_ptr<WorkerLifetimeCounters> counters, size_t ordinal) noexcept
        : counters_(std::move(counters)), ordinal_(ordinal), owns_lifetime_(true) {
        counters_->constructed.fetch_add(1, std::memory_order_relaxed);
        counters_->live.fetch_add(1, std::memory_order_relaxed);
    }

    MoveOnlyWorkerState() = delete;
    MoveOnlyWorkerState(const MoveOnlyWorkerState&) = delete;
    MoveOnlyWorkerState& operator=(const MoveOnlyWorkerState&) = delete;

    MoveOnlyWorkerState(MoveOnlyWorkerState&& other) noexcept
        : counters_(std::move(other.counters_)), ordinal_(other.ordinal_),
          owns_lifetime_(std::exchange(other.owns_lifetime_, false)) {}

    MoveOnlyWorkerState& operator=(MoveOnlyWorkerState&&) = delete;

    ~MoveOnlyWorkerState() {
        if (owns_lifetime_) {
            counters_->live.fetch_sub(1, std::memory_order_relaxed);
            counters_->destroyed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] size_t ordinal() const noexcept {
        return ordinal_;
    }

private:
    std::shared_ptr<WorkerLifetimeCounters> counters_;
    size_t ordinal_;
    bool owns_lifetime_;
};

static_assert(!std::is_default_constructible_v<MoveOnlyWorkerState>);
static_assert(!std::is_copy_constructible_v<MoveOnlyWorkerState>);
static_assert(std::is_nothrow_move_constructible_v<MoveOnlyWorkerState>);

class ActiveOperationGuard final {
public:
    explicit ActiveOperationGuard(std::atomic_size_t& active) noexcept : active_(&active) {
        active_->fetch_add(1, std::memory_order_relaxed);
    }

    ActiveOperationGuard(const ActiveOperationGuard&) = delete;
    ActiveOperationGuard& operator=(const ActiveOperationGuard&) = delete;

    ~ActiveOperationGuard() {
        active_->fetch_sub(1, std::memory_order_relaxed);
    }

private:
    std::atomic_size_t* active_;
};

void test_move_only_results_preserve_index_order() {
    constexpr size_t count = 19;
    constexpr std::array<uint32_t, 3> thread_counts{1, 2, 4};

    for (const uint32_t threads : thread_counts) {
        std::array<std::atomic_uint, count> calls{};
        const auto results =
            ordered_parallel_map<MoveOnlyResult>(count, threads, [&](size_t index) {
                calls[index].fetch_add(1, std::memory_order_relaxed);
                return MoveOnlyResult(index * 17 + 5);
            });

        CHECK(results.size() == count);
        for (size_t index = 0; index < count; ++index) {
            CHECK(results[index].value() == index * 17 + 5);
            CHECK(calls[index].load(std::memory_order_relaxed) == 1);
        }
    }
}

void test_empty_input_and_invalid_thread_count() {
    std::atomic_uint calls{0};
    for (const uint32_t threads : std::array<uint32_t, 3>{1, 2, 4}) {
        const auto results = ordered_parallel_map<int>(0, threads, [&](size_t) {
            calls.fetch_add(1, std::memory_order_relaxed);
            return 1;
        });
        CHECK(results.empty());
    }
    CHECK(calls.load(std::memory_order_relaxed) == 0);

    bool caught = false;
    try {
        (void)ordered_parallel_map<int>(3, 0, [&](size_t index) {
            calls.fetch_add(1, std::memory_order_relaxed);
            return static_cast<int>(index);
        });
    } catch (const std::invalid_argument&) {
        caught = true;
    } catch (...) {
        CHECK(false);
    }
    CHECK(caught);
    CHECK(calls.load(std::memory_order_relaxed) == 0);

    caught = false;
    try {
        (void)ordered_parallel_map<int>(0, 0, [&](size_t) {
            calls.fetch_add(1, std::memory_order_relaxed);
            return 1;
        });
    } catch (const std::invalid_argument&) {
        caught = true;
    } catch (...) {
        CHECK(false);
    }
    CHECK(caught);
    CHECK(calls.load(std::memory_order_relaxed) == 0);

    const auto bounded = ordered_parallel_map<size_t>(2, std::numeric_limits<uint32_t>::max(),
                                                      [](size_t index) { return index + 41; });
    CHECK(bounded == std::vector<size_t>({41, 42}));
}

void test_work_stealing_move_only_results_preserve_index_order() {
    constexpr size_t count = 19;
    constexpr std::array<uint32_t, 3> worker_counts{1, 2, 4};

    for (const uint32_t workers : worker_counts) {
        auto lifetime = std::make_shared<WorkerLifetimeCounters>();
        std::array<std::atomic_uint, count> calls{};
        std::atomic_bool invalid_worker_ordinal{false};
        const size_t active_workers = std::min<size_t>(workers, count);

        const auto results = ordered_work_stealing_map<MoveOnlyResult>(
            count, workers,
            [lifetime](size_t worker_ordinal) {
                return MoveOnlyWorkerState(lifetime, worker_ordinal);
            },
            [&](MoveOnlyWorkerState& worker, size_t index) {
                if (worker.ordinal() >= active_workers) {
                    invalid_worker_ordinal.store(true, std::memory_order_relaxed);
                }
                calls[index].fetch_add(1, std::memory_order_relaxed);
                return MoveOnlyResult(index * 29 + 7);
            });

        CHECK(results.size() == count);
        CHECK(!invalid_worker_ordinal.load(std::memory_order_relaxed));
        for (size_t index = 0; index < count; ++index) {
            CHECK(results[index].value() == index * 29 + 7);
            CHECK(calls[index].load(std::memory_order_relaxed) == 1);
        }
        CHECK(lifetime->constructed.load(std::memory_order_relaxed) == active_workers);
        CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == active_workers);
        CHECK(lifetime->live.load(std::memory_order_relaxed) == 0);
    }
}

void test_work_stealing_empty_invalid_and_worker_clamp() {
    auto empty_lifetime = std::make_shared<WorkerLifetimeCounters>();
    std::atomic_uint factory_calls{0};
    std::atomic_uint operation_calls{0};

    auto factory = [&](size_t worker_ordinal) {
        factory_calls.fetch_add(1, std::memory_order_relaxed);
        return MoveOnlyWorkerState(empty_lifetime, worker_ordinal);
    };
    auto operation = [&](MoveOnlyWorkerState&, size_t index) {
        operation_calls.fetch_add(1, std::memory_order_relaxed);
        return MoveOnlyResult(index);
    };

    for (const uint32_t workers : std::array<uint32_t, 3>{1, 2, 4}) {
        const auto results =
            ordered_work_stealing_map<MoveOnlyResult>(0, workers, factory, operation);
        CHECK(results.empty());
    }
    CHECK(factory_calls.load(std::memory_order_relaxed) == 0);
    CHECK(operation_calls.load(std::memory_order_relaxed) == 0);

    for (const size_t count : std::array<size_t, 2>{0, 3}) {
        bool caught = false;
        try {
            (void)ordered_work_stealing_map<MoveOnlyResult>(count, 0, factory, operation);
        } catch (const std::invalid_argument&) {
            caught = true;
        } catch (...) {
            CHECK(false);
        }
        CHECK(caught);
    }
    CHECK(factory_calls.load(std::memory_order_relaxed) == 0);
    CHECK(operation_calls.load(std::memory_order_relaxed) == 0);
    CHECK(empty_lifetime->constructed.load(std::memory_order_relaxed) == 0);
    CHECK(empty_lifetime->destroyed.load(std::memory_order_relaxed) == 0);
    CHECK(empty_lifetime->live.load(std::memory_order_relaxed) == 0);

    auto clamp_lifetime = std::make_shared<WorkerLifetimeCounters>();
    std::array<std::atomic_uint, 2> clamp_calls{};
    const auto clamped = ordered_work_stealing_map<MoveOnlyResult>(
        clamp_calls.size(), std::numeric_limits<uint32_t>::max(),
        [clamp_lifetime](size_t worker_ordinal) {
            return MoveOnlyWorkerState(clamp_lifetime, worker_ordinal);
        },
        [&](MoveOnlyWorkerState&, size_t index) {
            clamp_calls[index].fetch_add(1, std::memory_order_relaxed);
            return MoveOnlyResult(index + 41);
        });
    CHECK(clamped.size() == 2);
    CHECK(clamped[0].value() == 41);
    CHECK(clamped[1].value() == 42);
    CHECK(clamp_calls[0].load(std::memory_order_relaxed) == 1);
    CHECK(clamp_calls[1].load(std::memory_order_relaxed) == 1);
    CHECK(clamp_lifetime->constructed.load(std::memory_order_relaxed) == 2);
    CHECK(clamp_lifetime->destroyed.load(std::memory_order_relaxed) == 2);
    CHECK(clamp_lifetime->live.load(std::memory_order_relaxed) == 0);
}

void test_work_stealing_factory_failure_starts_no_operations() {
    auto lifetime = std::make_shared<WorkerLifetimeCounters>();
    std::atomic_uint factory_calls{0};
    std::atomic_uint operation_calls{0};
    size_t live_at_catch = std::numeric_limits<size_t>::max();
    bool caught = false;

    try {
        (void)ordered_work_stealing_map<int>(
            8, 4,
            [&](size_t worker_ordinal) {
                factory_calls.fetch_add(1, std::memory_order_relaxed);
                if (worker_ordinal == 2) {
                    throw std::runtime_error("worker setup failure at ordinal 2");
                }
                return MoveOnlyWorkerState(lifetime, worker_ordinal);
            },
            [&](MoveOnlyWorkerState&, size_t index) {
                operation_calls.fetch_add(1, std::memory_order_relaxed);
                return static_cast<int>(index);
            });
    } catch (const std::runtime_error& error) {
        caught = true;
        live_at_catch = lifetime->live.load(std::memory_order_relaxed);
        CHECK(std::string_view(error.what()) == "worker setup failure at ordinal 2");
    } catch (...) {
        CHECK(false);
    }

    CHECK(caught);
    CHECK(factory_calls.load(std::memory_order_relaxed) == 3);
    CHECK(operation_calls.load(std::memory_order_relaxed) == 0);
    CHECK(lifetime->constructed.load(std::memory_order_relaxed) == 2);
    CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 2);
    CHECK(live_at_catch == 0);
    CHECK(lifetime->live.load(std::memory_order_relaxed) == 0);
}

void test_work_stealing_sequential_path_attempts_all_indices_before_rethrow() {
    constexpr size_t count = 6;
    auto lifetime = std::make_shared<WorkerLifetimeCounters>();
    std::array<std::atomic_uint, count> calls{};
    std::atomic_size_t active_operations{0};
    std::atomic_bool invalid_worker_ordinal{false};
    size_t live_at_catch = std::numeric_limits<size_t>::max();
    size_t active_at_catch = std::numeric_limits<size_t>::max();
    bool caught = false;

    try {
        (void)ordered_work_stealing_map<int>(
            count, 1,
            [lifetime](size_t worker_ordinal) {
                return MoveOnlyWorkerState(lifetime, worker_ordinal);
            },
            [&](MoveOnlyWorkerState& worker, size_t index) {
                ActiveOperationGuard active(active_operations);
                calls[index].fetch_add(1, std::memory_order_relaxed);
                if (worker.ordinal() != 0) {
                    invalid_worker_ordinal.store(true, std::memory_order_relaxed);
                }
                if (index == 1) {
                    throw std::runtime_error("stealing failure at index 1");
                }
                if (index == 4) {
                    throw std::runtime_error("stealing failure at index 4");
                }
                return static_cast<int>(index);
            });
    } catch (const std::runtime_error& error) {
        caught = true;
        live_at_catch = lifetime->live.load(std::memory_order_relaxed);
        active_at_catch = active_operations.load(std::memory_order_relaxed);
        CHECK(std::string_view(error.what()) == "stealing failure at index 1");
    } catch (...) {
        CHECK(false);
    }

    CHECK(caught);
    CHECK(!invalid_worker_ordinal.load(std::memory_order_relaxed));
    for (const auto& call_count : calls) {
        CHECK(call_count.load(std::memory_order_relaxed) == 1);
    }
    CHECK(lifetime->constructed.load(std::memory_order_relaxed) == 1);
    CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 1);
    CHECK(live_at_catch == 0);
    CHECK(active_at_catch == 0);
}

void test_sequential_path_attempts_all_indices_before_rethrow() {
    std::array<std::atomic_uint, 5> calls{};
    bool caught = false;
    try {
        (void)ordered_parallel_map<int>(calls.size(), 1, [&](size_t index) {
            calls[index].fetch_add(1, std::memory_order_relaxed);
            if (index == 1) {
                throw std::runtime_error("sequential failure at index 1");
            }
            if (index == 3) {
                throw std::runtime_error("sequential failure at index 3");
            }
            return static_cast<int>(index);
        });
    } catch (const std::runtime_error& error) {
        caught = true;
        CHECK(std::string_view(error.what()) == "sequential failure at index 1");
    } catch (...) {
        CHECK(false);
    }
    CHECK(caught);
    for (const auto& count : calls) {
        CHECK(count.load(std::memory_order_relaxed) == 1);
    }
}

class ExceptionBarrierControl final {
public:
    ExceptionBarrierControl()
        : release_low_future_(release_low_promise_.get_future().share()),
          release_ordinary_future_(release_ordinary_promise_.get_future().share()),
          release_tail_future_(release_tail_promise_.get_future().share()),
          high_failed_future_(high_failed_promise_.get_future().share()),
          low_and_tail_ready_future_(low_and_tail_ready_promise_.get_future().share()),
          tail_finished_future_(tail_finished_promise_.get_future().share()) {}

    ExceptionBarrierControl(const ExceptionBarrierControl&) = delete;
    ExceptionBarrierControl& operator=(const ExceptionBarrierControl&) = delete;

    [[nodiscard]] std::shared_future<void> release_low_future() const {
        return release_low_future_;
    }

    [[nodiscard]] std::shared_future<void> release_tail_future() const {
        return release_tail_future_;
    }

    [[nodiscard]] std::shared_future<void> release_ordinary_future() const {
        return release_ordinary_future_;
    }

    [[nodiscard]] const std::shared_future<void>& high_failed_future() const noexcept {
        return high_failed_future_;
    }

    [[nodiscard]] const std::shared_future<void>& low_and_tail_ready_future() const noexcept {
        return low_and_tail_ready_future_;
    }

    [[nodiscard]] const std::shared_future<void>& tail_finished_future() const noexcept {
        return tail_finished_future_;
    }

    void mark_high_failed() {
        high_failed_promise_.set_value();
    }

    void mark_low_or_tail_ready() {
        if (low_and_tail_ready_count_.fetch_add(1, std::memory_order_acq_rel) == 1) {
            low_and_tail_ready_promise_.set_value();
        }
    }

    void mark_tail_finished() {
        tail_finished_promise_.set_value();
    }

    void release_low() noexcept {
        bool expected = false;
        if (low_released_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            try {
                release_low_promise_.set_value();
            } catch (...) {
            }
        }
    }

    void release_tail() noexcept {
        bool expected = false;
        if (tail_released_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            try {
                release_tail_promise_.set_value();
            } catch (...) {
            }
        }
    }

    void release_ordinary() noexcept {
        bool expected = false;
        if (ordinary_released_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            try {
                release_ordinary_promise_.set_value();
            } catch (...) {
            }
        }
    }

private:
    std::promise<void> release_low_promise_;
    std::promise<void> release_ordinary_promise_;
    std::promise<void> release_tail_promise_;
    std::promise<void> high_failed_promise_;
    std::promise<void> low_and_tail_ready_promise_;
    std::promise<void> tail_finished_promise_;
    std::shared_future<void> release_low_future_;
    std::shared_future<void> release_ordinary_future_;
    std::shared_future<void> release_tail_future_;
    std::shared_future<void> high_failed_future_;
    std::shared_future<void> low_and_tail_ready_future_;
    std::shared_future<void> tail_finished_future_;
    std::atomic_uint low_and_tail_ready_count_{0};
    std::atomic_bool low_released_{false};
    std::atomic_bool ordinary_released_{false};
    std::atomic_bool tail_released_{false};
};

struct InvocationResult final {
    bool caught_runtime_error = false;
    std::string message;
};

void check_lowest_exception_index_wins_after_tail_drains(uint32_t threads) {
    using namespace std::chrono_literals;
    constexpr auto timeout = 2s;
    auto control = std::make_shared<ExceptionBarrierControl>();

    std::promise<InvocationResult> invocation_promise;
    auto invocation_future = invocation_promise.get_future();
    std::thread invocation_thread(
        [control, threads, invocation_promise = std::move(invocation_promise)]() mutable {
            InvocationResult invocation_result;
            try {
                (void)ordered_parallel_map<int>(6, threads, [control, threads](size_t index) {
                    if (threads >= 4 && (index == 0 || index == 2)) {
                        control->release_ordinary_future().wait();
                    }
                    if (index == 1) {
                        control->release_low_future().wait();
                        control->mark_low_or_tail_ready();
                        throw std::runtime_error("failure at index 1");
                    }
                    if (index == 3) {
                        throw std::runtime_error("failure at index 3");
                    }
                    // With four workers, indices 0, 1, and 2 are gated. With
                    // two workers, FIFO dispatch leaves one worker to reach
                    // index 3. In both cases, reaching index 4 proves the
                    // index-3 invocation has unwound through invoke() before
                    // index 1 is released.
                    if (index == 4) {
                        control->mark_high_failed();
                    }
                    if (index == 5) {
                        control->mark_low_or_tail_ready();
                        control->release_tail_future().wait();
                        control->mark_tail_finished();
                    }
                    return static_cast<int>(index);
                });
                invocation_result.message = "no exception";
            } catch (const std::runtime_error& error) {
                invocation_result.caught_runtime_error = true;
                invocation_result.message = error.what();
            } catch (const std::exception& error) {
                invocation_result.message = std::string("wrong exception: ") + error.what();
            } catch (...) {
                invocation_result.message = "wrong non-standard exception";
            }
            invocation_promise.set_value(std::move(invocation_result));
        });

    const bool high_failed =
        control->high_failed_future().wait_for(timeout) == std::future_status::ready;
    CHECK(high_failed);
    control->release_ordinary();
    control->release_low();

    const bool low_and_tail_ready =
        control->low_and_tail_ready_future().wait_for(timeout) == std::future_status::ready;
    CHECK(low_and_tail_ready);
    if (low_and_tail_ready) {
        CHECK(invocation_future.wait_for(0s) == std::future_status::timeout);
    }

    // Always release the tail gate, including after failed readiness checks.
    control->release_ordinary();
    control->release_low();
    control->release_tail();
    const bool invocation_ready = invocation_future.wait_for(timeout) == std::future_status::ready;
    CHECK(invocation_ready);

    if (invocation_ready) {
        const auto result = invocation_future.get();
        CHECK(result.caught_runtime_error);
        CHECK(result.message == "failure at index 1");
        CHECK(control->tail_finished_future().wait_for(0s) == std::future_status::ready);
        invocation_thread.join();
    } else {
        // Keep this test bounded even if a broken implementation deadlocks.
        invocation_thread.detach();
    }
}

void test_lowest_exception_index_wins_after_tail_drains() {
    check_lowest_exception_index_wins_after_tail_drains(2);
    check_lowest_exception_index_wins_after_tail_drains(4);
}

class WorkStealingFailureControl final {
public:
    static constexpr size_t item_count = 6;

    WorkStealingFailureControl()
        : release_low_future_(release_low_promise_.get_future().share()),
          release_tail_future_(release_tail_promise_.get_future().share()),
          high_failed_future_(high_failed_promise_.get_future().share()),
          low_failed_future_(low_failed_promise_.get_future().share()),
          tail_started_future_(tail_started_promise_.get_future().share()),
          high_and_tail_ready_future_(high_and_tail_ready_promise_.get_future().share()),
          tail_finished_future_(tail_finished_promise_.get_future().share()),
          lifetime_(std::make_shared<WorkerLifetimeCounters>()) {}

    WorkStealingFailureControl(const WorkStealingFailureControl&) = delete;
    WorkStealingFailureControl& operator=(const WorkStealingFailureControl&) = delete;

    [[nodiscard]] const std::shared_future<void>& release_low_future() const noexcept {
        return release_low_future_;
    }

    [[nodiscard]] const std::shared_future<void>& release_tail_future() const noexcept {
        return release_tail_future_;
    }

    [[nodiscard]] const std::shared_future<void>& high_failed_future() const noexcept {
        return high_failed_future_;
    }

    [[nodiscard]] const std::shared_future<void>& low_failed_future() const noexcept {
        return low_failed_future_;
    }

    [[nodiscard]] const std::shared_future<void>& tail_started_future() const noexcept {
        return tail_started_future_;
    }

    [[nodiscard]] const std::shared_future<void>& high_and_tail_ready_future() const noexcept {
        return high_and_tail_ready_future_;
    }

    [[nodiscard]] const std::shared_future<void>& tail_finished_future() const noexcept {
        return tail_finished_future_;
    }

    [[nodiscard]] const std::shared_ptr<WorkerLifetimeCounters>& lifetime() const noexcept {
        return lifetime_;
    }

    [[nodiscard]] std::atomic_size_t& active_operations() noexcept {
        return active_operations_;
    }

    void record_call(size_t index) noexcept {
        calls_[index].fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] unsigned call_count(size_t index) const noexcept {
        return calls_[index].load(std::memory_order_relaxed);
    }

    void mark_high_failed() {
        bool expected = false;
        if (high_failed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            high_failed_promise_.set_value();
            mark_high_or_tail_ready();
        }
    }

    void mark_low_failed() {
        bool expected = false;
        if (low_failed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            low_failed_promise_.set_value();
        }
    }

    void mark_tail_started() {
        bool expected = false;
        if (tail_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            tail_started_promise_.set_value();
            mark_high_or_tail_ready();
        }
    }

    void mark_tail_finished() {
        bool expected = false;
        if (tail_finished_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            tail_finished_promise_.set_value();
        }
    }

    void release_low() noexcept {
        bool expected = false;
        if (low_released_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            try {
                release_low_promise_.set_value();
            } catch (...) {
            }
        }
    }

    void release_tail() noexcept {
        bool expected = false;
        if (tail_released_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            try {
                release_tail_promise_.set_value();
            } catch (...) {
            }
        }
    }

private:
    void mark_high_or_tail_ready() {
        if (high_and_tail_ready_count_.fetch_add(1, std::memory_order_acq_rel) == 1) {
            high_and_tail_ready_promise_.set_value();
        }
    }

    std::promise<void> release_low_promise_;
    std::promise<void> release_tail_promise_;
    std::promise<void> high_failed_promise_;
    std::promise<void> low_failed_promise_;
    std::promise<void> tail_started_promise_;
    std::promise<void> high_and_tail_ready_promise_;
    std::promise<void> tail_finished_promise_;
    std::shared_future<void> release_low_future_;
    std::shared_future<void> release_tail_future_;
    std::shared_future<void> high_failed_future_;
    std::shared_future<void> low_failed_future_;
    std::shared_future<void> tail_started_future_;
    std::shared_future<void> high_and_tail_ready_future_;
    std::shared_future<void> tail_finished_future_;
    std::shared_ptr<WorkerLifetimeCounters> lifetime_;
    std::array<std::atomic_uint, item_count> calls_{};
    std::atomic_size_t active_operations_{0};
    std::atomic_uint high_and_tail_ready_count_{0};
    std::atomic_bool high_failed_{false};
    std::atomic_bool low_failed_{false};
    std::atomic_bool tail_started_{false};
    std::atomic_bool tail_finished_{false};
    std::atomic_bool low_released_{false};
    std::atomic_bool tail_released_{false};
};

struct WorkStealingInvocationResult final {
    bool caught_runtime_error = false;
    std::string message;
    size_t live_workers_at_catch = std::numeric_limits<size_t>::max();
    size_t destroyed_workers_at_catch = std::numeric_limits<size_t>::max();
    size_t active_operations_at_catch = std::numeric_limits<size_t>::max();
};

void check_work_stealing_lowest_exception_wins_after_claimed_tail_drains(uint32_t workers) {
    using namespace std::chrono_literals;
    constexpr auto timeout = 2s;
    auto control = std::make_shared<WorkStealingFailureControl>();

    std::promise<WorkStealingInvocationResult> invocation_promise;
    auto invocation_future = invocation_promise.get_future();
    std::thread invocation_thread(
        [control, workers, invocation_promise = std::move(invocation_promise)]() mutable {
            WorkStealingInvocationResult invocation_result;
            try {
                (void)ordered_work_stealing_map<MoveOnlyResult>(
                    WorkStealingFailureControl::item_count, workers,
                    [control](size_t worker_ordinal) {
                        return MoveOnlyWorkerState(control->lifetime(), worker_ordinal);
                    },
                    [control](MoveOnlyWorkerState&, size_t index) {
                        ActiveOperationGuard active(control->active_operations());
                        control->record_call(index);
                        if (index == 0) {
                            control->release_low_future().wait();
                            control->mark_low_failed();
                            throw std::runtime_error("stealing failure at index 0");
                        }
                        if (index == 2) {
                            control->mark_high_failed();
                            throw std::runtime_error("stealing failure at index 2");
                        }
                        if (index == 3) {
                            control->mark_tail_started();
                            control->release_tail_future().wait();
                            control->mark_tail_finished();
                        }
                        return MoveOnlyResult(index);
                    });
                invocation_result.message = "no exception";
            } catch (const std::runtime_error& error) {
                invocation_result.caught_runtime_error = true;
                invocation_result.message = error.what();
            } catch (const std::exception& error) {
                invocation_result.message = std::string("wrong exception: ") + error.what();
            } catch (...) {
                invocation_result.message = "wrong non-standard exception";
            }
            invocation_result.live_workers_at_catch =
                control->lifetime()->live.load(std::memory_order_relaxed);
            invocation_result.destroyed_workers_at_catch =
                control->lifetime()->destroyed.load(std::memory_order_relaxed);
            invocation_result.active_operations_at_catch =
                control->active_operations().load(std::memory_order_relaxed);
            invocation_promise.set_value(std::move(invocation_result));
        });

    const bool high_and_tail_ready =
        control->high_and_tail_ready_future().wait_for(timeout) == std::future_status::ready;
    CHECK(high_and_tail_ready);
    if (high_and_tail_ready) {
        CHECK(control->high_failed_future().wait_for(0s) == std::future_status::ready);
        CHECK(control->tail_started_future().wait_for(0s) == std::future_status::ready);
        CHECK(invocation_future.wait_for(0s) == std::future_status::timeout);

        // The lower canonical failure happens after the higher failure, while
        // the already-claimed tail remains blocked.
        control->release_low();
        const bool low_failed =
            control->low_failed_future().wait_for(timeout) == std::future_status::ready;
        CHECK(low_failed);
        if (low_failed) {
            CHECK(invocation_future.wait_for(0s) == std::future_status::timeout);
            CHECK(control->lifetime()->live.load(std::memory_order_relaxed) ==
                  std::min<size_t>(workers, WorkStealingFailureControl::item_count));
        }
    }

    // Always release both gates so a failed readiness assertion cannot leave
    // the test's invocation thread permanently blocked.
    control->release_low();
    control->release_tail();
    const bool invocation_ready = invocation_future.wait_for(timeout) == std::future_status::ready;
    CHECK(invocation_ready);

    if (invocation_ready) {
        const auto result = invocation_future.get();
        const size_t expected_workers =
            std::min<size_t>(workers, WorkStealingFailureControl::item_count);
        CHECK(result.caught_runtime_error);
        CHECK(result.message == "stealing failure at index 0");
        CHECK(result.live_workers_at_catch == 0);
        CHECK(result.destroyed_workers_at_catch == expected_workers);
        CHECK(result.active_operations_at_catch == 0);
        CHECK(control->tail_finished_future().wait_for(0s) == std::future_status::ready);
        for (size_t index = 0; index < WorkStealingFailureControl::item_count; ++index) {
            CHECK(control->call_count(index) == 1);
        }
        invocation_thread.join();
    } else {
        invocation_thread.detach();
    }
}

void test_work_stealing_lowest_exception_wins_after_claimed_tail_drains() {
    check_work_stealing_lowest_exception_wins_after_claimed_tail_drains(2);
    check_work_stealing_lowest_exception_wins_after_claimed_tail_drains(4);
}

template <typename Action> void run_test(std::string_view name, Action&& action) {
    current_test = name;
    try {
        std::forward<Action>(action)();
    } catch (const std::exception& error) {
        ++checks;
        ++failures;
        std::cerr << "unexpected exception in " << current_test << ": " << error.what() << '\n';
    } catch (...) {
        ++checks;
        ++failures;
        std::cerr << "unexpected non-standard exception in " << current_test << '\n';
    }
}

} // namespace

int main() {
    run_test("move-only ordered results", test_move_only_results_preserve_index_order);
    run_test("empty input and invalid threads", test_empty_input_and_invalid_thread_count);
    run_test("work-stealing move-only ordered results",
             test_work_stealing_move_only_results_preserve_index_order);
    run_test("work-stealing empty invalid and clamp",
             test_work_stealing_empty_invalid_and_worker_clamp);
    run_test("work-stealing factory failure",
             test_work_stealing_factory_failure_starts_no_operations);
    run_test("work-stealing sequential lowest indexed exception",
             test_work_stealing_sequential_path_attempts_all_indices_before_rethrow);
    run_test("sequential lowest indexed exception",
             test_sequential_path_attempts_all_indices_before_rethrow);
    run_test("lowest indexed exception drains tail",
             test_lowest_exception_index_wins_after_tail_drains);
    run_test("work-stealing lowest indexed exception drains claimed tail",
             test_work_stealing_lowest_exception_wins_after_claimed_tail_drains);

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " checks failed\n";
        return 1;
    }
    std::cout << "ordered parallel map: " << checks << " checks passed\n";
    return 0;
}
