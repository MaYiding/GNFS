#pragma once

#include "../core/integer.hpp"
#include "attempt_context.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>

namespace gnfs::cofactor {

using core::Integer;

/// Brent variant of Pollard rho with batched GCD.
///
/// Position in the dispatch chain (when ENV `GNFS_COFACTOR_BRENT=1`):
///   trial-div → SQUFOF → BrentPollardRho → Pollard rho (legacy) → ECM
///
/// Algorithm reference: Richard P. Brent, "An Improved Monte Carlo
/// Factorization Algorithm", BIT 20 (1980), pp. 176-184.
///
/// Key differences vs `pollard_rho` in smooth_check.hpp:
///   * Returns a non-trivial split `(p, q)` with `p*q = n`, both `> 1 < n`.
///     The legacy `pollard_rho` returns a single factor; callers split out
///     `n/factor` ad-hoc and reverify.
///   * Native `__uint128_t` fast path for n that fits uint64 (n² fits 128).
///     Falls back to GMP Integer for larger cofactors.
///   * Power detection up front: refuses to factor n = p^k (rho cycles on
///     squarefree-free inputs; let SQUFOF / ECM handle perfect powers).
///   * Seedable + deterministic: same `n + seed` always yields the same
///     factor pair (or `nullopt`).
///
/// Batch size of 100 keeps gcd cost amortized while bounding worst-case
/// backtrack to 200 steps (BATCH × 2 in pollard_rho). Tuning here mirrors
/// the existing `pollard_rho` choice of 128 — slightly smaller to bound
/// backtrack cost when caller is invoked with tight `max_iter` budgets.
class BrentPollardRho {
public:
    /// Aggregate stats counter — atomic so concurrent callers can write
    /// without external locking. `flush_stats_line(stderr_stream)` formats
    /// `[brent_rho] tried=X succ=Y avg_iter=Z`.
    struct Stats {
        std::atomic<uint64_t> tried{0};
        std::atomic<uint64_t> succ{0};
        std::atomic<uint64_t> total_iter{0};

        void record(bool success, uint64_t iter_used) noexcept {
            tried.fetch_add(1, std::memory_order_relaxed);
            if (success)
                succ.fetch_add(1, std::memory_order_relaxed);
            total_iter.fetch_add(iter_used, std::memory_order_relaxed);
        }

        void reset() noexcept {
            tried.store(0, std::memory_order_relaxed);
            succ.store(0, std::memory_order_relaxed);
            total_iter.store(0, std::memory_order_relaxed);
        }
    };

    /// Global stats singleton — caller can opt-in to record via
    /// `record_to_global=true`. Cofactorizer dispatch uses this so the
    /// pipeline can emit one stderr summary line per sieve round.
    [[nodiscard]] static Stats& global_stats() noexcept {
        static Stats s;
        return s;
    }

    /// Explicit total budget for one residual/side Brent invocation.
    ///
    /// The unit is one evaluation of f(x)=x^2+c mod n. Schedule construction,
    /// primality/perfect-power checks, and GCD calls do not consume this budget.
    /// Zero means zero Brent work and is never interpreted as a default.
    struct EvaluationBudgetV1 final {
        uint64_t max_function_evaluations = 0;

        [[nodiscard]] friend constexpr bool
        operator==(const EvaluationBudgetV1&, const EvaluationBudgetV1&) noexcept = default;
    };

    struct RetryV1 final {
        uint64_t polynomial_c = 1;
        uint64_t function_evaluation_budget = 0;

        [[nodiscard]] friend constexpr bool operator==(const RetryV1&,
                                                       const RetryV1&) noexcept = default;
    };

    /// Immutable V1 retry plan. Unused budget from an early retry is not
    /// transferred to a later retry.
    struct Seed256RetryScheduleV1 final {
        std::array<RetryV1, COFACTOR_BRENT_POLLARD_RHO_RETRY_COUNT_V1> retries{};

        [[nodiscard]] friend constexpr bool
        operator==(const Seed256RetryScheduleV1&, const Seed256RetryScheduleV1&) noexcept = default;
    };

    struct SplitOutcomeV1 final {
        std::optional<std::pair<Integer, Integer>> factors;
        uint64_t function_evaluations_used = 0;
        uint32_t retries_started = 0;
    };

