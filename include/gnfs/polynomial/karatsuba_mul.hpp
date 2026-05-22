#pragma once

// Polynomial Karatsuba multiplication over F_p[x] (with p < 2^32).
//
// Background:
//   The classical schoolbook polynomial multiplication of two polynomials
//   of degree n costs O(n^2) coefficient multiplications. Karatsuba's
//   1960 divide-and-conquer trick reduces this to O(n^{log_2 3}) ≈ O(n^1.585)
//   by performing only three half-size sub-multiplications instead of four.
//
//   Splitting a polynomial a of degree (2m - 1) at midpoint m gives
//       a(x) = a_low(x) + x^m * a_high(x)
//   where deg(a_low), deg(a_high) < m. The product a*b expands as
//       a * b = (a_low * b_low)             [z0]
//             + x^m * (a_low * b_high + a_high * b_low)
//             + x^{2m} * (a_high * b_high)  [z2]
//   The cross term is recovered via the classic identity
//       z1 = (a_low + a_high) * (b_low + b_high) - z0 - z2
//   so only three half-size multiplications (z0, z2, and the (low+high)
//   product) are needed.
//
//   This helper provides a `schoolbook_mul_mod` reference (matching
//   `ModularPoly::mul_raw`) and a `karatsuba_mul_mod` recursive entry that
//   dispatches to schoolbook below a runtime-configurable threshold.
//
// ENV:
//   GNFS_POLY_KARATSUBA_THRESHOLD = N (integer in [4, 4096], default 32):
//     Recursion base case. When max(deg a, deg b) < N, fall back to
//     schoolbook. Values <= 0 / non-numeric / empty / unset all yield
//     the default 32. Values above 4096 clamp to 4096.
//
// Threshold rationale (default 32):
//   Karatsuba has significant per-call overhead — allocations for the
//   sums (a_low + a_high), (b_low + b_high), and the intermediate z0 / z1
//   / z2 buffers; plus three recursive calls of half size. For tiny
//   inputs schoolbook's tight inner loop wins. Empirical sweet spot for
//   uint64-coefficient F_p[x] multiplication on modern CPUs is around
//   16-64; we pick 32 as a conservative middle. Below 4 the recursion
//   degenerates (a 3-coeff polynomial splits at midpoint 1, leaving the
//   "high" side with degree 1 and the recursion fails to amortise), so
//   we clamp the minimum to 4.
//
// Modulus precondition:
//   p < 2^32 so that uint64_t * uint64_t never overflows. Intermediate
//   sums of up to (n + 1) products fit in uint64_t when each product is
//   < 2^64; we reduce mod p after each addition to keep the accumulator
//   strictly below p << 2^32, well within range. Callers that need
//   p >= 2^32 must use `ModularPoly::mul_raw` (which routes through
//   `mul_mod` with `__uint128_t`).
//
// Correctness guarantee:
//   For any (a, b, p) with p prime and p < 2^32 and coefficients in
//   [0, p), `karatsuba_mul_mod(a, b, p, out)` writes the same `out`
//   vector as `schoolbook_mul_mod(a, b, p, out)` (bit-for-bit, including
//   leading-zero handling of the result). Empty input on either side
//   yields an empty output. The threshold value does not alter the
//   mathematical result; only the recursion depth changes.
//
// Integration status:
//   This is a future-infrastructure helper. The main `ModularPoly::mul_raw`
//   entry continues to use schoolbook. Callers wishing to experiment
//   with Karatsuba dispatch (e.g. as the M(n) primitive backing Half-GCD
//   `GNFS_POLY_HGCD`) call this helper directly. No behavior change for
//   existing callers.

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

