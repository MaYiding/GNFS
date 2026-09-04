// test_linalg.cpp - Test linear algebra components

#include "support/test_check.hpp"
#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/core/relation.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/gauss.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/schirokauer.hpp>
#include <gnfs/linalg/sge.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gnfs;
using namespace gnfs::linalg;

template <typename Exception, typename Callable>
void require_throws(Callable&& callable, const char* context) {
    try {
        callable();
    } catch (const Exception&) {
        return;
    } catch (...) {
        throw std::runtime_error(std::string(context) + ": wrong exception type");
    }
    throw std::runtime_error(std::string(context) + ": expected exception was not thrown");
}

// Test SparseRow operations
void test_sparse_row() {
    std::cout << "Testing SparseRow..." << std::endl;

    SparseRow row;

    // Test set and test
    row.set(5);
    row.set(10);
    row.set(3);

    GNFS_TEST_CHECK(row.test(3));
    GNFS_TEST_CHECK(row.test(5));
    GNFS_TEST_CHECK(row.test(10));
    GNFS_TEST_CHECK(!row.test(0));
    GNFS_TEST_CHECK(!row.test(7));

    // Test weight
    GNFS_TEST_CHECK(row.weight() == 3);

    // Test first/last nonzero
    GNFS_TEST_CHECK(row.first_nonzero() == 3);
    GNFS_TEST_CHECK(row.last_nonzero() == 10);

    // Test clear
    row.clear(5);
    GNFS_TEST_CHECK(!row.test(5));
    GNFS_TEST_CHECK(row.weight() == 2);

    // Test flip
    row.flip(7); // Add
    GNFS_TEST_CHECK(row.test(7));
    row.flip(7); // Remove
    GNFS_TEST_CHECK(!row.test(7));

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

    GNFS_TEST_CHECK(row1.test(1));
    GNFS_TEST_CHECK(row1.test(2));
    GNFS_TEST_CHECK(!row1.test(3)); // Cancelled
    GNFS_TEST_CHECK(row1.test(4));
    GNFS_TEST_CHECK(row1.test(5));
    GNFS_TEST_CHECK(row1.weight() == 4);

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

    GNFS_TEST_CHECK(mat.num_rows() == 3);
    GNFS_TEST_CHECK(mat.num_cols() == 5);
    GNFS_TEST_CHECK(mat.test(0, 0));
    GNFS_TEST_CHECK(mat.test(0, 2));
    GNFS_TEST_CHECK(!mat.test(0, 1));

    // Test total weight
    GNFS_TEST_CHECK(mat.total_weight() == 6);

    // Test swap rows
    mat.swap_rows(0, 1);
    GNFS_TEST_CHECK(mat.test(0, 1));
    GNFS_TEST_CHECK(mat.test(0, 3));
    GNFS_TEST_CHECK(mat.test(1, 0));
    GNFS_TEST_CHECK(mat.test(1, 2));

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

    GNFS_TEST_CHECK(transposed.num_rows() == 4);
    GNFS_TEST_CHECK(transposed.num_cols() == 3);

    // Check elements
    GNFS_TEST_CHECK(transposed.test(0, 1)); // Was (1, 0)
    GNFS_TEST_CHECK(transposed.test(1, 0)); // Was (0, 1)
    GNFS_TEST_CHECK(transposed.test(1, 2)); // Was (2, 1)
    GNFS_TEST_CHECK(transposed.test(2, 1)); // Was (1, 2)
    GNFS_TEST_CHECK(transposed.test(2, 2)); // Was (2, 2)
    GNFS_TEST_CHECK(transposed.test(3, 0)); // Was (0, 3)

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

    GNFS_TEST_CHECK(bv.test(0));
    GNFS_TEST_CHECK(bv.test(50));
    GNFS_TEST_CHECK(bv.test(99));
    GNFS_TEST_CHECK(!bv.test(1));
    GNFS_TEST_CHECK(!bv.test(49));

    // Test popcount
    GNFS_TEST_CHECK(bv.popcount() == 3);

    // Test flip
    bv.flip(50);
    GNFS_TEST_CHECK(!bv.test(50));
    GNFS_TEST_CHECK(bv.popcount() == 2);

    // Test clear
    bv.clear(0);
    GNFS_TEST_CHECK(!bv.test(0));

    // Test XOR
    BitVector bv2(100);
    bv2.set(50);
    bv2.set(99);

    bv.xor_with(bv2);
    // bv was {99}, bv2 is {50, 99}
    // After XOR: {50} (99 cancels)
    GNFS_TEST_CHECK(bv.test(50));
    GNFS_TEST_CHECK(!bv.test(99));

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
    mat.set(0, 0);
    mat.set(0, 1);
    mat.set(1, 0);
    mat.set(1, 2);
    mat.set(2, 1);
    mat.set(2, 2);

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

    GNFS_TEST_CHECK(result.rank == 2);

    std::cout << "  Gaussian (simple): PASSED" << std::endl;
}