    /// Build an algorithm-bound, full-width Seed256 retry schedule.
    ///
    /// Retry 0 uses c=1. Draw ordinals 0 and 1 provide retries 1 and 2.
    /// For uint64 cofactors the draw maps to [2,n-1], independent of whether
    /// the execution backend is native or GMP. Larger cofactors use the frozen
    /// range [2,UINT64_MAX]. Budget slices follow the V1 ceil-slice contract:
    /// each retry receives min(ceil(total/3), remaining).
    ///
    /// The schedule contains only executable polynomial and budget data; it
    /// does not authenticate `n`. Callers must retain the schedule-to-cofactor
    /// association established when this factory returns.
    [[nodiscard]] static Seed256RetryScheduleV1
    make_seed256_retry_schedule_v1(const Integer& n, EvaluationBudgetV1 budget,
                                   const CofactorAttemptContext& attempt) {
        if (attempt.domain != CofactorRandomDomainV1::brent_pollard_rho ||
            attempt.algorithm_identity !=
                COFACTOR_BRENT_POLLARD_RHO_SCHEDULE_ALGORITHM_IDENTITY_V1) {
            throw std::invalid_argument(
                "cofactor attempt is not bound to the V1 Brent-Pollard-rho schedule");
        }

        Seed256RetryScheduleV1 schedule;
        const uint64_t slice =
            budget.max_function_evaluations / COFACTOR_BRENT_POLLARD_RHO_RETRY_COUNT_V1 +
            static_cast<uint64_t>(
                budget.max_function_evaluations % COFACTOR_BRENT_POLLARD_RHO_RETRY_COUNT_V1 != 0);
        uint64_t remaining = budget.max_function_evaluations;
        for (RetryV1& retry : schedule.retries) {
            retry.function_evaluation_budget = std::min(slice, remaining);
            remaining -= retry.function_evaluation_budget;
        }

        schedule.retries[0].polynomial_c = 1;
        if (n.fits_uint64()) {
            const uint64_t n_u = n.to_uint64();
            const uint64_t modulus = n_u > 2 ? n_u - 2 : 1;
            for (uint32_t retry = 1; retry < schedule.retries.size(); ++retry) {
                schedule.retries[retry].polynomial_c =
                    (cofactor_random_u64(attempt.seed, retry - 1) % modulus) + 2;
            }
        } else {
            constexpr uint64_t modulus = std::numeric_limits<uint64_t>::max() - 1;
            for (uint32_t retry = 1; retry < schedule.retries.size(); ++retry) {
                schedule.retries[retry].polynomial_c =
                    (cofactor_random_u64(attempt.seed, retry - 1) % modulus) + 2;
            }
        }
        return schedule;
    }

    /// Attempt to split `n` into `(p, q)` with `p*q == n`, `1 < p ≤ q < n`.
    ///
    /// Even `n >= 4` returns `(2,n/2)` without consuming evaluation budget.
    /// The legacy signature and integer-seed polynomial stream are preserved.
    /// `max_iter` is now a strict bound, including backtracking; tight-budget
    /// outcomes that historically relied on a one-evaluation overrun, and the
    /// old UINT64_MAX ceil-slice overflow, are intentionally corrected.
    ///
    /// Returns nullopt on:
    ///   * `n < 4` (too small to split non-trivially)
    ///   * `n` is an odd perfect power (rho cycles); ECM / p-1 should handle
    ///   * iteration budget exhausted across all retries
    ///
    /// @param n          Composite to split.
    /// @param max_iter   Upper bound on `f(x)` evaluations across all 3 c retries.
    /// @param seed       PRNG seed for c selection (deterministic).
    /// @param record     If true, accumulate into `global_stats()`.
    [[nodiscard]] static std::optional<std::pair<Integer, Integer>>
    split(const Integer& n, uint64_t max_iter = static_cast<uint64_t>(1) << 20, int seed = 1,
          bool record = true) {
        // Trivial reject
        if (n.compare(Integer(static_cast<uint64_t>(4))) < 0) {
            if (record)
                global_stats().record(false, 0);
            return std::nullopt;
        }

        // Even fast path: gcd would catch 2 but explicit is clearer.
        if (n.is_even()) {
            Integer two(static_cast<uint64_t>(2));
            Integer half;
            mpz_divexact(half.get_mpz(), n.get_mpz(), two.get_mpz());
            if (half.compare(Integer(static_cast<uint64_t>(1))) > 0) {
                if (record)
                    global_stats().record(true, 0);
                return std::make_pair(std::move(two), std::move(half));
            }
            if (record)
                global_stats().record(false, 0);
            return std::nullopt;
        }

        // Perfect power detection — Pollard rho cycles on n = p^k.
        // GMP's `mpz_perfect_power_p` is exact (not probabilistic) and
        // accepts negative bases too, so cheap.
        if (mpz_perfect_power_p(n.get_mpz()) != 0) {
            if (record)
                global_stats().record(false, 0);
            return std::nullopt;
        }

        uint64_t iter_used = 0;
        std::optional<std::pair<Integer, Integer>> result;

#ifdef __SIZEOF_INT128__
        // Native fast path: n fits uint64. We need n² < 2^128 for the
        // mod-mul step to stay in __uint128_t. uint64::max² ≈ 2^128 - 2^65,
        // which fits, so the condition is simply `n.fits_uint64()`.
        if (n.fits_uint64()) {
            uint64_t n_u = n.to_uint64();
            result = split_u128(n_u, max_iter, seed, iter_used);
        } else {
            result = split_integer(n, max_iter, seed, iter_used);
        }
#else
        result = split_integer(n, max_iter, seed, iter_used);
#endif

        if (record)
            global_stats().record(result.has_value(), iter_used);
        return result;
    }

