#pragma once

// ECM Montgomery curve warm-pool (T5 Stage 2) — pre-generate Suyama-parametrised
// curves so the hot ECM retry loop can pop ready-to-use curves instead of
// reconstructing (A, x_0, z_0) from scratch each time.
//
// Motivation:
//   `ECM::try_curve_with_pk` spends ~hundreds of mpz operations on Suyama
//   setup before the Stage-1 multiplication even starts:
//       u = sigma^2 - 5     (mod n)
//       v = 4 * sigma       (mod n)
//       x_0 = u^3            (mod n)
//       z_0 = v^3            (mod n)
//       diff = v - u         (mod n)
//       num  = (v - u)^3 * (3u + v)   (mod n)
//       den  = 16 * x_0 * v            (mod n)
//       a24  = num * den^{-1}          (mod n)
//   The pool pre-builds these per (sigma) under a ThreadPool fan-out, so the
//   curve-pop path on the hot critical section is just a vector move under
//   a small mutex.
//
// Lucky-factor capture:
//   When den has a non-trivial gcd with N (or when den is not invertible),
//   the standard ECM body would either bail out or extract a factor on the
//   spot. The pool captures that factor inside `CachedCurve::lucky_factor`
//   and exposes it to callers — the calling factorisation routine can use
//   it directly (and skip running Stage 1 with this curve, which would be
//   undefined).
//
// API surface:
//   - `CachedCurve` — value type with valid/lucky/standard variants.
//   - `EcmCurvePool` — bounded queue with mutex; `pop_curve` returns the
//     next curve (synchronous fallback if pool drained).
//   - `ecm_curve_pool_size_from_env()` — parse GNFS_ECM_CURVE_POOL ENV.
//
// Non-goals:
//   - We do NOT change the existing ECM::factor / ECM::quick_factor public
//     path. The pool is an opt-in helper layered on top.
//   - Sigma generation is the caller's responsibility; we accept a vector
//     of sigmas to keep the pool deterministic (callers can pass either a
//     deterministic sequence or randomly generated ones).

#include "../core/integer.hpp"
#include "../util/thread_pool.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace gnfs::cofactor {

using core::Integer;

/// A single Suyama-parametrised Montgomery curve, ready for Stage-1 use.
///
/// Three states:
///   1. valid && !lucky_factor: standard curve, callers run mont_mul as usual.
///   2. valid && lucky_factor.has_value(): den-gcd hit a non-trivial factor
///      during setup; callers should consume the factor and skip Stage 1.
///   3. !valid: setup failed in a way that's neither recoverable nor lucky
///      (e.g. den was 0 mod n or sigma < 6). Callers should pop the next.
struct CachedCurve {
    Integer A;             // a24 = (A+2)/4, ready for mont_double
    Integer x_0;           // starting point X (Z=1 implicit by Suyama math)
    Integer z_0;           // starting point Z
    uint64_t sigma = 0;    // for traceability / determinism

    bool valid = false;                          // whether (A, x_0, z_0) are usable
    std::optional<Integer> lucky_factor;         // non-empty iff den-gcd hit a factor
};

