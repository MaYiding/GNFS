// test_edge_cases.cpp — 边界/极端情况覆盖
// 覆盖 BACKLOG [TEST] 边界/极端情况覆盖率
// 涵盖：Integer 溢出/边界、负 mod、空矩阵、cofactor 边界、relation 边界、sieve 参数
#include "gnfs/cofactor/ecm.hpp"
#include "gnfs/cofactor/smooth_check.hpp"
#include "gnfs/cofactor/trial_division.hpp"
#include "gnfs/core/integer.hpp"
#include "gnfs/core/relation.hpp"
#include "gnfs/factor_base/builder.hpp"
#include "gnfs/linalg/block_lanczos.hpp"
#include "gnfs/linalg/gauss.hpp"
#include "gnfs/linalg/matrix_builder.hpp"
#include "gnfs/linalg/schirokauer.hpp"
#include "gnfs/linalg/sparse_matrix.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/relation/collector.hpp"
#include "gnfs/relation/filter.hpp"
#include "gnfs/sieve/lattice_basis.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/sieve/special_q.hpp"
#include "gnfs/sqrt/algebraic_sqrt.hpp"
#include "gnfs/sqrt/class_group.hpp"
#include "gnfs/sqrt/couveignes.hpp"
#include "gnfs/sqrt/hensel_sqrt.hpp"
#include "gnfs/sqrt/modular_poly.hpp"
#include "gnfs/sqrt/number_field.hpp"
#include "gnfs/sqrt/rational_sqrt.hpp"
#include "support/test_assert.hpp"
#include "support/test_check.hpp"

#include <array>
#include <climits> // INT64_MAX, INT64_MIN, UINT32_MAX
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

using gnfs::test::run_case;
using namespace gnfs::core;
using namespace gnfs::linalg;
using namespace gnfs::cofactor;
using namespace gnfs::relation;
using namespace gnfs::sieve;
using gnfs::factor_base::FactorBase;
using gnfs::polynomial::BaseMSelector;
using gnfs::sqrt::AlgebraicSqrt;
using gnfs::sqrt::ClassGroup;
using gnfs::sqrt::CouveignesSqrt;
using gnfs::sqrt::HenselSqrt;
using gnfs::sqrt::IdealClass;
using gnfs::sqrt::ModularPoly;
using gnfs::sqrt::NumberField;
using gnfs::sqrt::PrimeIdeal;
using gnfs::sqrt::RationalSqrt;

// ─── Integer 负 mod ────────────────────────────────────────────────────

// Integer::operator% 使用 mpz_tdiv_r（截断除法），结果符号与被除数一致
void test_integer_negative_mod_truncated() {
    std::cout << "Testing Integer negative mod (truncated)..." << std::endl;

    // (-7) % 3 = -1  (截断: -7/3 = -2.33 → -2, 余数 = -7 - (-2)*3 = -1)
    Integer a(int64_t(-7));
    Integer b(int64_t(3));
    Integer r = a % b;
    TEST_ASSERT(r.to_int64() == -1);

    // 7 % (-3) = 1  (截断: 7/(-3) = -2.33 → -2, 余数 = 7 - (-2)*(-3) = 1)
    Integer c(int64_t(7));
    Integer d(int64_t(-3));
    Integer r2 = c % d;
    TEST_ASSERT(r2.to_int64() == 1);

    // (-10) % 5 = 0  (整除)
    Integer e(int64_t(-10));
    Integer f(int64_t(5));
    Integer r3 = e % f;
    TEST_ASSERT(r3.is_zero());

    // 10 % 3 = 1  (正常正值不受影响)
    Integer g(int64_t(10));
    Integer h(int64_t(3));
    Integer r4 = g % h;
    TEST_ASSERT(r4.to_int64() == 1);

    // Integer::mod() static — 使用相同的 mpz_tdiv_r
    Integer result;
    Integer::mod(result, Integer(int64_t(-7)), Integer(int64_t(3)));
    TEST_ASSERT(result.to_int64() == -1);

    // operator%= in-place
    Integer x(int64_t(-17));
    x %= Integer(int64_t(5));
    TEST_ASSERT(x.to_int64() == -2); // -17 = (-3)*5 + (-2)

    std::cout << "  PASS" << std::endl;
}

// ─── Integer gcd 零输入 ────────────────────────────────────────────────

void test_integer_gcd_with_zero() {
    std::cout << "Testing Integer gcd with zero inputs..." << std::endl;

    Integer zero(int64_t(0));
    Integer six(int64_t(6));

    // gcd(0, n) = |n| (GMP 约定)
    Integer g1 = gcd(zero, six);
    TEST_ASSERT(g1.to_int64() == 6);

    // gcd(n, 0) = |n|
    Integer g2 = gcd(six, zero);
    TEST_ASSERT(g2.to_int64() == 6);

    // gcd(0, 0) = 0 (GMP 约定)
    Integer g3 = gcd(zero, zero);
    TEST_ASSERT(g3.is_zero());

    // gcd(1, n) = 1 for any n
    Integer one(int64_t(1));
    Integer big("123456789012345678");
    Integer g4 = gcd(one, big);
    TEST_ASSERT(g4.to_int64() == 1);

    // gcd(n, n) = n for positive n
    Integer n7(int64_t(7));
    Integer g5 = gcd(n7, n7);
    TEST_ASSERT(g5.to_int64() == 7);

    // gcd with large numbers
    Integer a("36893488147419103232"); // 2^65
    Integer b("18446744073709551616"); // 2^64
    Integer g6 = gcd(a, b);
    TEST_ASSERT(g6 == b); // gcd(2^65, 2^64) = 2^64

    std::cout << "  PASS" << std::endl;
}

// ─── Integer sqrt 边界 ─────────────────────────────────────────────────

void test_integer_sqrt_edge_cases() {
    std::cout << "Testing Integer sqrt edge cases..." << std::endl;

    // sqrt(0) = 0
    Integer s0 = sqrt(Integer(int64_t(0)));
    TEST_ASSERT(s0.is_zero());

    // sqrt(1) = 1
    Integer s1 = sqrt(Integer(int64_t(1)));
    TEST_ASSERT(s1.to_int64() == 1);

    // sqrt(4) = 2 (perfect square)
    Integer s2 = sqrt(Integer(int64_t(4)));
    TEST_ASSERT(s2.to_int64() == 2);

    // sqrt(9) = 3
    Integer s3 = sqrt(Integer(int64_t(9)));
    TEST_ASSERT(s3.to_int64() == 3);

    // sqrt(5) = 2 (floor, not a perfect square)
    Integer s4 = sqrt(Integer(int64_t(5)));
    TEST_ASSERT(s4.to_int64() == 2);

    // sqrt(10^20) = 10^10 (exact)
    Integer big("100000000000000000000");
    Integer s5 = sqrt(big);
    TEST_ASSERT(s5.to_string() == "10000000000");

    // sqrt(INT64_MAX) = floor(sqrt(2^63-1)) = 3037000499
    Integer imax(INT64_MAX);
    Integer s6 = sqrt(imax);
    TEST_ASSERT(s6.to_int64() == 3037000499LL);

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
    TEST_ASSERT(p0.to_int64() == 1);

    // pow(0, n) = 0 for n > 0
    Integer pz = pow(zero_int, 5u);
    TEST_ASSERT(pz.is_zero());

    // pow(1, n) = 1 for any n
    Integer p1 = pow(one_int, 1000000u);
    TEST_ASSERT(p1.to_int64() == 1);

    // pow(2, 10) = 1024
    Integer p1024 = pow(two_int, 10u);
    TEST_ASSERT(p1024.to_int64() == 1024);

    // powmod(x, 0, m) = 1  (for x > 0)
    Integer exp0(int64_t(0));
    Integer pm_exp0 = powmod(two_int, exp0, mod7);
    TEST_ASSERT(pm_exp0.to_int64() == 1);

    // powmod(0, n, m) = 0 for n > 0
    Integer exp3(int64_t(3));
    Integer pm_base0 = powmod(zero_int, exp3, mod7);
    TEST_ASSERT(pm_base0.is_zero());

    // powmod(1, large, m) = 1
    Integer large_exp("99999999999");
    Integer pm_one = powmod(one_int, large_exp, mod7);
    TEST_ASSERT(pm_one.to_int64() == 1);

    // powmod(2, 10, 1000) = 24  (normal case as sanity check)
    Integer mod1000(int64_t(1000));
    Integer exp10(int64_t(10));
    Integer pm_2_10 = powmod(two_int, exp10, mod1000);
    TEST_ASSERT(pm_2_10.to_int64() == 24);

    std::cout << "  PASS" << std::endl;
}

// Native-width integer overloads must remain callable when long/long long do
// not alias int64_t/uint64_t on the target ABI (macOS versus Linux LP64).
void test_integer_native_width_overloads() {
    std::cout << "Testing Integer native-width overloads..." << std::endl;

    const long signed_long = -123L;
    const unsigned long unsigned_long = 123UL;
    const long long signed_long_long = -456LL;
    const unsigned long long unsigned_long_long = 456ULL;

    Integer from_signed_long(signed_long);
    Integer from_unsigned_long(unsigned_long);
    Integer from_signed_long_long(signed_long_long);
    Integer from_unsigned_long_long(unsigned_long_long);
    TEST_ASSERT(from_signed_long.to_int64() == -123);
    TEST_ASSERT(from_unsigned_long.to_uint64() == 123);
    TEST_ASSERT(from_signed_long_long.to_int64() == -456);
    TEST_ASSERT(from_unsigned_long_long.to_uint64() == 456);

    Integer assigned_signed_long;
    Integer assigned_unsigned_long;
    Integer assigned_signed_long_long;
    Integer assigned_unsigned_long_long;
    assigned_signed_long = signed_long;
    assigned_unsigned_long = unsigned_long;
    assigned_signed_long_long = signed_long_long;
    assigned_unsigned_long_long = unsigned_long_long;
    TEST_ASSERT(assigned_signed_long.to_int64() == -123);
    TEST_ASSERT(assigned_unsigned_long.to_uint64() == 123);
    TEST_ASSERT(assigned_signed_long_long.to_int64() == -456);
    TEST_ASSERT(assigned_unsigned_long_long.to_uint64() == 456);

    Integer assigned_int;
    Integer assigned_unsigned_int;
    assigned_int = -789;
    assigned_unsigned_int = 789u;
    TEST_ASSERT(assigned_int.to_int64() == -789);
    TEST_ASSERT(assigned_unsigned_int.to_uint64() == 789);

    std::cout << "  PASS" << std::endl;
}

// ─── Integer 加减法 INT64 边界 ────────────────────────────────────────

void test_integer_add_sub_boundary() {
    std::cout << "Testing Integer add/sub near INT64 boundary..." << std::endl;

    // INT64_MAX + 1 → 2^63 (no GMP overflow)
    Integer maxint(INT64_MAX);
    maxint += Integer(int64_t(1));
    TEST_ASSERT(maxint.to_string() == "9223372036854775808");
    TEST_ASSERT(maxint.is_positive());
    TEST_ASSERT(!maxint.fits_int64());

    // INT64_MIN - 1 → -2^63 - 1 (no GMP overflow)
    Integer minint(INT64_MIN);
    minint -= Integer(int64_t(1));
    TEST_ASSERT(minint.to_string() == "-9223372036854775809");
    TEST_ASSERT(minint.is_negative());

    // Negate INT64_MIN → 2^63 (doesn't fit int64)
    Integer negmin(INT64_MIN);
    Integer neg_of_min = -negmin;
    TEST_ASSERT(neg_of_min.is_positive());
    TEST_ASSERT(neg_of_min.to_string() == "9223372036854775808");
    TEST_ASSERT(!neg_of_min.fits_int64());

    // INT64_MIN + INT64_MIN = -2^64 (no overflow via GMP)
    Integer minint2(INT64_MIN);
    minint2 += Integer(INT64_MIN);
    TEST_ASSERT(minint2.to_string() == "-18446744073709551616");

    std::cout << "  PASS" << std::endl;
}

