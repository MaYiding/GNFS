// test_joining_thread.cpp - portable joining-thread ownership contracts

#include <gnfs/util/joining_thread.hpp>

#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {

using gnfs::util::JoiningThread;

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

} // namespace

int main() {
    test_default_state_and_explicit_join();
    test_scope_exit_joins();
    test_move_construction_transfers_join_ownership();
    test_move_assignment_joins_old_thread_and_transfers_ownership();
    test_empty_and_self_move_assignment();
    test_stack_unwind_joins();
    test_join_and_constructor_exceptions_propagate();

    std::cout << "JoiningThread: " << checks_passed << " checks passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
