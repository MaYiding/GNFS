// Unit tests for gnfs::util::Timer + Stopwatch + ScopedTimer.

#include "gnfs/util/timer.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

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

}  // namespace

void test_default_state() {
    std::cout << "Testing Timer default state..." << std::endl;

    Timer t;
    assert(!t.is_running());
    assert(t.elapsed_seconds() == 0.0);
    assert(t.elapsed_ms() == 0.0);
    assert(t.elapsed_us() == 0.0);
    assert(t.elapsed_ns() == 0);

    std::cout << "  default state: PASS" << std::endl;
}

void test_start_stop_basic() {
    std::cout << "Testing Timer start/stop..." << std::endl;

    Timer t;
    t.start();
    assert(t.is_running());
    busy_for(1ms);
    t.stop();
    assert(!t.is_running());

    // Should have accumulated > 0
    assert(t.elapsed_seconds() > 0.0);
    assert(t.elapsed_ms() > 0.0);
    assert(t.elapsed_ns() > 0);

    // Idempotent stop (second stop should be no-op)
    int64_t snapshot_ns = t.elapsed_ns();
    t.stop();
    assert(t.elapsed_ns() == snapshot_ns);

    std::cout << "  start/stop: PASS" << std::endl;
}

void test_double_start_idempotent() {
    std::cout << "Testing Timer double start (idempotent)..." << std::endl;

    Timer t;
    t.start();
    auto first_start_running = t.is_running();
    busy_for(500us);

    // Second start should be no-op (per impl: only sets start_ if !running_)
    t.start();
    busy_for(500us);
    t.stop();

    assert(first_start_running);
    // Accumulated should be ≥ 1 ms (we busied for 1 ms total)
    assert(t.elapsed_us() >= 800.0);  // Some leeway for clock jitter

    std::cout << "  double start idempotent: PASS" << std::endl;
}

void test_accumulating_across_cycles() {
    std::cout << "Testing Timer accumulates across multiple cycles..." << std::endl;

    Timer t;

    t.start();
    busy_for(1ms);
    t.stop();
    int64_t after_first = t.elapsed_ns();

    // Restart without resetting accumulated_
    t.start();
    busy_for(1ms);
    t.stop();
    int64_t after_second = t.elapsed_ns();

    // Second cycle's accumulation strictly greater
    assert(after_second > after_first);
    // Total roughly 2 ms (with leeway)
    assert(after_second > 1500000);  // > 1.5 ms in ns

    std::cout << "  accumulation: PASS" << std::endl;
}

void test_reset() {
    std::cout << "Testing Timer reset..." << std::endl;

    Timer t;
    t.start();
    busy_for(500us);
    t.stop();
    assert(t.elapsed_ns() > 0);

    t.reset();
    assert(t.elapsed_seconds() == 0.0);
    assert(t.elapsed_ns() == 0);
    assert(!t.is_running());

    // After reset, can re-start
    t.start();
    busy_for(500us);
    t.stop();
    assert(t.elapsed_ns() > 0);

    std::cout << "  reset: PASS" << std::endl;
}

void test_restart() {
    std::cout << "Testing Timer restart..." << std::endl;

    Timer t;
    t.start();
    busy_for(500us);
    t.stop();
    int64_t before_restart = t.elapsed_ns();
    assert(before_restart > 0);

    // restart should reset and start in one step
    t.restart();
    assert(t.is_running());

    busy_for(500us);
    int64_t during_running = t.elapsed_ns();
    // Should be much smaller than before_restart accumulated, because reset
    assert(during_running < before_restart * 2);  // crude but safe

    t.stop();

    std::cout << "  restart: PASS" << std::endl;
}

void test_running_query_while_active() {
    std::cout << "Testing elapsed query while running..." << std::endl;

    Timer t;
    t.start();
    busy_for(500us);

    // Query while running should include in-flight time
    int64_t snapshot1 = t.elapsed_ns();
    busy_for(500us);
    int64_t snapshot2 = t.elapsed_ns();

    assert(snapshot1 > 0);
    assert(snapshot2 > snapshot1);  // Time advances

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
    int64_t ns = t.elapsed_ns();

    // All units describe the same duration; conversion factors must hold.
    // Allow tiny epsilon for floating-point.
    assert(std::abs(seconds * 1000.0 - ms) < 1e-6);
    assert(std::abs(ms * 1000.0 - us) < 1e-3);
    assert(std::abs(us * 1000.0 - static_cast<double>(ns)) < 1e3);

    // All > 0 (we busied 2 ms)
    assert(seconds > 0);
    assert(ms > 1.5);  // Allow slop for clock jitter
    assert(us > 1500);
    assert(ns > 1500000);

    std::cout << "  unit conversions: PASS" << std::endl;
}

void test_scoped_timer() {
    std::cout << "Testing ScopedTimer RAII..." << std::endl;

    Timer t;
    assert(!t.is_running());

    {
        Timer::ScopedTimer guard(t);
        assert(t.is_running());
        busy_for(1ms);
    }  // guard destructor stops timer

    assert(!t.is_running());
    assert(t.elapsed_ns() > 0);

    std::cout << "  ScopedTimer RAII: PASS" << std::endl;
}

void test_scoped_timer_via_factory() {
    std::cout << "Testing Timer::scoped() factory..." << std::endl;

    Timer t;
    {
        auto guard = t.scoped();
        assert(t.is_running());
        busy_for(500us);
    }
    assert(!t.is_running());
    assert(t.elapsed_ns() > 0);

    std::cout << "  scoped() factory: PASS" << std::endl;
}

void test_scoped_timer_move() {
    std::cout << "Testing ScopedTimer move semantics..." << std::endl;

    Timer t;
    {
        Timer::ScopedTimer outer(t);
        assert(t.is_running());

        // Move outer into inner; outer becomes inactive
        Timer::ScopedTimer inner(std::move(outer));
        assert(t.is_running());  // Inner now owns; timer still running

        busy_for(500us);
    }  // Inner destroyed first (last-declared, first-destroyed), stops timer.
       // Outer destroyed after; its active_=false so no-op.

    assert(!t.is_running());
    assert(t.elapsed_ns() > 0);

    std::cout << "  ScopedTimer move: PASS" << std::endl;
}

void test_stopwatch_basic() {
    std::cout << "Testing Stopwatch..." << std::endl;

    Stopwatch sw;
    busy_for(1ms);

    double elapsed_s = sw.elapsed_seconds();
    double elapsed_ms = sw.elapsed_ms();

    assert(elapsed_s > 0);
    assert(elapsed_ms > 0.8);  // Lower bound for 1 ms busy spin
    assert(std::abs(elapsed_s * 1000.0 - elapsed_ms) < 1e-3);

    std::cout << "  Stopwatch basic: PASS" << std::endl;
}

void test_stopwatch_restart() {
    std::cout << "Testing Stopwatch restart..." << std::endl;

    Stopwatch sw;
    busy_for(2ms);
    double before = sw.elapsed_ms();
    assert(before > 1.0);

    sw.restart();
    double after_restart = sw.elapsed_ms();
    // Immediately after restart, elapsed should be near zero
    assert(after_restart < before);
    assert(after_restart < 1.0);  // < 1ms, much less than 2ms before

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
