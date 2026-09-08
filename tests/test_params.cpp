// Unit tests for GNFSParams::compute() — auto-parameter calculator
#include "gnfs/core/params.hpp"
#include "support/test_check.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>

using namespace gnfs::core;

void test_basic_fields() {
    std::cout << "Testing basic fields..." << std::endl;

    auto p = GNFSParams::compute(40);
    GNFS_TEST_CHECK(p.bits == 40);
    GNFS_TEST_CHECK(p.digits == static_cast<size_t>(40 * 0.30103 + 1));

    auto p2 = GNFSParams::compute(100);
    GNFS_TEST_CHECK(p2.bits == 100);
    GNFS_TEST_CHECK(p2.digits == static_cast<size_t>(100 * 0.30103 + 1));

    std::cout << "  PASS" << std::endl;
}

void test_degree_selection() {
    std::cout << "Testing degree selection..." << std::endl;

    // <= 50 bits → degree 3
    GNFS_TEST_CHECK(GNFSParams::compute(20).degree == 3);
    GNFS_TEST_CHECK(GNFSParams::compute(50).degree == 3);

    // 51-100 bits → degree 3
    GNFS_TEST_CHECK(GNFSParams::compute(80).degree == 3);
    GNFS_TEST_CHECK(GNFSParams::compute(100).degree == 3);

    // 101-200 bits: forced degree 3 (even-degree polynomial bug fix)
    GNFS_TEST_CHECK(GNFSParams::compute(101).degree == 3);
    GNFS_TEST_CHECK(GNFSParams::compute(150).degree == 3);
    GNFS_TEST_CHECK(GNFSParams::compute(200).degree == 3);
    // 201-400 bits: forced degree 4
    GNFS_TEST_CHECK(GNFSParams::compute(250).degree == 4);
    GNFS_TEST_CHECK(GNFSParams::compute(350).degree == 4);
    // 401+ bits: formula-based, clamped to [5, 6]
    GNFS_TEST_CHECK(GNFSParams::compute(500).degree == 6);  // d_opt ≈ 5.62
    GNFS_TEST_CHECK(GNFSParams::compute(700).degree == 6);  // d_opt ≈ 6.17
    GNFS_TEST_CHECK(GNFSParams::compute(1000).degree == 6); // d_opt ≈ 6.82, clamped to 6
    GNFS_TEST_CHECK(GNFSParams::compute(2000).degree == 6); // d_opt ≈ 8.07, clamped to 6

    std::cout << "  PASS" << std::endl;
}

void test_factor_base_bounds() {
    std::cout << "Testing factor base bounds..." << std::endl;

    // Small N: empirical values (reduced per CADO-NFS calibration)
    auto p6 = GNFSParams::compute(17); // ~6 digits
    GNFS_TEST_CHECK(p6.rational_bound >= 200 && p6.rational_bound <= 2000);

    auto p15 = GNFSParams::compute(48); // ~15 digits
    GNFS_TEST_CHECK(p15.rational_bound >= 1000 && p15.rational_bound <= 10000);

    auto p20 = GNFSParams::compute(65); // ~20 digits
    GNFS_TEST_CHECK(p20.rational_bound >= 2000 && p20.rational_bound <= 20000);

    auto p30 = GNFSParams::compute(98); // ~30 digits
    GNFS_TEST_CHECK(p30.rational_bound >= 10000 && p30.rational_bound <= 100000);

    // Bounds should increase with N size
    auto small = GNFSParams::compute(30);
    auto big = GNFSParams::compute(200);
    GNFS_TEST_CHECK(big.rational_bound > small.rational_bound);

    // algebraic_bound >= rational_bound (typically ~2×)
    for (size_t bits : {size_t{20}, size_t{50}, size_t{80}, size_t{120}, size_t{200}}) {
        auto p = GNFSParams::compute(bits);
        GNFS_TEST_CHECK(p.algebraic_bound >= p.rational_bound);
    }

    // Upper bound: never exceed UINT32_MAX
    auto huge = GNFSParams::compute(500);
    GNFS_TEST_CHECK(huge.rational_bound <= UINT32_MAX);

    std::cout << "  PASS" << std::endl;
}

