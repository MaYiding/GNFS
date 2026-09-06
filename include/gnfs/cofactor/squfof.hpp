#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gnfs::cofactor {

/// SQUFOF (SQUare FOrm Factorization) — Shanks 1975
///
/// Factors composites up to ~2^62 in O(N^{1/4}) time with tiny constant.
/// Reference: Jason Gower & Samuel Wagstaff Jr., "Square Form Factorization",
///            Mathematics of Computation 77(261), 2008.
///
/// Algorithm operates on the infrastructure of binary quadratic forms
/// with discriminant Δ = 4kN (for multiplier k). Uses continued fraction
/// expansion of √(kN) to find a proper square form, then extract factor.
class SQUFOF {
private:
    // Single source of truth for the production order. Diagnostics derive
    // both their slot count and their multiplier labels from this schedule.
    inline static constexpr auto multiplier_schedule_ = std::to_array<uint64_t>({
        1,
        3 * 5,
        3,
        5,
        7,
        11,
        3 * 7,
        3 * 11,
        5 * 7,
        5 * 11,
        7 * 11,
    });

public:
    enum class MultiplierProbeStatus : uint8_t {
        ineligible_input,
        invalid_slot,
        overflow,
        attempted,
    };

    struct MultiplierProbeResult {
        MultiplierProbeStatus status = MultiplierProbeStatus::ineligible_input;
        uint64_t forward_iterations = 0;
        bool core_hit = false;
        // Zero means that the core did not yield a proper factor of n.
        uint64_t accepted_factor = 0;
    };

    struct MultiplierDiagnostics {
        uint64_t multiplier = 0;
        uint64_t attempts = 0;
        uint64_t forward_iterations = 0;
        uint64_t core_hits = 0;
        uint64_t accepted_hits = 0;
        uint64_t overflow_skips = 0;
    };

    inline static constexpr size_t diagnostic_slot_count = multiplier_schedule_.size();

    /// Caller-owned cumulative counters for factor_with_diagnostics().
    ///
    /// This object is intentionally unsynchronized. Reuse it sequentially to
    /// accumulate observations, or call reset() for a fresh sample. Multiplier
    /// slots stay untouched by preprocessing fast paths:
    ///   - n <= 1 increments trivial_input_hits;
    ///   - every even n, including an even square, increments even_fast_path_hits;
    ///   - only odd squares increment square_fast_path_hits.
    struct Diagnostics {
        uint64_t factor_calls = 0;
        uint64_t trivial_input_hits = 0;
        uint64_t even_fast_path_hits = 0;
        uint64_t square_fast_path_hits = 0;
        std::array<MultiplierDiagnostics, diagnostic_slot_count> slots{};

        Diagnostics() noexcept {
            reset();
        }

        void reset() noexcept {
            factor_calls = 0;
            trivial_input_hits = 0;
            even_fast_path_hits = 0;
            square_fast_path_hits = 0;
            for (size_t slot_index = 0; slot_index < slots.size(); ++slot_index) {
                slots[slot_index] = MultiplierDiagnostics{};
                slots[slot_index].multiplier = multiplier_schedule_[slot_index];
            }
        }
    };

    /// Production multiplier order used by factor() and diagnostics slots.
    [[nodiscard]] static constexpr const auto& multiplier_schedule() noexcept {
        return multiplier_schedule_;
    }

    /// Probe exactly one slot in the production multiplier schedule.
    ///
    /// Invalid slots fail closed before input preprocessing. Inputs handled by
    /// factor() preprocessing (n <= 1, even n, and odd squares) are ineligible
    /// because no multiplier is attempted for them in production.
    [[nodiscard]] static MultiplierProbeResult probe_multiplier(uint64_t n, size_t schedule_slot,
                                                                uint32_t max_iterations = 0) {
        MultiplierProbeResult probe;
        if (schedule_slot >= multiplier_schedule_.size()) {
            probe.status = MultiplierProbeStatus::invalid_slot;
            return probe;
        }

        if (n <= 1 || n % 2 == 0)
            return probe;
        const uint64_t square_root = isqrt(n);
        if (square_root * square_root == n)
            return probe;

        const uint64_t k = multiplier_schedule_[schedule_slot];
        (void)try_multiplier<false, true>(n, k, max_iterations, nullptr, schedule_slot, &probe);
        return probe;
    }