namespace detail {

/// Minimum permitted Karatsuba threshold. Below 4 the recursion fails to
/// produce a meaningful split (a 3-coeff input splits as 2 + 1, and the
/// recursive (low + high) sum has only 2 coeffs which schoolbook would
/// handle just as well).
inline constexpr int kKaratsubaThresholdMin = 4;

/// Maximum permitted Karatsuba threshold. Above 4096 the helper essentially
/// always falls back to schoolbook for realistic GNFS use cases, defeating
/// the purpose; we clamp to keep the value meaningful for testing.
inline constexpr int kKaratsubaThresholdMax = 4096;

/// Default Karatsuba threshold. Empirically a reasonable midpoint for
/// uint64-coefficient polynomial multiplication.
inline constexpr int kKaratsubaThresholdDefault = 32;

/// Parse the GNFS_POLY_KARATSUBA_THRESHOLD env, applying the default and
/// clamp rules described in the helper header docstring. Pure of side
/// effects on caller — `poly_karatsuba_threshold` caches the value.
[[nodiscard]] inline int parse_karatsuba_threshold_env() noexcept {
    const char* env = std::getenv("GNFS_POLY_KARATSUBA_THRESHOLD");
    if (env == nullptr || env[0] == '\0') {
        return kKaratsubaThresholdDefault;
    }
    // Reject non-numeric or leading-whitespace strings outright. strtol
    // is permissive (would accept leading spaces and stop at first
    // non-digit); we require a pure integer literal optionally prefixed
    // by '-' for negativity detection. Anything else → default.
    const char* p = env;
    if (*p == '-' || *p == '+') {
        ++p;
    }
    if (*p == '\0') {
        return kKaratsubaThresholdDefault;  // bare sign with no digits
    }
    for (const char* q = p; *q != '\0'; ++q) {
        if (*q < '0' || *q > '9') {
            return kKaratsubaThresholdDefault;
        }
    }

    long parsed = 0;
    try {
        parsed = std::stol(env);
    } catch (...) {
        return kKaratsubaThresholdDefault;
    }
    if (parsed <= 0) {
        return kKaratsubaThresholdDefault;
    }
    if (parsed < static_cast<long>(kKaratsubaThresholdMin)) {
        return kKaratsubaThresholdMin;
    }
    if (parsed > static_cast<long>(kKaratsubaThresholdMax)) {
        return kKaratsubaThresholdMax;
    }
    return static_cast<int>(parsed);
}

/// Cached, atomic-backed singleton holder for the parsed env. Test-only
/// reset hook re-runs the parse on next access.
struct KaratsubaThresholdCache {
    std::once_flag once;
    std::atomic<int> value{kKaratsubaThresholdDefault};
};

[[nodiscard]] inline KaratsubaThresholdCache& karatsuba_threshold_cache() noexcept {
    static KaratsubaThresholdCache cache;
    return cache;
}

}  // namespace detail

/// Returns the active Karatsuba recursion threshold for the current process.
///
/// First call: parses GNFS_POLY_KARATSUBA_THRESHOLD (see header docs).
/// Subsequent calls: returns cached value. Test-only reset available via
/// `poly_karatsuba_threshold_reset_env_cache_for_testing()`.
[[nodiscard]] inline int poly_karatsuba_threshold() noexcept {
    auto& cache = detail::karatsuba_threshold_cache();
    std::call_once(cache.once, []() {
        detail::karatsuba_threshold_cache().value.store(
            detail::parse_karatsuba_threshold_env(),
            std::memory_order_relaxed);
    });
    return cache.value.load(std::memory_order_relaxed);
}

/// Test-only: reset the env cache. After calling this, the next
/// `poly_karatsuba_threshold()` will re-read the env. Not thread-safe
/// against concurrent readers and must be called from test setup only.
inline void poly_karatsuba_threshold_reset_env_cache_for_testing() noexcept {
    auto& cache = detail::karatsuba_threshold_cache();
    // Construct a fresh once_flag in place to allow re-parsing on next call.
    cache.once.~once_flag();
    new (&cache.once) std::once_flag();
    cache.value.store(detail::kKaratsubaThresholdDefault,
                      std::memory_order_relaxed);
}

/// Schoolbook polynomial multiplication mod p, matching the semantics of
/// `ModularPoly::mul_raw`. Reference for test parity and base case of
/// `karatsuba_mul_mod`.
///
/// Precondition: p prime, p < 2^32 (so uint64_t * uint64_t cannot overflow
/// in the inner product). Coefficients must satisfy `c < p`.
inline void schoolbook_mul_mod(
        std::span<const uint64_t> a,
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
            // ai * bj < (2^32)^2 = 2^64, fits in uint64_t. Reduce then add.
            const uint64_t prod = (ai * bj) % p;
            uint64_t sum = out[i + j] + prod;
            if (sum >= p) sum -= p;
            out[i + j] = sum;
        }
    }
}

