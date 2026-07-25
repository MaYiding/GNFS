#pragma once

/// @file fixed_slot_executor.hpp
/// @brief Static-partition parallel execution with canonical slot results.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::util {

using std::size_t;

struct FixedSlotPartition {
    size_t worker_ordinal;
    size_t begin;
    size_t end;

    [[nodiscard]] friend constexpr bool operator==(const FixedSlotPartition&,
                                                   const FixedSlotPartition&) = default;
};

struct FixedSlotExecutionStats {
    size_t slot_count = 0;
    uint32_t requested_workers = 0;
    uint32_t resolved_workers = 0;
    uint32_t peak_workers = 0;

    [[nodiscard]] friend constexpr bool operator==(const FixedSlotExecutionStats&,
                                                   const FixedSlotExecutionStats&) = default;
};

template <class Result> struct FixedSlotExecution {
    std::vector<Result> slots;
    FixedSlotExecutionStats stats;
};

enum class FixedSlotFailurePolicy : uint8_t {
    cancel_remaining,
    drain_all,
};

namespace fixed_slot_executor_detail {

class LaunchGate final {
public:
    explicit LaunchGate(size_t expected) : expected_(expected) {}

    [[nodiscard]] bool worker_arrive_and_wait() {
        std::unique_lock lock(mutex_);
        ++arrived_;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_ || cancelled_; });
        return released_ && !cancelled_;
    }

    [[nodiscard]] bool release_when_ready() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return arrived_ == expected_ || cancelled_; });
        if (cancelled_) {
            return false;
        }
        peak_workers_ = arrived_;
        released_ = true;
        condition_.notify_all();
        return true;
    }

    void cancel() {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] size_t peak_workers() const {
        std::lock_guard lock(mutex_);
        return peak_workers_;
    }

private:
    size_t expected_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    size_t arrived_ = 0;
    size_t peak_workers_ = 0;
    bool released_ = false;
    bool cancelled_ = false;
};

[[nodiscard]] inline std::vector<FixedSlotPartition> make_partitions(size_t slot_count,
                                                                     size_t worker_count) {
    std::vector<FixedSlotPartition> partitions;
    partitions.reserve(worker_count);
    const size_t base_slots = slot_count / worker_count;
    const size_t extra_slots = slot_count % worker_count;
    for (size_t worker = 0; worker < worker_count; ++worker) {
        const size_t begin = worker * base_slots + std::min(worker, extra_slots);
        const size_t count = base_slots + (worker < extra_slots ? size_t{1} : size_t{0});
        partitions.push_back(FixedSlotPartition{worker, begin, begin + count});
    }
    return partitions;
}