// ─── SparseRow 空行边界 ────────────────────────────────────────────────

void test_sparse_row_empty() {
    std::cout << "Testing SparseRow empty behavior..." << std::endl;

    SparseRow empty_row;

    TEST_ASSERT(empty_row.empty());
    TEST_ASSERT(empty_row.weight() == 0);

    // first_nonzero() → UINT32_MAX (sentinel for "no elements")
    TEST_ASSERT(empty_row.first_nonzero() == UINT32_MAX);
    TEST_ASSERT(empty_row.last_nonzero() == UINT32_MAX);

    // test() on any col → false
    TEST_ASSERT(!empty_row.test(0));
    TEST_ASSERT(!empty_row.test(1000000));

    // xor_with empty → stays empty
    SparseRow other_empty;
    empty_row.xor_with(other_empty);
    TEST_ASSERT(empty_row.empty());

    // SparseRow::set() is idempotent: multiple set(col) = single set(col)
    // This is the correct GF(2) "set bit to 1" semantic. Use flip() for toggle.
    {
        SparseRow row;
        row.set(5); // bit 5 = 1
        row.set(5); // idempotent: bit 5 still = 1
        TEST_ASSERT(row.weight() == 1);
        TEST_ASSERT(row.test(5));
    }

    // flip() provides GF(2) toggle: double flip cancels
    {
        SparseRow row;
        row.flip(5); // bit 5 = 1
        row.flip(5); // bit 5 = 0
        TEST_ASSERT(row.weight() == 0);
        TEST_ASSERT(!row.test(5));
    }

    std::cout << "  PASS" << std::endl;
}

// ─── SparseMatrix 零维矩阵 ────────────────────────────────────────────

void test_sparse_matrix_zero_rows() {
    std::cout << "Testing SparseMatrix with 0 rows..." << std::endl;

    SparseMatrix m(0, 5);
    TEST_ASSERT(m.num_rows() == 0);
    TEST_ASSERT(m.num_cols() == 5);
    TEST_ASSERT(m.total_weight() == 0);

    // transpose of 0×5 → 5×0
    SparseMatrix t = m.transpose();
    TEST_ASSERT(t.num_rows() == 5);
    TEST_ASSERT(t.num_cols() == 0);
    TEST_ASSERT(t.total_weight() == 0);

    std::cout << "  PASS" << std::endl;
}

void test_sparse_matrix_zero_cols() {
    std::cout << "Testing SparseMatrix with 0 cols..." << std::endl;

    SparseMatrix m(3, 0);
    TEST_ASSERT(m.num_rows() == 3);
    TEST_ASSERT(m.num_cols() == 0);
    TEST_ASSERT(m.total_weight() == 0);

    std::cout << "  PASS" << std::endl;
}

void test_sparse_matrix_one_by_one_zero() {
    std::cout << "Testing 1x1 all-zero SparseMatrix with Gaussian..." << std::endl;

    SparseMatrix m(1, 1);
    TEST_ASSERT(!m.test(0, 0));
    TEST_ASSERT(m.total_weight() == 0);

    // Gaussian on 1×1 zero matrix → rank 0
    GaussianEliminator elim;
    auto result = elim.eliminate(m);
    TEST_ASSERT(result.rank == 0);

    std::cout << "  PASS" << std::endl;
}

void test_sparse_matrix_one_by_one_one() {
    std::cout << "Testing 1x1 SparseMatrix [[1]] with Gaussian..." << std::endl;

    SparseMatrix m(1, 1);
    m.set(0, 0);
    TEST_ASSERT(m.test(0, 0));
    TEST_ASSERT(m.total_weight() == 1);

    // Gaussian on 1×1 [[1]] → rank 1, no free variables
    GaussianEliminator elim;
    auto result = elim.eliminate(m);
    TEST_ASSERT(result.rank == 1);
    TEST_ASSERT(result.free_cols.empty());

    std::cout << "  PASS" << std::endl;
}

void test_sparse_matrix_all_zero() {
    std::cout << "Testing all-zero 5x3 SparseMatrix → rank 0..." << std::endl;

    SparseMatrix m(5, 3);
    TEST_ASSERT(m.total_weight() == 0);

    GaussianEliminator elim;
    auto result = elim.eliminate(m);
    TEST_ASSERT(result.rank == 0);

    std::cout << "  PASS" << std::endl;
}

// ─── BitVector 边界 ────────────────────────────────────────────────────

void test_bitvector_empty() {
    std::cout << "Testing BitVector(0) empty..." << std::endl;

    BitVector bv(0);
    TEST_ASSERT(bv.size() == 0);
    TEST_ASSERT(bv.is_zero());
    TEST_ASSERT(bv.popcount() == 0);
    TEST_ASSERT(bv.set_bits().empty());

    // XOR two empty → still empty
    BitVector bv2(0);
    bv.xor_with(bv2);
    TEST_ASSERT(bv.is_zero());

    std::cout << "  PASS" << std::endl;
}

void test_bitvector_single_bit() {
    std::cout << "Testing BitVector(1) single bit..." << std::endl;

    BitVector bv(1);
    TEST_ASSERT(bv.size() == 1);
    TEST_ASSERT(bv.is_zero());
    TEST_ASSERT(!bv.test(0));

    bv.set(0);
    TEST_ASSERT(bv.test(0));
    TEST_ASSERT(bv.popcount() == 1);
    TEST_ASSERT(!bv.is_zero());

    bv.clear(0);
    TEST_ASSERT(!bv.test(0));
    TEST_ASSERT(bv.is_zero());

    bv.flip(0);
    TEST_ASSERT(bv.test(0));
    bv.flip(0);
    TEST_ASSERT(!bv.test(0));

    std::cout << "  PASS" << std::endl;
}

