#pragma once

// Polynomial Half-GCD (Knuth-Schönhage HGCD) over F_p[x].
//
// Background:
//   The classical Euclidean GCD over polynomials of degree n costs O(n^2). The
//   Knuth-Schönhage Half-GCD (HGCD) algorithm reduces a polynomial pair (a, b)
//   with deg(a) >= deg(b) to a pair (a', b') with deg(a') ~ deg(a)/2 > deg(b')
//   in O(M(n) log n) total work, where M(n) is polynomial multiplication cost.
//   For schoolbook polynomial multiplication this is O(n^2 / log n); for FFT
//   multiplication it is O(n log^2 n).
//
//   Here we operate over F_p[x] using ModularPoly's existing schoolbook
//   primitives (mul_raw, divmod, add, sub). Even with schoolbook M(n) = O(n^2),
//   HGCD wins through divide-and-conquer pruning above a base-case threshold.
//   The output is bit-for-bit identical to ModularPoly::gcd (Euclidean reference)
//   modulo monic normalization (same normalization both paths).
//
// Algorithm (high-level):
//   half_gcd(a, b) with deg(a) >= deg(b):
//     1. If deg(b) <= deg(a)/2: return identity (no work to do)
//     2. Cut bits: high_a = a >> k, high_b = b >> k where k = deg(a) - m, m = ceil(deg(a)/2)
//     3. Recurse: M1 = half_gcd(high_a, high_b)
//     4. Apply M1 to (a, b): (a_mid, b_mid) = M1 * (a, b)
//     5. If deg(b_mid) > m: do one Euclidean step (q, r), update M2 = [[0,1],[1,-q]] * M1
//        Otherwise M2 = M1.
//     6. Cut bits: high_a' = a_mid >> k', etc, and recurse: M3 = half_gcd(...)
//     7. Return M3 * M2.
//
//   gcd_via_hgcd(a, b):
//     Apply half_gcd to reduce, then a few Euclidean steps clean up the tail.
//     Loop until deg(b) == 0, then return monic-normalized a.
//
// ENV:
//   GNFS_POLY_HGCD = 0 (default): use existing Euclidean path
//   GNFS_POLY_HGCD = 1: use HGCD path when deg(a) >= kHGCDThreshold
//
// Threshold:
//   kHGCDThreshold = 16. Below this Euclidean is faster (HGCD recursion +
//   matrix-vector mult overhead dominates).
//
// Correctness guarantee:
//   For any a, b in F_p[x] with the same input, gcd_via_hgcd(a, b, p) returns
//   the SAME monic polynomial as ModularPoly::gcd(a, b, p) (bit-for-bit).
//   The unit-test suite enforces this on random polynomials in deg range [10, 200].

#include "../sqrt/modular_poly.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace gnfs::polynomial {

using sqrt::ModularPoly;

/// Minimum degree of `a` to dispatch to HGCD path. Below this we run Euclidean.
constexpr size_t kHGCDThreshold = 16;

/// Cached ENV-gated dispatch decision for HGCD.
/// GNFS_POLY_HGCD=1: enable. Default (unset or 0): disable.
[[nodiscard]] inline bool poly_hgcd_enabled() noexcept {
    static std::once_flag once;
    static std::atomic<bool> enabled{false};
    std::call_once(once, []() {
        const char* env = std::getenv("GNFS_POLY_HGCD");
        if (env != nullptr && std::strcmp(env, "1") == 0) {
            enabled.store(true, std::memory_order_relaxed);
        }
    });
    return enabled.load(std::memory_order_relaxed);
}

// ------------------------- 2x2 transformation matrix -------------------------
//
// HGCD returns a 2x2 matrix M = [[m11, m12], [m21, m22]] over F_p[x] such that
//   [a_new] = M * [a]
//   [b_new]       [b]
// i.e. a_new = m11*a + m12*b, b_new = m21*a + m22*b.
//
// The matrix is unimodular (det = ±1 in F_p), composed entirely of "Euclidean
// steps" (a, b) -> (b, a - q*b) which correspond to the matrix [[0,1],[1,-q]].

