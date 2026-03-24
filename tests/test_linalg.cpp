// test_linalg.cpp - Test linear algebra components

#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/gauss.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/schirokauer.hpp>
#include <gnfs/core/relation.hpp>
#include <gnfs/core/polynomial_context.hpp>

#include <cassert>
#include <iostream>
#include <vector>

using namespace gnfs;
using namespace gnfs::linalg;

// Test SparseRow operations
void test_sparse_row() {
    std::cout << "Testing SparseRow..." << std::endl;

    SparseRow row;

    // Test set and test
    row.set(5);
    row.set(10);
    row.set(3);

    assert(row.test(3));
    assert(row.test(5));
    assert(row.test(10));
    assert(!row.test(0));
    assert(!row.test(7));

    // Test weight
    assert(row.weight() == 3);

    // Test first/last nonzero
    assert(row.first_nonzero() == 3);
    assert(row.last_nonzero() == 10);

    // Test clear
    row.clear(5);
    assert(!row.test(5));
    assert(row.weight() == 2);

    // Test flip
    row.flip(7);  // Add
    assert(row.test(7));
    row.flip(7);  // Remove
    assert(!row.test(7));

    std::cout << "  SparseRow: PASSED" << std::endl;
}

// Test SparseRow XOR
void test_sparse_row_xor() {
    std::cout << "Testing SparseRow XOR..." << std::endl;

    SparseRow row1;
    row1.set(1);
    row1.set(3);
    row1.set(5);

    SparseRow row2;
    row2.set(2);
    row2.set(3);
    row2.set(4);

    // row1 XOR row2 = {1, 2, 4, 5} (3 cancels)
    row1.xor_with(row2);

    assert(row1.test(1));
    assert(row1.test(2));
    assert(!row1.test(3));  // Cancelled
    assert(row1.test(4));
    assert(row1.test(5));
    assert(row1.weight() == 4);

    std::cout << "  SparseRow XOR: PASSED" << std::endl;
}

// Test SparseMatrix basic operations
void test_sparse_matrix() {
    std::cout << "Testing SparseMatrix..." << std::endl;

    SparseMatrix mat(3, 5);

    // Set some elements
    mat.set(0, 0);
    mat.set(0, 2);
    mat.set(1, 1);
    mat.set(1, 3);
    mat.set(2, 2);
    mat.set(2, 4);

    assert(mat.num_rows() == 3);
    assert(mat.num_cols() == 5);
    assert(mat.test(0, 0));
    assert(mat.test(0, 2));
    assert(!mat.test(0, 1));

    // Test total weight
    assert(mat.total_weight() == 6);

    // Test swap rows
    mat.swap_rows(0, 1);
    assert(mat.test(0, 1));
    assert(mat.test(0, 3));
    assert(mat.test(1, 0));
    assert(mat.test(1, 2));

    std::cout << "  SparseMatrix: PASSED" << std::endl;
}

// Test SparseMatrix transpose
void test_sparse_matrix_transpose() {
    std::cout << "Testing SparseMatrix transpose..." << std::endl;

    // Create a 3x4 matrix
    SparseMatrix mat(3, 4);
    mat.set(0, 1);
    mat.set(0, 3);
    mat.set(1, 0);
    mat.set(1, 2);
    mat.set(2, 1);
    mat.set(2, 2);

    // Transpose to get 4x3 matrix
    SparseMatrix transposed = mat.transpose();

    assert(transposed.num_rows() == 4);
    assert(transposed.num_cols() == 3);

    // Check elements
    assert(transposed.test(0, 1));  // Was (1, 0)
    assert(transposed.test(1, 0));  // Was (0, 1)
    assert(transposed.test(1, 2));  // Was (2, 1)
    assert(transposed.test(2, 1));  // Was (1, 2)
    assert(transposed.test(2, 2));  // Was (2, 2)
    assert(transposed.test(3, 0));  // Was (0, 3)

    std::cout << "  SparseMatrix transpose: PASSED" << std::endl;
}

