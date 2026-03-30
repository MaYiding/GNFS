// Unit tests for GNFSParams::compute() — auto-parameter calculator
#include "gnfs/core/params.hpp"

#include <cassert>
#include <iostream>

using namespace gnfs::core;

void test_basic_fields() {
    std::cout << "Testing basic fields..." << std::endl;

    auto p = GNFSParams::compute(40);
    assert(p.bits == 40);
    assert(p.digits == static_cast<size_t>(40 * 0.30103 + 1));

    auto p2 = GNFSParams::compute(100);
    assert(p2.bits == 100);
    assert(p2.digits == static_cast<size_t>(100 * 0.30103 + 1));

    std::cout << "  PASS" << std::endl;
}

void test_degree_selection() {
    std::cout << "Testing degree selection..." << std::endl;

    // <= 50 bits → degree 3
    assert(GNFSParams::compute(20).degree == 3);
    assert(GNFSParams::compute(50).degree == 3);

    // 51-100 bits → degree 3
    assert(GNFSParams::compute(80).degree == 3);
    assert(GNFSParams::compute(100).degree == 3);

    // 101+ bits: analytical formula d = round((3·lnN/lnlnN)^{1/3}), clamped to [4, 8]
    assert(GNFSParams::compute(101).degree == 4);  // d_opt ≈ 3.67
    assert(GNFSParams::compute(150).degree == 4);  // d_opt ≈ 4.07
    assert(GNFSParams::compute(200).degree == 4);  // d_opt ≈ 4.39
    assert(GNFSParams::compute(250).degree == 5);  // d_opt ≈ 4.65
    assert(GNFSParams::compute(350).degree == 5);  // d_opt ≈ 5.10
    assert(GNFSParams::compute(500).degree == 6);  // d_opt ≈ 5.62
    assert(GNFSParams::compute(700).degree == 6);  // d_opt ≈ 6.17
    assert(GNFSParams::compute(1000).degree == 7); // d_opt ≈ 6.82
    assert(GNFSParams::compute(2000).degree == 8); // d_opt ≈ 8.07

    std::cout << "  PASS" << std::endl;
}

void test_factor_base_bounds() {
    std::cout << "Testing factor base bounds..." << std::endl;

    // Small N: empirical values (reduced per CADO-NFS calibration)
    auto p6 = GNFSParams::compute(17);  // ~6 digits
    assert(p6.rational_bound >= 200 && p6.rational_bound <= 2000);

    auto p15 = GNFSParams::compute(48);  // ~15 digits
    assert(p15.rational_bound >= 1000 && p15.rational_bound <= 10000);

    auto p20 = GNFSParams::compute(65);  // ~20 digits
    assert(p20.rational_bound >= 2000 && p20.rational_bound <= 20000);

    auto p30 = GNFSParams::compute(98);  // ~30 digits
    assert(p30.rational_bound >= 10000 && p30.rational_bound <= 100000);

    // Bounds should increase with N size
    auto small = GNFSParams::compute(30);
    auto big = GNFSParams::compute(200);
    assert(big.rational_bound > small.rational_bound);

    // algebraic_bound >= rational_bound (typically ~2×)
    for (size_t bits : {20, 50, 80, 120, 200}) {
        auto p = GNFSParams::compute(bits);
        assert(p.algebraic_bound >= p.rational_bound);
    }

    // Upper bound: never exceed UINT32_MAX
    auto huge = GNFSParams::compute(500);
    assert(huge.rational_bound <= UINT32_MAX);

    std::cout << "  PASS" << std::endl;
}

void test_special_q_above_fb_bound() {
    std::cout << "Testing special_q_min > algebraic_bound (regression)..." << std::endl;

    // This is the critical invariant fixed in Session 26:
    // SQ must be ABOVE the factor base to avoid wasteful sieving
    for (size_t bits = 10; bits <= 300; bits += 10) {
        auto p = GNFSParams::compute(bits);
        assert(p.special_q_min > p.algebraic_bound);
        assert(p.special_q_max > p.special_q_min);
        assert(p.special_q_min == p.algebraic_bound + 1);
    }

    // Verify 10× multiplier
    auto p = GNFSParams::compute(80);
    uint64_t expected_max = std::min(
        static_cast<uint64_t>(p.algebraic_bound) * 10,
        static_cast<uint64_t>(UINT32_MAX));
    assert(p.special_q_max == static_cast<uint32_t>(expected_max));

    std::cout << "  PASS" << std::endl;
}

void test_sieve_area_cap() {
    std::cout << "Testing sieve area cap..." << std::endl;

    // For large N, sieve area should be capped at 256M positions
    constexpr size_t MAX_AREA = 256 * 1024 * 1024;

    for (size_t bits : {100, 200, 300, 500}) {
        auto p = GNFSParams::compute(bits);
        size_t area = p.sieve_region_size();
        assert(area <= MAX_AREA);
        assert(area > 0);
    }

    // Small N should have reasonable sieve regions
    auto p_small = GNFSParams::compute(30);
    assert(p_small.sieve_region_size() < MAX_AREA);
    assert(p_small.sieve_region_size() > 0);

    std::cout << "  PASS" << std::endl;
}