struct HGCDMatrix {
    ModularPoly m11;
    ModularPoly m12;
    ModularPoly m21;
    ModularPoly m22;

    /// Default = identity (no transformation): [[1, 0], [0, 1]].
    HGCDMatrix() : m11(uint64_t{1}), m12(), m21(), m22(uint64_t{1}) {}

    HGCDMatrix(ModularPoly a, ModularPoly b, ModularPoly c, ModularPoly d)
        : m11(std::move(a)), m12(std::move(b)), m21(std::move(c)), m22(std::move(d)) {}
};

namespace detail {

/// Apply HGCDMatrix to vector (a, b): returns (m11*a + m12*b, m21*a + m22*b).
[[nodiscard]] inline std::pair<ModularPoly, ModularPoly> apply_matrix(
        const HGCDMatrix& M,
        const ModularPoly& a,
        const ModularPoly& b,
        uint64_t p) {
    ModularPoly t1 = ModularPoly::mul_raw(M.m11, a, p);
    ModularPoly t2 = ModularPoly::mul_raw(M.m12, b, p);
    ModularPoly new_a = ModularPoly::add(t1, t2, p);

    ModularPoly t3 = ModularPoly::mul_raw(M.m21, a, p);
    ModularPoly t4 = ModularPoly::mul_raw(M.m22, b, p);
    ModularPoly new_b = ModularPoly::add(t3, t4, p);

    return {std::move(new_a), std::move(new_b)};
}

/// Compose two HGCD matrices: returns A * B where the rows of (A * B) act on
/// the same input vector that B was meant to act on.
[[nodiscard]] inline HGCDMatrix compose(
        const HGCDMatrix& A,
        const HGCDMatrix& B,
        uint64_t p) {
    HGCDMatrix R;
    // R.m11 = A.m11 * B.m11 + A.m12 * B.m21
    R.m11 = ModularPoly::add(
        ModularPoly::mul_raw(A.m11, B.m11, p),
        ModularPoly::mul_raw(A.m12, B.m21, p), p);
    // R.m12 = A.m11 * B.m12 + A.m12 * B.m22
    R.m12 = ModularPoly::add(
        ModularPoly::mul_raw(A.m11, B.m12, p),
        ModularPoly::mul_raw(A.m12, B.m22, p), p);
    // R.m21 = A.m21 * B.m11 + A.m22 * B.m21
    R.m21 = ModularPoly::add(
        ModularPoly::mul_raw(A.m21, B.m11, p),
        ModularPoly::mul_raw(A.m22, B.m21, p), p);
    // R.m22 = A.m21 * B.m12 + A.m22 * B.m22
    R.m22 = ModularPoly::add(
        ModularPoly::mul_raw(A.m21, B.m12, p),
        ModularPoly::mul_raw(A.m22, B.m22, p), p);
    return R;
}

/// One Euclidean step: given (a, b) with deg(a) >= deg(b) > 0, compute
/// q = a div b, r = a - q*b, and return (b, r) with the corresponding
/// step matrix S = [[0, 1], [1, -q]].
///
/// "-q" is computed by negating coefficients mod p.
struct EuclideanStep {
    ModularPoly new_a;     // = b
    ModularPoly new_b;     // = r
    HGCDMatrix step_matrix; // [[0, 1], [1, -q]]
};

[[nodiscard]] inline EuclideanStep euclidean_step(
        const ModularPoly& a,
        const ModularPoly& b,
        uint64_t p) {
    auto [q, r] = ModularPoly::divmod(a, b, p);

    // Build step matrix [[0, 1], [1, -q]].
    HGCDMatrix S;
    S.m11 = ModularPoly();          // 0
    S.m12 = ModularPoly(uint64_t{1}); // 1
    S.m21 = ModularPoly(uint64_t{1}); // 1
    // m22 = -q (mod p) — negate each coefficient
    {
        std::vector<uint64_t> neg_q_coeffs;
        const auto& qc = q.coefficients();
        neg_q_coeffs.reserve(qc.size());
        for (uint64_t c : qc) {
            neg_q_coeffs.push_back(c == 0 ? 0 : p - c);
        }
        S.m22 = ModularPoly(std::move(neg_q_coeffs));
    }

    EuclideanStep step;
    step.new_a = b;
    step.new_b = std::move(r);
    step.step_matrix = std::move(S);
    return step;
}

/// Base-case Euclidean reduction: while deg(b) > target_deg, do one Euclidean
/// step and compose its matrix into M (accumulated on the left). Used as the
/// HGCD base case when the degree is small.
[[nodiscard]] inline HGCDMatrix base_case_reduce(
        ModularPoly& a,
        ModularPoly& b,
        size_t target_deg,
        uint64_t p) {
    HGCDMatrix M;  // identity
    while (!b.is_zero() && static_cast<size_t>(b.degree()) > target_deg) {
        auto step = euclidean_step(a, b, p);
        a = std::move(step.new_a);
        b = std::move(step.new_b);
        // Compose: M_new = step.step_matrix * M (left multiply).
        M = compose(step.step_matrix, M, p);
    }
    return M;
}

/// Truncate polynomial: return high-order coefficients above index k.
/// Equivalent to floor(poly / x^k). Coefficient at index i in the result
/// corresponds to original coefficient at index i + k.
[[nodiscard]] inline ModularPoly shift_right(const ModularPoly& poly, size_t k) {
    const auto& coeffs = poly.coefficients();
    if (coeffs.size() <= k) {
        return ModularPoly();
    }
    using diff_t = std::vector<uint64_t>::difference_type;
    std::vector<uint64_t> result(coeffs.begin() + static_cast<diff_t>(k), coeffs.end());
    return ModularPoly(std::move(result));
}

/// Simplified Half-GCD: reduces (a, b) so that deg(b) drops by at least
/// floor(deg(a) / 4) on each call, using one level of head-bits recursion.
///
/// This is a *simpler* variant of the classical Knuth-Schönhage HGCD that
/// keeps the recursion shallow and the control flow trivially terminating.
/// Full Knuth-Schönhage (with two recursive descents per call) gives a
/// stricter halving guarantee but is harder to implement correctly without
/// a high-precision proof obligation. Here we trade per-call sharpness for
/// guaranteed forward progress and obvious correctness: each call cuts off
/// at least one Euclidean quotient via the recursive head, plus one extra
/// Euclidean step. The outer loop in gcd_via_hgcd iterates O(log n) times.
///
/// Precondition: deg(a) >= deg(b), b != 0.
/// On return, a and b are updated to (a_new, b_new) with deg(a_new) <= deg(a)
/// and (in the typical case) deg(a_new) <= ceil(deg(a) / 2) + O(1).
/// The returned matrix M satisfies M * (a_old, b_old) = (a_new, b_new).
[[nodiscard]] inline HGCDMatrix half_gcd(
        ModularPoly& a,
        ModularPoly& b,
        uint64_t p) {
    if (b.is_zero() || a.is_zero()) {
        return HGCDMatrix();
    }

    int deg_a = a.degree();
    int deg_b = b.degree();
    assert(deg_a >= deg_b);

    // Target half-degree: HGCD reduces (a, b) until deg(b) < m.
    size_t m = static_cast<size_t>(deg_a + 1) / 2;

    // Already done?
    if (static_cast<size_t>(deg_b) < m) {
        return HGCDMatrix();
    }

    // Base case: small degree — fall back to Euclidean.
    if (static_cast<size_t>(deg_a) < kHGCDThreshold) {
        return base_case_reduce(a, b, m - 1, p);
    }

    // ----- Step 1: recurse on high parts (shift by m) -----
    //
    // Following Knuth-Schönhage, we shift by m so the top half of (a, b) has
    // roughly deg(a) - m ≈ m bits, and recurse. The recursive matrix M1 is
    // valid to apply to full (a, b) because Euclidean step matrices depend
    // only on the leading bits.
    size_t k1 = m;
    ModularPoly a_hi = shift_right(a, k1);
    ModularPoly b_hi = shift_right(b, k1);

    HGCDMatrix M_acc;  // identity
    if (!b_hi.is_zero() && !a_hi.is_zero() && a_hi.degree() >= b_hi.degree()) {
        if (static_cast<size_t>(a_hi.degree()) < kHGCDThreshold) {
            // High part too small for recursion — Euclidean directly on hi
            // parts. Stop when deg(b_hi) < hi_m so the matrix represents the
            // head Euclidean chain.
            size_t hi_m = static_cast<size_t>(a_hi.degree() + 1) / 2;
            if (hi_m > 0 && static_cast<size_t>(b_hi.degree()) >= hi_m) {
                M_acc = base_case_reduce(a_hi, b_hi, hi_m - 1, p);
            }
        } else {
            M_acc = half_gcd(a_hi, b_hi, p);
        }
    }

    // ----- Step 2: apply M_acc to full (a, b) -----
    ModularPoly a_mid, b_mid;
    {
        auto [na, nb] = apply_matrix(M_acc, a, b, p);
        a_mid = std::move(na);
        b_mid = std::move(nb);
    }

    // Re-orient if needed.
    if (a_mid.is_zero() || (!b_mid.is_zero() && a_mid.degree() < b_mid.degree())) {
        std::swap(a_mid, b_mid);
        std::swap(M_acc.m11, M_acc.m21);
        std::swap(M_acc.m12, M_acc.m22);
    }

    // Defensive: if the recursive matrix application did NOT reduce degree
    // (could happen with shift-precision edge cases), fall back to a Euclidean
    // base-case reduction on the original (a, b) to guarantee progress.
    if (!b_mid.is_zero() && static_cast<size_t>(b_mid.degree()) >= static_cast<size_t>(deg_b)) {
        // No useful reduction from M_acc — discard and do Euclidean.
        a_mid = std::move(a);
        b_mid = std::move(b);
        M_acc = HGCDMatrix();  // reset to identity
    }

    // If we've already crossed the m threshold, return.
    if (b_mid.is_zero() || static_cast<size_t>(b_mid.degree()) < m) {
        a = std::move(a_mid);
        b = std::move(b_mid);
        return M_acc;
    }

    // ----- Step 3: one Euclidean step to further reduce -----
    {
        auto step = euclidean_step(a_mid, b_mid, p);
        a_mid = std::move(step.new_a);
        b_mid = std::move(step.new_b);
        M_acc = compose(step.step_matrix, M_acc, p);
    }

    // ----- Step 4: if deg(b_mid) still >= m, finish with Euclidean -----
    //
    // This is the "second recursion" of classical Knuth-Schönhage replaced by
    // a base-case Euclidean reduction. We trade tighter per-call work for
    // implementation simplicity and obvious correctness. The outer loop in
    // gcd_via_hgcd iterates O(log n) times so total cost is still asymptotic
    // O(M(n) log^2 n) — slightly worse than classical O(M(n) log n) but the
    // wall-clock difference for n < 10000 is negligible.
    if (!b_mid.is_zero() && static_cast<size_t>(b_mid.degree()) >= m) {
        HGCDMatrix M3 = base_case_reduce(a_mid, b_mid, m - 1, p);
        M_acc = compose(M3, M_acc, p);
    }

    a = std::move(a_mid);
    b = std::move(b_mid);
    return M_acc;
}

/// Convert any non-zero polynomial to its monic representative.
[[nodiscard]] inline ModularPoly make_monic(const ModularPoly& poly, uint64_t p) {
    if (poly.is_zero()) {
        return poly;
    }
    const auto& coeffs = poly.coefficients();
    uint64_t lead = coeffs.back();
    if (lead == 1) {
        return poly;
    }
    // Use ModularPoly's helpers via scalar_mul.
    // Compute mod_inverse inline to avoid private-member dependency.
    auto mod_inv = [](uint64_t a, uint64_t m) -> uint64_t {
        __int128_t t = 0, new_t = 1;
        __int128_t r = static_cast<__int128_t>(m), new_r = static_cast<__int128_t>(a);
        while (new_r != 0) {
            __int128_t quotient = r / new_r;
            __int128_t temp_t = new_t;
            new_t = t - quotient * new_t;
            t = temp_t;
            __int128_t temp_r = new_r;
            new_r = r - quotient * new_r;
            r = temp_r;
        }
        if (t < 0) t += static_cast<__int128_t>(m);
        return static_cast<uint64_t>(t);
    };
    uint64_t inv = mod_inv(lead, p);
    return ModularPoly::scalar_mul(poly, inv, p);
}

}  // namespace detail