    /// Execute an immutable Seed256 retry schedule without ENV or PRNG reads.
    ///
    /// The returned accounting is exact for f(x) evaluations and never exceeds
    /// the sum of the three retry budgets.
    [[nodiscard]] static SplitOutcomeV1
    split_seeded_v1(const Integer& n, const Seed256RetryScheduleV1& schedule, bool record = true) {
        SplitOutcomeV1 outcome;
        validate_seed256_retry_schedule_v1(n, schedule);

        if (n.compare(Integer(static_cast<uint64_t>(4))) < 0) {
            if (record) {
                global_stats().record(false, 0);
            }
            return outcome;
        }

        if (n.is_even()) {
            Integer two(static_cast<uint64_t>(2));
            Integer half;
            mpz_divexact(half.get_mpz(), n.get_mpz(), two.get_mpz());
            if (half.compare(Integer(static_cast<uint64_t>(1))) > 0) {
                outcome.factors = std::make_pair(std::move(two), std::move(half));
            }
            if (record) {
                global_stats().record(outcome.factors.has_value(), 0);
            }
            return outcome;
        }

        if (mpz_perfect_power_p(n.get_mpz()) != 0) {
            if (record) {
                global_stats().record(false, 0);
            }
            return outcome;
        }

#ifdef __SIZEOF_INT128__
        if (n.fits_uint64()) {
            outcome.factors =
                split_u128_seeded_v1(n.to_uint64(), schedule, outcome.function_evaluations_used,
                                     outcome.retries_started);
        } else {
            outcome.factors = split_integer_seeded_v1(
                n, schedule, outcome.function_evaluations_used, outcome.retries_started);
        }
#else
        outcome.factors = split_integer_seeded_v1(n, schedule, outcome.function_evaluations_used,
                                                  outcome.retries_started);
#endif

        if (record) {
            global_stats().record(outcome.factors.has_value(), outcome.function_evaluations_used);
        }
        return outcome;
    }

private:
    static constexpr uint64_t BATCH_SIZE = 100;

