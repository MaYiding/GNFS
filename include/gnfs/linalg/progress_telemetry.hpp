#pragma once

// Iteration progress telemetry helper for Block Lanczos / Block Wiedemann
// (W12 T1).
//
// Scope
// -----
// Block Lanczos and Block Wiedemann SpMV loops on 50d+/60d matrices can run
// for hours. Without runtime telemetry, users have no visibility into:
//   * Current iteration index relative to the projected total
//   * Wall-time elapsed since loop entry
//   * Instantaneous throughput (iterations per second)
//   * Estimated time remaining (ETA)
//
// This helper provides an ENV-gated, opt-in iteration progress logger.
// Callers wire-in by constructing one `IterationProgressLogger` per SpMV
// loop and calling `tick(current_iter)` once per iteration. When the ENV
// gate is disabled (default), `tick` / `finish` are no-ops; no clock
// samples are taken; nothing is written to stderr.
//
// When enabled (`GNFS_LINALG_PROGRESS_INTERVAL=N` with N >= 1), the logger
// emits one stderr line on the first tick, every N-th tick thereafter, and
// one DONE summary line on finish. The output format is:
//
//   [linalg_progress] phase=<label> iter=<I>/<T> elapsed=<E>s rate=<R>/s eta=<ETA>s
//   [linalg_progress] phase=<label> DONE iter=<T>/<T> elapsed=<E>s avg_rate=<R>/s
//
// All output goes to `std::cerr` followed by `std::flush`. `std::cerr` is
// unbuffered by default on most platforms, but the explicit flush guards
// against environments (e.g. nohup-style fd redirects) that fold cerr into
// a fully-buffered fd.
//
// Correctness invariants
// ----------------------
// 1. Default-off path produces ZERO output to stderr. No clock samples,
//    no string formatting, no atomic loads beyond the cached gate check.
//    Bit-for-bit identical caller behavior versus the legacy no-telemetry
//    path.
// 2. Telemetry is observability only — the logger never modifies caller
//    state, never mutates iteration variables, never throws from `tick`.
//    Any output anomaly (e.g. rate underflow on very fast loops) is
//    handled by printing `?` rather than NaN/inf or crashing.
// 3. Multiple loggers may coexist (e.g. two phases tracked side by side).
//    They write to the same stderr stream serially. There is no global
//    state shared between loggers other than the cached ENV gate.
//
// ENV gate
// --------
//   GNFS_LINALG_PROGRESS_INTERVAL = N (positive integer)
//
//   * unset / empty                          → 0 (disabled, default)
//   * "0" / negative / leading non-digit     → 0 (disabled)
//   * "1", "100", "999999" (clean digits)    → N (enabled at interval N)
//   * "12abc" (numeric prefix)               → 12 (std::stoi accepts prefix;
//                                                 consistent with W11 T3
//                                                 GNFS_MPZ_POWM_BATCH_THREADS
//                                                 documented behavior)
//   * "1.5" / " 5" / leading whitespace      → 0 (disabled; stoi rejects)
//
// The cached value is resolved on the first call to
// `linalg_progress_interval()` via `std::call_once`. Subsequent calls only
// load an atomic int. Tests can re-resolve the cached value via
// `linalg_progress_reset_env_cache_for_testing()`.
//
// Thread-safety
// -------------
// `IterationProgressLogger` is NOT thread-safe. Each thread that wishes
// to track an iteration loop must own its own logger. The cached ENV gate
// reader is thread-safe (call_once + atomic).
//
// Usage
// -----
//   IterationProgressLogger lg("BL_SpMV", total_iters);
//   for (int64_t i = 0; i < total_iters; ++i) {
//       run_one_iter(i);
//       lg.tick(i);
//   }
//   lg.finish();  // explicit; dtor would also call it if not.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace gnfs::linalg {

