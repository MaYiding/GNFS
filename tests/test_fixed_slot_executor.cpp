// test_fixed_slot_executor.cpp - static-partition parallel slot contracts

#include <gnfs/util/fixed_slot_executor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using gnfs::util::execute_fixed_slots;
using gnfs::util::FixedSlotFailurePolicy;
using gnfs::util::FixedSlotPartition;
using std::size_t;
using std::uint32_t;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

class MoveOnlySlot final {
public:
    MoveOnlySlot(size_t index, size_t worker) noexcept : index_(index), worker_(worker) {}

    MoveOnlySlot() = delete;
    MoveOnlySlot(const MoveOnlySlot&) = delete;
    MoveOnlySlot& operator=(const MoveOnlySlot&) = delete;
    MoveOnlySlot(MoveOnlySlot&&) noexcept = default;
    MoveOnlySlot& operator=(MoveOnlySlot&&) = delete;

    [[nodiscard]] size_t index() const noexcept {
        return index_;
    }

    [[nodiscard]] size_t worker() const noexcept {
        return worker_;
    }

private:
    size_t index_;
    size_t worker_;
};

struct WorkerLifetime {
    std::atomic_size_t constructed{0};
    std::atomic_size_t destroyed{0};
    std::atomic_size_t live{0};
};

class NonMovableWorker final {
public:
    NonMovableWorker(std::shared_ptr<WorkerLifetime> lifetime, FixedSlotPartition partition)
        : lifetime_(std::move(lifetime)), partition_(partition) {
        lifetime_->constructed.fetch_add(1, std::memory_order_relaxed);
        lifetime_->live.fetch_add(1, std::memory_order_relaxed);
    }

    NonMovableWorker(const NonMovableWorker&) = delete;
    NonMovableWorker& operator=(const NonMovableWorker&) = delete;
    NonMovableWorker(NonMovableWorker&&) = delete;
    NonMovableWorker& operator=(NonMovableWorker&&) = delete;

    ~NonMovableWorker() {
        lifetime_->live.fetch_sub(1, std::memory_order_relaxed);
        lifetime_->destroyed.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] const FixedSlotPartition& partition() const noexcept {
        return partition_;
    }

private:
    std::shared_ptr<WorkerLifetime> lifetime_;
    FixedSlotPartition partition_;
};

class ActiveGuard final {
public:
    explicit ActiveGuard(std::atomic_size_t& active) noexcept : active_(&active) {
        active_->fetch_add(1, std::memory_order_relaxed);
    }

    ActiveGuard(const ActiveGuard&) = delete;
    ActiveGuard& operator=(const ActiveGuard&) = delete;

    ~ActiveGuard() {
        active_->fetch_sub(1, std::memory_order_relaxed);
    }

private:
    std::atomic_size_t* active_;
};

class FirstSlotBarrier final {
public:
    explicit FirstSlotBarrier(size_t expected)
        : expected_(expected), next_to_finish_(expected - size_t{1}) {}

    void arrive_and_finish_in_reverse(size_t worker_ordinal) {
        std::unique_lock lock(mutex_);
        ++arrived_;
        ++active_;
        max_active_ = std::max(max_active_, active_);
        condition_.notify_all();
        condition_.wait(lock, [&] { return arrived_ == expected_; });
        condition_.wait(lock,
                        [&] { return reverse_finished_ || worker_ordinal == next_to_finish_; });
        finish_order_.push_back(worker_ordinal);
        --active_;
        if (next_to_finish_ == 0) {
            reverse_finished_ = true;
        } else {
            --next_to_finish_;
        }
        condition_.notify_all();
    }

    [[nodiscard]] size_t max_active() const {
        std::lock_guard lock(mutex_);
        return max_active_;
    }

    [[nodiscard]] std::vector<size_t> finish_order() const {
        std::lock_guard lock(mutex_);
        return finish_order_;
    }

private:
    size_t expected_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    size_t arrived_ = 0;
    size_t active_ = 0;
    size_t max_active_ = 0;
    size_t next_to_finish_;
    bool reverse_finished_ = false;
    std::vector<size_t> finish_order_;
};

class CompletionOrderedSlot final {
public:
    CompletionOrderedSlot(size_t index, size_t worker, bool order_first_move,
                          FirstSlotBarrier& completion_barrier) noexcept
        : index_(index), worker_(worker), completion_barrier_(&completion_barrier),
          order_first_move_(order_first_move) {}

