// test_edge_cases.cpp — 边界/极端情况覆盖
// 覆盖 BACKLOG [TEST] 边界/极端情况覆盖率
// 涵盖：Integer 溢出/边界、负 mod、空矩阵、cofactor 边界、relation 边界、sieve 参数
#include "gnfs/core/integer.hpp"
#include "gnfs/core/relation.hpp"
#include "gnfs/linalg/sparse_matrix.hpp"
#include "gnfs/linalg/gauss.hpp"
#include "gnfs/linalg/block_lanczos.hpp"
#include "gnfs/cofactor/smooth_check.hpp"
#include "gnfs/cofactor/ecm.hpp"
#include "gnfs/cofactor/trial_division.hpp"
#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/filter.hpp"
#include "gnfs/sieve/special_q.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/factor_base/builder.hpp"
#include "gnfs/polynomial/base_m.hpp"

#include <cassert>
#include <climits>   // INT64_MAX, INT64_MIN, UINT32_MAX
#include <cstdint>
#include <iostream>
#include <sstream>

using namespace gnfs::core;
using namespace gnfs::linalg;
using namespace gnfs::cofactor;
using namespace gnfs::relation;
using namespace gnfs::sieve;

// ─── Integer 负 mod ────────────────────────────────────────────────────

// Integer::operator% 使用 mpz_tdiv_r（截断除法），结果符号与被除数一致
void test_integer_negative_mod_truncated() {
    std::cout << "Testing Integer negative mod (truncated)..." << std::endl;

    // (-7) % 3 = -1  (截断: -7/3 = -2.33 → -2, 余数 = -7 - (-2)*3 = -1)
    Integer a(int64_t(-7));
    Integer b(int64_t(3));
    Integer r = a % b;
    assert(r.to_int64() == -1);

    // 7 % (-3) = 1  (截断: 7/(-3) = -2.33 → -2, 余数 = 7 - (-2)*(-3) = 1)
    Integer c(int64_t(7));
    Integer d(int64_t(-3));
    Integer r2 = c % d;
    assert(r2.to_int64() == 1);

    // (-10) % 5 = 0  (整除)
    Integer e(int64_t(-10));
    Integer f(int64_t(5));
    Integer r3 = e % f;
    assert(r3.is_zero());

    // 10 % 3 = 1  (正常正值不受影响)
    Integer g(int64_t(10));
    Integer h(int64_t(3));
    Integer r4 = g % h;
    assert(r4.to_int64() == 1);

    // Integer::mod() static — 使用相同的 mpz_tdiv_r
    Integer result;
    Integer::mod(result, Integer(int64_t(-7)), Integer(int64_t(3)));
    assert(result.to_int64() == -1);

    // operator%= in-place
    Integer x(int64_t(-17));
    x %= Integer(int64_t(5));
    assert(x.to_int64() == -2);  // -17 = (-3)*5 + (-2)

    std::cout << "  PASS" << std::endl;
}

// ─── Integer gcd 零输入 ────────────────────────────────────────────────

void test_integer_gcd_with_zero() {
    std::cout << "Testing Integer gcd with zero inputs..." << std::endl;

    Integer zero(int64_t(0));
    Integer six(int64_t(6));

    // gcd(0, n) = |n| (GMP 约定)
    Integer g1 = gcd(zero, six);
    assert(g1.to_int64() == 6);

    // gcd(n, 0) = |n|
    Integer g2 = gcd(six, zero);
    assert(g2.to_int64() == 6);

    // gcd(0, 0) = 0 (GMP 约定)
    Integer g3 = gcd(zero, zero);
    assert(g3.is_zero());

    // gcd(1, n) = 1 for any n
    Integer one(int64_t(1));
    Integer big("123456789012345678");
    Integer g4 = gcd(one, big);
    assert(g4.to_int64() == 1);

    // gcd(n, n) = n for positive n
    Integer n7(int64_t(7));
    Integer g5 = gcd(n7, n7);
    assert(g5.to_int64() == 7);

    // gcd with large numbers
    Integer a("36893488147419103232");  // 2^65
    Integer b("18446744073709551616"); // 2^64
    Integer g6 = gcd(a, b);
    assert(g6 == b);  // gcd(2^65, 2^64) = 2^64

    std::cout << "  PASS" << std::endl;
}