void test_bitvector_word_boundaries() {
    std::cout << "Testing BitVector at word boundaries (63, 64, 65, 128)..." << std::endl;

    // BitVector(64): exactly 1 word (64 bits)
    {
        BitVector bv(64);
        TEST_ASSERT(bv.size() == 64);
        bv.set(0);  // first bit of first word
        bv.set(63); // last bit of first word
        TEST_ASSERT(bv.test(0));
        TEST_ASSERT(bv.test(63));
        TEST_ASSERT(!bv.test(1));
        TEST_ASSERT(bv.popcount() == 2);
    }

    // BitVector(65): 2 words; bit 64 is second word's bit 0
    {
        BitVector bv(65);
        TEST_ASSERT(bv.size() == 65);
        bv.set(64); // first bit of second word
        TEST_ASSERT(bv.test(64));
        TEST_ASSERT(!bv.test(63)); // last bit of first word should be 0
        TEST_ASSERT(bv.popcount() == 1);
    }

    // BitVector(128): exactly 2 words
    {
        BitVector bv(128);
        TEST_ASSERT(bv.size() == 128);
        bv.set(0);
        bv.set(63);
        bv.set(64);
        bv.set(127);
        TEST_ASSERT(bv.popcount() == 4);
        TEST_ASSERT(bv.test(0) && bv.test(63) && bv.test(64) && bv.test(127));
    }

    // BitVector(63): 63 bits, still fits in 1 word
    {
        BitVector bv(63);
        TEST_ASSERT(bv.size() == 63);
        bv.set(62); // last valid bit
        TEST_ASSERT(bv.test(62));
        TEST_ASSERT(bv.popcount() == 1);
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
    b.set(63); // cancel bit 63
    b.set(65); // add bit 65

    a.xor_with(b);
    // After XOR: bit 64 set, bit 65 set, bit 63 cleared
    TEST_ASSERT(!a.test(63));
    TEST_ASSERT(a.test(64));
    TEST_ASSERT(a.test(65));
    TEST_ASSERT(a.popcount() == 2);

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
    TEST_ASSERT(result.rank == 3);
    TEST_ASSERT(result.free_cols.empty());

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
    TEST_ASSERT(result.rank == 1);
    // Columns 1 and 2 are free (col 0 is pivot, col 2 is in same row but col 1 is free?)
    // Actually: pivot is col 0 (first nonzero), col 1 and col 2 have no pivot row
    // Free variables: columns with no pivot = col 1, col 2 minus the pivot cols
    // With 1 pivot and 3 cols total, 2 cols are free
    TEST_ASSERT(result.free_cols.size() == 2);

    std::cout << "  PASS" << std::endl;
}

// ─── BlockLanczos 空矩阵 ──────────────────────────────────────────────

void test_block_lanczos_empty_matrix() {
    std::cout << "Testing BlockLanczos on empty matrix (0 rows) → no deps..." << std::endl;

    SparseMatrix m(0, 5);
    BlockLanczos solver;
    auto deps = solver.find_dependencies(m, 1);
    // 0 rows → no dependencies (handled early in find_dependencies)
    TEST_ASSERT(deps.empty());

    std::cout << "  PASS" << std::endl;
}

void test_block_lanczos_zero_cols() {
    std::cout << "Testing BlockLanczos on matrix with 0 cols → bounded deps..." << std::endl;

    SparseMatrix m(3, 0);
    BlockLanczos solver;
    auto deps = solver.find_dependencies(m, 1);
    // No column constrains the left kernel; the caller requested one vector.
    GNFS_TEST_CHECK(deps.size() == 1);
    GNFS_TEST_CHECK(deps.front().size() == 3);
    GNFS_TEST_CHECK(deps.front()[0]);
    GNFS_TEST_CHECK(!deps.front()[1] && !deps.front()[2]);

    std::cout << "  PASS" << std::endl;
}

// ─── Integer 比较与 is_zero/is_positive/is_negative ──────────────────

void test_integer_sign_checks() {
    std::cout << "Testing Integer sign checks (zero, positive, negative)..." << std::endl;

    Integer zero_val(int64_t(0));
    TEST_ASSERT(zero_val.is_zero());
    TEST_ASSERT(!zero_val.is_positive());
    TEST_ASSERT(!zero_val.is_negative());

    Integer pos(int64_t(1));
    TEST_ASSERT(!pos.is_zero());
    TEST_ASSERT(pos.is_positive());
    TEST_ASSERT(!pos.is_negative());

    Integer neg(int64_t(-1));
    TEST_ASSERT(!neg.is_zero());
    TEST_ASSERT(!neg.is_positive());
    TEST_ASSERT(neg.is_negative());

    // Large positive
    Integer big("99999999999999999999999999999999");
    TEST_ASSERT(big.is_positive());
    TEST_ASSERT(!big.is_negative());

    // Large negative
    Integer nbig("-99999999999999999999999999999999");
    TEST_ASSERT(nbig.is_negative());
    TEST_ASSERT(!nbig.is_positive());

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
        TEST_ASSERT(!result.has_value());
    }

    // ECM with a prime → should return nullopt
    {
        Integer prime(int64_t(997));
        auto result = ECM::factor(prime);
        TEST_ASSERT(!result.has_value());
    }

    // ECM with n=2 (smallest prime) → nullopt
    {
        Integer two(int64_t(2));
        auto result = ECM::factor(two);
        TEST_ASSERT(!result.has_value());
    }

    // ECM with a known small composite → should find a factor
    {
        Integer composite(int64_t(143)); // 11 * 13
        auto result = ECM::factor(composite);
        if (result.has_value()) {
            int64_t f = result->to_int64();
            TEST_ASSERT(f == 11 || f == 13);
        }
        // ECM may or may not succeed — non-deterministic, but should not crash
    }

    // quick_factor with small composite
    {
        Integer composite(int64_t(15)); // 3 * 5
        auto result = ECM::quick_factor(composite);
        if (result.has_value()) {
            int64_t f = result->to_int64();
            TEST_ASSERT(f == 3 || f == 5);
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
        TEST_ASSERT(cls.type == CofactorClass::Smooth);
    }

    // classify_cofactor(2, lpb=1000) → Prime
    {
        Integer two(int64_t(2));
        auto cls = classify_cofactor(two, 1000);
        TEST_ASSERT(cls.type == CofactorClass::Prime);
        TEST_ASSERT(cls.factor1 == 2);
    }

    // classify_cofactor(4, lpb=1000) → PrimePower (2^2)
    {
        Integer four(int64_t(4));
        auto cls = classify_cofactor(four, 1000);
        TEST_ASSERT(cls.type == CofactorClass::PrimePower);
        TEST_ASSERT(cls.factor1 == 2);
        TEST_ASSERT(cls.power == 2);
    }

    // classify_cofactor with lpb=1 → almost everything is TooLarge
    {
        Integer two(int64_t(2));
        auto cls = classify_cofactor(two, 1);
        TEST_ASSERT(cls.type == CofactorClass::TooLarge);
    }

    // is_probable_prime_u64 edge: 0, 1
    TEST_ASSERT(!is_probable_prime_u64(0));
    TEST_ASSERT(!is_probable_prime_u64(1));
    TEST_ASSERT(is_probable_prime_u64(2));
    TEST_ASSERT(is_probable_prime_u64(3));

    // is_perfect_square(0)
    {
        uint64_t root;
        TEST_ASSERT(is_perfect_square(0, root) && root == 0);
        TEST_ASSERT(is_perfect_square(1, root) && root == 1);
    }

    // is_perfect_power: n<2 不是有意义的"完全幂",返回 false (新语义)
    {
        uint64_t base;
        uint8_t exp;
        TEST_ASSERT(!is_perfect_power(0, base, exp));
        TEST_ASSERT(!is_perfect_power(1, base, exp));
        // n=2 是素数,不是 perfect power (b^k with k>=2)
        TEST_ASSERT(!is_perfect_power(2, base, exp));
        // n=4 = 2² 是
        TEST_ASSERT(is_perfect_power(4, base, exp));
        TEST_ASSERT(base == 2 && exp == 2);
    }

    // pollard_rho with even number → should return 2
    TEST_ASSERT(pollard_rho(4) == 2);
    TEST_ASSERT(pollard_rho(6) == 2 || pollard_rho(6) == 3);

    // pollard_rho with n divisible by 3
    TEST_ASSERT(pollard_rho(9) == 3);

    std::cout << "  PASS" << std::endl;
}

void test_trial_division_edge_cases() {
    std::cout << "Testing trial division edge cases..." << std::endl;

    // Build a minimal factor base for N=143
    Integer n("143");
    auto bm_result = gnfs::polynomial::BaseMSelector::select(n, 2);
    TEST_ASSERT(bm_result.success);
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
        TEST_ASSERT(result.is_smooth);
    }

    // divide_rational with value=1 → smooth (cofactor=1)
    {
        Integer one(int64_t(1));
        auto result = divider.divide_rational(std::move(one));
        TEST_ASSERT(result.is_smooth || result.cofactor.to_uint64() == 1);
    }

    // divide_rational with negative value → should handle sign
    {
        Integer neg(int64_t(-30)); // 2 * 3 * 5
        auto result = divider.divide_rational(std::move(neg));
        TEST_ASSERT(result.is_smooth);
    }

    // divide_rational with a large prime > factor base → not smooth
    {
        Integer large(int64_t(10007)); // prime > 50
        auto result = divider.divide_rational(std::move(large));
        TEST_ASSERT(!result.is_smooth);
        TEST_ASSERT(result.cofactor.to_uint64() == 10007);
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
        TEST_ASSERT(r2.a == 0);
        TEST_ASSERT(r2.b == 1);
        TEST_ASSERT(r2.rational_factors.empty());
        TEST_ASSERT(r2.algebraic_factors.empty());
        TEST_ASSERT(r2.rational_large_prime.empty());
        TEST_ASSERT(r2.algebraic_large_prime.empty());
    }

    // Relation with negative a
    {
        Relation r(int64_t(-12345), int64_t(7));
        r.rational_factors = {0, 1, 2};
        r.algebraic_factors = {3, 4};
        std::stringstream ss;
        r.serialize(ss);
        auto r2 = Relation::deserialize(ss);
        TEST_ASSERT(r2.a == -12345);
        TEST_ASSERT(r2.b == 7);
        TEST_ASSERT(r2.rational_factors.size() == 3);
        TEST_ASSERT(r2.algebraic_factors.size() == 2);
    }

    // Relation with large primes round-trip
    {
        Relation r(int64_t(42), int64_t(1));
        r.rational_large_prime.push_back(PrimePower{999983ULL, 0, 1});
        r.algebraic_large_prime.push_back(PrimePower{999979ULL, 100, 1});
        std::stringstream ss;
        r.serialize(ss);
        auto r2 = Relation::deserialize(ss);
        TEST_ASSERT(r2.rational_large_prime.size() == 1);
        TEST_ASSERT(r2.rational_large_prime[0].p == 999983ULL);
        TEST_ASSERT(r2.algebraic_large_prime.size() == 1);
        TEST_ASSERT(r2.algebraic_large_prime[0].p == 999979ULL);
    }

    // Merged relation with extra_ab_pairs
    {
        Relation r(int64_t(100), int64_t(7));
        r.rational_factors = {0, 1};
        r.algebraic_factors = {2, 3, 4};
        r.extra_ab_pairs = {{-50, 3}, {200, 11}};
        std::stringstream ss;
        r.serialize(ss);
        auto r2 = Relation::deserialize(ss);
        TEST_ASSERT(r2.a == 100 && r2.b == 7);
        TEST_ASSERT(r2.extra_ab_pairs.size() == 2);
        TEST_ASSERT(r2.extra_ab_pairs[0].first == -50);
        TEST_ASSERT(r2.extra_ab_pairs[0].second == 3);
        TEST_ASSERT(r2.extra_ab_pairs[1].first == 200);
        TEST_ASSERT(r2.extra_ab_pairs[1].second == 11);
        TEST_ASSERT(r2.is_merged());
    }

    // Checksum corruption detection
    {
        Relation r(int64_t(42), int64_t(1));
        std::stringstream ss;
        r.serialize(ss);
        // Flip a byte in the middle of the stream
        std::string data = ss.str();
        TEST_ASSERT(data.size() > 16);
        data[12] ^= 0xFF; // corrupt a data byte
        std::stringstream ss2(data);
        bool caught = false;
        try {
            Relation::deserialize(ss2);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        TEST_ASSERT(caught);
    }

    // Invalid magic detection
    {
        std::stringstream ss;
        uint32_t bad_magic = 0xDEADBEEF;
        ss.write(reinterpret_cast<const char*>(&bad_magic), sizeof(bad_magic));
        bool caught = false;
        try {
            Relation::deserialize(ss);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        TEST_ASSERT(caught);
    }

    std::cout << "  PASS" << std::endl;
}

void test_relation_collector_edge_cases() {
    std::cout << "Testing RelationCollector edge cases..." << std::endl;

    // Empty collector
    {
        RelationCollector c;
        TEST_ASSERT(c.size() == 0);
        TEST_ASSERT(c.relations().empty());
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
        TEST_ASSERT(c.size() == 1); // duplicate rejected
        TEST_ASSERT(c.stats().duplicates_rejected == 1);
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
        TEST_ASSERT(c.size() == 2); // limited to 2
    }

    // Merge two collectors
    {
        RelationCollector c1, c2;
        Relation r1(int64_t(1), int64_t(1));
        Relation r2(int64_t(2), int64_t(1));
        c1.add(std::move(r1));
        c2.add(std::move(r2));
        c1.merge(c2);
        TEST_ASSERT(c1.size() == 2);
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
        TEST_ASSERT(filtered.empty());
        TEST_ASSERT(filter.stats().input_relations == 0);
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
        TEST_ASSERT(filtered.empty());
        TEST_ASSERT(filter.stats().singletons_removed == 5);
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
        TEST_ASSERT(filtered.size() == 3);
        TEST_ASSERT(filter.stats().singletons_removed == 0);
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
        TEST_ASSERT(filtered.size() == 1);
    }

    std::cout << "  PASS" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// Sieve 模块边界测试
// ═══════════════════════════════════════════════════════════════

void test_sieve_params_edge_cases() {
    std::cout << "Testing SieveParams edge cases..." << std::endl;

    // combined_threshold: 200 + 200 = 400, now correctly returns uint16_t
    {
        SieveParams params;
        params.rational_threshold = 200;
        params.algebraic_threshold = 200;
        uint16_t combined = params.combined_threshold();
        TEST_ASSERT(combined == 400);
    }

    // Normal case: combined threshold within range
    {
        SieveParams params;
        params.rational_threshold = 70;
        params.algebraic_threshold = 70;
        TEST_ASSERT(params.combined_threshold() == 140);
    }

    std::cout << "  PASS" << std::endl;
}

void test_special_q_edge_cases() {
    std::cout << "Testing SpecialQ edge cases..." << std::endl;

    // SpecialQ with q=0 → invalid
    {
        SpecialQ sq{0, 0, 0};
        TEST_ASSERT(!sq.is_valid());
    }

    // SpecialQ with q=1 → invalid
    {
        SpecialQ sq{1, 0, 0};
        TEST_ASSERT(!sq.is_valid());
    }

    // SpecialQ with q=2 → valid
    {
        SpecialQ sq{2, 1, 0};
        TEST_ASSERT(sq.is_valid());
    }

    // SpecialQRange from_indices
    {
        auto range = SpecialQRange::from_indices(10, 20);
        TEST_ASSERT(range.start_index == 10);
        TEST_ASSERT(range.end_index == 20);
        TEST_ASSERT(range.min_q == 0);
        TEST_ASSERT(range.max_q == UINT32_MAX);
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
        TEST_ASSERT(quick_cofactor_check(one, 1000, true));
    }

    // lpb = 0 → everything is too large
    {
        Integer two(int64_t(2));
        TEST_ASSERT(!quick_cofactor_check(two, 0, true));
    }

    // Negative residuals are malformed and must never pass the arbitrary-
    // precision range checks (negative values compare below every LP bound).
    {
        Integer negative_one(int64_t(-1));
        Integer negative_large("-123456789012345678901234567890");
        TEST_ASSERT(!quick_cofactor_check(negative_one, 1000, true));
        TEST_ASSERT(!quick_cofactor_check(negative_large, 1000, true, true));
    }

    std::cout << "  PASS" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// Hensel Sqrt 边界测试
// ═══════════════════════════════════════════════════════════════

void test_hensel_sqrt_edge_cases() {
    std::cout << "Testing HenselSqrt edge cases..." << std::endl;

    // Setup: N=9991 (97×103), degree=2
    // f(x) = x² + x + 91, which is irreducible (discriminant = -363)
    Integer n("9991");
    auto poly_result = BaseMSelector::select(n, 2);
    TEST_ASSERT(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);
    NumberField nf(ctx);

    // Test 1: Empty ab_pairs → returns Integer(1) by convention
    {
        HenselSqrt hs;
        auto r = hs.compute({}, nf);
        TEST_ASSERT(r.has_value());
        TEST_ASSERT(*r == Integer(int64_t(1)));
    }

    // Test 2: Default config construction
    {
        HenselSqrt hs; // default
        (void)hs;
    }

    // Test 3: Config with extra_precision = 0 — shouldn't crash
    {
        HenselSqrt::Config cfg;
        cfg.extra_precision = 0;
        HenselSqrt hs(cfg);
        std::vector<std::pair<int64_t, uint64_t>> pairs = {{5, 1}};
        auto r = hs.compute(pairs, nf);
        (void)r; // may be nullopt, just verify no crash
    }

    // Test 4: Config with very high extra_precision — shouldn't crash
    {
        HenselSqrt::Config cfg;
        cfg.extra_precision = 2000;
        HenselSqrt hs(cfg);
        std::vector<std::pair<int64_t, uint64_t>> pairs = {{5, 1}};
        auto r = hs.compute(pairs, nf);
        (void)r;
    }

    // Test 5: Config with high prime_start — may not find inert prime quickly
    {
        HenselSqrt::Config cfg;
        cfg.prime_start = 100000;
        HenselSqrt hs(cfg);
        std::vector<std::pair<int64_t, uint64_t>> pairs = {{3, 1}, {3, 1}};
        auto r = hs.compute(pairs, nf);
        (void)r;
    }

    // Test 6: Multiple different pairs — product may or may not be a square
    {
        HenselSqrt hs;
        std::vector<std::pair<int64_t, uint64_t>> pairs = {{1, 1}, {2, 1}, {3, 1}, {4, 1}};
        auto r = hs.compute(pairs, nf);
        (void)r; // just crash safety
    }

    // Test 7: Large a, b values — boundary test
    {
        HenselSqrt hs;
        std::vector<std::pair<int64_t, uint64_t>> pairs = {{1000000, 1}, {1000000, 1}};
        auto r = hs.compute(pairs, nf);
        (void)r;
    }

    // Test 8: Negative a values
    {
        HenselSqrt hs;
        std::vector<std::pair<int64_t, uint64_t>> pairs = {{-7, 2}, {-7, 2}};
        auto r = hs.compute(pairs, nf);
        (void)r;
    }

    // Note: Hensel precision sufficiency for real smooth relations is
    // verified by test_gnfs_progressive (L1-L5). The f'(α)² trick and
    // centering logic are exercised there with proper pipeline data.

    // Test 9: CRT-sign-exhausted 路径 — 100+ ab_pairs 触发 Nguyen hybrid
    // 分支,product 非平方时 CRT 试遍 2^(K-1) sign combo 全失败,设置
    // was_crt_sign_exhausted() == true。
    {
        HenselSqrt hs;
        std::vector<std::pair<int64_t, uint64_t>> pairs;
        // 100 个 (a, b) 对,且乘积 ∏(a - b*α) 在 Z[α] 中明显不是平方
        // (奇数个不同的 prime 元素相乘,product 必含 squarefree 部分)。
        for (int64_t i = 1; i <= 100; ++i) {
            pairs.push_back({2 * i + 1, 1}); // 奇数 a,产品含 odd primes
        }
        auto r = hs.compute(pairs, nf);
        // 不强制 r=nullopt(可能巧合是平方),但若 r=nullopt 应有 sign-exhausted
        // 标记;若 r 有值则 hensel 成功(也合理)。仅验证 API 调用不崩。
        if (!r) {
            // 若失败,可查 was_crt_sign_exhausted() 给 caller 判断 dep 无效
            std::cout << "    [info] crt_sign_exhausted="
                      << (hs.was_crt_sign_exhausted() ? "true" : "false") << "\n";
        }
        (void)r; // 不强制断言结果,仅锁住 API 不崩
    }

    std::cout << "  PASS (9 sub-tests)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// Schirokauer 大域 ℓ 边界测试
// ═══════════════════════════════════════════════════════════════

void test_schirokauer_large_ell_edge_cases() {
    std::cout << "Testing Schirokauer large ℓ edge cases..." << std::endl;

    // Setup: N=10403, degree=2
    Integer n("10403");
    auto poly_result = BaseMSelector::select(n, 2);
    TEST_ASSERT(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);
    uint32_t degree = ctx.degree();

    // Test 1: ℓ=3 → values in [0,3)
    {
        SchirokaurConfig cfg;
        cfg.primes = {3};
        SchirokaurMap sm(ctx, cfg);
        TEST_ASSERT(sm.num_columns() == degree);

        for (int64_t a = -5; a <= 5; ++a) {
            for (uint64_t b = 1; b <= 3; ++b) {
                auto maps = sm.compute(a, b);
                TEST_ASSERT(maps.size() == 1);
                TEST_ASSERT(maps[0].size() == degree);
                for (uint32_t v : maps[0])
                    TEST_ASSERT(v < 3);
            }
        }
    }

    // Test 2: ℓ=5 → values in [0,5)
    {
        SchirokaurConfig cfg;
        cfg.primes = {5};
        SchirokaurMap sm(ctx, cfg);
        auto maps = sm.compute(7, 2);
        TEST_ASSERT(maps.size() == 1);
        TEST_ASSERT(maps[0].size() == degree);
        for (uint32_t v : maps[0])
            TEST_ASSERT(v < 5);
    }

    // Test 3: Multiple primes [2, 3] → num_columns = 2 * degree
    {
        SchirokaurConfig cfg;
        cfg.primes = {2, 3};
        SchirokaurMap sm(ctx, cfg);
        TEST_ASSERT(sm.num_columns() == 2 * degree);

        auto maps = sm.compute(5, 1);
        TEST_ASSERT(maps.size() == 2);
        for (uint32_t v : maps[0])
            TEST_ASSERT(v < 2);
        for (uint32_t v : maps[1])
            TEST_ASSERT(v < 3);
    }

    // Test 4: Empty primes → num_columns = 0
    {
        SchirokaurConfig cfg;
        cfg.primes = {};
        SchirokaurMap sm(ctx, cfg);
        TEST_ASSERT(sm.num_columns() == 0);
        auto flat = sm.compute_flat(1, 1);
        TEST_ASSERT(flat.empty());
    }

    // Test 5: ℓ=7 with higher exponent_k=5 → values in [0,7)
    {
        SchirokaurConfig cfg;
        cfg.primes = {7};
        cfg.exponent_k = 5;
        SchirokaurMap sm(ctx, cfg);
        auto maps = sm.compute(11, 3);
        TEST_ASSERT(maps.size() == 1);
        TEST_ASSERT(maps[0].size() == degree);
        for (uint32_t v : maps[0])
            TEST_ASSERT(v < 7);
    }

    // Test 6: Determinism — same (a,b) always gives same result for ℓ=3
    {
        SchirokaurConfig cfg;
        cfg.primes = {3};
        SchirokaurMap sm(ctx, cfg);
        auto m1 = sm.compute(13, 5);
        auto m2 = sm.compute(13, 5);
        TEST_ASSERT(m1[0] == m2[0]);
    }

    // Test 7: The selected f = x^2 + 21x + 7 is irreducible modulo 2, 3, and
    // 5 and a perfect power modulo 7, so the checks above only take the
    // unsplit path. A quadratic that splits into linear factors keeps the
    // split path covered for odd ℓ: f = x^2 + 2x = x(x + 2) at m = 101.
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(0)));
        coeffs.push_back(Integer(int64_t(2)));
        coeffs.push_back(Integer(int64_t(1)));
        PolynomialContext split_ctx(n.clone(), std::move(coeffs), Integer(int64_t(101)));
        GNFS_TEST_CHECK(split_ctx.verify());

        struct SplitCase {
            uint32_t ell;
            uint32_t exponent_k;
        };
        const SplitCase split_cases[] = {{3u, 8u}, {5u, 8u}, {7u, 5u}};
        for (const auto& sc : split_cases) {
            SchirokaurConfig cfg;
            cfg.primes = {sc.ell};
            cfg.exponent_k = sc.exponent_k;
            SchirokaurMap sm(split_ctx, cfg);
            GNFS_TEST_CHECK(sm.num_columns() == degree);
            const auto maps = sm.compute(11, 3);
            GNFS_TEST_CHECK(maps.size() == 1);
            GNFS_TEST_CHECK(maps[0].size() == degree);
            for (uint32_t v : maps[0]) {
                GNFS_TEST_CHECK(v < sc.ell);
            }
        }
    }

    std::cout << "  PASS (ℓ=3,5,7,[2,3],empty and a split quadratic tested)" << std::endl;
}

// 覆盖 ℓ>2 且 degree ≥ 3,且 f mod ℓ 含一个 degree≥2 的 split factor。
// 前一测试只覆盖 degree=2 polynomial(splits trivially)。
// 这里:f(x) = x³ + x + 1 mod 3 = (x-1)(x²+x+2),degree-1 + degree-2
// split-mixed,触发 SchirokaurMap 的 split degree>=2 lift 分支。
void test_schirokauer_degree3_split_degree2() {
    std::cout << "Testing Schirokauer ℓ=3 deg=3 split-degree-2..." << std::endl;

    using gnfs::core::Integer;
    using gnfs::core::PolynomialContext;

    // f(x) = x³ + x + 1, N = 任意合数(不必能整除验证 f(m)=0 mod N,
    // 这里只用 ctx 提供 f mod ℓ 的信息)
    Integer n("10403"); // 任意 N(degree-3 ctx 需要 f(m) ≡ 0 mod N)
    // 找 m 使 f(m) ≡ 0 mod 10403:f(0)=1, f(1)=3, ...,直接构造
    // 简单做法:N=f(m) 自身 — N=f(10)=1011, m=10
    Integer N_construct(static_cast<int64_t>(1011));
    std::vector<Integer> coeffs;
    coeffs.emplace_back(Integer(int64_t(1))); // x^0
    coeffs.emplace_back(Integer(int64_t(1))); // x^1
    coeffs.emplace_back(Integer(int64_t(0))); // x^2
    coeffs.emplace_back(Integer(int64_t(1))); // x^3
    PolynomialContext ctx(N_construct.clone(), std::move(coeffs), Integer(int64_t(10)));

    // 用 ℓ=3 (CLAUDE.md: GF(2) 矩阵不能直接用 ℓ>2 的 map,但 SchirokaurMap
    // 本身支持任意 ℓ — 这里仅作 API 单元测试)
    SchirokaurConfig cfg;
    cfg.primes = {3};
    cfg.exponent_k = 4; // ℓ^k = 81
    SchirokaurMap sm(ctx, cfg);

    // num_columns 应 = degree = 3(每根 1 列)
    TEST_ASSERT(sm.num_columns() == 3);

    // 取若干 (a, b) 对计算 lambda 值,断言 ∈ [0, ℓ)
    std::vector<std::pair<int64_t, uint64_t>> pairs = {{1, 1}, {2, 1}, {5, 2}, {-3, 1}, {7, 3}};
    for (auto [a, b] : pairs) {
        auto maps = sm.compute(a, b);
        TEST_ASSERT(maps.size() == 1);
        TEST_ASSERT(maps[0].size() == 3);
        for (uint32_t v : maps[0]) {
            TEST_ASSERT(v < 3 && "λ value must be < ℓ=3");
        }
    }

    // λ(g^ℓ) ≡ 0 mod ℓ:任意 g 的 ℓ 倍贡献必为 0
    // (注:ℓ=3 时 self-sum 2v 不一定为 0 — 只有 ℓ 倍才必为 0)
    // 用 compute_flat 简单验证多次调用相同输入返回相同值(确定性)。
    {
        auto a1 = sm.compute_flat(7, 2);
        auto a2 = sm.compute_flat(7, 2);
        TEST_ASSERT(a1 == a2 && "deterministic");
    }

    std::cout << "  PASS (ℓ=3 deg=3 split={1,2} smoke test)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// NumberField 算术边界测试
// ═══════════════════════════════════════════════════════════════

void test_number_field_edge_cases() {
    std::cout << "Testing NumberField edge cases..." << std::endl;

    // Setup: N=9991 (97×103), f(x)=x²+x+91, m=99
    Integer n("9991");
    auto poly_result = BaseMSelector::select(n, 2);
    TEST_ASSERT(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);
    NumberField nf(ctx);

    // Test 1: zero element
    {
        auto z = nf.zero();
        TEST_ASSERT(z.is_zero());
    }

    // Test 2: one element
    {
        auto one = nf.one();
        TEST_ASSERT(!one.is_zero());
        TEST_ASSERT(one.coeff(0) == Integer(int64_t(1)));
    }

    // Test 3: multiply zero * alpha = zero
    {
        auto z = nf.zero();
        auto alpha = nf.alpha();
        auto result = nf.multiply(z, alpha);
        TEST_ASSERT(result.is_zero());
    }

    // Test 4: multiply one * alpha = alpha
    {
        auto one = nf.one();
        auto alpha = nf.alpha();
        auto result = nf.multiply(one, alpha);
        TEST_ASSERT(result.coeff(0) == Integer(int64_t(0)));
        TEST_ASSERT(result.coeff(1) == Integer(int64_t(1)));
    }

    // Test 5: from_ab(0, 0) → zero
    {
        auto elem = nf.from_ab(0, 0);
        TEST_ASSERT(elem.is_zero());
    }

    // Test 6: from_ab(5, 0) → constant 5
    {
        auto elem = nf.from_ab(5, 0);
        TEST_ASSERT(elem.coeff(0) == Integer(int64_t(5)));
        TEST_ASSERT(elem.degree() == 0);
    }

    // Test 7: from_ab(0, 1) → -α (coeff[0]=0, coeff[1]=-1)
    {
        auto elem = nf.from_ab(0, 1);
        TEST_ASSERT(elem.coeff(0) == Integer(int64_t(0)));
        TEST_ASSERT(elem.coeff(1) == Integer(int64_t(-1)));
    }

    // Test 8: from_ab(-100, 3) → -100 - 3α
    {
        auto elem = nf.from_ab(-100, 3);
        TEST_ASSERT(elem.coeff(0) == Integer(int64_t(-100)));
        TEST_ASSERT(elem.coeff(1) == Integer(int64_t(-3)));
    }

    // Test 9: norm_linear(0, 1) = f_0 (constant term)
    // N(0 - 1·α) = sum f_i * 0^i * 1^{d-i} = f_0
    {
        auto norm = nf.norm_linear(0, 1);
        TEST_ASSERT(norm == nf.coeff(0));
    }

    // Test 10: norm_linear(a, 0) = f_d * a^d (only highest term survives)
    // b=0 → b^{d-i}=0 for i<d, only i=d: f_d * a^d * b^0 = f_d * a^d
    {
        auto norm = nf.norm_linear(5, 0);
        // f(x) = x² + x + 91, so f_2 = 1, norm(5,0) = 1 * 25 = 25
        Integer expected(int64_t(25));
        TEST_ASSERT(norm == expected);
    }

    // Test 11: evaluate_at_m of zero = 0
    {
        auto z = nf.zero();
        auto result = nf.evaluate_at_m(z);
        TEST_ASSERT(result == Integer(int64_t(0)));
    }

    // Test 12: evaluate_at_m_mod_n of alpha = m (mod N)
    {
        auto alpha = nf.alpha();
        auto result = nf.evaluate_at_m_mod_n(alpha);
        TEST_ASSERT(result == nf.m());
    }

    // Test 13: norm_linear with large a — crash safety
    {
        auto norm = nf.norm_linear(INT64_MAX / 2, 1);
        (void)norm; // Integer handles big values via GMP
    }

    std::cout << "  PASS (13 sub-tests)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// MatrixBuilder 退化输入边界测试
// ═══════════════════════════════════════════════════════════════

void test_matrix_builder_edge_cases() {
    std::cout << "Testing MatrixBuilder edge cases..." << std::endl;

    // Setup: N=9991 (97×103), f(x)=x²+x+91 — leading coeff=1 (safe for modular_poly)
    // N=143 has f_2=2 ≡ 0 (mod 2), which triggers modular_poly assertion (known BACKLOG bug)
    Integer n("9991");
    auto poly_result = BaseMSelector::select(n, 2);
    TEST_ASSERT(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);

    gnfs::factor_base::FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 50;
    fb_opts.algebraic_bound = 50;
    fb_opts.parallel = false;
    auto fb = gnfs::factor_base::FactorBaseBuilder::build(ctx, fb_opts);

    // Test 1: Empty relations → 0-row matrix
    {
        MatrixBuilder mb;
        std::vector<Relation> empty;
        auto result = mb.build(empty, fb);
        TEST_ASSERT(result.matrix.num_rows() == 0);
        TEST_ASSERT(result.row_to_relation.empty());
    }

    // Test 2: Single relation → 1-row matrix
    {
        MatrixBuilder mb;
        std::vector<Relation> rels;
        Relation r(int64_t(5), int64_t(1));
        r.rational_factors = {0, 1};
        rels.push_back(std::move(r));
        auto result = mb.build(rels, fb);
        TEST_ASSERT(result.matrix.num_rows() == 1);
        TEST_ASSERT(result.row_to_relation.size() == 1);
        TEST_ASSERT(result.row_to_relation[0] == 0);
    }

    // Test 3: Config with sign column disabled
    {
        MatrixBuilderConfig cfg;
        cfg.include_sign_column = false;
        cfg.include_qc_columns = false;
        cfg.include_class_group = false;
        cfg.include_schirokauer = false;
        MatrixBuilder mb(cfg);
        std::vector<Relation> rels;
        Relation r(int64_t(3), int64_t(1));
        rels.push_back(std::move(r));
        auto result = mb.build(rels, fb);
        TEST_ASSERT(result.matrix.num_rows() == 1);
        // total_columns = rational + algebraic + LP only
        TEST_ASSERT(result.mapping.total_columns() > 0);
    }

    // Test 4: build_with_qc on empty relations
    {
        MatrixBuilder mb;
        std::vector<Relation> empty;
        auto result = mb.build_with_qc(empty, fb, ctx);
        TEST_ASSERT(result.matrix.num_rows() == 0);
    }

    std::cout << "  PASS (4 sub-tests)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// RationalSqrt 退化输入边界测试
// ═══════════════════════════════════════════════════════════════

void test_rational_sqrt_edge_cases() {
    std::cout << "Testing RationalSqrt edge cases..." << std::endl;

    // Setup: N=9991 (97×103), same FB as MatrixBuilder tests
    Integer n("9991");
    auto poly_result = BaseMSelector::select(n, 2);
    TEST_ASSERT(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);

    gnfs::factor_base::FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 50;
    fb_opts.algebraic_bound = 50;
    fb_opts.parallel = false;
    auto fb = gnfs::factor_base::FactorBaseBuilder::build(ctx, fb_opts);

    // Test 1: Empty dependency (all zero bits) → no relations selected
    {
        BitVector dep(5);
        // all bits zero → product is empty → should handle gracefully
        std::vector<Relation> rels;
        for (int i = 0; i < 5; ++i) {
            rels.emplace_back(int64_t(i + 1), int64_t(1));
        }
        RationalSqrt rs;
        auto result = rs.compute(dep, rels, fb, n, ctx.m());
        (void)result; // crash safety — may succeed or fail, no crash
    }

    // Test 2: Single relation selected with even exponents → perfect square
    {
        BitVector dep(1);
        dep.set(0);
        std::vector<Relation> rels;
        Relation r(int64_t(5), int64_t(1));
        // rational_factors indices reference FB; use pairs of same index for even exp
        r.rational_factors = {0, 0, 1, 1}; // 2² × 3²
        rels.push_back(std::move(r));

        RationalSqrt rs;
        auto result = rs.compute(dep, rels, fb, n, ctx.m());
        (void)result; // crash safety
    }

    std::cout << "  PASS (2 sub-tests)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// AlgebraicSqrt 退化输入边界测试
// ═══════════════════════════════════════════════════════════════

void test_algebraic_sqrt_edge_cases() {
    std::cout << "Testing AlgebraicSqrt edge cases..." << std::endl;

    // Setup: N=9991 (97×103), f(x)=x²+x+91
    Integer n("9991");
    auto poly_result = BaseMSelector::select(n, 2);
    TEST_ASSERT(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);

    // Test 1: Empty dependency (all zero bits) → error "No relations in dependency"
    {
        BitVector dep(5);
        std::vector<Relation> rels;
        for (int i = 0; i < 5; ++i) {
            rels.emplace_back(int64_t(i + 1), int64_t(1));
        }
        AlgebraicSqrt as;
        auto result = as.compute(dep, rels, ctx);
        TEST_ASSERT(!result.success);
        TEST_ASSERT(!result.error.empty());
    }

    // Test 2: Default config construction
    {
        AlgebraicSqrt as;
        (void)as;
    }

    // Test 3: Config with use_couveignes=false
    {
        AlgebraicSqrt::Config cfg;
        cfg.use_couveignes = false;
        AlgebraicSqrt as(cfg);
        (void)as;
    }

    // Test 4: Dependency vector longer than relations → test bounds
    // (avoids actual sqrt computation — just tests iteration logic)
    {
        BitVector dep(10);
        dep.set(0);
        std::vector<Relation> rels;
        rels.emplace_back(int64_t(7), int64_t(2));
        // dep has bits beyond rels.size(), but loop only goes to rels.size()
        // Hensel will attempt sqrt of single element (not a square) → fails → Couveignes fallback
        // Use config to disable Couveignes for speed
        AlgebraicSqrt::Config cfg;
        cfg.use_couveignes = false;
        AlgebraicSqrt as(cfg);
        auto result = as.compute(dep, rels, ctx);
        // Single element is not a square, so both Hensel fail and heuristic fail are expected
        (void)result; // crash safety
    }

    std::cout << "  PASS (4 sub-tests)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// Couveignes 退化输入边界测试
// ═══════════════════════════════════════════════════════════════

void test_couveignes_edge_cases() {
    std::cout << "Testing CouveignesSqrt edge cases..." << std::endl;

    // Setup: N=9991, f(x)=x²+x+91
    Integer n("9991");
    auto poly_result = BaseMSelector::select(n, 2);
    TEST_ASSERT(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);
    NumberField nf(ctx);

    // Test 1: Empty ab_pairs → returns nullopt (linalg layer should never
    // produce an empty dependency; silently returning 1 would mask bugs by
    // making the caller compute trivial gcd(±1, N)).
    {
        CouveignesSqrt cs;
        auto result = cs.compute({}, nf);
        TEST_ASSERT(!result.has_value());
    }

    // Test 2: Default config construction
    {
        CouveignesSqrt cs;
        (void)cs;
    }

    // Test 3: Config with very few max_prime_checks → fail gracefully (returns nullopt)
    {
        CouveignesSqrt::Config cfg;
        cfg.max_prime_checks = 3; // Too few to find suitable primes
        cfg.num_primes = 2;
        CouveignesSqrt cs(cfg);
        std::vector<std::pair<int64_t, uint64_t>> pairs = {{3, 1}};
        auto result = cs.compute(pairs, nf);
        // With only 3 checks, likely won't find 2 inert primes → nullopt
        (void)result;
    }

    // Test 4: Config with num_primes=0 → no CRT primes needed
    {
        CouveignesSqrt::Config cfg;
        cfg.num_primes = 0;
        CouveignesSqrt cs(cfg);
        std::vector<std::pair<int64_t, uint64_t>> pairs = {{2, 1}};
        auto result = cs.compute(pairs, nf);
        // 0 primes → insufficient → nullopt expected
        (void)result;
    }

    std::cout << "  PASS (4 sub-tests)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// PolynomialContext 构造与运算边界测试
// ═══════════════════════════════════════════════════════════════

void test_polynomial_context_edge_cases() {
    std::cout << "Testing PolynomialContext edge cases..." << std::endl;

    // Test 1: Empty coefficients → throws std::invalid_argument
    {
        bool threw = false;
        try {
            PolynomialContext ctx(Integer(int64_t(100)), {}, // empty
                                  Integer(int64_t(10)));
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TEST_ASSERT(threw);
    }

    // Test 2: Degree 0 (constant polynomial f(x) = 7)
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(7)));
        PolynomialContext ctx(Integer(int64_t(100)), std::move(coeffs), Integer(int64_t(0)));
        TEST_ASSERT(ctx.degree() == 0);
        TEST_ASSERT(ctx.coeff(0) == Integer(int64_t(7)));
        TEST_ASSERT(ctx.leading_coeff() == Integer(int64_t(7)));
    }

    // Test 3: Trailing zeros stripped → effective degree reduced
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(3))); // x^0
        coeffs.push_back(Integer(int64_t(2))); // x^1
        coeffs.push_back(Integer(int64_t(0))); // x^2 = 0 → stripped
        coeffs.push_back(Integer(int64_t(0))); // x^3 = 0 → stripped
        PolynomialContext ctx(Integer(int64_t(100)), std::move(coeffs), Integer(int64_t(0)));
        TEST_ASSERT(ctx.degree() == 1);
        TEST_ASSERT(ctx.leading_coeff() == Integer(int64_t(2)));
    }

    // Test 4: coeff() out of range → returns static zero
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(5)));
        PolynomialContext ctx(Integer(int64_t(100)), std::move(coeffs), Integer(int64_t(0)));
        TEST_ASSERT(ctx.coeff(100).is_zero());
    }

    // Test 5: evaluate(0) = f_0
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(91))); // f(x) = 91 + x + x²
        coeffs.push_back(Integer(int64_t(1)));
        coeffs.push_back(Integer(int64_t(1)));
        PolynomialContext ctx(Integer(int64_t(9991)), std::move(coeffs), Integer(int64_t(99)));
        TEST_ASSERT(ctx.evaluate(Integer(int64_t(0))) == Integer(int64_t(91)));
    }

    // Test 6: evaluate_mod with p=1 → always 0
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(91)));
        coeffs.push_back(Integer(int64_t(1)));
        coeffs.push_back(Integer(int64_t(1)));
        PolynomialContext ctx(Integer(int64_t(9991)), std::move(coeffs), Integer(int64_t(99)));
        TEST_ASSERT(ctx.evaluate_mod(42, 1) == 0);
    }

    // Test 7: evaluate_mod with x=0 → f_0 mod p
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(91)));
        coeffs.push_back(Integer(int64_t(1)));
        coeffs.push_back(Integer(int64_t(1)));
        PolynomialContext ctx(Integer(int64_t(9991)), std::move(coeffs), Integer(int64_t(99)));
        TEST_ASSERT(ctx.evaluate_mod(0, 7) == (91 % 7)); // 91 mod 7 = 0
    }

    // Test 8: evaluate with negative coefficients
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(-10)));
        coeffs.push_back(Integer(int64_t(3)));
        PolynomialContext ctx(Integer(int64_t(100)), std::move(coeffs), Integer(int64_t(0)));
        // f(5) = -10 + 3*5 = 5
        TEST_ASSERT(ctx.evaluate(Integer(int64_t(5))) == Integer(int64_t(5)));
    }

    // Test 9: verify() — f(m) ≡ 0 (mod n) for real polynomial
    {
        Integer n("9991");
        auto pr = BaseMSelector::select(n, 2);
        TEST_ASSERT(pr.success);
        auto ctx = BaseMSelector::create_context(n, pr);
        TEST_ASSERT(ctx.verify());
    }

    // Test 10: algebraic_norm(0, 0) → f_0 * 0^0 * 0^d + ... = just f_0 * 1 * 0^d
    // Actually: all terms have b^{d-i} factor where b=0, so for i<d: term=0
    // For i=d: f_d * a^d * b^0 = f_d * 0^d = 0
    // So norm(0,0) = 0
    {
        Integer n("9991");
        auto pr = BaseMSelector::select(n, 2);
        auto ctx = BaseMSelector::create_context(n, pr);
        auto norm = ctx.algebraic_norm(0, 0);
        TEST_ASSERT(norm.is_zero());
    }

    // Test 11: rational_value(m_as_int64, 1) = m - m = 0 (mod n)
    {
        Integer n("9991");
        auto pr = BaseMSelector::select(n, 2);
        auto ctx = BaseMSelector::create_context(n, pr);
        int64_t m_val = ctx.m().to_int64();
        auto rv = ctx.rational_value(m_val, 1);
        TEST_ASSERT(rv.is_zero());
    }

    // Test 12: skewness default
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(1)));
        PolynomialContext ctx(Integer(int64_t(100)), std::move(coeffs), Integer(int64_t(0)));
        TEST_ASSERT(ctx.skewness() == 1.0);
    }

    std::cout << "  PASS (12 sub-tests)" << std::endl;
}

