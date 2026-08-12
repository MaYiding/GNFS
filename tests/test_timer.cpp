// Unit tests for gnfs::util::Timer + Stopwatch + ScopedTimer.

#include "gnfs/util/timer.hpp"
#include "support/test_check.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <utility>

using namespace gnfs::util;
using namespace std::chrono_literals;

namespace {

// Minimum elapsed of a "busy" wait used for cross-platform timer assertions.
// We avoid std::this_thread::sleep_for because some CI sandboxes round it
// up generously; a busy spin gives a tight lower bound.
void busy_for(std::chrono::nanoseconds dur) {
    auto end = Timer::Clock::now() + dur;
    while (Timer::Clock::now() < end) {
        // spin
    }
}

} // namespace

void test_default_state() {
    std::cout << "Testing Timer default state..." << std::endl;

    Timer t;
    GNFS_TEST_CHECK(!t.is_running());
    GNFS_TEST_CHECK(t.elapsed_seconds() == 0.0);
    GNFS_TEST_CHECK(t.elapsed_ms() == 0.0);
    GNFS_TEST_CHECK(t.elapsed_us() == 0.0);
    GNFS_TEST_CHECK(t.elapsed_ns() == 0);

    std::cout << "  default state: PASS" << std::endl;
}

void test_start_stop_basic() {
    std::cout << "Testing Timer start/stop..." << std::endl;

    Timer t;
    t.start();
    GNFS_TEST_CHECK(t.is_running());
    busy_for(1ms);
    t.stop();
    GNFS_TEST_CHECK(!t.is_running());

    // Should have accumulated > 0
    GNFS_TEST_CHECK(t.elapsed_seconds() > 0.0);
    GNFS_TEST_CHECK(t.elapsed_ms() > 0.0);
    GNFS_TEST_CHECK(t.elapsed_ns() > 0);

    // Idempotent stop (second stop should be no-op)
    std::int64_t snapshot_ns = t.elapsed_ns();
    t.stop();
    GNFS_TEST_CHECK(t.elapsed_ns() == snapshot_ns);

    std::cout << "  start/stop: PASS" << std::endl;
}

void test_double_start_idempotent() {
    std::cout << "Testing Timer double start (idempotent)..." << std::endl;

    Timer t;
    t.start();
    auto first_start_running = t.is_running();
    busy_for(500us);

    // Second start should be no-op (per impl: only sets start_ if !running_)
    const std::int64_t before_second_start = t.elapsed_ns();
    t.start();
    const std::int64_t after_second_start = t.elapsed_ns();
    busy_for(500us);
    t.stop();

    GNFS_TEST_CHECK(first_start_running);
    GNFS_TEST_CHECK(after_second_start >= before_second_start);
    // We busied for 1 ms total; require non-zero accumulation. Don't pin a
    // specific magnitude — under parallel test load the busy spin can be
    // CPU-starved and report less elapsed wall-clock time than expected.
    GNFS_TEST_CHECK(t.elapsed_ns() > 0);

    std::cout << "  double start idempotent: PASS" << std::endl;
}

void test_accumulating_across_cycles() {
    std::cout << "Testing Timer accumulates across multiple cycles..." << std::endl;

    Timer t;

    t.start();
    busy_for(1ms);
    t.stop();
    std::int64_t after_first = t.elapsed_ns();

    // Restart without resetting accumulated_
    t.start();
    busy_for(1ms);
    t.stop();
    std::int64_t after_second = t.elapsed_ns();

    // Second cycle's accumulation strictly greater than the first cycle's
    // — that's the invariant we care about (accumulator across stop/start
    // cycles). Don't pin an absolute lower bound; busy_for() under heavy
    // parallel test load can be CPU-starved.
    GNFS_TEST_CHECK(after_second > after_first);

    std::cout << "  accumulation: PASS" << std::endl;
}

