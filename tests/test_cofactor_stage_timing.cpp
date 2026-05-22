// Unit tests for include/gnfs/cofactor/stage_timing.hpp (W12 T5).
//
// Verifies:
//   * ENV parsing for unset / "" / "0" / "1" / "true" / "garbage" /
//     " 1" (leading whitespace) — only the exact string "1" enables
//   * Disabled mode: StageTimer ctor/dtor do NOT touch the atomic stats
//     (no clock samples, no fetch_add)
//   * Enabled mode: StageTimer dtor increments total_ns by elapsed
//     nanoseconds AND increments call_count by 1
//   * Nested timers: outer wrapping inner each attribute to their own stage
//   * Concurrent timers from 4 threads accumulate atomically
//   * format_cofactor_timing_summary() returns "disabled" when ENV off
//   * format_cofactor_timing_summary() returns full breakdown when ENV on
//   * reset() zeros all counters
//   * Move-construct does not double-increment
//   * Move-assign finalizes the prior measurement once and adopts the new
//   * Perf-info probe (timing overhead per StageTimer scope)
//
// All tests share the same process; the cached env reader is reset via
// `cofactor_timing_reset_env_cache_for_testing()` between cases. Stats are
// reset via `cofactor_timing_stats().reset()` between cases.

// Force assert() to remain live even under -DNDEBUG so Release builds
// do not silently strip verification.
#ifdef NDEBUG
#  undef NDEBUG
#endif

#include "gnfs/cofactor/stage_timing.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnfs::cofactor;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    ++tests_passed; \
} while (0)

// Helper: clear stats and reset env cache between tests.
static void reset_state_with_env(const char* env_value /* nullable */) {
    cofactor_timing_stats().reset();
    if (env_value == nullptr) {
        unsetenv("GNFS_COFACTOR_TIMING_ENABLE");
    } else {
        setenv("GNFS_COFACTOR_TIMING_ENABLE", env_value, 1);
    }
    cofactor_timing_reset_env_cache_for_testing();
}

// ────────────────────────────────────────────────────────────────────
// ENV parsing tests
// ────────────────────────────────────────────────────────────────────

static void test_env_unset_default_off() {
    std::cout << "Testing GNFS_COFACTOR_TIMING_ENABLE unset => off..." << std::endl;
    reset_state_with_env(nullptr);
    assert(cofactor_timing_enabled() == false);
    TEST_PASS("env unset disables telemetry");
}

static void test_env_empty_off() {
    std::cout << "Testing GNFS_COFACTOR_TIMING_ENABLE=\"\" => off..." << std::endl;
    reset_state_with_env("");
    assert(cofactor_timing_enabled() == false);
    reset_state_with_env(nullptr);  // restore default
    TEST_PASS("env empty string disables");
}

static void test_env_zero_off() {
    std::cout << "Testing GNFS_COFACTOR_TIMING_ENABLE=0 => off..." << std::endl;
    reset_state_with_env("0");
    assert(cofactor_timing_enabled() == false);
    reset_state_with_env(nullptr);
    TEST_PASS("env=\"0\" disables");
}

static void test_env_one_on() {
    std::cout << "Testing GNFS_COFACTOR_TIMING_ENABLE=1 => on..." << std::endl;
    reset_state_with_env("1");
    assert(cofactor_timing_enabled() == true);
    reset_state_with_env(nullptr);
    TEST_PASS("env=\"1\" enables");
}

static void test_env_other_values_off() {
    std::cout << "Testing GNFS_COFACTOR_TIMING_ENABLE various non-\"1\" => off..." << std::endl;
    // Strict matching: only the exact string "1" enables. Leading whitespace,
    // trailing whitespace, "true", "TRUE", "01", "10", "yes" all disable.
    const char* others[] = {"true", "TRUE", "yes", "01", "10", "2", " 1", "1 ",
                            "garbage", "on", "ON"};
    for (const char* v : others) {
        reset_state_with_env(v);
        assert(cofactor_timing_enabled() == false &&
               "Only exact \"1\" should enable telemetry");
    }
    reset_state_with_env(nullptr);
    TEST_PASS("env non-\"1\" values all disable");
}

// ────────────────────────────────────────────────────────────────────
// Disabled-mode invariant
// ────────────────────────────────────────────────────────────────────

