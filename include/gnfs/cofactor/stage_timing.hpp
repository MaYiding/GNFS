#pragma once

// Cofactor per-stage timing telemetry (W12 T5).
//
// Provides an opt-in RAII timer that accumulates per-stage wall time and
// call counts across the 6 cofactor pipeline stages (TrialDivision,
// Squfof, BrentPollardRho, PollardRho, EcmStage1, EcmStage2). Designed for
// debugging slow 50d+/60d cofactor runs and tuning W5 T5 GNFS_SURVIVAL_*
// thresholds when users need per-stage visibility.
//
// What this helper actually delivers:
//   * 6 std::atomic<uint64_t> nanosecond accumulators (one per stage)
//   * 6 std::atomic<uint64_t> call counters (one per stage)
//   * RAII StageTimer that samples steady_clock at ctor + dtor
//   * Process-singleton stats accessor (persists across pipeline runs)
//   * Format / print helpers that emit a single-line summary
//
// What this helper does NOT do:
//   * It does NOT modify any cofactor algorithm. Helper-only future-infra.
//   * It does NOT auto-instrument the cofactor pipeline. Callers wishing to
//     measure must explicitly drop `StageTimer t(CofactorStage::EcmStage1);`
//     at the scope they want to attribute time to.
//   * It does NOT print summary automatically on pipeline exit. Caller must
//     call `print_cofactor_timing_summary()` (or call `format_*` and route
//     the string itself).
//
// Complementary to W5 T5 GNFS_SURVIVAL_FILTER / GNFS_SURVIVAL_THRESHOLD:
// the survival predictor estimates which cofactors are worth running the
// full pipeline on. This telemetry measures how long each stage actually
// took when it did run. Together they let users tune the threshold by
// observing the wall-time gain from raising the threshold (e.g., does
// dropping ECM Stage 2 calls actually save 70% of cofactor time? telemetry
// answers).
//
// ENV control:
//   * GNFS_COFACTOR_TIMING_ENABLE=1  → timing enabled (clock samples + atomic ops)
//   * unset / "0" / anything else    → disabled (default, zero overhead:
//                                       StageTimer ctor + dtor become no-ops,
//                                       NO clock samples, NO atomic ops)
//
// Concurrency:
//   * All atomic operations use std::memory_order_relaxed. Telemetry is
//     fundamentally racey across threads (two timers in different threads
//     can both update total_ns concurrently); the relaxed ordering is
//     correct here because we never read back values to drive control
//     flow, only to format the summary. The total visible from one thread
//     to another may lag by a few ns but is eventually consistent.
//   * Concurrent StageTimer instances from different threads, even on the
//     same CofactorStage, accumulate atomically without races.
//
// Process singleton storage strategy:
//   * `cofactor_timing_stats()` returns a reference to a function-local
//     static `StageTimingStats`. This is the same idiom used by
//     `survival_stats()` in survival_predictor.hpp (W5 T5). Function-local
//     static gives us guaranteed C++11 thread-safe one-time initialization
//     and ODR safety across translation units.
//   * Choice rationale: `inline` namespace-scope variable would also work
//     (C++17), but function-local static is what the cofactor module
//     already uses for telemetry singletons, so we follow that convention
//     for codebase consistency.
//
// Move semantics:
//   * StageTimer is non-copyable, movable. The moved-from timer is marked
//     `moved_from_ = true` so its destructor will NOT double-increment.
//     The moved-to timer assumes responsibility for the elapsed-time
//     accumulation when it goes out of scope.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <iostream>

namespace gnfs::cofactor {

/// Enumeration of cofactor pipeline stages tracked by the telemetry helper.
///
/// Ordering matches the canonical dispatch order in
/// `smooth_check.hpp::classify_cofactor`:
///   TrialDivision → SQUFOF → BrentPollardRho (opt-in) → Pollard rho (legacy)
///   → ECM Stage 1 → ECM Stage 2.
///
/// `kNumStages` is the sentinel count for fixed-size arrays. It is NOT a
/// valid stage to pass to StageTimer.
enum class CofactorStage : int {
    TrialDivision   = 0,
    Squfof          = 1,
    BrentPollardRho = 2,
    PollardRho      = 3,
    EcmStage1       = 4,
    EcmStage2       = 5,
    kNumStages      = 6,
};

/// Human-readable stage name (for formatting summary output).
[[nodiscard]] inline const char* stage_name(CofactorStage stage) noexcept {
    switch (stage) {
        case CofactorStage::TrialDivision:   return "trial";
        case CofactorStage::Squfof:          return "squfof";
        case CofactorStage::BrentPollardRho: return "brent_rho";
        case CofactorStage::PollardRho:      return "pollard_rho";
        case CofactorStage::EcmStage1:       return "ecm_s1";
        case CofactorStage::EcmStage2:       return "ecm_s2";
        case CofactorStage::kNumStages:      return "?";
    }
    return "?";
}

/// Process-singleton atomic statistics for cofactor stage timing.
///
/// One atomic nanosecond accumulator + one atomic call counter per stage.
/// Always present in the process; updates only happen when the telemetry
/// gate `cofactor_timing_enabled()` returns true (i.e., when ENV
/// `GNFS_COFACTOR_TIMING_ENABLE=1` is set).
///
/// All atomic operations use memory_order_relaxed. Telemetry never drives
/// control flow, so eventual consistency is sufficient.
struct StageTimingStats {
    std::array<std::atomic<uint64_t>,
               static_cast<int>(CofactorStage::kNumStages)> total_ns;
    std::array<std::atomic<uint64_t>,
               static_cast<int>(CofactorStage::kNumStages)> call_count;

