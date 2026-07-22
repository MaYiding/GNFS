#pragma once

/// @file two_large_prime.hpp
/// @brief Strict normalization for SIQS two-large-prime cofactors.

#include <gnfs/util/primes.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace gnfs::siqs {

/// Canonical factorization of a two-large-prime cofactor.
struct TwoLargePrimeFactors {
    uint64_t p;
    uint64_t q;

    [[nodiscard]] friend constexpr bool operator==(
        const TwoLargePrimeFactors&,
        const TwoLargePrimeFactors&) = default;
};

/// Validate and canonicalize a candidate split of a SIQS 2LP cofactor.
///
/// The candidate is accepted only when it is an exact factorization into two
/// deterministic uint64 primes, both within the configured large-prime bound.
/// The result is ordered as p <= q. Repeated factors (p^2) are valid.
///
/// Keeping the candidate split explicit makes this function a pure validation
/// boundary. In particular, callers of split_cofactor_64() must pass its output
/// through this function before treating the factors as a usable 2LP relation.
[[nodiscard]] inline std::optional<TwoLargePrimeFactors>
normalize_two_large_prime(uint64_t cofactor,
                          uint64_t large_prime_bound,
                          std::pair<uint64_t, uint64_t> candidate) {
    if (cofactor <= 1 || gnfs::util::is_prime_u64(cofactor)) {
        return std::nullopt;
    }

    auto [p, q] = candidate;
    if (p > q) {
        std::swap(p, q);
    }

    if (p < 2 || q < 2 || p > large_prime_bound || q > large_prime_bound) {
        return std::nullopt;
    }

    // Division establishes p*q == cofactor without overflowing uint64_t.
    if (cofactor % p != 0 || cofactor / p != q) {
        return std::nullopt;
    }

    if (!gnfs::util::is_prime_u64(p) || !gnfs::util::is_prime_u64(q)) {
        return std::nullopt;
    }

    return TwoLargePrimeFactors{p, q};
}

} // namespace gnfs::siqs
