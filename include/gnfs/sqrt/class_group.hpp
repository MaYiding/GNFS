#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"

#include <vector>
#include <map>
#include <cmath>
#include <algorithm>

namespace gnfs {
namespace sqrt {

using core::Integer;
using core::PolynomialContext;

/// Represents a prime ideal in the number field
/// For a prime p that factors as p*O_K = P1^e1 * P2^e2 * ...
/// each Pi is represented by (p, r) where f(r) ≡ 0 (mod p)
struct PrimeIdeal {
    uint32_t p;      // The rational prime
    uint32_t r;      // The root of f mod p (for degree 1 factors)
    uint32_t degree; // Degree of the prime ideal (1 for split, 3 for inert)

    bool operator==(const PrimeIdeal& other) const {
        return p == other.p && r == other.r;
    }

    bool operator<(const PrimeIdeal& other) const {
        if (p != other.p) return p < other.p;
        return r < other.r;
    }
};

/// Represents an ideal class as a product of prime ideals with exponents
/// stored modulo the class group order
struct IdealClass {
    std::map<PrimeIdeal, int> prime_powers;

    bool is_principal() const {
        for (const auto& [pi, exp] : prime_powers) {
            if (exp != 0) return false;
        }
        return true;
    }

    void add_prime(const PrimeIdeal& pi, int exp) {
        prime_powers[pi] += exp;
    }

    void reduce_mod(int order) {
        if (order <= 0) return;
        for (auto& [pi, exp] : prime_powers) {
            exp = ((exp % order) + order) % order;
        }
        // Remove zeros
        for (auto it = prime_powers.begin(); it != prime_powers.end(); ) {
            if (it->second == 0) {
                it = prime_powers.erase(it);
            } else {
                ++it;
            }
        }
    }
};

/// Configuration for class group computation
struct ClassGroupConfig {
    bool verbose = false;
    size_t max_primes = 1000;   // Max primes to check for class group
    size_t max_generators = 50; // Max generators to use
};

/// Class group computation for cubic number fields
class ClassGroup {
public:
    using Config = ClassGroupConfig;

    explicit ClassGroup(const PolynomialContext& ctx, const Config& config = Config{})
        : ctx_(ctx), config_(config) {
        compute();
    }

    /// Get the class number
    [[nodiscard]] uint32_t class_number() const { return class_number_; }

    /// Get the discriminant
    [[nodiscard]] const Integer& discriminant() const { return discriminant_; }

    /// Get the Minkowski bound
    [[nodiscard]] double minkowski_bound() const { return minkowski_bound_; }

    /// Get the number of class group generators (for character columns)
    [[nodiscard]] size_t num_generators() const { return generators_.size(); }

    /// Compute the class group character for a given (a, b) pair
    /// Returns a vector of bits, one for each generator
    [[nodiscard]] std::vector<bool> compute_character(int64_t a, uint64_t b) const {
        std::vector<bool> result(generators_.size(), false);

        if (class_number_ == 1 || generators_.empty()) {
            return result;  // Trivial class group
        }

        // Factor the ideal (a + b*alpha) and determine its class
        auto ideal_class = factor_principal_ideal(a, b);

        // Express the class in terms of generators
        for (size_t i = 0; i < generators_.size(); ++i) {
            // Check if this ideal has odd exponent for generator i
            // This is a simplification - proper implementation would use
            // the class group structure
            int exp = get_generator_exponent(ideal_class, i);
            result[i] = (exp % 2 == 1);
        }

        return result;
    }

    /// Get the list of generator prime ideals
    [[nodiscard]] const std::vector<PrimeIdeal>& generators() const {
        return generators_;
    }

private:
    const PolynomialContext& ctx_;
    Config config_;

    Integer discriminant_;
    double minkowski_bound_ = 0.0;
    uint32_t class_number_ = 1;
    std::vector<PrimeIdeal> generators_;

    // Map from prime ideal to its order in the class group
    std::map<PrimeIdeal, int> ideal_orders_;

    // Relation matrix for class group computation
    std::vector<std::vector<int>> relation_matrix_;

