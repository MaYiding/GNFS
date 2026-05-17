#pragma once

#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
#include "../util/primes.hpp"

#include <vector>
#include <map>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace gnfs::sqrt {

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

/// Class group computation for number fields of any degree
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

    /// Compute the discriminant of the polynomial using Res(f, f')
    /// Δ(f) = (-1)^(d(d-1)/2) · Res(f, f') / a_d
    void compute_discriminant() {
        uint32_t d = ctx_.degree();
        if (d <= 1) {
            discriminant_ = Integer(int64_t(1));
            return;
        }

        // Build f and f' coefficient vectors
        std::vector<Integer> f(d + 1), f_prime(d);
        for (uint32_t i = 0; i <= d; ++i) {
            f[i] = ctx_.coeff(i).clone();
        }
        for (uint32_t i = 0; i < d; ++i) {
            f_prime[i] = f[i + 1].clone();
            f_prime[i] *= Integer(static_cast<int64_t>(i + 1));
        }

        // Compute Res(f, f') via Sylvester matrix determinant
        // Sylvester matrix size = d + (d-1) = 2d-1
        Integer res = compute_resultant(f, d, f_prime, d - 1);

        // Δ = (-1)^(d(d-1)/2) · Res(f, f') / a_d
        uint32_t sign_exp = d * (d - 1) / 2;
        if (sign_exp % 2 == 1) {
            res.negate();
        }
        Integer a_d = ctx_.coeff(d).clone();
        if (!a_d.is_zero()) {
            res /= a_d;
        }

        discriminant_ = std::move(res);
    }

    /// Compute resultant Res(f, g) via Sylvester matrix determinant
    /// f has degree deg_f, g has degree deg_g
    /// Matrix size = deg_f + deg_g
    [[nodiscard]] static Integer compute_resultant(
            const std::vector<Integer>& f, uint32_t deg_f,
            const std::vector<Integer>& g, uint32_t deg_g) {

        uint32_t n = deg_f + deg_g;
        if (n == 0) return Integer(int64_t(1));

        // Build Sylvester matrix (n × n)
        // First deg_g rows: coefficients of x^{deg_g-1}·f, ..., f (shifted copies)
        // Last deg_f rows: coefficients of x^{deg_f-1}·g, ..., g (shifted copies)
        // Integer default-inits to 0 — no explicit zero-fill needed.
        std::vector<std::vector<Integer>> M(n, std::vector<Integer>(n));

        // First deg_g rows from f
        for (uint32_t row = 0; row < deg_g; ++row) {
            for (uint32_t k = 0; k <= deg_f; ++k) {
                // row-th shifted copy: f[k] at column row + k
                uint32_t col = row + k;
                if (col < n) {
                    M[row][col] = f[deg_f - k].clone(); // coefficients in descending order
                }
            }
        }

        // Last deg_f rows from g
        for (uint32_t row = 0; row < deg_f; ++row) {
            for (uint32_t k = 0; k <= deg_g; ++k) {
                uint32_t col = row + k;
                if (col < n) {
                    M[deg_g + row][col] = g[deg_g - k].clone();
                }
            }
        }

        // Compute determinant using Bareiss algorithm (fraction-free)
        return bareiss_determinant(M, n);
    }

    /// Bareiss algorithm: fraction-free Gaussian elimination for Integer matrix determinant
    [[nodiscard]] static Integer bareiss_determinant(
            std::vector<std::vector<Integer>>& M, uint32_t n) {
        int sign = 1;
        Integer prev_pivot(int64_t(1));

        for (uint32_t k = 0; k < n; ++k) {
            // Find pivot in column k from rows k..n-1
            uint32_t pivot_row = k;
            while (pivot_row < n && M[pivot_row][k].is_zero()) {
                ++pivot_row;
            }
            if (pivot_row == n) {
                return Integer(int64_t(0)); // singular
            }
            if (pivot_row != k) {
                std::swap(M[k], M[pivot_row]);
                sign = -sign;
            }

            Integer cur_pivot = M[k][k].clone();

            // Eliminate below pivot
            // v22: term1/term2 复用 buffer (n² 节省 2×(n-k-1)² allocs)
            Integer term1;
            Integer term2;
            for (uint32_t i = k + 1; i < n; ++i) {
                for (uint32_t j = k + 1; j < n; ++j) {
                    // M[i][j] = (cur_pivot * M[i][j] - M[i][k] * M[k][j]) / prev_pivot
                    term1 = cur_pivot;
                    term1 *= M[i][j];
                    term2 = M[i][k];
                    term2 *= M[k][j];
                    term1 -= term2;
                    term1 /= prev_pivot; // exact division guaranteed by Bareiss
                    M[i][j] = std::move(term1);
                }
                M[i][k] = Integer(int64_t(0));
            }

            prev_pivot = std::move(cur_pivot);
        }

        // Determinant = sign * M[n-1][n-1]
        Integer result = M[n - 1][n - 1].clone();
        if (sign < 0) {
            result.negate();
        }
        return result;
    }

