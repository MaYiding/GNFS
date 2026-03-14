#pragma once

#include "gnfs/linalg/matrix_builder.hpp"
#include <vector>

namespace gnfs::linalg {

/// Block Lanczos algorithm for finding null space
class BlockLanczos {
public:
    BlockLanczos() = default;
    
    /// Find dependencies (null space vectors) in the matrix
    std::vector<std::vector<bool>> find_dependencies(const SparseMatrix& matrix, size_t max_deps = 64);
    
private:
    // Gaussian elimination for small matrices
    std::vector<std::vector<bool>> find_dependencies_gaussian(const SparseMatrix& matrix, size_t max_deps);
    
    // Iterative method for large matrices
    std::vector<std::vector<bool>> find_dependencies_iterative(const SparseMatrix& matrix, size_t max_deps);
};

} // namespace gnfs::linalg
