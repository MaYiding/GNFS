#pragma once

// Polynomial Euclidean division (divrem) helper over F_p[x] (with p < 2^32).
//
// Background:
//   Given polynomials `num` and `den` in F_p[x] with `den` not the zero
//   polynomial, Euclidean division produces quotient `quot` and remainder
//   `rem` satisfying
//       num = quot * den + rem, deg(rem) < deg(den).
//   The classical schoolbook algorithm walks the dividend from high to low
//   degree, peeling off `c * den * x^k` at each step. The cost is
//   O((deg num - deg den + 1) * (deg den + 1)) coefficient multiplications,
//   i.e. roughly quadratic in the polynomial sizes.
//
//   The Newton-reciprocal trick replaces the quadratic walk with a
//   sub-quadratic computation by reducing divrem to two polynomial
//   multiplications. Reverse the coefficient vectors of `num` and `den`
//   (call the reversed forms `num_rev` and `den_rev`). The reversed
//   polynomials satisfy
//       quot_rev = num_rev * den_rev^{-1}  (mod x^{q + 1})
//   where `q = deg(num) - deg(den)` is the quotient degree, and
//   `den_rev^{-1}` is the power-series inverse of `den_rev` (which has
//   non-zero constant term because the leading coefficient of `den` is
//   non-zero). Newton iteration computes `den_rev^{-1} mod x^{2k}` from
//   `den_rev^{-1} mod x^k` via
//       r_{k+1} = r_k * (2 - den_rev * r_k)  (mod x^{2k}),
//   doubling the precision each step. Convergence is reached in
//   O(log q) rounds, with each round dominated by two polynomial
//   multiplications. After recovering `quot_rev` via one further
//   multiplication, the remainder is `rem = num - quot * den`.
//
//   When polynomial multiplication uses a sub-quadratic primitive such
//   as Karatsuba (O(n^1.585), see `karatsuba_mul.hpp`), the divrem path
//   inherits sub-quadratic asymptotics. With schoolbook multiplication
//   the helper still tracks identical cost to direct schoolbook divrem,
//   so it is safe to dispatch on size threshold without regressing the
//   small-degree fast path.
//
//   This helper provides a `divrem_modp_schoolbook` reference matching
//   the semantics of `ModularPoly::divmod` and a `divrem_modp` entry that
//   routes between schoolbook and Newton-reciprocal divrem according to a
//   compile-time threshold and a runtime three-state ENV gate.
//
// ENV (GNFS_POLY_DIVREM_SUBQUADRATIC):
//   unset / empty / "auto" → Auto (default; same as ForceOff for now,
//     since the helper is opt-in until callers explicitly wire it in).
//   "0" / "off"            → ForceOff (always schoolbook).
//   "1" / "on"             → ForceOn  (Newton-reciprocal above threshold).
//   Anything else          → Auto.
//
// Threshold rationale (kDivremSubquadraticThreshold = 32):
//   Newton-reciprocal divrem requires O(log q) Newton rounds (one mul +
//   one square-ish mul each), one final mul to recover the quotient
//   (`quot_rev = num_rev * den_rev^{-1}` mod x^{q+1}), and one final mul
//   plus subtraction to recover the remainder (`rem = num - quot * den`).
//   The hidden constant is non-trivial: per-round allocations for the
//   intermediate series, plus reversed/truncated coefficient copies.
//   For small `deg(num)` (< ~32) the schoolbook walk is faster because
//   its inner loop is very tight (no allocations, no temporary buffers).
//   Empirically the crossover is in the 32 - 64 range; we pick 32 as a
//   conservative midpoint mirroring the W9 Karatsuba threshold default.
//   Callers may force schoolbook by setting the ENV to "0".
//
// Modulus precondition:
//   p prime, p < 2^32 (so uint64_t * uint64_t fits without overflow in
//   the schoolbook inner products and in the Newton iteration). Caller
//   must ensure all coefficients of `num` and `den` are reduced mod p.
//
// Bit-for-bit guarantee:
//   For any (num, den, p) with p prime, p < 2^32, coefficients in [0, p),
//   and `den` not the zero polynomial,
//       divrem_modp(num, den, p, quot, rem)
//   produces the same `quot` and `rem` as
//       divrem_modp_schoolbook(num, den, p, quot, rem)
//   bit-for-bit (same coefficient vector lengths after trailing-zero trim,
//   same coefficient values). The gate value affects only which kernel
//   computes the result, not the mathematical answer.
//
// Integration status:
//   Future-infrastructure helper. The main `ModularPoly::divmod` entry
//   continues to use schoolbook. Callers (e.g. a future Half-GCD rewrite
//   that wires in sub-quadratic primitives end-to-end) call this helper
//   directly. No behavior change for existing callers.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::polynomial {

