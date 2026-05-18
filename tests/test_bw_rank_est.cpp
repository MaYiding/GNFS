// Unit tests for gnfs::linalg::compute_rank_est.
//
// The BW thin solve (B' = M^T·M variant, BACKLOG #1 step 7) emits a rank
// lower-bound diagnostic by summing `LingenResult::degrees[j]` over valid
// columns (bit j set in `valid_mask`). The diagnostic is what distinguishes
// "matrix has excess, BW should find deps" (rank_est ≈ m) from "pathologically
// rank-deficient, sieve gap" (rank_est ≪ m). A drift here silently miscolors
// the 50d empirical interpretation, so the helper is unit-locked.

#include "gnfs/linalg/block_wiedemann.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using gnfs::linalg::LingenResult;
using gnfs::linalg::compute_rank_est;

void test_empty_lingen() {
    std::cout << "Testing compute_rank_est on default-init LingenResult..." << std::endl;
    LingenResult R;
    // valid_mask=0, all degrees=0 → rank_est=0
    assert(compute_rank_est(R) == 0);
    std::cout << "  PASS" << std::endl;
}

void test_single_valid_col() {
    std::cout << "Testing single valid col..." << std::endl;
    LingenResult R;
    R.valid_mask = 1ULL;          // only bit 0
    R.degrees[0] = 42;
    R.degrees[1] = 99;            // invalid col, should not count
    R.degrees[63] = 1000000;
    assert(compute_rank_est(R) == 42);
    std::cout << "  PASS" << std::endl;
}

void test_all_64_valid_uniform() {
    std::cout << "Testing all 64 valid columns, uniform degrees..." << std::endl;
    LingenResult R;
    R.valid_mask = ~uint64_t(0);  // all 64 bits set
    for (int j = 0; j < 64; ++j) R.degrees[j] = 100;
    assert(compute_rank_est(R) == 6400);
    std::cout << "  PASS" << std::endl;
}

void test_mask_selects_subset() {
    std::cout << "Testing valid_mask selects subset of degrees..." << std::endl;
    LingenResult R;
    // bits 0, 5, 17, 63 set; only those degrees count
    R.valid_mask = (1ULL << 0) | (1ULL << 5) | (1ULL << 17) | (1ULL << 63);
    for (int j = 0; j < 64; ++j) R.degrees[j] = j + 1;  // 1..64
    // expected = degrees[0] + degrees[5] + degrees[17] + degrees[63]
    //         = 1 + 6 + 18 + 64 = 89
    assert(compute_rank_est(R) == 89);
    std::cout << "  PASS" << std::endl;
}

void test_invalid_cols_ignored_even_with_large_degrees() {
    std::cout << "Testing invalid col degrees are ignored..." << std::endl;
    LingenResult R;
    R.valid_mask = (1ULL << 0);
    R.degrees[0] = 7;
    for (int j = 1; j < 64; ++j) R.degrees[j] = 1000000;  // garbage in invalid slots
    assert(compute_rank_est(R) == 7);
    std::cout << "  PASS" << std::endl;
}

void test_full_rank_thin_solve_like() {
    std::cout << "Testing full-rank thin-solve-like profile (BW phase 2)..." << std::endl;
    // Mimics test_thin_matrix_bw_solve: 5200×6000 rank≈5100
    // Realistic profile: most cols valid, average degree ~85
    LingenResult R;
    R.valid_mask = ~uint64_t(0);
    for (int j = 0; j < 64; ++j) R.degrees[j] = 80;
    assert(compute_rank_est(R) == 64 * 80);  // 5120
    // This sits ≈ m=5200 → matrix has excess → BW should find deps
    std::cout << "  PASS" << std::endl;
}

void test_extreme_rank_deficiency_profile() {
    std::cout << "Testing extreme rank deficiency (rank ≪ m)..." << std::endl;
    // Mimics test_thin_matrix_bw_extreme_rank_deficiency
    // Only 1 col valid with very low degree
    LingenResult R;
    R.valid_mask = 1ULL;
    R.degrees[0] = 1;
    assert(compute_rank_est(R) == 1);  // ≪ m=5200 → pathological
    std::cout << "  PASS" << std::endl;
}

void test_zero_degrees_with_valid_mask() {
    std::cout << "Testing valid_mask set but all degrees=0..." << std::endl;
    // degenerate: bits set in mask but degrees=0 → rank_est=0
    LingenResult R;
    R.valid_mask = ~uint64_t(0);
    // degrees default-init to 0
    assert(compute_rank_est(R) == 0);
    std::cout << "  PASS" << std::endl;
}

void test_only_high_bit_set() {
    std::cout << "Testing only bit 63 set in valid_mask..." << std::endl;
    LingenResult R;
    R.valid_mask = 1ULL << 63;
    R.degrees[63] = 256;
    R.degrees[0] = 1024;  // not valid, ignored
    assert(compute_rank_est(R) == 256);
    std::cout << "  PASS" << std::endl;
}

void test_noexcept_contract() {
    std::cout << "Testing compute_rank_est noexcept..." << std::endl;
    LingenResult R;
    static_assert(noexcept(compute_rank_est(R)));
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== linalg/compute_rank_est tests ===" << std::endl;

    test_empty_lingen();
    test_single_valid_col();
    test_all_64_valid_uniform();
    test_mask_selects_subset();
    test_invalid_cols_ignored_even_with_large_degrees();
    test_full_rank_thin_solve_like();
    test_extreme_rank_deficiency_profile();
    test_zero_degrees_with_valid_mask();
    test_only_high_bit_set();
    test_noexcept_contract();

    std::cout << "\n=== All linalg/compute_rank_est tests PASSED ===" << std::endl;
    return 0;
}