/// Build one Suyama-parametrised curve given (n, sigma).
///
/// Returns a `CachedCurve` with `valid=true` if setup succeeded, possibly
/// carrying a `lucky_factor`. On unrecoverable failures (e.g. sigma < 6),
/// returns `valid=false`.
[[nodiscard]] inline CachedCurve build_suyama_curve(const Integer& n, uint64_t sigma) {
    CachedCurve out;
    out.sigma = sigma;

    if (sigma < 6) {
        out.valid = false;
        return out;
    }

    // u = sigma^2 - 5 mod n; v = 4*sigma mod n.
    Integer u(static_cast<unsigned long long>(sigma * sigma - 5));
    u %= n;
    Integer v(static_cast<unsigned long long>(4 * sigma));
    v %= n;

    // x_0 = u^3 mod n; z_0 = v^3 mod n.
    Integer x0;
    mpz_powm_ui(x0.get_mpz(), u.get_mpz(), 3, n.get_mpz());
    Integer z0;
    mpz_powm_ui(z0.get_mpz(), v.get_mpz(), 3, n.get_mpz());

    // diff = v - u mod n.
    Integer diff;
    mpz_sub(diff.get_mpz(), v.get_mpz(), u.get_mpz());
    if (diff.is_negative()) diff += n;
    diff %= n;

    Integer diff3;
    mpz_powm_ui(diff3.get_mpz(), diff.get_mpz(), 3, n.get_mpz());

    // sum3u_v = v + 3u mod n via mpz_addmul_ui.
    Integer sum3u_v;
    sum3u_v = v;
    mpz_addmul_ui(sum3u_v.get_mpz(), u.get_mpz(), 3);
    sum3u_v %= n;

    // numerator = diff^3 * (v + 3u) mod n.
    Integer numerator;
    mpz_mul(numerator.get_mpz(), diff3.get_mpz(), sum3u_v.get_mpz());
    numerator %= n;

    // denom = 16 * x_0 * v mod n.
    Integer denom;
    mpz_mul(denom.get_mpz(), x0.get_mpz(), v.get_mpz());
    denom %= n;
    mpz_mul_2exp(denom.get_mpz(), denom.get_mpz(), 4);
    denom %= n;

    // gcd(denom, n) — lucky-factor hit if 1 < g < n.
    Integer g = core::gcd(denom, n);
    if (!g.is_one()) {
        if (g.compare(n) == 0) {
            // den ≡ 0 (mod n) — degenerate, can't build a valid curve.
            out.valid = false;
            return out;
        }
        // Lucky factor! Capture it and mark valid=true so caller can consume.
        out.lucky_factor = std::move(g);
        out.valid = true;
        return out;
    }

    Integer denom_inv = core::mod_inverse(denom, n);
    if (denom_inv.is_zero()) {
        out.valid = false;
        return out;
    }

    // a24 = (A+2)/4 = numerator * denom_inv mod n. Ready for mont_double.
    Integer a24;
    mpz_mul(a24.get_mpz(), numerator.get_mpz(), denom_inv.get_mpz());
    a24 %= n;

    out.A = std::move(a24);
    out.x_0 = std::move(x0);
    out.z_0 = std::move(z0);
    out.valid = true;
    return out;
}

/// Bounded warm-pool of ready-to-use Montgomery curves.
///
/// Construction performs the full pre-generation (parallel under a small
/// per-instance ThreadPool when the pool size is large enough to amortise
/// the fan-out overhead, otherwise sequential). `pop_curve` returns the
/// next available curve under the pool mutex; if the pool is drained it
/// falls back to a synchronous `build_suyama_curve` call using one of the
/// supplied sigmas (round-robin via an internal counter).
///
/// Thread-safety:
///   - `pop_curve`, `size`, `empty` are safe to call concurrently.
///   - The pool itself is not copyable (mutex member).
class EcmCurvePool {
public:
    /// Build a pool of `pool_size` curves for `n` using the given sigmas.
    ///
    /// If `sigmas.size() < pool_size`, the pool generates `sigmas.size()`
    /// curves only (no implicit sigma generation — caller chooses).
    /// If `sigmas.size() > pool_size`, the extra sigmas are remembered for
    /// the synchronous fallback path in `pop_curve` after the pool drains.
    ///
    /// `parallel_threads = 0` selects a sensible default
    /// (`std::thread::hardware_concurrency()` capped at 8).
    explicit EcmCurvePool(size_t pool_size,
                          const Integer& n,
                          std::vector<uint64_t> sigmas,
                          size_t parallel_threads = 0)
        : n_(n), sigmas_(std::move(sigmas)), next_sigma_idx_(0) {
        if (pool_size == 0) return;

        // Clamp to available sigmas.
        size_t to_build = std::min(pool_size, sigmas_.size());
        if (to_build == 0) return;

        pool_.reserve(to_build);

        // Parallelisation only pays off when we have ≥ 4 curves to build.
        // Fewer than that, sequential is faster than spawning a pool.
        if (to_build >= 4) {
            uint32_t nthreads = static_cast<uint32_t>(parallel_threads);
            if (nthreads == 0) {
                nthreads = std::thread::hardware_concurrency();
                if (nthreads == 0) nthreads = 4;
                if (nthreads > 8) nthreads = 8;
            }
            if (nthreads > to_build) nthreads = static_cast<uint32_t>(to_build);

            util::ThreadPool tp(nthreads);
            std::vector<std::future<CachedCurve>> futures;
            futures.reserve(to_build);
            for (size_t i = 0; i < to_build; ++i) {
                uint64_t sg = sigmas_[i];
                futures.push_back(tp.submit([this, sg]() {
                    return build_suyama_curve(n_, sg);
                }));
            }
            for (auto& f : futures) {
                pool_.push_back(f.get());
            }
        } else {
            for (size_t i = 0; i < to_build; ++i) {
                pool_.push_back(build_suyama_curve(n_, sigmas_[i]));
            }
        }

        // After bulk build the fallback index resumes from the first sigma
        // not yet consumed (only meaningful when sigmas_.size() > pool_size).
        next_sigma_idx_.store(to_build, std::memory_order_relaxed);
    }