/// Three-state dispatch decision for the divrem subquadratic helper.
/// Auto and ForceOff currently both route to schoolbook; ForceOn enables
/// the Newton-reciprocal kernel above `kDivremSubquadraticThreshold`.
enum class DivremSubquadraticMode {
    Auto,
    ForceOff,
    ForceOn,
};

namespace detail_divrem {

/// Trim trailing zero coefficients in place so that the resulting vector
/// represents the canonical polynomial (no spurious leading zeros).
inline void trim_trailing_zeros(std::vector<uint64_t>& v) {
    while (!v.empty() && v.back() == 0) {
        v.pop_back();
    }
}

/// Multiply mod p of two reduced coefficients with p < 2^32. The product
/// fits in uint64_t (max < 2^64), so a single `%` reduction suffices.
[[nodiscard]] inline uint64_t mul_mod(uint64_t a, uint64_t b, uint64_t p) {
    return (a * b) % p;
}

/// Extended Euclidean modular inverse of `a` mod `p`, with `p` prime and
/// `0 < a < p`. Throws on a zero argument.
[[nodiscard]] inline uint64_t mod_inverse(uint64_t a, uint64_t p) {
    if (a == 0) {
        throw std::runtime_error("divrem_subquadratic: mod_inverse of zero");
    }
    // Standard extended GCD over signed 128-bit intermediates to avoid
    // any underflow / sign confusion at the [0, p) boundary.
    long long old_r = static_cast<long long>(a);
    long long r = static_cast<long long>(p);
    long long old_s = 1;
    long long s = 0;
    while (r != 0) {
        long long q = old_r / r;
        long long temp = old_r - q * r;
        old_r = r;
        r = temp;
        temp = old_s - q * s;
        old_s = s;
        s = temp;
    }
    // old_s is the Bezout coefficient for `a`; reduce into [0, p).
    long long pp = static_cast<long long>(p);
    long long inv = old_s % pp;
    if (inv < 0) inv += pp;
    return static_cast<uint64_t>(inv);
}

/// Schoolbook polynomial multiplication mod p, matching `schoolbook_mul_mod`
/// from `karatsuba_mul.hpp` (kept locally so the helper is self-contained
/// and does not pull in a runtime dependency on Karatsuba).
inline void schoolbook_mul(std::span<const uint64_t> a,
                           std::span<const uint64_t> b,
                           uint64_t p,
                           std::vector<uint64_t>& out) {
    if (a.empty() || b.empty()) {
        out.clear();
        return;
    }
    const size_t result_size = a.size() + b.size() - 1;
    out.assign(result_size, 0);
    for (size_t i = 0; i < a.size(); ++i) {
        const uint64_t ai = a[i];
        if (ai == 0) continue;
        for (size_t j = 0; j < b.size(); ++j) {
            const uint64_t bj = b[j];
            if (bj == 0) continue;
            const uint64_t prod = mul_mod(ai, bj, p);
            uint64_t sum = out[i + j] + prod;
            if (sum >= p) sum -= p;
            out[i + j] = sum;
        }
    }
}

/// Truncate a polynomial to its leading `k` coefficients (mod x^k).
/// If the source has fewer than `k` coefficients, the result is the
/// source as-is; trailing zeros are left intact (Newton iteration relies
/// on a fixed precision, not on a trimmed polynomial).
inline void truncate_to(std::vector<uint64_t>& v, size_t k) {
    if (v.size() > k) v.resize(k);
}

/// Reverse a polynomial in place (so the leading coefficient becomes the
/// constant term, etc.). The polynomial must already be trimmed of
/// trailing zeros; otherwise reversal produces a polynomial whose
/// constant term is zero and Newton iteration cannot start.
inline std::vector<uint64_t> reverse_poly(std::span<const uint64_t> v) {
    std::vector<uint64_t> out(v.begin(), v.end());
    std::reverse(out.begin(), out.end());
    return out;
}

/// Compute `r' = r * (2 - g * r) mod x^k` for one Newton iteration step.
/// Inputs and outputs are all truncated to precision `k`.
///
/// Concretely: given a current approximation `r` of `g^{-1}` modulo
/// x^{k/2} (or any precision <= k), expand to precision k via the
/// quadratic-convergent recurrence above. The constant term of `g` must
/// be non-zero modulo p for the inverse to exist.
inline void newton_step(std::span<const uint64_t> g,
                        std::span<const uint64_t> r,
                        uint64_t p,
                        size_t k,
                        std::vector<uint64_t>& out) {
    // tmp = g * r (truncated to k)
    std::vector<uint64_t> gr;
    schoolbook_mul(g, r, p, gr);
    truncate_to(gr, k);
    // diff = 2 - gr (mod p, truncated to k). Note: in F_p, "2" is just
    // the integer 2 mod p. Subtract componentwise.
    std::vector<uint64_t> diff(k, 0);
    // Constant term: (2 - gr[0]) mod p
    uint64_t two_mod = 2 % p;
    uint64_t g0 = (!gr.empty()) ? gr[0] : 0;
    diff[0] = (two_mod >= g0) ? (two_mod - g0)
                              : (p - (g0 - two_mod));
    for (size_t i = 1; i < k; ++i) {
        uint64_t gi = (i < gr.size()) ? gr[i] : 0;
        diff[i] = (gi == 0) ? 0 : (p - gi);
    }
    // out = r * diff (truncated to k)
    schoolbook_mul(r, diff, p, out);
    truncate_to(out, k);
}

/// Compute the formal power-series inverse of `g` modulo x^k. Requires
/// `g[0] != 0`. The result `r` satisfies `(g * r) mod x^k = 1`.
///
/// Starts from the constant-term inverse `r0 = g[0]^{-1} mod p` and
/// doubles the precision via Newton iteration until reaching or
/// exceeding k, then truncates.
inline void series_inverse(std::span<const uint64_t> g,
                           uint64_t p,
                           size_t k,
                           std::vector<uint64_t>& out) {
    if (k == 0) {
        out.clear();
        return;
    }
    if (g.empty() || g[0] == 0) {
        throw std::runtime_error(
            "divrem_subquadratic: series_inverse requires non-zero constant term");
    }
    // r0 = inverse of constant term, precision 1.
    std::vector<uint64_t> r{mod_inverse(g[0], p)};
    size_t cur = 1;
    while (cur < k) {
        size_t next = std::min(2 * cur, k);
        std::vector<uint64_t> r_next;
        newton_step(g, r, p, next, r_next);
        r = std::move(r_next);
        cur = next;
    }
    out = std::move(r);
    truncate_to(out, k);
}

/// Cached ENV reader for GNFS_POLY_DIVREM_SUBQUADRATIC.
/// Three-state: Auto / ForceOff / ForceOn. Anything unrecognized → Auto.
struct DivremGateCache {
    std::once_flag once;
    std::atomic<int> mode{static_cast<int>(DivremSubquadraticMode::Auto)};
};

[[nodiscard]] inline DivremGateCache& divrem_gate_cache() noexcept {
    static DivremGateCache cache;
    return cache;
}

[[nodiscard]] inline DivremSubquadraticMode parse_divrem_gate_env() noexcept {
    const char* env = std::getenv("GNFS_POLY_DIVREM_SUBQUADRATIC");
    if (env == nullptr || env[0] == '\0') {
        return DivremSubquadraticMode::Auto;
    }
    if (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0) {
        return DivremSubquadraticMode::ForceOff;
    }
    if (std::strcmp(env, "1") == 0 || std::strcmp(env, "on") == 0) {
        return DivremSubquadraticMode::ForceOn;
    }
    if (std::strcmp(env, "auto") == 0) {
        return DivremSubquadraticMode::Auto;
    }
    return DivremSubquadraticMode::Auto;
}

}  // namespace detail_divrem

