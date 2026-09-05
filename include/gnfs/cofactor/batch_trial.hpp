#pragma once

// Batch trial division (T5 Stage 1) — process K cofactors together with a
// shared small-primes table, sweeping outer-by-prime / inner-by-cofactor.
//
// Motivation:
//   The per-cofactor trial-division path (TrialDivider::divide_*) is a hot
//   loop in GNFS sieve Phase 4. The wheel-2-3-5 fast prefix and uint128 fast
//   path already win the per-cofactor case, but each call still iterates the
//   factor base anew. When the sieve queues many cofactors for examination
//   simultaneously (≥ 8 at a time), a batch sweep amortises the outer prime
//   loop and improves cache locality for the small-primes table.
//
// Design:
//   - Lazy-static shared small-primes sieve (segmented sieve to prime_bound).
//   - For each prime p ≤ prime_bound: sweep all K cofactors, divide out p
//     where p | cofactor[i] (full power, exp bounded at 255 for safety).
//   - After the outer prime loop terminates, cofactor[i] == 1 is recorded
//     as smooth; otherwise the residual is the "remaining" non-smooth part.
//
// Cooperation with existing code:
//   - `BatchTrialResult` stores parallel `is_smooth[i]` + `remaining[i]`
//     vectors with size == input span. Factor-and-exponent vectors are NOT
//     returned (this API is a smoothness pre-filter, not a full factorization
//     replacement). For relations the caller still goes through TrialDivider
//     for the factor-index/exponent accounting.
//   - ENV-gated via `batch_trial_size_from_env()`. When K=1 (default) the
//     wider GNFS pipeline keeps the per-cofactor path; opting in requires
//     `GNFS_COFACTOR_BATCH_SIZE=K` with K ≥ 2 at runtime.
//
// Cross-platform notes:
//   - Sieve uses std::vector<bool> + simple Eratosthenes (zero-alloc beyond
//     the bool buffer + the prime list itself).
//   - The thread-safety contract is "safe to call concurrently from many
//     threads"; the lazy-static is built once under std::call_once and the
//     resulting vector is immutable after that. Per-call scratch lives on
//     the stack.

#include "../core/integer.hpp"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::cofactor {

using core::Integer;

/// Result of batch trial division. Same length as the input span.
///
/// Index `i` corresponds to input cofactor `i`. `is_smooth[i]` is true iff
/// every prime factor of cofactor[i] is ≤ prime_bound (i.e. the residual
/// reduced to 1). `remaining[i]` always contains the residual after sweeping
/// the prime list — equal to 1 when `is_smooth[i]` is true.
struct BatchTrialResult {
    std::vector<bool> is_smooth;
    std::vector<Integer> remaining;

    [[nodiscard]] size_t size() const noexcept { return is_smooth.size(); }
    [[nodiscard]] bool empty() const noexcept { return is_smooth.empty(); }
};

namespace detail {

/// Keep the eager Eratosthenes table bounded. An invalid/unbounded caller
/// value must fail before `prime_bound + 1` can wrap or allocate excessively.
inline constexpr size_t kMaxBatchTrialPrimeBound = 100'000'000;

inline void validate_batch_trial_prime_bound(size_t prime_bound) {
    if (prime_bound > kMaxBatchTrialPrimeBound) {
        throw std::invalid_argument("batch trial prime bound exceeds sieve cap (100M)");
    }
}

/// Lazy-allocated small-primes table for a given prime_bound. Built via
/// Sieve of Eratosthenes on first request per (bound) value. Two-level cache:
///   - The outermost `mutex` protects the deque of (bound, primes) entries.
///   - Each entry stores the cached primes for one bound.
/// For the typical GNFS workload prime_bound varies across one or two values
/// (rational vs algebraic factor base bounds), so linear scan is fine.
[[nodiscard]] inline const std::vector<uint64_t>& small_primes_for(size_t prime_bound) {
    struct Entry {
        size_t bound;
        std::vector<uint64_t> primes;
    };
    static std::mutex mu;
    // References are returned after the lock is released. deque keeps existing
    // entries stable when another thread inserts a new bound.
    static std::deque<Entry> cache;

    std::lock_guard<std::mutex> lock(mu);
    validate_batch_trial_prime_bound(prime_bound);
    for (const auto& e : cache) {
        if (e.bound == prime_bound) return e.primes;
    }

    // Eratosthenes sieve to prime_bound (inclusive).
    Entry entry;
    entry.bound = prime_bound;
    if (prime_bound < 2) {
        cache.push_back(std::move(entry));
        return cache.back().primes;
    }

    std::vector<bool> is_prime(prime_bound + 1, true);
    is_prime[0] = is_prime[1] = false;
    for (size_t i = 2; i <= prime_bound / i; ++i) {
        if (is_prime[i]) {
            for (size_t j = i * i; j <= prime_bound; j += i) {
                is_prime[j] = false;
            }
        }
    }
    // π(n) ≈ n / ln(n); reserve approximate count to avoid log(n) reallocs.
    size_t approx_pi = static_cast<size_t>(static_cast<double>(prime_bound) /
                                           std::max(1.0, std::log(static_cast<double>(prime_bound))));
    entry.primes.reserve(approx_pi + 16);
    for (size_t i = 2; i <= prime_bound; ++i) {
        if (is_prime[i]) entry.primes.push_back(static_cast<uint64_t>(i));
    }

    cache.push_back(std::move(entry));
    return cache.back().primes;
}

/// Divide value by p as many times as possible; the operation is performed
/// in-place. This result is a smoothness pre-filter and has no uint8 exponent
/// field, so truncating the valuation would misclassify high prime powers.
inline void strip_prime_inplace(Integer& value, uint32_t p) {
    // Fast path: uint64 division.
    if (value.fits_uint64()) {
        uint64_t v = value.to_uint64();
        while (v != 0 && (v % p) == 0) {
            v /= p;
        }
        value = v;
        return;
    }
    // GMP fallback for big integers.
    while (mpz_divisible_ui_p(value.get_mpz(), p) != 0) {
        mpz_divexact_ui(value.get_mpz(), value.get_mpz(), p);
    }
}

} // namespace detail