    static void validate_seed256_retry_schedule_v1(const Integer& n,
                                                   const Seed256RetryScheduleV1& schedule) {
        uint64_t total_budget = 0;
        const bool fits_u64 = n.fits_uint64();
        const uint64_t n_u = fits_u64 ? n.to_uint64() : 0;

        for (std::size_t retry_index = 0; retry_index < schedule.retries.size(); ++retry_index) {
            const RetryV1& retry = schedule.retries[retry_index];
            if ((retry_index == 0 && retry.polynomial_c != 1) ||
                (retry_index > 0 && retry.polynomial_c < 2)) {
                throw std::invalid_argument(
                    "V1 Brent-Pollard-rho retry polynomial c violates the schedule shape");
            }
            // No polynomial is evaluated for n < 4, but the factory must
            // still produce a schedule that the executor can validate and
            // return as a zero-work outcome.
            if (fits_u64 && n_u >= 4 && retry.polynomial_c >= n_u) {
                throw std::invalid_argument(
                    "V1 Brent-Pollard-rho retry polynomial c is outside the cofactor range");
            }
            if (retry.function_evaluation_budget >
                std::numeric_limits<uint64_t>::max() - total_budget) {
                throw std::invalid_argument("V1 Brent-Pollard-rho retry budgets overflow uint64");
            }
            total_budget += retry.function_evaluation_budget;
        }
    }

#ifdef __SIZEOF_INT128__
    [[nodiscard]] static std::optional<std::pair<Integer, Integer>>
    split_u128_seeded_v1(uint64_t n, const Seed256RetryScheduleV1& schedule, uint64_t& iter_used,
                         uint32_t& retries_started) {
        for (const RetryV1& retry : schedule.retries) {
            if (retry.function_evaluation_budget == 0) {
                continue;
            }
            ++retries_started;
            const uint64_t factor =
                brent_loop_u128(n, retry.polynomial_c, retry.function_evaluation_budget, iter_used);
            if (factor > 1 && factor < n) {
                const uint64_t other = n / factor;
                if (factor * other == n && other > 1 && other < n) {
                    const uint64_t lo = factor < other ? factor : other;
                    const uint64_t hi = factor < other ? other : factor;
                    return std::make_pair(Integer(lo), Integer(hi));
                }
            }
        }
        return std::nullopt;
    }

    /// Fast path — native 128-bit modular arithmetic. `n` must be odd > 3,
    /// not a perfect power, and fit uint64.
    [[nodiscard]] static std::optional<std::pair<Integer, Integer>>
    split_u128(uint64_t n, uint64_t max_iter, int seed, uint64_t& iter_used) {
        // PRNG: deterministic from seed.
        std::mt19937_64 rng(static_cast<uint64_t>(seed));
        // Three c attempts. First always c=1 (Pollard's classic default,
        // best for small balanced semiprimes). Then two RNG-derived.
        uint64_t c_values[3];
        c_values[0] = 1;
        // Avoid division-by-zero edge: when n < 3, but split() filters n < 4
        // and the even branch covers n = 4, 6, 8... Still guard for n == 2
        // or 3 reaching here (shouldn't happen).
        uint64_t mod = (n > 2) ? (n - 2) : 1;
        c_values[1] = (rng() % mod) + 2; // ∈ [2, n-1]
        c_values[2] = (rng() % mod) + 2;

        // We split max_iter across the three retries: each gets up to
        // max_iter/3, but tracker `iter_used` accumulates across all.
        uint64_t per_retry = max_iter / 3 + static_cast<uint64_t>(max_iter % 3 != 0);
        if (per_retry == 0)
            per_retry = 1;

        for (int attempt = 0; attempt < 3; ++attempt) {
            if (iter_used >= max_iter)
                break;
            uint64_t c = c_values[attempt];
            // Skip degenerate c=0 (would give f(0) = 0 fixed point).
            if (c == 0)
                c = 1;

            uint64_t budget = std::min(per_retry, max_iter - iter_used);
            uint64_t factor = brent_loop_u128(n, c, budget, iter_used);
            if (factor > 1 && factor < n) {
                uint64_t other = n / factor;
                if (factor * other == n && other > 1 && other < n) {
                    uint64_t lo = factor < other ? factor : other;
                    uint64_t hi = factor < other ? other : factor;
                    return std::make_pair(Integer(lo), Integer(hi));
                }
            }
        }
        return std::nullopt;
    }

