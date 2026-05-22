#pragma once

// Montgomery batch inversion helper for ECM Stage 1 / Stage 2 hot loops.
//
// Background:
//   ECM point arithmetic (Montgomery curve point doubling / addition,
//   Suyama parametrisation, Brent-Suyama polynomial extension) repeatedly
//   inverts mod-N values. For 50d+/60d cofactors (N around 200-330 bits)
//   `mpz_invert` (extended Euclidean) is materially more expensive than
//   `mpz_mul + mpz_mod`. Montgomery's classical "batch inversion trick"
//   amortises a single modular inverse across a batch of k values:
//
//     forward: p_0 = v_0;   p_i = p_{i-1} * v_i mod n      (k-1 mults)
//     central: q   = p_{k-1}^{-1} mod n                    (1 invert)
//     reverse: inv_i = q * p_{i-1} mod n, q = q * v_i mod n (2*(k-1) mults)
//
//   Total: 1 inverse + 3k mults (vs naive k inverses + 0 mults). For large
//   N where inverse cost > 3 * mult cost (typical) the batch path wins as
//   soon as k >= 2; the break-even point widens further as N grows.
//
// ENV `GNFS_ECM_BATCH_INV = 0 | 1` (default 0):
//   * 0 (unset, "0", any non-"1" string): helper available but ECM hot
//     loops keep their current per-element `mpz_invert` path. Zero
//     behaviour change.
//   * 1: helper available and callers that have wired the gate via
//        `ecm_batch_inv_enabled()` may opt into the batched path.
//
//   At this commit the helper is future-infrastructure: no caller in the
//   main ECM pipeline (`ECM::stage1` / `ECM::stage2` etc.) reads the gate
//   yet. The header is provided so that targeted hot loops can be migrated
//   incrementally with bit-for-bit parity checks against the existing
//   per-element inverse path.
//
// Bit-for-bit guarantee:
//   The Montgomery trick is *mathematically equivalent* to per-element
//   `mpz_invert`, not an approximation. Provided that
//   gcd(v_i, n) == 1 for all i, `batch_mod_inverse(values, n).inverses[i]`
//   equals `naive_mod_inverse(values, n).inverses[i]` for every i (and
//   equals `mpz_invert(_, v_i, n)`). The unit test suite asserts this
//   per-index across k = 1..100 and varying n widths (small prime,
//   ~2^64 prime, ~200-bit prime).
//
// Failure semantics:
//   ECM's standard "lucky failure" path relies on a non-invertible value
//   exposing a non-trivial factor of n. `batch_mod_inverse` mirrors this:
//   when `mpz_invert(_, p_{k-1}, n) == 0` we know gcd(p_{k-1}, n) > 1,
//   which implies at least one v_i shares a factor with n. We then sweep
//   the input span linearly and return the first non-trivial
//   gcd(v_i, n) in `BatchInvResult::found_factor`. `inverses` is empty in
//   that case. `naive_mod_inverse` returns the same `found_factor` for the
//   same input (per-element invert fails on the first culprit; the helper
//   walks the span the same way so both paths report the same i_culprit).
//   Edge case: if some v_i with gcd > 1 yields gcd == n (e.g. v_i is a
//   multiple of n), no non-trivial factor exists and `found_factor` is
//   `std::nullopt` despite the invert failure; callers must treat this as
//   "could not factor" rather than "definitive factor".
//
// Threading:
//   The helper is non-`thread_local` and allocates a single `mpz_t`
//   scratch on the stack per call. Concurrent calls on disjoint
//   `(values, n)` instances are safe (GMP's per-call disjoint-operand
//   guarantee suffices). No mutable static state.

#include "../core/integer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <gmp.h>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gnfs::cofactor {

namespace detail {

/// Cached env-parsed flag for `GNFS_ECM_BATCH_INV`. Reset via
/// `ecm_batch_inv_reset_env_cache_for_testing()` for unit tests that
/// toggle the env between assertions. Per-process, the env is read at
/// most once across `ecm_batch_inv_enabled()` calls.
struct EcmBatchInvCache {
    std::once_flag once;
    std::atomic<bool> value{false};
};

inline EcmBatchInvCache& ecm_batch_inv_cache() noexcept {
    static EcmBatchInvCache cache;
    return cache;
}

/// Parse `GNFS_ECM_BATCH_INV`. Returns `true` only when the env is set to
/// the exact string `"1"`. All other values (unset / empty / "0" /
/// "garbage" / "true" / "2") yield `false`. Conservative parsing matches
/// the project's other strict-"1" gates such as `GNFS_FILTER_RADIX_SORT`.
inline bool parse_ecm_batch_inv_env() noexcept {
    const char* env = std::getenv("GNFS_ECM_BATCH_INV");
    if (env == nullptr) return false;
    return std::strcmp(env, "1") == 0;
}

}  // namespace detail