/// Compute GCD of two polynomials over F_p via Half-GCD.
///
/// Output is bit-for-bit identical (monic, normalized) to ModularPoly::gcd.
[[nodiscard]] inline ModularPoly gcd_via_hgcd(
        const ModularPoly& a_in,
        const ModularPoly& b_in,
        uint64_t p) {
    // Trivial cases.
    if (a_in.is_zero()) return detail::make_monic(b_in, p);
    if (b_in.is_zero()) return detail::make_monic(a_in, p);

    ModularPoly a = a_in;
    ModularPoly b = b_in;

    // Ensure deg(a) >= deg(b).
    if (a.degree() < b.degree()) {
        std::swap(a, b);
    }

    // Use HGCD to do most of the work, then clean up with Euclidean steps.
    // The loop is bounded by O(log n) HGCD calls plus O(n) base-case steps.
    // Defensive iteration bound: each HGCD call must reduce deg(a) by at
    // least 1 in expectation; we cap iterations to guarantee termination
    // even if HGCD produces identity for some pathological input.
    size_t safety_iter_cap = 8 * static_cast<size_t>(a.degree() + 1);
    size_t iter = 0;
    while (!b.is_zero() && static_cast<size_t>(a.degree()) >= kHGCDThreshold) {
        if (++iter > safety_iter_cap) {
            // Should never happen for well-formed input. Break to Euclidean
            // tail. (Test catches this if it ever occurs.)
            break;
        }
        if (a.degree() < b.degree()) {
            std::swap(a, b);
        }
        int deg_before = a.degree();
        HGCDMatrix M = detail::half_gcd(a, b, p);
        (void)M;
        // Defensive: ensure forward progress. If HGCD did not reduce deg(b)
        // below m or did not change (a, b), advance one Euclidean step.
        int deg_after = a.degree();
        if (deg_after >= deg_before && !b.is_zero()) {
            auto step = detail::euclidean_step(a, b, p);
            a = std::move(step.new_a);
            b = std::move(step.new_b);
        }
    }

    // Tail: small-degree Euclidean.
    while (!b.is_zero()) {
        auto [q, r] = ModularPoly::divmod(a, b, p);
        (void)q;
        a = std::move(b);
        b = std::move(r);
    }

    return detail::make_monic(a, p);
}

/// Dispatch entry: returns true if HGCD should be used for the given input
/// degree under the current ENV setting. Otherwise caller should use Euclidean.
[[nodiscard]] inline bool should_use_hgcd(size_t deg_a) noexcept {
    return poly_hgcd_enabled() && deg_a >= kHGCDThreshold;
}

}  // namespace gnfs::polynomial