void test_special_q_above_fb_bound() {
    std::cout << "Testing special_q_min > algebraic_bound (regression)..." << std::endl;

    // This is the critical invariant fixed in Session 26:
    // SQ must be ABOVE the factor base to avoid wasteful sieving
    for (size_t bits = 10; bits <= 300; bits += 10) {
        auto p = GNFSParams::compute(bits);
        GNFS_TEST_CHECK(p.special_q_min > p.algebraic_bound);
        GNFS_TEST_CHECK(p.special_q_max > p.special_q_min);
        GNFS_TEST_CHECK(p.special_q_min == p.algebraic_bound + 1);
    }

    // Verify 10× multiplier
    auto p = GNFSParams::compute(80);
    uint64_t expected_max =
        std::min(static_cast<uint64_t>(p.algebraic_bound) * 10, static_cast<uint64_t>(UINT32_MAX));
    GNFS_TEST_CHECK(p.special_q_max == static_cast<uint32_t>(expected_max));

    std::cout << "  PASS" << std::endl;
}

void test_sieve_area_cap() {
    std::cout << "Testing sieve area cap..." << std::endl;

    // For large N, sieve area should be capped at a reasonable limit
    constexpr size_t MAX_AREA = 1024ULL * 1024 * 1024; // 1G positions

    for (size_t bits : {size_t{100}, size_t{200}, size_t{300}, size_t{500}}) {
        auto p = GNFSParams::compute(bits);
        size_t area = p.sieve_region_size();
        GNFS_TEST_CHECK(area <= MAX_AREA);
        GNFS_TEST_CHECK(area > 0);
    }

    // Small N should have reasonable sieve regions
    auto p_small = GNFSParams::compute(30);
    GNFS_TEST_CHECK(p_small.sieve_region_size() < MAX_AREA);
    GNFS_TEST_CHECK(p_small.sieve_region_size() > 0);

    std::cout << "  PASS" << std::endl;
}

void test_sieve_region_geometry() {
    std::cout << "Testing sieve region geometry..." << std::endl;

    for (size_t bits : {size_t{20}, size_t{50}, size_t{80}, size_t{120}}) {
        auto p = GNFSParams::compute(bits);
        // i range should be symmetric around 0
        GNFS_TEST_CHECK(p.sieve_i_min < 0);
        GNFS_TEST_CHECK(p.sieve_i_max > 0);
        GNFS_TEST_CHECK(p.sieve_i_max == -(p.sieve_i_min + 1));

        // j range starts at 1 (skip j=0)
        GNFS_TEST_CHECK(p.sieve_j_min == 1);
        GNFS_TEST_CHECK(p.sieve_j_max > 0);
    }

    std::cout << "  PASS" << std::endl;
}