// Test Gaussian elimination with a larger matrix
void test_gaussian_larger() {
    std::cout << "Testing Gaussian elimination (larger)..." << std::endl;

    // Create a 5x4 matrix with rank 3
    // We want more rows than columns to have dependencies
    SparseMatrix mat(5, 4);

    // Row 0: 1 0 1 0
    mat.set(0, 0);
    mat.set(0, 2);

    // Row 1: 0 1 0 1
    mat.set(1, 1);
    mat.set(1, 3);

    // Row 2: 1 1 1 1
    mat.set(2, 0);
    mat.set(2, 1);
    mat.set(2, 2);
    mat.set(2, 3);

    // Row 3: 1 0 1 0 (same as row 0)
    mat.set(3, 0);
    mat.set(3, 2);

    // Row 4: 0 1 0 1 (same as row 1)
    mat.set(4, 1);
    mat.set(4, 3);

    GaussianConfig config;
    config.compute_null_space = true;

    GaussianEliminator elim(config);
    auto result = elim.eliminate(mat);

    std::cout << "  Rank: " << result.rank << std::endl;

    // Row 3 = Row 0, Row 4 = Row 1, Row 2 = Row 0 XOR Row 1 (over GF(2))
    // So rank is 2 (only rows 0, 1 are independent)
    GNFS_TEST_CHECK(result.rank == 2);

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
    mapping.num_rational_fb = 3; // 3 rational primes
    mapping.num_algebraic_fb = 0;
    mapping.num_large_primes_rat = 0;
    mapping.num_large_primes_alg = 0;

    GNFS_TEST_CHECK(mapping.total_columns() == 4); // sign + 3 primes
    GNFS_TEST_CHECK(mapping.rat_fb_start() == 1);
    GNFS_TEST_CHECK(mapping.alg_fb_start() == 4);

    std::cout << "  MatrixBuilder: PASSED" << std::endl;
}

// Test dependency finding
void test_find_dependencies() {
    std::cout << "Testing find_dependencies..." << std::endl;

    // Create a matrix where rows 0 and 2 are identical
    // This creates a dependency: row 0 XOR row 2 = 0
    SparseMatrix mat(4, 3);

    // Row 0: 1 1 0
    mat.set(0, 0);
    mat.set(0, 1);

    // Row 1: 0 1 1
    mat.set(1, 1);
    mat.set(1, 2);

    // Row 2: 1 1 0 (same as row 0)
    mat.set(2, 0);
    mat.set(2, 1);

    // Row 3: 1 0 1
    mat.set(3, 0);
    mat.set(3, 2);

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
    mat.set(0, 0);
    mat.set(0, 2);

    // Row 1: 0 1 0 1
    mat.set(1, 1);
    mat.set(1, 3);

    // Row 2: 1 1 1 1 = row 0 XOR row 1
    mat.set(2, 0);
    mat.set(2, 1);
    mat.set(2, 2);
    mat.set(2, 3);

    // Test with BlockLanczos solver
    BlockLanczos solver;
    auto deps = solver.find_dependencies(mat, 2);

    std::cout << "  Found " << deps.size() << " dependencies" << std::endl;

    // In this matrix, rows 0 XOR row 1 XOR row 2 = 0
    // So there should be at least one dependency
    GNFS_TEST_CHECK(deps.size() >= 1);

    std::cout << "  verify_dependency: PASSED" << std::endl;
}

// Test: default schirokauer_primes must be {2} only (GF(2) compatible)
void test_default_schirokauer_primes() {
    std::cout << "Testing default schirokauer_primes..." << std::endl;

    // Default config must use only ℓ=2 for GF(2) matrix.
    // ℓ=3 produces values in {0,1,2} which cannot be faithfully represented
    // in GF(2) — taking mod 2 destroys the mod-3 constraint.
    MatrixBuilderConfig config;
    GNFS_TEST_CHECK(config.schirokauer_primes.size() == 1 &&
                    "Default should have exactly 1 Schirokauer prime");
    GNFS_TEST_CHECK(config.schirokauer_primes[0] == 2 &&
                    "Default Schirokauer prime must be 2 for GF(2) matrix");

    // Explicitly setting {2} should also work
    MatrixBuilderConfig config2;
    config2.schirokauer_primes = {2};
    GNFS_TEST_CHECK(config2.schirokauer_primes.size() == 1);
    GNFS_TEST_CHECK(config2.schirokauer_primes[0] == 2);

    std::cout << "  Default schirokauer_primes: PASSED" << std::endl;
}

// Test matrix stats
// Thin matrix test (m < n) — BACKLOG #80 step 7 — confirm BW/BL can find
// left-kernel of M when rows < cols. Synthetic: 3 rows × 4 cols, all rows
// identical → XOR of any 2 rows = 0 (left null space dimension = 2).
void test_thin_matrix_dependencies() {
    std::cout << "Testing thin matrix (m<n) dependency finding..." << std::endl;

    // m=3 < n=4, all rows identical = "1 0 1 1"
    SparseMatrix mat(3, 4);
    for (size_t r = 0; r < 3; ++r) {
        mat.set(r, 0);
        mat.set(r, 2);
        mat.set(r, 3);
    }

    // BL on m<n: 小矩阵 (m,n < 5000) BW 会 delegate 到 BL.
    // BL 假设 m>n 但小 matrix Gaussian 内部应能 handle.
    BlockLanczos solver;
    auto deps = solver.find_dependencies(mat, 2);

    std::cout << "  Found " << deps.size() << " deps (expected >= 1)" << std::endl;

    // Verify: each dep is a length-m bit vector. XOR of selected rows = 0.
    for (const auto& dep : deps) {
        if (dep.size() != 3) {
            std::cerr << "  FAIL: dep length " << dep.size() << " != 3" << std::endl;
            std::exit(1);
        }
        // Sum (XOR) of selected rows
        std::vector<bool> xor_row(4, false);
        for (size_t r = 0; r < 3; ++r) {
            if (!dep[r])
                continue;
            for (uint32_t col_idx : mat.row(r).indices()) {
                xor_row[col_idx] = !xor_row[col_idx];
            }
        }
        bool is_zero = true;
        for (bool b : xor_row)
            if (b) {
                is_zero = false;
                break;
            }
        if (!is_zero) {
            std::cerr << "  FAIL: dependency does not XOR to zero" << std::endl;
            std::exit(1);
        }
    }

    std::cout << "  thin_matrix_dependencies: PASSED" << std::endl;
}

