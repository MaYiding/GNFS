#pragma once

// Polynomial modular squaring helper over F_p[x] (with p < 2^32).
//
// Background:
//   Squaring a polynomial `a` of size n via the schoolbook full-multiply
//   path (`a * a`) costs n^2 coefficient products. The structure of the
//   product `(sum a_i x^i)^2` lets us roughly halve the work: the diagonal
//   contribution at out[2k] is a_k^2 (one square per k), and the
//   off-diagonal contribution at out[i+j] for i != j is 2 * a_i * a_j
//   (one product, doubled, accumulated once because (i, j) and (j, i)
//   yield the same term). The total work is
//       n squares + n*(n-1)/2 off-diagonal products + n*(n-1)/2 doublings,
//   ~n^2/2 mul-adds versus n^2 in the full-mul path.
//
//   Karatsuba squaring works analogously to Karatsuba multiplication but
//   recurses on three sub-squares plus one cross sub-square instead of
//   three full sub-products plus one cross sub-product. Specifically,
//   splitting a = a_low + x^m * a_high gives
//       a^2 = a_low^2 + x^m * 2 * (a_low * a_high) + x^{2m} * a_high^2
//   Substituting the Karatsuba identity for the cross term yields
//       z0 = a_low^2
//       z2 = a_high^2
//       z1 = (a_low + a_high)^2 - z0 - z2
//   so squaring needs three recursive squares (z0, z2, and (low+high)^2)
//   plus one final cross via z1, never invoking full mul on a pair of
//   distinct operands. This matches the W9 Karatsuba mul shape while
//   trading "three half-size mul" for "three half-size square + sub".
//
//   This helper provides `schoolbook_square_mod` (the work-halved
//   schoolbook square reference), `karatsuba_square_mod` (the recursive
//   Karatsuba square kernel), and a dispatching entry `square_mod` that
//   routes between the two according to a runtime-configurable threshold
//   and the three-state ENV gate. Both kernels yield output bit-for-bit
//   identical to `karatsuba_mul_mod(a, a, p, ...)` (W9) and
//   `schoolbook_mul_mod(a, a, p, ...)` golden references.
//
// ENV (GNFS_POLY_SQUARE_OPT):
//   unset / empty / "auto"  → Auto (default; squaring optimisation
//                              enabled — uses the work-halved schoolbook
//                              path below threshold and Karatsuba
//                              squaring above threshold).
//   "0" / "off"             → ForceOff (squaring optimisation disabled;
//                              callers that want this path get a routed
//                              call into the W9 `karatsuba_mul_mod(a, a,
//                              p, out)` full-mul path so the behaviour
//                              is unchanged vs prior helper-free code).
//   "1" / "on"              → ForceOn (squaring optimisation enabled;
//                              functionally equivalent to Auto, kept
//                              distinct so user intent is recorded).
//   Anything else           → Auto.
//
// ENV (GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD):
//   N integer in [4, 4096], default 32. Parsed exactly like the W9
//   GNFS_POLY_KARATSUBA_THRESHOLD env: leading whitespace and non-digit
//   prefixes fall back to default; negative or zero values fall back to
//   default; below 4 clamps to 4; above 4096 clamps to 4096.
//
// Threshold rationale (default 32):
//   Karatsuba squaring has the same per-call overhead as Karatsuba
//   multiplication (recursive sub-buffer allocations + three recursive
//   calls). For tiny inputs the work-halved schoolbook square's tight
//   inner loop wins. Default 32 mirrors the W9 Karatsuba multiplication
//   default so users tuning both helpers do not face two divergent
//   tuning surfaces. Above this threshold the recursion exposes
//   O(n^{log_2 3}) asymptotics same as Karatsuba mul.
//
// Modulus precondition:
//   p prime, p < 2^32 (so uint64_t * uint64_t fits in uint64_t for the
//   schoolbook square's a_i * a_j accumulation and for the inner Karatsuba
//   sums and sub-squares). Caller must ensure all coefficients of `a` are
//   reduced mod p. The helper does not validate primality of p; that is
//   the caller's responsibility.
//
// Bit-for-bit guarantee:
//   For any (a, p) with p prime, p < 2^32, coefficients in [0, p),
//       square_mod(a, p, out)
//   produces an `out` vector bit-for-bit identical (length + per-index
//   coefficient values) to
//       karatsuba_mul_mod(a, a, p, out)
//   and equivalently to
//       schoolbook_mul_mod(a, a, p, out).
//   Both ENV gate values and both threshold paths only change which
//   internal kernel runs, never the mathematical answer. Empty input
//   gives an empty output. Output is canonical with trailing zeros
//   trimmed, matching W9 `schoolbook_mul_mod` / `karatsuba_mul_mod`
//   contract.
//
// Integration status:
//   Future-infrastructure helper. The main `ModularPoly::sqr` /
//   `ModularPoly::mul_raw(a, a)` entries are not wired in. Callers
//   wishing to exploit half-work squaring (e.g. in CRT root-finding
//   power chains, Karatsuba sub-squares of a future `ModularPoly`
//   optimisation, or `ntt_mul_mod` self-convolution short-circuit) call
//   this helper directly. No behavior change for existing callers.

