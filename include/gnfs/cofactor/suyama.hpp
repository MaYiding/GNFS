#pragma once

// Exact arithmetic for Suyama's ECM curve parametrisation.
//
// sigma is an externally supplied uint64_t.  Do not form sigma*sigma or
// 4*sigma in a native integer: both products can wrap before reduction when
// callers use the full uint64_t domain.  Construct the products as GMP
// integers instead so the curve is deterministic on every supported ABI.

#include "../core/integer.hpp"

#include <cstdint>

namespace gnfs::cofactor::detail {

/// Compute u = sigma^2 - 5 (mod n) and v = 4*sigma (mod n).
///
/// The caller must provide a positive modulus n, matching the precondition of
/// the surrounding ECM setup routines.
inline void compute_suyama_uv(core::Integer& u, core::Integer& v, uint64_t sigma,
                              const core::Integer& n) {
    const core::Integer sigma_value(sigma);

    mpz_mul(u.get_mpz(), sigma_value.get_mpz(), sigma_value.get_mpz());
    mpz_sub_ui(u.get_mpz(), u.get_mpz(), 5);
    mpz_mod(u.get_mpz(), u.get_mpz(), n.get_mpz());

    mpz_mul_2exp(v.get_mpz(), sigma_value.get_mpz(), 2);
    mpz_mod(v.get_mpz(), v.get_mpz(), n.get_mpz());
}

} // namespace gnfs::cofactor::detail