// ─── Integer sqrt 边界 ─────────────────────────────────────────────────

void test_integer_sqrt_edge_cases() {
    std::cout << "Testing Integer sqrt edge cases..." << std::endl;

    // sqrt(0) = 0
    Integer s0 = sqrt(Integer(int64_t(0)));
    assert(s0.is_zero());

    // sqrt(1) = 1
    Integer s1 = sqrt(Integer(int64_t(1)));
    assert(s1.to_int64() == 1);

    // sqrt(4) = 2 (perfect square)
    Integer s2 = sqrt(Integer(int64_t(4)));
    assert(s2.to_int64() == 2);

    // sqrt(9) = 3
    Integer s3 = sqrt(Integer(int64_t(9)));
    assert(s3.to_int64() == 3);

    // sqrt(5) = 2 (floor, not a perfect square)
    Integer s4 = sqrt(Integer(int64_t(5)));
    assert(s4.to_int64() == 2);

    // sqrt(10^20) = 10^10 (exact)
    Integer big("100000000000000000000");
    Integer s5 = sqrt(big);
    assert(s5.to_string() == "10000000000");

    // sqrt(INT64_MAX) = floor(sqrt(2^63-1)) = 3037000499
    Integer imax(INT64_MAX);
    Integer s6 = sqrt(imax);
    assert(s6.to_int64() == 3037000499LL);

    std::cout << "  PASS" << std::endl;
}

// ─── Integer pow/powmod 边界 ───────────────────────────────────────────

void test_integer_pow_edge_cases() {
    std::cout << "Testing Integer pow edge cases..." << std::endl;

    Integer zero_int(int64_t(0));
    Integer one_int(int64_t(1));
    Integer two_int(int64_t(2));
    Integer mod7(int64_t(7));

    // pow(x, 0) = 1 for any x > 0
    Integer p0 = pow(two_int, 0u);
    assert(p0.to_int64() == 1);

    // pow(0, n) = 0 for n > 0
    Integer pz = pow(zero_int, 5u);
    assert(pz.is_zero());

    // pow(1, n) = 1 for any n
    Integer p1 = pow(one_int, 1000000u);
    assert(p1.to_int64() == 1);

    // pow(2, 10) = 1024
    Integer p1024 = pow(two_int, 10u);
    assert(p1024.to_int64() == 1024);

    // powmod(x, 0, m) = 1  (for x > 0)
    Integer exp0(int64_t(0));
    Integer pm_exp0 = powmod(two_int, exp0, mod7);
    assert(pm_exp0.to_int64() == 1);

    // powmod(0, n, m) = 0 for n > 0
    Integer exp3(int64_t(3));
    Integer pm_base0 = powmod(zero_int, exp3, mod7);
    assert(pm_base0.is_zero());

    // powmod(1, large, m) = 1
    Integer large_exp("99999999999");
    Integer pm_one = powmod(one_int, large_exp, mod7);
    assert(pm_one.to_int64() == 1);

    // powmod(2, 10, 1000) = 24  (normal case as sanity check)
    Integer mod1000(int64_t(1000));
    Integer exp10(int64_t(10));
    Integer pm_2_10 = powmod(two_int, exp10, mod1000);
    assert(pm_2_10.to_int64() == 24);

    std::cout << "  PASS" << std::endl;
}

// ─── Integer 加减法 INT64 边界 ────────────────────────────────────────

void test_integer_add_sub_boundary() {
    std::cout << "Testing Integer add/sub near INT64 boundary..." << std::endl;

    // INT64_MAX + 1 → 2^63 (no GMP overflow)
    Integer maxint(INT64_MAX);
    maxint += Integer(int64_t(1));
    assert(maxint.to_string() == "9223372036854775808");
    assert(maxint.is_positive());
    assert(!maxint.fits_int64());

    // INT64_MIN - 1 → -2^63 - 1 (no GMP overflow)
    Integer minint(INT64_MIN);
    minint -= Integer(int64_t(1));
    assert(minint.to_string() == "-9223372036854775809");
    assert(minint.is_negative());

    // Negate INT64_MIN → 2^63 (doesn't fit int64)
    Integer negmin(INT64_MIN);
    Integer neg_of_min = -negmin;
    assert(neg_of_min.is_positive());
    assert(neg_of_min.to_string() == "9223372036854775808");
    assert(!neg_of_min.fits_int64());

    // INT64_MIN + INT64_MIN = -2^64 (no overflow via GMP)
    Integer minint2(INT64_MIN);
    minint2 += Integer(INT64_MIN);
    assert(minint2.to_string() == "-18446744073709551616");

    std::cout << "  PASS" << std::endl;
}

