#pragma once

#include "../core/integer.hpp"

#include <cassert>
#include <cstdint>
#include <gmp.h>

namespace gnfs::cofactor::brent_suyama {

using core::Integer;

/// Supported Brent-Suyama polynomial degrees.
///
/// Brent 1984 / Suyama 1985: when ECM Stage 2 BSGS uses the polynomial
/// F(x) = x^d in place of the identity F(x) = x, the factor-finding
/// condition
///
///     F(X_giant/Z_giant) - F(X_baby/Z_baby) === 0  (mod p)
///
/// is equivalent (after clearing denominators) to
///
///     X_giant^d * Z_baby^d - X_baby^d * Z_giant^d === 0  (mod p).
///
/// Because F(x) - F(y) = (x - y) * h(x, y), this catches the classical
/// equality (x = y) plus extra coincidences from h(x, y) === 0 mod p.
/// Higher even degrees (d = lcm(1, 2, ..., k)) detect more "twist primes"
/// per Suyama 1985 §3.
///
/// Practical choices in literature (GMP-ECM, Pari/GP, msieve):
///   d = 1  : classical BSGS (identity polynomial)
///   d = 2  : Suyama squaring trick (small uplift)
///   d = 6  : lcm(1, 2, 3) (moderate)
///   d = 12 : lcm(1, 2, 3, 4) (common default)
///   d = 30 : lcm(1, 2, 3, 4, 5) (large-B2 mode)
[[nodiscard]] constexpr bool is_supported_degree(uint32_t d) noexcept {
    return d == 1 || d == 2 || d == 6 || d == 12 || d == 30;
}

/// Default Brent-Suyama degree if `GNFS_ECM_BS_DEGREE` is unset.
constexpr uint32_t DEFAULT_DEGREE = 12;

/// Compute X^degree mod n in-place into `out`.
///
/// Uses GMP `mpz_powm_ui` for fast modular exponentiation
/// (O(log degree) modmuls via binary exponentiation).
///
/// For degree = 1, this is the identity (out = X mod n), which makes the
/// Brent-Suyama path equivalent to classical BSGS (sanity / fallback).
inline void evaluate_polynomial(Integer& out, const Integer& X,
                                uint32_t degree, const Integer& n) {
    assert(is_supported_degree(degree) && "Brent-Suyama: degree must be 1, 2, 6, 12, or 30");
    assert(!n.is_zero() && "Brent-Suyama: modulus n must be non-zero");

    if (degree == 1) {
        // F(x) = x  -- identity; out = X mod n
        mpz_mod(out.get_mpz(), X.get_mpz(), n.get_mpz());
        return;
    }
    // GMP handles edge cases (X = 0, X = 1, X = n) safely.
    mpz_powm_ui(out.get_mpz(), X.get_mpz(), degree, n.get_mpz());
}

/// Precomputed Brent-Suyama "polynomial point": (X^d mod n, Z^d mod n).
///
/// In classical BSGS we store (X_d, Z_d) per baby step. In Brent-Suyama
/// we store (X_d^degree mod n, Z_d^degree mod n) so that the giant-baby
/// cross product is computed against the polynomial-evaluated coordinates.
struct PolynomialPoint {
    Integer x_pow;  // X^degree mod n
    Integer z_pow;  // Z^degree mod n

    PolynomialPoint() = default;
    PolynomialPoint(const Integer& X, const Integer& Z, uint32_t degree, const Integer& n) {
        evaluate_polynomial(x_pow, X, degree, n);
        evaluate_polynomial(z_pow, Z, degree, n);
    }
};

/// Accumulate one Brent-Suyama cross product into `accum`.
///
/// Given the precomputed polynomial point (F_baby_x, F_baby_z) and a freshly
/// computed giant polynomial point (F_giant_x, F_giant_z), compute
///
///     c = F_giant_x * F_baby_z - F_baby_x * F_giant_z  (mod n)
///
/// If `c == 0` the giant and baby polynomial-evaluated points coincide
/// modulo the unknown factor; the caller should compute gcd(F_giant_z, n)
/// to attempt to extract it. Otherwise `accum *= c (mod n)`.
///
/// `tmp1` and `tmp2` are caller-provided scratch Integers (hoisted to avoid
/// per-call mpz_init / mpz_clear overhead in the hot inner loop -- mirrors
/// the v22 buffer-hoist pattern in `ECM::stage2()`).
///
/// Returns:
///   true   if `c == 0` and the caller should run a GCD-on-Z check;
///   false  otherwise (accum was updated and the loop should continue).
[[nodiscard]] inline bool accumulate_cross_product(
        Integer& accum,
        const PolynomialPoint& F_baby,
        const Integer& F_giant_x, const Integer& F_giant_z,
        const Integer& n,
        Integer& tmp1, Integer& tmp2) {
    // tmp1 = F_giant_x * F_baby.z_pow mod n
    mpz_mul(tmp1.get_mpz(), F_giant_x.get_mpz(), F_baby.z_pow.get_mpz());
    mpz_mod(tmp1.get_mpz(), tmp1.get_mpz(), n.get_mpz());

    // tmp2 = F_baby.x_pow * F_giant_z mod n
    mpz_mul(tmp2.get_mpz(), F_baby.x_pow.get_mpz(), F_giant_z.get_mpz());
    mpz_mod(tmp2.get_mpz(), tmp2.get_mpz(), n.get_mpz());

    // tmp1 -= tmp2; if negative, add n. tmp1 stays in [0, n-1].
    mpz_sub(tmp1.get_mpz(), tmp1.get_mpz(), tmp2.get_mpz());
    if (mpz_sgn(tmp1.get_mpz()) < 0) {
        mpz_add(tmp1.get_mpz(), tmp1.get_mpz(), n.get_mpz());
    }

    if (mpz_sgn(tmp1.get_mpz()) == 0) {
        // Polynomial coordinates coincide mod factor; signal GCD check.
        return true;
    }

    // accum *= tmp1 mod n
    mpz_mul(accum.get_mpz(), accum.get_mpz(), tmp1.get_mpz());
    mpz_mod(accum.get_mpz(), accum.get_mpz(), n.get_mpz());

    return false;
}

}  // namespace gnfs::cofactor::brent_suyama
