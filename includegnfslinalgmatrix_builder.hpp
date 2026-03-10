#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/factor_base/builder.hpp"
#include <vector>
#include <cstdint>

namespace gnfs::linalg {

using gnfs::core::Relation;
using gnfs::factor_base::FactorBase;

/// Sparse matrix for linear algebra
struct SparseMatrix {
    size_t rows;
    size_t cols;
    std::vector<std::vector<uint32_t>> data;  // data[row] = list of column indices
    
    SparseMatrix() : rows(0), cols(0) {}
    SparseMatrix(size_t r, size_t c) : rows(r), cols(c), data(r) {}
};

/// Build matrix from relations
class MatrixBuilder {
public:
    MatrixBuilder(const FactorBase& fb);
    
    /// Build sparse matrix from relations
    SparseMatrix build(const std::vector<Relation>& relations);
    
private:
    FactorBase fb_;
};

} // namespace gnfs::linalg
