#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

namespace gnfs::cofactor {

/// Smoothness survival rate predictor for cofactor early reject.
///
/// Position in the dispatch chain (when ENV `GNFS_SURVIVAL_FILTER=1`):
///   survival_filter (this) → trial-div → SQUFOF → BrentPollardRho → Pollard rho (legacy) → ECM
///
/// Algorithm: Dickman's rho function ρ(u) estimates the density of
/// y-smooth integers up to x, where u = log(x) / log(y). For a cofactor
/// of `c_bits` and smoothness bound of `B_bits`, u_smooth = c_bits / B_bits
/// gives the proportion of integers near 2^c_bits that are 2^B_bits-smooth.
///
/// With large primes (LP), survival probability lifts to ρ(u_smooth) + LP
/// term. We use a conservative two-term approximation:
///   survival ≈ ρ(u_smooth) + u_smooth · ρ(u_smooth - 1) · (1 - B/LP factor)
/// but for simplicity here we estimate via max(ρ(u_smooth), ρ(u_lp)) where
/// u_lp = cofactor_bits / lp_bound_bits. This is a permissive lower bound
/// on survival probability — never rejects when LP could save us.
///
/// References:
///   * Dickman, K. (1930). "On the frequency of numbers containing prime
///     factors of a certain relative magnitude". Arkiv för matematik.
///   * Granville, A. (2008). "Smooth numbers: Computational number theory
///     and beyond". MSRI Publications.
///   * van de Lune & Wattel (1969). Numerical values of Dickman's rho.

namespace detail {

/// Dickman ρ(u) values for integer u ∈ {2, 3, ..., 10}.
/// (For u ∈ [1, 2], the closed form ρ(u) = 1 - ln(u) is used directly.)
///
/// Source: high-precision tabulation matching Bach & Peralta (1996) and
/// van de Lune & Wattel (1969). For u > 10, asymptotic decay
/// ρ(u) ~ u^{-u} · (1 + o(1)) takes over; we evaluate via that closed form.
///
/// Indexing: table_integer[k] = ρ(2 + k), k ∈ {0, ..., 8}.
inline const double* dickman_table_integer() {
    static const double table[9] = {
        3.068528194401e-01,  // ρ(2)
        4.860838829061e-02,  // ρ(3)
        4.910925648681e-03,  // ρ(4)
        3.547247885221e-04,  // ρ(5)
        1.964939005051e-05,  // ρ(6)
        8.745961700586e-07,  // ρ(7)
        3.232169325490e-08,  // ρ(8)
        1.016247775265e-09,  // ρ(9)
        2.770171395103e-11,  // ρ(10)
    };
    return table;
}

/// log of ρ at integer u, for log-linear interpolation between integers.
/// This gives much better accuracy than linear interpolation on ρ directly
/// because ρ decays super-exponentially.
inline const double* dickman_log_table() {
    // Pre-computed log of each table_integer entry to avoid per-call log().
    static const double log_table[9] = {
        -1.181584081421e+00,  // log ρ(2)
        -3.024309366533e+00,  // log ρ(3)
        -5.315111346263e+00,  // log ρ(4)
        -7.945202132122e+00,  // log ρ(5)
        -1.084319015935e+01,  // log ρ(6)
        -1.395039300921e+01,  // log ρ(7)
        -1.724518108538e+01,  // log ρ(8)
        -2.070434311466e+01,  // log ρ(9)
        -2.430583961547e+01,  // log ρ(10)
    };
    return log_table;
}

} // namespace detail

/// Dickman's ρ(u) — fraction of integers near N that are N^{1/u}-smooth.
///
/// Properties:
///   * ρ(u) = 1 for u ≤ 1 (every positive integer is trivially smooth)
///   * ρ(u) is continuous, strictly decreasing, log-convex for u > 0
///   * ρ(u) → 0 as u → ∞ via ρ(u) ~ u^{-u} (asymptotic, Hildebrand 1986)
///   * ρ(2) = 1 - ln(2) ≈ 0.30685
///   * ρ(3) ≈ 0.04860
///   * ρ(u) integrates against psi(x, y) = # of y-smooth integers ≤ x
///
/// Implementation:
///   * u ≤ 1: returns 1.0 exactly
///   * 1 < u ≤ 2: closed form ρ(u) = 1 - ln(u) (exact)
///   * 2 < u ≤ 10: log-linear interpolation on integer-anchored table
///     (more accurate than linear-in-rho given super-exponential decay)
///   * u > 10: asymptotic ρ(u) ≈ u^{-u} (worst-case overestimate of
///     density, harmless: makes survival predictor more permissive)
///   * NaN / negative u: clamped to 1.0 (defensive default = no reject)
[[nodiscard]] inline double dickman_rho(double u) {
    if (!(u > 0.0)) return 1.0;     // catches NaN, negative, zero
    if (u <= 1.0) return 1.0;
    if (u <= 2.0) {
        // Exact closed form on [1, 2]: ρ(u) = 1 - ln(u).
        return 1.0 - std::log(u);
    }
    if (u >= 10.0) {
        // Asymptotic decay; for u >> 10 ρ(u) is astronomically small.
        // u^{-u} = exp(-u * ln(u)). Guard against -inf.
        const double log_u = std::log(u);
        const double exponent = -u * log_u;
        if (exponent < -700.0) return 0.0;  // underflow region
        return std::exp(exponent);
    }
    // u ∈ (2, 10): log-linear interpolation between adjacent integer
    // anchors. Indexing: idx = floor(u) - 2, frac = u - floor(u).
    // log ρ(u) ≈ (1 - frac) * log ρ(idx + 2) + frac * log ρ(idx + 3).
    const int idx = static_cast<int>(u) - 2;  // 0 .. 7
    const double frac = u - static_cast<double>(static_cast<int>(u));
    const double* log_table = detail::dickman_log_table();
    const double log_v = (1.0 - frac) * log_table[idx] + frac * log_table[idx + 1];
    return std::exp(log_v);
}

/// Estimate cofactor survival probability through the cofactor pipeline.
///
/// Two-criterion conservative estimate:
///   u_smooth = cofactor_bits / smoothness_bound_bits
///       (entirely B-smooth path)
///   u_lp    = cofactor_bits / lp_bound_bits
///       (B-smooth + at most one prime in (B, LP] path; we approximate
///        this as "LP-smooth" for the upper-bound estimate)
///
/// Survival probability lower bound: max(ρ(u_smooth), ρ(u_lp)).
/// Taking the max ensures the LP path (which is more permissive) lifts
/// the estimate when applicable. This is intentionally permissive —
/// when in doubt, prefer not to reject (avoids false-negative loss of
/// smooth relations).
///
/// Special cases:
///   * smoothness_bound_bits == 0: returns 1.0 (no info, do not reject)
///   * cofactor_bits == 0: returns 1.0 (cofactor is 1, trivially smooth)
///   * cofactor_bits <= smoothness_bound_bits: returns 1.0 (trivially
///     within B, no rejection ever)
[[nodiscard]] inline double estimate_survival(
        uint64_t cofactor_bits,
        uint64_t smoothness_bound_bits,
        uint64_t lp_bound_bits) {
    if (smoothness_bound_bits == 0) return 1.0;
    if (cofactor_bits == 0) return 1.0;
    if (cofactor_bits <= smoothness_bound_bits) return 1.0;

    const double u_smooth = static_cast<double>(cofactor_bits) /
                            static_cast<double>(smoothness_bound_bits);
    const double p_smooth = dickman_rho(u_smooth);

    double p_lp = 0.0;
    if (lp_bound_bits > 0 && cofactor_bits > lp_bound_bits) {
        const double u_lp = static_cast<double>(cofactor_bits) /
                            static_cast<double>(lp_bound_bits);
        p_lp = dickman_rho(u_lp);
    } else if (lp_bound_bits > 0) {
        // cofactor_bits <= lp_bound_bits → entirely within LP space
        p_lp = 1.0;
    }

    return std::max(p_smooth, p_lp);
}

/// ENV-gate cache for `GNFS_SURVIVAL_FILTER`. Parsed once (first call)
/// and cached. Returns true iff env is set to "1" (any other value
/// disables). Default = false (filter disabled, zero overhead).
[[nodiscard]] inline bool survival_filter_enabled() {
    static const bool enabled = []() {
        const char* env = std::getenv("GNFS_SURVIVAL_FILTER");
        return env != nullptr && env[0] == '1' && env[1] == '\0';
    }();
    return enabled;
}

/// ENV-gate cache for `GNFS_SURVIVAL_THRESHOLD`. Parsed once (first call)
/// and cached. Default = 0.0 (no rejection even when filter is enabled).
///
/// Threshold values:
///   * 0.0     — no rejection (filter ON but neutered; safe default)
///   * 1e-12   — reject only catastrophically unlikely cofactors
///   * 1e-6    — moderate: reject if survival < 0.0001%
///   * 1e-3    — aggressive: reject if survival < 0.1%
///   * 0.01    — very aggressive: may lose 5-10% smooth relations
///
/// Values outside [0.0, 1.0] are clamped to default 0.0.
[[nodiscard]] inline double survival_threshold() {
    static const double threshold = []() {
        const char* env = std::getenv("GNFS_SURVIVAL_THRESHOLD");
        if (env == nullptr || env[0] == '\0') return 0.0;
        char* end = nullptr;
        const double v = std::strtod(env, &end);
        if (end == env) return 0.0;          // parse failure
        if (!(v >= 0.0 && v <= 1.0)) return 0.0;  // out of range, NaN
        return v;
    }();
    return threshold;
}

/// Top-level reject decision.
///
/// Returns true iff:
///   1. `GNFS_SURVIVAL_FILTER=1` is set, AND
///   2. `GNFS_SURVIVAL_THRESHOLD` > 0 (strictly greater), AND
///   3. estimate_survival(...) < threshold
///
/// All three conditions must hold; the threshold == 0 path always
/// returns false, equivalent to filter being disabled. This is the
/// guaranteed-correct path for "filter enabled but no rejection".
[[nodiscard]] inline bool should_reject_cofactor(
        uint64_t cofactor_bits,
        uint64_t smoothness_bound_bits,
        uint64_t lp_bound_bits) {
    if (!survival_filter_enabled()) return false;
    const double th = survival_threshold();
    if (!(th > 0.0)) return false;     // threshold == 0 ⇒ never reject
    const double p = estimate_survival(cofactor_bits, smoothness_bound_bits, lp_bound_bits);
    return p < th;
}

/// Telemetry counters for survival predictor decisions.
///
/// Updated by integration points in smooth_check.hpp (and elsewhere when
/// the cofactor pipeline runs). Atomic for thread-safety across sieve
/// workers. Pipeline / test harness reads at end of run.
struct SurvivalPredictorStats {
    std::atomic<uint64_t> predictor_rejects{0};
    std::atomic<uint64_t> predictor_passes_then_failed{0};
    std::atomic<uint64_t> predictor_passes_then_smooth{0};

    void record_reject() noexcept {
        predictor_rejects.fetch_add(1, std::memory_order_relaxed);
    }
    void record_pass_failed() noexcept {
        predictor_passes_then_failed.fetch_add(1, std::memory_order_relaxed);
    }
    void record_pass_smooth() noexcept {
        predictor_passes_then_smooth.fetch_add(1, std::memory_order_relaxed);
    }
    void reset() noexcept {
        predictor_rejects.store(0, std::memory_order_relaxed);
        predictor_passes_then_failed.store(0, std::memory_order_relaxed);
        predictor_passes_then_smooth.store(0, std::memory_order_relaxed);
    }
};

/// Process-wide stats singleton.
inline SurvivalPredictorStats& survival_stats() {
    static SurvivalPredictorStats stats;
    return stats;
}

} // namespace gnfs::cofactor
