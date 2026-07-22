#pragma once

/// @file two_large_prime_congruence.hpp
/// @brief Exact modular-congruence gate for materialized SIQS 1LP/2LP cycles.

#include <gnfs/siqs/congruence.hpp>
#include <gnfs/siqs/two_large_prime_materializer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using std::size_t;

enum class TwoLargePrimeCongruenceStatus : uint8_t {
    valid,
    invalid_modulus,
    invalid_factor_base,
    invalid_materialized_cycle,
    invalid_dependency,
    exponent_overflow,
    dependency_not_square,
    row_identity_mismatch,
    dependency_mismatch, // Internal invariant breach after valid rows/parity.
};

struct VerifiedTwoLargePrimeXY {
    core::Integer x_modulus;
    core::Integer y_modulus;
};

class TwoLargePrimeDependencyResult {
public:
    [[nodiscard]] static TwoLargePrimeDependencyResult
    failure(TwoLargePrimeCongruenceStatus status) {
        if (status == TwoLargePrimeCongruenceStatus::valid) {
            status = TwoLargePrimeCongruenceStatus::invalid_dependency;
        }
        return TwoLargePrimeDependencyResult(status, std::nullopt);
    }

    [[nodiscard]] static TwoLargePrimeDependencyResult success(VerifiedTwoLargePrimeXY verified) {
        return TwoLargePrimeDependencyResult(TwoLargePrimeCongruenceStatus::valid,
                                             std::move(verified));
    }

    [[nodiscard]] TwoLargePrimeCongruenceStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<VerifiedTwoLargePrimeXY>& verified() const noexcept {
        return verified_;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return status_ == TwoLargePrimeCongruenceStatus::valid && verified_.has_value();
    }

private:
    TwoLargePrimeDependencyResult(TwoLargePrimeCongruenceStatus status,
                                  std::optional<VerifiedTwoLargePrimeXY> verified)
        : status_(status), verified_(std::move(verified)) {}

    TwoLargePrimeCongruenceStatus status_;
    std::optional<VerifiedTwoLargePrimeXY> verified_;
};

