// Unit tests for include/gnfs/linalg/progress_telemetry.hpp (W12 T1).
//
// Verifies:
//   * ENV parsing: unset / "0" / "100" / "garbage" / "12abc" / " 5"
//   * Disabled mode (interval==0): tick + finish produce zero stderr output,
//     no clock samples are taken, the logger is a true no-op.
//   * Enabled mode (interval > 0): tick fires on the first call and on
//     every Nth call thereafter; finish() always fires a DONE summary.
//   * Multiple loggers can coexist in the same process and write distinct
//     phase-labelled lines without interfering.
//   * Edge: total == 0 prints eta=?; iter > total clamps; iter < 0 clamps;
//     extremely fast loops (rate near zero) print "?" rather than NaN/inf.
//   * Move semantics: move-construct and move-assign transfer ownership
//     and the moved-from logger skips the DONE summary (no double print).
//   * Reset env cache: toggle ENV between scenarios via the test hook.
//   * Perf-info probe: 100k disabled-mode ticks should be free (no
//     assertion on timing, just informational).

// Force assert() to remain live even under -DNDEBUG so Release builds do
// not silently strip verification.
#ifdef NDEBUG
#  undef NDEBUG
#endif

#include "gnfs/linalg/progress_telemetry.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

using namespace gnfs::linalg;

// ────────────────────────────────────────────────────────────────────
// Stderr capture helper (RAII redirect of std::cerr to a string stream).
// ────────────────────────────────────────────────────────────────────

class StderrCapture {
public:
    StderrCapture() : prev_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~StderrCapture() { std::cerr.rdbuf(prev_); }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;
    [[nodiscard]] std::string str() const { return buf_.str(); }
    void clear() { buf_.str(std::string{}); buf_.clear(); }
private:
    std::stringstream buf_;
    std::streambuf* prev_;
};

