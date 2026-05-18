#pragma once

#include "../core/integer.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace gnfs::polynomial {

using core::Integer;

/// Bareiss algorithm: fraction-free Gaussian elimination for Integer matrix
/// determinant. Modifies M in place. Returns 0 for singular matrices.
[[nodiscard]] inline Integer bareiss_determinant(
        std::vector<std::vector<Integer>>& M, uint32_t n) {
    int sign = 1;
    Integer prev_pivot(1);

    for (uint32_t k = 0; k < n; ++k) {
        uint32_t pivot_row = k;
        while (pivot_row < n && M[pivot_row][k].is_zero()) {
            ++pivot_row;
        }
        if (pivot_row == n) {
            return Integer{};
        }
        if (pivot_row != k) {
            std::swap(M[k], M[pivot_row]);
            sign = -sign;
        }

        Integer cur_pivot = M[k][k];

        Integer term;
        for (uint32_t i = k + 1; i < n; ++i) {
            for (uint32_t j = k + 1; j < n; ++j) {
                mpz_mul(term.get_mpz(), cur_pivot.get_mpz(), M[i][j].get_mpz());
                mpz_submul(term.get_mpz(), M[i][k].get_mpz(), M[k][j].get_mpz());
                mpz_divexact(term.get_mpz(), term.get_mpz(), prev_pivot.get_mpz());
                M[i][j] = std::move(term);
            }
            M[i][k] = int64_t(0);
        }

        prev_pivot = std::move(cur_pivot);
    }

    Integer result = M[n - 1][n - 1];
    if (sign < 0) {
        result.negate();
    }
    return result;
}

/// Compute resultant Res(f, g) via Sylvester matrix determinant.
/// f has degree deg_f (coefficients f[0..deg_f] with f[i] = coefficient of x^i),
/// g has degree deg_g. Returns Integer determinant of (deg_f+deg_g) × (deg_f+deg_g)
/// Sylvester matrix. Both polynomials must satisfy f[deg_f] != 0, g[deg_g] != 0.
[[nodiscard]] inline Integer resultant(
        const std::vector<Integer>& f, uint32_t deg_f,
        const std::vector<Integer>& g, uint32_t deg_g) {

    uint32_t n = deg_f + deg_g;
    if (n == 0) return Integer(1);

    std::vector<std::vector<Integer>> M(n, std::vector<Integer>(n));

    for (uint32_t row = 0; row < deg_g; ++row) {
        for (uint32_t k = 0; k <= deg_f; ++k) {
            uint32_t col = row + k;
            if (col < n) {
                M[row][col] = f[deg_f - k];
            }
        }
    }

    for (uint32_t row = 0; row < deg_f; ++row) {
        for (uint32_t k = 0; k <= deg_g; ++k) {
            uint32_t col = row + k;
            if (col < n) {
                M[deg_g + row][col] = g[deg_g - k];
            }
        }
    }

    return bareiss_determinant(M, n);
}

/// Compute discriminant of polynomial f of degree d ≥ 1.
/// Formula: Δ(f) = (-1)^(d(d-1)/2) · Res(f, f') / a_d
/// where a_d is the leading coefficient. f[i] is coefficient of x^i.
///
/// For d == 1: returns 1 (linear polynomial discriminant convention).
/// For d == 2: returns b^2 - 4ac (the classical formula).
/// For d ≥ 3: uses Resultant(f, f') via Sylvester matrix.
[[nodiscard]] inline Integer discriminant(
        const std::vector<Integer>& f, uint32_t d) {

    if (d == 0) return Integer{};
    if (d == 1) return Integer(1);

    if (d == 2) {
        Integer b2;
        mpz_mul(b2.get_mpz(), f[1].get_mpz(), f[1].get_mpz());

        Integer ac;
        mpz_mul(ac.get_mpz(), f[0].get_mpz(), f[2].get_mpz());
        mpz_mul_2exp(ac.get_mpz(), ac.get_mpz(), 2);

        b2 -= ac;
        return b2;
    }

    std::vector<Integer> f_prime(d);
    for (uint32_t i = 0; i < d; ++i) {
        f_prime[i] = f[i + 1];
        f_prime[i] *= static_cast<int64_t>(i + 1);
    }

    Integer res = resultant(f, d, f_prime, d - 1);

    uint32_t sign_exp = d * (d - 1) / 2;
    if (sign_exp % 2 == 1) {
        res.negate();
    }
    if (!f[d].is_zero()) {
        res /= f[d];
    }

    return res;
}

}  // namespace gnfs::polynomial