// ─── SparseRow 空行边界 ────────────────────────────────────────────────

void test_sparse_row_empty() {
    std::cout << "Testing SparseRow empty behavior..." << std::endl;

    SparseRow empty_row;

    assert(empty_row.empty());
    assert(empty_row.weight() == 0);

    // first_nonzero() → UINT32_MAX (sentinel for "no elements")
    assert(empty_row.first_nonzero() == UINT32_MAX);
    assert(empty_row.last_nonzero() == UINT32_MAX);

    // test() on any col → false
    assert(!empty_row.test(0));
    assert(!empty_row.test(1000000));

    // xor_with empty → stays empty
    SparseRow other_empty;
    empty_row.xor_with(other_empty);
    assert(empty_row.empty());

    // SparseRow::set() toggle semantics (BACKLOG: "重复 set 等价于 clear"):
    // Double set WITHOUT calling ensure_sorted() in between → both appended as
    // duplicates → ensure_sorted() removes even-count entries (GF(2) cancellation).
    // IMPORTANT: calling test()/weight() between sets resets sorted_=true which
    // makes the second set() idempotent. The toggle only works without intervening
    // sorted-state operations.
    {
        SparseRow row;
        row.set(5);   // appended; sorted_=false now
        row.set(5);   // sorted_=false → appended without dedup → indices=[5,5]
        // weight() calls ensure_sorted(): [5,5] → count=2 (even) → removed
        assert(row.weight() == 0);
        assert(!row.test(5));
    }

    std::cout << "  PASS" << std::endl;
}

// ─── SparseMatrix 零维矩阵 ────────────────────────────────────────────

void test_sparse_matrix_zero_rows() {
    std::cout << "Testing SparseMatrix with 0 rows..." << std::endl;

    SparseMatrix m(0, 5);
    assert(m.num_rows() == 0);
    assert(m.num_cols() == 5);
    assert(m.total_weight() == 0);

    // transpose of 0×5 → 5×0
    SparseMatrix t = m.transpose();
    assert(t.num_rows() == 5);
    assert(t.num_cols() == 0);
    assert(t.total_weight() == 0);

    std::cout << "  PASS" << std::endl;
}

void test_sparse_matrix_zero_cols() {
    std::cout << "Testing SparseMatrix with 0 cols..." << std::endl;

    SparseMatrix m(3, 0);
    assert(m.num_rows() == 3);
    assert(m.num_cols() == 0);
    assert(m.total_weight() == 0);

    std::cout << "  PASS" << std::endl;
}

void test_sparse_matrix_one_by_one_zero() {
    std::cout << "Testing 1x1 all-zero SparseMatrix with Gaussian..." << std::endl;

    SparseMatrix m(1, 1);
    assert(!m.test(0, 0));
    assert(m.total_weight() == 0);

    // Gaussian on 1×1 zero matrix → rank 0
    GaussianEliminator elim;
    auto result = elim.eliminate(m);
    assert(result.rank == 0);

    std::cout << "  PASS" << std::endl;
}

void test_sparse_matrix_one_by_one_one() {
    std::cout << "Testing 1x1 SparseMatrix [[1]] with Gaussian..." << std::endl;

    SparseMatrix m(1, 1);
    m.set(0, 0);
    assert(m.test(0, 0));
    assert(m.total_weight() == 1);

    // Gaussian on 1×1 [[1]] → rank 1, no free variables
    GaussianEliminator elim;
    auto result = elim.eliminate(m);
    assert(result.rank == 1);
    assert(result.free_cols.empty());

    std::cout << "  PASS" << std::endl;
}

void test_sparse_matrix_all_zero() {
    std::cout << "Testing all-zero 5x3 SparseMatrix → rank 0..." << std::endl;

    SparseMatrix m(5, 3);
    assert(m.total_weight() == 0);

    GaussianEliminator elim;
    auto result = elim.eliminate(m);
    assert(result.rank == 0);

    std::cout << "  PASS" << std::endl;
}