namespace detail {

/// Add two polynomial spans (a + b) coefficient-wise mod p. Output is the
/// max-length result, zero-extending the shorter input.
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
inline void sub_in_place(std::vector<uint64_t>& dst,
                         std::span<const uint64_t> src,
                         uint64_t p) {
    for (size_t i = 0; i < src.size(); ++i) {
        if (i >= dst.size()) {
            // Should not happen for valid Karatsuba arithmetic — z1's
            // initial length covers z0 and z2 spans. Defensive resize.
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
/// growing dst beyond its initial size. Any src[i] with base + i >=
/// dst.size() must be zero — otherwise the Karatsuba recursion produced
/// degree information inconsistent with the expected product size, which
/// is a programming error. We assert this defensively so any future
/// algorithmic regression surfaces immediately.
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
            // Out-of-range write would imply src has non-zero coefficient
            // contributing past the algebraic degree bound. With well-formed
            // Karatsuba inputs this happens only for spurious trailing zeros,
            // which we accept silently. Non-zero trailing data is a bug.
            assert(src[i] == 0 && "Karatsuba sub-product exceeded expected degree");
        }
    }
}

/// Trim trailing zero coefficients from `v`. Karatsuba's z1 sub-product
/// frequently has a trailing zero after the (z1 - z0 - z2) cancellation
/// of leading coefficients; trimming keeps the recursion's size
/// invariants aligned with the algebraic degree.
inline void trim_trailing_zeros(std::vector<uint64_t>& v) {
    while (!v.empty() && v.back() == 0) {
        v.pop_back();
    }
}

/// Recursive Karatsuba kernel. Splits at the midpoint of the larger
/// operand. Threshold dispatch happens here on `max(a.size(), b.size())`.
///
/// Sub-product (z0, z1, z2) sizes are trimmed of trailing zeros after
/// each computation so that the recursive composition's `add_shifted_in_place`
/// never tries to grow `out` beyond the algebraic degree bound. This is
/// essential because `z1 = sum_a * sum_b - z0 - z2` regularly produces
/// a trailing zero (the leading coefficients cancel by Karatsuba's
/// design) and propagating that ghost coefficient upward would inflate
/// the result size on each recursion level.
inline void karatsuba_recursive(
        std::span<const uint64_t> a,
        std::span<const uint64_t> b,
        uint64_t p,
        int threshold,
        std::vector<uint64_t>& out) {
    if (a.empty() || b.empty()) {
        out.clear();
        return;
    }
    const size_t na = a.size();
    const size_t nb = b.size();
    const size_t nmax = std::max(na, nb);

    // Base case: either input below threshold size → schoolbook.
    // We test on `nmax` so that very-uneven pairs (e.g., 200 * 5) still
    // route to schoolbook (Karatsuba on a vastly-asymmetric split would
    // waste cross-term work).
    if (static_cast<int>(nmax) < threshold) {
        schoolbook_mul_mod(a, b, p, out);
        return;
    }

    // Split point: half of the larger operand, ceiling.
    const size_t m = (nmax + 1) / 2;

    auto split_low  = [&](std::span<const uint64_t> v) -> std::span<const uint64_t> {
        return v.subspan(0, std::min(m, v.size()));
    };
    auto split_high = [&](std::span<const uint64_t> v) -> std::span<const uint64_t> {
        if (v.size() <= m) return {};  // empty high half
        return v.subspan(m, v.size() - m);
    };

    const auto a_low  = split_low(a);
    const auto a_high = split_high(a);
    const auto b_low  = split_low(b);
    const auto b_high = split_high(b);

    std::vector<uint64_t> z0, z2, z1, sum_a, sum_b;

    // z0 = a_low * b_low.
    karatsuba_recursive(a_low, b_low, p, threshold, z0);
    trim_trailing_zeros(z0);

    // z2 = a_high * b_high (may be empty if both highs are empty).
    if (a_high.empty() || b_high.empty()) {
        z2.clear();
    } else {
        karatsuba_recursive(a_high, b_high, p, threshold, z2);
        trim_trailing_zeros(z2);
    }

    // z1 = (a_low + a_high) * (b_low + b_high) - z0 - z2.
    add_mod(a_low, a_high, p, sum_a);
    add_mod(b_low, b_high, p, sum_b);
    trim_trailing_zeros(sum_a);
    trim_trailing_zeros(sum_b);
    karatsuba_recursive(sum_a, sum_b, p, threshold, z1);

    // Subtract z0 and z2 from z1 in place (mod p), then trim the
    // resulting trailing zeros (these arise from Karatsuba's intentional
    // leading-coefficient cancellation).
    sub_in_place(z1, std::span<const uint64_t>(z0.data(), z0.size()), p);
    if (!z2.empty()) {
        sub_in_place(z1, std::span<const uint64_t>(z2.data(), z2.size()), p);
    }
    trim_trailing_zeros(z1);

    // Compose result: out = z0 + x^m * z1 + x^{2m} * z2.
    out.assign(na + nb - 1, 0);
    add_shifted_in_place(out, std::span<const uint64_t>(z0.data(), z0.size()),
                         0, p);
    add_shifted_in_place(out, std::span<const uint64_t>(z1.data(), z1.size()),
                         m, p);
    if (!z2.empty()) {
        add_shifted_in_place(out, std::span<const uint64_t>(z2.data(), z2.size()),
                             2 * m, p);
    }
}

}  // namespace detail

/// Karatsuba multiplication mod p with threshold-based recursion to
/// schoolbook. Output bit-for-bit identical to `schoolbook_mul_mod` for
/// any matching (a, b, p) with p prime and p < 2^32.
///
/// `out` is resized to `a.size() + b.size() - 1` (or made empty if
/// either input is empty).
inline void karatsuba_mul_mod(
        std::span<const uint64_t> a,
        std::span<const uint64_t> b,
        uint64_t p,
        std::vector<uint64_t>& out) {
    int threshold = poly_karatsuba_threshold();
    // Defensive clamp in case caller bypassed env (should already be
    // bounded by `poly_karatsuba_threshold`, but make the recursive
    // kernel independently safe).
    if (threshold < detail::kKaratsubaThresholdMin) {
        threshold = detail::kKaratsubaThresholdMin;
    }
    if (threshold > detail::kKaratsubaThresholdMax) {
        threshold = detail::kKaratsubaThresholdMax;
    }
    detail::karatsuba_recursive(a, b, p, threshold, out);
}

}  // namespace gnfs::polynomial
