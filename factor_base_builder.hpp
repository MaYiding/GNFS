#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial.hpp"
#include <vector>
#include <cstdint>
#include <optional>
#include <span>

namespace gnfs::factor_base {

using gnfs::core::Integer;
using gnfs::core::PolynomialContext;

/// Prime in the factor base
struct FactorBasePrime {
    uint32_t p;      // Prime number
    uint32_t r;      // Root of polynomial mod prime
    
    FactorBasePrime() : p(0), r(0) {}
    FactorBasePrime(uint32_t prime, uint32_t root) : p(prime), r(root) {}
    
    // For compatibility
    uint32_t prime() const { return p; }
    uint32_t root() const { return r; }
};

/// Factor base for GNFS
class FactorBase {
public:
    FactorBase() = default;
    
    // Access methods
    size_t rational_count() const { return rational_primes_.size(); }
    size_t algebraic_count() const { return algebraic_primes_.size(); }
    
    // For compatibility with old API
    size_t rational_size() const { return rational_count(); }
    size_t algebraic_size() const { return algebraic_count(); }
    
    // Get prime lists
    std::span<const FactorBasePrime> rational() const { 
        return std::span<const FactorBasePrime>(rational_primes_); 
    }
    std::span<const FactorBasePrime> algebraic() const { 
        return std::span<const FactorBasePrime>(algebraic_primes_); 
    }
    
    // Find prime index
    std::optional<size_t> find_rational(uint32_t prime) const;
    std::optional<size_t> find_algebraic(uint32_t prime, uint32_t root) const;
    
    // Direct access (for internal use)
    std::vector<FactorBasePrime>& rational_primes() { return rational_primes_; }
    std::vector<FactorBasePrime>& algebraic_primes() { return algebraic_primes_; }
    const std::vector<FactorBasePrime>& rational_primes() const { return rational_primes_; }
    const std::vector<FactorBasePrime>& algebraic_primes() const { return algebraic_primes_; }
    
private:
    std::vector<FactorBasePrime> rational_primes_;
    std::vector<FactorBasePrime> algebraic_primes_;
};

/// Builder for factor base
class FactorBaseBuilder {
public:
    struct Options {
        uint32_t rational_bound = 1000;
        uint32_t algebraic_bound = 1000;
        bool parallel = true;
        
        Options() = default;
    };
    
    // Static build method
    static FactorBase build(const PolynomialContext& ctx, const Options& opts);
    
    // Instance-based method (for compatibility)
    FactorBaseBuilder(const PolynomialContext& ctx);
    FactorBase build(uint32_t rational_bound, uint32_t algebraic_bound);
    
private:
    PolynomialContext ctx_;
    
    static void find_rational_primes(FactorBase& fb, uint32_t bound);
    static void find_algebraic_primes(FactorBase& fb, const PolynomialContext& ctx, uint32_t bound);
    static std::vector<uint32_t> find_roots_mod_p(const gnfs::core::IntPolynomial& f, uint32_t p);
};

} // namespace gnfs::factor_base