// ─── BitVector 边界 ────────────────────────────────────────────────────

void test_bitvector_empty() {
    std::cout << "Testing BitVector(0) empty..." << std::endl;

    BitVector bv(0);
    assert(bv.size() == 0);
    assert(bv.is_zero());
    assert(bv.popcount() == 0);
    assert(bv.set_bits().empty());

    // XOR two empty → still empty
    BitVector bv2(0);
    bv.xor_with(bv2);
    assert(bv.is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_bitvector_single_bit() {
    std::cout << "Testing BitVector(1) single bit..." << std::endl;

    BitVector bv(1);
    assert(bv.size() == 1);
    assert(bv.is_zero());
    assert(!bv.test(0));

    bv.set(0);
    assert(bv.test(0));
    assert(bv.popcount() == 1);
    assert(!bv.is_zero());

    bv.clear(0);
    assert(!bv.test(0));
    assert(bv.is_zero());

    bv.flip(0);
    assert(bv.test(0));
    bv.flip(0);
    assert(!bv.test(0));

    std::cout << "  PASS" << std::endl;
}

void test_bitvector_word_boundaries() {
    std::cout << "Testing BitVector at word boundaries (63, 64, 65, 128)..." << std::endl;

    // BitVector(64): exactly 1 word (64 bits)
    {
        BitVector bv(64);
        assert(bv.size() == 64);
        bv.set(0);    // first bit of first word
        bv.set(63);   // last bit of first word
        assert(bv.test(0));
        assert(bv.test(63));
        assert(!bv.test(1));
        assert(bv.popcount() == 2);
    }

    // BitVector(65): 2 words; bit 64 is second word's bit 0
    {
        BitVector bv(65);
        assert(bv.size() == 65);
        bv.set(64);  // first bit of second word
        assert(bv.test(64));
        assert(!bv.test(63));  // last bit of first word should be 0
        assert(bv.popcount() == 1);
    }

    // BitVector(128): exactly 2 words
    {
        BitVector bv(128);
        assert(bv.size() == 128);
        bv.set(0);
        bv.set(63);
        bv.set(64);
        bv.set(127);
        assert(bv.popcount() == 4);
        assert(bv.test(0) && bv.test(63) && bv.test(64) && bv.test(127));
    }

    // BitVector(63): 63 bits, still fits in 1 word
    {
        BitVector bv(63);
        assert(bv.size() == 63);
        bv.set(62);  // last valid bit
        assert(bv.test(62));
        assert(bv.popcount() == 1);
    }

    std::cout << "  PASS" << std::endl;
}

void test_bitvector_xor_boundary() {
    std::cout << "Testing BitVector XOR across word boundary..." << std::endl;

    // XOR: bit 63 (last of word 0) and bit 64 (first of word 1)
    BitVector a(128);
    a.set(63);
    a.set(64);

    BitVector b(128);
    b.set(63);  // cancel bit 63
    b.set(65);  // add bit 65

    a.xor_with(b);
    // After XOR: bit 64 set, bit 65 set, bit 63 cleared
    assert(!a.test(63));
    assert(a.test(64));
    assert(a.test(65));
    assert(a.popcount() == 2);

    std::cout << "  PASS" << std::endl;
}

// ─── Gaussian 额外边界 ────────────────────────────────────────────────

void test_gaussian_identity_matrix() {
    std::cout << "Testing Gaussian on 3x3 identity → rank 3..." << std::endl;

    SparseMatrix m(3, 3);
    m.set(0, 0);
    m.set(1, 1);
    m.set(2, 2);

    GaussianEliminator elim;
    auto result = elim.eliminate(m);
    assert(result.rank == 3);
    assert(result.free_cols.empty());

    std::cout << "  PASS" << std::endl;
}

void test_gaussian_single_row() {
    std::cout << "Testing Gaussian on single non-zero row..." << std::endl;

    // 1×3 matrix [1 0 1] → rank 1, 2 free columns
    SparseMatrix m(1, 3);
    m.set(0, 0);
    m.set(0, 2);

    GaussianEliminator elim;
    auto result = elim.eliminate(m);
    assert(result.rank == 1);
    // Columns 1 and 2 are free (col 0 is pivot, col 2 is in same row but col 1 is free?)
    // Actually: pivot is col 0 (first nonzero), col 1 and col 2 have no pivot row
    // Free variables: columns with no pivot = col 1, col 2 minus the pivot cols
    // With 1 pivot and 3 cols total, 2 cols are free
    assert(result.free_cols.size() == 2);

    std::cout << "  PASS" << std::endl;
}

// ─── BlockLanczos 空矩阵 ──────────────────────────────────────────────

void test_block_lanczos_empty_matrix() {
    std::cout << "Testing BlockLanczos on empty matrix (0 rows) → no deps..." << std::endl;

    SparseMatrix m(0, 5);
    BlockLanczos solver;
    auto deps = solver.find_dependencies(m, 1);
    // 0 rows → no dependencies (handled early in find_dependencies)
    assert(deps.empty());

    std::cout << "  PASS" << std::endl;
}

void test_block_lanczos_zero_cols() {
    std::cout << "Testing BlockLanczos on matrix with 0 cols → no deps..." << std::endl;

    SparseMatrix m(3, 0);
    BlockLanczos solver;
    auto deps = solver.find_dependencies(m, 1);
    // 0 cols → early return, no dependencies
    assert(deps.empty());

    std::cout << "  PASS" << std::endl;
}

// ─── Integer 比较与 is_zero/is_positive/is_negative ──────────────────

void test_integer_sign_checks() {
    std::cout << "Testing Integer sign checks (zero, positive, negative)..." << std::endl;

    Integer zero_val(int64_t(0));
    assert(zero_val.is_zero());
    assert(!zero_val.is_positive());
    assert(!zero_val.is_negative());

    Integer pos(int64_t(1));
    assert(!pos.is_zero());
    assert(pos.is_positive());
    assert(!pos.is_negative());

    Integer neg(int64_t(-1));
    assert(!neg.is_zero());
    assert(!neg.is_positive());
    assert(neg.is_negative());

    // Large positive
    Integer big("99999999999999999999999999999999");
    assert(big.is_positive());
    assert(!big.is_negative());

    // Large negative
    Integer nbig("-99999999999999999999999999999999");
    assert(nbig.is_negative());
    assert(!nbig.is_positive());

    std::cout << "  PASS" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// Cofactor 模块边界测试
// ═══════════════════════════════════════════════════════════════

void test_ecm_edge_cases() {
    std::cout << "Testing ECM edge cases..." << std::endl;

    // ECM with n=1 → should return nullopt (n is "one", nothing to factor)
    {
        Integer one(int64_t(1));
        auto result = ECM::factor(one);
        assert(!result.has_value());
    }

    // ECM with a prime → should return nullopt
    {
        Integer prime(int64_t(997));
        auto result = ECM::factor(prime);
        assert(!result.has_value());
    }

    // ECM with n=2 (smallest prime) → nullopt
    {
        Integer two(int64_t(2));
        auto result = ECM::factor(two);
        assert(!result.has_value());
    }

    // ECM with a known small composite → should find a factor
    {
        Integer composite(int64_t(143)); // 11 * 13
        auto result = ECM::factor(composite);
        if (result.has_value()) {
            int64_t f = result->to_int64();
            assert(f == 11 || f == 13);
        }
        // ECM may or may not succeed — non-deterministic, but should not crash
    }

    // quick_factor with small composite
    {
        Integer composite(int64_t(15)); // 3 * 5
        auto result = ECM::quick_factor(composite);
        if (result.has_value()) {
            int64_t f = result->to_int64();
            assert(f == 3 || f == 5);
        }
    }

    std::cout << "  PASS" << std::endl;
}

void test_cofactor_classify_edge_cases() {
    std::cout << "Testing cofactor classify edge cases..." << std::endl;

    // classify_cofactor(1, lpb) → Smooth
    {
        Integer one(int64_t(1));
        auto cls = classify_cofactor(one, 1000);
        assert(cls.type == CofactorClass::Smooth);
    }

    // classify_cofactor(2, lpb=1000) → Prime
    {
        Integer two(int64_t(2));
        auto cls = classify_cofactor(two, 1000);
        assert(cls.type == CofactorClass::Prime);
        assert(cls.factor1 == 2);
    }

    // classify_cofactor(4, lpb=1000) → PrimePower (2^2)
    {
        Integer four(int64_t(4));
        auto cls = classify_cofactor(four, 1000);
        assert(cls.type == CofactorClass::PrimePower);
        assert(cls.factor1 == 2);
        assert(cls.power == 2);
    }

    // classify_cofactor with lpb=1 → almost everything is TooLarge
    {
        Integer two(int64_t(2));
        auto cls = classify_cofactor(two, 1);
        assert(cls.type == CofactorClass::TooLarge);
    }

    // is_probable_prime_u64 edge: 0, 1
    assert(!is_probable_prime_u64(0));
    assert(!is_probable_prime_u64(1));
    assert(is_probable_prime_u64(2));
    assert(is_probable_prime_u64(3));

    // is_perfect_square(0)
    {
        uint64_t root;
        assert(is_perfect_square(0, root) && root == 0);
        assert(is_perfect_square(1, root) && root == 1);
    }

    // is_perfect_power(0), (1)
    {
        uint64_t base;
        uint8_t exp;
        assert(is_perfect_power(0, base, exp));
        assert(base == 0 && exp == 1);
        assert(is_perfect_power(1, base, exp));
        assert(base == 1 && exp == 1);
    }

    // pollard_rho with even number → should return 2
    assert(pollard_rho(4) == 2);
    assert(pollard_rho(6) == 2 || pollard_rho(6) == 3);

    // pollard_rho with n divisible by 3
    assert(pollard_rho(9) == 3);

    std::cout << "  PASS" << std::endl;
}

void test_trial_division_edge_cases() {
    std::cout << "Testing trial division edge cases..." << std::endl;

    // Build a minimal factor base for N=143
    Integer n("143");
    auto bm_result = gnfs::polynomial::BaseMSelector::select(n, 2);
    assert(bm_result.success);
    auto ctx = gnfs::polynomial::BaseMSelector::create_context(n, bm_result);

    gnfs::factor_base::FactorBaseBuilder::Options opts;
    opts.rational_bound = 50;
    opts.algebraic_bound = 50;
    opts.parallel = false;
    auto fb = gnfs::factor_base::FactorBaseBuilder::build(ctx, opts);

    TrialDivider divider(fb);

    // divide_rational with value=0 → smooth (cofactor=1)
    {
        Integer zero(int64_t(0));
        auto result = divider.divide_rational(std::move(zero));
        assert(result.is_smooth);
    }

    // divide_rational with value=1 → smooth (cofactor=1)
    {
        Integer one(int64_t(1));
        auto result = divider.divide_rational(std::move(one));
        assert(result.is_smooth || result.cofactor.to_uint64() == 1);
    }

    // divide_rational with negative value → should handle sign
    {
        Integer neg(int64_t(-30)); // 2 * 3 * 5
        auto result = divider.divide_rational(std::move(neg));
        assert(result.is_smooth);
    }

    // divide_rational with a large prime > factor base → not smooth
    {
        Integer large(int64_t(10007)); // prime > 50
        auto result = divider.divide_rational(std::move(large));
        assert(!result.is_smooth);
        assert(result.cofactor.to_uint64() == 10007);
    }

    std::cout << "  PASS" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// Relation 模块边界测试
// ═══════════════════════════════════════════════════════════════

void test_relation_serialization_edge_cases() {
    std::cout << "Testing Relation serialization edge cases..." << std::endl;

    // Relation with no factors at all
    {
        Relation r(int64_t(0), int64_t(1));
        std::stringstream ss;
        r.serialize(ss);
        auto r2 = Relation::deserialize(ss);
        assert(r2.a == 0);
        assert(r2.b == 1);
        assert(r2.rational_factors.empty());
        assert(r2.algebraic_factors.empty());
        assert(r2.rational_large_prime.empty());
        assert(r2.algebraic_large_prime.empty());
    }

    // Relation with negative a
    {
        Relation r(int64_t(-12345), int64_t(7));
        r.rational_factors = {0, 1, 2};
        r.algebraic_factors = {3, 4};
        std::stringstream ss;
        r.serialize(ss);
        auto r2 = Relation::deserialize(ss);
        assert(r2.a == -12345);
        assert(r2.b == 7);
        assert(r2.rational_factors.size() == 3);
        assert(r2.algebraic_factors.size() == 2);
    }

    // Relation with large primes round-trip
    {
        Relation r(int64_t(42), int64_t(1));
        r.rational_large_prime.push_back(PrimePower{999983ULL, 0, 1});
        r.algebraic_large_prime.push_back(PrimePower{999979ULL, 100, 1});
        std::stringstream ss;
        r.serialize(ss);
        auto r2 = Relation::deserialize(ss);
        assert(r2.rational_large_prime.size() == 1);
        assert(r2.rational_large_prime[0].p == 999983ULL);
        assert(r2.algebraic_large_prime.size() == 1);
        assert(r2.algebraic_large_prime[0].p == 999979ULL);
    }

    std::cout << "  PASS" << std::endl;
}

void test_relation_collector_edge_cases() {
    std::cout << "Testing RelationCollector edge cases..." << std::endl;

    // Empty collector
    {
        RelationCollector c;
        assert(c.size() == 0);
        assert(c.relations().empty());
    }

    // Collector with duplicate rejection
    {
        CollectorConfig cfg;
        cfg.check_duplicates = true;
        RelationCollector c(cfg);

        Relation r1(int64_t(5), int64_t(1));
        Relation r2(int64_t(5), int64_t(1)); // same (a,b)
        c.add(std::move(r1));
        c.add(std::move(r2));
        assert(c.size() == 1); // duplicate rejected
        assert(c.stats().duplicates_rejected == 1);
    }

    // Collector with max_relations limit
    {
        CollectorConfig cfg;
        cfg.max_relations = 2;
        cfg.check_duplicates = false;
        RelationCollector c(cfg);

        for (int64_t i = 1; i <= 5; ++i) {
            Relation r(i, int64_t(1));
            c.add(std::move(r));
        }
        assert(c.size() == 2); // limited to 2
    }

    // Merge two collectors
    {
        RelationCollector c1, c2;
        Relation r1(int64_t(1), int64_t(1));
        Relation r2(int64_t(2), int64_t(1));
        c1.add(std::move(r1));
        c2.add(std::move(r2));
        c1.merge(c2);
        assert(c1.size() == 2);
    }

    std::cout << "  PASS" << std::endl;
}

void test_relation_filter_edge_cases() {
    std::cout << "Testing RelationFilter edge cases..." << std::endl;

    // Filter with empty input
    {
        FilterConfig cfg;
        cfg.remove_singletons = true;
        RelationFilter filter(cfg);
        std::vector<Relation> empty;
        auto filtered = filter.filter(std::move(empty));
        assert(filtered.empty());
        assert(filter.stats().input_relations == 0);
    }

    // Filter with all singletons → empty result
    {
        FilterConfig cfg;
        cfg.remove_singletons = true;
        RelationFilter filter(cfg);
        std::vector<Relation> rels;
        for (int i = 0; i < 5; ++i) {
            Relation r(int64_t(i + 1), int64_t(1));
            r.rational_large_prime.push_back(
                PrimePower{uint64_t(10007 + i * 2), 0, 1}); // each unique
            rels.push_back(std::move(r));
        }
        auto filtered = filter.filter(std::move(rels));
        assert(filtered.empty());
        assert(filter.stats().singletons_removed == 5);
    }

    // Filter with all full relations (no LP) → all pass
    {
        FilterConfig cfg;
        cfg.remove_singletons = true;
        RelationFilter filter(cfg);
        std::vector<Relation> rels;
        for (int i = 0; i < 3; ++i) {
            Relation r(int64_t(i + 1), int64_t(1));
            rels.push_back(std::move(r));
        }
        auto filtered = filter.filter(std::move(rels));
        assert(filtered.size() == 3);
        assert(filter.stats().singletons_removed == 0);
    }

    // Filter with singletons disabled → keeps everything
    {
        FilterConfig cfg;
        cfg.remove_singletons = false;
        RelationFilter filter(cfg);
        std::vector<Relation> rels;
        Relation r(int64_t(1), int64_t(1));
        r.rational_large_prime.push_back(PrimePower{99991ULL, 0, 1}); // singleton
        rels.push_back(std::move(r));
        auto filtered = filter.filter(std::move(rels));
        assert(filtered.size() == 1);
    }

    std::cout << "  PASS" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// Sieve 模块边界测试
// ═══════════════════════════════════════════════════════════════

void test_sieve_params_edge_cases() {
    std::cout << "Testing SieveParams edge cases..." << std::endl;

    // combined_threshold overflow: 200 + 200 = 400 → wraps to 144 in uint8_t
    {
        SieveParams params;
        params.rational_threshold = 200;
        params.algebraic_threshold = 200;
        uint8_t combined = params.combined_threshold();
        // uint8_t overflow: (200 + 200) mod 256 = 144
        assert(combined == 144);
        // This demonstrates the BACKLOG bug: combined_threshold uint8_t overflow
    }

    // Normal case: combined threshold within range
    {
        SieveParams params;
        params.rational_threshold = 70;
        params.algebraic_threshold = 70;
        assert(params.combined_threshold() == 140);
    }

    std::cout << "  PASS" << std::endl;
}

void test_special_q_edge_cases() {
    std::cout << "Testing SpecialQ edge cases..." << std::endl;

    // SpecialQ with q=0 → invalid
    {
        SpecialQ sq{0, 0, 0};
        assert(!sq.is_valid());
    }

    // SpecialQ with q=1 → invalid
    {
        SpecialQ sq{1, 0, 0};
        assert(!sq.is_valid());
    }

    // SpecialQ with q=2 → valid
    {
        SpecialQ sq{2, 1, 0};
        assert(sq.is_valid());
    }

    // SpecialQRange from_indices
    {
        auto range = SpecialQRange::from_indices(10, 20);
        assert(range.start_index == 10);
    }

    std::cout << "  PASS" << std::endl;
}

void test_quick_cofactor_check_edge_cases() {
    std::cout << "Testing quick_cofactor_check edge cases..." << std::endl;

    // cofactor = 0 (degenerate)
    {
        Integer zero(int64_t(0));
        // Implementation-specific: should this be "smooth"? Depends on definition
        // Just verify it doesn't crash
        auto result = quick_cofactor_check(zero, 1000, true);
        (void)result; // no crash is the test
    }

    // cofactor = 1 → smooth
    {
        Integer one(int64_t(1));
        assert(quick_cofactor_check(one, 1000, true));
    }

    // lpb = 0 → everything is too large
    {
        Integer two(int64_t(2));
        assert(!quick_cofactor_check(two, 0, true));
    }

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== Edge Case Tests ===" << std::endl;

    // Integer 负 mod
    test_integer_negative_mod_truncated();

    // Integer gcd 零输入
    test_integer_gcd_with_zero();

    // Integer sqrt 边界
    test_integer_sqrt_edge_cases();

    // Integer pow/powmod 边界
    test_integer_pow_edge_cases();

    // Integer 加减法 INT64 边界
    test_integer_add_sub_boundary();

    // Integer 符号检查
    test_integer_sign_checks();

    // SparseRow 空行
    test_sparse_row_empty();

    // SparseMatrix 零维
    test_sparse_matrix_zero_rows();
    test_sparse_matrix_zero_cols();
    test_sparse_matrix_one_by_one_zero();
    test_sparse_matrix_one_by_one_one();
    test_sparse_matrix_all_zero();

    // BitVector 边界
    test_bitvector_empty();
    test_bitvector_single_bit();
    test_bitvector_word_boundaries();
    test_bitvector_xor_boundary();

    // Gaussian 退化情形
    test_gaussian_identity_matrix();
    test_gaussian_single_row();

    // BlockLanczos 空矩阵
    test_block_lanczos_empty_matrix();
    test_block_lanczos_zero_cols();

    // Cofactor 边界
    test_ecm_edge_cases();
    test_cofactor_classify_edge_cases();
    test_trial_division_edge_cases();
    test_quick_cofactor_check_edge_cases();

    // Relation 边界
    test_relation_serialization_edge_cases();
    test_relation_collector_edge_cases();
    test_relation_filter_edge_cases();

    // Sieve 参数边界
    test_sieve_params_edge_cases();
    test_special_q_edge_cases();

    std::cout << "\nAll edge case tests passed!" << std::endl;
    return 0;
}
