#pragma once

/// @file joined_worker_group.hpp
/// @brief Indexed worker launch with all-or-none release and deterministic errors.

#include "joining_thread.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::util {

using std::size_t;

namespace joined_worker_group_detail {

enum class LaunchGateState : std::uint8_t {
    launching,
    released,
    cancelled,
};

/// A no-allocation launch gate whose cancellation path is noexcept.
class LaunchGate final {
public:
    explicit LaunchGate(size_t expected_workers) noexcept : expected_workers_(expected_workers) {}

    [[nodiscard]] bool worker_arrive_and_wait() noexcept {
        arrived_workers_.fetch_add(1, std::memory_order_release);
        arrived_workers_.notify_one();

        LaunchGateState state = state_.load(std::memory_order_acquire);
        while (state == LaunchGateState::launching) {
            state_.wait(LaunchGateState::launching, std::memory_order_acquire);
            state = state_.load(std::memory_order_acquire);
        }
        return state == LaunchGateState::released;
    }

    void release_when_ready() noexcept {
        size_t arrived = arrived_workers_.load(std::memory_order_acquire);
        while (arrived != expected_workers_) {
            arrived_workers_.wait(arrived, std::memory_order_acquire);
            arrived = arrived_workers_.load(std::memory_order_acquire);
        }
        state_.store(LaunchGateState::released, std::memory_order_release);
        state_.notify_all();
    }

    void cancel() noexcept {
        state_.store(LaunchGateState::cancelled, std::memory_order_release);
        state_.notify_all();
    }

private:
    size_t expected_workers_;
    std::atomic_size_t arrived_workers_{0};
    std::atomic<LaunchGateState> state_{LaunchGateState::launching};
};

/// Test seam for injecting indexed thread-launch behavior.
///
/// `thread_launcher(ordinal, task)` must return a joinable `JoiningThread` that
/// invokes `task` exactly once. The launcher is called serially on the caller
/// thread. Production code should use `run_joined_worker_group` below.
template <class Worker, class ThreadLauncher>
void run_joined_worker_group_with_launcher(size_t worker_count, Worker&& worker,
                                           ThreadLauncher&& thread_launcher) {
    static_assert(std::is_invocable_v<Worker&, size_t>);
    if (worker_count == 0) {
        return;
    }

    // Finish every allocation and potentially throwing synchronization-state
    // construction before the first physical worker can be launched.
    std::vector<std::exception_ptr> worker_errors(worker_count);
    std::vector<JoiningThread> workers;
    workers.reserve(worker_count);
    LaunchGate launch_gate(worker_count);

    try {
        for (size_t worker_ordinal = 0; worker_ordinal < worker_count; ++worker_ordinal) {
            auto task = [&, worker_ordinal]() noexcept {
                if (!launch_gate.worker_arrive_and_wait()) {
                    return;
                }
                try {
                    std::invoke(worker, worker_ordinal);
                } catch (...) {
                    worker_errors[worker_ordinal] = std::current_exception();
                }
            };

            JoiningThread launched = std::invoke(thread_launcher, worker_ordinal, std::move(task));
            if (!launched.joinable()) {
                throw std::logic_error("joined worker launcher returned a non-joinable thread");
            }
            workers.push_back(std::move(launched));
        }
    } catch (...) {
        const std::exception_ptr launch_error = std::current_exception();
        launch_gate.cancel();
        workers.clear();
        std::rethrow_exception(launch_error);
    }

    // No worker body can pass the gate until every thread object exists.
    launch_gate.release_when_ready();

    // JoiningThread destruction joins every worker before error inspection.
    workers.clear();
    for (const auto& error : worker_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
}

} // namespace joined_worker_group_detail

/// Run an indexed set of physical workers and join all of them before return.
///
/// `worker(ordinal)` may be invoked concurrently and must be safe for distinct
/// ordinals. Worker bodies are released only after all `worker_count` threads
/// have been created and reached the launch gate. A partial thread-launch
/// failure releases and joins the already-created threads without invoking a
/// worker body, then preserves the launcher exception. Worker exceptions are
/// captured at the noexcept thread boundary; after every worker is joined, the
/// exception from the lowest failing ordinal is rethrown. A zero worker count
/// is a no-op.
template <class Worker> void run_joined_worker_group(size_t worker_count, Worker&& worker) {
    const auto thread_launcher = [](size_t, auto&& task) {
        return JoiningThread(std::forward<decltype(task)>(task));
    };
    joined_worker_group_detail::run_joined_worker_group_with_launcher(
        worker_count, std::forward<Worker>(worker), thread_launcher);
}

} // namespace gnfs::util