/// Read the `GNFS_ECM_BATCH_INV` env into a cached boolean. First call
/// parses the env once (via `std::once_flag`); subsequent calls return the
/// cached value via a relaxed atomic load. Strict `"1"` parsing — see
/// `parse_ecm_batch_inv_env()`. Default (env unset / non-"1"): `false`.
[[nodiscard]] inline bool ecm_batch_inv_enabled() noexcept {
    auto& cache = detail::ecm_batch_inv_cache();
    std::call_once(cache.once, [&cache]() {
        cache.value.store(detail::parse_ecm_batch_inv_env(),
                          std::memory_order_relaxed);
    });
    return cache.value.load(std::memory_order_relaxed);
}

/// Reset the cached gate. Intended for unit tests that toggle
/// `GNFS_ECM_BATCH_INV` between assertions via `setenv` / `unsetenv`.
/// Not thread-safe; only call when no concurrent caller is executing
/// `ecm_batch_inv_enabled()` or `batch_mod_inverse()`.
inline void ecm_batch_inv_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::ecm_batch_inv_cache();
    cache.~EcmBatchInvCache();
    new (&cache) detail::EcmBatchInvCache();
}

/// Result of a batch (or naive) modular inverse pass.
///
/// On success `inverses.size() == values.size()` and each `inverses[i] ==
/// values[i]^{-1} mod n`. `found_factor` is `std::nullopt`.
///
/// On failure (some `gcd(values[i], n) > 1`):
///   * `inverses` is empty
///   * `found_factor` contains the first non-trivial `gcd(values[i], n)`
///     encountered when scanning `values` left-to-right, IF one exists
///   * `found_factor` is `std::nullopt` if every failing `v_i` yields a
///     gcd of either 1 or n (no non-trivial divisor extractable)
struct BatchInvResult {
    std::vector<gnfs::core::Integer> inverses;
    std::optional<gnfs::core::Integer> found_factor;
};

namespace detail {

/// Sweep `values` left-to-right looking for the first index whose
/// `gcd(values[i], n)` is a non-trivial divisor of `n`. Returns the
/// non-trivial gcd if found, else `std::nullopt`. Used by both
/// `batch_mod_inverse` (after a failed central `mpz_invert`) and
/// `naive_mod_inverse` (after a per-element failure) so they report the
/// same culprit for the same input.
inline std::optional<gnfs::core::Integer>
find_first_nontrivial_gcd(const std::vector<gnfs::core::Integer>& values,
                          const gnfs::core::Integer& n) {
    gnfs::core::Integer g;
    for (const auto& v : values) {
        mpz_gcd(g.get_mpz(), v.get_mpz(), n.get_mpz());
        // Skip trivial cases: gcd == 1 (this value is invertible) and
        // gcd == n (this value is a multiple of n, no useful factor).
        if (!g.is_one() && g.compare(n) != 0) {
            return g.clone();
        }
    }
    return std::nullopt;
}

}  // namespace detail