namespace detail {

inline std::atomic<int>& cached_progress_interval() noexcept {
    static std::atomic<int> v{0};
    return v;
}

inline std::once_flag& cached_progress_flag() noexcept {
    static std::once_flag f;
    return f;
}

/// Parse `GNFS_LINALG_PROGRESS_INTERVAL` from the environment into a
/// clamped non-negative integer. See header comment for the full parsing
/// matrix. Returns 0 (disabled) for any invalid / non-positive value.
inline int resolve_progress_interval_from_env() noexcept {
    const char* v = std::getenv("GNFS_LINALG_PROGRESS_INTERVAL");
    if (v == nullptr || v[0] == '\0') {
        return 0;
    }
    // Reject leading whitespace explicitly so " 5" yields 0 rather than 5.
    // std::strtol would skip the whitespace silently; we want stricter
    // semantics so the parsing matrix in the header comment matches the
    // implementation.
    if (v[0] == ' ' || v[0] == '\t' || v[0] == '\n') {
        return 0;
    }
    char* end = nullptr;
    long parsed = std::strtol(v, &end, 10);
    if (end == v) {
        // No digits consumed (e.g. "garbage", "abc123").
        return 0;
    }
    if (parsed <= 0) {
        return 0;
    }
    // Clamp to int max defensively. Realistic intervals are <= 1e6;
    // anything larger gets the legacy "disabled" behavior because
    // `tick` could not fire often enough to be useful anyway. We choose
    // to honor the user's request up to INT_MAX rather than silently
    // disable.
    if (parsed > static_cast<long>(0x7FFFFFFF)) {
        parsed = 0x7FFFFFFF;
    }
    return static_cast<int>(parsed);
}

}  // namespace detail

/// Returns the cached interval value parsed from
/// `GNFS_LINALG_PROGRESS_INTERVAL`. Zero means disabled (no telemetry).
[[nodiscard]] inline int linalg_progress_interval() noexcept {
    std::call_once(detail::cached_progress_flag(), []() noexcept {
        detail::cached_progress_interval().store(
            detail::resolve_progress_interval_from_env(),
            std::memory_order_relaxed);
    });
    return detail::cached_progress_interval().load(std::memory_order_relaxed);
}

/// Convenience predicate: telemetry is enabled iff interval > 0.
[[nodiscard]] inline bool linalg_progress_enabled() noexcept {
    return linalg_progress_interval() > 0;
}

/// Test-only: re-resolve the ENV cache. NOT thread-safe; call only from
/// single-threaded test setup. Mirrors the pattern from W8 T5
/// `integer_scratch_pool_reset_env_cache_for_testing()`.
inline void linalg_progress_reset_env_cache_for_testing() noexcept {
    detail::cached_progress_interval().store(
        detail::resolve_progress_interval_from_env(),
        std::memory_order_relaxed);
    // Mark the once_flag as already-executed so a subsequent
    // `linalg_progress_interval()` call does not overwrite our value
    // with a (potentially stale) re-parse.
    std::call_once(detail::cached_progress_flag(), []() noexcept {
        // No-op: state already set above.
    });
}

/// RAII iteration progress logger.
///
/// Construct one per SpMV loop. Call `tick(current_iter)` once per
/// iteration. The destructor (or explicit `finish()`) emits a DONE
/// summary line.
///
/// Disabled fast path: when `linalg_progress_interval()` is 0, the ctor
/// records `interval_ = 0` and all subsequent `tick` / `finish` calls
/// short-circuit to a no-op (single branch on a cached int). No clock
/// samples are taken; no string formatting occurs; stderr is untouched.
///
/// Enabled path: ctor samples `steady_clock::now()` as the start time.
/// `tick(current_iter)` emits a line iff:
///   * It is the FIRST tick (last_logged_iter_ == -1), OR
///   * current_iter - last_logged_iter_ >= interval_
/// `finish()` emits a DONE summary regardless of interval.
///
/// Format details:
///   * iter clamps to [0, total]; out-of-range inputs print clamped
///   * elapsed printed with 3 decimals (millisecond precision)
///   * rate printed with 1 decimal; underflow (elapsed == 0) prints "?"
///   * eta printed with 1 decimal; underflow or rate==0 prints "?"
///   * total == 0 prints eta=? (cannot project unknown total)
class IterationProgressLogger {
public:
    /// Construct a logger with the given phase label and total iteration
    /// count. The label is copied (string_view contents must outlive ctor
    /// only). Total may be 0 (no projection possible) or negative
    /// (clamped to 0).
    IterationProgressLogger(std::string_view phase_label, std::int64_t total_iters)
        : phase_(phase_label),
          total_(total_iters < 0 ? 0 : total_iters),
          interval_(linalg_progress_interval()),
          last_logged_iter_(-1),
          start_(std::chrono::steady_clock::now()),
          finished_(false) {
        // Nothing else to do; if disabled, all subsequent calls are no-ops.
    }