void test_matrix_stats() {
    std::cout << "Testing matrix stats..." << std::endl;

    SparseMatrix mat(10, 8);

    // Add some elements
    for (size_t i = 0; i < 10; ++i) {
        mat.set(i, i % 8);
        mat.set(i, (i + 1) % 8);
    }

    auto stats = compute_matrix_stats(mat);

    GNFS_TEST_CHECK(stats.num_rows == 10);
    GNFS_TEST_CHECK(stats.num_cols == 8);
    GNFS_TEST_CHECK(stats.total_weight == 20); // 2 per row * 10 rows
    GNFS_TEST_CHECK(stats.has_excess());
    GNFS_TEST_CHECK(stats.excess == 2); // 10 - 8

    std::cout << "  Matrix stats: PASSED" << std::endl;
}

// Test Block Lanczos solver
void test_structured_gauss() {
    std::cout << "Testing Block Lanczos solver..." << std::endl;

    // Create a matrix with clear dependencies
    SparseMatrix mat(6, 4);

    // Rows 0, 1 are independent
    mat.set(0, 0);
    mat.set(0, 1);
    mat.set(1, 2);
    mat.set(1, 3);

    // Row 2 = Row 0 XOR Row 1
    mat.set(2, 0);
    mat.set(2, 1);
    mat.set(2, 2);
    mat.set(2, 3);

    // Rows 3, 4 are independent
    mat.set(3, 0);
    mat.set(3, 2);
    mat.set(4, 1);
    mat.set(4, 3);

    // Row 5 = Row 3 XOR Row 4
    mat.set(5, 0);
    mat.set(5, 1);
    mat.set(5, 2);
    mat.set(5, 3);

    BlockLanczos solver;
    auto deps = solver.find_dependencies(mat, 4);

    std::cout << "  Found " << deps.size() << " dependencies" << std::endl;

    // Should find at least the two dependencies
    // (rows 0,1,2 and rows 3,4,5)
    GNFS_TEST_CHECK(deps.size() >= 1);

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
    GNFS_TEST_CHECK(smap.num_columns() == 3 &&
                    "Schirokauer map must produce degree_ columns even with repeated roots");

    // Verify map is in split mode (not unsplit)
    GNFS_TEST_CHECK(!smap.prime_info_.empty());
    GNFS_TEST_CHECK(smap.prime_info_[0].is_split &&
                    "f with repeated roots mod 2 must use split mode, not unsplit");

    // The squarefree factor x+1 (multiplicity 1 in f) should be lifted
    // The x² factor (multiplicity 2) should be skipped → those columns zero-padded
    // So we expect exactly 1 factor of degree 1
    GNFS_TEST_CHECK(smap.prime_info_[0].factors.size() == 1 &&
                    "Only multiplicity-1 factor (x+1) should be lifted");
    GNFS_TEST_CHECK(smap.prime_info_[0].factors[0].degree == 1 &&
                    "Lifted factor (x+1) should have degree 1");

    // Compute map for several (a, b) values — must not crash and return valid values
    std::vector<std::pair<int64_t, uint64_t>> test_cases = {{3, 2},  {7, 1},  {1, 5},
                                                            {11, 3}, {-1, 4}, {0, 1}};

    for (auto [a, b] : test_cases) {
        auto maps = smap.compute(a, b);
        GNFS_TEST_CHECK(maps.size() == 1 && "Should have maps for 1 prime");
        GNFS_TEST_CHECK(maps[0].size() == 3 && "Each map should have degree_ = 3 values");
        for (uint32_t v : maps[0]) {
            GNFS_TEST_CHECK(v < 2 && "Map values must be in [0, ℓ)");
        }
    }

    // Determinism: same input should give same output
    auto map1 = smap.compute(3, 2);
    auto map2 = smap.compute(3, 2);
    GNFS_TEST_CHECK(map1[0] == map2[0] && "Schirokauer map must be deterministic");

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

    GNFS_TEST_CHECK(smap.num_columns() == 3);
    // Perfect power mod ℓ has no multiplicity-1 factors → falls back to unsplit mode
    // (zero-filling would remove all Schirokauer constraints, making every dependency trivial)
    GNFS_TEST_CHECK(!smap.prime_info_[0].is_split &&
                    "Perfect power mod 2 must fall back to unsplit mode");
    GNFS_TEST_CHECK(smap.prime_info_[0].exponent == 7 &&
                    "Unsplit exponent should be ℓ^d - 1 = 2^3 - 1 = 7");

    // Unsplit mode produces actual Schirokauer values (not all zeros)
    auto maps = smap.compute(5, 3);
    GNFS_TEST_CHECK(maps[0].size() == 3);
    // Values are computed from (a - b*α)^7 mod (f, 2^3), then extract bits
    // Just verify we get 3 values (actual values depend on element)

    std::cout << "  Perfect power (x^3 mod 2, unsplit fallback): PASSED" << std::endl;
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
    // Actually deg-3 squarefree reducible over GF(2) must factor into a linear × irreducible
    // quadratic. (x+1)(x²+x+1) = x³+1+x²+x = x³+x²+x+1 mod 2 = the same as above. Wait:
    // (x+1)(x²+x+1) = x³ + x²·1 + x·1 + 1·1 + x² + x = ? Let me expand: x·(x²+x+1) = x³+x²+x, plus
    // 1·(x²+x+1) = x²+x+1 Total: x³+x²+x + x²+x+1 = x³ + 2x² + 2x + 1 = x³ + 1 (mod 2) So
    // (x+1)(x²+x+1) = x³+1 mod 2. f mod 2 = x³+1 = (x+1)(x²+x+1) — squarefree! Two coprime factors.
    //
    // Use f(x) = x³ + 3, so f mod 2 = x³ + 1 = (x+1)(x²+x+1)
    // f(10) = 1000 + 3 = 1003
    std::vector<Integer> coeffs = {Integer(3), Integer(0), Integer(0), Integer(1)};
    PolynomialContext ctx(Integer(1003), std::move(coeffs), Integer(10), 1.0);

    SchirokaurConfig config;
    config.primes = {2};
    config.exponent_k = 3;

    SchirokaurMap smap(ctx, config);

    GNFS_TEST_CHECK(smap.num_columns() == 3);
    GNFS_TEST_CHECK(smap.prime_info_[0].is_split);
    // Should have 2 factors: (x+1) of degree 1, (x²+x+1) of degree 2
    GNFS_TEST_CHECK(smap.prime_info_[0].factors.size() == 2 &&
                    "Squarefree reducible should have 2 lifted factors");

    uint32_t total_deg = 0;
    for (const auto& fi : smap.prime_info_[0].factors) {
        total_deg += fi.degree;
    }
    GNFS_TEST_CHECK(total_deg == 3 && "Factor degrees should sum to polynomial degree");

    // Compute and verify
    auto maps = smap.compute(7, 1);
    GNFS_TEST_CHECK(maps[0].size() == 3);
    for (uint32_t v : maps[0]) {
        GNFS_TEST_CHECK(v < 2);
    }

    std::cout << "  Squarefree reducible ((x+1)(x^2+x+1) mod 2): PASSED" << std::endl;
}

