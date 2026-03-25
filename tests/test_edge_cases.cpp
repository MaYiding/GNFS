// test_edge_cases.cpp — 边界/极端情况覆盖
// 覆盖 BACKLOG [TEST] 边界/极端情况覆盖率约 15%
// 专注于三类缺口：Integer 溢出/边界、负 mod、空矩阵
#include "gnfs/core/integer.hpp"
#include "gnfs/linalg/sparse_matrix.hpp"
#include "gnfs/linalg/gauss.hpp"
#include "gnfs/linalg/block_lanczos.hpp"

#include <cassert>
#include <climits>   // INT64_MAX, INT64_MIN, UINT32_MAX
#include <cstdint>
#include <iostream>

using namespace gnfs::core;
using namespace gnfs::linalg;

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

    std::cout << "\nAll edge case tests passed!" << std::endl;
    return 0;
}