    /// Checked denominator update used by the continued-fraction recurrence.
    ///
    /// Exposed as a small arithmetic primitive so boundary tests and
    /// diagnostics can verify the same overflow contract as the production
    /// core. Returns false when the mathematically expected positive result
    /// cannot be represented by uint64_t.
    [[nodiscard]] static bool checked_recurrence_update(uint64_t q_prev, uint64_t b,
                                                        uint64_t p_prev, uint64_t p_new,
                                                        uint64_t& q_new) noexcept {
        return update_q(q_prev, b, p_prev, p_new, q_new);
    }

    /// Factor n using SQUFOF. Returns a non-trivial factor, or 1 on failure.
    /// n must be > 1 and composite. Works for n up to ~2^62.
    [[nodiscard]] static uint64_t factor(uint64_t n, uint32_t max_iterations = 0) {
        // The false template instance contains no diagnostics reads, writes, or
        // runtime branches. The production path never accepts a probe object.
        return factor_impl<false>(n, max_iterations, nullptr);
    }

    /// Factor n while accumulating diagnostics in caller-owned storage.
    ///
    /// Return values and max_iterations semantics are identical to factor().
    /// The supplied object is cumulative and is not reset automatically.
    [[nodiscard]] static uint64_t factor_with_diagnostics(uint64_t n, uint32_t max_iterations,
                                                          Diagnostics& diagnostics) {
        return factor_impl<true>(n, max_iterations, &diagnostics);
    }

private:
    template <bool CollectDiagnostics>
    [[nodiscard]] static uint64_t factor_impl(uint64_t n, uint32_t max_iterations,
                                              [[maybe_unused]] Diagnostics* diagnostics) {
        if constexpr (CollectDiagnostics) {
            ++diagnostics->factor_calls;
        }

        if (n <= 1) {
            if constexpr (CollectDiagnostics) {
                ++diagnostics->trivial_input_hits;
            }
            return 1;
        }
        if (n % 2 == 0) {
            if constexpr (CollectDiagnostics) {
                ++diagnostics->even_fast_path_hits;
            }
            return 2;
        }
        const uint64_t square_root = isqrt(n);
        if (square_root * square_root == n) {
            if constexpr (CollectDiagnostics) {
                ++diagnostics->square_fast_path_hits;
            }
            return square_root;
        }

        // Try multipliers to avoid short-period cases. Keep k=1 first for the
        // common path, then try k=15: Gower-Wagstaff Table 5 gives it the best
        // expected work among the non-trivial multipliers in this set. Preserve
        // the prior order for the remaining fallbacks.
        for (size_t slot_index = 0; slot_index < multiplier_schedule_.size(); ++slot_index) {
            const uint64_t k = multiplier_schedule_[slot_index];
            const uint64_t factor = try_multiplier<CollectDiagnostics, false>(
                n, k, max_iterations, diagnostics, slot_index, nullptr);
            if (factor > 1)
                return factor;
        }
        return 1;
    }

