// test_joining_thread.cpp - portable joining-thread ownership contracts

#include <gnfs/util/joined_worker_group.hpp>
#include <gnfs/util/joining_thread.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using gnfs::util::JoiningThread;
using gnfs::util::run_joined_worker_group;
using std::size_t;

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

static_assert(std::is_nothrow_default_constructible_v<JoiningThread>);
static_assert(!std::is_copy_constructible_v<JoiningThread>);
static_assert(!std::is_copy_assignable_v<JoiningThread>);
static_assert(std::is_nothrow_move_constructible_v<JoiningThread>);
static_assert(std::is_nothrow_move_assignable_v<JoiningThread>);

class ThrowOnMoveCallable final {
public:
    explicit ThrowOnMoveCallable(std::atomic_bool& invoked) noexcept : invoked_(&invoked) {}

    ThrowOnMoveCallable(const ThrowOnMoveCallable&) = delete;
    ThrowOnMoveCallable& operator=(const ThrowOnMoveCallable&) = delete;

    ThrowOnMoveCallable(ThrowOnMoveCallable&& other) : invoked_(other.invoked_) {
        throw std::runtime_error("injected callable move failure");
    }

    void operator()() const noexcept {
        invoked_->store(true, std::memory_order_relaxed);
    }

private:
    std::atomic_bool* invoked_;
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

void test_default_state_and_explicit_join() {
    JoiningThread empty;
    CHECK(!empty.joinable());

    std::atomic_int value{0};
    JoiningThread worker(
        [](std::atomic_int* output, int input) noexcept {
            output->store(input, std::memory_order_relaxed);
        },
        &value, 17);
    CHECK(worker.joinable());
    worker.join();
    CHECK(!worker.joinable());
    CHECK(value.load(std::memory_order_relaxed) == 17);
}

void test_scope_exit_joins() {
    std::promise<void> started_promise;
    std::future<void> started = started_promise.get_future();
    std::promise<void> release_promise;
    std::shared_future<void> release = release_promise.get_future().share();
    std::atomic_bool finished{false};

    {
        JoiningThread worker([&] {
            started_promise.set_value();
            release.wait();
            finished.store(true, std::memory_order_release);
        });
        started.wait();
        CHECK(worker.joinable());
        CHECK(!finished.load(std::memory_order_acquire));
        release_promise.set_value();
    }

    CHECK(finished.load(std::memory_order_acquire));
}

void test_move_construction_transfers_join_ownership() {
    std::promise<void> started_promise;
    std::future<void> started = started_promise.get_future();
    std::promise<void> release_promise;
    std::shared_future<void> release = release_promise.get_future().share();
    std::atomic_bool finished{false};

    JoiningThread source([&] {
        started_promise.set_value();
        release.wait();
        finished.store(true, std::memory_order_release);
    });
    started.wait();

    JoiningThread destination(std::move(source));
    CHECK(!source.joinable());
    CHECK(destination.joinable());
    release_promise.set_value();
    destination.join();
    CHECK(finished.load(std::memory_order_acquire));
}

void test_move_assignment_joins_old_thread_and_transfers_ownership() {
    std::promise<void> old_started_promise;
    std::future<void> old_started = old_started_promise.get_future();
    std::promise<void> old_release_promise;
    std::shared_future<void> old_release = old_release_promise.get_future().share();
    std::atomic_bool old_finished{false};

    std::promise<void> new_started_promise;
    std::future<void> new_started = new_started_promise.get_future();
    std::promise<void> new_release_promise;
    std::shared_future<void> new_release = new_release_promise.get_future().share();
    std::atomic_bool new_finished{false};

    JoiningThread destination([&] {
        old_started_promise.set_value();
        old_release.wait();
        old_finished.store(true, std::memory_order_release);
    });
    JoiningThread source([&] {
        new_started_promise.set_value();
        new_release.wait();
        new_finished.store(true, std::memory_order_release);
    });
    old_started.wait();
    new_started.wait();
    CHECK(!old_finished.load(std::memory_order_acquire));
    CHECK(!new_finished.load(std::memory_order_acquire));

    old_release_promise.set_value();
    destination = std::move(source);
    CHECK(old_finished.load(std::memory_order_acquire));
    CHECK(!source.joinable());
    CHECK(destination.joinable());

    new_release_promise.set_value();
    destination.join();
    CHECK(new_finished.load(std::memory_order_acquire));
}

void test_empty_and_self_move_assignment() {
    std::atomic_int transferred_value{0};
    JoiningThread source(
        [](std::atomic_int* output) noexcept { output->store(23, std::memory_order_release); },
        &transferred_value);
    JoiningThread destination;
    destination = std::move(source);
    CHECK(!source.joinable());
    CHECK(destination.joinable());
    destination.join();
    CHECK(transferred_value.load(std::memory_order_acquire) == 23);

    std::promise<void> started_promise;
    std::future<void> started = started_promise.get_future();
    std::promise<void> release_promise;
    std::shared_future<void> release = release_promise.get_future().share();
    std::atomic_bool finished{false};
    JoiningThread joinable([&] {
        started_promise.set_value();
        release.wait();
        finished.store(true, std::memory_order_release);
    });
    started.wait();

    JoiningThread empty;
    release_promise.set_value();
    joinable = std::move(empty);
    CHECK(finished.load(std::memory_order_acquire));
    CHECK(!joinable.joinable());
    CHECK(!empty.joinable());

    JoiningThread self;
    JoiningThread* self_alias = &self;
    self = std::move(*self_alias);
    CHECK(!self.joinable());
}

void test_stack_unwind_joins() {
    std::atomic_bool finished{false};
    bool caught = false;
    try {
        std::promise<void> started_promise;
        std::future<void> started = started_promise.get_future();
        std::promise<void> release_promise;
        std::shared_future<void> release = release_promise.get_future().share();
        JoiningThread worker([&] {
            started_promise.set_value();
            release.wait();
            finished.store(true, std::memory_order_release);
        });
        started.wait();
        release_promise.set_value();
        throw std::runtime_error("injected stack unwind");
    } catch (const std::runtime_error& error) {
        caught = true;
        CHECK(std::string_view(error.what()) == "injected stack unwind");
    } catch (...) {
        CHECK(false);
    }
    CHECK(caught);
    CHECK(finished.load(std::memory_order_acquire));
}

void test_join_and_constructor_exceptions_propagate() {
    JoiningThread empty;
    bool caught_join_error = false;
    try {
        empty.join();
    } catch (const std::system_error&) {
        caught_join_error = true;
    } catch (...) {
        CHECK(false);
    }
    CHECK(caught_join_error);

    std::atomic_bool invoked{false};
    bool caught_move_error = false;
    try {
        JoiningThread worker{ThrowOnMoveCallable(invoked)};
        (void)worker;
    } catch (const std::runtime_error& error) {
        caught_move_error = true;
        CHECK(std::string_view(error.what()) == "injected callable move failure");
    } catch (...) {
        CHECK(false);
    }
    CHECK(caught_move_error);
    CHECK(!invoked.load(std::memory_order_relaxed));
}

void test_joined_worker_group_zero_is_noop() {
    std::atomic_size_t calls{0};
    run_joined_worker_group(
        0, [&](size_t) noexcept { calls.fetch_add(1, std::memory_order_relaxed); });
    CHECK(calls.load(std::memory_order_relaxed) == 0);
}

void test_joined_worker_group_releases_only_after_full_launch() {
    constexpr size_t worker_count = 4;
    std::atomic_size_t launch_attempts{0};
    std::array<std::atomic_uint, worker_count> calls{};
    std::atomic_bool body_started_before_full_launch{false};

    gnfs::util::joined_worker_group_detail::run_joined_worker_group_with_launcher(
        worker_count,
        [&](size_t worker_ordinal) {
            if (launch_attempts.load(std::memory_order_acquire) != worker_count) {
                body_started_before_full_launch.store(true, std::memory_order_relaxed);
            }
            calls[worker_ordinal].fetch_add(1, std::memory_order_relaxed);
        },
        [&](size_t worker_ordinal, auto&& task) {
            CHECK(worker_ordinal == launch_attempts.load(std::memory_order_relaxed));
            launch_attempts.fetch_add(1, std::memory_order_release);
            return JoiningThread(std::forward<decltype(task)>(task));
        });

    CHECK(launch_attempts.load(std::memory_order_relaxed) == worker_count);
    CHECK(!body_started_before_full_launch.load(std::memory_order_relaxed));
    for (const auto& call_count : calls) {
        CHECK(call_count.load(std::memory_order_relaxed) == 1);
    }
}

void test_joined_worker_group_partial_launch_failure_starts_no_bodies() {
    constexpr size_t worker_count = 5;
    constexpr size_t failing_ordinal = 2;
    const std::error_code expected_code =
        std::make_error_code(std::errc::resource_unavailable_try_again);
    std::atomic_size_t launched_threads{0};
    std::atomic_size_t exited_threads{0};
    std::atomic_size_t live_threads{0};
    std::atomic_size_t body_calls{0};
    size_t launch_attempts = 0;
    size_t live_at_catch = std::numeric_limits<size_t>::max();
    bool caught = false;

    try {
        gnfs::util::joined_worker_group_detail::run_joined_worker_group_with_launcher(
            worker_count,
            [&](size_t) noexcept { body_calls.fetch_add(1, std::memory_order_relaxed); },
            [&](size_t worker_ordinal, auto&& task) -> JoiningThread {
                ++launch_attempts;
                if (worker_ordinal == failing_ordinal) {
                    throw std::system_error(expected_code, "injected indexed launch failure");
                }
                return JoiningThread([owned_task = std::forward<decltype(task)>(task),
                                      &launched_threads, &exited_threads,
                                      &live_threads]() mutable noexcept {
                    launched_threads.fetch_add(1, std::memory_order_relaxed);
                    ActiveGuard guard(live_threads);
                    owned_task();
                    exited_threads.fetch_add(1, std::memory_order_relaxed);
                });
            });
    } catch (const std::system_error& error) {
        caught = true;
        live_at_catch = live_threads.load(std::memory_order_relaxed);
        CHECK(error.code() == expected_code);
        CHECK(std::string_view(error.what()).find("injected indexed launch failure") !=
              std::string_view::npos);
    } catch (...) {
        CHECK(false);
    }

    CHECK(caught);
    CHECK(launch_attempts == failing_ordinal + 1);
    CHECK(body_calls.load(std::memory_order_relaxed) == 0);
    CHECK(launched_threads.load(std::memory_order_relaxed) == failing_ordinal);
    CHECK(exited_threads.load(std::memory_order_relaxed) == failing_ordinal);
    CHECK(live_at_catch == 0);
    CHECK(live_threads.load(std::memory_order_relaxed) == 0);
}

void test_joined_worker_group_joins_all_and_rethrows_lowest_ordinal() {
    constexpr size_t worker_count = 4;
    std::array<std::atomic_uint, worker_count> calls{};
    std::atomic_size_t active_workers{0};
    std::promise<void> higher_failure_ready_promise;
    std::shared_future<void> higher_failure_ready =
        higher_failure_ready_promise.get_future().share();
    size_t active_at_catch = std::numeric_limits<size_t>::max();
    bool caught = false;

    try {
        run_joined_worker_group(worker_count, [&](size_t worker_ordinal) {
            ActiveGuard guard(active_workers);
            calls[worker_ordinal].fetch_add(1, std::memory_order_relaxed);
            if (worker_ordinal == 2) {
                higher_failure_ready_promise.set_value();
                throw std::runtime_error("worker failure at ordinal 2");
            }
            if (worker_ordinal == 0) {
                higher_failure_ready.wait();
                throw std::runtime_error("worker failure at ordinal 0");
            }
            higher_failure_ready.wait();
            std::this_thread::yield();
        });
    } catch (const std::runtime_error& error) {
        caught = true;
        active_at_catch = active_workers.load(std::memory_order_relaxed);
        CHECK(std::string_view(error.what()) == "worker failure at ordinal 0");
    } catch (...) {
        CHECK(false);
    }

    CHECK(caught);
    CHECK(active_at_catch == 0);
    CHECK(active_workers.load(std::memory_order_relaxed) == 0);
    for (const auto& call_count : calls) {
        CHECK(call_count.load(std::memory_order_relaxed) == 1);
    }
}

} // namespace

int main() {
    test_default_state_and_explicit_join();
    test_scope_exit_joins();
    test_move_construction_transfers_join_ownership();
    test_move_assignment_joins_old_thread_and_transfers_ownership();
    test_empty_and_self_move_assignment();
    test_stack_unwind_joins();
    test_join_and_constructor_exceptions_propagate();
    test_joined_worker_group_zero_is_noop();
    test_joined_worker_group_releases_only_after_full_launch();
    test_joined_worker_group_partial_launch_failure_starts_no_bodies();
    test_joined_worker_group_joins_all_and_rethrows_lowest_ordinal();

    std::cout << "JoiningThread: " << checks_passed << " checks passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