    StageTimingStats() {
        for (auto& a : total_ns) a.store(0, std::memory_order_relaxed);
        for (auto& a : call_count) a.store(0, std::memory_order_relaxed);
    }

    /// Zero every counter. Useful for per-pipeline-run isolation: caller
    /// invokes `reset()` at pipeline entry to scope the telemetry to that
    /// run only.
    void reset() noexcept {
        for (auto& a : total_ns) a.store(0, std::memory_order_relaxed);
        for (auto& a : call_count) a.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t total_ns_for(CofactorStage stage) const noexcept {
        return total_ns[static_cast<std::size_t>(stage)].load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t call_count_for(CofactorStage stage) const noexcept {
        return call_count[static_cast<std::size_t>(stage)].load(std::memory_order_relaxed);
    }
};

namespace detail {

inline std::once_flag& cofactor_timing_env_once_flag() {
    static std::once_flag flag;
    return flag;
}

inline std::atomic<bool>& cofactor_timing_env_value() {
    static std::atomic<bool> value{false};
    return value;
}

inline bool parse_cofactor_timing_env() {
    const char* env = std::getenv("GNFS_COFACTOR_TIMING_ENABLE");
    if (env == nullptr) return false;
    // Strict matching: only the exact string "1" enables. All other values
    // ("", "0", "true", "yes", "01", " 1", "1 ", "TRUE") evaluate to false.
    // This mirrors the boolean ENV convention used by GNFS_INTEGER_SCRATCH_POOL,
    // GNFS_FILTER_RADIX_SORT, GNFS_V0_WEIGHT3.
    return env[0] == '1' && env[1] == '\0';
}

}  // namespace detail

/// Read GNFS_COFACTOR_TIMING_ENABLE and return whether the telemetry is on.
///
/// Cached via std::call_once + std::atomic<bool>. The first invocation
/// parses the environment; subsequent invocations return the cached value
/// with only an atomic load (no getenv on the hot path).
///
///   * Unset / empty: returns false (disabled)
///   * "1" (exact): returns true (enabled)
///   * Any other value: returns false
[[nodiscard]] inline bool cofactor_timing_enabled() noexcept {
    std::call_once(detail::cofactor_timing_env_once_flag(), []() {
        detail::cofactor_timing_env_value().store(
            detail::parse_cofactor_timing_env(),
            std::memory_order_release);
    });
    return detail::cofactor_timing_env_value().load(std::memory_order_acquire);
}

/// Test-only: re-parse GNFS_COFACTOR_TIMING_ENABLE.
///
/// NOT thread-safe — call only from single-threaded test setup. The cached
/// once_flag is not reset; this helper directly overwrites the cached
/// atomic value, so a subsequent call to `cofactor_timing_enabled()`
/// returns the freshly parsed value without re-invoking the once_flag
/// initializer.
inline void cofactor_timing_reset_env_cache_for_testing() noexcept {
    detail::cofactor_timing_env_value().store(
        detail::parse_cofactor_timing_env(),
        std::memory_order_release);
}

/// Process-singleton accessor for cofactor stage timing stats.
///
/// Returns a reference to a function-local static StageTimingStats. The
/// underlying storage persists across pipeline runs unless `reset()` is
/// called explicitly. Concurrent callers from any thread are safe because
/// the underlying atomic operations are race-free.
inline StageTimingStats& cofactor_timing_stats() {
    static StageTimingStats stats;
    return stats;
}

/// RAII timer for one cofactor stage.
///
/// Construction:
///   * Captures the stage tag.
///   * If `cofactor_timing_enabled()` returns true at ctor time, samples
///     steady_clock::now() and saves it in start_. The local `enabled_`
///     boolean is cached so the dtor does not re-check the gate (and so a
///     toggle of the env between ctor and dtor cannot leave a half-timed
///     scope).
///   * If disabled, does NOT call steady_clock::now() and does NOT touch
///     the singleton stats. Zero overhead per scope: ctor + dtor are
///     essentially a single boolean load and a couple of trivial
///     assignments.
///
/// Destruction:
///   * If enabled_ AND not moved-from: samples steady_clock::now(), computes
///     elapsed in nanoseconds, fetch_add into total_ns[stage] and
///     fetch_add 1 into call_count[stage] (both relaxed).
///   * Otherwise: no-op.
///
/// Move semantics:
///   * Non-copyable: a timer represents a unique scope of time being
///     measured. Copying would double-count.
///   * Move-construct / move-assign transfer the (stage, start_, enabled_)
///     state. The moved-from timer sets moved_from_ = true so its dtor
///     does NOT double-increment.
///
/// Nested timers (e.g., a TrialDivision timer that encloses an EcmStage1
/// timer) each accumulate to their own stage independently. The outer
/// timer's elapsed time will naturally include the inner timer's elapsed
/// time, which is the intended semantics (caller is responsible for
/// deciding where to place each timer to express the desired attribution).
class StageTimer {
public:
    explicit StageTimer(CofactorStage stage) noexcept
        : stage_(stage),
          enabled_(cofactor_timing_enabled()),
          start_{},
          moved_from_(false) {
        if (enabled_) {
            start_ = std::chrono::steady_clock::now();
        }
    }

    ~StageTimer() {
        if (!moved_from_ && enabled_) {
            const auto end = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_)
                    .count();
            // Defensive: clock skew on some platforms can return negative
            // deltas. Coerce negative to 0 to keep accumulator monotone.
            const uint64_t add = (elapsed > 0) ? static_cast<uint64_t>(elapsed)
                                               : 0ULL;
            auto& stats = cofactor_timing_stats();
            stats.total_ns[static_cast<std::size_t>(stage_)]
                .fetch_add(add, std::memory_order_relaxed);
            stats.call_count[static_cast<std::size_t>(stage_)]
                .fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Non-copyable.
    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;

    // Movable: transfer ownership of the in-flight measurement. The
    // moved-from timer is marked so its dtor does not double-increment.
    StageTimer(StageTimer&& other) noexcept
        : stage_(other.stage_),
          enabled_(other.enabled_),
          start_(other.start_),
          moved_from_(other.moved_from_) {
        other.moved_from_ = true;
    }

    StageTimer& operator=(StageTimer&& other) noexcept {
        if (this != &other) {
            // Before adopting the new state, finalize the current
            // measurement if this timer was actively tracking.
            if (!moved_from_ && enabled_) {
                const auto end = std::chrono::steady_clock::now();
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - start_).count();
                const uint64_t add = (elapsed > 0) ? static_cast<uint64_t>(elapsed)
                                                   : 0ULL;
                auto& stats = cofactor_timing_stats();
                stats.total_ns[static_cast<std::size_t>(stage_)]
                    .fetch_add(add, std::memory_order_relaxed);
                stats.call_count[static_cast<std::size_t>(stage_)]
                    .fetch_add(1, std::memory_order_relaxed);
            }
            stage_ = other.stage_;
            enabled_ = other.enabled_;
            start_ = other.start_;
            moved_from_ = other.moved_from_;
            other.moved_from_ = true;
        }
        return *this;
    }

private:
    CofactorStage stage_;
    bool enabled_;
    std::chrono::steady_clock::time_point start_;
    bool moved_from_;
};

/// Format the current cofactor timing stats as a single human-readable line.
///
/// Format when telemetry was disabled the entire run (no counters ever
/// updated by any StageTimer):
///   "[cofactor_timing] disabled"
///
/// When telemetry is enabled (or was enabled at some point), regardless of
/// whether any timers actually ran:
///   "[cofactor_timing] trial=<ns>ns/<calls>calls squfof=... brent_rho=...
///    pollard_rho=... ecm_s1=... ecm_s2=..."
///
/// We treat "all counters are zero AND telemetry never enabled" as the
/// disabled-summary case. If telemetry IS enabled but no timers fired,
/// callers still see the full breakdown with zeroes (useful for verifying
/// that the gate is on and the missing time means nothing was measured).
[[nodiscard]] inline std::string format_cofactor_timing_summary() {
    if (!cofactor_timing_enabled()) {
        return "[cofactor_timing] disabled";
    }
    auto& stats = cofactor_timing_stats();
    std::ostringstream oss;
    oss << "[cofactor_timing]";
    for (int i = 0; i < static_cast<int>(CofactorStage::kNumStages); ++i) {
        const auto stage = static_cast<CofactorStage>(i);
        oss << ' ' << stage_name(stage) << '='
            << stats.total_ns_for(stage) << "ns/"
            << stats.call_count_for(stage) << "calls";
    }
    return oss.str();
}

/// Convenience: print the summary line to stderr with std::flush.
///
/// Use at pipeline exit (or whenever the caller wants a snapshot dumped).
/// Does not call reset(); subsequent measurements continue to accumulate.
inline void print_cofactor_timing_summary() {
    std::cerr << format_cofactor_timing_summary() << std::endl;
}

}  // namespace gnfs::cofactor