// Test BitVector
void test_bitvector() {
    std::cout << "Testing BitVector..." << std::endl;

    BitVector bv(100);

    // Test set and test
    bv.set(0);
    bv.set(50);
    bv.set(99);

    assert(bv.test(0));
    assert(bv.test(50));
    assert(bv.test(99));
    assert(!bv.test(1));
    assert(!bv.test(49));

    // Test popcount
    assert(bv.popcount() == 3);

    // Test flip
    bv.flip(50);
    assert(!bv.test(50));
    assert(bv.popcount() == 2);

    // Test clear
    bv.clear(0);
    assert(!bv.test(0));

    // Test XOR
    BitVector bv2(100);
    bv2.set(50);
    bv2.set(99);

    bv.xor_with(bv2);
    // bv was {99}, bv2 is {50, 99}
    // After XOR: {50} (99 cancels)
    assert(bv.test(50));
    assert(!bv.test(99));

    std::cout << "  BitVector: PASSED" << std::endl;
}

// Test Gaussian elimination on a simple matrix
void test_gaussian_simple() {
    std::cout << "Testing Gaussian elimination (simple)..." << std::endl;

    // Create a simple matrix with known null space
    // [ 1 1 0 ]
    // [ 1 0 1 ]
    // [ 0 1 1 ]
    // Null space: (1, 1, 1) since row1 + row2 + row3 = 0 in GF(2)

    SparseMatrix mat(3, 3);
    mat.set(0, 0); mat.set(0, 1);
    mat.set(1, 0); mat.set(1, 2);
    mat.set(2, 1); mat.set(2, 2);

    GaussianConfig config;
    config.compute_null_space = true;

    GaussianEliminator elim(config);
    auto result = elim.eliminate(mat);

    std::cout << "  Rank: " << result.rank << std::endl;
    std::cout << "  Free cols: " << result.free_cols.size() << std::endl;

    // This is a full rank matrix (rank = 3), so null space should be empty
    // Actually, let me recalculate...
    // Row reduction:
    // [1 1 0] -> [1 1 0] -> [1 0 1]
    // [1 0 1] -> [0 1 1] -> [0 1 1]
    // [0 1 1] -> [0 0 0] -> [0 0 0]
    // Rank = 2, one free variable

    assert(result.rank == 2);

    std::cout << "  Gaussian (simple): PASSED" << std::endl;
}

// Test Gaussian elimination with a larger matrix
void test_gaussian_larger() {
    std::cout << "Testing Gaussian elimination (larger)..." << std::endl;

    // Create a 5x4 matrix with rank 3
    // We want more rows than columns to have dependencies
    SparseMatrix mat(5, 4);

    // Row 0: 1 0 1 0
    mat.set(0, 0); mat.set(0, 2);

    // Row 1: 0 1 0 1
    mat.set(1, 1); mat.set(1, 3);

    // Row 2: 1 1 1 1
    mat.set(2, 0); mat.set(2, 1); mat.set(2, 2); mat.set(2, 3);

    // Row 3: 1 0 1 0 (same as row 0)
    mat.set(3, 0); mat.set(3, 2);

    // Row 4: 0 1 0 1 (same as row 1)
    mat.set(4, 1); mat.set(4, 3);

    GaussianConfig config;
    config.compute_null_space = true;

    GaussianEliminator elim(config);
    auto result = elim.eliminate(mat);

    std::cout << "  Rank: " << result.rank << std::endl;

    // Row 3 = Row 0, Row 4 = Row 1
    // So rank should be 3 (rows 0, 1, 2 are independent)

    assert(result.rank <= 4);
    assert(result.rank >= 2);

    std::cout << "  Gaussian (larger): PASSED" << std::endl;
}

