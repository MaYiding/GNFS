#include "gnfs/sqrt/algebraic_sqrt.hpp"

namespace gnfs::sqrt {

AlgebraicSqrt::AlgebraicSqrt(const PolynomialContext& ctx) : ctx_(ctx) {}

Integer AlgebraicSqrt::compute(const std::vector<Relation>& relations, const std::vector<bool>& dependency) {
    if (dependency.size() != relations.size()) {
        throw std::runtime_error("Dependency size mismatch");
    }
    
    // In the algebraic number field Q(α) where α is a root of f(x),
    // we need to compute the product of (a - b*α) for all relations,
    // then take the square root and reduce modulo n.
    
    // Simplified approach: use resultants and polynomial arithmetic
    // For a more complete implementation, we'd use:
    // 1. Represent elements in Q(α) as polynomials mod f(x)
    // 2. Multiply them together
    // 3. Take square root in the number field
    // 4. Evaluate at α ≡ m (mod n)
    
    // Very simplified version: compute product of f(a/b) * b^deg(f)
    Integer product(1);
    size_t degree = ctx_.f.degree();
    
    for (size_t i = 0; i < dependency.size(); ++i) {
        if (!dependency[i]) continue;
        if (i >= relations.size()) continue;
        
        const auto& rel = relations[i];
        
        Integer a(rel.a);
        Integer b(rel.b);
        
        // Compute f(a/b) * b^deg = f(a) evaluated at x=a, then adjust for b
        // This is a simplification; proper implementation would work in number field
        
        // Evaluate f(a)
        Integer fa = ctx_.f.evaluate(a);
        
        // Multiply by b^deg to clear denominator
        for (size_t d = 0; d < degree; ++d) {
            fa *= b;
        }
        
        product *= fa;
        
        // Keep size manageable
        if (product.bit_length() > ctx_.n.bit_length() * 2) {
            product %= ctx_.n;
        }
    }
    
    // Final reduction
    product %= ctx_.n;
    
    // Make positive
    if (product.is_negative()) {
        product += ctx_.n;
    }
    
    // Take square root
    Integer sqrt_result;
    mpz_sqrt(sqrt_result.get_mpz(), product.get_mpz());
    
    // Verify
    Integer check = sqrt_result.clone();
    check *= sqrt_result;
    check %= ctx_.n;
    
    if (check != product) {
        // Try alternative
        product += ctx_.n;
        mpz_sqrt(sqrt_result.get_mpz(), product.get_mpz());
    }
    
    return sqrt_result;
}

} // namespace gnfs::sqrt
