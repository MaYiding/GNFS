#pragma once

#include "gnfs/util/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::util {

/// Evaluate `function(index)` for every index and return results in index
/// order. `function` may run concurrently and therefore must be safe for
/// simultaneous calls over distinct indices.
///
/// All successfully submitted futures are drained before an exception is
/// rethrown. If multiple indexed calls fail, the exception from the lowest
/// index wins regardless of worker completion order. The `max_threads == 1`
/// path preserves the same attempt-all and lowest-index error contract without
/// constructing a ThreadPool.
///
/// `Result` may be move-only and need not be default-constructible. Void
/// results are intentionally unsupported because callers of this helper need
/// an explicit ordered outcome for every successful index.
template <typename Result, typename Function>
[[nodiscard]] std::vector<Result> ordered_parallel_map(size_t count, uint32_t max_threads,
                                                       Function&& function) {
    static_assert(!std::is_void_v<Result>);
    static_assert(std::is_move_constructible_v<Result>);
    if (max_threads == 0) {
        throw std::invalid_argument("ordered_parallel_map requires at least one thread");
    }

    std::vector<std::optional<Result>> slots(count);
    std::vector<std::exception_ptr> errors(count);

    auto invoke = [&](size_t index) noexcept {
        try {
            slots[index].emplace(std::invoke(function, index));
        } catch (...) {
            errors[index] = std::current_exception();
        }
    };

    if (max_threads == 1 || count <= 1) {
        for (size_t index = 0; index < count; ++index) {
            invoke(index);
        }
    } else {
        const size_t bounded_threads = std::min<size_t>(max_threads, count);
        ThreadPool pool(static_cast<uint32_t>(bounded_threads));
        std::vector<std::future<void>> futures;
        futures.reserve(count);

        std::exception_ptr submission_error;
        for (size_t index = 0; index < count; ++index) {
            try {
                futures.push_back(pool.submit([&, index] { invoke(index); }));
            } catch (...) {
                submission_error = std::current_exception();
                break;
            }
        }

        // invoke() captures indexed task failures, so get() is expected not to
        // throw. Still drain defensively in case the worker wrapper itself ever
        // gains a throwing operation.
        for (size_t index = 0; index < futures.size(); ++index) {
            try {
                futures[index].get();
            } catch (...) {
                if (!errors[index]) {
                    errors[index] = std::current_exception();
                }
            }
        }

        // A lower indexed task failure has deterministic precedence over the
        // failure to submit the next index. No later index was started.
        if (submission_error) {
            for (size_t index = 0; index < futures.size(); ++index) {
                if (errors[index]) {
                    std::rethrow_exception(errors[index]);
                }
            }
            std::rethrow_exception(submission_error);
        }
    }

    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    std::vector<Result> results;
    results.reserve(count);
    for (auto& slot : slots) {
        // Every missing slot has a corresponding error handled above.
        if (!slot) {
            throw std::logic_error("ordered_parallel_map completed without an indexed outcome");
        }
        results.push_back(std::move(*slot));
    }
    return results;
}

/// Evaluate `operation(worker_state, index)` for every canonical index using
/// dynamic atomic work claiming, then return results in index order.
///
/// `worker_factory(worker_ordinal)` runs sequentially on the caller thread
/// before any operation starts. It must return a move-constructible state that
/// is used by exactly one logical worker. This makes expensive or mutable
/// worker-local state safe to reuse across dynamically claimed items. A setup
/// failure therefore starts no operations and propagates directly.
///
/// Operation failures are captured per canonical index. Every worker keeps
/// claiming work, all worker threads are joined, and stack unwinding destroys
/// every worker state before the lowest-index operation failure reaches the
/// caller. Scheduling and completion timing therefore cannot select the
/// visible error or reorder successful results.
///
/// `Result` and worker state may be move-only and need not be
/// default-constructible. Void results are intentionally unsupported because
/// callers require an explicit outcome for every successful index.
template <typename Result, typename WorkerFactory, typename Operation>
[[nodiscard]] std::vector<Result> ordered_work_stealing_map(size_t count, uint32_t max_workers,
                                                            WorkerFactory&& worker_factory,
                                                            Operation&& operation) {
    static_assert(!std::is_void_v<Result>);
    static_assert(std::is_move_constructible_v<Result>);
    if (max_workers == 0) {
        throw std::invalid_argument("ordered_work_stealing_map requires at least one worker");
    }
    if (count == 0) {
        return {};
    }

    using WorkerState = std::remove_cvref_t<std::invoke_result_t<WorkerFactory&, size_t>>;
    static_assert(!std::is_void_v<WorkerState>);
    static_assert(std::is_move_constructible_v<WorkerState>);

    const size_t active_workers_wide = std::min<size_t>(max_workers, count);
    const uint32_t active_workers = static_cast<uint32_t>(active_workers_wide);

    // Construct every worker state before scheduling. Besides making setup
    // failure deterministic, this guarantees no operation observes a partial
    // worker-state set.
    std::vector<WorkerState> worker_states;
    worker_states.reserve(active_workers_wide);
    for (size_t worker_ordinal = 0; worker_ordinal < active_workers_wide; ++worker_ordinal) {
        worker_states.emplace_back(std::invoke(worker_factory, worker_ordinal));
    }

    std::vector<std::optional<Result>> slots(count);
    std::vector<std::exception_ptr> errors(count);
    std::atomic<size_t> next_index{0};

    const auto worker_summaries = ordered_parallel_map<size_t>(
        active_workers_wide, active_workers, [&](size_t worker_ordinal) {
            size_t processed = 0;
            while (true) {
                const size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= count) {
                    break;
                }
                try {
                    slots[index].emplace(
                        std::invoke(operation, worker_states[worker_ordinal], index));
                } catch (...) {
                    errors[index] = std::current_exception();
                }
                ++processed;
            }
            return processed;
        });

    size_t processed_total = 0;
    for (const size_t processed : worker_summaries) {
        processed_total += processed;
    }
    if (processed_total != count) {
        throw std::logic_error("ordered_work_stealing_map did not claim every index");
    }

    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    std::vector<Result> results;
    results.reserve(count);
    for (auto& slot : slots) {
        if (!slot) {
            throw std::logic_error(
                "ordered_work_stealing_map completed without an indexed outcome");
        }
        results.push_back(std::move(*slot));
    }
    return results;
}

} // namespace gnfs::util