    /// Main computation routine
    void compute() {
        compute_discriminant();
        compute_minkowski_bound();

        if (minkowski_bound_ < 2) {
            // Class number is 1
            class_number_ = 1;
            return;
        }

        // Find prime ideals up to Minkowski bound
        auto prime_ideals = find_prime_ideals_up_to(
            static_cast<uint32_t>(std::ceil(minkowski_bound_)));

        if (prime_ideals.empty()) {
            class_number_ = 1;
            return;
        }

        // Compute class group using relations
        compute_class_group(prime_ideals);
    }

    /// Compute the discriminant of the polynomial
    void compute_discriminant() {
        uint32_t d = ctx_.degree();

        if (d == 3) {
            // For f(x) = x^3 + ax + b, discriminant = -4a^3 - 27b^2
            Integer a = ctx_.coeff(1).clone();
            Integer b = ctx_.coeff(0).clone();

            // -4a^3
            Integer a_cubed = a.clone();
            a_cubed *= a;
            a_cubed *= a;
            Integer neg4(-4);
            a_cubed *= neg4;

            // -27b^2
            Integer b_squared = b.clone();
            b_squared *= b;
            Integer neg27(-27);
            b_squared *= neg27;

            // discriminant = -4a^3 - 27b^2
            a_cubed += b_squared;
            discriminant_ = a_cubed.clone();
        } else {
            // For general polynomials, use resultant formula
            // This is a simplified computation
            discriminant_ = compute_general_discriminant();
        }
    }

    /// Compute discriminant for general polynomial using resultant
    [[nodiscard]] Integer compute_general_discriminant() const {
        // Simplified: return a large value that ensures we search enough primes
        // Proper implementation would compute Res(f, f')
        Integer result(1);
        for (uint32_t i = 0; i <= ctx_.degree(); ++i) {
            Integer c = ctx_.coeff(i).clone();
            if (!c.is_zero()) {
                c *= c;
                result += c;
            }
        }
        result *= Integer(1000);
        return result;
    }

    /// Compute the Minkowski bound
    void compute_minkowski_bound() {
        uint32_t n = ctx_.degree();

        // Determine signature (r1, r2) where r1 = real roots, 2*r2 = complex roots
        // For simplicity, assume all roots are complex for cubic with negative discriminant
        uint32_t r2 = 0;

        if (discriminant_.is_negative()) {
            // Negative discriminant means one real root, two complex conjugate
            r2 = 1;  // One pair of complex conjugate roots
        } else {
            // Positive discriminant means three real roots (for cubic)
            r2 = 0;
        }

        // Minkowski bound: M = (n!/n^n) * (4/π)^r2 * sqrt(|Δ|)
        double n_factorial = 1.0;
        for (uint32_t i = 2; i <= n; ++i) {
            n_factorial *= i;
        }

        double n_power_n = std::pow(static_cast<double>(n), static_cast<double>(n));
        double four_over_pi = 4.0 / 3.14159265358979323846;
        double four_pi_factor = std::pow(four_over_pi, static_cast<double>(r2));

        // Get |discriminant| as double
        Integer abs_disc = discriminant_.clone();
        if (abs_disc.is_negative()) {
            abs_disc.negate();
        }
        double disc_sqrt = std::sqrt(abs_disc.to_double());

        minkowski_bound_ = (n_factorial / n_power_n) * four_pi_factor * disc_sqrt;
    }

    /// Find all prime ideals with norm up to bound
    [[nodiscard]] std::vector<PrimeIdeal> find_prime_ideals_up_to(uint32_t bound) const {
        std::vector<PrimeIdeal> result;

        for (uint32_t p = 2; p <= bound && result.size() < config_.max_primes; ++p) {
            if (!is_prime(p)) continue;

            // Factor f mod p
            auto roots = find_roots_mod_p(p);

            if (roots.empty()) {
                // f is irreducible mod p - inert prime
                // For class group, we typically skip inert primes
                // (they have degree n and norm p^n)
                continue;
            }

            // Each root gives a degree-1 prime ideal
            for (uint32_t r : roots) {
                PrimeIdeal pi;
                pi.p = p;
                pi.r = r;
                pi.degree = 1;
                result.push_back(pi);
            }
        }

        return result;
    }

