#include "gnfs/linalg/matrix_builder.hpp"

namespace gnfs::linalg {

MatrixBuilder::MatrixBuilder(const FactorBase& fb) : fb_(fb) {}

SparseMatrix MatrixBuilder::build(const std::vector<Relation>& relations) {
    size_t num_primes = fb_.rational_size() + fb_.algebraic_size();
    SparseMatrix matrix(relations.size(), num_primes);
    
    for (size_t i = 0; i < relations.size(); ++i) {
        const auto& rel = relations[i];
        
        // Add rational factors
        for (uint32_t factor : rel.rational_factors) {
            // Find index in factor base
            for (size_t j = 0; j < fb_.rational_primes.size(); ++j) {
                if (fb_.rational_primes[j].prime == factor) {
                    matrix.data[i].push_back(static_cast<uint32_t>(j));
                    break;
                }
            }
        }
        
        // Add algebraic factors
        for (uint32_t factor : rel.algebraic_factors) {
            size_t offset = fb_.rational_size();
            for (size_t j = 0; j < fb_.algebraic_primes.size(); ++j) {
                if (fb_.algebraic_primes[j].prime == factor) {
                    matrix.data[i].push_back(static_cast<uint32_t>(offset + j));
                    break;
                }
            }
        }
    }
    
    return matrix;
}

} // namespace gnfs::linalg