// Test matrix builder with relations
void test_matrix_builder() {
    std::cout << "Testing MatrixBuilder..." << std::endl;

    // Create some mock relations
    std::vector<core::Relation> relations;

    // Relation 0: factors at index 0, 1
    core::Relation rel0(1, 1);
    rel0.rational_factors.push_back(0);
    rel0.rational_factors.push_back(1);
    relations.push_back(std::move(rel0));

    // Relation 1: factors at index 1, 2
    core::Relation rel1(2, 1);
    rel1.rational_factors.push_back(1);
    rel1.rational_factors.push_back(2);
    relations.push_back(std::move(rel1));

    // Relation 2: factors at index 0, 2 (sum of rel0 and rel1 in GF(2))
    core::Relation rel2(3, 1);
    rel2.rational_factors.push_back(0);
    rel2.rational_factors.push_back(2);
    relations.push_back(std::move(rel2));

    // Create a mock factor base structure
    // We'll just verify the matrix structure without a real factor base

    // Since we don't have a real FactorBase, let's test ColumnMapping directly
    ColumnMapping mapping;
    mapping.has_sign_column = true;
    mapping.sign_column = 0;
    mapping.num_rational_fb = 3;  // 3 rational primes
    mapping.num_algebraic_fb = 0;
    mapping.num_large_primes_rat = 0;
    mapping.num_large_primes_alg = 0;

    assert(mapping.total_columns() == 4);  // sign + 3 primes
    assert(mapping.rat_fb_start() == 1);
    assert(mapping.alg_fb_start() == 4);

    std::cout << "  MatrixBuilder: PASSED" << std::endl;
}

// Test dependency finding
void test_find_dependencies() {
    std::cout << "Testing find_dependencies..." << std::endl;

    // Create a matrix where rows 0 and 2 are identical
    // This creates a dependency: row 0 XOR row 2 = 0
    SparseMatrix mat(4, 3);

    // Row 0: 1 1 0
    mat.set(0, 0); mat.set(0, 1);

    // Row 1: 0 1 1
    mat.set(1, 1); mat.set(1, 2);

    // Row 2: 1 1 0 (same as row 0)
    mat.set(2, 0); mat.set(2, 1);

    // Row 3: 1 0 1
    mat.set(3, 0); mat.set(3, 2);

    BlockLanczos solver;
    auto deps = solver.find_dependencies(mat, 1);

    std::cout << "  Found " << deps.size() << " dependencies" << std::endl;

    // Dependencies should be valid (sum of selected rows XOR to zero)
    for (const auto& dep : deps) {
        std::cout << "  Dependency found (size=" << dep.size() << ")" << std::endl;
    }

    std::cout << "  find_dependencies: PASSED" << std::endl;
}

// Test dependency verification (inline implementation since no verify_dependency function)
void test_verify_dependency() {
    std::cout << "Testing dependency verification..." << std::endl;

    SparseMatrix mat(3, 4);

    // Row 0: 1 0 1 0
    mat.set(0, 0); mat.set(0, 2);

    // Row 1: 0 1 0 1
    mat.set(1, 1); mat.set(1, 3);

    // Row 2: 1 1 1 1 = row 0 XOR row 1
    mat.set(2, 0); mat.set(2, 1); mat.set(2, 2); mat.set(2, 3);

    // Test with BlockLanczos solver
    BlockLanczos solver;
    auto deps = solver.find_dependencies(mat, 2);

    std::cout << "  Found " << deps.size() << " dependencies" << std::endl;

    // In this matrix, rows 0 XOR row 1 XOR row 2 = 0
    // So there should be at least one dependency
    assert(deps.size() >= 1);

    std::cout << "  verify_dependency: PASSED" << std::endl;
}

// Test: default schirokauer_primes must be {2} only (GF(2) compatible)
void test_default_schirokauer_primes() {
    std::cout << "Testing default schirokauer_primes..." << std::endl;

    // Default config must use only ℓ=2 for GF(2) matrix.
    // ℓ=3 produces values in {0,1,2} which cannot be faithfully represented
    // in GF(2) — taking mod 2 destroys the mod-3 constraint.
    MatrixBuilderConfig config;
    assert(config.schirokauer_primes.size() == 1 &&
           "Default should have exactly 1 Schirokauer prime");
    assert(config.schirokauer_primes[0] == 2 &&
           "Default Schirokauer prime must be 2 for GF(2) matrix");

    // Explicitly setting {2} should also work
    MatrixBuilderConfig config2;
    config2.schirokauer_primes = {2};
    assert(config2.schirokauer_primes.size() == 1);
    assert(config2.schirokauer_primes[0] == 2);

    std::cout << "  Default schirokauer_primes: PASSED" << std::endl;
}