    /// One Brent cycle attempt with polynomial f(x) = x² + c mod n.
    /// Returns 1 if no factor found within `budget` evals, else factor.
    [[nodiscard]] static uint64_t brent_loop_u128(uint64_t n, uint64_t c, uint64_t budget,
                                                  uint64_t& iter_used) {
        auto f = [n, c](uint64_t x) -> uint64_t {
            __uint128_t xx = static_cast<__uint128_t>(x) * x + c;
            return static_cast<uint64_t>(xx % n);
        };

        auto gcd64 = [](uint64_t a, uint64_t b) -> uint64_t {
            while (b != 0) {
                uint64_t t = b;
                b = a % b;
                a = t;
            }
            return a;
        };

        uint64_t x = 2, y = 2;
        uint64_t d = 1;
        uint64_t r = 1;  // Brent step size (doubles each phase)
        uint64_t q = 1;  // accumulated product mod n
        uint64_t ys = y; // backtrack save point
        uint64_t local_used = 0;

        do {
            x = y;
            // Advance y by r steps without checking gcd.
            for (uint64_t i = 0; i < r; ++i) {
                if (local_used >= budget)
                    break;
                y = f(y);
                ++local_used;
            }

            // Accumulate |x-y| products in batches; gcd at batch end.
            uint64_t k = 0;
            while (k < r && d == 1) {
                ys = y;
                uint64_t batch = std::min(BATCH_SIZE, r - k);
                for (uint64_t i = 0; i < batch; ++i) {
                    if (local_used >= budget)
                        break;
                    y = f(y);
                    ++local_used;
                    uint64_t diff = (x > y) ? x - y : y - x;
                    if (diff == 0) {
                        // Cycle detected within batch — bail to outer loop.
                        d = n;
                        break;
                    }
                    __uint128_t qq = static_cast<__uint128_t>(q) * diff;
                    q = static_cast<uint64_t>(qq % n);
                }
                if (q == 0) {
                    d = n;
                    break;
                }
                d = gcd64(q, n);
                k += BATCH_SIZE;
                if (local_used >= budget)
                    break;
            }
            r <<= 1;
        } while (d == 1 && local_used < budget);

        // Backtrack when batched product was divisible by n.
        if (d == n) {
            d = 1;
            uint64_t back_steps = 0;
            const uint64_t MAX_BACK = BATCH_SIZE * 2;
            while (d == 1 && back_steps < MAX_BACK && local_used < budget) {
                ys = f(ys);
                ++back_steps;
                ++local_used;
                uint64_t diff = (x > ys) ? x - ys : ys - x;
                if (diff == 0)
                    break;
                d = gcd64(diff, n);
            }
        }

        iter_used += local_used;
        if (d == 1 || d == n)
            return 1;
        return d;
    }
#endif // __SIZEOF_INT128__

    [[nodiscard]] static std::optional<std::pair<Integer, Integer>>
    split_integer_seeded_v1(const Integer& n, const Seed256RetryScheduleV1& schedule,
                            uint64_t& iter_used, uint32_t& retries_started) {
        const Integer one(static_cast<uint64_t>(1));
        for (const RetryV1& retry : schedule.retries) {
            if (retry.function_evaluation_budget == 0) {
                continue;
            }
            ++retries_started;
            const Integer c(retry.polynomial_c);
            Integer factor = brent_loop_integer(n, c, retry.function_evaluation_budget, iter_used);
            if (factor.compare(one) <= 0 || factor.compare(n) >= 0) {
                continue;
            }

            Integer other;
            mpz_divexact(other.get_mpz(), n.get_mpz(), factor.get_mpz());
            const Integer product = factor * other;
            if (product.compare(n) != 0 || other.compare(one) <= 0 || other.compare(n) >= 0) {
                continue;
            }
            Integer lo = factor.compare(other) <= 0 ? factor.clone() : other.clone();
            Integer hi = factor.compare(other) <= 0 ? other.clone() : factor.clone();
            return std::make_pair(std::move(lo), std::move(hi));
        }
        return std::nullopt;
    }