#include "gnfs/polynomial/karatsuba_mul.hpp"

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
#include <string>
#include <vector>

namespace gnfs::polynomial {

/// Three-state dispatch decision for the squaring optimisation helper.
/// Auto and ForceOn both enable the half-work squaring path. ForceOff
/// routes through `karatsuba_mul_mod(a, a, ...)` (the W9 full-mul path),
/// providing a regression-bisect escape hatch that bypasses every
/// squaring-specific code path.
enum class PolySquareMode {
    Auto,
    ForceOff,
    ForceOn,
};

namespace detail_poly_square {

/// Minimum permitted Karatsuba squaring threshold. Mirrors W9 Karatsuba
/// multiplication's minimum so user-tuning surface is consistent.
inline constexpr int kSquareThresholdMin = 4;

/// Maximum permitted Karatsuba squaring threshold. Mirrors W9 Karatsuba
/// multiplication's maximum so user-tuning surface is consistent.
inline constexpr int kSquareThresholdMax = 4096;

/// Default Karatsuba squaring threshold. Mirrors the W9 Karatsuba
/// multiplication default (32) so users tuning both helpers do not face
/// two divergent tuning surfaces.
inline constexpr int kSquareThresholdDefault = 32;

/// Parse GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD. Same accept / clamp /
/// fallback rules as `parse_karatsuba_threshold_env` (W9). Pure of
/// side effects on caller — the cached holder runs this once via
/// `std::call_once`.
[[nodiscard]] inline int parse_square_threshold_env() noexcept {
    const char* env = std::getenv("GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD");
    if (env == nullptr || env[0] == '\0') {
        return kSquareThresholdDefault;
    }
    // Reject leading whitespace / non-digit prefixes outright. The W9
    // helper applies the same strict check via a hand-rolled scan
    // (std::stol would otherwise accept leading spaces / + / partial
    // numeric prefixes).
    const char* p = env;
    if (*p == '-' || *p == '+') {
        ++p;
    }
    if (*p == '\0') {
        return kSquareThresholdDefault;  // bare sign without digits
    }
    for (const char* q = p; *q != '\0'; ++q) {
        if (*q < '0' || *q > '9') {
            return kSquareThresholdDefault;
        }
    }

    long parsed = 0;
    try {
        parsed = std::stol(env);
    } catch (...) {
        return kSquareThresholdDefault;
    }
    if (parsed <= 0) {
        return kSquareThresholdDefault;
    }
    if (parsed < static_cast<long>(kSquareThresholdMin)) {
        return kSquareThresholdMin;
    }
    if (parsed > static_cast<long>(kSquareThresholdMax)) {
        return kSquareThresholdMax;
    }
    return static_cast<int>(parsed);
}

/// Cached singleton for the parsed threshold env. Test-only reset hook
/// re-parses on next access.
struct SquareThresholdCache {
    std::once_flag once;
    std::atomic<int> value{kSquareThresholdDefault};
};

[[nodiscard]] inline SquareThresholdCache& square_threshold_cache() noexcept {
    static SquareThresholdCache cache;
    return cache;
}

/// Cached singleton for the parsed gate env. Test-only reset hook
/// re-parses on next access.
struct SquareGateCache {
    std::once_flag once;
    std::atomic<int> mode{static_cast<int>(PolySquareMode::Auto)};
};

[[nodiscard]] inline SquareGateCache& square_gate_cache() noexcept {
    static SquareGateCache cache;
    return cache;
}

[[nodiscard]] inline PolySquareMode parse_square_gate_env() noexcept {
    const char* env = std::getenv("GNFS_POLY_SQUARE_OPT");
    if (env == nullptr || env[0] == '\0') {
        return PolySquareMode::Auto;
    }
    if (std::strcmp(env, "0") == 0 || std::strcmp(env, "off") == 0) {
        return PolySquareMode::ForceOff;
    }
    if (std::strcmp(env, "1") == 0 || std::strcmp(env, "on") == 0) {
        return PolySquareMode::ForceOn;
    }
    if (std::strcmp(env, "auto") == 0) {
        return PolySquareMode::Auto;
    }
    return PolySquareMode::Auto;
}

/// Add two polynomial spans (a + b) coefficient-wise mod p. Output is
/// the max-length result, zero-extending the shorter input. Identical
/// semantics to W9 `karatsuba_mul.hpp`'s detail::add_mod, kept local so
/// this header is self-contained for the Karatsuba squaring recursion.
inline void add_mod(std::span<const uint64_t> a,
                    std::span<const uint64_t> b,
                    uint64_t p,
                    std::vector<uint64_t>& out) {
    const size_t n = std::max(a.size(), b.size());
    out.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        uint64_t s = 0;
        if (i < a.size()) s = a[i];
        if (i < b.size()) {
            s += b[i];
            if (s >= p) s -= p;
        }
        out[i] = s;
    }
}

/// In-place: dst[i] = (dst[i] - src[i]) mod p for i in [0, src.size()).
/// Identical semantics to W9 `karatsuba_mul.hpp`'s detail::sub_in_place.
inline void sub_in_place(std::vector<uint64_t>& dst,
                         std::span<const uint64_t> src,
                         uint64_t p) {
    for (size_t i = 0; i < src.size(); ++i) {
        if (i >= dst.size()) {
            dst.resize(i + 1, 0);
        }
        uint64_t a = dst[i];
        uint64_t b = src[i];
        if (a >= b) {
            dst[i] = a - b;
        } else {
            dst[i] = p - (b - a);
        }
    }
}

/// In-place: dst[base + i] = (dst[base + i] + src[i]) mod p, *without*
/// growing dst beyond its initial size. Out-of-range positions must
/// receive only zero src[i]; non-zero data past the algebraic degree
/// bound is asserted. Identical semantics to W9 `karatsuba_mul.hpp`'s
/// detail::add_shifted_in_place.
inline void add_shifted_in_place(std::vector<uint64_t>& dst,
                                 std::span<const uint64_t> src,
                                 size_t base,
                                 uint64_t p) {
    for (size_t i = 0; i < src.size(); ++i) {
        const size_t pos = base + i;
        if (pos < dst.size()) {
            uint64_t s = dst[pos] + src[i];
            if (s >= p) s -= p;
            dst[pos] = s;
        } else {
            assert(src[i] == 0 &&
                   "Karatsuba square sub-product exceeded expected degree");
        }
    }
}

/// Trim trailing zero coefficients in place so that the resulting
/// vector represents the canonical polynomial.
inline void trim_trailing_zeros(std::vector<uint64_t>& v) {
    while (!v.empty() && v.back() == 0) {
        v.pop_back();
    }
}

}  // namespace detail_poly_square