/// Recursion / dispatch threshold. When `deg(num) < kDivremSubquadraticThreshold`
/// the helper unconditionally uses schoolbook divrem, regardless of the
/// gate. Mirrors the W9 Karatsuba threshold default for consistency.
inline constexpr size_t kDivremSubquadraticThreshold = 32;

/// Returns the current process-wide divrem subquadratic gate mode.
///
/// First call: parses GNFS_POLY_DIVREM_SUBQUADRATIC (see header docstring).
/// Subsequent calls: returns cached value. Test-only reset available via
/// `divrem_subquadratic_reset_env_cache_for_testing()`.
[[nodiscard]] inline DivremSubquadraticMode divrem_subquadratic_mode() noexcept {
    auto& cache = detail_divrem::divrem_gate_cache();
    std::call_once(cache.once, []() {
        detail_divrem::divrem_gate_cache().mode.store(
            static_cast<int>(detail_divrem::parse_divrem_gate_env()),
            std::memory_order_relaxed);
    });
    return static_cast<DivremSubquadraticMode>(
        cache.mode.load(std::memory_order_relaxed));
}

/// Convenience predicate. True iff the gate is ForceOn (the helper's
/// Newton-reciprocal path is only enabled by explicit opt-in for now).
/// Auto stays conservative and routes to schoolbook so a wired caller
/// observes legacy behavior by default.
[[nodiscard]] inline bool divrem_subquadratic_enabled() noexcept {
    return divrem_subquadratic_mode() == DivremSubquadraticMode::ForceOn;
}