    /// Slow path — n exceeds uint64 (or 128-bit unavailable). Uses GMP
    /// Integer for all arithmetic. Same Brent + batch GCD structure.
    [[nodiscard]] static std::optional<std::pair<Integer, Integer>>
    split_integer(const Integer& n, uint64_t max_iter, int seed, uint64_t& iter_used) {
        std::mt19937_64 rng(static_cast<uint64_t>(seed));

        // Generate three c values in [1, min(n-1, UINT32_MAX)].
        // For very large n we cap c to uint32 to keep RNG cheap and avoid
        // needing a full-width random Integer.
        Integer c_values[3];
        c_values[0] = Integer(static_cast<uint64_t>(1));
        c_values[1] = Integer(static_cast<uint64_t>((rng() % UINT32_MAX) + 2));
        c_values[2] = Integer(static_cast<uint64_t>((rng() % UINT32_MAX) + 2));

        uint64_t per_retry = max_iter / 3 + static_cast<uint64_t>(max_iter % 3 != 0);
        if (per_retry == 0)
            per_retry = 1;

        for (int attempt = 0; attempt < 3; ++attempt) {
            if (iter_used >= max_iter)
                break;
            uint64_t budget = std::min(per_retry, max_iter - iter_used);
            Integer factor = brent_loop_integer(n, c_values[attempt], budget, iter_used);
            if (factor.compare(Integer(static_cast<uint64_t>(1))) > 0 && factor.compare(n) < 0) {
                Integer other;
                mpz_divexact(other.get_mpz(), n.get_mpz(), factor.get_mpz());
                // verify factor * other == n (mpz_divexact assumes exact div)
                Integer prod = factor * other;
                if (prod.compare(n) != 0)
                    continue;
                if (other.compare(Integer(static_cast<uint64_t>(1))) <= 0)
                    continue;
                if (other.compare(n) >= 0)
                    continue;
                Integer lo = (factor.compare(other) <= 0) ? factor.clone() : other.clone();
                Integer hi = (factor.compare(other) <= 0) ? other.clone() : factor.clone();
                return std::make_pair(std::move(lo), std::move(hi));
            }
        }
        return std::nullopt;
    }

    /// Single Brent cycle for GMP Integer. Returns 1 if no factor found.
    [[nodiscard]] static Integer brent_loop_integer(const Integer& n, const Integer& c,
                                                    uint64_t budget, uint64_t& iter_used) {
        // f(x) = (x*x + c) mod n
        auto f = [&](Integer& x) {
            Integer x2 = x * x;
            x2 += c;
            mpz_mod(x.get_mpz(), x2.get_mpz(), n.get_mpz());
        };

        Integer x(static_cast<uint64_t>(2));
        Integer y(static_cast<uint64_t>(2));
        Integer d(static_cast<uint64_t>(1));
        Integer q(static_cast<uint64_t>(1));
        Integer ys(static_cast<uint64_t>(2));
        Integer one(static_cast<uint64_t>(1));
        uint64_t r = 1;
        uint64_t local_used = 0;

        do {
            x = y.clone();
            for (uint64_t i = 0; i < r; ++i) {
                if (local_used >= budget)
                    break;
                f(y);
                ++local_used;
            }

            uint64_t k = 0;
            while (k < r && d.compare(one) == 0) {
                ys = y.clone();
                uint64_t batch = std::min(BATCH_SIZE, r - k);
                for (uint64_t i = 0; i < batch; ++i) {
                    if (local_used >= budget)
                        break;
                    f(y);
                    ++local_used;
                    Integer diff;
                    if (x.compare(y) >= 0) {
                        Integer::sub(diff, x, y);
                    } else {
                        Integer::sub(diff, y, x);
                    }
                    if (diff.is_zero()) {
                        d = n.clone();
                        break;
                    }
                    Integer prod = q * diff;
                    mpz_mod(q.get_mpz(), prod.get_mpz(), n.get_mpz());
                }
                if (q.is_zero()) {
                    d = n.clone();
                    break;
                }
                d = core::gcd(q, n);
                k += BATCH_SIZE;
                if (local_used >= budget)
                    break;
            }
            r <<= 1;
        } while (d.compare(one) == 0 && local_used < budget);

        if (d.compare(n) == 0) {
            d = one.clone();
            uint64_t back_steps = 0;
            const uint64_t MAX_BACK = BATCH_SIZE * 2;
            while (d.compare(one) == 0 && back_steps < MAX_BACK && local_used < budget) {
                f(ys);
                ++back_steps;
                ++local_used;
                Integer diff;
                if (x.compare(ys) >= 0) {
                    Integer::sub(diff, x, ys);
                } else {
                    Integer::sub(diff, ys, x);
                }
                if (diff.is_zero())
                    break;
                d = core::gcd(diff, n);
            }
        }

        iter_used += local_used;
        if (d.compare(one) == 0 || d.compare(n) == 0)
            return Integer(static_cast<uint64_t>(1));
        return d;
    }
};

} // namespace gnfs::cofactor