public:
    /// Count the number of distinct real roots of f(x) using Sturm's theorem.
    /// Works for any degree polynomial with Integer coefficients.
    /// Uses pseudo-remainder over Z with sign correction + content reduction.
    [[nodiscard]] static uint32_t count_real_roots(const std::vector<Integer>& coeffs, uint32_t degree) {
        if (degree == 0) return 0;
        if (degree == 1) return 1;

        // Guard: polynomial must not be identically zero
        bool all_zero = true;
        for (uint32_t i = 0; i <= degree && i < coeffs.size(); ++i) {
            if (!coeffs[i].is_zero()) { all_zero = false; break; }
        }
        if (all_zero) {
            throw std::logic_error("count_real_roots: zero polynomial");
        }

        // --- internal polynomial type ---
        struct IntPoly {
            std::vector<Integer> c;  // c[i] = coefficient of x^i
            IntPoly() = default;
            // c(n) default-inits Integer to 0 — no explicit zero loop needed.
            explicit IntPoly(size_t n) : c(n) {}
            [[nodiscard]] int deg() const {
                for (int i = static_cast<int>(c.size()) - 1; i >= 0; --i)
                    if (!c[static_cast<size_t>(i)].is_zero()) return i;
                return -1;
            }
            [[nodiscard]] int leading_sign() const {
                int d = deg();
                if (d < 0) return 0;
                if (c[static_cast<size_t>(d)].is_negative()) return -1;
                return 1;
            }
        };

        // --- build f_0 = f ---
        IntPoly f0(static_cast<size_t>(degree) + 1);
        for (uint32_t i = 0; i <= degree; ++i) f0.c[i] = coeffs[i].clone();

        // --- build f_1 = f' ---
        IntPoly f1(degree);
        for (uint32_t i = 1; i <= degree; ++i) {
            f1.c[i - 1] = coeffs[i].clone();
            f1.c[i - 1] *= Integer(static_cast<int64_t>(i));
        }

        // --- helper: compute pseudo-remainder prem(A, B) over Z ---
        // Returns {prem, actual_iters} where prem satisfies:
        //   lc(B)^actual_iters * A = Q * B + prem
        // Note: actual_iters <= deg(A)-deg(B)+1; can be less when degree
        // drops by >1 in a single step.
        struct PremResult { IntPoly R; int iters; };
        auto pseudo_remainder = [](const IntPoly& A, const IntPoly& B) -> PremResult {
            int db = B.deg();
            assert(db >= 0);

            IntPoly R(A.c.size());
            for (size_t i = 0; i < A.c.size(); ++i) R.c[i] = A.c[i].clone();

            const Integer& lc_b = B.c[static_cast<size_t>(db)];

            int iters = 0;
            int max_iters = static_cast<int>(A.c.size()) + 10;
            for (int iter = 0; iter < max_iters; ++iter) {
                int dr = R.deg();
                if (dr < db) break;

                Integer lc_r = R.c[static_cast<size_t>(dr)].clone();
                int shift = dr - db;

                // R = lc_b * R - lc_r * x^shift * B
                for (size_t i = 0; i < R.c.size(); ++i) {
                    R.c[i] *= lc_b;
                }
                // v22: term 复用 (mpz_set) 节省 db allocs/iter
                Integer term;
                for (int i = 0; i <= db; ++i) {
                    term = lc_r;
                    term *= B.c[static_cast<size_t>(i)];
                    R.c[static_cast<size_t>(i + shift)] -= term;
                }
                ++iters;
            }
            return {std::move(R), iters};
        };

        // --- helper: divide out abs(content) to prevent coefficient blowup ---
        auto reduce_content = [](IntPoly& P) {
            Integer content(int64_t(0));
            for (const auto& x : P.c) {
                if (!x.is_zero()) {
                    Integer ax = x.clone();
                    if (ax.is_negative()) ax.negate();
                    if (content.is_zero()) {
                        content = std::move(ax);
                    } else {
                        content = core::gcd(content, ax);
                    }
                }
            }
            if (!content.is_zero() && !(content == Integer(int64_t(1)))) {
                for (auto& x : P.c) {
                    x /= content;
                }
            }
        };

        // --- build Sturm chain ---
        // f_0 = f, f_1 = f'
        // f_{k+1} = -prem(f_{k-1}, f_k), with sign correction for pseudo-remainder scaling
        std::vector<IntPoly> chain;
        chain.reserve(f0.deg() + 2);  // chain length ≤ degree + 1
        chain.push_back(std::move(f0));
        chain.push_back(std::move(f1));

        while (true) {
            const IntPoly& prev2 = chain[chain.size() - 2];
            const IntPoly& prev1 = chain[chain.size() - 1];

            int d1 = prev1.deg();
            if (d1 < 0) break;

            auto [R, actual_iters] = pseudo_remainder(prev2, prev1);

            // Negate to get Sturm chain element: f_{k+1} = -rem(f_{k-1}, f_k)
            for (auto& x : R.c) x.negate();

            // Sign correction for pseudo-remainder scaling factor lc(B)^s:
            // prem(A,B) = lc(B)^s · rem(A,B) where s = actual_iters.
            // After negation: -prem = lc(B)^s · f_{k+1}
            // If lc(B)^s < 0, signs are flipped — negate to restore.
            // lc(B)^s < 0 iff lc(B) < 0 AND s is odd.
            int lc_sign = prev1.leading_sign();
            if (lc_sign < 0 && actual_iters % 2 == 1) {
                for (auto& x : R.c) x.negate();
            }

            // Reduce content to keep coefficients manageable
            reduce_content(R);

            if (R.deg() < 0) break;

            chain.push_back(std::move(R));
        }

        // --- count sign changes at +∞ and -∞ ---
        auto count_sign_changes = [&](bool at_neg_inf) -> uint32_t {
            uint32_t changes = 0;
            int prev_sign = 0;

            for (const auto& p : chain) {
                int d = p.deg();
                if (d < 0) continue;

                int sign = p.leading_sign();
                if (at_neg_inf && d % 2 == 1) sign = -sign;

                if (sign == 0) continue;

                if (prev_sign != 0 && prev_sign != sign) {
                    ++changes;
                }
                prev_sign = sign;
            }
            return changes;
        };

        uint32_t v_neg = count_sign_changes(true);
        uint32_t v_pos = count_sign_changes(false);

        return (v_neg >= v_pos) ? (v_neg - v_pos) : 0;
    }

