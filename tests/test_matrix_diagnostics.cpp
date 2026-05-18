// Unit tests for gnfs::linalg::compute_matrix_diagnostics.
//
// MatrixDiagnostics emits row/col weight distribution at Phase 5 entry,
// revealing sieve gap (empty cols) and SGE-eliminable garbage (singleton
// cols/rows). The function walks all nnz once so its contract — what each
// counter measures — must be locked in unit tests so drift can't silently
// change the 50d diagnostic interpretation.

#include "gnfs/linalg/matrix_builder.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using gnfs::linalg::SparseMatrix;
using gnfs::linalg::MatrixDiagnostics;
using gnfs::linalg::compute_matrix_diagnostics;

void test_empty_matrix() {
    std::cout << "Testing 0x0 matrix..." << std::endl;
    SparseMatrix M(0, 0);
    auto d = compute_matrix_diagnostics(M);
    assert(d.empty_rows == 0);
    assert(d.singleton_rows == 0);
    assert(d.min_row_weight == 0);
    assert(d.max_row_weight == 0);
    assert(d.empty_cols == 0);
    assert(d.singleton_cols == 0);
    assert(d.low_weight_cols == 0);
    assert(d.max_col_weight == 0);
    assert(d.avg_col_weight == 0.0);
    std::cout << "  PASS" << std::endl;
}

void test_rows_no_cols() {
    std::cout << "Testing rows × 0 (no cols)..." << std::endl;
    // Edge: rows exist but num_cols == 0. compute_matrix_diagnostics returns
    // early; we just need it to not crash and not report stale stats.
    SparseMatrix M(5, 0);
    auto d = compute_matrix_diagnostics(M);
    assert(d.empty_cols == 0);  // no cols → no empty_cols either
    std::cout << "  PASS" << std::endl;
}

void test_zero_rows_with_cols() {
    std::cout << "Testing 0 × cols (no rows)..." << std::endl;
    SparseMatrix M(0, 4);
    auto d = compute_matrix_diagnostics(M);
    // 0 rows means all 4 cols are empty (no contribution)
    assert(d.empty_cols == 4);
    assert(d.singleton_cols == 0);
    assert(d.max_col_weight == 0);
    assert(d.avg_col_weight == 0.0);
    std::cout << "  PASS" << std::endl;
}

void test_identity_like() {
    std::cout << "Testing 4×4 identity-like (every col is singleton)..." << std::endl;
    SparseMatrix M(4, 4);
    for (uint32_t i = 0; i < 4; ++i) M.row(i).set(i);
    auto d = compute_matrix_diagnostics(M);
    // Each row has weight 1, each col has weight 1
    assert(d.empty_rows == 0);
    assert(d.singleton_rows == 4);
    assert(d.min_row_weight == 1);
    assert(d.max_row_weight == 1);
    assert(d.empty_cols == 0);
    assert(d.singleton_cols == 4);  // every col is SGE-eliminable
    assert(d.max_col_weight == 1);
    assert(d.avg_col_weight == 1.0);
    std::cout << "  PASS" << std::endl;
}

void test_empty_rows() {
    std::cout << "Testing matrix with empty rows..." << std::endl;
    SparseMatrix M(5, 4);
    // Row 0: empty
    // Row 1: {0}
    // Row 2: {1, 2}
    // Row 3: empty
    // Row 4: {0, 1}
    M.row(1).set(0);
    M.row(2).set(1);
    M.row(2).set(2);
    M.row(4).set(0);
    M.row(4).set(1);
    auto d = compute_matrix_diagnostics(M);
    assert(d.empty_rows == 2);          // rows 0, 3
    assert(d.singleton_rows == 1);      // row 1
    assert(d.min_row_weight == 0);
    assert(d.max_row_weight == 2);
    // Col weights: 0→{1,4}=2, 1→{2,4}=2, 2→{2}=1, 3→{}=0
    assert(d.empty_cols == 1);          // col 3
    assert(d.singleton_cols == 1);      // col 2
    assert(d.low_weight_cols == 2);     // cols 0, 1 (weight 2 ∈ [2,4])
    assert(d.max_col_weight == 2);
    std::cout << "  PASS" << std::endl;
}

void test_dense_block() {
    std::cout << "Testing 3×3 fully dense block..." << std::endl;
    SparseMatrix M(3, 3);
    for (uint32_t i = 0; i < 3; ++i) {
        for (uint32_t j = 0; j < 3; ++j) {
            M.row(i).set(j);
        }
    }
    auto d = compute_matrix_diagnostics(M);
    assert(d.empty_rows == 0);
    assert(d.singleton_rows == 0);
    assert(d.min_row_weight == 3);
    assert(d.max_row_weight == 3);
    assert(d.empty_cols == 0);
    assert(d.singleton_cols == 0);
    assert(d.low_weight_cols == 3);     // all 3 cols have weight 3 ∈ [2,4]
    assert(d.max_col_weight == 3);
    assert(d.avg_col_weight == 3.0);
    std::cout << "  PASS" << std::endl;
}