template <class Result, class WorkerFactory, class Operation, class ThreadLauncher>
[[nodiscard]] FixedSlotExecution<Result> execute_fixed_slots_with_launcher(
    size_t slot_count, uint32_t requested_workers, FixedSlotFailurePolicy failure_policy,
    WorkerFactory&& worker_factory, Operation&& operation, ThreadLauncher&& thread_launcher) {
    static_assert(!std::is_void_v<Result>);
    static_assert(std::is_move_constructible_v<Result>);
    if (requested_workers == 0) {
        throw std::invalid_argument("execute_fixed_slots requires at least one worker");
    }
    if (failure_policy != FixedSlotFailurePolicy::cancel_remaining &&
        failure_policy != FixedSlotFailurePolicy::drain_all) {
        throw std::invalid_argument("execute_fixed_slots received an invalid failure policy");
    }

    FixedSlotExecutionStats stats;
    stats.slot_count = slot_count;
    stats.requested_workers = requested_workers;
    if (slot_count == 0) {
        return FixedSlotExecution<Result>{{}, stats};
    }

    const size_t worker_count = std::min(slot_count, static_cast<size_t>(requested_workers));
    if (worker_count > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::overflow_error("resolved fixed-slot worker count exceeds uint32");
    }
    stats.resolved_workers = static_cast<uint32_t>(worker_count);

    const auto partitions = make_partitions(slot_count, worker_count);
    using WorkerState =
        std::remove_cvref_t<std::invoke_result_t<WorkerFactory&, const FixedSlotPartition&>>;
    static_assert(!std::is_void_v<WorkerState>);
    static_assert(std::is_move_constructible_v<WorkerState>);

    // Complete all potentially throwing state construction before a worker can
    // observe a slot. A setup failure therefore starts zero operations.
    std::vector<WorkerState> worker_states;
    worker_states.reserve(worker_count);
    for (const FixedSlotPartition& partition : partitions) {
        worker_states.emplace_back(std::invoke(worker_factory, partition));
    }

    std::vector<std::optional<Result>> slots(slot_count);
    std::vector<std::exception_ptr> slot_errors(slot_count);
    std::vector<std::exception_ptr> worker_errors(worker_count);
    constexpr size_t no_failed_slot = std::numeric_limits<size_t>::max();
    std::atomic_size_t first_failure_slot{no_failed_slot};
    std::atomic_bool infrastructure_stop{false};
    LaunchGate launch_gate(worker_count);
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);

    try {
        for (size_t worker = 0; worker < worker_count; ++worker) {
            auto task = [&, worker] {
                try {
                    if (!launch_gate.worker_arrive_and_wait()) {
                        return;
                    }
                    const FixedSlotPartition partition = partitions[worker];
                    for (size_t slot = partition.begin; slot < partition.end; ++slot) {
                        if (infrastructure_stop.load(std::memory_order_acquire) ||
                            (failure_policy == FixedSlotFailurePolicy::cancel_remaining &&
                             first_failure_slot.load(std::memory_order_acquire) !=
                                 no_failed_slot)) {
                            break;
                        }
                        try {
                            slots[slot].emplace(
                                std::invoke(operation, worker_states[worker], slot));
                        } catch (...) {
                            slot_errors[slot] = std::current_exception();
                            if (failure_policy == FixedSlotFailurePolicy::cancel_remaining) {
                                size_t expected = no_failed_slot;
                                (void)first_failure_slot.compare_exchange_strong(
                                    expected, slot, std::memory_order_release,
                                    std::memory_order_relaxed);
                                break;
                            }
                        }
                    }
                } catch (...) {
                    worker_errors[worker] = std::current_exception();
                    infrastructure_stop.store(true, std::memory_order_release);
                    try {
                        launch_gate.cancel();
                    } catch (...) {
                    }
                }
            };
            workers.emplace_back(std::invoke(thread_launcher, std::move(task)));
        }
        if (!launch_gate.release_when_ready()) {
            workers.clear();
            for (const auto& error : worker_errors) {
                if (error) {
                    std::rethrow_exception(error);
                }
            }
            throw std::runtime_error("fixed-slot launch gate cancelled without an error");
        }
    } catch (...) {
        const std::exception_ptr launch_error = std::current_exception();
        try {
            launch_gate.cancel();
        } catch (...) {
        }
        workers.clear();
        std::rethrow_exception(launch_error);
    }

    // jthread destruction joins every worker before any outcome is inspected.
    workers.clear();
    stats.peak_workers = static_cast<uint32_t>(launch_gate.peak_workers());
    if (stats.peak_workers != stats.resolved_workers) {
        throw std::runtime_error("fixed-slot workers did not all reach the launch gate");
    }

    for (const auto& error : worker_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    if (failure_policy == FixedSlotFailurePolicy::cancel_remaining) {
        const size_t failed_slot = first_failure_slot.load(std::memory_order_acquire);
        if (failed_slot != no_failed_slot) {
            std::rethrow_exception(slot_errors[failed_slot]);
        }
    }
    for (const auto& error : slot_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    std::vector<Result> ordered;
    ordered.reserve(slot_count);
    for (auto& slot : slots) {
        if (!slot) {
            throw std::runtime_error("fixed-slot execution completed without a slot result");
        }
        ordered.push_back(std::move(*slot));
    }
    return FixedSlotExecution<Result>{std::move(ordered), stats};
}

} // namespace fixed_slot_executor_detail

/// Execute one operation for every canonical slot using static partitions.
///
/// `worker_factory(partition)` runs serially on the caller thread before any
/// operation starts. It must return move-constructible worker-local state.
/// Wrap non-movable state in a move-only owning handle such as
/// `std::unique_ptr`.
///
/// Every resolved worker waits at a launch gate, then processes only its
/// contiguous partition in ascending slot order. Under `cancel_remaining`, the
/// first published operation failure asks every worker to stop before its next
/// slot; already-running operations finish, and that first published failure
/// is rethrown. Under `drain_all`, all slots still run so the lowest-index
/// failure is canonical. Both policies join every worker and discard every
/// partial result before rethrowing. Successful results are returned in
/// canonical slot order, independent of completion order. `operation` may be
/// invoked concurrently and must use only its supplied worker state for mutable
/// worker-local data.
///
/// A positive request is required. Empty input resolves no workers. Otherwise
/// the worker count is clamped to the slot count.
template <class Result, class WorkerFactory, class Operation>
[[nodiscard]] FixedSlotExecution<Result>
execute_fixed_slots(size_t slot_count, uint32_t requested_workers,
                    FixedSlotFailurePolicy failure_policy, WorkerFactory&& worker_factory,
                    Operation&& operation) {
    const auto thread_launcher = [](auto&& task) {
        return std::jthread(std::forward<decltype(task)>(task));
    };
    return fixed_slot_executor_detail::execute_fixed_slots_with_launcher<Result>(
        slot_count, requested_workers, failure_policy, std::forward<WorkerFactory>(worker_factory),
        std::forward<Operation>(operation), thread_launcher);
}

} // namespace gnfs::util