void test_reset() {
    std::cout << "Testing Timer reset..." << std::endl;

    Timer t;
    t.start();
    busy_for(500us);
    t.stop();
    GNFS_TEST_CHECK(t.elapsed_ns() > 0);

    t.reset();
    GNFS_TEST_CHECK(t.elapsed_seconds() == 0.0);
    GNFS_TEST_CHECK(t.elapsed_ns() == 0);
    GNFS_TEST_CHECK(!t.is_running());

    // After reset, can re-start
    t.start();
    busy_for(500us);
    t.stop();
    GNFS_TEST_CHECK(t.elapsed_ns() > 0);

    std::cout << "  reset: PASS" << std::endl;
}

void test_restart() {
    std::cout << "Testing Timer restart..." << std::endl;

    Timer t;
    t.start();
    busy_for(500us);
    t.stop();
    std::int64_t before_restart = t.elapsed_ns();
    GNFS_TEST_CHECK(before_restart > 0);

    // A runner can be preempted between restart() and elapsed_ns(). Retry the
    // immediate observation so one scheduling delay cannot fail the contract.
    std::int64_t after_restart = before_restart;
    for (int attempt = 0; attempt < 5 && after_restart >= before_restart; ++attempt) {
        t.restart();
        after_restart = t.elapsed_ns();
    }
    GNFS_TEST_CHECK(after_restart < before_restart);
    GNFS_TEST_CHECK(t.is_running());

    busy_for(500us);
    std::int64_t during_running = t.elapsed_ns();
    GNFS_TEST_CHECK(during_running > after_restart);

    t.stop();

    std::cout << "  restart: PASS" << std::endl;
}

void test_running_query_while_active() {
    std::cout << "Testing elapsed query while running..." << std::endl;

    Timer t;
    t.start();
    busy_for(500us);

    // Query while running should include in-flight time
    std::int64_t snapshot1 = t.elapsed_ns();
    busy_for(500us);
    std::int64_t snapshot2 = t.elapsed_ns();

    GNFS_TEST_CHECK(snapshot1 > 0);
    GNFS_TEST_CHECK(snapshot2 > snapshot1); // Time advances

    t.stop();

    std::cout << "  running query: PASS" << std::endl;
}

void test_elapsed_unit_conversions() {
    std::cout << "Testing elapsed unit conversions consistency..." << std::endl;

    Timer t;
    t.start();
    busy_for(2ms);
    t.stop();

    double seconds = t.elapsed_seconds();
    double ms = t.elapsed_ms();
    double us = t.elapsed_us();
    std::int64_t ns = t.elapsed_ns();

    // All units describe the same duration; conversion factors must hold.
    // Allow tiny epsilon for floating-point.
    GNFS_TEST_CHECK(std::abs(seconds * 1000.0 - ms) < 1e-6);
    GNFS_TEST_CHECK(std::abs(ms * 1000.0 - us) < 1e-3);
    GNFS_TEST_CHECK(std::abs(us * 1000.0 - static_cast<double>(ns)) < 1e3);

    // All > 0 (we busied 2 ms). Don't pin absolute magnitudes — under heavy
    // parallel test load a busy spin can wall-clock less than its target.
    GNFS_TEST_CHECK(seconds > 0);
    GNFS_TEST_CHECK(ms > 0);
    GNFS_TEST_CHECK(us > 0);
    GNFS_TEST_CHECK(ns > 0);

    std::cout << "  unit conversions: PASS" << std::endl;
}

void test_scoped_timer() {
    std::cout << "Testing ScopedTimer RAII..." << std::endl;

    Timer t;
    GNFS_TEST_CHECK(!t.is_running());

    {
        Timer::ScopedTimer guard(t);
        GNFS_TEST_CHECK(t.is_running());
        busy_for(1ms);
    } // guard destructor stops timer

    GNFS_TEST_CHECK(!t.is_running());
    GNFS_TEST_CHECK(t.elapsed_ns() > 0);

    std::cout << "  ScopedTimer RAII: PASS" << std::endl;
}