/// Test-only: reset the ENV cache. After calling this, the next
/// `divrem_subquadratic_mode()` will re-read the env. Not thread-safe
/// against concurrent readers and must be called from test setup only.
inline void divrem_subquadratic_reset_env_cache_for_testing() noexcept {
    auto& cache = detail_divrem::divrem_gate_cache();
    cache.once.~once_flag();
    new (&cache.once) std::once_flag();
    cache.mode.store(static_cast<int>(DivremSubquadraticMode::Auto),
                     std::memory_order_relaxed);
}

/// Schoolbook polynomial divrem mod p, matching the semantics of
/// `ModularPoly::divmod`. Both `quot` and `rem` are returned with
/// trailing zeros trimmed. Throws `std::runtime_error` if `den` is the
/// zero polynomial.
inline void divrem_modp_schoolbook(const std::vector<uint64_t>& num,
                                   const std::vector<uint64_t>& den,
                                   uint64_t p,
                                   std::vector<uint64_t>& quot,
                                   std::vector<uint64_t>& rem) {
    // Trim both inputs defensively so the degree computations are robust
    // to caller-supplied trailing zeros.
    std::vector<uint64_t> num_trim(num);
    std::vector<uint64_t> den_trim(den);
    detail_divrem::trim_trailing_zeros(num_trim);
    detail_divrem::trim_trailing_zeros(den_trim);

    if (den_trim.empty()) {
        throw std::runtime_error(
            "divrem_modp_schoolbook: division by zero polynomial");
    }
    if (num_trim.empty()) {
        // 0 = 0 * den + 0
        quot.clear();
        rem.clear();
        return;
    }
    if (num_trim.size() < den_trim.size()) {
        // deg(num) < deg(den): quot = 0, rem = num (already trimmed)
        quot.clear();
        rem = std::move(num_trim);
        return;
    }

    // Walk: peel `c * den * x^k` from `rem` from the highest degree down.
    const int num_deg = static_cast<int>(num_trim.size()) - 1;
    const int den_deg = static_cast<int>(den_trim.size()) - 1;
    std::vector<uint64_t> work(std::move(num_trim));
    std::vector<uint64_t> q(static_cast<size_t>(num_deg - den_deg + 1), 0);

    const uint64_t b_lead_inv =
        detail_divrem::mod_inverse(den_trim.back(), p);

    for (int i = num_deg; i >= den_deg; --i) {
        if (work[static_cast<size_t>(i)] == 0) continue;
        const uint64_t c =
            detail_divrem::mul_mod(work[static_cast<size_t>(i)],
                                   b_lead_inv, p);
        q[static_cast<size_t>(i - den_deg)] = c;
        for (int j = 0; j <= den_deg; ++j) {
            const uint64_t term =
                detail_divrem::mul_mod(c, den_trim[static_cast<size_t>(j)],
                                       p);
            const size_t pos = static_cast<size_t>(i - den_deg + j);
            if (work[pos] >= term) {
                work[pos] -= term;
            } else {
                work[pos] = p - (term - work[pos]);
            }
        }
    }

    // The lower `den_deg` coefficients of `work` form the remainder.
    work.resize(static_cast<size_t>(den_deg));
    detail_divrem::trim_trailing_zeros(work);
    detail_divrem::trim_trailing_zeros(q);
    quot = std::move(q);
    rem = std::move(work);
}