    template <bool CollectDiagnostics, bool CollectProbe>
    [[nodiscard]] static uint64_t try_multiplier(uint64_t n, uint64_t k, uint32_t max_iterations,
                                                 [[maybe_unused]] Diagnostics* diagnostics,
                                                 size_t slot_index,
                                                 [[maybe_unused]] MultiplierProbeResult* probe) {
        if (k > 1 && n > UINT64_MAX / k) {
            if constexpr (CollectDiagnostics) {
                ++diagnostics->slots[slot_index].overflow_skips;
            }
            if constexpr (CollectProbe) {
                probe->status = MultiplierProbeStatus::overflow;
            }
            return 1;
        }

        uint64_t D = k * n;
        if (D < 2)
            return 1;
        // D ≡ 2,3 mod 4 时 SQUFOF 周期偏长;对 k=1 时如果 N ≢ 1 mod 4
        // 改用 D=4N 既避免 D≡2,3 又把 k 的搜索空间限制在奇 k。
        if (k == 1 && (n % 4) != 1) {
            if (n > UINT64_MAX / 4) {
                if constexpr (CollectDiagnostics) {
                    ++diagnostics->slots[slot_index].overflow_skips;
                }
                if constexpr (CollectProbe) {
                    probe->status = MultiplierProbeStatus::overflow;
                }
                return 1;
            }
            D = 4 * n;
        }

        if constexpr (CollectDiagnostics) {
            ++diagnostics->slots[slot_index].attempts;
        }
        if constexpr (CollectProbe) {
            probe->status = MultiplierProbeStatus::attempted;
        }
        const uint64_t result = squfof_core<CollectDiagnostics, CollectProbe>(
            D, max_iterations, diagnostics, slot_index, probe);
        if (result > 1 && result < D) {
            if constexpr (CollectDiagnostics) {
                ++diagnostics->slots[slot_index].core_hits;
            }
            if constexpr (CollectProbe) {
                probe->core_hit = true;
            }
            const uint64_t factor = gcd(result, n);
            if (factor > 1 && factor < n) {
                if constexpr (CollectDiagnostics) {
                    ++diagnostics->slots[slot_index].accepted_hits;
                }
                if constexpr (CollectProbe) {
                    probe->accepted_factor = factor;
                }
                return factor;
            }
        }
        return 1;
    }

    static uint64_t gcd(uint64_t a, uint64_t b) {
        while (b) {
            uint64_t t = b;
            b = a % b;
            a = t;
        }
        return a;
    }

    static uint64_t isqrt(uint64_t n) {
        if (n == 0)
            return 0;
        uint64_t x = static_cast<uint64_t>(std::sqrt(static_cast<double>(n)));
        // double may round sqrt(UINT64_MAX - epsilon) to 2^32.  Clamp to
        // floor(sqrt(UINT64_MAX)) before using multiplication for correction.
        x = std::min<uint64_t>(x, UINT32_MAX);
        while (x > 0 && x * x > n)
            --x;
        while (x < UINT32_MAX && (x + 1) * (x + 1) <= n)
            ++x;
        return x;
    }

    /// Update a continued-fraction denominator without relying on unsigned
    /// subtraction wrapping. The recurrence is
    ///
    ///     Q_new = Q_prev + b * (P_prev - P_new).
    ///
    /// P_new may be greater than P_prev, in which case the second term is
    /// negative. Native uint64_t subtraction would wrap and silently corrupt
    /// the continued-fraction state. Return false if the mathematically
    /// expected positive value cannot be represented.
    [[nodiscard]] static bool update_q(uint64_t q_prev, uint64_t b, uint64_t p_prev, uint64_t p_new,
                                       uint64_t& q_new) noexcept {
        if (p_prev >= p_new) {
            const uint64_t delta = p_prev - p_new;
            if (b != 0 && delta > (UINT64_MAX - q_prev) / b)
                return false;
            q_new = q_prev + b * delta;
            return true;
        }

        const uint64_t delta = p_new - p_prev;
        if (b != 0 && delta > q_prev / b)
            return false;
        q_new = q_prev - b * delta;
        return true;
    }