    CompletionOrderedSlot() = delete;
    CompletionOrderedSlot(const CompletionOrderedSlot&) = delete;
    CompletionOrderedSlot& operator=(const CompletionOrderedSlot&) = delete;

    CompletionOrderedSlot(CompletionOrderedSlot&& other)
        : index_(other.index_), worker_(other.worker_),
          completion_barrier_(other.completion_barrier_),
          order_first_move_(other.order_first_move_), move_generation_(other.move_generation_ + 1) {
        if (other.move_generation_ == 0 && order_first_move_) {
            completion_barrier_->arrive_and_finish_in_reverse(worker_);
        }
        other.completion_barrier_ = nullptr;
        other.order_first_move_ = false;
    }

    CompletionOrderedSlot& operator=(CompletionOrderedSlot&&) = delete;

    [[nodiscard]] size_t index() const noexcept {
        return index_;
    }

    [[nodiscard]] size_t worker() const noexcept {
        return worker_;
    }

private:
    size_t index_;
    size_t worker_;
    FirstSlotBarrier* completion_barrier_;
    bool order_first_move_;
    size_t move_generation_ = 0;
};

struct ResultLifetime {
    std::atomic_size_t constructed{0};
    std::atomic_size_t destroyed{0};
    std::atomic_size_t live{0};
};

class CountedSlot final {
public:
    CountedSlot(std::shared_ptr<ResultLifetime> lifetime, size_t index)
        : lifetime_(std::move(lifetime)), index_(index) {
        lifetime_->constructed.fetch_add(1, std::memory_order_relaxed);
        lifetime_->live.fetch_add(1, std::memory_order_relaxed);
    }

    CountedSlot(const CountedSlot&) = delete;
    CountedSlot& operator=(const CountedSlot&) = delete;
    CountedSlot(CountedSlot&&) noexcept = default;
    CountedSlot& operator=(CountedSlot&&) = delete;

    ~CountedSlot() {
        if (lifetime_) {
            lifetime_->live.fetch_sub(1, std::memory_order_relaxed);
            lifetime_->destroyed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] size_t index() const noexcept {
        return index_;
    }

private:
    std::shared_ptr<ResultLifetime> lifetime_;
    size_t index_;
};

[[nodiscard]] std::vector<FixedSlotPartition> expected_partitions(uint32_t workers) {
    switch (workers) {
    case 1:
        return {{0, 0, 7}};
    case 2:
        return {{0, 0, 4}, {1, 4, 7}};
    case 4:
        return {{0, 0, 2}, {1, 2, 4}, {2, 4, 6}, {3, 6, 7}};
    default:
        return {};
    }
}

void test_static_partitions_and_canonical_results() {
    constexpr size_t slot_count = 7;
    for (const uint32_t workers : std::array<uint32_t, 3>{1, 2, 4}) {
        auto lifetime = std::make_shared<WorkerLifetime>();
        std::vector<FixedSlotPartition> observed_partitions;
        std::array<std::atomic_uint, slot_count> calls{};
        std::atomic_bool invalid_partition{false};
        std::atomic_bool operation_on_caller{false};
        std::vector<std::thread::id> operation_threads(workers);
        const std::thread::id caller_thread = std::this_thread::get_id();
        bool factory_left_caller = false;
        std::barrier operation_overlap(static_cast<std::ptrdiff_t>(workers));
        FirstSlotBarrier completion_barrier(workers);

        auto execution = execute_fixed_slots<CompletionOrderedSlot>(
            slot_count, workers, FixedSlotFailurePolicy::cancel_remaining,
            [&](const FixedSlotPartition& partition) {
                if (std::this_thread::get_id() != caller_thread) {
                    factory_left_caller = true;
                }
                observed_partitions.push_back(partition);
                return std::make_unique<NonMovableWorker>(lifetime, partition);
            },
            [&](std::unique_ptr<NonMovableWorker>& worker, size_t index) {
                if (std::this_thread::get_id() == caller_thread) {
                    operation_on_caller.store(true, std::memory_order_relaxed);
                }
                calls[index].fetch_add(1, std::memory_order_relaxed);
                if (index < worker->partition().begin || index >= worker->partition().end) {
                    invalid_partition.store(true, std::memory_order_relaxed);
                }
                if (index == worker->partition().begin) {
                    operation_threads[worker->partition().worker_ordinal] =
                        std::this_thread::get_id();
                    operation_overlap.arrive_and_wait();
                }
                return CompletionOrderedSlot(index, worker->partition().worker_ordinal,
                                             index == worker->partition().begin,
                                             completion_barrier);
            });

        CHECK(!factory_left_caller);
        CHECK(!invalid_partition.load(std::memory_order_relaxed));
        CHECK(!operation_on_caller.load(std::memory_order_relaxed));
        CHECK(observed_partitions == expected_partitions(workers));
        CHECK(completion_barrier.max_active() == workers);
        for (size_t worker = 0; worker < workers; ++worker) {
            CHECK(operation_threads[worker] != caller_thread);
            for (size_t earlier = 0; earlier < worker; ++earlier) {
                CHECK(operation_threads[worker] != operation_threads[earlier]);
            }
        }
        std::vector<size_t> expected_finish_order;
        for (size_t worker = workers; worker > 0; --worker) {
            expected_finish_order.push_back(worker - size_t{1});
        }
        CHECK(completion_barrier.finish_order() == expected_finish_order);
        CHECK(execution.stats.slot_count == slot_count);
        CHECK(execution.stats.requested_workers == workers);
        CHECK(execution.stats.resolved_workers == workers);
        CHECK(execution.stats.peak_workers == workers);
        CHECK(execution.slots.size() == slot_count);
        for (size_t index = 0; index < slot_count; ++index) {
            CHECK(calls[index].load(std::memory_order_relaxed) == 1);
            CHECK(execution.slots[index].index() == index);
            const auto partition =
                std::find_if(observed_partitions.begin(), observed_partitions.end(),
                             [index](const FixedSlotPartition& candidate) {
                                 return index >= candidate.begin && index < candidate.end;
                             });
            CHECK(partition != observed_partitions.end());
            if (partition != observed_partitions.end()) {
                CHECK(execution.slots[index].worker() == partition->worker_ordinal);
            }
        }
        CHECK(lifetime->constructed.load(std::memory_order_relaxed) == workers);
        CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == workers);
        CHECK(lifetime->live.load(std::memory_order_relaxed) == 0);
    }
}