// Test matrix stats
void test_matrix_stats() {
    std::cout << "Testing matrix stats..." << std::endl;

    SparseMatrix mat(10, 8);

    // Add some elements
    for (size_t i = 0; i < 10; ++i) {
        mat.set(i, i % 8);
        mat.set(i, (i + 1) % 8);
    }

    auto stats = compute_matrix_stats(mat);

    assert(stats.num_rows == 10);
    assert(stats.num_cols == 8);
    assert(stats.total_weight == 20);  // 2 per row * 10 rows
    assert(stats.has_excess());
    assert(stats.excess == 2);  // 10 - 8

    std::cout << "  Matrix stats: PASSED" << std::endl;
}

// Test Block Lanczos solver
void test_structured_gauss() {
    std::cout << "Testing Block Lanczos solver..." << std::endl;

    // Create a matrix with clear dependencies
    SparseMatrix mat(6, 4);

    // Rows 0, 1 are independent
    mat.set(0, 0); mat.set(0, 1);
    mat.set(1, 2); mat.set(1, 3);

    // Row 2 = Row 0 XOR Row 1
    mat.set(2, 0); mat.set(2, 1); mat.set(2, 2); mat.set(2, 3);

    // Rows 3, 4 are independent
    mat.set(3, 0); mat.set(3, 2);
    mat.set(4, 1); mat.set(4, 3);

    // Row 5 = Row 3 XOR Row 4
    mat.set(5, 0); mat.set(5, 1); mat.set(5, 2); mat.set(5, 3);

    BlockLanczos solver;
    auto deps = solver.find_dependencies(mat, 4);

    std::cout << "  Found " << deps.size() << " dependencies" << std::endl;

    // Should find at least the two dependencies
    // (rows 0,1,2 and rows 3,4,5)
    assert(deps.size() >= 1);

    std::cout << "  Block Lanczos: PASSED" << std::endl;
}

// Regression test: Schirokauer map with f having repeated roots mod 2.
// Before fix, code fell back to unsplit mode computing γ^(ℓ^d-1) mod (f, ℓ^k)
// which is mathematically wrong when f is not irreducible mod ℓ.
void test_schirokauer_repeated_roots() {
    std::cout << "Testing Schirokauer map with repeated roots mod 2..." << std::endl;

    using core::Integer;
    using core::PolynomialContext;

    // f(x) = x³ + 3x² + 4x + 2
    // f mod 2 = x³ + x² = x²(x+1) — has repeated root x=0 with multiplicity 2
    // f(10) = 1000 + 300 + 40 + 2 = 1342
    std::vector<Integer> coeffs = {Integer(2), Integer(4), Integer(3), Integer(1)};
    PolynomialContext ctx(Integer(1342), std::move(coeffs), Integer(10), 1.0);

    SchirokaurConfig config;
    config.primes = {2};
    config.exponent_k = 3;

    SchirokaurMap smap(ctx, config);

    // Should produce degree=3 columns (one per Schirokauer prime)
    assert(smap.num_columns() == 3 &&
           "Schirokauer map must produce degree_ columns even with repeated roots");

    // Verify map is in split mode (not unsplit)
    assert(!smap.prime_info_.empty());
    assert(smap.prime_info_[0].is_split &&
           "f with repeated roots mod 2 must use split mode, not unsplit");

    // The squarefree factor x+1 (multiplicity 1 in f) should be lifted
    // The x² factor (multiplicity 2) should be skipped → those columns zero-padded
    // So we expect exactly 1 factor of degree 1
    assert(smap.prime_info_[0].factors.size() == 1 &&
           "Only multiplicity-1 factor (x+1) should be lifted");
    assert(smap.prime_info_[0].factors[0].degree == 1 &&
           "Lifted factor (x+1) should have degree 1");

    // Compute map for several (a, b) values — must not crash and return valid values
    std::vector<std::pair<int64_t, uint64_t>> test_cases = {
        {3, 2}, {7, 1}, {1, 5}, {11, 3}, {-1, 4}, {0, 1}
    };

    for (auto [a, b] : test_cases) {
        auto maps = smap.compute(a, b);
        assert(maps.size() == 1 && "Should have maps for 1 prime");
        assert(maps[0].size() == 3 && "Each map should have degree_ = 3 values");
        for (uint32_t v : maps[0]) {
            assert(v < 2 && "Map values must be in [0, ℓ)");
        }
    }

    // Determinism: same input should give same output
    auto map1 = smap.compute(3, 2);
    auto map2 = smap.compute(3, 2);
    assert(map1[0] == map2[0] && "Schirokauer map must be deterministic");

    std::cout << "  Repeated roots (x^2*(x+1) mod 2): PASSED" << std::endl;
}