void test_configurable_sieve_width_is_exact() {
    std::cout << "Testing exact configurable sieve widths..." << std::endl;

    for (const int32_t width : {1, 2, 3, 5, 4095, std::numeric_limits<int32_t>::max()}) {
        const auto [i_min, i_max] = GNFSParams::sieve_i_bounds_for_width(width);
        const int64_t observed = static_cast<int64_t>(i_max) - static_cast<int64_t>(i_min) + 1;
        GNFS_TEST_CHECK(observed == width);
    }

    bool rejected = false;
    try {
        (void)GNFSParams::sieve_i_bounds_for_width(0);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    GNFS_TEST_CHECK(rejected);

    std::cout << "  PASS" << std::endl;
}

void test_large_prime_bound() {
    std::cout << "Testing large prime bound..." << std::endl;

    for (size_t bits : {size_t{20}, size_t{50}, size_t{80}, size_t{120}, size_t{200}}) {
        auto p = GNFSParams::compute(bits);
        // LP bound should be >= FB bound
        GNFS_TEST_CHECK(p.large_prime_bound >= p.rational_bound);
        // For larger N (>40 bits → >12 digits), LP should be enabled
        if (bits >= 40) {
            GNFS_TEST_CHECK(p.large_prime_bits > 0);
            GNFS_TEST_CHECK(p.large_prime_bound > p.rational_bound);
        }
    }

    std::cout << "  PASS" << std::endl;
}

void test_lp_bits_env_override() {
    std::cout << "Testing GNFS_OVERRIDE_LP_BITS ENV override..." << std::endl;

    // Default: 60d (197-bit) → lp_bits = 26
    unsetenv("GNFS_OVERRIDE_LP_BITS");
    auto default_60d = GNFSParams::compute(197);
    GNFS_TEST_CHECK(default_60d.large_prime_bits == 26);

    // ENV=25 → lp_bits = 25
    setenv("GNFS_OVERRIDE_LP_BITS", "25", 1);
    auto override_25 = GNFSParams::compute(197);
    GNFS_TEST_CHECK(override_25.large_prime_bits == 25);
    GNFS_TEST_CHECK(override_25.large_prime_bound == (1ULL << 25));

    // ENV=27 → lp_bits = 27 (any 1..30 in range)
    setenv("GNFS_OVERRIDE_LP_BITS", "27", 1);
    auto override_27 = GNFSParams::compute(197);
    GNFS_TEST_CHECK(override_27.large_prime_bits == 27);

    // ENV=0 (out of range 1..30) → no override (back to default 26)
    setenv("GNFS_OVERRIDE_LP_BITS", "0", 1);
    auto invalid_0 = GNFSParams::compute(197);
    GNFS_TEST_CHECK(invalid_0.large_prime_bits == 26);

    // ENV=99 (out of range) → no override
    setenv("GNFS_OVERRIDE_LP_BITS", "99", 1);
    auto invalid_99 = GNFSParams::compute(197);
    GNFS_TEST_CHECK(invalid_99.large_prime_bits == 26);

    // Clean up
    unsetenv("GNFS_OVERRIDE_LP_BITS");
    auto cleanup = GNFSParams::compute(197);
    GNFS_TEST_CHECK(cleanup.large_prime_bits == 26);

    std::cout << "  PASS" << std::endl;
}

void test_target_multiplier_reloads_per_call() {
    std::cout << "Testing GNFS_SIEVE_TARGET_MULT reload semantics..." << std::endl;

    std::optional<std::string> previous_value;
    if (const char* previous = std::getenv("GNFS_SIEVE_TARGET_MULT")) {
        previous_value = previous;
    }
    const auto restore_environment = [&]() noexcept {
        if (previous_value.has_value()) {
            (void)setenv("GNFS_SIEVE_TARGET_MULT", previous_value->c_str(), 1);
        } else {
            (void)unsetenv("GNFS_SIEVE_TARGET_MULT");
        }
    };

    unsetenv("GNFS_SIEVE_TARGET_MULT");
    const auto params = GNFSParams::compute(197);
    const uint32_t baseline_max_special_q = params.max_special_q;
    setenv("GNFS_SIEVE_TARGET_MULT", "100", 1);
    const auto scaled_params = GNFSParams::compute(197);
    GNFS_TEST_CHECK(scaled_params.max_special_q == baseline_max_special_q);

    unsetenv("GNFS_SIEVE_TARGET_MULT");
    constexpr size_t columns = 1000;
    const size_t baseline = params.raw_relation_target(columns);
    GNFS_TEST_CHECK(baseline > 0);

    setenv("GNFS_SIEVE_TARGET_MULT", "2", 1);
    const size_t doubled = params.raw_relation_target(columns);
    GNFS_TEST_CHECK(doubled == baseline * 2);

    setenv("GNFS_SIEVE_TARGET_MULT", "3.5", 1);
    const size_t fractional = params.raw_relation_target(columns);
    GNFS_TEST_CHECK(fractional == static_cast<size_t>(static_cast<double>(baseline) * 3.5));

    setenv("GNFS_SIEVE_TARGET_MULT", "0.1", 1);
    const size_t minimum = params.raw_relation_target(columns);
    GNFS_TEST_CHECK(minimum == static_cast<size_t>(static_cast<double>(baseline) * 0.1));

    setenv("GNFS_SIEVE_TARGET_MULT", "100.0", 1);
    const size_t maximum = params.raw_relation_target(columns);
    GNFS_TEST_CHECK(maximum == static_cast<size_t>(static_cast<double>(baseline) * 100.0));

    // Whitespace, non-decimal syntax, invalid suffixes, non-finite values, and
    // out-of-range values must fall back to 1.
    for (const char* invalid :
         {" 2", "2 ", "0x1p1", "2x", ".", "1e", "1e+", "nan", "0.01", "101", "1e-9999", "1e9999"}) {
        setenv("GNFS_SIEVE_TARGET_MULT", invalid, 1);
        GNFS_TEST_CHECK(params.raw_relation_target(columns) == baseline);
    }

    restore_environment();
    std::cout << "  PASS" << std::endl;
}

void test_threshold_values() {
    std::cout << "Testing threshold values..." << std::endl;

    for (size_t bits : {size_t{20}, size_t{50}, size_t{80}, size_t{120}, size_t{200}}) {
        auto p = GNFSParams::compute(bits);
        // Thresholds must be non-zero (otherwise sieve accepts nothing)
        GNFS_TEST_CHECK(p.rational_threshold > 0);
        GNFS_TEST_CHECK(p.algebraic_threshold > 0);
        // Thresholds stored as uint16_t, must fit
        GNFS_TEST_CHECK(p.rational_threshold <= UINT16_MAX);
        GNFS_TEST_CHECK(p.algebraic_threshold <= UINT16_MAX);
        // For LP-enabled N, thresholds should be larger to allow LP cofactors
        if (p.large_prime_bits > 0) {
            GNFS_TEST_CHECK(p.rational_threshold > 56); // must exceed no-LP baseline
        }
    }

    std::cout << "  PASS" << std::endl;
}

void test_max_special_q() {
    std::cout << "Testing max_special_q scaling..." << std::endl;

    auto small = GNFSParams::compute(30);         // ~10 digits
    GNFS_TEST_CHECK(small.max_special_q >= 2000); // 下限保证

    auto medium = GNFSParams::compute(80);          // ~25 digits
    GNFS_TEST_CHECK(medium.max_special_q >= 20000); // 基于 est_rels 动态计算

    auto large = GNFSParams::compute(140); // ~42 digits
    GNFS_TEST_CHECK(large.max_special_q >= 100000);

    auto huge = GNFSParams::compute(200); // ~60 digits
    GNFS_TEST_CHECK(huge.max_special_q >= 1000000);

    // 单调性：更大的 N 需要更多 SQs
    GNFS_TEST_CHECK(medium.max_special_q >= small.max_special_q);
    GNFS_TEST_CHECK(large.max_special_q >= medium.max_special_q);
    GNFS_TEST_CHECK(huge.max_special_q >= large.max_special_q);

    std::cout << "  PASS" << std::endl;
}

void test_estimated_relations() {
    std::cout << "Testing estimated_relations_needed..." << std::endl;

    for (size_t bits : {size_t{30}, size_t{60}, size_t{100}}) {
        auto p = GNFSParams::compute(bits);
        size_t est = p.estimated_relations_needed();
        GNFS_TEST_CHECK(est > 0);
        // Should be reasonable: at least target_excess
        GNFS_TEST_CHECK(est > p.target_excess);
    }

    std::cout << "  PASS" << std::endl;
}

void test_sieve_memory() {
    std::cout << "Testing sieve_memory_bytes..." << std::endl;

    for (size_t bits : {size_t{30}, size_t{80}, size_t{200}}) {
        auto p = GNFSParams::compute(bits);
        size_t mem = p.sieve_memory_bytes();
        // Memory = positions × sizeof(uint16_t) = positions × 2
        GNFS_TEST_CHECK(mem == p.sieve_region_size() * sizeof(uint16_t));
        // Cap: 256M positions × 2 = 512 MB
        GNFS_TEST_CHECK(mem <= 512ULL * 1024 * 1024);
    }

    std::cout << "  PASS" << std::endl;
}

void test_polynomial_params() {
    std::cout << "Testing polynomial selection params..." << std::endl;

    for (size_t bits : {size_t{30}, size_t{80}, size_t{200}}) {
        auto p = GNFSParams::compute(bits);
        GNFS_TEST_CHECK(p.leading_coeff_bound > 0);
        GNFS_TEST_CHECK(p.search_radius >= 100);
        GNFS_TEST_CHECK(p.num_candidates >= 1000);
        GNFS_TEST_CHECK(p.skewness_steps >= 100);
    }

    // Larger N should have larger leading_coeff_bound
    auto small = GNFSParams::compute(40);
    auto big = GNFSParams::compute(200);
    GNFS_TEST_CHECK(big.leading_coeff_bound >= small.leading_coeff_bound);

    std::cout << "  PASS" << std::endl;
}

void test_extreme_bit_length_scaling() {
    std::cout << "Testing extreme bit-length scaling..." << std::endl;

    // The derived counters are uint32_t, so a huge bit length must saturate
    // instead of wrapping the intermediate size_t multiplication.
    const auto p = GNFSParams::compute((std::numeric_limits<size_t>::max)());
    GNFS_TEST_CHECK(p.num_candidates == (std::numeric_limits<uint32_t>::max)());
    GNFS_TEST_CHECK(p.skewness_steps == (std::numeric_limits<uint32_t>::max)() - 1);

    std::cout << "  PASS" << std::endl;
}

void test_qc_and_excess() {
    std::cout << "Testing QC primes and target excess..." << std::endl;

    for (size_t bits : {size_t{30}, size_t{80}, size_t{200}}) {
        auto p = GNFSParams::compute(bits);
        // ≤30 digit uses 20 QC primes (Session 51 optimization), larger uses 32-128
        GNFS_TEST_CHECK(p.num_qc_primes >= 20);
        GNFS_TEST_CHECK(p.num_qc_primes <= 128);
        GNFS_TEST_CHECK(p.target_excess >= 200);
    }

    std::cout << "  PASS" << std::endl;
}

void test_monotonicity() {
    std::cout << "Testing parameter monotonicity..." << std::endl;

    // FB bound should generally increase with N size (with exceptions for
    // optimized parameter bands, e.g., ≤25 digit uses smaller FB than ≤20 digit
    // for better LP/BL trade-off)
    uint32_t prev_fb = 0;
    for (size_t bits = 20; bits <= 200; bits += 20) {
        auto p = GNFSParams::compute(bits);
        // Allow non-monotonic FB within the small-N optimized band (≤25 digit ≈ ≤83 bit)
        if (bits > 90) {
            GNFS_TEST_CHECK(p.rational_bound >= prev_fb);
        }
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
    test_configurable_sieve_width_is_exact();
    test_large_prime_bound();
    test_lp_bits_env_override();
    test_target_multiplier_reloads_per_call();
    test_threshold_values();
    test_max_special_q();
    test_estimated_relations();
    test_sieve_memory();
    test_polynomial_params();
    test_extreme_bit_length_scaling();
    test_qc_and_excess();
    test_monotonicity();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
