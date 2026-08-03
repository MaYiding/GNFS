#pragma once

/// @file congruence.hpp
/// @brief Small, fail-closed helpers for modular-square congruences.

#include <gnfs/core/integer.hpp>

namespace gnfs::siqs {

/// Return whether x^2 and y^2 have the same canonical residue modulo modulus.
///
/// Inputs may be negative or unreduced.  A modulus smaller than two does not
/// define the congruence boundary used by SIQS and is rejected.
[[nodiscard]] inline bool are_congruent_squares(const core::Integer& x, const core::Integer& y,
                                                const core::Integer& modulus) {
    if (!modulus.is_positive() || modulus.is_one()) {
        return false;
    }

    core::Integer x_modulus;
    core::Integer y_modulus;
    mpz_mod(x_modulus.get_mpz(), x.get_mpz(), modulus.get_mpz());
    mpz_mod(y_modulus.get_mpz(), y.get_mpz(), modulus.get_mpz());

    core::Integer x_square;
    core::Integer y_square;
    mpz_mul(x_square.get_mpz(), x_modulus.get_mpz(), x_modulus.get_mpz());
    mpz_mod(x_square.get_mpz(), x_square.get_mpz(), modulus.get_mpz());
    mpz_mul(y_square.get_mpz(), y_modulus.get_mpz(), y_modulus.get_mpz());
    mpz_mod(y_square.get_mpz(), y_square.get_mpz(), modulus.get_mpz());
    return x_square == y_square;
}

} // namespace gnfs::siqs