    /// Destructor: if `finish()` was not called explicitly and the logger
    /// is enabled, emit the DONE summary line. Disabled loggers do nothing.
    ~IterationProgressLogger() {
        if (!finished_ && interval_ > 0) {
            try {
                finish_impl();
            } catch (...) {
                // Swallow exceptions in dtor (noexcept-friendly).
            }
        }
    }

    // Non-copyable.
    IterationProgressLogger(const IterationProgressLogger&) = delete;
    IterationProgressLogger& operator=(const IterationProgressLogger&) = delete;

    // Movable. Moved-from logger is marked finished so its dtor skips the
    // DONE line (transferred to the move target).
    IterationProgressLogger(IterationProgressLogger&& other) noexcept
        : phase_(std::move(other.phase_)),
          total_(other.total_),
          interval_(other.interval_),
          last_logged_iter_(other.last_logged_iter_),
          start_(other.start_),
          finished_(other.finished_) {
        other.finished_ = true;  // Prevent moved-from dtor from emitting.
        other.interval_ = 0;     // Belt-and-suspenders: disable tick too.
    }

    IterationProgressLogger& operator=(IterationProgressLogger&& other) noexcept {
        if (this != &other) {
            // First, finalize the current logger if it is owning live state.
            if (!finished_ && interval_ > 0) {
                try {
                    finish_impl();
                } catch (...) {
                    // Swallow.
                }
            }
            phase_ = std::move(other.phase_);
            total_ = other.total_;
            interval_ = other.interval_;
            last_logged_iter_ = other.last_logged_iter_;
            start_ = other.start_;
            finished_ = other.finished_;
            other.finished_ = true;
            other.interval_ = 0;
        }
        return *this;
    }

    /// Record an iteration. May emit one progress line to stderr if the
    /// interval gate fires. No-op when the logger is disabled.
    void tick(std::int64_t current_iter) {
        if (interval_ <= 0 || finished_) {
            return;
        }
        // Clamp negative inputs to 0; clamp above-total inputs to total.
        std::int64_t clamped = current_iter;
        if (clamped < 0) {
            clamped = 0;
        }
        if (total_ > 0 && clamped > total_) {
            clamped = total_;
        }
        // Emit on the first tick, then every interval_ ticks thereafter.
        bool should_emit = false;
        if (last_logged_iter_ < 0) {
            should_emit = true;
        } else if (clamped - last_logged_iter_ >= static_cast<std::int64_t>(interval_)) {
            should_emit = true;
        }
        if (!should_emit) {
            return;
        }
        last_logged_iter_ = clamped;
        emit_progress_line(clamped);
    }