void test_empty_invalid_and_clamped_workers() {
    std::atomic_uint factory_calls{0};
    std::atomic_uint operation_calls{0};
    const auto factory = [&](FixedSlotPartition partition) {
        factory_calls.fetch_add(1, std::memory_order_relaxed);
        return partition;
    };
    const auto operation = [&](FixedSlotPartition& partition, size_t index) {
        operation_calls.fetch_add(1, std::memory_order_relaxed);
        return MoveOnlySlot(index, partition.worker_ordinal);
    };

    const auto empty = execute_fixed_slots<MoveOnlySlot>(
        0, 4, FixedSlotFailurePolicy::cancel_remaining, factory, operation);
    CHECK(empty.slots.empty());
    CHECK(empty.stats.slot_count == 0);
    CHECK(empty.stats.requested_workers == 4);
    CHECK(empty.stats.resolved_workers == 0);
    CHECK(empty.stats.peak_workers == 0);
    CHECK(factory_calls.load(std::memory_order_relaxed) == 0);
    CHECK(operation_calls.load(std::memory_order_relaxed) == 0);

    for (const size_t count : std::array<size_t, 2>{0, 3}) {
        bool caught = false;
        try {
            (void)execute_fixed_slots<MoveOnlySlot>(
                count, 0, FixedSlotFailurePolicy::cancel_remaining, factory, operation);
        } catch (const std::invalid_argument&) {
            caught = true;
        } catch (...) {
            CHECK(false);
        }
        CHECK(caught);
    }
    CHECK(factory_calls.load(std::memory_order_relaxed) == 0);
    CHECK(operation_calls.load(std::memory_order_relaxed) == 0);

    bool invalid_policy_caught = false;
    try {
        (void)execute_fixed_slots<MoveOnlySlot>(3, 2, static_cast<FixedSlotFailurePolicy>(0xff),
                                                factory, operation);
    } catch (const std::invalid_argument&) {
        invalid_policy_caught = true;
    } catch (...) {
        CHECK(false);
    }
    CHECK(invalid_policy_caught);
    CHECK(factory_calls.load(std::memory_order_relaxed) == 0);
    CHECK(operation_calls.load(std::memory_order_relaxed) == 0);

    const auto clamped = execute_fixed_slots<MoveOnlySlot>(
        3, 10, FixedSlotFailurePolicy::cancel_remaining, factory, operation);
    CHECK(clamped.stats.requested_workers == 10);
    CHECK(clamped.stats.resolved_workers == 3);
    CHECK(clamped.stats.peak_workers == 3);
    CHECK(clamped.slots.size() == 3);
    CHECK(factory_calls.load(std::memory_order_relaxed) == 3);
    CHECK(operation_calls.load(std::memory_order_relaxed) == 3);
}