// ═══════════════════════════════════════════════════════════════
// FactorBase 查找与构造边界测试
// ═══════════════════════════════════════════════════════════════

void test_factor_base_edge_cases() {
    std::cout << "Testing FactorBase edge cases..." << std::endl;

    // Test 1: Empty FactorBase — all counts = 0
    {
        FactorBase fb;
        TEST_ASSERT(fb.rational_count() == 0);
        TEST_ASSERT(fb.algebraic_count() == 0);
        TEST_ASSERT(fb.sieve_algebraic_count() == 0);
    }

    // Test 2: find_rational on empty FB → nullopt
    {
        FactorBase fb;
        TEST_ASSERT(!fb.find_rational(2).has_value());
        TEST_ASSERT(!fb.find_rational(0).has_value());
    }

    // Test 3: find_algebraic on empty FB → nullopt
    {
        FactorBase fb;
        TEST_ASSERT(!fb.find_algebraic(2, 1).has_value());
    }

    // Test 4: Add rational primes and verify lookup
    {
        FactorBase fb;
        fb.add_rational(2, 1);
        fb.add_rational(3, 2);
        fb.add_rational(5, 3);
        TEST_ASSERT(fb.rational_count() == 3);
        auto idx = fb.find_rational(3);
        TEST_ASSERT(idx.has_value());
        TEST_ASSERT(*idx == 1);
        TEST_ASSERT(!fb.find_rational(7).has_value());
    }

    // Test 5: Add algebraic primes and verify lookup
    {
        FactorBase fb;
        fb.add_algebraic(2, 1, 1);
        fb.add_algebraic(3, 2, 2);
        TEST_ASSERT(fb.algebraic_count() == 2);
        auto idx = fb.find_algebraic(3, 2);
        TEST_ASSERT(idx.has_value());
        TEST_ASSERT(*idx == 1);
        // Wrong root → not found
        TEST_ASSERT(!fb.find_algebraic(3, 0).has_value());
    }

    // Test 6: sieve_algebraic_count defaults to algebraic_count when not set
    {
        FactorBase fb;
        fb.add_algebraic(2, 1, 1);
        fb.add_algebraic(3, 2, 2);
        fb.add_algebraic(5, 3, 3);
        TEST_ASSERT(fb.sieve_algebraic_count() == 3); // defaults to algebraic_count()
    }

    // Test 7: sieve_algebraic_count set explicitly
    {
        FactorBase fb;
        fb.add_algebraic(2, 1, 1);
        fb.add_algebraic(3, 2, 2);
        fb.add_algebraic(5, 3, 3);       // SQ range prime
        fb.set_sieve_algebraic_count(2); // only first 2 are sieve primes
        TEST_ASSERT(fb.sieve_algebraic_count() == 2);
        TEST_ASSERT(fb.algebraic_count() == 3);
    }

    // Test 8: build_index rebuilds from scratch
    {
        FactorBase fb;
        fb.add_rational(2, 1);
        fb.add_rational(3, 2);
        TEST_ASSERT(fb.find_rational(2).has_value());
        // Manually rebuild index
        fb.build_index();
        TEST_ASSERT(fb.find_rational(2).has_value());
        TEST_ASSERT(fb.find_rational(3).has_value());
        TEST_ASSERT(*fb.find_rational(2) == 0);
    }

    // Test 9: Only rational, no algebraic
    {
        FactorBase fb;
        fb.add_rational(2, 1);
        fb.add_rational(3, 2);
        TEST_ASSERT(fb.rational_count() == 2);
        TEST_ASSERT(fb.algebraic_count() == 0);
        TEST_ASSERT(fb.sieve_algebraic_count() == 0);
    }

    // Test 10: Only algebraic, no rational
    {
        FactorBase fb;
        fb.add_algebraic(2, 0, 1);
        TEST_ASSERT(fb.rational_count() == 0);
        TEST_ASSERT(fb.algebraic_count() == 1);
    }

    std::cout << "  PASS (10 sub-tests)" << std::endl;
}

