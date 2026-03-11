#include "gnfs/factor_base/builder.hpp"
#include <algorithm>

namespace gnfs::factor_base {

// ============================================================
// FactorBase methods
// ============================================================

std::optional<size_t> FactorBase::find_rational(uint32_t prime) const {
    for (size_t i = 0; i < rational_primes_.size(); ++i) {
        if (rational_primes_[i].p == prime) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<size_t> FactorBase::find_algebraic(uint32_t prime, uint32_t root) const {
    for (size_t i = 0; i < algebraic_primes_.size(); ++i) {
        if (algebraic_primes_[i].p == prime && algebraic_primes_[i].r == root) {
            return i;
        }
    }
    return std::nullopt;
}

// ============================================================
// FactorBaseBuilder methods
// ============================================================

FactorBaseBuilder::FactorBaseBuilder(const PolynomialContext& ctx) : ctx_(ctx) {}

FactorBase FactorBaseBuilder::build(const PolynomialContext& ctx, const Options& opts) {
    FactorBase fb;
    
    find_rational_primes(fb, opts.rational_bound);
    find_algebraic_primes(fb, ctx, opts.algebraic_bound);
    
    return fb;
}

FactorBase FactorBaseBuilder::build(uint32_t rational_bound, uint32_t algebraic_bound) {
    Options opts;
    opts.rational_bound = rational_bound;
    opts.algebraic_bound = algebraic_bound;
    return build(ctx_, opts);
}

void FactorBaseBuilder::find_rational_primes(FactorBase& fb, uint32_t bound) {
    // Sieve of Eratosthenes
    std::vector<bool> is_prime(bound + 1, true);
    is_prime[0] = is_prime[1] = false;
    
    for (uint32_t p = 2; p <= bound; ++p) {
        if (!is_prime[p]) continue;
        
        // Mark multiples
        for (uint32_t k = p * 2; k <= bound; k += p) {
            is_prime[k] = false;
        }
        
        // Add to factor base (root is 0 for rational side)
        fb.rational_primes().emplace_back(p, 0);
    }
}

void FactorBaseBuilder::find_algebraic_primes(FactorBase& fb, const PolynomialContext& ctx, uint32_t bound) {
    // Sieve of Eratosthenes
    std::vector<bool> is_prime(bound + 1, true);
    is_prime[0] = is_prime[1] = false;
    
    for (uint32_t p = 2; p <= bound; ++p) {
        if (!is_prime[p]) continue;
        
        // Mark multiples
        for (uint32_t k = p * 2; k <= bound; k += p) {
            is_prime[k] = false;
        }
        
        // Find roots of f(x) ≡ 0 (mod p)
        auto roots = find_roots_mod_p(ctx.f, p);
        
        for (uint32_t root : roots) {
            fb.algebraic_primes().emplace_back(p, root);
        }
    }
}

std::vector<uint32_t> FactorBaseBuilder::find_roots_mod_p(const gnfs::core::IntPolynomial& f, uint32_t p) {
    std::vector<uint32_t> roots;
    
    // Brute force search for roots
    // For small p this is acceptable
    for (uint32_t r = 0; r < p; ++r) {
        // Evaluate f(r) mod p using Horner's method
        uint64_t result = 0;
        
        for (size_t i = f.degree() + 1; i > 0; --i) {
            size_t idx = i - 1;
            result = (result * r) % p;
            
            // Get coefficient mod p
            Integer coeff = f[idx].clone();
            coeff %= Integer(static_cast<int64_t>(p));
            
            // Handle negative coefficients
            int64_t c = coeff.is_negative() ? 
                       (static_cast<int64_t>(p) + coeff.to_int64()) : coeff.to_int64();
            
            result = (result + c) % p;
        }
        
        if (result == 0) {
            roots.push_back(r);
        }
    }
    
    return roots;
}

} // namespace gnfs::factor_base
