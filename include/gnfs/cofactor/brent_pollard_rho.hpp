#pragma once

#include "../core/integer.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <random>
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
            if (success) succ.fetch_add(1, std::memory_order_relaxed);
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

    /// Attempt to split `n` into `(p, q)` with `p*q == n`, `1 < p ≤ q < n`.
    ///
    /// Returns nullopt on:
    ///   * `n < 4` (too small to split non-trivially)
    ///   * `n` is even (caller should divide by 2 first; we still try)
    ///   * `n` is a perfect power (rho cycles); ECM / p-1 should handle
    ///   * iteration budget exhausted across all retries
    ///
    /// @param n          Composite to split.
    /// @param max_iter   Upper bound on `f(x)` evaluations across all 3 c retries.
    /// @param seed       PRNG seed for c selection (deterministic).
    /// @param record     If true, accumulate into `global_stats()`.
    [[nodiscard]] static std::optional<std::pair<Integer, Integer>>
    split(const Integer& n,
          uint64_t max_iter = static_cast<uint64_t>(1) << 20,
          int seed = 1,
          bool record = true) {
        // Trivial reject
        if (n.compare(Integer(static_cast<uint64_t>(4))) < 0) {
            if (record) global_stats().record(false, 0);
            return std::nullopt;
        }

        // Even fast path: gcd would catch 2 but explicit is clearer.
        if (n.is_even()) {
            Integer two(static_cast<uint64_t>(2));
            Integer half;
            mpz_divexact(half.get_mpz(), n.get_mpz(), two.get_mpz());
            if (half.compare(Integer(static_cast<uint64_t>(1))) > 0) {
                if (record) global_stats().record(true, 0);
                return std::make_pair(std::move(two), std::move(half));
            }
            if (record) global_stats().record(false, 0);
            return std::nullopt;
        }

        // Perfect power detection — Pollard rho cycles on n = p^k.
        // GMP's `mpz_perfect_power_p` is exact (not probabilistic) and
        // accepts negative bases too, so cheap.
        if (mpz_perfect_power_p(n.get_mpz()) != 0) {
            if (record) global_stats().record(false, 0);
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

        if (record) global_stats().record(result.has_value(), iter_used);
        return result;
    }

private:
    static constexpr uint64_t BATCH_SIZE = 100;

#ifdef __SIZEOF_INT128__
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
        c_values[1] = (rng() % mod) + 2;   // ∈ [2, n-1]
        c_values[2] = (rng() % mod) + 2;

        // We split max_iter across the three retries: each gets up to
        // max_iter/3, but tracker `iter_used` accumulates across all.
        uint64_t per_retry = (max_iter + 2) / 3;
        if (per_retry == 0) per_retry = 1;

        for (int attempt = 0; attempt < 3; ++attempt) {
            if (iter_used >= max_iter) break;
            uint64_t c = c_values[attempt];
            // Skip degenerate c=0 (would give f(0) = 0 fixed point).
            if (c == 0) c = 1;

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
    [[nodiscard]] static uint64_t brent_loop_u128(uint64_t n, uint64_t c,
                                                   uint64_t budget,
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
        uint64_t r = 1;        // Brent step size (doubles each phase)
        uint64_t q = 1;        // accumulated product mod n
        uint64_t ys = y;       // backtrack save point
        uint64_t local_used = 0;

        do {
            x = y;
            // Advance y by r steps without checking gcd.
            for (uint64_t i = 0; i < r; ++i) {
                if (local_used >= budget) break;
                y = f(y);
                ++local_used;
            }

            // Accumulate |x-y| products in batches; gcd at batch end.
            uint64_t k = 0;
            while (k < r && d == 1) {
                ys = y;
                uint64_t batch = std::min(BATCH_SIZE, r - k);
                for (uint64_t i = 0; i < batch; ++i) {
                    if (local_used >= budget) break;
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
                if (q == 0) { d = n; break; }
                d = gcd64(q, n);
                k += BATCH_SIZE;
                if (local_used >= budget) break;
            }
            r <<= 1;
        } while (d == 1 && local_used < budget);

        // Backtrack when batched product was divisible by n.
        if (d == n) {
            d = 1;
            uint64_t back_steps = 0;
            const uint64_t MAX_BACK = BATCH_SIZE * 2;
            while (d == 1 && back_steps < MAX_BACK) {
                ys = f(ys);
                uint64_t diff = (x > ys) ? x - ys : ys - x;
                if (diff == 0) break;
                d = gcd64(diff, n);
                ++back_steps;
                ++local_used;
                if (local_used >= budget) break;
            }
        }

        iter_used += local_used;
        if (d == 1 || d == n) return 1;
        return d;
    }
#endif // __SIZEOF_INT128__

    /// Slow path — n exceeds uint64 (or 128-bit unavailable). Uses GMP
    /// Integer for all arithmetic. Same Brent + batch GCD structure.
    [[nodiscard]] static std::optional<std::pair<Integer, Integer>>
    split_integer(const Integer& n, uint64_t max_iter, int seed,
                  uint64_t& iter_used) {
        std::mt19937_64 rng(static_cast<uint64_t>(seed));

        // Generate three c values in [1, min(n-1, UINT32_MAX)].
        // For very large n we cap c to uint32 to keep RNG cheap and avoid
        // needing a full-width random Integer.
        Integer c_values[3];
        c_values[0] = Integer(static_cast<uint64_t>(1));
        c_values[1] = Integer(static_cast<uint64_t>((rng() % UINT32_MAX) + 2));
        c_values[2] = Integer(static_cast<uint64_t>((rng() % UINT32_MAX) + 2));

        uint64_t per_retry = (max_iter + 2) / 3;
        if (per_retry == 0) per_retry = 1;

        for (int attempt = 0; attempt < 3; ++attempt) {
            if (iter_used >= max_iter) break;
            uint64_t budget = std::min(per_retry, max_iter - iter_used);
            Integer factor = brent_loop_integer(n, c_values[attempt],
                                                  budget, iter_used);
            if (factor.compare(Integer(static_cast<uint64_t>(1))) > 0
                && factor.compare(n) < 0) {
                Integer other;
                mpz_divexact(other.get_mpz(), n.get_mpz(), factor.get_mpz());
                // verify factor * other == n (mpz_divexact assumes exact div)
                Integer prod = factor * other;
                if (prod.compare(n) != 0) continue;
                if (other.compare(Integer(static_cast<uint64_t>(1))) <= 0) continue;
                if (other.compare(n) >= 0) continue;
                Integer lo = (factor.compare(other) <= 0) ? factor.clone() : other.clone();
                Integer hi = (factor.compare(other) <= 0) ? other.clone() : factor.clone();
                return std::make_pair(std::move(lo), std::move(hi));
            }
        }
        return std::nullopt;
    }

    /// Single Brent cycle for GMP Integer. Returns 1 if no factor found.
    [[nodiscard]] static Integer brent_loop_integer(const Integer& n,
                                                     const Integer& c,
                                                     uint64_t budget,
                                                     uint64_t& iter_used) {
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
                if (local_used >= budget) break;
                f(y);
                ++local_used;
            }

            uint64_t k = 0;
            while (k < r && d.compare(one) == 0) {
                ys = y.clone();
                uint64_t batch = std::min(BATCH_SIZE, r - k);
                for (uint64_t i = 0; i < batch; ++i) {
                    if (local_used >= budget) break;
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
                if (q.is_zero()) { d = n.clone(); break; }
                d = core::gcd(q, n);
                k += BATCH_SIZE;
                if (local_used >= budget) break;
            }
            r <<= 1;
        } while (d.compare(one) == 0 && local_used < budget);

        if (d.compare(n) == 0) {
            d = one.clone();
            uint64_t back_steps = 0;
            const uint64_t MAX_BACK = BATCH_SIZE * 2;
            while (d.compare(one) == 0 && back_steps < MAX_BACK) {
                f(ys);
                Integer diff;
                if (x.compare(ys) >= 0) {
                    Integer::sub(diff, x, ys);
                } else {
                    Integer::sub(diff, ys, x);
                }
                if (diff.is_zero()) break;
                d = core::gcd(diff, n);
                ++back_steps;
                ++local_used;
                if (local_used >= budget) break;
            }
        }

        iter_used += local_used;
        if (d.compare(one) == 0 || d.compare(n) == 0) return Integer(static_cast<uint64_t>(1));
        return d;
    }
};

} // namespace gnfs::cofactor
