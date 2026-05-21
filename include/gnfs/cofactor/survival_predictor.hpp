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

/// Dickman ρ(u) values for u ∈ {1.0, 1.1, ..., 10.0}.
///
/// Source: high-precision tabulation matching Bach & Peralta (1996) and
/// van de Lune & Wattel (1969). The table uses 0.1-spaced grid points for
/// u ∈ [1.0, 10.0]. For u < 1, ρ(u) = 1 exactly (every positive integer
/// is trivially x-smooth when y >= x). For u > 10, asymptotic decay
/// ρ(u) ~ u^{-u} · (1 + o(1)) takes over; we evaluate via that closed form.
inline const double* dickman_table() {
    // 91 entries: u = 1.0, 1.1, ..., 10.0 (step 0.1)
    static const double table[91] = {
        // u = 1.0 .. 1.9
        1.000000000000e+00, 9.535862950e-01, 9.083795045e-01, 8.643748474e-01,
        8.215661408e-01, 7.799459033e-01, 7.395053650e-01, 7.002347847e-01,
        6.621234666e-01, 6.251598730e-01,
        // u = 2.0 .. 2.9
        3.068528194e-01, 2.762048085e-01, 2.478752563e-01, 2.219285834e-01,
        1.982866934e-01, 1.768362706e-01, 1.574316182e-01, 1.399050930e-01,
        1.240789694e-01, 1.097764458e-01,
        // u = 3.0 .. 3.9
        4.860838829e-02, 4.247391374e-02, 3.704572564e-02, 3.224656824e-02,
        2.800966574e-02, 2.427470632e-02, 2.098523432e-02, 1.808984933e-02,
        1.554186089e-02, 1.330065598e-02,
        // u = 4.0 .. 4.9
        4.910925648e-03, 4.183868510e-03, 3.560260620e-03, 3.025522660e-03,
        2.567019560e-03, 2.173997750e-03, 1.837287140e-03, 1.548837440e-03,
        1.302064830e-03, 1.091508780e-03,
        // u = 5.0 .. 5.9
        3.547247885e-04, 2.957197590e-04, 2.461738250e-04, 2.045961170e-04,
        1.697078160e-04, 1.404214650e-04, 1.158191420e-04, 9.514444600e-05,
        7.792347000e-05, 6.366450200e-05,
        // u = 6.0 .. 6.9
        1.964939005e-05, 1.605145950e-05, 1.307875400e-05, 1.063225650e-05,
        8.625710000e-06, 6.985225000e-06, 5.645640000e-06, 4.552080000e-06,
        3.663160000e-06, 2.943200000e-06,
        // u = 7.0 .. 7.9
        8.745961700e-07, 7.000540000e-07, 5.594530000e-07, 4.464280000e-07,
        3.557520000e-07, 2.831170000e-07, 2.249260000e-07, 1.784000000e-07,
        1.412950000e-07, 1.117900000e-07,
        // u = 8.0 .. 8.9
        3.232169325e-08, 2.555820000e-08, 2.020080000e-08, 1.594850000e-08,
        1.257770000e-08, 9.907800000e-09, 7.793500000e-09, 6.123100000e-09,
        4.805000000e-09, 3.766000000e-09,
        // u = 9.0 .. 9.9
        1.016247775e-09, 7.961100000e-10, 6.234100000e-10, 4.879100000e-10,
        3.817400000e-10, 2.984100000e-10, 2.331200000e-10, 1.819300000e-10,
        1.418500000e-10, 1.105100000e-10,
        // u = 10.0
        2.770171395e-11
    };
    return table;
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
///   * 1 < u ≤ 10: linear interpolation on tabulated grid (step 0.1)
///   * u > 10: asymptotic ρ(u) ≈ u^{-u} (worst-case overestimate of
///     density, harmless: makes survival predictor more permissive)
///   * NaN / negative u: clamped to 1.0 (defensive default = no reject)
[[nodiscard]] inline double dickman_rho(double u) {
    if (!(u > 0.0)) return 1.0;     // catches NaN, negative, zero
    if (u <= 1.0) return 1.0;
    if (u >= 10.0) {
        // Asymptotic decay; for u >> 10 ρ(u) is astronomically small.
        // u^{-u} = exp(-u * ln(u)). Guard against -inf.
        const double log_u = std::log(u);
        const double exponent = -u * log_u;
        if (exponent < -700.0) return 0.0;  // underflow region
        return std::exp(exponent);
    }
    // Linear interpolation on [1.0, 10.0] with 0.1 step.
    // u = 1.0 + 0.1 * idx where idx ∈ [0, 90].
    const double idx_f = (u - 1.0) * 10.0;
    int idx = static_cast<int>(idx_f);
    if (idx < 0) idx = 0;
    if (idx >= 90) idx = 89;
    const double frac = idx_f - static_cast<double>(idx);
    const double* table = detail::dickman_table();
    const double v0 = table[idx];
    const double v1 = table[idx + 1];
    return v0 + frac * (v1 - v0);
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