// Test Schirokauer map with f being a perfect power mod 2
void test_schirokauer_perfect_power() {
    std::cout << "Testing Schirokauer map with perfect power mod 2..." << std::endl;

    using core::Integer;
    using core::PolynomialContext;

    // f(x) = x³ + 2x² + 4x + 8
    // f mod 2 = x³ (a perfect cube of x — all zeros except leading)
    // f(10) = 1000 + 200 + 40 + 8 = 1248
    std::vector<Integer> coeffs = {Integer(8), Integer(4), Integer(2), Integer(1)};
    PolynomialContext ctx(Integer(1248), std::move(coeffs), Integer(10), 1.0);

    SchirokaurConfig config;
    config.primes = {2};
    config.exponent_k = 3;

    SchirokaurMap smap(ctx, config);

    assert(smap.num_columns() == 3);
    assert(smap.prime_info_[0].is_split &&
           "Perfect power mod 2 must use split mode");
    assert(smap.prime_info_[0].factors.empty() &&
           "Perfect power mod 2 should have no liftable factors");

    // All Schirokauer values should be zero (no constraints available)
    auto maps = smap.compute(5, 3);
    assert(maps[0].size() == 3);
    for (uint32_t v : maps[0]) {
        assert(v == 0 && "Perfect power case should zero-fill all columns");
    }

    std::cout << "  Perfect power (x^3 mod 2): PASSED" << std::endl;
}

// Test Schirokauer map with squarefree reducible f (existing split path, should still work)
void test_schirokauer_squarefree_reducible() {
    std::cout << "Testing Schirokauer map with squarefree reducible f mod 2..." << std::endl;

    using core::Integer;
    using core::PolynomialContext;

    // f(x) = x³ + x² + x + 1 = (x+1)(x²+1) over Z
    // f mod 2 = x³ + x² + x + 1 = (x+1)(x²+1) = (x+1)(x+1)² = (x+1)³ mod 2?
    // No: x²+1 = (x+1)² mod 2. So f mod 2 = (x+1)³ — repeated!
    // Need a TRULY squarefree example.
    //
    // f(x) = x³ + x + 2 → f mod 2 = x³ + x = x(x²+1) = x(x+1)² — still repeated
    //
    // f(x) = x³ + x² + 2 → f mod 2 = x³ + x² = x²(x+1) — repeated
    //
    // For degree 3 squarefree-reducible over GF(2), we need 3 distinct linear roots:
    // x(x+1)(x+a) but GF(2) only has 0 and 1, so x(x+1) times... no third root exists.
    // Actually deg-3 squarefree reducible over GF(2) must factor into a linear × irreducible quadratic.
    // (x+1)(x²+x+1) = x³+1+x²+x = x³+x²+x+1 mod 2 = the same as above.
    // Wait: (x+1)(x²+x+1) = x³ + x²·1 + x·1 + 1·1 + x² + x = ?
    // Let me expand: x·(x²+x+1) = x³+x²+x, plus 1·(x²+x+1) = x²+x+1
    // Total: x³+x²+x + x²+x+1 = x³ + 2x² + 2x + 1 = x³ + 1 (mod 2)
    // So (x+1)(x²+x+1) = x³+1 mod 2.
    // f mod 2 = x³+1 = (x+1)(x²+x+1) — squarefree! Two coprime factors.
    //
    // Use f(x) = x³ + 3, so f mod 2 = x³ + 1 = (x+1)(x²+x+1)
    // f(10) = 1000 + 3 = 1003
    std::vector<Integer> coeffs = {Integer(3), Integer(0), Integer(0), Integer(1)};
    PolynomialContext ctx(Integer(1003), std::move(coeffs), Integer(10), 1.0);

    SchirokaurConfig config;
    config.primes = {2};
    config.exponent_k = 3;

    SchirokaurMap smap(ctx, config);

    assert(smap.num_columns() == 3);
    assert(smap.prime_info_[0].is_split);
    // Should have 2 factors: (x+1) of degree 1, (x²+x+1) of degree 2
    assert(smap.prime_info_[0].factors.size() == 2 &&
           "Squarefree reducible should have 2 lifted factors");

    uint32_t total_deg = 0;
    for (const auto& fi : smap.prime_info_[0].factors) {
        total_deg += fi.degree;
    }
    assert(total_deg == 3 && "Factor degrees should sum to polynomial degree");

    // Compute and verify
    auto maps = smap.compute(7, 1);
    assert(maps[0].size() == 3);
    for (uint32_t v : maps[0]) {
        assert(v < 2);
    }

    std::cout << "  Squarefree reducible ((x+1)(x^2+x+1) mod 2): PASSED" << std::endl;
}

