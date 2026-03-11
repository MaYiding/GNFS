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
    // Implementation details
};

} // namespace gnfs::linalg