namespace two_large_prime_congruence_detail {

[[nodiscard]] inline bool has_valid_modulus(const core::Integer& modulus) {
    return modulus.is_positive() && !modulus.is_one();
}

[[nodiscard]] inline bool has_valid_factor_base(std::span<const uint32_t> factor_base_primes) {
    if (factor_base_primes.empty() || factor_base_primes.front() != 0) {
        return false;
    }
    for (size_t i = 1; i < factor_base_primes.size(); ++i) {
        if (factor_base_primes[i] < 2 ||
            (i > 1 && factor_base_primes[i - 1] >= factor_base_primes[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool has_valid_materialized_shape(const MaterializedTwoLargePrimeCycle& row,
                                                       size_t factor_base_size,
                                                       const core::Integer& modulus) {
    if (row.value_modulus.is_negative() || row.value_modulus >= modulus ||
        row.factor_base_exponents.size() != factor_base_size || row.factor_base_exponents.empty() ||
        row.factor_base_exponents.front() != 0 || row.large_prime_square_roots.empty() ||
        row.relation_indices.empty()) {
        return false;
    }

    for (size_t i = 1; i < row.large_prime_square_roots.size(); ++i) {
        if (row.large_prime_square_roots[i - 1] > row.large_prime_square_roots[i]) {
            return false;
        }
    }
    if (row.large_prime_square_roots.front() < 2) {
        return false;
    }

    for (size_t i = 1; i < row.relation_indices.size(); ++i) {
        if (row.relation_indices[i - 1] >= row.relation_indices[i]) {
            return false;
        }
    }
    return true;
}

inline void multiply_modulus(core::Integer& product, const core::Integer& factor,
                             const core::Integer& modulus) {
    mpz_mul(product.get_mpz(), product.get_mpz(), factor.get_mpz());
    mpz_mod(product.get_mpz(), product.get_mpz(), modulus.get_mpz());
}

[[nodiscard]] inline core::Integer modular_power(uint64_t base, uint64_t exponent,
                                                 const core::Integer& modulus) {
    // Construct through the fixed-width overload: unsigned long is only
    // 32 bits on Windows LLP64 and cannot safely carry either operand.
    return core::powmod(core::Integer(static_cast<uint64_t>(base)),
                        core::Integer(static_cast<uint64_t>(exponent)), modulus);
}

[[nodiscard]] inline TwoLargePrimeDependencyResult failure(TwoLargePrimeCongruenceStatus status) {
    return TwoLargePrimeDependencyResult::failure(status);
}

} // namespace two_large_prime_congruence_detail

/// Validate the exact arithmetic identity represented by one materialized row.
///
/// The row is required to encode
///
///   value_modulus^2 = (-1)^negative * product(fb[i]^exponent[i])
///                     * product(large_prime_square_roots[j]^2) (mod modulus).
///
/// Endpoint primality and configured large-prime bounds belong to the adapter
/// boundary and are deliberately not repeated here.
[[nodiscard]] inline TwoLargePrimeCongruenceStatus
check_materialized_two_large_prime_identity(const MaterializedTwoLargePrimeCycle& row,
                                            std::span<const uint32_t> factor_base_primes,
                                            const core::Integer& modulus) {
    using namespace two_large_prime_congruence_detail;

    if (!has_valid_modulus(modulus)) {
        return TwoLargePrimeCongruenceStatus::invalid_modulus;
    }
    if (!has_valid_factor_base(factor_base_primes)) {
        return TwoLargePrimeCongruenceStatus::invalid_factor_base;
    }
    if (!has_valid_materialized_shape(row, factor_base_primes.size(), modulus)) {
        return TwoLargePrimeCongruenceStatus::invalid_materialized_cycle;
    }

    core::Integer left;
    mpz_mul(left.get_mpz(), row.value_modulus.get_mpz(), row.value_modulus.get_mpz());
    mpz_mod(left.get_mpz(), left.get_mpz(), modulus.get_mpz());

    core::Integer right(1);
    for (size_t i = 1; i < factor_base_primes.size(); ++i) {
        if (row.factor_base_exponents[i] == 0) {
            continue;
        }
        auto factor = modular_power(static_cast<uint64_t>(factor_base_primes[i]),
                                    static_cast<uint64_t>(row.factor_base_exponents[i]), modulus);
        multiply_modulus(right, factor, modulus);
    }
    for (const uint64_t large_prime : row.large_prime_square_roots) {
        auto factor = modular_power(large_prime, uint64_t{2}, modulus);
        multiply_modulus(right, factor, modulus);
    }
    if (row.negative) {
        mpz_neg(right.get_mpz(), right.get_mpz());
        mpz_mod(right.get_mpz(), right.get_mpz(), modulus.get_mpz());
    }

    return left == right ? TwoLargePrimeCongruenceStatus::valid
                         : TwoLargePrimeCongruenceStatus::row_identity_mismatch;
}

/// Verify a selected dependency and return its canonical square pair.
///
/// Dependency row indices may be supplied in any order.  Empty, repeated, or
/// out-of-range selections are rejected.  Every selected row is revalidated
/// before its exponents contribute to the dependency.
[[nodiscard]] inline TwoLargePrimeDependencyResult verify_materialized_two_large_prime_dependency(
    std::span<const MaterializedTwoLargePrimeCycle> rows, std::span<const size_t> dependency,
    std::span<const uint32_t> factor_base_primes, const core::Integer& modulus) {
    using namespace two_large_prime_congruence_detail;

    if (!has_valid_modulus(modulus)) {
        return failure(TwoLargePrimeCongruenceStatus::invalid_modulus);
    }
    if (!has_valid_factor_base(factor_base_primes)) {
        return failure(TwoLargePrimeCongruenceStatus::invalid_factor_base);
    }
    if (dependency.empty()) {
        return failure(TwoLargePrimeCongruenceStatus::invalid_dependency);
    }

    std::vector<size_t> selected(dependency.begin(), dependency.end());
    std::sort(selected.begin(), selected.end());
    if (selected.back() >= rows.size() ||
        std::adjacent_find(selected.begin(), selected.end()) != selected.end()) {
        return failure(TwoLargePrimeCongruenceStatus::invalid_dependency);
    }

    std::vector<uint64_t> exponent_sums(factor_base_primes.size(), uint64_t{0});
    bool negative = false;
    for (const size_t row_index : selected) {
        const auto row_status = check_materialized_two_large_prime_identity(
            rows[row_index], factor_base_primes, modulus);
        if (row_status != TwoLargePrimeCongruenceStatus::valid) {
            return failure(row_status);
        }

        negative = negative != rows[row_index].negative;
        for (size_t i = 0; i < exponent_sums.size(); ++i) {
            const uint64_t addend = rows[row_index].factor_base_exponents[i];
            if (addend > std::numeric_limits<uint64_t>::max() - exponent_sums[i]) {
                return failure(TwoLargePrimeCongruenceStatus::exponent_overflow);
            }
            exponent_sums[i] += addend;
        }
    }

    if (negative || std::any_of(exponent_sums.begin(), exponent_sums.end(),
                                [](uint64_t exponent) { return (exponent & uint64_t{1}) != 0; })) {
        return failure(TwoLargePrimeCongruenceStatus::dependency_not_square);
    }

    core::Integer x_modulus(1);
    core::Integer y_modulus(1);
    for (const size_t row_index : selected) {
        multiply_modulus(x_modulus, rows[row_index].value_modulus, modulus);
        for (const uint64_t large_prime : rows[row_index].large_prime_square_roots) {
            const core::Integer factor(static_cast<uint64_t>(large_prime));
            multiply_modulus(y_modulus, factor, modulus);
        }
    }
    for (size_t i = 1; i < exponent_sums.size(); ++i) {
        const uint64_t half_exponent = exponent_sums[i] / 2;
        if (half_exponent == 0) {
            continue;
        }
        auto factor =
            modular_power(static_cast<uint64_t>(factor_base_primes[i]), half_exponent, modulus);
        multiply_modulus(y_modulus, factor, modulus);
    }

    if (!are_congruent_squares(x_modulus, y_modulus, modulus)) {
        return failure(TwoLargePrimeCongruenceStatus::dependency_mismatch);
    }

    return TwoLargePrimeDependencyResult::success(
        VerifiedTwoLargePrimeXY{std::move(x_modulus), std::move(y_modulus)});
}

} // namespace gnfs::siqs