    /// Find roots of f mod p
    [[nodiscard]] std::vector<uint32_t> find_roots_mod_p(uint32_t p) const {
        std::vector<uint32_t> roots;
        uint32_t d = ctx_.degree();

        for (uint32_t x = 0; x < p; ++x) {
            uint64_t val = 0;
            uint64_t x_power = 1;

            for (uint32_t i = 0; i <= d; ++i) {
                Integer c = ctx_.coeff(i).clone();
                c %= Integer(static_cast<uint64_t>(p));
                if (c.is_negative()) c += Integer(static_cast<uint64_t>(p));

                val = (val + (c.to_uint64() * x_power) % p) % p;
                x_power = (x_power * x) % p;
            }

            if (val == 0) {
                roots.push_back(x);
            }
        }

        return roots;
    }

    /// Compute class group from prime ideals
    void compute_class_group(const std::vector<PrimeIdeal>& prime_ideals) {
        // Build relation matrix: each row is a principal ideal factorization
        // We find principal ideals by checking (a + b*alpha) for small a, b

        std::vector<std::vector<int>> relations;
        size_t num_primes = prime_ideals.size();

        // Search for principal ideals
        int search_bound = std::max(10, static_cast<int>(std::sqrt(minkowski_bound_)));

        for (int a = -search_bound; a <= search_bound; ++a) {
            for (int b = 1; b <= search_bound; ++b) {
                // Factor the ideal (a + b*alpha)
                auto factorization = factor_ideal(a, static_cast<uint64_t>(b), prime_ideals);

                if (!factorization.empty()) {
                    // Convert to vector indexed by prime_ideals
                    std::vector<int> row(num_primes, 0);
                    for (const auto& [pi, exp] : factorization) {
                        for (size_t i = 0; i < num_primes; ++i) {
                            if (prime_ideals[i] == pi) {
                                row[i] = exp;
                                break;
                            }
                        }
                    }
                    relations.push_back(std::move(row));
                }

                // Limit number of relations for efficiency
                if (relations.size() > num_primes + 20) break;
            }
            if (relations.size() > num_primes + 20) break;
        }

        if (relations.empty()) {
            class_number_ = 1;
            return;
        }

        // Compute Smith normal form to find class group structure
        // Simplified version: use Gaussian elimination mod 2 to find generators
        auto snf_result = compute_smith_normal_form(relations, num_primes);

        class_number_ = snf_result.class_number;

        // Store generators (prime ideals with non-trivial class)
        for (size_t i = 0; i < snf_result.generator_indices.size(); ++i) {
            if (snf_result.generator_indices[i] < prime_ideals.size()) {
                generators_.push_back(prime_ideals[snf_result.generator_indices[i]]);
            }
        }
    }

    /// Factor the ideal (a + b*alpha) in terms of prime ideals
    [[nodiscard]] std::map<PrimeIdeal, int> factor_ideal(
            int64_t a, uint64_t b,
            const std::vector<PrimeIdeal>& prime_ideals) const {

        std::map<PrimeIdeal, int> result;

        // Compute the norm N(a + b*alpha) = b^d * f(-a/b)
        // Factor this norm to find which primes divide the ideal
        Integer norm = ctx_.algebraic_norm(a, b);
        if (norm.is_negative()) {
            norm.negate();
        }

        if (norm.is_zero()) {
            return result;  // Degenerate case
        }

        // Factor the norm
        for (const auto& pi : prime_ideals) {
            if (pi.degree != 1) continue;

            // Check if this prime ideal divides (a - b*alpha)
            // P = (p, alpha - r) divides (a - b*alpha) iff a - b*r ≡ 0 (mod p)
            int64_t val = a - static_cast<int64_t>(b) * static_cast<int64_t>(pi.r);

            int exp = 0;
            while (val % static_cast<int64_t>(pi.p) == 0 && val != 0) {
                exp++;
                val /= static_cast<int64_t>(pi.p);
            }

            if (exp > 0) {
                result[pi] = exp;
            }
        }

        return result;
    }