    /// Core SQUFOF: factor D using continued fraction of √D.
    /// Returns a non-trivial factor of D, or 1 on failure.
    ///
    /// Uses the "reduced form" approach:
    ///   P_0 = floor(√D), Q_0 = 1, Q_1 = D - P_0²
    ///   b_i = floor((P_i + floor(√D)) / Q_{i+1})
    ///   P_{i+1} = b_i * Q_{i+1} - P_i
    ///   Q_{i+2} = Q_i + b_i * (P_i - P_{i+1})
    ///
    /// Look for Q_i that is a perfect square at even steps (i ≥ 2).
    /// Then do an inverse walk to extract the factor.
    template <bool CollectDiagnostics, bool CollectProbe>
    [[nodiscard]] static uint64_t
    squfof_core(uint64_t D, uint32_t max_iter, [[maybe_unused]] Diagnostics* diagnostics,
                [[maybe_unused]] size_t slot_index, [[maybe_unused]] MultiplierProbeResult* probe) {
        uint64_t sqrtD = isqrt(D);
        if (sqrtD * sqrtD == D)
            return sqrtD;

        if (max_iter == 0) {
            max_iter = static_cast<uint32_t>(std::min(
                4.0 * std::pow(static_cast<double>(D), 0.25) + 200, static_cast<double>(1u << 22)));
        }

        // Initialization
        uint64_t Pprev = sqrtD;
        uint64_t Qprev = 1;
        uint64_t Qcurr = D - sqrtD * sqrtD;
        if (Qcurr == 0)
            return sqrtD;

        // Forward cycle
        for (uint32_t i = 0; i < max_iter; ++i) {
            if constexpr (CollectDiagnostics) {
                ++diagnostics->slots[slot_index].forward_iterations;
            }
            if constexpr (CollectProbe) {
                ++probe->forward_iterations;
            }
            uint64_t b = (sqrtD + Pprev) / Qcurr;
            uint64_t Pnew = b * Qcurr - Pprev;
            uint64_t Qnew = 0;
            if (!update_q(Qprev, b, Pprev, Pnew, Qnew))
                return 1;

            // Check for perfect square Q at even iteration (i odd = step i+1 even since i starts 0)
            // Gower-Wagstaff: check Q_{i+1} at odd i (0-indexed), i.e., even steps
            if ((i & 1) == 1) {
                uint64_t sq = isqrt(Qcurr);
                if (sq > 0 && sq * sq == Qcurr) {
                    // Found proper square form. Do inverse walk.
                    uint64_t f = inverse_walk(D, sqrtD, Pprev, sq);
                    if (f > 1 && f < D)
                        return f;
                }
            }

            Pprev = Pnew;
            Qprev = Qcurr;
            Qcurr = Qnew;

            if (Qcurr == 0)
                break;
        }

        return 1;
    }

    /// Inverse walk from a square form to extract a factor.
    /// Given that Q = sq², start a new CF expansion from the reduced form
    /// and walk until P doesn't change (P_{i+1} == P_i).
    static uint64_t inverse_walk(uint64_t D, uint64_t sqrtD, uint64_t P_at_sq, uint64_t sq) {
        // Start inverse walk
        uint64_t b = (sqrtD + P_at_sq) / sq;
        uint64_t P = b * sq - P_at_sq;
        uint64_t Qprev = sq;
        uint64_t Qcurr = (D - P * P) / Qprev;

        if (Qcurr == 0)
            return gcd(D, P);

        uint32_t max_rev = 1u << 20;
        for (uint32_t i = 0; i < max_rev; ++i) {
            b = (sqrtD + P) / Qcurr;
            uint64_t Pnew = b * Qcurr - P;

            if (Pnew == P) {
                // Ambiguous form found
                return gcd(D, P);
            }

            uint64_t Qnew = 0;
            if (!update_q(Qprev, b, P, Pnew, Qnew))
                return 1;
            Qprev = Qcurr;
            P = Pnew;
            Qcurr = Qnew;

            if (Qcurr == 0)
                return gcd(D, P);
        }

        return 1;
    }
};

} // namespace gnfs::cofactor