private:
    /// Compute the Minkowski bound using correct signature for any degree.
    /// Signature (r1, r2): r1 = real roots, r2 = complex conjugate pairs, r1 + 2*r2 = d.
    /// Uses Sturm's theorem to count real roots exactly.
    void compute_minkowski_bound() {
        uint32_t d = ctx_.degree();

        // Determine signature (r1, r2) using Sturm's theorem
        std::vector<Integer> f(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            f[i] = ctx_.coeff(i).clone();
        }
        uint32_t r1 = count_real_roots(f, d);
        if (r1 > d || (d - r1) % 2 != 0) {
            throw std::logic_error(
                "compute_minkowski_bound: invalid signature r1=" +
                std::to_string(r1) + " for degree " + std::to_string(d));
        }
        uint32_t r2 = (d - r1) / 2;

        // Minkowski bound: M = (d!/d^d) * (4/π)^r2 * sqrt(|Δ|)
        double d_factorial = 1.0;
        for (uint32_t i = 2; i <= d; ++i) {
            d_factorial *= i;
        }

        double d_power_d = std::pow(static_cast<double>(d), static_cast<double>(d));
        double four_over_pi = 4.0 / M_PI;
        double four_pi_factor = std::pow(four_over_pi, static_cast<double>(r2));

        // Get |discriminant| as double
        Integer abs_disc = discriminant_.clone();
        if (abs_disc.is_negative()) {
            abs_disc.negate();
        }
        double disc_sqrt = std::sqrt(abs_disc.to_double());

        minkowski_bound_ = (d_factorial / d_power_d) * four_pi_factor * disc_sqrt;
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
        roots.reserve(d);  // bounded by polynomial degree

        // 预计算 c[i] mod p 一次 (从 ctx_.coeff() clone 在 (i,x) 内层是冷启动 BUG):
        // p × (d+1) clones/mods 节省 → 典型 p=1000/d=5 时省 6000 clones per prime
        const uint64_t p64 = static_cast<uint64_t>(p);
        std::vector<uint64_t> c_mod_p(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            Integer c = ctx_.coeff(i).clone();
            c %= Integer(p64);
            if (c.is_negative()) c += Integer(p64);
            c_mod_p[i] = c.to_uint64();
        }

        for (uint32_t x = 0; x < p; ++x) {
            uint64_t val = 0;
            uint64_t x_power = 1;

            for (uint32_t i = 0; i <= d; ++i) {
                val = (val + c_mod_p[i] * x_power) % p64;
                x_power = (x_power * x) % p64;
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
        // Reserve: search loop iterates (2·sb+1) × sb pairs, with low hit rate.
        // Conservative reserve: ~10% pairs yield principal ideal.
        relations.reserve(static_cast<size_t>((2 * search_bound + 1) * search_bound / 10));

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
            // Use __int128_t to avoid overflow when b*r approaches INT64_MAX
            __int128_t val128 = static_cast<__int128_t>(a)
                              - static_cast<__int128_t>(b) * static_cast<__int128_t>(pi.r);

            int exp = 0;
            if (val128 == 0) {
                // a = b*r exactly: (a - bα) = -b(α - r)
                // v_P = v_p(b) + 1 for unramified degree-1 prime ideal
                exp = 1;
                uint64_t b_tmp = b;
                while (b_tmp % pi.p == 0) {
                    ++exp;
                    b_tmp /= pi.p;
                }
            } else {
                while (val128 % static_cast<__int128_t>(pi.p) == 0) {
                    ++exp;
                    val128 /= static_cast<__int128_t>(pi.p);
                }
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
            // Use __int128_t to avoid overflow when b*r approaches INT64_MAX
            __int128_t val128 = static_cast<__int128_t>(a)
                              - static_cast<__int128_t>(b) * static_cast<__int128_t>(gen.r);

            int exp = 0;
            if (val128 == 0) {
                // a = b*r exactly: (a - bα) = -b(α - r)
                // v_P = v_p(b) + 1 for unramified degree-1 prime ideal
                exp = 1;
                uint64_t b_tmp = b;
                while (b_tmp % gen.p == 0) {
                    ++exp;
                    b_tmp /= gen.p;
                }
            } else {
                while (val128 % static_cast<__int128_t>(gen.p) == 0) {
                    ++exp;
                    val128 /= static_cast<__int128_t>(gen.p);
                }
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

    /// Compute Smith Normal Form of the relation matrix
    /// Returns invariant factors, class number, and generator indices
    [[nodiscard]] SNFResult compute_smith_normal_form(
            std::vector<std::vector<int>>& matrix,
            size_t num_cols) const {

        SNFResult result;
        result.class_number = 1;

        if (matrix.empty() || num_cols == 0) {
            return result;
        }

        size_t num_rows = matrix.size();
        size_t min_dim = std::min(num_rows, num_cols);

        // Smith Normal Form via row/column operations over Z
        for (size_t k = 0; k < min_dim; ++k) {
            // Phase 1: Find smallest nonzero |entry| and move to (k,k)
            bool found = false;
            while (true) {
                // Find minimum nonzero |entry| in submatrix [k:, k:]
                int min_abs = 0;
                size_t min_r = k, min_c = k;
                for (size_t i = k; i < num_rows; ++i) {
                    for (size_t j = k; j < num_cols; ++j) {
                        if (matrix[i][j] != 0) {
                            int abs_val = std::abs(matrix[i][j]);
                            if (min_abs == 0 || abs_val < min_abs) {
                                min_abs = abs_val;
                                min_r = i;
                                min_c = j;
                            }
                        }
                    }
                }
                if (min_abs == 0) break; // all zero in submatrix

                found = true;

                // Move pivot to (k,k)
                if (min_r != k) std::swap(matrix[min_r], matrix[k]);
                if (min_c != k) {
                    for (size_t i = 0; i < num_rows; ++i) {
                        std::swap(matrix[i][min_c], matrix[i][k]);
                    }
                }

                // Phase 2: Eliminate row k and column k
                bool changed = true;
                while (changed) {
                    changed = false;

                    // Eliminate column k
                    for (size_t i = k + 1; i < num_rows; ++i) {
                        if (matrix[i][k] != 0) {
                            int q = matrix[i][k] / matrix[k][k];
                            for (size_t j = k; j < num_cols; ++j) {
                                matrix[i][j] -= q * matrix[k][j];
                            }
                            if (matrix[i][k] != 0) {
                                // Didn't fully eliminate — swap and retry
                                std::swap(matrix[i], matrix[k]);
                                changed = true;
                                break;
                            }
                        }
                    }
                    if (changed) continue;

                    // Eliminate row k
                    for (size_t j = k + 1; j < num_cols; ++j) {
                        if (matrix[k][j] != 0) {
                            int q = matrix[k][j] / matrix[k][k];
                            for (size_t i = k; i < num_rows; ++i) {
                                matrix[i][j] -= q * matrix[i][k];
                            }
                            if (matrix[k][j] != 0) {
                                // Swap columns and retry
                                for (size_t i = 0; i < num_rows; ++i) {
                                    std::swap(matrix[i][j], matrix[i][k]);
                                }
                                changed = true;
                                break;
                            }
                        }
                    }
                }

                // Phase 3: Check divisibility — m[k][k] must divide all entries below
                bool divisible = true;
                for (size_t i = k + 1; i < num_rows && divisible; ++i) {
                    for (size_t j = k + 1; j < num_cols && divisible; ++j) {
                        if (matrix[i][j] % matrix[k][k] != 0) {
                            // Add row i to row k, then re-eliminate
                            for (size_t c = k; c < num_cols; ++c) {
                                matrix[k][c] += matrix[i][c];
                            }
                            divisible = false;
                        }
                    }
                }
                if (divisible) break; // done with position k
                // else: loop back — now (k,k) has a smaller value
            }

            if (!found) break;

            // Make diagonal entry positive
            if (matrix[k][k] < 0) {
                for (size_t j = k; j < num_cols; ++j) {
                    matrix[k][j] = -matrix[k][j];
                }
            }
        }

        // Collect invariant factors (diagonal entries > 1)
        // and compute class number
        for (size_t k = 0; k < min_dim; ++k) {
            int d_k = std::abs(matrix[k][k]);
            if (d_k > 1) {
                result.invariant_factors.push_back(d_k);
                result.generator_indices.push_back(k);
                // Protect against overflow
                if (result.class_number <= UINT32_MAX / static_cast<uint32_t>(d_k)) {
                    result.class_number *= static_cast<uint32_t>(d_k);
                } else {
                    result.class_number = UINT32_MAX;
                }
            }
        }
        // Columns beyond min_dim that had no pivot also contribute
        // (zero diagonal = infinite cyclic factor, but in class group context
        // these indicate free generators needing more relations)

        return result;
    }

    /// Simple primality test — delegates to util::is_prime_u32 (robust at uint32 boundary).
    [[nodiscard]] static bool is_prime(uint32_t n) {
        return gnfs::util::is_prime_u32(n);
    }
};

} // namespace gnfs::sqrt
