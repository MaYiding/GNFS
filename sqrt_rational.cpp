#include "gnfs/sqrt/rational_sqrt.hpp"

namespace gnfs::sqrt {

RationalSqrt::RationalSqrt(const PolynomialContext& ctx) : ctx_(ctx) {}

Integer RationalSqrt::compute(const std::vector<Relation>& relations, const std::vector<bool>& dependency) {
    if (dependency.size() != relations.size()) {
        throw std::runtime_error("Dependency size mismatch");
    }
    
    // Compute product of (a + b*m) for all relations in dependency
    Integer product(1);
    
    for (size_t i = 0; i < dependency.size(); ++i) {
        if (!dependency[i]) continue;
        if (i >= relations.size()) continue;
        
        const auto& rel = relations[i];
        
        // Compute a + b*m
        Integer a(rel.a);
        Integer b(rel.b);
        Integer bm = b * ctx_.m;
        Integer val = a + bm;
        
        // Multiply into product
        product *= val;
        
        // Reduce modulo n periodically to keep size manageable
        if (product.bit_length() > ctx_.n.bit_length() * 2) {
            product %= ctx_.n;
        }
    }
    
    // Final reduction
    product %= ctx_.n;
    
    // The product should be a perfect square in theory
    // Take square root modulo n
    // For simplicity, we'll use Tonelli-Shanks or just GMP's sqrt
    
    // If product is negative, make it positive
    if (product.is_negative()) {
        product += ctx_.n;
    }
    
    // Try to take square root
    Integer sqrt_result;
    mpz_sqrt(sqrt_result.get_mpz(), product.get_mpz());
    
    // Verify it's actually a square root
    Integer check = sqrt_result.clone();
    check *= sqrt_result;
    check %= ctx_.n;
    
    if (check != product) {
        // Not a perfect square - this might happen with wrong dependency
        // Try adding/subtracting ctx_.n
        product += ctx_.n;
        mpz_sqrt(sqrt_result.get_mpz(), product.get_mpz());
    }
    
    return sqrt_result;
}

} // namespace gnfs::sqrt