/// Returns the active squaring-Karatsuba threshold for the current
/// process.
///
/// First call: parses GNFS_POLY_SQUARE_KARATSUBA_THRESHOLD (see header
/// docs). Subsequent calls: returns cached value. Test-only reset via
/// `poly_square_reset_env_cache_for_testing()`.
[[nodiscard]] inline size_t poly_square_karatsuba_threshold() noexcept {
    auto& cache = detail_poly_square::square_threshold_cache();
    std::call_once(cache.once, []() {
        detail_poly_square::square_threshold_cache().value.store(
            detail_poly_square::parse_square_threshold_env(),
            std::memory_order_relaxed);
    });
    return static_cast<size_t>(
        cache.value.load(std::memory_order_relaxed));
}

/// Returns the current process-wide squaring optimisation gate mode.
///
/// First call: parses GNFS_POLY_SQUARE_OPT (see header docs).
/// Subsequent calls: returns cached value. Test-only reset via
/// `poly_square_reset_env_cache_for_testing()`.
[[nodiscard]] inline PolySquareMode poly_square_mode() noexcept {
    auto& cache = detail_poly_square::square_gate_cache();
    std::call_once(cache.once, []() {
        detail_poly_square::square_gate_cache().mode.store(
            static_cast<int>(detail_poly_square::parse_square_gate_env()),
            std::memory_order_relaxed);
    });
    return static_cast<PolySquareMode>(
        cache.mode.load(std::memory_order_relaxed));
}