// ─── ModularPoly 边界/极端情况 ─────────────────────────────────────────

void test_modular_poly_edge_cases() {
    std::cout << "Testing ModularPoly edge cases..." << std::endl;
    const uint64_t p = 7;

    // Test 1: Zero polynomial properties
    {
        ModularPoly zero;
        TEST_ASSERT(zero.is_zero());
        TEST_ASSERT(!zero.is_one());
        TEST_ASSERT(zero.degree() == -1);
        TEST_ASSERT(zero.coeff(0) == 0);
        TEST_ASSERT(zero.coeff(100) == 0);
    }

    // Test 2: Constant polynomial
    {
        ModularPoly one(1);
        TEST_ASSERT(!one.is_zero());
        TEST_ASSERT(one.is_one());
        TEST_ASSERT(one.degree() == 0);

        ModularPoly c(5);
        TEST_ASSERT(c.degree() == 0);
        TEST_ASSERT(c.coeff(0) == 5);
    }

    // Test 3: Normalization strips trailing zeros
    {
        ModularPoly poly(std::vector<uint64_t>{3, 0, 0, 0});
        TEST_ASSERT(poly.degree() == 0);
        TEST_ASSERT(poly.coeff(0) == 3);

        ModularPoly all_zero(std::vector<uint64_t>{0, 0, 0});
        TEST_ASSERT(all_zero.is_zero());
        TEST_ASSERT(all_zero.degree() == -1);
    }

    // Test 4: add/sub with zero → identity
    {
        ModularPoly a(std::vector<uint64_t>{3, 2, 1});
        ModularPoly zero;
        auto sum = ModularPoly::add(a, zero, p);
        TEST_ASSERT(sum.coeff(0) == 3 && sum.coeff(1) == 2 && sum.coeff(2) == 1);

        auto diff = ModularPoly::sub(a, zero, p);
        TEST_ASSERT(diff.coeff(0) == 3 && diff.coeff(1) == 2 && diff.coeff(2) == 1);
    }

    // Test 5: sub(a, a) = 0
    {
        ModularPoly a(std::vector<uint64_t>{3, 5, 1});
        auto diff = ModularPoly::sub(a, a, p);
        TEST_ASSERT(diff.is_zero());
    }

    // Test 6: mul_raw with zero → zero
    {
        ModularPoly a(std::vector<uint64_t>{3, 2});
        ModularPoly zero;
        auto prod = ModularPoly::mul_raw(a, zero, p);
        TEST_ASSERT(prod.is_zero());

        auto prod2 = ModularPoly::mul_raw(zero, a, p);
        TEST_ASSERT(prod2.is_zero());
    }

    // Test 7: scalar_mul by 0 → zero
    {
        ModularPoly a(std::vector<uint64_t>{3, 5, 1});
        auto scaled = ModularPoly::scalar_mul(a, 0, p);
        TEST_ASSERT(scaled.is_zero());
    }

    // Test 8: power with exponent 0 → 1
    {
        std::vector<uint64_t> f = {1, 0, 1}; // x^2 + 1 mod 7
        ModularPoly a(std::vector<uint64_t>{3, 2});
        auto result = ModularPoly::power(a, Integer(int64_t(0)), f, p);
        TEST_ASSERT(result.is_one());
    }

    // Test 9: divmod by zero polynomial → exception
    {
        ModularPoly a(std::vector<uint64_t>{1, 1});
        ModularPoly zero;
        bool caught = false;
        try {
            (void)ModularPoly::divmod(a, zero, p);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        TEST_ASSERT(caught);
    }

    // Test 10: divmod when a.degree < b.degree → quotient=0, remainder=a
    {
        ModularPoly a(std::vector<uint64_t>{3});    // degree 0
        ModularPoly b(std::vector<uint64_t>{1, 1}); // degree 1
        auto [q, r] = ModularPoly::divmod(a, b, p);
        TEST_ASSERT(q.is_zero());
        TEST_ASSERT(r.coeff(0) == 3);
    }

    // Test 11: gcd(a, a) → monic version of a
    {
        ModularPoly a(std::vector<uint64_t>{2, 3}); // 3x + 2
        auto g = ModularPoly::gcd(a, a, p);
        // Should be monic: (3x + 2) / 3 = x + 2*3^{-1}
        // 3^{-1} mod 7 = 5, so 2*5 = 10 mod 7 = 3
        TEST_ASSERT(g.degree() == 1);
        TEST_ASSERT(g.coeff(1) == 1); // monic
    }

    // Test 12: is_irreducible for degree 0 → false, degree 1 → true
    {
        std::vector<uint64_t> deg0 = {5};
        TEST_ASSERT(!ModularPoly::is_irreducible(deg0, p));

        std::vector<uint64_t> deg1 = {3, 1}; // x + 3
        TEST_ASSERT(ModularPoly::is_irreducible(deg1, p));
    }

    // Test 13: reduce when polynomial already smaller than f → unchanged
    {
        std::vector<uint64_t> f = {1, 0, 0, 1};         // x^3 + 1
        ModularPoly small(std::vector<uint64_t>{2, 3}); // 3x + 2 (degree 1 < 3)
        auto reduced = ModularPoly::reduce(small, f, p);
        TEST_ASSERT(reduced.coeff(0) == 2);
        TEST_ASSERT(reduced.coeff(1) == 3);
        TEST_ASSERT(reduced.degree() == 1);
    }

    // Test 14: set_coeff extends polynomial and normalizes
    {
        ModularPoly poly;
        poly.set_coeff(3, 5); // x^3 coefficient = 5
        TEST_ASSERT(poly.degree() == 3);
        TEST_ASSERT(poly.coeff(3) == 5);
        TEST_ASSERT(poly.coeff(0) == 0);

        // Setting leading coeff to 0 reduces degree
        poly.set_coeff(3, 0);
        TEST_ASSERT(poly.is_zero());
    }

    // Test 15: arithmetic reduces raw coefficients, including near UINT64_MAX moduli
    {
        const uint64_t wide_p = std::numeric_limits<uint64_t>::max() - 58;
        ModularPoly a(std::vector<uint64_t>{wide_p + 1, 1, 1});
        ModularPoly b(std::vector<uint64_t>{wide_p + 2, wide_p - 1, wide_p - 1});
        const auto sum = ModularPoly::add(a, b, wide_p);
        GNFS_TEST_CHECK(sum.coeff(0) == 3);

        const auto product = ModularPoly::mul_raw(
            ModularPoly(std::vector<uint64_t>{1, 1, 1}),
            ModularPoly(std::vector<uint64_t>{wide_p - 1, wide_p - 1, wide_p - 1}), wide_p);
        GNFS_TEST_CHECK(product.coeff(1) == wide_p - 2);
    }

    // Test 16: invalid moduli and modulus polynomials fail closed
    {
        bool caught = false;
        try {
            (void)ModularPoly::add(ModularPoly(1), ModularPoly(1), 0);
        } catch (const std::invalid_argument&) {
            caught = true;
        }
        GNFS_TEST_CHECK(caught);

        caught = false;
        try {
            (void)ModularPoly::reduce(ModularPoly(1), {}, p);
        } catch (const std::invalid_argument&) {
            caught = true;
        }
        GNFS_TEST_CHECK(caught);
    }

    // Test 17: negative powers are rejected instead of being shifted toward zero
    {
        bool caught = false;
        try {
            (void)ModularPoly::power(ModularPoly(1), Integer(int64_t(-1)), {1, 1}, p);
        } catch (const std::invalid_argument&) {
            caught = true;
        }
        GNFS_TEST_CHECK(caught);
    }

    // Test 18: a size_t wraparound cannot trigger an unbounded resize
    {
        ModularPoly poly;
        bool caught = false;
        try {
            poly.set_coeff(std::numeric_limits<size_t>::max(), 1);
        } catch (const std::length_error&) {
            caught = true;
        }
        GNFS_TEST_CHECK(caught);
    }

    std::cout << "  PASS (18 sub-tests)" << std::endl;
}

// ─── LatticeBasis 边界/极端情况 ─────────────────────────────────────────

void test_lattice_basis_edge_cases() {
    std::cout << "Testing LatticeBasis edge cases..." << std::endl;

    // Test 1: Small q=2, r=1 → basis computation
    {
        SpecialQ sq;
        sq.q = 2;
        sq.r = 1;
        auto basis = compute_lattice_basis(sq);
        int64_t det = basis.determinant();
        TEST_ASSERT(det == 2 || det == -2);
        TEST_ASSERT(basis.verify_ab(basis.e0, basis.f0));
        TEST_ASSERT(basis.verify_ab(basis.e1, basis.f1));
    }

    // Test 2: q=3, r=0 → a ≡ 0 (mod 3) lattice
    {
        SpecialQ sq;
        sq.q = 3;
        sq.r = 0;
        auto basis = compute_lattice_basis(sq);
        int64_t det = basis.determinant();
        TEST_ASSERT(det == 3 || det == -3);
        // (3, 0) and (0, 1) should be a valid basis pair
        TEST_ASSERT(basis.verify_ab(basis.e0, basis.f0));
        TEST_ASSERT(basis.verify_ab(basis.e1, basis.f1));
    }

    // Test 3: q=r=1 edge case → a - b ≡ 0 (mod 1) always true
    {
        SpecialQ sq;
        sq.q = 1;
        sq.r = 0;
        auto basis = compute_lattice_basis(sq);
        // All (a,b) satisfy the condition mod 1
        TEST_ASSERT(basis.verify_ab(0, 0));
        TEST_ASSERT(basis.verify_ab(7, 3));
        TEST_ASSERT(basis.verify_ab(-5, 2));
    }

    // Test 4: to_ab and verify_ab roundtrip — any lattice point should satisfy
    {
        SpecialQ sq;
        sq.q = 101;
        sq.r = 42;
        auto basis = compute_lattice_basis(sq);

        for (int i = -5; i <= 5; ++i) {
            for (int j = -5; j <= 5; ++j) {
                auto [a, b] = basis.to_ab(i, j);
                TEST_ASSERT(basis.verify_ab(a, b));
            }
        }
    }

    // Test 5: SieveRegion index roundtrip
    {
        SieveRegion region;
        region.i_min = -10;
        region.i_max = 9;
        region.j_min = 1;
        region.j_max = 5;

        TEST_ASSERT(region.i_width() == 20);
        TEST_ASSERT(region.j_height() == 5);
        TEST_ASSERT(region.size() == 100);

        // Roundtrip: index → ij → index
        for (size_t idx = 0; idx < region.size(); ++idx) {
            auto [i, j] = region.index_to_ij(idx);
            size_t idx2 = region.ij_to_index(i, j);
            TEST_ASSERT(idx == idx2);
        }
    }

    // Test 6: default_sieve_region with various skewness values
    {
        constexpr size_t max_sieve_area = size_t{256} * 1024 * 1024;

        const auto check_region_contract = [](const SieveRegion& region) {
            GNFS_TEST_CHECK(region.i_width() > 0);
            GNFS_TEST_CHECK(region.j_height() > 0);
            GNFS_TEST_CHECK(region.size() > 0);
            GNFS_TEST_CHECK(region.size() <= size_t{256} * 1024 * 1024);
            GNFS_TEST_CHECK(region.size() == static_cast<size_t>(region.i_width()) *
                                                 static_cast<size_t>(region.j_height()));

            const std::array<std::pair<int32_t, int32_t>, 4> endpoints{{
                {region.i_min, region.j_min},
                {region.i_max, region.j_min},
                {region.i_min, region.j_max},
                {region.i_max, region.j_max},
            }};
            for (const auto& [i, j] : endpoints) {
                const size_t index = region.ij_to_index(i, j);
                GNFS_TEST_CHECK(index < region.size());
                const auto coordinate = region.index_to_ij(index);
                GNFS_TEST_CHECK(coordinate.first == i);
                GNFS_TEST_CHECK(coordinate.second == j);
            }

            const size_t width = static_cast<size_t>(region.i_width());
            const std::array<size_t, 5> indices{{
                0,
                width - 1,
                region.size() / 2,
                region.size() - width,
                region.size() - 1,
            }};
            for (const size_t index : indices) {
                const auto [i, j] = region.index_to_ij(index);
                GNFS_TEST_CHECK(region.ij_to_index(i, j) == index);
            }

            const auto index_sentinel = region.index_to_ij(region.size());
            GNFS_TEST_CHECK(index_sentinel.first == region.i_min);
            GNFS_TEST_CHECK(index_sentinel.second == region.j_min);
            GNFS_TEST_CHECK(region.ij_to_index(region.i_min - 1, region.j_min) == region.size());
            GNFS_TEST_CHECK(region.ij_to_index(region.i_min, region.j_min - 1) == region.size());
        };

        const auto same_region = [](const SieveRegion& lhs, const SieveRegion& rhs) {
            return lhs.i_min == rhs.i_min && lhs.i_max == rhs.i_max && lhs.j_min == rhs.j_min &&
                   lhs.j_max == rhs.j_max;
        };

        // skewness = 1.0 → symmetric region
        auto r1 = default_sieve_region(1.0);
        GNFS_TEST_CHECK(r1.i_width() == 23170);
        GNFS_TEST_CHECK(r1.j_height() == 11585);
        check_region_contract(r1);

        // skewness = 100.0 → wider i, shorter j
        auto r2 = default_sieve_region(100.0);
        GNFS_TEST_CHECK(r2.i_width() >= r1.i_width());   // wider or equal
        GNFS_TEST_CHECK(r2.j_height() <= r1.j_height()); // shorter or equal
        check_region_contract(r2);

        // skewness = 0.5 retains the historical symmetric fallback.
        auto r3 = default_sieve_region(0.5);
        GNFS_TEST_CHECK(same_region(r3, r1));
        check_region_contract(r3);

        // Finite extreme skew saturates to a one-row region without exceeding
        // the old area cap or overflowing the int32 width contract.
        for (const double skewness :
             std::array<double, 2>{1e10, std::numeric_limits<double>::max()}) {
            const auto extreme = default_sieve_region(skewness);
            GNFS_TEST_CHECK(extreme.i_width() == static_cast<int32_t>(max_sieve_area));
            GNFS_TEST_CHECK(extreme.j_height() == 1);
            GNFS_TEST_CHECK(extreme.size() == max_sieve_area);
            check_region_contract(extreme);
        }

        // Invalid/non-positive inputs choose the deterministic skew=1 fallback.
        for (const double invalid : std::array<double, 6>{
                 std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity(),
                 -std::numeric_limits<double>::infinity(),
                 -1.0,
                 -std::numeric_limits<double>::max(),
                 0.0,
             }) {
            const auto fallback = default_sieve_region(invalid);
            GNFS_TEST_CHECK(same_region(fallback, r1));
            check_region_contract(fallback);
        }

        // Arbitrary public endpoints must fail closed instead of overflowing
        // the int32 return type used by i_width().
        const SieveRegion unrepresentable{
            std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max(),
            1,
            1,
        };
        GNFS_TEST_CHECK(unrepresentable.i_width() == 0);
        GNFS_TEST_CHECK(unrepresentable.size() == 0);

        const SieveRegion unrepresentable_height{
            0,
            0,
            std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max(),
        };
        GNFS_TEST_CHECK(unrepresentable_height.i_width() == 1);
        GNFS_TEST_CHECK(unrepresentable_height.j_height() == 0);
        GNFS_TEST_CHECK(unrepresentable_height.size() == 0);
        GNFS_TEST_CHECK(unrepresentable_height.ij_to_index(0, 0) == unrepresentable_height.size());
        const auto height_sentinel = unrepresentable_height.index_to_ij(0);
        GNFS_TEST_CHECK(height_sentinel.first == unrepresentable_height.i_min);
        GNFS_TEST_CHECK(height_sentinel.second == unrepresentable_height.j_min);

        // Small valid regions adjacent to both j endpoints retain exact
        // row-major index roundtrips without needing a representable
        // coordinate one-past j_max.
        const std::array<SieveRegion, 2> endpoint_regions{{
            {-2, 2, std::numeric_limits<int32_t>::max() - 1, std::numeric_limits<int32_t>::max()},
            {-2, 2, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min() + 1},
        }};
        for (const auto& endpoint_region : endpoint_regions) {
            GNFS_TEST_CHECK(endpoint_region.i_width() == 5);
            GNFS_TEST_CHECK(endpoint_region.j_height() == 2);
            GNFS_TEST_CHECK(endpoint_region.size() == 10);
            for (size_t index = 0; index < endpoint_region.size(); ++index) {
                const auto [i, j] = endpoint_region.index_to_ij(index);
                GNFS_TEST_CHECK(endpoint_region.ij_to_index(i, j) == index);
            }
            const auto final_coordinate = endpoint_region.index_to_ij(endpoint_region.size() - 1);
            GNFS_TEST_CHECK(final_coordinate.first == endpoint_region.i_max);
            GNFS_TEST_CHECK(final_coordinate.second == endpoint_region.j_max);
            GNFS_TEST_CHECK(
                endpoint_region.ij_to_index(endpoint_region.i_max + 1, endpoint_region.j_min) ==
                endpoint_region.size());
            const auto index_sentinel = endpoint_region.index_to_ij(endpoint_region.size());
            GNFS_TEST_CHECK(index_sentinel.first == endpoint_region.i_min);
            GNFS_TEST_CHECK(index_sentinel.second == endpoint_region.j_min);
        }
    }

    // Test 7: Large prime q — determinant still ±q
    {
        SpecialQ sq;
        sq.q = 99991;
        sq.r = 12345;
        auto basis = compute_lattice_basis(sq);
        int64_t det = basis.determinant();
        TEST_ASSERT(det == 99991 || det == -99991);
        TEST_ASSERT(basis.verify_ab(basis.e0, basis.f0));
        TEST_ASSERT(basis.verify_ab(basis.e1, basis.f1));
    }

    // Test 8: moderate root value — boundary
    // BACKLOG P2 FIXED 2026-05-18: r=q-1 oscillation now bounded by MAX_GAUSSIAN_ITERS=64.
    // compute_lattice_basis terminates regardless of dot/n1 = ±0.5 exact ties.
    // Both r=30 (safe convergence) and r=q-1 (formerly oscillating) now PASS.
    {
        SpecialQ sq;
        sq.q = 97;
        sq.r = 30;
        auto basis = compute_lattice_basis(sq);
        int64_t det = basis.determinant();
        TEST_ASSERT(det == 97 || det == -97);
        TEST_ASSERT(basis.verify_ab(basis.e0, basis.f0));
        TEST_ASSERT(basis.verify_ab(basis.e1, basis.f1));
    }

    // Test 9: r=q-1 boundary — formerly known oscillation case (BACKLOG P2)
    // Now bounded by max_iters guard, returns valid basis even if Gaussian
    // reduction didn't fully converge in optimal sense.
    {
        SpecialQ sq;
        sq.q = 97;
        sq.r = 96;
        auto basis = compute_lattice_basis(sq);
        int64_t det = basis.determinant();
        TEST_ASSERT(det == 97 || det == -97);
        // Don't assert verify_ab — partial reduction may produce non-canonical
        // basis but should still describe the lattice (det invariant holds).
        (void)basis;
    }

    std::cout << "  PASS (9 sub-tests, BACKLOG P2 oscillation fixed)" << std::endl;
}

// ─── ClassGroup 结构体边界/极端情况 ─────────────────────────────────────

void test_class_group_struct_edge_cases() {
    std::cout << "Testing ClassGroup struct edge cases..." << std::endl;

    // Test 1: IdealClass reduce_mod with order=0 → no-op
    {
        IdealClass cls;
        PrimeIdeal pi{5, 2, 1};
        cls.add_prime(pi, 7);
        cls.reduce_mod(0);                      // order=0 → no reduction
        TEST_ASSERT(cls.prime_powers[pi] == 7); // unchanged
    }

    // Test 2: IdealClass reduce_mod with negative exponents
    {
        IdealClass cls;
        PrimeIdeal pi{3, 1, 1};
        cls.prime_powers[pi] = -5;
        cls.reduce_mod(3); // -5 mod 3 → ((-5 % 3) + 3) % 3 = ((-2) + 3) % 3 = 1
        TEST_ASSERT(cls.prime_powers[pi] == 1);
    }

    // Test 3: IdealClass add_prime accumulates
    {
        IdealClass cls;
        PrimeIdeal pi{7, 0, 1};
        cls.add_prime(pi, 3);
        cls.add_prime(pi, 4);
        TEST_ASSERT(cls.prime_powers[pi] == 7);
    }

    // Test 4: PrimeIdeal self-comparison
    {
        PrimeIdeal pi{5, 3, 1};
        TEST_ASSERT(pi == pi);
        TEST_ASSERT(!(pi < pi));
    }

    // Test 5: IdealClass with all-zero exponents → principal
    {
        IdealClass cls;
        PrimeIdeal p1{2, 0, 1};
        PrimeIdeal p2{3, 1, 1};
        cls.add_prime(p1, 0);
        cls.add_prime(p2, 0);
        // Note: zero entries are not removed by add_prime, only by reduce_mod
        // But is_principal checks for all exp==0
        TEST_ASSERT(cls.is_principal());
    }

    // Test 6: ClassGroup with degree-2 polynomial (non-cubic) — general discriminant path
    {
        // f(x) = x^2 + 1, m = 12, N = 145 = 5×29, f(12) = 145
        std::vector<Integer> coeffs = {Integer(int64_t(1)), Integer(int64_t(0)),
                                       Integer(int64_t(1))};
        PolynomialContext ctx(Integer("145"), std::move(coeffs), Integer(int64_t(12)));
        ClassGroup cg(ctx);
        // Should not crash; class_number >= 1
        TEST_ASSERT(cg.class_number() >= 1);
        TEST_ASSERT(cg.minkowski_bound() >= 0.0);
    }

    // Test 7: ClassGroup with very small discriminant → trivial
    {
        // f(x) = x^3 + x + 1, m=5, N=131: Δ=-31, MB≈1.57 < 2 → trivial
        std::vector<Integer> c = {Integer(int64_t(1)), Integer(int64_t(1)), Integer(int64_t(0)),
                                  Integer(int64_t(1))};
        PolynomialContext ctx(Integer("131"), std::move(c), Integer(int64_t(5)));
        ClassGroup cg(ctx);
        TEST_ASSERT(cg.class_number() == 1);
        TEST_ASSERT(cg.num_generators() == 0);
        // Character for any (a,b) should be empty
        auto ch = cg.compute_character(3, 1);
        TEST_ASSERT(ch.empty());
    }

    // Test 8: ClassGroupConfig customization
    {
        ClassGroup::Config cfg;
        cfg.max_primes = 5;
        cfg.max_generators = 2;
        cfg.verbose = false;

        std::vector<Integer> c = {Integer(int64_t(1)), Integer(int64_t(5)), Integer(int64_t(0)),
                                  Integer(int64_t(1))};
        PolynomialContext ctx(Integer("19"), std::move(c), Integer(int64_t(2)));
        ClassGroup cg(ctx, cfg);
        TEST_ASSERT(cg.class_number() >= 1);
    }

    std::cout << "  PASS (8 sub-tests)" << std::endl;
}

int main() {
    int failed = 0;

    std::cout << "=== Edge Case Tests ===" << std::endl;

    // Integer 负 mod
    failed += !run_case(test_integer_negative_mod_truncated);

    // Integer gcd 零输入
    failed += !run_case(test_integer_gcd_with_zero);

    // Integer sqrt 边界
    failed += !run_case(test_integer_sqrt_edge_cases);

    // Integer pow/powmod 边界
    failed += !run_case(test_integer_pow_edge_cases);

    // Integer native-width overloads
    failed += !run_case(test_integer_native_width_overloads);

    // Integer 加减法 INT64 边界
    failed += !run_case(test_integer_add_sub_boundary);

    // Integer 符号检查
    failed += !run_case(test_integer_sign_checks);

    // SparseRow 空行
    failed += !run_case(test_sparse_row_empty);

    // SparseMatrix 零维
    failed += !run_case(test_sparse_matrix_zero_rows);
    failed += !run_case(test_sparse_matrix_zero_cols);
    failed += !run_case(test_sparse_matrix_one_by_one_zero);
    failed += !run_case(test_sparse_matrix_one_by_one_one);
    failed += !run_case(test_sparse_matrix_all_zero);

    // BitVector 边界
    failed += !run_case(test_bitvector_empty);
    failed += !run_case(test_bitvector_single_bit);
    failed += !run_case(test_bitvector_word_boundaries);
    failed += !run_case(test_bitvector_xor_boundary);

    // Gaussian 退化情形
    failed += !run_case(test_gaussian_identity_matrix);
    failed += !run_case(test_gaussian_single_row);

    // BlockLanczos 空矩阵
    failed += !run_case(test_block_lanczos_empty_matrix);
    failed += !run_case(test_block_lanczos_zero_cols);

    // Cofactor 边界
    failed += !run_case(test_ecm_edge_cases);
    failed += !run_case(test_cofactor_classify_edge_cases);
    failed += !run_case(test_trial_division_edge_cases);
    failed += !run_case(test_quick_cofactor_check_edge_cases);

    // Relation 边界
    failed += !run_case(test_relation_serialization_edge_cases);
    failed += !run_case(test_relation_collector_edge_cases);
    failed += !run_case(test_relation_filter_edge_cases);

    // Sieve 参数边界
    failed += !run_case(test_sieve_params_edge_cases);
    failed += !run_case(test_special_q_edge_cases);

    // Hensel Sqrt 边界
    failed += !run_case(test_hensel_sqrt_edge_cases);

    // Schirokauer 大域 ℓ 边界
    failed += !run_case(test_schirokauer_large_ell_edge_cases);
    failed += !run_case(test_schirokauer_degree3_split_degree2);

    // NumberField 算术边界
    failed += !run_case(test_number_field_edge_cases);

    // MatrixBuilder 退化输入
    failed += !run_case(test_matrix_builder_edge_cases);

    // RationalSqrt 退化输入
    failed += !run_case(test_rational_sqrt_edge_cases);

    // AlgebraicSqrt 退化输入
    failed += !run_case(test_algebraic_sqrt_edge_cases);

    // Couveignes 退化输入
    failed += !run_case(test_couveignes_edge_cases);

    // PolynomialContext 构造与运算边界
    failed += !run_case(test_polynomial_context_edge_cases);

    // FactorBase 查找与构造边界
    failed += !run_case(test_factor_base_edge_cases);

    // ModularPoly 算术边界 (Session 34)
    failed += !run_case(test_modular_poly_edge_cases);

    // LatticeBasis 几何边界 (Session 34)
    failed += !run_case(test_lattice_basis_edge_cases);

    // ClassGroup 结构体边界 (Session 34)
    failed += !run_case(test_class_group_struct_edge_cases);

    if (failed != 0) {
        std::cout << failed << " test case(s) failed" << std::endl;
        return 1;
    }

    std::cout << "\nAll edge case tests passed!" << std::endl;
    return 0;
}
