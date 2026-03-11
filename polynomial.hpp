#pragma once

#include "gnfs/core/integer.hpp"
#include <vector>
#include <cstddef>

namespace gnfs::core {

/// Polynomial with Integer coefficients
class IntPolynomial {
public:
    IntPolynomial() = default;
    explicit IntPolynomial(size_t degree);
    IntPolynomial(std::vector<Integer> coeffs);

    // Access
    size_t degree() const;
    const Integer& operator[](size_t i) const;
    Integer& operator[](size_t i);

    // Evaluation
    Integer evaluate(const Integer& x) const;

    // Resize
    void resize(size_t new_degree);
    
    // Normalize (remove leading zeros)
    void normalize();

    const std::vector<Integer>& coefficients() const { return coeffs_; }
    std::vector<Integer>& coefficients() { return coeffs_; }

private:
    std::vector<Integer> coeffs_;
};

/// Polynomial context for GNFS
struct PolynomialContext {
    Integer n;              // Number to factor
    IntPolynomial f;        // Algebraic polynomial
    IntPolynomial g;        // Rational polynomial (usually linear: g(x) = x - m)
    Integer m;              // Common root modulo n
    
    PolynomialContext() = default;
    PolynomialContext(Integer n_, IntPolynomial f_, IntPolynomial g_, Integer m_)
        : n(std::move(n_)), f(std::move(f_)), g(std::move(g_)), m(std::move(m_)) {}
    
    // Evaluate f(x) mod p
    uint64_t evaluate_mod(uint32_t x, uint32_t p) const {
        uint64_t result = 0;
        
        for (size_t i = f.degree() + 1; i > 0; --i) {
            size_t idx = i - 1;
            result = (result * x) % p;
            
            // Get coefficient mod p
            Integer coeff = f[idx].clone();
            coeff %= Integer(static_cast<int64_t>(p));
            
            // Handle negative coefficients
            int64_t c = coeff.is_negative() ? 
                       (static_cast<int64_t>(p) + coeff.to_int64()) : coeff.to_int64();
            
            result = (result + static_cast<uint64_t>(c)) % p;
        }
        
        return result;
    }
};

} // namespace gnfs::core