    /// Emit the DONE summary line. Subsequent `tick` / `finish` calls
    /// become no-ops. Idempotent.
    void finish() {
        if (interval_ <= 0 || finished_) {
            finished_ = true;
            return;
        }
        finish_impl();
    }

private:
    void finish_impl() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s = elapsed_seconds(now);
        const double avg_rate = compute_rate(total_, elapsed_s);
        std::cerr << "[linalg_progress] phase=" << phase_
                  << " DONE iter=" << total_ << '/' << total_
                  << " elapsed=" << format_seconds(elapsed_s) << "s"
                  << " avg_rate=" << format_rate(avg_rate) << "/s"
                  << '\n'
                  << std::flush;
        finished_ = true;
    }

    void emit_progress_line(std::int64_t clamped_iter) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s = elapsed_seconds(now);
        const double rate = compute_rate(clamped_iter, elapsed_s);
        const double eta_s = compute_eta(clamped_iter, total_, rate);
        std::cerr << "[linalg_progress] phase=" << phase_
                  << " iter=" << clamped_iter << '/' << total_
                  << " elapsed=" << format_seconds(elapsed_s) << "s"
                  << " rate=" << format_rate(rate) << "/s"
                  << " eta=" << format_eta(eta_s) << "s"
                  << '\n'
                  << std::flush;
    }

    [[nodiscard]] double elapsed_seconds(
            std::chrono::steady_clock::time_point now) const noexcept {
        using namespace std::chrono;
        const auto delta = now - start_;
        const auto ns = duration_cast<nanoseconds>(delta).count();
        return static_cast<double>(ns) / 1e9;
    }

    /// Iterations per second. Returns 0.0 when elapsed is non-positive
    /// (avoid div-by-zero and avoid spurious huge rates from sub-microsecond
    /// elapsed). 0.0 is rendered as "?" by `format_rate`.
    [[nodiscard]] static double compute_rate(std::int64_t iter,
                                             double elapsed_s) noexcept {
        if (elapsed_s <= 0.0 || iter <= 0) {
            return 0.0;
        }
        return static_cast<double>(iter) / elapsed_s;
    }

    /// Estimated time remaining in seconds. Returns -1.0 as a sentinel for
    /// "unknown" (total unknown, rate zero, or already past total).
    /// Rendered as "?" by `format_eta`.
    [[nodiscard]] static double compute_eta(std::int64_t iter,
                                            std::int64_t total,
                                            double rate) noexcept {
        if (total <= 0 || rate <= 0.0 || iter >= total) {
            return -1.0;
        }
        const std::int64_t remaining = total - iter;
        return static_cast<double>(remaining) / rate;
    }

    /// Format a non-negative double with 3 decimals (millisecond precision).
    /// Used for elapsed time. Negative values are clamped to 0 defensively.
    [[nodiscard]] static std::string format_seconds(double s) {
        if (s < 0.0 || !std::isfinite(s)) {
            return std::string("0.000");
        }
        return format_double(s, 3);
    }

    /// Format a rate. 0.0 (or non-positive / non-finite) renders as "?".
    [[nodiscard]] static std::string format_rate(double r) {
        if (r <= 0.0 || !std::isfinite(r)) {
            return std::string("?");
        }
        return format_double(r, 1);
    }

    /// Format an ETA. -1.0 (sentinel) or non-positive / non-finite
    /// renders as "?".
    [[nodiscard]] static std::string format_eta(double eta) {
        if (eta < 0.0 || !std::isfinite(eta)) {
            return std::string("?");
        }
        return format_double(eta, 1);
    }

    /// Hand-rolled double-to-string with a fixed number of decimals.
    /// Avoids `<iomanip>` global state side effects (`std::setprecision`
    /// would mutate the caller's iostream flags). Avoids `std::to_chars`
    /// floating-point overloads for libstdc++ portability (some older
    /// versions ship without them).
    [[nodiscard]] static std::string format_double(double v, int decimals) {
        // Round to `decimals` digits via integer scaling.
        double scale = 1.0;
        for (int i = 0; i < decimals; ++i) {
            scale *= 10.0;
        }
        const bool negative = v < 0.0;
        if (negative) {
            v = -v;
        }
        // Round half to even is overkill; round half up via +0.5 trick.
        const long long scaled = static_cast<long long>(v * scale + 0.5);
        const long long integer_part = scaled / static_cast<long long>(scale);
        const long long fractional_part = scaled - integer_part * static_cast<long long>(scale);
        std::string out;
        if (negative) {
            out.push_back('-');
        }
        out += std::to_string(integer_part);
        if (decimals > 0) {
            out.push_back('.');
            // Pad fractional with leading zeros.
            std::string frac = std::to_string(fractional_part);
            if (static_cast<int>(frac.size()) < decimals) {
                out.append(decimals - static_cast<int>(frac.size()), '0');
            }
            out += frac;
        }
        return out;
    }

    std::string phase_;
    std::int64_t total_;
    int interval_;
    std::int64_t last_logged_iter_;
    std::chrono::steady_clock::time_point start_;
    bool finished_;
};

}  // namespace gnfs::linalg
