#include "gnfs/linalg/matrix_builder.hpp"
#include <unordered_map>
#include <algorithm>

namespace gnfs::linalg {

MatrixBuilder::MatrixBuilder(const FactorBase& fb) : fb_(fb) {}

SparseMatrix MatrixBuilder::build(const std::vector<Relation>& relations) {
    if (relations.empty()) {
        return SparseMatrix(0, 0);
    }
    
    size_t num_rational_primes = fb_.rational_count();
    size_t num_algebraic_primes = fb_.algebraic_count();
    size_t num_primes = num_rational_primes + num_algebraic_primes;
    
    SparseMatrix matrix(relations.size(), num_primes);
    
    // Build index maps for faster lookup
    std::unordered_map<uint32_t, uint32_t> rational_index_map;
    for (size_t i = 0; i < num_rational_primes; ++i) {
        rational_index_map[fb_.rational()[i].p] = static_cast<uint32_t>(i);
    }
    
    std::unordered_map<uint64_t, uint32_t> algebraic_index_map;
    for (size_t i = 0; i < num_algebraic_primes; ++i) {
        uint64_t key = (static_cast<uint64_t>(fb_.algebraic()[i].p) << 32) | fb_.algebraic()[i].r;
        algebraic_index_map[key] = static_cast<uint32_t>(num_rational_primes + i);
    }
    
    // Build matrix rows
    for (size_t i = 0; i < relations.size(); ++i) {
        const auto& rel = relations[i];
        std::unordered_map<uint32_t, uint32_t> exponent_map;
        
        // Count exponents for rational factors
        for (uint32_t factor : rel.rational_factors) {
            auto it = rational_index_map.find(factor);
            if (it != rational_index_map.end()) {
                exponent_map[it->second]++;
            }
        }
        
        // Count exponents for algebraic factors
        for (uint32_t factor : rel.algebraic_factors) {
            // Try to find in algebraic prime list
            for (size_t j = 0; j < num_algebraic_primes; ++j) {
                if (fb_.algebraic()[j].p == factor) {
                    uint32_t col = static_cast<uint32_t>(num_rational_primes + j);
                    exponent_map[col]++;
                    break;  // Only count first match
                }
            }
        }
        
        // In GF(2), we only care about odd exponents
        std::vector<uint32_t> row_indices;
        for (const auto& [col, exp] : exponent_map) {
            if (exp % 2 == 1) {
                row_indices.push_back(col);
            }
        }
        
        // Sort for consistency
        std::sort(row_indices.begin(), row_indices.end());
        matrix.data[i] = std::move(row_indices);
    }
    
    return matrix;
}

} // namespace gnfs::linalg