void test_factory_failure_starts_no_slots() {
    auto lifetime = std::make_shared<WorkerLifetime>();
    std::atomic_uint factory_calls{0};
    std::atomic_uint operation_calls{0};
    bool caught = false;
    try {
        (void)execute_fixed_slots<MoveOnlySlot>(
            8, 4, FixedSlotFailurePolicy::cancel_remaining,
            [&](FixedSlotPartition partition) {
                factory_calls.fetch_add(1, std::memory_order_relaxed);
                if (partition.worker_ordinal == 2) {
                    throw std::runtime_error("factory failure at worker 2");
                }
                return std::make_unique<NonMovableWorker>(lifetime, partition);
            },
            [&](std::unique_ptr<NonMovableWorker>& worker, size_t index) {
                operation_calls.fetch_add(1, std::memory_order_relaxed);
                return MoveOnlySlot(index, worker->partition().worker_ordinal);
            });
    } catch (const std::runtime_error& error) {
        caught = true;
        CHECK(std::string_view(error.what()) == "factory failure at worker 2");
    } catch (...) {
        CHECK(false);
    }

    CHECK(caught);
    CHECK(factory_calls.load(std::memory_order_relaxed) == 3);
    CHECK(operation_calls.load(std::memory_order_relaxed) == 0);
    CHECK(lifetime->constructed.load(std::memory_order_relaxed) == 2);
    CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 2);
    CHECK(lifetime->live.load(std::memory_order_relaxed) == 0);
}

void test_partial_thread_launch_failure_starts_no_slots() {
    auto lifetime = std::make_shared<WorkerLifetime>();
    std::atomic_size_t started_threads{0};
    std::atomic_size_t exited_threads{0};
    std::atomic_size_t live_threads{0};
    std::atomic_uint operation_calls{0};
    size_t launch_attempts = 0;
    bool caught = false;
    try {
        (void)
            gnfs::util::fixed_slot_executor_detail::execute_fixed_slots_with_launcher<MoveOnlySlot>(
                8, 4, FixedSlotFailurePolicy::cancel_remaining,
                [&](const FixedSlotPartition& partition) {
                    return std::make_unique<NonMovableWorker>(lifetime, partition);
                },
                [&](std::unique_ptr<NonMovableWorker>& worker, size_t index) {
                    operation_calls.fetch_add(1, std::memory_order_relaxed);
                    return MoveOnlySlot(index, worker->partition().worker_ordinal);
                },
                [&](auto&& task) -> gnfs::util::JoiningThread {
                    ++launch_attempts;
                    if (launch_attempts == 3) {
                        throw std::runtime_error("thread launch failure at worker 2");
                    }
                    return gnfs::util::JoiningThread(
                        [owned_task = std::forward<decltype(task)>(task), &started_threads,
                         &exited_threads, &live_threads]() mutable {
                            started_threads.fetch_add(1, std::memory_order_relaxed);
                            ActiveGuard guard(live_threads);
                            owned_task();
                            exited_threads.fetch_add(1, std::memory_order_relaxed);
                        });
                });
    } catch (const std::runtime_error& error) {
        caught = true;
        CHECK(std::string_view(error.what()) == "thread launch failure at worker 2");
    } catch (...) {
        CHECK(false);
    }

    CHECK(caught);
    CHECK(launch_attempts == 3);
    CHECK(operation_calls.load(std::memory_order_relaxed) == 0);
    CHECK(started_threads.load(std::memory_order_relaxed) == 2);
    CHECK(exited_threads.load(std::memory_order_relaxed) == 2);
    CHECK(live_threads.load(std::memory_order_relaxed) == 0);
    CHECK(lifetime->constructed.load(std::memory_order_relaxed) == 4);
    CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 4);
    CHECK(lifetime->live.load(std::memory_order_relaxed) == 0);
}