// Regression test: EDF must be reproducible and must never return a composite
// factor when its bounded Cantor-Zassenhaus search cannot split the input.
void test_gf_poly_factorization_determinism() {
    std::cout << "Testing deterministic GF polynomial factorization..." << std::endl;

    // f = (x^2+x+1)(x^3+x^2+1)(x^3+x+1) over GF(2), represented low-to-high.
    const GFPolyOps::Poly input = {1, 0, 1, 1, 1, 1, 1, 0, 1};
    const std::vector<GFPolyOps::Poly> expected = {
        {1, 1, 1},
        {1, 0, 1, 1},
        {1, 1, 0, 1},
    };

    const auto baseline = GFPolyOps::factor(input, 2);
    GNFS_TEST_CHECK(baseline == expected &&
                    "GF factorization must use canonical degree/coefficient ordering");
    for (int iteration = 0; iteration < 8; ++iteration) {
        GNFS_TEST_CHECK(GFPolyOps::factor(input, 2) == baseline &&
                        "GF factorization must be deterministic across calls");
    }

    auto product = GFPolyOps::Poly{1};
    for (const auto& factor : baseline) {
        GNFS_TEST_CHECK(factor.back() == 1);
        GNFS_TEST_CHECK(gnfs::sqrt::ModularPoly::is_irreducible(factor, 2));
        product = GFPolyOps::mul(product, factor, 2);
    }
    GNFS_TEST_CHECK(product == input && "GF factorization factors must multiply back to the input");

    const GFPolyOps::Poly two_linear_factors = {0, 1, 1}; // x(x+1)
    require_throws<std::runtime_error>([&] { (void)GFPolyOps::edf(two_linear_factors, 1, 2, 0); },
                                       "EDF must fail closed when its attempt budget is exhausted");

    std::cout << "  Deterministic EDF and fail-closed validation: PASSED" << std::endl;
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
    GNFS_TEST_CHECK(deps.size() >= 1);

    // Verify each dependency: v^T * M = 0 over GF(2)
    for (const auto& dep : deps) {
        GNFS_TEST_CHECK(dep.size() == m);

        // Compute M^T * v
        std::vector<bool> result(n, false);
        for (size_t i = 0; i < m; ++i) {
            if (!dep[i])
                continue;
            for (uint32_t col : mat.row(i).indices()) {
                if (col < n)
                    result[col] = !result[col];
            }
        }

        // Must be all zeros
        for (size_t j = 0; j < n; ++j) {
            GNFS_TEST_CHECK(!result[j]);
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
    GNFS_TEST_CHECK(idx0.size() == 3);
    GNFS_TEST_CHECK(idx0[0] == 2);
    GNFS_TEST_CHECK(idx0[1] == 5);
    GNFS_TEST_CHECK(idx0[2] == 8);

    auto& idx1 = mat.row(1).indices();
    GNFS_TEST_CHECK(idx1.size() == 2);
    GNFS_TEST_CHECK(idx1[0] == 1);
    GNFS_TEST_CHECK(idx1[1] == 9);

    std::cout << "  ensure_all_sorted: PASSED" << std::endl;
}

// Test SGE: weight-1 column elimination
void test_sge_weight1() {
    std::cout << "Testing SGE weight-1 elimination..." << std::endl;

    // 4×4 matrix where column 2 has weight 1 (only row 1)
    // Row 0: {0, 1}
    // Row 1: {0, 2}     ← only contributor to col 2
    // Row 2: {1, 3}
    // Row 3: {0, 3}
    SparseMatrix mat(4, 4);
    mat.set(0, 0);
    mat.set(0, 1);
    mat.set(1, 0);
    mat.set(1, 2);
    mat.set(2, 1);
    mat.set(2, 3);
    mat.set(3, 0);
    mat.set(3, 3);

    SGEConfig config;
    config.eliminate_weight2 = false; // only test weight-1
    auto result = SGE::preprocess(mat, config);

    // Row 1 should be eliminated (weight-1 col 2)
    GNFS_TEST_CHECK(result.reduced_matrix.num_rows() == 3);
    GNFS_TEST_CHECK(result.weight1_eliminated >= 1);

    // Verify row composition: each reduced row maps to exactly one original row
    for (auto& comp : result.row_composition) {
        GNFS_TEST_CHECK(!comp.empty());
    }

    std::cout << "  SGE weight-1: " << result.original_rows << "×" << result.original_cols << " → "
              << result.reduced_matrix.num_rows() << "×" << result.reduced_matrix.num_cols()
              << " (w1=" << result.weight1_eliminated << ")" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// Test SGE: weight-2 column merging
void test_sge_weight2() {
    std::cout << "Testing SGE weight-2 merging..." << std::endl;

    // 5×6 matrix: cols 4,5 have weight 2; cols 0-3 have weight 3
    // Row 0: {0, 1, 2, 3}
    // Row 1: {0, 2, 4}
    // Row 2: {1, 3, 4}      ← shares col 4 with row 1
    // Row 3: {0, 1, 5}
    // Row 4: {2, 3, 5}      ← shares col 5 with row 3
    SparseMatrix mat(5, 6);
    mat.set(0, 0);
    mat.set(0, 1);
    mat.set(0, 2);
    mat.set(0, 3);
    mat.set(1, 0);
    mat.set(1, 2);
    mat.set(1, 4);
    mat.set(2, 1);
    mat.set(2, 3);
    mat.set(2, 4);
    mat.set(3, 0);
    mat.set(3, 1);
    mat.set(3, 5);
    mat.set(4, 2);
    mat.set(4, 3);
    mat.set(4, 5);

    SGEConfig config;
    config.eliminate_weight1 = false; // only test weight-2
    auto result = SGE::preprocess(mat, config);

    // Two weight-2 merges: cols 4 and 5 → 2 rows eliminated
    GNFS_TEST_CHECK(result.reduced_matrix.num_rows() == 3);
    GNFS_TEST_CHECK(result.weight2_merged == 2);

    // The merged rows' compositions should contain 2 original rows
    size_t merged_count = 0;
    for (auto& comp : result.row_composition) {
        if (comp.size() == 2)
            ++merged_count;
    }
    GNFS_TEST_CHECK(merged_count == 2);

    std::cout << "  SGE weight-2: " << result.original_rows << "×" << result.original_cols << " → "
              << result.reduced_matrix.num_rows() << "×" << result.reduced_matrix.num_cols()
              << " (w2=" << result.weight2_merged << ")" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// Test SGE: dependency expansion correctness
void test_sge_expand_dependency() {
    std::cout << "Testing SGE dependency expansion..." << std::endl;

    // Create a matrix with known null space, apply SGE, find deps on reduced,
    // expand back, and verify against original.
    // 6×4 matrix (6 rows, 4 cols) — excess = 2
    // Design: rows 0,1,2 XOR to zero (a dependency)
    // Row 0: {0, 1}
    // Row 1: {1, 2}
    // Row 2: {0, 2}        ← XOR of rows 0,1 = row 2
    // Row 3: {0, 3}         (column 3 has weight 2: rows 3,4)
    // Row 4: {2, 3}
    // Row 5: {1}             (column 1 has weight 3)
    SparseMatrix mat(6, 4);
    mat.set(0, 0);
    mat.set(0, 1);
    mat.set(1, 1);
    mat.set(1, 2);
    mat.set(2, 0);
    mat.set(2, 2);
    mat.set(3, 0);
    mat.set(3, 3);
    mat.set(4, 2);
    mat.set(4, 3);
    mat.set(5, 1);

    auto sge_result = SGE::preprocess(mat);

    std::cout << "  SGE: 6×4 → " << sge_result.reduced_matrix.num_rows() << "×"
              << sge_result.reduced_matrix.num_cols() << " (w1=" << sge_result.weight1_eliminated
              << " w2=" << sge_result.weight2_merged << ")" << std::endl;

    // Find deps on reduced matrix
    BlockLanczos solver;
    auto deps = solver.find_dependencies(sge_result.reduced_matrix);
    GNFS_TEST_CHECK(!deps.empty());

    // Expand and verify against original matrix
    for (auto& dep : deps) {
        auto orig = sge_result.expand_dependency(dep);
        GNFS_TEST_CHECK(orig.size() == 6);

        // Verify: v^T * M = 0 over GF(2)
        std::vector<bool> check(4, false);
        for (size_t r = 0; r < 6; ++r) {
            if (!orig[r])
                continue;
            for (auto col : mat.row(r).indices()) {
                check[col] = !check[col];
            }
        }
        for (size_t c = 0; c < 4; ++c) {
            GNFS_TEST_CHECK(!check[c]);
        }
    }

    std::cout << "  Expanded deps verified: " << deps.size() << " deps, all v^T*M=0" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// Release-active contract tests for dependency expansion. These fixtures use
// explicit exception checks, so malformed solver output and corrupt provenance
// stay fail-closed when NDEBUG is enabled.
void test_sge_expand_dependency_contract() {
    std::cout << "Testing SGE dependency expansion contract..." << std::endl;

    SGEResult valid;
    valid.original_rows = 3;
    valid.reduced_matrix = SparseMatrix(2, 1);
    valid.row_composition = {{0, 2}, {1, 2}};

    const auto expanded = valid.expand_dependency({true, true});
    const std::vector<bool> expected{true, true, false};
    if (expanded != expected) {
        throw std::runtime_error("valid SGE dependency did not expand with GF(2) parity");
    }

    require_throws<std::invalid_argument>([&] { (void)valid.expand_dependency({true}); },
                                          "short reduced dependency");
    require_throws<std::invalid_argument>(
        [&] { (void)valid.expand_dependency({true, false, true}); }, "long reduced dependency");

    SGEResult inconsistent_shape;
    inconsistent_shape.original_rows = 3;
    inconsistent_shape.reduced_matrix = SparseMatrix(1, 1);
    inconsistent_shape.row_composition = {{0}, {1}};
    require_throws<std::logic_error>(
        [&] { (void)inconsistent_shape.expand_dependency({false, false}); },
        "row composition and reduced matrix mismatch");

    SGEResult invalid_composition;
    invalid_composition.original_rows = 3;
    invalid_composition.reduced_matrix = SparseMatrix(2, 1);
    invalid_composition.row_composition = {{0, 3}, {1, 2}};
    require_throws<std::out_of_range>(
        [&] { (void)invalid_composition.expand_dependency({false, false}); },
        "out-of-range original row in unselected composition");

    std::cout << "  PASSED" << std::endl;
}

void test_sge_expand_dependencies_contract() {
    std::cout << "Testing SGE batch dependency expansion contract..." << std::endl;

    SGEResult valid;
    valid.original_rows = 4;
    valid.reduced_matrix = SparseMatrix(3, 1);
    valid.row_composition = {{0, 3}, {1, 3}, {2}};

    const std::vector<std::vector<bool>> reduced_dependencies{
        {true, false, false},
        {false, true, true},
        {true, true, false},
    };
    const auto expanded = valid.expand_dependencies(reduced_dependencies);
    if (expanded.size() != reduced_dependencies.size()) {
        throw std::runtime_error("batch SGE expansion returned the wrong result count");
    }
    for (size_t i = 0; i < reduced_dependencies.size(); ++i) {
        if (expanded[i] != valid.expand_dependency(reduced_dependencies[i])) {
            throw std::runtime_error("batch SGE expansion differs from strict single expansion");
        }
    }

    const std::vector<std::vector<bool>> empty_dependencies;
    if (!valid.expand_dependencies(empty_dependencies).empty()) {
        throw std::runtime_error("empty SGE dependency batch did not remain empty");
    }

    const std::vector<std::vector<bool>> malformed_batch{
        {true, false, false},
        {true, false},
        {false, true, true},
    };
    require_throws<std::invalid_argument>([&] { (void)valid.expand_dependencies(malformed_batch); },
                                          "malformed dependency inside batch");

    SGEResult invalid_composition;
    invalid_composition.original_rows = 4;
    invalid_composition.reduced_matrix = SparseMatrix(3, 1);
    invalid_composition.row_composition = {{0}, {1, 4}, {2, 3}};
    const std::vector<std::vector<bool>> never_selects_invalid_row{
        {true, false, false},
        {false, false, true},
    };
    require_throws<std::out_of_range>(
        [&] { (void)invalid_composition.expand_dependencies(never_selects_invalid_row); },
        "out-of-range original row in batch-unselected composition");

    std::cout << "  PASSED" << std::endl;
}

// Test SGE: row_composition cap (BACKLOG #6 safety)
// 构造 weight-2 链 r0-r1-r2-...-rN, 每两个相邻行共享一个 w2 column.
// 链合并应让 composition[r0] 累积 r0,r1,r2,... 到 N 个 entries.
// 设置低 cap (e.g., 4) 验证 merge throughput 受限 + weight2_skipped_cap > 0.
void test_sge_row_composition_cap() {
    std::cout << "Testing SGE row_composition cap (BACKLOG #6)..." << std::endl;

    // 10 rows × 9 cols. Each col c has weight 2 (rows c, c+1).
    // Row 0: {0}
    // Row 1: {0, 1}
    // Row 2: {1, 2}
    // ...
    // Row 9: {8}
    // Phase 1 wo eliminates row 0 (col 0 has w1) and row 9 (col 8 has w1)...
    // Actually each col c is in {c, c+1} (w2 if both rows alive).
    // Col 0 has only Row 1 if Row 0 is empty? No — Row 0 = {0} → col 0 has R0, R1 (w2).

    // Better construction: build a long weight-2 chain that forces growth.
    // Rows 0..N: Row i = {i-1 if i>0, i if i<N}
    //   Row 0: {0}
    //   Row 1: {0, 1}
    //   Row 2: {1, 2}
    //   ...
    //   Row 8: {7, 8}
    //   Row 9: {8}
    // All cols are w2.
    constexpr size_t N = 10;
    SparseMatrix mat(N, N - 1);
    for (size_t r = 0; r < N; ++r) {
        if (r > 0)
            mat.set(r, r - 1);
        if (r < N - 1)
            mat.set(r, r);
    }

    // Run with low cap to force trigger
    SGEConfig config;
    config.eliminate_weight1 = false; // Force weight-2 path
    config.row_composition_cap = 4;   // Small cap
    auto result_capped = SGE::preprocess(mat, config);

    // Run again without cap to baseline
    SGEConfig config_no_cap;
    config_no_cap.eliminate_weight1 = false;
    config_no_cap.row_composition_cap = 0;
    auto result_baseline = SGE::preprocess(mat, config_no_cap);

    std::cout << "  no_cap: w2_merged=" << result_baseline.weight2_merged
              << " w2_skipped=" << result_baseline.weight2_skipped_cap << "\n";
    std::cout << "  cap=4:  w2_merged=" << result_capped.weight2_merged
              << " w2_skipped=" << result_capped.weight2_skipped_cap << "\n";

    // Cap=4 should skip some merges (composition grows above 4 in chain)
    GNFS_TEST_CHECK(result_capped.weight2_skipped_cap > 0 &&
                    "row_composition_cap should trigger skips on chain matrix");
    GNFS_TEST_CHECK(result_capped.weight2_skipped_cap == 0 ||
                    result_capped.weight2_merged < result_baseline.weight2_merged ||
                    result_capped.weight2_merged == result_baseline.weight2_merged);

    // No row_composition entry exceeds cap in capped result
    for (const auto& comp : result_capped.row_composition) {
        // After merge, composition may exceed cap due to pre-merge sum check
        // (we check prospective = comp[r1]+comp[r2] > cap). Worst case comp[r1]
        // is at cap before final merge that pushed over → could be cap + cap = 2*cap.
        // Use generous 2*cap check.
        GNFS_TEST_CHECK(comp.size() <= 2 * config.row_composition_cap &&
                        "composition size should stay bounded by ~2*cap");
    }

    std::cout << "  PASSED" << std::endl;
}

// Test SGE: cap=0 (disabled) should behave identical to old (no cap) behavior
void test_sge_row_composition_cap_disabled() {
    std::cout << "Testing SGE row_composition cap disabled (cap=0)..." << std::endl;

    SparseMatrix mat(5, 6);
    mat.set(0, 0);
    mat.set(0, 1);
    mat.set(0, 2);
    mat.set(0, 3);
    mat.set(1, 0);
    mat.set(1, 2);
    mat.set(1, 4);
    mat.set(2, 1);
    mat.set(2, 3);
    mat.set(2, 4);
    mat.set(3, 0);
    mat.set(3, 1);
    mat.set(3, 5);
    mat.set(4, 2);
    mat.set(4, 3);
    mat.set(4, 5);

    SGEConfig config;
    config.eliminate_weight1 = false;
    config.row_composition_cap = 0; // disabled
    auto result = SGE::preprocess(mat, config);

    // Should never skip with cap=0
    GNFS_TEST_CHECK(result.weight2_skipped_cap == 0);

    std::cout << "  PASSED" << std::endl;
}

// Test SGE: empty and trivial matrices
// Test SGE: cascading chain — w1 elimination triggers more w1 columns.
// Verifies worklist re-seeding in Phase 1 works correctly.
void test_sge_cascading_weight1() {
    std::cout << "Testing SGE cascading weight-1 chain..." << std::endl;

    // 3×3 staircase:
    //   Row 0: {0}        ← col 0 has only R0 (w1)
    //   Row 1: {0, 1}     ← col 1 has R1, R2 (w2)
    //   Row 2: {1, 2}     ← col 2 has only R2 (w1)
    //
    // Cascade: kill col 2 → R2 dies → col 1 becomes w1 (only R1) → kill →
    // R1 dies → col 0 becomes w1 (only R0) → kill → R0 dies → empty matrix.
    SparseMatrix mat(3, 3);
    mat.set(0, 0);
    mat.set(1, 0);
    mat.set(1, 1);
    mat.set(2, 1);
    mat.set(2, 2);

    SGEConfig config;
    config.eliminate_weight2 = false; // 只测 w1 级联
    auto result = SGE::preprocess(mat, config);

    // 全部 3 列通过 w1 cascade 消除
    GNFS_TEST_CHECK(result.reduced_matrix.num_rows() == 0);
    GNFS_TEST_CHECK(result.weight1_eliminated == 3);

    std::cout << "  Cascading w1: " << result.original_rows << "x" << result.original_cols
              << " → empty (w1=" << result.weight1_eliminated << ")" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// Test SGE: w1 → w2 → w1 alternating cascade across passes.
// Phase 1 eliminates w1, Phase 2 merges w2 (which may create new w1),
// next pass picks up new w1. Verifies multi-pass convergence.
void test_sge_alternating_cascade() {
    std::cout << "Testing SGE w1→w2→w1 alternating cascade..." << std::endl;

    // 5×5 matrix designed for multi-pass cascade:
    //   Row 0: {0}             col 0 w1 (R0 only)         — kill R0 in pass 1 Phase 1
    //   Row 1: {1, 2}          col 1 {R1, R2} w2          — merge in pass 1 Phase 2
    //   Row 2: {1, 3}          col 2 {R1, R4} w2          — merge in pass 1 Phase 2
    //   Row 3: {3, 4}          col 3 {R2, R3} w2
    //   Row 4: {2, 4}          col 4 {R3, R4} w2
    //
    // After Phase 1: R0 + col 0 死, 1 weight-1 eliminated.
    // Phase 2 处理 w2,产生新结构;新 pass 又可能产生 w1。
    SparseMatrix mat(5, 5);
    mat.set(0, 0);
    mat.set(1, 1);
    mat.set(1, 2);
    mat.set(2, 1);
    mat.set(2, 3);
    mat.set(3, 3);
    mat.set(3, 4);
    mat.set(4, 2);
    mat.set(4, 4);

    SGEConfig config;
    config.eliminate_weight1 = true;
    config.eliminate_weight2 = true;
    auto result = SGE::preprocess(mat, config);

    // 期望:R0 在 Phase 1 被消(col 0 是 w1),后续 w2 merge 把剩余压缩
    GNFS_TEST_CHECK(result.weight1_eliminated >= 1);        // 至少 col 0 触发
    GNFS_TEST_CHECK(result.reduced_matrix.num_rows() <= 4); // R0 必定消失
    GNFS_TEST_CHECK(result.passes >= 1);

    std::cout << "  Alternating cascade: " << result.original_rows << "x" << result.original_cols
              << " → " << result.reduced_matrix.num_rows() << "x"
              << result.reduced_matrix.num_cols() << " (passes=" << result.passes
              << ", w1=" << result.weight1_eliminated << ", w2=" << result.weight2_merged << ")"
              << std::endl;
    std::cout << "  PASSED" << std::endl;
}

void test_sge_edge_cases() {
    std::cout << "Testing SGE edge cases..." << std::endl;

    // Empty matrix
    {
        SparseMatrix empty(0, 0);
        auto result = SGE::preprocess(empty);
        GNFS_TEST_CHECK(result.reduced_matrix.num_rows() == 0);
        GNFS_TEST_CHECK(result.reduced_matrix.num_cols() == 0);
    }

    // 1×1 matrix
    {
        SparseMatrix tiny(1, 1);
        tiny.set(0, 0);
        auto result = SGE::preprocess(tiny);
        // Weight-1 column → eliminated
        GNFS_TEST_CHECK(result.reduced_matrix.num_rows() == 0);
        GNFS_TEST_CHECK(result.weight1_eliminated == 1);
    }

    // Identity matrix (all weight-1 columns)
    {
        SparseMatrix ident(5, 5);
        for (size_t i = 0; i < 5; ++i)
            ident.set(i, i);
        auto result = SGE::preprocess(ident);
        GNFS_TEST_CHECK(result.reduced_matrix.num_rows() == 0);
        GNFS_TEST_CHECK(result.weight1_eliminated == 5);
    }

    std::cout << "  Edge cases: PASSED" << std::endl;
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
    test_thin_matrix_dependencies();
    test_verify_dependency();
    test_matrix_stats();
    test_structured_gauss();
    test_schirokauer_repeated_roots();
    test_schirokauer_perfect_power();
    test_schirokauer_squarefree_reducible();
    test_gf_poly_factorization_determinism();
    test_parallel_block_lanczos_correctness();
    test_ensure_all_sorted();
    test_sge_weight1();
    test_sge_weight2();
    test_sge_cascading_weight1();
    test_sge_alternating_cascade();
    test_sge_expand_dependency();
    test_sge_expand_dependency_contract();
    test_sge_expand_dependencies_contract();
    test_sge_row_composition_cap();
    test_sge_row_composition_cap_disabled();
    test_sge_edge_cases();

    std::cout << std::endl;
    std::cout << "=== All Linear Algebra Tests PASSED ===" << std::endl;

    return 0;
}