/// Convenience predicate. True iff the squaring optimisation kernels
/// (schoolbook half-work + Karatsuba square) are enabled. ForceOff is
/// the only mode that disables the optimisation (and routes through W9
/// `karatsuba_mul_mod(a, a, ...)` instead).
[[nodiscard]] inline bool poly_square_enabled() noexcept {
    return poly_square_mode() != PolySquareMode::ForceOff;
}

/// Test-only: reset both env caches (threshold and gate). After calling
/// this, the next `poly_square_mode()` / `poly_square_karatsuba_threshold()`
/// call will re-read the env. Not thread-safe against concurrent
/// readers and must be called from test setup only.
inline void poly_square_reset_env_cache_for_testing() noexcept {
    {
        auto& cache = detail_poly_square::square_threshold_cache();
        cache.once.~once_flag();
        new (&cache.once) std::once_flag();
        cache.value.store(detail_poly_square::kSquareThresholdDefault,
                          std::memory_order_relaxed);
    }
    {
        auto& cache = detail_poly_square::square_gate_cache();
        cache.once.~once_flag();
        new (&cache.once) std::once_flag();
        cache.mode.store(static_cast<int>(PolySquareMode::Auto),
                         std::memory_order_relaxed);
    }
}

/// Schoolbook squaring mod p, exploiting the (i, j) ↔ (j, i) symmetry to
/// roughly halve the work versus calling schoolbook full-mul on (a, a).
///
/// Output: trimmed to canonical form (trailing zeros removed), matching
/// W9 `schoolbook_mul_mod` contract.
///
/// Precondition: p prime, p < 2^32 (so uint64 * uint64 of two reduced
/// coefficients fits without overflow). Coefficients must satisfy
/// `c < p`.
inline void schoolbook_square_mod(std::span<const uint64_t> a,
                                  uint64_t p,
                                  std::vector<uint64_t>& out) {
    if (a.empty()) {
        out.clear();
        return;
    }
    const size_t n = a.size();
    const size_t result_size = 2 * n - 1;
    out.assign(result_size, 0);

    // Diagonal: out[2k] += a[k]^2 mod p.
    // Off-diagonal (i < j): out[i+j] += 2 * a[i] * a[j] mod p (the (j, i)
    // contribution is the same product, so we add the doubled product
    // exactly once at position i+j).
    for (size_t i = 0; i < n; ++i) {
        const uint64_t ai = a[i];
        if (ai == 0) continue;
        // Diagonal contribution at position 2i: a[i]^2.
        // ai < p < 2^32 so ai * ai < 2^64 fits uint64_t.
        const uint64_t sq = (ai * ai) % p;
        {
            uint64_t s = out[2 * i] + sq;
            if (s >= p) s -= p;
            out[2 * i] = s;
        }
        // Off-diagonal contributions at position i+j for j > i.
        for (size_t j = i + 1; j < n; ++j) {
            const uint64_t aj = a[j];
            if (aj == 0) continue;
            // ai * aj < 2^64 fits uint64_t. Then double mod p.
            const uint64_t prod = (ai * aj) % p;
            // 2 * prod can be up to 2 * (p - 1). p < 2^32 so 2*(p-1) <
            // 2^33 fits uint64_t.
            uint64_t doubled = prod + prod;
            if (doubled >= p) doubled -= p;
            uint64_t s = out[i + j] + doubled;
            if (s >= p) s -= p;
            out[i + j] = s;
        }
    }
    detail_poly_square::trim_trailing_zeros(out);
}