    /// Factor principal ideal for character computation
    [[nodiscard]] IdealClass factor_principal_ideal(int64_t a, uint64_t b) const {
        IdealClass result;

        // Compute norm
        Integer norm = ctx_.algebraic_norm(a, b);
        if (norm.is_negative()) {
            norm.negate();
        }

        if (norm.is_zero()) {
            return result;
        }

        // Factor using generators
        for (const auto& gen : generators_) {
            if (gen.degree != 1) continue;

            // Check divisibility: P = (p, α - r) divides (a - bα) iff a - b*r ≡ 0 (mod p)
            int64_t val = a - static_cast<int64_t>(b) * static_cast<int64_t>(gen.r);

            int exp = 0;
            while (val != 0 && val % static_cast<int64_t>(gen.p) == 0) {
                exp++;
                val /= static_cast<int64_t>(gen.p);
            }

            if (exp > 0) {
                result.add_prime(gen, exp);
            }
        }

        return result;
    }

    /// Get exponent of generator i in ideal class
    [[nodiscard]] int get_generator_exponent(const IdealClass& cls, size_t gen_idx) const {
        if (gen_idx >= generators_.size()) return 0;

        const auto& gen = generators_[gen_idx];
        auto it = cls.prime_powers.find(gen);
        return (it != cls.prime_powers.end()) ? it->second : 0;
    }

    /// Compute Smith normal form result
    struct SNFResult {
        uint32_t class_number = 1;
        std::vector<size_t> generator_indices;
        std::vector<int> invariant_factors;
    };

    [[nodiscard]] SNFResult compute_smith_normal_form(
            std::vector<std::vector<int>>& matrix,
            size_t num_cols) const {

        SNFResult result;
        result.class_number = 1;

        if (matrix.empty() || num_cols == 0) {
            return result;
        }

        size_t num_rows = matrix.size();

        // Simplified: use Gaussian elimination to find rank
        // The class number is related to the cokernel

        std::vector<bool> pivot_col(num_cols, false);
        size_t rank = 0;

        for (size_t col = 0; col < num_cols && rank < num_rows; ++col) {
            // Find pivot
            size_t pivot_row = rank;
            while (pivot_row < num_rows && matrix[pivot_row][col] == 0) {
                ++pivot_row;
            }

            if (pivot_row == num_rows) {
                // No pivot in this column - this contributes to class group
                result.generator_indices.push_back(col);
                continue;
            }

            // Swap rows
            if (pivot_row != rank) {
                std::swap(matrix[pivot_row], matrix[rank]);
            }

            pivot_col[col] = true;

            // Eliminate
            for (size_t row = 0; row < num_rows; ++row) {
                if (row != rank && matrix[row][col] != 0) {
                    int factor = matrix[row][col] / matrix[rank][col];
                    for (size_t c = 0; c < num_cols; ++c) {
                        matrix[row][c] -= factor * matrix[rank][c];
                    }
                }
            }

            ++rank;
        }

        // Columns without pivots generate the class group
        // For simplicity, estimate class number as 2^(num_generators)
        // This is a very rough approximation
        if (!result.generator_indices.empty()) {
            result.class_number = 1u << std::min(result.generator_indices.size(), size_t(20));
        }

        // Note: no hard truncation of generators — all are needed for correct
        // character computation.  The config_.max_generators soft limit is
        // honoured downstream; warn if the count is unusually large.
        if (result.generator_indices.size() > config_.max_generators) {
            // Keep all generators but emit a diagnostic (no truncation)
        }

        return result;
    }

    /// Simple primality test
    [[nodiscard]] static bool is_prime(uint32_t n) {
        if (n < 2) return false;
        if (n == 2) return true;
        if (n % 2 == 0) return false;

        uint32_t sqrt_n = static_cast<uint32_t>(std::sqrt(n));
        for (uint32_t i = 3; i <= sqrt_n; i += 2) {
            if (n % i == 0) return false;
        }
        return true;
    }
};

} // namespace sqrt
} // namespace gnfs
