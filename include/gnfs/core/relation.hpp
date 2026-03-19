#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/types.hpp"
#include "gnfs/util/safe_math.hpp"
#include <vector>
#include <cstdint>
#include <iostream>

namespace gnfs::core {

/// A relation in GNFS represents a smooth (a,b) pair
struct Relation {
    /// Type alias for large prime lists
    using LargePrimeList = std::vector<PrimePower>;

    int64_t a = 0;
    int64_t b = 0;

    // Rational side factorization: indices into factor base
    std::vector<uint32_t> rational_factors;

    // Algebraic side factorization: indices into factor base
    std::vector<uint32_t> algebraic_factors;

    // Large primes (if any) - stored as vectors for multiple large primes
    LargePrimeList rational_large_prime;
    LargePrimeList algebraic_large_prime;

    // Default constructor
    Relation() = default;

    // Constructor with (a, b) pair
    Relation(int64_t a_, int64_t b_) : a(a_), b(b_) {}

    // Get as ABPair
    [[nodiscard]] ABPair ab() const {
        return ABPair(a, util::safe_abs(b));
    }

    // Check if valid (b != 0)
    [[nodiscard]] bool is_valid() const {
        return b != 0;
    }

    // Check if this is a full relation (no large primes)
    [[nodiscard]] bool is_full() const {
        return rational_large_prime.empty() && algebraic_large_prime.empty();
    }

    // Total number of large primes
    [[nodiscard]] size_t num_large_primes() const {
        return rational_large_prime.size() + algebraic_large_prime.size();
    }

    // Clone this relation
    [[nodiscard]] Relation clone() const {
        Relation copy;
        copy.a = a;
        copy.b = b;
        copy.rational_factors = rational_factors;
        copy.algebraic_factors = algebraic_factors;
        copy.rational_large_prime = rational_large_prime;
        copy.algebraic_large_prime = algebraic_large_prime;
        return copy;
    }

    // Serialize to output stream
    void serialize(std::ostream& os) const {
        os.write(reinterpret_cast<const char*>(&a), sizeof(a));
        os.write(reinterpret_cast<const char*>(&b), sizeof(b));

        // Write rational factors
        uint32_t rat_count = static_cast<uint32_t>(rational_factors.size());
        os.write(reinterpret_cast<const char*>(&rat_count), sizeof(rat_count));
        os.write(reinterpret_cast<const char*>(rational_factors.data()),
                 rat_count * sizeof(uint32_t));

        // Write algebraic factors
        uint32_t alg_count = static_cast<uint32_t>(algebraic_factors.size());
        os.write(reinterpret_cast<const char*>(&alg_count), sizeof(alg_count));
        os.write(reinterpret_cast<const char*>(algebraic_factors.data()),
                 alg_count * sizeof(uint32_t));

        // Write large primes (rational)
        uint32_t lp_rat_count = static_cast<uint32_t>(rational_large_prime.size());
        os.write(reinterpret_cast<const char*>(&lp_rat_count), sizeof(lp_rat_count));
        for (const auto& lp : rational_large_prime) {
            os.write(reinterpret_cast<const char*>(&lp.p), sizeof(lp.p));
            os.write(reinterpret_cast<const char*>(&lp.r), sizeof(lp.r));
            os.write(reinterpret_cast<const char*>(&lp.e), sizeof(lp.e));
        }

        // Write large primes (algebraic)
        uint32_t lp_alg_count = static_cast<uint32_t>(algebraic_large_prime.size());
        os.write(reinterpret_cast<const char*>(&lp_alg_count), sizeof(lp_alg_count));
        for (const auto& lp : algebraic_large_prime) {
            os.write(reinterpret_cast<const char*>(&lp.p), sizeof(lp.p));
            os.write(reinterpret_cast<const char*>(&lp.r), sizeof(lp.r));
            os.write(reinterpret_cast<const char*>(&lp.e), sizeof(lp.e));
        }
    }

    // Deserialize from input stream
    static Relation deserialize(std::istream& is) {
        Relation rel;
        is.read(reinterpret_cast<char*>(&rel.a), sizeof(rel.a));
        is.read(reinterpret_cast<char*>(&rel.b), sizeof(rel.b));

        // Read rational factors
        uint32_t rat_count;
        is.read(reinterpret_cast<char*>(&rat_count), sizeof(rat_count));
        rel.rational_factors.resize(rat_count);
        is.read(reinterpret_cast<char*>(rel.rational_factors.data()),
                rat_count * sizeof(uint32_t));

        // Read algebraic factors
        uint32_t alg_count;
        is.read(reinterpret_cast<char*>(&alg_count), sizeof(alg_count));
        rel.algebraic_factors.resize(alg_count);
        is.read(reinterpret_cast<char*>(rel.algebraic_factors.data()),
                alg_count * sizeof(uint32_t));

        // Read large primes (rational)
        uint32_t lp_rat_count;
        is.read(reinterpret_cast<char*>(&lp_rat_count), sizeof(lp_rat_count));
        rel.rational_large_prime.reserve(lp_rat_count);
        for (uint32_t i = 0; i < lp_rat_count; ++i) {
            PrimePower lp;
            is.read(reinterpret_cast<char*>(&lp.p), sizeof(lp.p));
            is.read(reinterpret_cast<char*>(&lp.r), sizeof(lp.r));
            is.read(reinterpret_cast<char*>(&lp.e), sizeof(lp.e));
            rel.rational_large_prime.push_back(lp);
        }

        // Read large primes (algebraic)
        uint32_t lp_alg_count;
        is.read(reinterpret_cast<char*>(&lp_alg_count), sizeof(lp_alg_count));
        rel.algebraic_large_prime.reserve(lp_alg_count);
        for (uint32_t i = 0; i < lp_alg_count; ++i) {
            PrimePower lp;
            is.read(reinterpret_cast<char*>(&lp.p), sizeof(lp.p));
            is.read(reinterpret_cast<char*>(&lp.r), sizeof(lp.r));
            is.read(reinterpret_cast<char*>(&lp.e), sizeof(lp.e));
            rel.algebraic_large_prime.push_back(lp);
        }

        return rel;
    }
};

} // namespace gnfs::core
