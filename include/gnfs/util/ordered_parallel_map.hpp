#pragma once

#include "gnfs/util/thread_pool.hpp"

#include <algorithm>
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

} // namespace gnfs::util
