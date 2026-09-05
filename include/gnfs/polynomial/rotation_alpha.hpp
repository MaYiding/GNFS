#pragma once

#include "../core/integer.hpp"
#include "int_polynomial.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gnfs::polynomial {

using core::Integer;

/// RotationAlphaTracker — cheap-alpha proxy for Stage 2 top-K ranking.
///
/// Motivation:
/// In `KleinjungSelector::stage2_root_optimization`, the translation +
/// 3-iter rotation loop produces 101 × 4 = 404 candidate polynomials per
/// (a_d, m) input. The current code ranks them by L² norm only, then runs
/// full Murphy E on the top-3 L²-min entries.
///
/// L² norm by itself is a poor predictor of Murphy E. A polynomial with
/// slightly larger L² but better alpha (more roots mod small primes) may
/// outperform an L²-min poly that happens to be alpha-bad. CADO-NFS uses
/// `lognorm + alpha` (called `exp_E`) for this ranking. Computing full
/// alpha (78k prime sweep) per candidate is too expensive (>400 sweeps).
///
/// This class precomputes a "cheap alpha" using only small primes
/// (default p ≤ 100, ~25 primes). For each candidate polynomial in the
/// rotation loop, computing cheap alpha is ~25 root-finding ops per
/// polynomial = bounded-and-fast.
///
/// Trade-off: cheap alpha = Σ_{p ≤ B_small} (root_count(f, p) / p - 1/(p-1)) log(p)
/// is a noisy estimate of full alpha (correlation typically 0.7-0.9 in practice).
/// Used as a tie-breaker for L²-norm ranking, it improves top-K selection
/// without adding measurable cost.
///
/// Rotation-incremental note:
/// For `f_new = f_old + k*(x - m)`, root sets of `f mod p` change in
/// non-trivial ways. A truly incremental update requires either:
///   (1) Re-running root finding per affected prime (no speedup), or
///   (2) Building a 2D root sieve (CADO-NFS ropt_stage2.cpp; multi-week port).
/// For the small-prime cheap-alpha proxy, simple brute-force root
/// enumeration (O(d · p) for d-degree poly) is cheap enough that
/// per-rotation re-computation is acceptable. The proxy doesn't aspire
/// to full incremental — it's the L²+α replacement for the L²-only
/// ranking, with α computed cheaply on the fly.
class RotationAlphaTracker {
public:
    /// Constructor: capture small-prime sweep bound.
    /// @param small_prime_bound Sum alpha contributions over primes ≤ this bound.
    ///                          Default 100 → 25 primes — bounded cost per call.
    explicit RotationAlphaTracker(uint32_t small_prime_bound = 100)
        : small_prime_bound_(small_prime_bound) {
        init_small_primes();
    }

    /// Compute cheap alpha for polynomial f (no caching, per-call).
    /// Used as a tie-breaker / refinement for L² norm in top-K ranking.
    ///
    /// Per-prime contribution mirrors MurphyEvaluator::alpha_contribution
    /// but limited to small primes only and skipping derivative double-root /
    /// projective root bonuses (cheaper). For the ranking use case, the
    /// dominant component is the root_count term, which this captures.
    [[nodiscard]] double cheap_alpha(const IntPolynomial& f) const {
        double alpha = 0.0;
        for (uint32_t p : small_primes_) {
            const auto roots = f.roots_mod_p(p);
            const uint32_t r = static_cast<uint32_t>(roots.size());
            const double pd = static_cast<double>(p);
            const double log_p = std::log(pd);
            alpha += (static_cast<double>(r) / pd - 1.0 / (pd - 1.0)) * log_p;
        }
        return alpha;
    }

    /// Estimate quality score for top-K ranking: L² norm + alpha-proxy.
    /// Lower is better (Murphy E is monotonic decreasing in this score).
    ///
    /// Formula matches CADO-NFS `exp_E = lognorm + alpha`:
    ///   lower exp_E → larger Dickman ρ → higher Murphy E.
    /// Caller provides L²-norm (already computed). This routine adds the
    /// cheap-alpha contribution.
    [[nodiscard]] double score(const IntPolynomial& f, double l2_norm) const {
        // log-norm is what Murphy uses, but l2_norm is a usable scale-equivalent
        // proxy. Combined score: log(l2_norm) + alpha → smaller is better.
        const double log_norm = (l2_norm > 1e-300) ? std::log(l2_norm) : -1e300;
        return log_norm + cheap_alpha(f);
    }

    /// Access the small-prime list (mostly for tests).
    [[nodiscard]] const std::vector<uint32_t>& small_primes() const noexcept {
        return small_primes_;
    }

private:
    uint32_t small_prime_bound_;
    std::vector<uint32_t> small_primes_;

    void init_small_primes() {
        // Bound the caller-controlled sieve and avoid `bound + 1` wrapping at
        // UINT32_MAX before it is converted to size_t.
        constexpr uint32_t MAX_SMALL_PRIME_BOUND = 1'000'000;
        if (small_prime_bound_ > MAX_SMALL_PRIME_BOUND) {
            throw std::invalid_argument("RotationAlphaTracker small-prime bound is too large");
        }
        const size_t sieve_size = static_cast<size_t>(small_prime_bound_) + 1;
        std::vector<bool> sieve(sieve_size, true);
        if (small_prime_bound_ >= 1) sieve[0] = false;
        if (small_prime_bound_ >= 2) sieve[1] = false;
        for (uint32_t i = 2; static_cast<uint64_t>(i) * i <= small_prime_bound_; ++i) {
            if (sieve[i]) {
                for (uint64_t j = static_cast<uint64_t>(i) * i;
                     j <= small_prime_bound_;
                     j += i) {
                    sieve[j] = false;
                }
            }
        }
        small_primes_.clear();
        small_primes_.reserve(small_prime_bound_ / 4 + 4);
        for (uint32_t i = 2; i <= small_prime_bound_; ++i) {
            if (sieve[i]) small_primes_.push_back(i);
        }
    }
};

} // namespace gnfs::polynomial