    // Non-copyable / non-movable: holds a mutex.
    EcmCurvePool(const EcmCurvePool&) = delete;
    EcmCurvePool& operator=(const EcmCurvePool&) = delete;
    EcmCurvePool(EcmCurvePool&&) = delete;
    EcmCurvePool& operator=(EcmCurvePool&&) = delete;

    /// Pop the next ready curve. If the pool is drained, synchronously
    /// build one using the next unused sigma. If no sigmas remain either,
    /// returns a `CachedCurve` with `valid=false`.
    [[nodiscard]] CachedCurve pop_curve() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!pool_.empty()) {
                CachedCurve c = std::move(pool_.back());
                pool_.pop_back();
                return c;
            }
        }
        // Synchronous fallback: grab next unused sigma (round-robin).
        size_t idx = next_sigma_idx_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= sigmas_.size()) {
            // Truly drained — caller should generate a fresh sigma if needed.
            CachedCurve c;
            c.valid = false;
            return c;
        }
        return build_suyama_curve(n_, sigmas_[idx]);
    }

    /// Current pool occupancy (pre-built curves still in the queue).
    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return pool_.size();
    }

    /// True iff `size() == 0`. Convenience shortcut.
    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return pool_.empty();
    }

    /// Sigma index that the next synchronous-fallback `pop_curve` would use.
    /// Exposed for tests / diagnostics.
    [[nodiscard]] size_t next_sigma_index() const noexcept {
        return next_sigma_idx_.load(std::memory_order_relaxed);
    }

    /// Total sigmas remembered (pool capacity hint + fallback reserve).
    [[nodiscard]] size_t total_sigmas() const noexcept { return sigmas_.size(); }

private:
    Integer n_;
    std::vector<uint64_t> sigmas_;
    std::vector<CachedCurve> pool_;
    mutable std::mutex mu_;
    std::atomic<size_t> next_sigma_idx_;
};

/// Parse `GNFS_ECM_CURVE_POOL` ENV. Returns 0 (disabled) on:
///   - ENV unset / empty / invalid
///   - parsed value < 4 (below this size the warm-pool overhead exceeds the
///     savings — callers should fall through to the legacy direct ECM path)
/// Otherwise returns the parsed pool size, capped at 1024.
[[nodiscard]] inline size_t ecm_curve_pool_size_from_env() noexcept {
    const char* env = std::getenv("GNFS_ECM_CURVE_POOL");
    if (env == nullptr || env[0] == '\0') return 0;

    char* end = nullptr;
    unsigned long parsed = std::strtoul(env, &end, 10);
    if (end == env) return 0;
    if (parsed < 4) return 0;
    if (parsed > 1024) parsed = 1024;
    return static_cast<size_t>(parsed);
}

} // namespace gnfs::cofactor