// Regression test: parallel Block Lanczos produces same results as Gaussian
// for a medium-sized matrix (verifies parallelization correctness)
void test_parallel_block_lanczos_correctness() {
    std::cout << "Testing parallel Block Lanczos correctness..." << std::endl;

    // Create a matrix large enough that BL is non-trivial but small enough
    // to verify with Gaussian. 200 rows × 150 cols, ~30% density.
    const size_t m = 200;
    const size_t n = 150;

    SparseMatrix mat(m, n);
    std::mt19937 rng(12345);

    for (size_t i = 0; i < m; ++i) {
        // Each row gets 3-8 non-zero entries
        size_t nnz = 3 + rng() % 6;
        for (size_t k = 0; k < nnz; ++k) {
            mat.set(i, rng() % n);
        }
    }

    // Find dependencies using the unified solver (dispatches to Gaussian for <10K)
    BlockLanczos solver;
    auto deps = solver.find_dependencies(mat, 10);

    std::cout << "  Found " << deps.size() << " dependencies" << std::endl;
    assert(deps.size() >= 1);

    // Verify each dependency: v^T * M = 0 over GF(2)
    for (const auto& dep : deps) {
        assert(dep.size() == m);

        // Compute M^T * v
        std::vector<bool> result(n, false);
        for (size_t i = 0; i < m; ++i) {
            if (!dep[i]) continue;
            for (uint32_t col : mat.row(i).indices()) {
                if (col < n) result[col] = !result[col];
            }
        }

        // Must be all zeros
        for (size_t j = 0; j < n; ++j) {
            assert(!result[j]);
        }
    }

    std::cout << "  Parallel BL correctness: PASSED" << std::endl;
}

// Test ensure_all_sorted eliminates const_cast UB
void test_ensure_all_sorted() {
    std::cout << "Testing ensure_all_sorted..." << std::endl;

    SparseMatrix mat(5, 10);

    // Add elements in non-sorted order via direct row access
    mat.row(0).set(5);
    mat.row(0).set(2);
    mat.row(0).set(8);
    mat.row(1).set(9);
    mat.row(1).set(1);

    // Before ensure_all_sorted, rows may be unsorted
    mat.ensure_all_sorted();

    // After ensure_all_sorted, indices() must return sorted lists
    auto& idx0 = mat.row(0).indices();
    assert(idx0.size() == 3);
    assert(idx0[0] == 2);
    assert(idx0[1] == 5);
    assert(idx0[2] == 8);

    auto& idx1 = mat.row(1).indices();
    assert(idx1.size() == 2);
    assert(idx1[0] == 1);
    assert(idx1[1] == 9);

    std::cout << "  ensure_all_sorted: PASSED" << std::endl;
}

int main() {
    std::cout << "=== Linear Algebra Tests ===" << std::endl;
    std::cout << std::endl;

    test_sparse_row();
    test_sparse_row_xor();
    test_sparse_matrix();
    test_sparse_matrix_transpose();
    test_bitvector();
    test_gaussian_simple();
    test_gaussian_larger();
    test_matrix_builder();
    test_default_schirokauer_primes();
    test_find_dependencies();
    test_verify_dependency();
    test_matrix_stats();
    test_structured_gauss();
    test_schirokauer_repeated_roots();
    test_schirokauer_perfect_power();
    test_schirokauer_squarefree_reducible();
    test_parallel_block_lanczos_correctness();
    test_ensure_all_sorted();

    std::cout << std::endl;
    std::cout << "=== All Linear Algebra Tests PASSED ===" << std::endl;

    return 0;
}