/// Compute `values[i]^{-1} mod n` for every i using one modular inverse
/// plus `3*(k-1)` modular multiplications (Montgomery's trick).
///
/// Preconditions:
///   * `n > 0` (caller's responsibility; behaviour undefined otherwise)
///   * Each `values[i]` is reduced mod `n` (the helper still works on
///     unreduced inputs; the resulting inverse is then `(v_i mod n)^{-1}
///     mod n`, which matches per-element `mpz_invert` semantics)
///
/// Returns a `BatchInvResult`:
///   * On full success: `inverses` is the k-element inverse list; `found_factor`
///     is empty
///   * On invert failure: `inverses` is empty; `found_factor` holds the
///     first non-trivial `gcd(values[i], n)` if any (else `std::nullopt`)
///
/// Edge cases:
///   * `values.empty()` -> empty `inverses`, empty `found_factor`
///   * `values.size() == 1` -> single `mpz_invert`, no prefix products
///   * `n == 1` -> `mpz_invert` returns 0, falls through to gcd sweep;
///     every gcd equals 1, so `found_factor` stays empty (mirrors per-
///     element behaviour on n=1)
inline BatchInvResult
batch_mod_inverse(const std::vector<gnfs::core::Integer>& values,
                  const gnfs::core::Integer& n) {
    BatchInvResult result;
    const std::size_t k = values.size();

    if (k == 0) {
        return result;
    }

    if (k == 1) {
        gnfs::core::Integer inv;
        int ok = mpz_invert(inv.get_mpz(),
                            values[0].get_mpz(),
                            n.get_mpz());
        if (ok == 0) {
            result.found_factor = detail::find_first_nontrivial_gcd(values, n);
            return result;
        }
        result.inverses.reserve(1);
        result.inverses.push_back(std::move(inv));
        return result;
    }

    // Forward pass: p[i] = v[0] * v[1] * ... * v[i] mod n.
    // p[0] is a copy (taken mod n) of v[0] so that the central invert
    // operates on a fully reduced value.
    std::vector<gnfs::core::Integer> prefix;
    prefix.reserve(k);
    {
        gnfs::core::Integer p0;
        mpz_mod(p0.get_mpz(), values[0].get_mpz(), n.get_mpz());
        prefix.push_back(std::move(p0));
    }
    for (std::size_t i = 1; i < k; ++i) {
        gnfs::core::Integer pi;
        mpz_mul(pi.get_mpz(),
                prefix[i - 1].get_mpz(),
                values[i].get_mpz());
        mpz_mod(pi.get_mpz(), pi.get_mpz(), n.get_mpz());
        prefix.push_back(std::move(pi));
    }

    // Central: q = (prefix[k-1])^{-1} mod n. On failure sweep for a
    // non-trivial divisor of n and bail out.
    gnfs::core::Integer q;
    {
        int ok = mpz_invert(q.get_mpz(),
                            prefix[k - 1].get_mpz(),
                            n.get_mpz());
        if (ok == 0) {
            result.found_factor = detail::find_first_nontrivial_gcd(values, n);
            return result;
        }
    }

    // Reverse pass: inv[i] = q * prefix[i-1] mod n, then q = q * v[i] mod n.
    // After the loop q holds (prefix[0])^{-1} mod n = v[0]^{-1} mod n.
    std::vector<gnfs::core::Integer> inverses(k);
    for (std::size_t i = k - 1; i >= 1; --i) {
        gnfs::core::Integer inv_i;
        mpz_mul(inv_i.get_mpz(),
                q.get_mpz(),
                prefix[i - 1].get_mpz());
        mpz_mod(inv_i.get_mpz(), inv_i.get_mpz(), n.get_mpz());
        inverses[i] = std::move(inv_i);

        // Update q for the next iteration: q := q * v[i] mod n
        // (this turns q from prefix[i]^{-1} into prefix[i-1]^{-1}).
        mpz_mul(q.get_mpz(), q.get_mpz(), values[i].get_mpz());
        mpz_mod(q.get_mpz(), q.get_mpz(), n.get_mpz());
    }
    inverses[0] = std::move(q);

    result.inverses = std::move(inverses);
    return result;
}

/// Compute `values[i]^{-1} mod n` for every i using `k` per-element
/// `mpz_invert` calls. Reference implementation used by the unit tests to
/// assert bit-for-bit parity with `batch_mod_inverse`. May also be useful
/// for callers that want an unambiguous "no batched trick" baseline path.
///
/// Returns the same `BatchInvResult` shape as `batch_mod_inverse`:
///   * On success: `inverses` is the k-element inverse list (per-element)
///   * On failure (some `mpz_invert` returns 0): `inverses` is empty;
///     `found_factor` is the first non-trivial `gcd(values[i], n)` (same
///     sweep as `batch_mod_inverse`, so both paths report the same culprit
///     for the same input)
inline BatchInvResult
naive_mod_inverse(const std::vector<gnfs::core::Integer>& values,
                  const gnfs::core::Integer& n) {
    BatchInvResult result;
    const std::size_t k = values.size();

    if (k == 0) {
        return result;
    }

    std::vector<gnfs::core::Integer> inverses;
    inverses.reserve(k);

    for (std::size_t i = 0; i < k; ++i) {
        gnfs::core::Integer inv;
        int ok = mpz_invert(inv.get_mpz(),
                            values[i].get_mpz(),
                            n.get_mpz());
        if (ok == 0) {
            // Failure: sweep input span for first non-trivial gcd and
            // return without partial inverses (matches batch path
            // semantics).
            result.found_factor = detail::find_first_nontrivial_gcd(values, n);
            return result;
        }
        inverses.push_back(std::move(inv));
    }

    result.inverses = std::move(inverses);
    return result;
}

}  // namespace gnfs::cofactor