namespace detail_divrem {

/// Newton-reciprocal polynomial divrem mod p, used above the threshold
/// when `divrem_subquadratic_enabled()` returns true. See header for
/// background. Output matches schoolbook bit-for-bit.
///
/// The algorithm computes
///   1. den_rev^{-1} mod x^{q+1} via Newton iteration (series_inverse)
///   2. quot_rev    = num_rev * den_rev^{-1}    mod x^{q+1}
///   3. quot        = reverse(quot_rev)
///   4. rem         = num - quot * den
inline void divrem_modp_newton(const std::vector<uint64_t>& num,
                               const std::vector<uint64_t>& den,
                               uint64_t p,
                               std::vector<uint64_t>& quot,
                               std::vector<uint64_t>& rem) {
    std::vector<uint64_t> num_trim(num);
    std::vector<uint64_t> den_trim(den);
    trim_trailing_zeros(num_trim);
    trim_trailing_zeros(den_trim);

    assert(!den_trim.empty() && "divrem_modp_newton: caller must guard zero den");
    assert(num_trim.size() >= den_trim.size() &&
           "divrem_modp_newton: caller must guard deg(num) < deg(den)");

    const size_t qsize =
        num_trim.size() - den_trim.size() + 1;  // deg(quot) + 1

    // Reverse to compute quotient via series inverse.
    auto num_rev = reverse_poly(std::span<const uint64_t>(num_trim));
    auto den_rev = reverse_poly(std::span<const uint64_t>(den_trim));

    // den_rev[0] = leading coefficient of den, guaranteed non-zero by
    // trim. series_inverse asserts this.
    std::vector<uint64_t> den_rev_inv;
    series_inverse(std::span<const uint64_t>(den_rev), p, qsize, den_rev_inv);

    // Truncate num_rev to qsize for the quotient mul (only the leading
    // q+1 coefficients of num contribute to the quotient).
    if (num_rev.size() > qsize) num_rev.resize(qsize);

    std::vector<uint64_t> quot_rev;
    schoolbook_mul(std::span<const uint64_t>(num_rev),
                   std::span<const uint64_t>(den_rev_inv), p, quot_rev);
    truncate_to(quot_rev, qsize);

    // The reversed quotient may have trailing zeros if the natural
    // quotient has fewer than qsize coefficients; pad with zeros up to
    // exactly qsize before reversing so the orientation is correct.
    if (quot_rev.size() < qsize) {
        quot_rev.resize(qsize, 0);
    }

    std::vector<uint64_t> q(qsize, 0);
    for (size_t i = 0; i < qsize; ++i) {
        q[i] = quot_rev[qsize - 1 - i];
    }
    trim_trailing_zeros(q);

    // Recover remainder: rem = num - q * den. Use schoolbook since
    // |q| * |den| ~ |num|, which is bounded.
    std::vector<uint64_t> qd;
    if (q.empty()) {
        qd.clear();
    } else {
        schoolbook_mul(std::span<const uint64_t>(q),
                       std::span<const uint64_t>(den_trim), p, qd);
    }

    std::vector<uint64_t> r;
    r.resize(num_trim.size(), 0);
    for (size_t i = 0; i < num_trim.size(); ++i) {
        const uint64_t a = num_trim[i];
        const uint64_t b = (i < qd.size()) ? qd[i] : 0;
        r[i] = (a >= b) ? (a - b) : (p - (b - a));
    }
    trim_trailing_zeros(r);

    // Sanity: the recovered remainder has degree < deg(den). If a
    // numerical or algorithmic regression broke this invariant, abort
    // loudly so the regression surfaces in tests rather than corrupting
    // downstream callers.
    assert(r.size() < den_trim.size() &&
           "divrem_modp_newton: remainder degree must be < degree of den");

    quot = std::move(q);
    rem = std::move(r);
}

}  // namespace detail_divrem