// Count occurrences of `needle` in `haystack`.
static std::size_t count_occurrences(const std::string& haystack,
                                     const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// Convenience: set ENV and reload the cached gate.
static void set_env_interval(const char* value) {
    if (value == nullptr) {
        unsetenv("GNFS_LINALG_PROGRESS_INTERVAL");
    } else {
        setenv("GNFS_LINALG_PROGRESS_INTERVAL", value, 1);
    }
    linalg_progress_reset_env_cache_for_testing();
}

// ────────────────────────────────────────────────────────────────────
// ENV parsing tests
// ────────────────────────────────────────────────────────────────────

static void test_env_unset_default_zero() {
    std::cout << "Testing GNFS_LINALG_PROGRESS_INTERVAL unset => 0 (disabled)..." << std::endl;
    set_env_interval(nullptr);
    assert(linalg_progress_interval() == 0);
    assert(linalg_progress_enabled() == false);
    std::cout << "  ENV unset: PASS (disabled)" << std::endl;
}

static void test_env_zero_explicit() {
    std::cout << "Testing GNFS_LINALG_PROGRESS_INTERVAL=\"0\" => 0..." << std::endl;
    set_env_interval("0");
    assert(linalg_progress_interval() == 0);
    assert(linalg_progress_enabled() == false);
    set_env_interval(nullptr);
    std::cout << "  ENV \"0\": PASS (disabled)" << std::endl;
}

static void test_env_positive_clean() {
    std::cout << "Testing GNFS_LINALG_PROGRESS_INTERVAL=\"100\" => 100..." << std::endl;
    set_env_interval("100");
    assert(linalg_progress_interval() == 100);
    assert(linalg_progress_enabled() == true);
    set_env_interval(nullptr);
    std::cout << "  ENV \"100\": PASS (enabled at 100)" << std::endl;
}

static void test_env_garbage_disabled() {
    std::cout << "Testing GNFS_LINALG_PROGRESS_INTERVAL=\"garbage\" => 0..." << std::endl;
    set_env_interval("garbage");
    assert(linalg_progress_interval() == 0);
    assert(linalg_progress_enabled() == false);
    set_env_interval(nullptr);
    std::cout << "  ENV \"garbage\": PASS (disabled)" << std::endl;
}

static void test_env_numeric_prefix_documented() {
    // Documented behavior: std::strtol accepts a numeric prefix.
    // "12abc" -> 12. This matches W11 T3 (mpz_powm_parallel) semantics.
    std::cout << "Testing GNFS_LINALG_PROGRESS_INTERVAL=\"12abc\" => 12 (documented prefix-parse)..." << std::endl;
    set_env_interval("12abc");
    assert(linalg_progress_interval() == 12);
    assert(linalg_progress_enabled() == true);
    set_env_interval(nullptr);
    std::cout << "  ENV \"12abc\": PASS (interval=12 per documented prefix behavior)" << std::endl;
}

static void test_env_leading_whitespace_rejected() {
    std::cout << "Testing GNFS_LINALG_PROGRESS_INTERVAL=\" 5\" => 0..." << std::endl;
    set_env_interval(" 5");
    assert(linalg_progress_interval() == 0);
    assert(linalg_progress_enabled() == false);
    set_env_interval(nullptr);
    std::cout << "  ENV \" 5\": PASS (leading whitespace rejected -> disabled)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Disabled mode tests
// ────────────────────────────────────────────────────────────────────

static void test_disabled_no_stderr_output() {
    std::cout << "Testing disabled logger produces ZERO stderr output..." << std::endl;
    set_env_interval(nullptr);
    {
        StderrCapture cap;
        IterationProgressLogger lg("BL_SpMV", 1000);
        for (std::int64_t i = 0; i < 1000; ++i) {
            lg.tick(i);
        }
        lg.finish();
        // No output expected.
        const std::string out = cap.str();
        assert(out.empty() && "disabled logger should not write to stderr");
    }
    std::cout << "  disabled mode: PASS (zero stderr output for 1000 ticks)" << std::endl;
}

static void test_disabled_dtor_no_output() {
    std::cout << "Testing disabled logger dtor (without finish()) is silent..." << std::endl;
    set_env_interval(nullptr);
    {
        StderrCapture cap;
        {
            IterationProgressLogger lg("Phase", 10);
            for (std::int64_t i = 0; i < 10; ++i) lg.tick(i);
            // dtor runs at scope exit without explicit finish()
        }
        const std::string out = cap.str();
        assert(out.empty() && "disabled logger dtor should not write to stderr");
    }
    std::cout << "  disabled dtor: PASS (silent)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Enabled mode tests
// ────────────────────────────────────────────────────────────────────

static void test_enabled_first_tick_and_interval() {
    std::cout << "Testing enabled logger fires on first tick + every Nth tick..." << std::endl;
    // Interval = 10. We tick 100 times. Expected progress lines:
    //   first tick (iter=0) + ticks where (iter - last) >= 10
    //   So at iter = 0, 10, 20, 30, ..., 90 => 10 progress lines.
    // Plus the DONE summary on finish() => 11 total lines.
    set_env_interval("10");
    StderrCapture cap;
    {
        IterationProgressLogger lg("TestPhase", 100);
        for (std::int64_t i = 0; i < 100; ++i) {
            lg.tick(i);
        }
        lg.finish();
    }
    set_env_interval(nullptr);

    const std::string out = cap.str();
    const std::size_t progress_lines = count_occurrences(out, "[linalg_progress] phase=TestPhase iter=");
    const std::size_t done_lines = count_occurrences(out, "[linalg_progress] phase=TestPhase DONE");
    std::cout << "  captured: " << progress_lines << " progress + "
              << done_lines << " done line(s)" << std::endl;
    // First tick (iter=0) + ticks 10, 20, 30, 40, 50, 60, 70, 80, 90 = 10 lines
    assert(progress_lines == 10);
    assert(done_lines == 1);
    // The DONE summary should contain "iter=100/100"
    assert(out.find("DONE iter=100/100") != std::string::npos);
    std::cout << "  enabled tick gating: PASS (10 progress + 1 DONE)" << std::endl;
}

static void test_enabled_finish_emits_done() {
    std::cout << "Testing finish() emits DONE summary..." << std::endl;
    set_env_interval("5");
    StderrCapture cap;
    {
        IterationProgressLogger lg("F", 50);
        lg.tick(0);   // First tick: fires
        lg.tick(25);  // Mid: fires (delta 25 >= 5)
        lg.finish();
    }
    set_env_interval(nullptr);

    const std::string out = cap.str();
    assert(count_occurrences(out, "[linalg_progress] phase=F iter=") == 2);
    assert(count_occurrences(out, "[linalg_progress] phase=F DONE") == 1);
    std::cout << "  finish() DONE: PASS" << std::endl;
}

static void test_enabled_finish_idempotent() {
    std::cout << "Testing finish() is idempotent (double-call safe)..." << std::endl;
    set_env_interval("1");
    StderrCapture cap;
    {
        IterationProgressLogger lg("Idempotent", 5);
        lg.tick(0);
        lg.finish();
        lg.finish();  // second call: no-op
        lg.tick(1);   // after finish: no-op
    }
    set_env_interval(nullptr);
    const std::string out = cap.str();
    // 1 progress line (iter=0) + 1 DONE line. No more.
    assert(count_occurrences(out, "[linalg_progress] phase=Idempotent iter=") == 1);
    assert(count_occurrences(out, "[linalg_progress] phase=Idempotent DONE") == 1);
    std::cout << "  finish() idempotent: PASS" << std::endl;
}

static void test_multiple_loggers_coexist() {
    std::cout << "Testing multiple loggers do not interleave incorrectly..." << std::endl;
    set_env_interval("3");
    StderrCapture cap;
    {
        IterationProgressLogger a("A_phase", 9);
        IterationProgressLogger b("B_phase", 9);
        // Alternating ticks.
        for (std::int64_t i = 0; i < 9; ++i) {
            a.tick(i);
            b.tick(i);
        }
        a.finish();
        b.finish();
    }
    set_env_interval(nullptr);
    const std::string out = cap.str();
    // For each phase: first tick (0) + every 3 ticks = 0, 3, 6 = 3 progress lines.
    // Plus 1 DONE each.
    assert(count_occurrences(out, "[linalg_progress] phase=A_phase iter=") == 3);
    assert(count_occurrences(out, "[linalg_progress] phase=B_phase iter=") == 3);
    assert(count_occurrences(out, "[linalg_progress] phase=A_phase DONE") == 1);
    assert(count_occurrences(out, "[linalg_progress] phase=B_phase DONE") == 1);
    std::cout << "  multiple loggers: PASS (A=3+1, B=3+1, distinct labels)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Edge cases
// ────────────────────────────────────────────────────────────────────

static void test_total_zero_eta_unknown() {
    std::cout << "Testing total=0 produces eta=? (cannot project)..." << std::endl;
    set_env_interval("1");
    StderrCapture cap;
    {
        IterationProgressLogger lg("ZeroTotal", 0);
        lg.tick(0);
        lg.finish();
    }
    set_env_interval(nullptr);
    const std::string out = cap.str();
    // First tick should contain "eta=?" since total is 0.
    assert(out.find("eta=?") != std::string::npos);
    // Should NOT contain NaN or inf.
    assert(out.find("nan") == std::string::npos);
    assert(out.find("NaN") == std::string::npos);
    assert(out.find("inf") == std::string::npos);
    assert(out.find("Inf") == std::string::npos);
    std::cout << "  total=0 ETA: PASS (eta=? not NaN/inf)" << std::endl;
}

static void test_negative_total_clamped() {
    std::cout << "Testing negative total is clamped to 0..." << std::endl;
    set_env_interval("1");
    StderrCapture cap;
    {
        IterationProgressLogger lg("NegTotal", -100);
        lg.tick(0);
        lg.finish();
    }
    set_env_interval(nullptr);
    const std::string out = cap.str();
    // total clamped to 0; first tick should print "iter=0/0".
    assert(out.find("iter=0/0") != std::string::npos);
    // DONE should also reference total=0.
    assert(out.find("DONE iter=0/0") != std::string::npos);
    std::cout << "  negative total: PASS (clamped to 0)" << std::endl;
}

static void test_iter_above_total_clamped() {
    std::cout << "Testing iter > total is clamped..." << std::endl;
    set_env_interval("1");
    StderrCapture cap;
    {
        IterationProgressLogger lg("Above", 100);
        lg.tick(200);  // > total => clamps to 100
        lg.finish();
    }
    set_env_interval(nullptr);
    const std::string out = cap.str();
    // The first tick must show clamped iter (100/100), not raw 200.
    assert(out.find("iter=100/100") != std::string::npos);
    assert(out.find("iter=200") == std::string::npos);
    std::cout << "  iter > total clamp: PASS" << std::endl;
}

static void test_negative_iter_clamped() {
    std::cout << "Testing negative iter is clamped to 0..." << std::endl;
    set_env_interval("1");
    StderrCapture cap;
    {
        IterationProgressLogger lg("NegIter", 10);
        lg.tick(-5);  // clamp to 0
        lg.finish();
    }
    set_env_interval(nullptr);
    const std::string out = cap.str();
    assert(out.find("iter=0/10") != std::string::npos);
    assert(out.find("iter=-5") == std::string::npos);
    std::cout << "  negative iter: PASS (clamped to 0)" << std::endl;
}

static void test_zero_rate_prints_question() {
    std::cout << "Testing rate==0 (instant ticks) prints \"?\" not NaN..." << std::endl;
    set_env_interval("1");
    StderrCapture cap;
    {
        IterationProgressLogger lg("ZeroRate", 100);
        lg.tick(0);  // First tick: iter=0 => compute_rate returns 0.0 => "?"
        // Note: subsequent ticks DO have nonzero iter, so they may or may
        // not show "?" depending on elapsed time. We only assert here on
        // the iter=0 case where the rate is guaranteed to be 0.0 by
        // construction (iter == 0 path).
        lg.finish();
    }
    set_env_interval(nullptr);
    const std::string out = cap.str();
    // The first progress line should contain rate=?
    assert(out.find("rate=?") != std::string::npos);
    // Should not contain NaN/inf even if instantaneous timing collapses.
    assert(out.find("nan") == std::string::npos);
    assert(out.find("inf") == std::string::npos);
    std::cout << "  rate underflow: PASS (rate=? not NaN/inf)" << std::endl;
}

static void test_extreme_rate_saturates_before_formatting() {
    std::cout << "Testing extreme rate formatting saturates without integer UB..." << std::endl;
    set_env_interval("1");
    StderrCapture cap;
    {
        IterationProgressLogger lg("ExtremeRate", INT64_MAX);
        // Ensure elapsed_s is positive while keeping the rate far above the
        // formatter's long-long-scaled range.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lg.tick(INT64_MAX);
        lg.finish();
    }
    set_env_interval(nullptr);

    const std::string out = cap.str();
    assert(out.find("rate=922337203685477580.7/s") != std::string::npos);
    assert(out.find("avg_rate=922337203685477580.7/s") != std::string::npos);
    assert(out.find("nan") == std::string::npos);
    assert(out.find("NaN") == std::string::npos);
    assert(out.find("inf") == std::string::npos);
    assert(out.find("Inf") == std::string::npos);
    std::cout << "  extreme rate: PASS (finite saturated output)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Move semantics
// ────────────────────────────────────────────────────────────────────

static void test_move_construct_no_double_emit() {
    std::cout << "Testing move-construct: moved-from skips DONE..." << std::endl;
    set_env_interval("1");
    StderrCapture cap;
    {
        IterationProgressLogger a("MoveA", 5);
        a.tick(0);
        IterationProgressLogger b(std::move(a));
        // a is now moved-from; its dtor should NOT emit a DONE.
        b.tick(2);
        // Both go out of scope. Only b should emit DONE.
    }
    set_env_interval(nullptr);
    const std::string out = cap.str();
    // We expect exactly 1 DONE line (from b).
    assert(count_occurrences(out, "[linalg_progress] phase=MoveA DONE") == 1);
    std::cout << "  move-construct: PASS (single DONE from move target)" << std::endl;
}

static void test_move_assign_releases_prior() {
    std::cout << "Testing move-assign: lhs finalises old + adopts new..." << std::endl;
    set_env_interval("1");
    StderrCapture cap;
    {
        IterationProgressLogger a("AssignA", 5);
        a.tick(0);
        IterationProgressLogger b("AssignB", 5);
        b.tick(0);
        // Move-assign b -> a. Existing `a` should be finalised first
        // (emits its DONE for "AssignA"), then `a` adopts `b`'s state.
        a = std::move(b);
        a.tick(2);
        // b is now moved-from; its dtor should NOT emit a second DONE.
        // a's dtor should emit DONE for "AssignB" (b's phase).
    }
    set_env_interval(nullptr);
    const std::string out = cap.str();
    // Exactly one DONE for AssignA (from the move-assign finalisation),
    // exactly one DONE for AssignB (from a's dtor after adopting b).
    assert(count_occurrences(out, "[linalg_progress] phase=AssignA DONE") == 1);
    assert(count_occurrences(out, "[linalg_progress] phase=AssignB DONE") == 1);
    std::cout << "  move-assign: PASS (AssignA DONE, AssignB DONE, no doubles)" << std::endl;
}

// ────────────────────────────────────────────────────────────────────
// Perf info (no assertion, just timing observation)
// ────────────────────────────────────────────────────────────────────

static void perf_info_disabled_loop() {
    std::cout << "Perf-info: 100,000 ticks in disabled mode (should be ~0 ms)..." << std::endl;
    set_env_interval(nullptr);
    auto t0 = std::chrono::steady_clock::now();
    {
        IterationProgressLogger lg("PerfDisabled", 100000);
        for (std::int64_t i = 0; i < 100000; ++i) {
            lg.tick(i);
        }
        lg.finish();
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "  100k ticks (disabled): " << ms << " ms (info only)" << std::endl;
    // Informational only; no assertion. A disabled logger ought to be
    // essentially free, but compiler / inlining variance makes timing
    // assertions fragile.
}

int main() {
    std::cout << "=== Linalg Iteration Progress Telemetry Tests (W12 T1) ===" << std::endl;

    std::cout << "\n--- ENV parsing ---" << std::endl;
    test_env_unset_default_zero();
    test_env_zero_explicit();
    test_env_positive_clean();
    test_env_garbage_disabled();
    test_env_numeric_prefix_documented();
    test_env_leading_whitespace_rejected();

    std::cout << "\n--- Disabled mode ---" << std::endl;
    test_disabled_no_stderr_output();
    test_disabled_dtor_no_output();

    std::cout << "\n--- Enabled mode ---" << std::endl;
    test_enabled_first_tick_and_interval();
    test_enabled_finish_emits_done();
    test_enabled_finish_idempotent();
    test_multiple_loggers_coexist();

    std::cout << "\n--- Edge cases ---" << std::endl;
    test_total_zero_eta_unknown();
    test_negative_total_clamped();
    test_iter_above_total_clamped();
    test_negative_iter_clamped();
    test_zero_rate_prints_question();
    test_extreme_rate_saturates_before_formatting();

    std::cout << "\n--- Move semantics ---" << std::endl;
    test_move_construct_no_double_emit();
    test_move_assign_releases_prior();

    std::cout << "\n--- Perf info ---" << std::endl;
    perf_info_disabled_loop();

    std::cout << "\nAll linalg iteration progress telemetry tests passed!" << std::endl;
    return 0;
}