/// Batch trial division — sweep prime list outer, cofactors inner.
///
/// @param cofactors absolute-value cofactors to test (any sign treated as |x|)
/// @param prime_bound largest prime included in the sweep (inclusive)
/// @return parallel vectors: is_smooth[i] + remaining[i]
///
/// Complexity: O(P · K + sum(log_p(cofactor[i])) ) where P = π(prime_bound).
/// The outer-by-prime layout pulls each prime's div-loop into L1 once per
/// batch instead of per cofactor.
///
/// Determinism: deterministic; identical inputs produce identical results.
/// Thread-safety: safe to call concurrently; lazy primes table is built once.
[[nodiscard]] inline BatchTrialResult batch_trial_divide(std::span<const Integer> cofactors, size_t prime_bound) {

    detail::validate_batch_trial_prime_bound(prime_bound);

    BatchTrialResult result;
    result.is_smooth.assign(cofactors.size(), false);
    result.remaining.reserve(cofactors.size());

    // Copy cofactors into `remaining`, normalising sign.
    for (const auto& c : cofactors) {
        Integer copy = c; // copy ctor (Integer copy ctor clones mpz_t)
        if (copy.is_negative()) copy.negate();
        result.remaining.push_back(std::move(copy));
    }

    // Initial smoothness check (cofactor already 0 or 1 → done).
    bool all_done = true;
    for (size_t i = 0; i < result.remaining.size(); ++i) {
        if (result.remaining[i].fits_uint64()) {
            uint64_t v = result.remaining[i].to_uint64();
            if (v == 0 || v == 1) {
                result.is_smooth[i] = true;
                if (v == 0) result.remaining[i] = uint64_t{1};
            } else {
                all_done = false;
            }
        } else {
            all_done = false;
        }
    }
    if (all_done) return result;

    const auto& primes = detail::small_primes_for(prime_bound);

    // Outer-by-prime, inner-by-cofactor sweep.
    for (uint64_t p : primes) {
        bool any_remaining = false;
        for (size_t i = 0; i < result.remaining.size(); ++i) {
            if (result.is_smooth[i]) continue;
            Integer& v = result.remaining[i];

            // Fast skip: divisibility test before strip.
            bool divisible;
            if (v.fits_uint64()) {
                uint64_t vu = v.to_uint64();
                if (vu == 1) {
                    result.is_smooth[i] = true;
                    continue;
                }
                divisible = (vu % p) == 0;
            } else {
                divisible = mpz_divisible_ui_p(v.get_mpz(), p) != 0;
            }
            if (divisible) {
                detail::strip_prime_inplace(v, static_cast<uint32_t>(p));
                if (v.fits_uint64() && v.to_uint64() == 1) {
                    result.is_smooth[i] = true;
                    continue;
                }
            }
            any_remaining = true;
        }
        if (!any_remaining) break; // every cofactor done
    }

    // Final smoothness pass — anything that reduced to 1 along the way and
    // wasn't already marked also gets marked.
    for (size_t i = 0; i < result.remaining.size(); ++i) {
        if (!result.is_smooth[i] && result.remaining[i].fits_uint64() && result.remaining[i].to_uint64() == 1) {
            result.is_smooth[i] = true;
        }
    }

    return result;
}

/// Parse `GNFS_COFACTOR_BATCH_SIZE` ENV. Returns 1 (disabled) on any of:
///   - ENV unset
///   - ENV not a valid positive integer
///   - parsed value < 2 or has a negative sign
/// Otherwise returns the parsed batch size, capped at 4096 to guard against
/// pathological values causing excessive memory pressure.
[[nodiscard]] inline size_t batch_trial_size_from_env() noexcept {
    const char* env = std::getenv("GNFS_COFACTOR_BATCH_SIZE");
    if (env == nullptr || env[0] == '\0') return 1;

    // Manual parse to avoid exception overhead. strtoul("-1", ...) returns
    // ULONG_MAX, which would otherwise bypass the lower-bound check.
    const char* first = env;
    while (*first != '\0' && std::isspace(static_cast<unsigned char>(*first))) {
        ++first;
    }
    if (*first == '-') return 1;

    char* end = nullptr;
    unsigned long parsed = std::strtoul(first, &end, 10);
    if (end == first) return 1;
    if (parsed < 2) return 1;
    if (parsed > 4096) parsed = 4096;
    return static_cast<size_t>(parsed);
}

} // namespace gnfs::cofactor
