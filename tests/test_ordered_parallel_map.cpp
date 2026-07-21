#include "gnfs/util/ordered_parallel_map.hpp"

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
    run_test("sequential lowest indexed exception",
             test_sequential_path_attempts_all_indices_before_rethrow);
    run_test("lowest indexed exception drains tail",
             test_lowest_exception_index_wins_after_tail_drains);

    if (failures != 0) {
        std::cerr << failures << " of " << checks << " checks failed\n";
        return 1;
    }
    std::cout << "ordered parallel map: " << checks << " checks passed\n";
    return 0;
}