namespace detail_poly_square {

/// Recursive Karatsuba squaring kernel. Splits at the midpoint of the
/// operand. Threshold dispatch is on `a.size()` (single-operand size,
/// not a max as in mul, because both halves come from the same input).
///
/// Sub-products (z0, z2, z1) are trimmed of trailing zeros after each
/// computation so that `add_shifted_in_place` never tries to grow `out`
/// beyond the algebraic degree bound. This matches the W9 mul kernel
/// behaviour: z1 = (a_low + a_high)^2 - z0 - z2 routinely produces a
/// trailing zero by Karatsuba's intentional leading-coefficient
/// cancellation.
inline void karatsuba_square_recursive(std::span<const uint64_t> a,
                                       uint64_t p,
                                       int threshold,
                                       std::vector<uint64_t>& out) {
    if (a.empty()) {
        out.clear();
        return;
    }
    const size_t n = a.size();

    // Base case: input below threshold size → work-halved schoolbook
    // square. We deliberately do NOT untrim the schoolbook output here
    // (downstream `trim_trailing_zeros` in the caller handles it).
    if (static_cast<int>(n) < threshold) {
        schoolbook_square_mod(a, p, out);
        return;
    }

    // Split point: half of the operand, ceiling.
    const size_t m = (n + 1) / 2;

    const auto a_low  = a.subspan(0, std::min(m, n));
    const auto a_high = (n > m) ? a.subspan(m, n - m)
                                : std::span<const uint64_t>{};

    std::vector<uint64_t> z0, z2, z1, sum;

    // z0 = a_low^2
    karatsuba_square_recursive(a_low, p, threshold, z0);
    trim_trailing_zeros(z0);

    // z2 = a_high^2 (may be empty if a_high is empty)
    if (a_high.empty()) {
        z2.clear();
    } else {
        karatsuba_square_recursive(a_high, p, threshold, z2);
        trim_trailing_zeros(z2);
    }

    // z1 = (a_low + a_high)^2 - z0 - z2
    add_mod(a_low, a_high, p, sum);
    trim_trailing_zeros(sum);
    karatsuba_square_recursive(sum, p, threshold, z1);
    sub_in_place(z1, std::span<const uint64_t>(z0.data(), z0.size()), p);
    if (!z2.empty()) {
        sub_in_place(z1, std::span<const uint64_t>(z2.data(), z2.size()), p);
    }
    trim_trailing_zeros(z1);

    // Compose: out = z0 + x^m * z1 + x^{2m} * z2.
    // Result size is 2n - 1 by degree count.
    out.assign(2 * n - 1, 0);
    add_shifted_in_place(out,
                         std::span<const uint64_t>(z0.data(), z0.size()),
                         0, p);
    add_shifted_in_place(out,
                         std::span<const uint64_t>(z1.data(), z1.size()),
                         m, p);
    if (!z2.empty()) {
        add_shifted_in_place(out,
                             std::span<const uint64_t>(z2.data(), z2.size()),
                             2 * m, p);
    }
}

}  // namespace detail_poly_square