void test_singleton_cols_separated_from_low_weight() {
    std::cout << "Testing low_weight_cols range [2,4] does not include weight=1..." << std::endl;
    SparseMatrix M(5, 3);
    // Col 0 weight=1 (singleton, NOT low_weight)
    // Col 1 weight=4 (low_weight)
    // Col 2 weight=5 (heavy, not low_weight)
    M.row(0).set(0);
    for (uint32_t i = 0; i < 4; ++i) M.row(i).set(1);
    for (uint32_t i = 0; i < 5; ++i) M.row(i).set(2);
    auto d = compute_matrix_diagnostics(M);
    assert(d.empty_cols == 0);
    assert(d.singleton_cols == 1);     // col 0
    assert(d.low_weight_cols == 1);    // col 1 only
    assert(d.max_col_weight == 5);
    std::cout << "  PASS" << std::endl;
}

void test_avg_col_weight_includes_empty() {
    std::cout << "Testing avg_col_weight includes empty cols in denominator..." << std::endl;
    SparseMatrix M(3, 4);
    // 1 nnz on col 0, no other nnz
    M.row(0).set(0);
    auto d = compute_matrix_diagnostics(M);
    // total_weight=1, num_cols=4 → avg = 0.25
    assert(d.avg_col_weight > 0.24 && d.avg_col_weight < 0.26);
    assert(d.empty_cols == 3);
    assert(d.singleton_cols == 1);
    std::cout << "  PASS" << std::endl;
}

void test_dup_set_is_idempotent() {
    std::cout << "Testing duplicate set() collapses (GF(2) idempotency)..." << std::endl;
    SparseMatrix M(2, 3);
    // Setting the same bit twice is a no-op for indices_ (set() detects dup).
    // Diagnostics should reflect dedup'd column weight.
    M.row(0).set(0);
    M.row(0).set(0);
    M.row(0).set(0);
    auto d = compute_matrix_diagnostics(M);
    // Row 0 weight should be 1, not 3
    assert(d.singleton_rows == 1);
    assert(d.min_row_weight == 0);   // row 1 is empty
    assert(d.max_row_weight == 1);
    assert(d.singleton_cols == 1);   // col 0
    assert(d.max_col_weight == 1);
    std::cout << "  PASS" << std::endl;
}

void test_realistic_excess_matrix() {
    std::cout << "Testing 10×8 random-ish matrix with excess..." << std::endl;
    SparseMatrix M(10, 8);
    // Row weights varying 1..5
    M.row(0).set(0);                              // w=1
    M.row(1).set(1); M.row(1).set(2);             // w=2
    M.row(2).set(0); M.row(2).set(3);             // w=2
    M.row(3).set(4); M.row(3).set(5); M.row(3).set(6); M.row(3).set(7);  // w=4
    M.row(4).set(0); M.row(4).set(1); M.row(4).set(2); M.row(4).set(3); M.row(4).set(4);  // w=5
    M.row(5).set(7);                              // w=1
    M.row(6).set(2); M.row(6).set(5);             // w=2
    M.row(7).set(0); M.row(7).set(3); M.row(7).set(6);  // w=3
    // rows 8, 9 empty
    auto d = compute_matrix_diagnostics(M);
    assert(d.empty_rows == 2);
    assert(d.singleton_rows == 2);   // rows 0, 5
    assert(d.min_row_weight == 0);
    assert(d.max_row_weight == 5);
    // Col weights: 0={0,2,4,7}=4, 1={1,4}=2, 2={1,4,6}=3, 3={2,4,7}=3,
    //              4={3,4}=2, 5={3,6}=2, 6={3,7}=2, 7={3,5}=2
    assert(d.empty_cols == 0);
    assert(d.singleton_cols == 0);
    // weight=2 cols: 1,4,5,6,7 (5 cols); weight=3 cols: 2,3 (2 cols); weight=4 col: 0 (1 col)
    // low_weight_cols (∈ [2,4]) = 8 cols total (all of them)
    assert(d.low_weight_cols == 8);
    assert(d.max_col_weight == 4);
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== linalg/compute_matrix_diagnostics tests ===" << std::endl;

    test_empty_matrix();
    test_rows_no_cols();
    test_zero_rows_with_cols();
    test_identity_like();
    test_empty_rows();
    test_dense_block();
    test_singleton_cols_separated_from_low_weight();
    test_avg_col_weight_includes_empty();
    test_dup_set_is_idempotent();
    test_realistic_excess_matrix();

    std::cout << "\n=== All linalg/compute_matrix_diagnostics tests PASSED ===" << std::endl;
    return 0;
}