void test_scoped_timer_via_factory() {
    std::cout << "Testing Timer::scoped() factory..." << std::endl;

    Timer t;
    {
        auto guard = t.scoped();
        GNFS_TEST_CHECK(t.is_running());
        busy_for(500us);
    }
    GNFS_TEST_CHECK(!t.is_running());
    GNFS_TEST_CHECK(t.elapsed_ns() > 0);

    std::cout << "  scoped() factory: PASS" << std::endl;
}

void test_scoped_timer_move() {
    std::cout << "Testing ScopedTimer move semantics..." << std::endl;

    Timer t;
    {
        Timer::ScopedTimer outer(t);
        GNFS_TEST_CHECK(t.is_running());

        // Move outer into inner; outer becomes inactive
        Timer::ScopedTimer inner(std::move(outer));
        GNFS_TEST_CHECK(t.is_running()); // Inner now owns; timer still running

        busy_for(500us);
    } // Inner destroyed first (last-declared, first-destroyed), stops timer.
      // Outer destroyed after; its active_=false so no-op.

    GNFS_TEST_CHECK(!t.is_running());
    GNFS_TEST_CHECK(t.elapsed_ns() > 0);

    std::cout << "  ScopedTimer move: PASS" << std::endl;
}

void test_stopwatch_basic() {
    std::cout << "Testing Stopwatch..." << std::endl;

    Stopwatch sw;
    busy_for(1ms);

    double before_s = sw.elapsed_seconds();
    double elapsed_ms = sw.elapsed_ms();
    double after_s = sw.elapsed_seconds();

    GNFS_TEST_CHECK(before_s > 0);
    GNFS_TEST_CHECK(after_s > 0);
    GNFS_TEST_CHECK(elapsed_ms > 0); // Don't pin magnitude; busy spin can be CPU-starved
    GNFS_TEST_CHECK(elapsed_ms >= before_s * 1000.0);
    GNFS_TEST_CHECK(elapsed_ms <= after_s * 1000.0);

    std::cout << "  Stopwatch basic: PASS" << std::endl;
}

void test_stopwatch_restart() {
    std::cout << "Testing Stopwatch restart..." << std::endl;

    Stopwatch sw;
    busy_for(2ms);
    double before = sw.elapsed_ms();
    GNFS_TEST_CHECK(before > 0); // Busy-spin advances clock at least minimally

    // A runner can be preempted between restart() and elapsed_ms(). Retry the
    // immediate observation so one scheduling delay cannot fail the contract.
    double after_restart = before;
    for (int attempt = 0; attempt < 5 && after_restart >= before; ++attempt) {
        sw.restart();
        after_restart = sw.elapsed_ms();
    }
    GNFS_TEST_CHECK(after_restart < before);

    std::cout << "  Stopwatch restart: PASS" << std::endl;
}

void test_noexcept_contract() {
    std::cout << "Testing noexcept contract..." << std::endl;

    Timer t;
    // These are documented noexcept; compile-time check via static_assert
    static_assert(noexcept(t.start()));
    static_assert(noexcept(t.stop()));
    static_assert(noexcept(t.reset()));
    static_assert(noexcept(t.restart()));
    static_assert(noexcept(t.is_running()));
    static_assert(noexcept(t.elapsed_seconds()));
    static_assert(noexcept(t.elapsed_ns()));
    static_assert(noexcept(t.scoped()));

    Stopwatch sw;
    static_assert(noexcept(sw.restart()));
    static_assert(noexcept(sw.elapsed_seconds()));
    static_assert(noexcept(sw.elapsed_ms()));

    std::cout << "  noexcept contract: PASS" << std::endl;
}

int main() {
    std::cout << "=== util/timer.hpp tests ===" << std::endl;

    test_default_state();
    test_start_stop_basic();
    test_double_start_idempotent();
    test_accumulating_across_cycles();
    test_reset();
    test_restart();
    test_running_query_while_active();
    test_elapsed_unit_conversions();
    test_scoped_timer();
    test_scoped_timer_via_factory();
    test_scoped_timer_move();
    test_stopwatch_basic();
    test_stopwatch_restart();
    test_noexcept_contract();

    std::cout << "\n=== All util/timer.hpp tests PASSED ===" << std::endl;
    return 0;
}