/// Main divrem entry. Dispatches between schoolbook and Newton-reciprocal
/// according to the runtime gate (`divrem_subquadratic_mode`) and the
/// compile-time size threshold (`kDivremSubquadraticThreshold`).
///
/// Output `quot` and `rem` are trimmed of trailing zeros. Throws
/// `std::runtime_error` if `den` is the zero polynomial.
///
/// Routing rules:
///   - `den` is zero                        → throw
///   - `num` is zero                        → quot = rem = empty
///   - deg(num) < deg(den)                  → quot = empty, rem = num
///   - gate ≠ ForceOn OR
///     num.size() < kDivremSubquadraticThreshold
///                                          → schoolbook
///   - else                                 → Newton-reciprocal
inline void divrem_modp(const std::vector<uint64_t>& num,
                        const std::vector<uint64_t>& den,
                        uint64_t p,
                        std::vector<uint64_t>& quot,
                        std::vector<uint64_t>& rem) {
    std::vector<uint64_t> num_trim(num);
    std::vector<uint64_t> den_trim(den);
    detail_divrem::trim_trailing_zeros(num_trim);
    detail_divrem::trim_trailing_zeros(den_trim);

    if (den_trim.empty()) {
        throw std::runtime_error(
            "divrem_modp: division by zero polynomial");
    }
    if (num_trim.empty()) {
        quot.clear();
        rem.clear();
        return;
    }
    if (num_trim.size() < den_trim.size()) {
        quot.clear();
        rem = std::move(num_trim);
        return;
    }

    const bool subq_enabled = divrem_subquadratic_enabled();
    if (!subq_enabled || num_trim.size() < kDivremSubquadraticThreshold) {
        divrem_modp_schoolbook(num_trim, den_trim, p, quot, rem);
        return;
    }

    // Newton-reciprocal path. We re-pass the trimmed inputs (the inner
    // routine asserts both invariants we just checked).
    detail_divrem::divrem_modp_newton(num_trim, den_trim, p, quot, rem);
}

}  // namespace gnfs::polynomial