void test_cancel_remaining_stops_unstarted_slots() {
    constexpr size_t slot_count = 8;
    constexpr uint32_t worker_count = 4;
    std::array<std::atomic_uint, slot_count> calls{};
    FirstSlotBarrier first_slot_barrier(worker_count);
    std::mutex failure_mutex;
    std::condition_variable failure_condition;
    bool failing_worker_stopped = false;
    size_t launch_ordinal = 0;
    bool caught = false;

    try {
        (void)
            gnfs::util::fixed_slot_executor_detail::execute_fixed_slots_with_launcher<MoveOnlySlot>(
                slot_count, worker_count, FixedSlotFailurePolicy::cancel_remaining,
                [](const FixedSlotPartition& partition) { return partition; },
                [&](FixedSlotPartition& partition, size_t index) {
                    calls[index].fetch_add(1, std::memory_order_relaxed);
                    if (index == partition.begin) {
                        first_slot_barrier.arrive_and_finish_in_reverse(partition.worker_ordinal);
                        if (partition.worker_ordinal == 0) {
                            throw std::runtime_error("cancel remaining at slot 0");
                        }
                        std::unique_lock lock(failure_mutex);
                        failure_condition.wait(lock, [&] { return failing_worker_stopped; });
                    }
                    return MoveOnlySlot(index, partition.worker_ordinal);
                },
                [&](auto&& task) -> gnfs::util::JoiningThread {
                    const size_t worker = launch_ordinal++;
                    return gnfs::util::JoiningThread(
                        [owned_task = std::forward<decltype(task)>(task), worker, &failure_mutex,
                         &failure_condition, &failing_worker_stopped]() mutable {
                            owned_task();
                            if (worker == 0) {
                                {
                                    std::lock_guard lock(failure_mutex);
                                    failing_worker_stopped = true;
                                }
                                failure_condition.notify_all();
                            }
                        });
                });
    } catch (const std::runtime_error& error) {
        caught = true;
        CHECK(std::string_view(error.what()) == "cancel remaining at slot 0");
    } catch (...) {
        CHECK(false);
    }

    CHECK(caught);
    CHECK(launch_ordinal == worker_count);
    CHECK(first_slot_barrier.max_active() == worker_count);
    for (size_t index = 0; index < slot_count; ++index) {
        const bool is_partition_first = index % 2 == 0;
        CHECK(calls[index].load(std::memory_order_relaxed) == (is_partition_first ? 1U : 0U));
    }
}

void test_slot_failures_drain_before_lowest_index_rethrow() {
    constexpr size_t slot_count = 8;
    auto lifetime = std::make_shared<WorkerLifetime>();
    auto result_lifetime = std::make_shared<ResultLifetime>();
    std::array<std::atomic_uint, slot_count> calls{};
    std::atomic_size_t active{0};
    size_t active_at_catch = std::numeric_limits<size_t>::max();
    bool caught = false;

    try {
        (void)execute_fixed_slots<CountedSlot>(
            slot_count, 4, FixedSlotFailurePolicy::drain_all,
            [&](FixedSlotPartition partition) {
                return std::make_unique<NonMovableWorker>(lifetime, partition);
            },
            [&](std::unique_ptr<NonMovableWorker>&, size_t index) {
                ActiveGuard guard(active);
                calls[index].fetch_add(1, std::memory_order_relaxed);
                if (index == 1) {
                    throw std::runtime_error("slot failure at index 1");
                }
                if (index == 6) {
                    throw std::runtime_error("slot failure at index 6");
                }
                return CountedSlot(result_lifetime, index);
            });
    } catch (const std::runtime_error& error) {
        caught = true;
        active_at_catch = active.load(std::memory_order_relaxed);
        CHECK(std::string_view(error.what()) == "slot failure at index 1");
    } catch (...) {
        CHECK(false);
    }

    CHECK(caught);
    for (const auto& call_count : calls) {
        CHECK(call_count.load(std::memory_order_relaxed) == 1);
    }
    CHECK(active_at_catch == 0);
    CHECK(active.load(std::memory_order_relaxed) == 0);
    CHECK(lifetime->constructed.load(std::memory_order_relaxed) == 4);
    CHECK(lifetime->destroyed.load(std::memory_order_relaxed) == 4);
    CHECK(lifetime->live.load(std::memory_order_relaxed) == 0);
    CHECK(result_lifetime->constructed.load(std::memory_order_relaxed) == slot_count - 2);
    CHECK(result_lifetime->destroyed.load(std::memory_order_relaxed) == slot_count - 2);
    CHECK(result_lifetime->live.load(std::memory_order_relaxed) == 0);
}

} // namespace

int main() {
    test_static_partitions_and_canonical_results();
    test_empty_invalid_and_clamped_workers();
    test_factory_failure_starts_no_slots();
    test_partial_thread_launch_failure_starts_no_slots();
    test_cancel_remaining_stops_unstarted_slots();
    test_slot_failures_drain_before_lowest_index_rethrow();

    std::cout << "Fixed-slot executor: " << checks_passed << " checks passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