/// Karatsuba squaring mod p with threshold-based recursion to the
/// work-halved schoolbook square. Output bit-for-bit identical to
/// `schoolbook_square_mod(a, p, out)` and to W9
/// `karatsuba_mul_mod(a, a, p, out)`.
///
/// Output: trimmed to canonical form (trailing zeros removed). `out` is
/// sized `2 * a.size() - 1` before trimming, or empty if `a` is empty.
///
/// Precondition: p prime, p < 2^32. Coefficients must satisfy `c < p`.
inline void karatsuba_square_mod(std::span<const uint64_t> a,
                                 uint64_t p,
                                 std::vector<uint64_t>& out) {
    int threshold = static_cast<int>(poly_square_karatsuba_threshold());
    // Defensive clamp in case caller bypassed env (should already be
    // bounded by `poly_square_karatsuba_threshold`).
    if (threshold < detail_poly_square::kSquareThresholdMin) {
        threshold = detail_poly_square::kSquareThresholdMin;
    }
    if (threshold > detail_poly_square::kSquareThresholdMax) {
        threshold = detail_poly_square::kSquareThresholdMax;
    }
    detail_poly_square::karatsuba_square_recursive(a, p, threshold, out);
    detail_poly_square::trim_trailing_zeros(out);
}

/// Main squaring entry. Dispatches between schoolbook squaring,
/// Karatsuba squaring, and W9 full-mul `karatsuba_mul_mod(a, a, ...)`
/// according to the runtime gate (`poly_square_mode`) and the runtime
/// threshold (`poly_square_karatsuba_threshold`).
///
/// Output `out` is sized `2 * a.size() - 1` (before trimming) or empty
/// if `a` is empty. Trailing zeros are trimmed to give a canonical
/// polynomial representation matching W9 `karatsuba_mul_mod`.
///
/// Routing rules:
///   - empty input → empty output (short-circuit)
///   - size-1 input → out = { (a[0] * a[0]) mod p }
///   - gate == ForceOff → W9 `karatsuba_mul_mod(a, a, p, out)`
///     (bypass squaring optimisation entirely)
///   - gate == ForceOn or Auto:
///       a.size() <  threshold → `schoolbook_square_mod`
///       a.size() >= threshold → `karatsuba_square_mod`
inline void square_mod(std::span<const uint64_t> a,
                       uint64_t p,
                       std::vector<uint64_t>& out) {
    if (a.empty()) {
        out.clear();
        return;
    }
    if (a.size() == 1) {
        // Size-1 short-circuit: out = { (a[0] * a[0]) mod p }. Common to
        // all gates and threshold values.
        const uint64_t ai = a[0];
        out.assign(1, (ai * ai) % p);
        // Trim in case a[0] == 0 leaves out[0] == 0 (canonical empty).
        detail_poly_square::trim_trailing_zeros(out);
        return;
    }

    const PolySquareMode mode = poly_square_mode();
    if (mode == PolySquareMode::ForceOff) {
        // Route through the W9 full-mul path. We deliberately copy `a`
        // into a vector so that we can pass identical operands to
        // `karatsuba_mul_mod` without aliasing concerns (it expects two
        // input spans and writes to a third output vector).
        const std::vector<uint64_t> a_vec(a.begin(), a.end());
        std::span<const uint64_t> sa(a_vec.data(), a_vec.size());
        karatsuba_mul_mod(sa, sa, p, out);
        return;
    }

    // Auto or ForceOn: route by threshold.
    const size_t threshold = poly_square_karatsuba_threshold();
    if (a.size() < threshold) {
        schoolbook_square_mod(a, p, out);
    } else {
        karatsuba_square_mod(a, p, out);
    }
}

}  // namespace gnfs::polynomial