static void test_disabled_mode_no_counter_updates() {
    std::cout << "Testing disabled mode: StageTimer does NOT update counters..." << std::endl;
    reset_state_with_env("0");
    auto& stats = cofactor_timing_stats();
    const uint64_t pre_total = stats.total_ns_for(CofactorStage::TrialDivision);
    const uint64_t pre_calls = stats.call_count_for(CofactorStage::TrialDivision);

    {
        StageTimer t(CofactorStage::TrialDivision);
        // Sleep ~1ms to make the elapsed time non-trivial; in enabled mode
        // this would clearly update counters. In disabled mode, the timer
        // must not touch the atomic stats at all.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const uint64_t post_total = stats.total_ns_for(CofactorStage::TrialDivision);
    const uint64_t post_calls = stats.call_count_for(CofactorStage::TrialDivision);
    assert(post_total == pre_total && "disabled mode must not update total_ns");
    assert(post_calls == pre_calls && "disabled mode must not update call_count");

    reset_state_with_env(nullptr);
    TEST_PASS("disabled mode does not touch counters");
}

// ────────────────────────────────────────────────────────────────────
// Enabled-mode counter update
// ────────────────────────────────────────────────────────────────────

static void test_enabled_mode_counter_increments() {
    std::cout << "Testing enabled mode: StageTimer dtor updates counters..." << std::endl;
    reset_state_with_env("1");
    auto& stats = cofactor_timing_stats();
    // Stats already reset by reset_state_with_env.

    constexpr int N_LOOPS = 5;
    for (int i = 0; i < N_LOOPS; ++i) {
        StageTimer t(CofactorStage::Squfof);
        // Burn a tiny amount of CPU so steady_clock advances; sleep is safer
        // than busy-loop for deterministic non-zero elapsed.
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    const uint64_t total = stats.total_ns_for(CofactorStage::Squfof);
    const uint64_t calls = stats.call_count_for(CofactorStage::Squfof);
    assert(calls == static_cast<uint64_t>(N_LOOPS) &&
           "call_count should equal number of timer scopes");
    assert(total > 0 && "total_ns should be > 0 after 5x100us scopes");
    // Sanity: each scope >= 100us, total >= ~400us = 4e5 ns. Allow some
    // slack for clock resolution / scheduler.
    assert(total >= 100'000ULL &&
           "total_ns should be at least 100us across 5 scopes");

    reset_state_with_env(nullptr);
    TEST_PASS("enabled mode increments call_count and total_ns");
}

// ────────────────────────────────────────────────────────────────────
// Nested timers
// ────────────────────────────────────────────────────────────────────

static void test_nested_timers_independent() {
    std::cout << "Testing nested timers attribute independently..." << std::endl;
    reset_state_with_env("1");
    auto& stats = cofactor_timing_stats();

    {
        StageTimer outer(CofactorStage::TrialDivision);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        {
            StageTimer inner(CofactorStage::EcmStage1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    const uint64_t outer_total = stats.total_ns_for(CofactorStage::TrialDivision);
    const uint64_t outer_calls = stats.call_count_for(CofactorStage::TrialDivision);
    const uint64_t inner_total = stats.total_ns_for(CofactorStage::EcmStage1);
    const uint64_t inner_calls = stats.call_count_for(CofactorStage::EcmStage1);

    assert(outer_calls == 1 && "outer timer ran exactly once");
    assert(inner_calls == 1 && "inner timer ran exactly once");
    assert(outer_total > inner_total &&
           "outer timer encloses inner so total must be larger");
    assert(inner_total > 0 && "inner timer recorded non-zero time");

    reset_state_with_env(nullptr);
    TEST_PASS("nested timers attribute to independent stages");
}

// ────────────────────────────────────────────────────────────────────
// Concurrent timers across threads
// ────────────────────────────────────────────────────────────────────

static void test_concurrent_timers_atomic() {
    std::cout << "Testing 4 threads * 100 timers each accumulate atomically..." << std::endl;
    reset_state_with_env("1");
    auto& stats = cofactor_timing_stats();

    constexpr int N_THREADS = 4;
    constexpr int N_PER = 100;
    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([]() {
            for (int i = 0; i < N_PER; ++i) {
                StageTimer timer(CofactorStage::BrentPollardRho);
                // Tiny non-zero work so steady_clock advances reliably.
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }
    for (auto& th : threads) th.join();

    const uint64_t calls = stats.call_count_for(CofactorStage::BrentPollardRho);
    assert(calls == static_cast<uint64_t>(N_THREADS * N_PER) &&
           "all 400 timer scopes must be counted exactly once");
    const uint64_t total = stats.total_ns_for(CofactorStage::BrentPollardRho);
    assert(total > 0 && "concurrent total_ns must be > 0");

    reset_state_with_env(nullptr);
    TEST_PASS("concurrent 4-thread x 100-call timers accumulate atomically");
}

// ────────────────────────────────────────────────────────────────────
// format_cofactor_timing_summary
// ────────────────────────────────────────────────────────────────────

static void test_format_summary_when_disabled() {
    std::cout << "Testing format_cofactor_timing_summary when disabled..." << std::endl;
    reset_state_with_env(nullptr);
    const std::string s = format_cofactor_timing_summary();
    assert(s == "[cofactor_timing] disabled" &&
           "disabled-mode summary should be exactly the sentinel string");
    TEST_PASS("disabled-mode summary == \"[cofactor_timing] disabled\"");
}

static void test_format_summary_when_enabled_nonzero() {
    std::cout << "Testing format_cofactor_timing_summary when enabled with nonzero counters..." << std::endl;
    reset_state_with_env("1");
    {
        StageTimer t(CofactorStage::EcmStage2);
        std::this_thread::sleep_for(std::chrono::microseconds(150));
    }
    const std::string s = format_cofactor_timing_summary();
    // Must contain the [cofactor_timing] tag and all 6 stage names.
    assert(s.find("[cofactor_timing]") != std::string::npos &&
           "summary must contain leading tag");
    assert(s.find("trial=") != std::string::npos);
    assert(s.find("squfof=") != std::string::npos);
    assert(s.find("brent_rho=") != std::string::npos);
    assert(s.find("pollard_rho=") != std::string::npos);
    assert(s.find("ecm_s1=") != std::string::npos);
    assert(s.find("ecm_s2=") != std::string::npos);
    assert(s.find("calls") != std::string::npos);
    // ecm_s2 should NOT be 0ns/0calls (we just ran a timer for it).
    // We can verify by checking that "ecm_s2=0ns/0calls" does NOT appear.
    assert(s.find("ecm_s2=0ns/0calls") == std::string::npos &&
           "ecm_s2 should have a non-zero entry");
    std::cout << "  summary: " << s << "\n";
    reset_state_with_env(nullptr);
    TEST_PASS("enabled summary contains all stage names with live counters");
}

// ────────────────────────────────────────────────────────────────────
// reset()
// ────────────────────────────────────────────────────────────────────

static void test_reset_zeros_counters() {
    std::cout << "Testing StageTimingStats::reset() zeros all counters..." << std::endl;
    reset_state_with_env("1");
    auto& stats = cofactor_timing_stats();
    {
        StageTimer t1(CofactorStage::TrialDivision);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    {
        StageTimer t2(CofactorStage::EcmStage1);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    assert(stats.call_count_for(CofactorStage::TrialDivision) == 1);
    assert(stats.call_count_for(CofactorStage::EcmStage1) == 1);
    assert(stats.total_ns_for(CofactorStage::TrialDivision) > 0);
    assert(stats.total_ns_for(CofactorStage::EcmStage1) > 0);

    stats.reset();

    for (int i = 0; i < static_cast<int>(CofactorStage::kNumStages); ++i) {
        const auto s = static_cast<CofactorStage>(i);
        assert(stats.total_ns_for(s) == 0 && "all total_ns must be zero after reset");
        assert(stats.call_count_for(s) == 0 && "all call_count must be zero after reset");
    }
    reset_state_with_env(nullptr);
    TEST_PASS("reset() zeros all 6 stage counters");
}

// ────────────────────────────────────────────────────────────────────
// Move semantics
// ────────────────────────────────────────────────────────────────────

static void test_move_construct_no_double_increment() {
    std::cout << "Testing move-construct does NOT double-increment..." << std::endl;
    reset_state_with_env("1");
    auto& stats = cofactor_timing_stats();
    {
        StageTimer original(CofactorStage::PollardRho);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        // Move-construct into a new timer; original is now moved-from.
        StageTimer adopted(std::move(original));
        // Both timers are about to be destroyed (adopted at end of scope,
        // original was already moved-from). Only `adopted` should record.
    }
    const uint64_t calls = stats.call_count_for(CofactorStage::PollardRho);
    assert(calls == 1 && "move-construct must NOT double-count");
    assert(stats.total_ns_for(CofactorStage::PollardRho) > 0);
    reset_state_with_env(nullptr);
    TEST_PASS("move-construct records exactly once");
}

static void test_move_assign_finalizes_prior() {
    std::cout << "Testing move-assign finalizes prior measurement..." << std::endl;
    reset_state_with_env("1");
    auto& stats = cofactor_timing_stats();
    {
        StageTimer a(CofactorStage::Squfof);
        std::this_thread::sleep_for(std::chrono::microseconds(100));

        // Brand-new timer for a different stage. Move-assigning b into a
        // should finalize a's prior measurement (record Squfof), then a
        // adopts b's tracking (EcmStage2).
        StageTimer b(CofactorStage::EcmStage2);
        std::this_thread::sleep_for(std::chrono::microseconds(100));

        a = std::move(b);  // finalizes the prior Squfof timer, adopts EcmStage2.
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        // At end of scope, a's dtor records EcmStage2.
        // b is moved-from and its dtor does nothing.
    }
    const uint64_t squfof_calls = stats.call_count_for(CofactorStage::Squfof);
    const uint64_t ecm_calls = stats.call_count_for(CofactorStage::EcmStage2);
    assert(squfof_calls == 1 && "move-assign finalizes prior Squfof timer once");
    assert(ecm_calls == 1 && "move-assign adopts then records EcmStage2 once");
    assert(stats.total_ns_for(CofactorStage::Squfof) > 0);
    assert(stats.total_ns_for(CofactorStage::EcmStage2) > 0);
    reset_state_with_env(nullptr);
    TEST_PASS("move-assign finalizes prior + records new exactly once each");
}

// ────────────────────────────────────────────────────────────────────
// stage_name lookup
// ────────────────────────────────────────────────────────────────────

static void test_stage_name_lookup() {
    std::cout << "Testing stage_name() returns expected strings..." << std::endl;
    using std::string;
    assert(string(stage_name(CofactorStage::TrialDivision))   == "trial");
    assert(string(stage_name(CofactorStage::Squfof))          == "squfof");
    assert(string(stage_name(CofactorStage::BrentPollardRho)) == "brent_rho");
    assert(string(stage_name(CofactorStage::PollardRho))      == "pollard_rho");
    assert(string(stage_name(CofactorStage::EcmStage1))       == "ecm_s1");
    assert(string(stage_name(CofactorStage::EcmStage2))       == "ecm_s2");
    TEST_PASS("stage_name covers all 6 stages with stable strings");
}

// ────────────────────────────────────────────────────────────────────
// Perf info probe (informational, not a strict assertion)
// ────────────────────────────────────────────────────────────────────

static void test_perf_timer_overhead() {
    std::cout << "Testing perf-info: StageTimer scope overhead..." << std::endl;
    reset_state_with_env("1");
    cofactor_timing_stats().reset();

    constexpr int N = 100'000;
    // OFF baseline.
    reset_state_with_env("0");
    const auto t0_off = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        StageTimer t(CofactorStage::TrialDivision);
        (void)t;
    }
    const auto t1_off = std::chrono::steady_clock::now();
    const double off_ms = std::chrono::duration<double, std::milli>(
                              t1_off - t0_off).count();

    // ON measurement.
    reset_state_with_env("1");
    cofactor_timing_stats().reset();
    const auto t0_on = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        StageTimer t(CofactorStage::TrialDivision);
        (void)t;
    }
    const auto t1_on = std::chrono::steady_clock::now();
    const double on_ms = std::chrono::duration<double, std::milli>(
                             t1_on - t0_on).count();

    std::cout << "  [perf info] " << N << " StageTimer scopes: "
              << "OFF=" << off_ms << "ms, ON=" << on_ms
              << "ms, overhead per scope = "
              << ((on_ms - off_ms) * 1e6 / N) << " ns\n";
    // Sanity: both should finish well under 10 seconds.
    assert(off_ms < 10000.0);
    assert(on_ms < 10000.0);
    reset_state_with_env(nullptr);
    TEST_PASS("perf info: timer scope overhead measured");
}

// ────────────────────────────────────────────────────────────────────
// Main
// ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "===========================================\n";
    std::cout << "  Cofactor Stage Timing Telemetry Tests\n";
    std::cout << "===========================================\n\n";

    test_env_unset_default_off();
    test_env_empty_off();
    test_env_zero_off();
    test_env_one_on();
    test_env_other_values_off();

    test_disabled_mode_no_counter_updates();
    test_enabled_mode_counter_increments();
    test_nested_timers_independent();
    test_concurrent_timers_atomic();

    test_format_summary_when_disabled();
    test_format_summary_when_enabled_nonzero();

    test_reset_zeros_counters();

    test_move_construct_no_double_increment();
    test_move_assign_finalizes_prior();

    test_stage_name_lookup();

    test_perf_timer_overhead();

    std::cout << "\n===========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "===========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