void test_sieve_region_geometry() {
    std::cout << "Testing sieve region geometry..." << std::endl;

    for (size_t bits : {20, 50, 80, 120}) {
        auto p = GNFSParams::compute(bits);
        // i range should be symmetric around 0
        assert(p.sieve_i_min < 0);
        assert(p.sieve_i_max > 0);
        assert(p.sieve_i_max == -(p.sieve_i_min + 1));

        // j range starts at 1 (skip j=0)
        assert(p.sieve_j_min == 1);
        assert(p.sieve_j_max > 0);
    }

    std::cout << "  PASS" << std::endl;
}

void test_large_prime_bound() {
    std::cout << "Testing large prime bound..." << std::endl;

    for (size_t bits : {20, 50, 80, 120, 200}) {
        auto p = GNFSParams::compute(bits);
        // LP bound should be >= FB bound
        assert(p.large_prime_bound >= p.rational_bound);
        // For larger N (>40 bits → >12 digits), LP should be enabled
        if (bits >= 40) {
            assert(p.large_prime_bits > 0);
            assert(p.large_prime_bound > p.rational_bound);
        }
    }

    std::cout << "  PASS" << std::endl;
}

void test_threshold_values() {
    std::cout << "Testing threshold values..." << std::endl;

    for (size_t bits : {20, 50, 80, 120, 200}) {
        auto p = GNFSParams::compute(bits);
        // Thresholds must be non-zero (otherwise sieve accepts nothing)
        assert(p.rational_threshold > 0);
        assert(p.algebraic_threshold > 0);
        // Thresholds stored as uint16_t, must fit
        assert(p.rational_threshold <= UINT16_MAX);
        assert(p.algebraic_threshold <= UINT16_MAX);
        // For LP-enabled N, thresholds should be larger to allow LP cofactors
        if (p.large_prime_bits > 0) {
            assert(p.rational_threshold > 56);  // must exceed no-LP baseline
        }
    }

    std::cout << "  PASS" << std::endl;
}

void test_max_special_q() {
    std::cout << "Testing max_special_q scaling..." << std::endl;

    auto small = GNFSParams::compute(30);  // ~10 digits
    assert(small.max_special_q >= 2000);  // 下限保证

    auto medium = GNFSParams::compute(80);  // ~25 digits
    assert(medium.max_special_q >= 20000);  // 基于 est_rels 动态计算

    auto large = GNFSParams::compute(140);  // ~42 digits
    assert(large.max_special_q >= 100000);

    auto huge = GNFSParams::compute(200);  // ~60 digits
    assert(huge.max_special_q >= 1000000);

    // 单调性：更大的 N 需要更多 SQs
    assert(medium.max_special_q >= small.max_special_q);
    assert(large.max_special_q >= medium.max_special_q);
    assert(huge.max_special_q >= large.max_special_q);

    std::cout << "  PASS" << std::endl;
}

void test_estimated_relations() {
    std::cout << "Testing estimated_relations_needed..." << std::endl;

    for (size_t bits : {30, 60, 100}) {
        auto p = GNFSParams::compute(bits);
        size_t est = p.estimated_relations_needed();
        assert(est > 0);
        // Should be reasonable: at least target_excess
        assert(est > p.target_excess);
    }

    std::cout << "  PASS" << std::endl;
}

void test_sieve_memory() {
    std::cout << "Testing sieve_memory_bytes..." << std::endl;

    for (size_t bits : {30, 80, 200}) {
        auto p = GNFSParams::compute(bits);
        size_t mem = p.sieve_memory_bytes();
        // Memory = positions × sizeof(uint16_t) = positions × 2
        assert(mem == p.sieve_region_size() * sizeof(uint16_t));
        // Cap: 256M positions × 2 = 512 MB
        assert(mem <= 512ULL * 1024 * 1024);
    }

    std::cout << "  PASS" << std::endl;
}

void test_polynomial_params() {
    std::cout << "Testing polynomial selection params..." << std::endl;

    for (size_t bits : {30, 80, 200}) {
        auto p = GNFSParams::compute(bits);
        assert(p.leading_coeff_bound > 0);
        assert(p.search_radius >= 100);
        assert(p.num_candidates >= 1000);
        assert(p.skewness_steps >= 100);
    }

    // Larger N should have larger leading_coeff_bound
    auto small = GNFSParams::compute(40);
    auto big = GNFSParams::compute(200);
    assert(big.leading_coeff_bound >= small.leading_coeff_bound);

    std::cout << "  PASS" << std::endl;
}

void test_qc_and_excess() {
    std::cout << "Testing QC primes and target excess..." << std::endl;

    for (size_t bits : {30, 80, 200}) {
        auto p = GNFSParams::compute(bits);
        assert(p.num_qc_primes >= 32);
        assert(p.num_qc_primes <= 128);
        assert(p.target_excess >= 200);
    }

    std::cout << "  PASS" << std::endl;
}

void test_monotonicity() {
    std::cout << "Testing parameter monotonicity..." << std::endl;

    // FB bound should generally increase with N size
    uint32_t prev_fb = 0;
    for (size_t bits = 20; bits <= 200; bits += 20) {
        auto p = GNFSParams::compute(bits);
        assert(p.rational_bound >= prev_fb);
        prev_fb = p.rational_bound;
    }

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== GNFSParams Unit Tests ===" << std::endl;

    test_basic_fields();
    test_degree_selection();
    test_factor_base_bounds();
    test_special_q_above_fb_bound();
    test_sieve_area_cap();
    test_sieve_region_geometry();
    test_large_prime_bound();
    test_threshold_values();
    test_max_special_q();
    test_estimated_relations();
    test_sieve_memory();
    test_polynomial_params();
    test_qc_and_excess();
    test_monotonicity();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
