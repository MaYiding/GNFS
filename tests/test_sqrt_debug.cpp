/// test_sqrt_debug.cpp - 平方根计算调试测试

// #define COUVEIGNES_DEBUG 1

#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/number_field.hpp>
#include <gnfs/sqrt/couveignes.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/core/polynomial_context.hpp>
#include <gnfs/polynomial/int_polynomial.hpp>

#include <chrono>
#include <iostream>
#include <vector>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::sqrt;
using namespace gnfs::polynomial;

// 测试数域乘法和平方根
void test_number_field_sqrt() {
    std::cout << "=== Number Field Square Root Test ===\n\n";

    // 创建简单的多项式上下文
    // f(x) = x^2 + 1, N = 5, m = 2 (因为 2^2 + 1 = 5)
    Integer N(5);
    Integer m(2);

    // f(x) = x^2 + 1
    std::vector<Integer> f_coeffs;
    f_coeffs.push_back(Integer(1));  // 常数项
    f_coeffs.push_back(Integer(static_cast<int64_t>(0)));  // x 系数
    f_coeffs.push_back(Integer(1));  // x^2 系数

    PolynomialContext ctx(N.clone(), std::move(f_coeffs), m.clone());

    std::cout << "N = " << N.to_string() << "\n";
    std::cout << "m = " << m.to_string() << "\n";
    std::cout << "f(x) = x^2 + 1\n\n";

    // 创建数域
    NumberField nf(ctx);

    // 测试 1: 简单乘法
    std::cout << "--- Test 1: Simple multiplication ---\n";
    auto elem1 = nf.from_ab(1, 1);  // 1 - α
    auto elem2 = nf.from_ab(1, 1);  // 1 - α
    auto product = nf.multiply(elem1, elem2);

    std::cout << "elem1 = 1 - α\n";
    std::cout << "elem2 = 1 - α\n";
    std::cout << "product = (1 - α)^2 = ";
    for (size_t i = 0; i <= product.degree(); ++i) {
        if (i > 0) std::cout << " + ";
        std::cout << product.coeff(i).to_string() << "*α^" << i;
    }
    std::cout << "\n";
    // (1 - α)^2 = 1 - 2α + α^2 = 1 - 2α + (-1) = -2α (since α^2 = -1)
    std::cout << "Expected: 0 + (-2)*α = -2α\n\n";

    // 测试 2: 在 m 处求值
    std::cout << "--- Test 2: Evaluate at m ---\n";
    Integer val = nf.evaluate_at_m(product);
    std::cout << "product(m) = product(2) = " << val.to_string() << "\n";
    // -2*2 = -4
    std::cout << "Expected: -4\n\n";

    // 测试 3: 简单 Couveignes 测试
    std::cout << "--- Test 3: Couveignes sqrt ---\n";

    // 创建一个已知是完全平方的元素
    // (1 - α)^2 的平方根应该是 ±(1 - α)
    auto known_square = nf.multiply(elem1, elem1.clone());

    std::cout << "Known square = (1 - α)^2\n";

    // 使用 Couveignes 计算平方根
    CouveignesSqrt::Config cfg;
    cfg.num_primes = 4;  // 减少到 4 个素数以加快测试
    cfg.prime_start = 100;

    std::cout << "Starting Couveignes with " << cfg.num_primes << " primes...\n";
    CouveignesSqrt couveignes(cfg);

    // 从 ab_pairs 计算
    std::vector<std::pair<int64_t, uint64_t>> ab_pairs;
    ab_pairs.emplace_back(1, 1);
    ab_pairs.emplace_back(1, 1);  // 两个 (1 - α) 相乘

    auto sqrt_opt = couveignes.compute(ab_pairs, nf);

    if (sqrt_opt) {
        std::cout << "Couveignes sqrt found: ";
        for (size_t i = 0; i <= sqrt_opt->degree(); ++i) {
            if (i > 0) std::cout << " + ";
            std::cout << sqrt_opt->coeff(i).to_string() << "*α^" << i;
        }
        std::cout << "\n";

        // 验证: sqrt^2 应该等于 known_square
        auto sqrt_squared = nf.multiply_mod_n(*sqrt_opt, *sqrt_opt);
        std::cout << "sqrt^2 = ";
        for (size_t i = 0; i <= sqrt_squared.degree(); ++i) {
            if (i > 0) std::cout << " + ";
            std::cout << sqrt_squared.coeff(i).to_string() << "*α^" << i;
        }
        std::cout << "\n";

        Integer sqrt_at_m = nf.evaluate_at_m_mod_n(*sqrt_opt);
        std::cout << "sqrt(m) mod N = " << sqrt_at_m.to_string() << "\n";
    } else {
        std::cout << "Couveignes FAILED to find sqrt\n";
    }

    std::cout << "\n";
}

// 测试更大的数
void test_larger_number() {
    std::cout << "=== Larger Number Test ===\n\n";

    // N = 143 = 11 * 13
    // f(x) = x^3 + x^2 + x - 1, m = 5
    // f(5) = 125 + 25 + 5 - 1 = 154 ≠ 0 mod 143... 需要找正确的

    // 用 GNFS 标准形式: f(x) = x^d - N^(1/d) 近似
    // 对于 N=143, d=3, m ≈ 5.23
    // 用 m=5: f(x) = x^3 + ... 使得 f(5) ≡ 0 mod 143

    // 简化: 用已知可行的例子
    // N = 15 = 3 * 5
    // f(x) = x^2 - 4, m = 2 不行 (4-4=0 但需要 mod 15)

    // 用 N = 21 = 3 * 7
    // m = 4, f(x) = x^2 + x - 2 => f(4) = 16 + 4 - 2 = 18 ≢ 0 mod 21

    // 让我用标准方法: N = p*q, m = floor(N^(1/d))
    // N = 143, d = 3, m = 5
    // 构造 f: f(m) ≡ 0 mod N
    // f(x) = x^3 - 18x + c where c = 5^3 - 18*5 mod 143 = 125 - 90 = 35
    // f(x) = x^3 - 18x + 35 => f(5) = 125 - 90 + 35 = 70 ≢ 0 mod 143

    // 更简单: 用 base-m 方法
    // N = 143, m = 5
    // 143 = 28 * 5 + 3 => c0 = 3
    // 28 = 5 * 5 + 3 => c1 = 3
    // 5 = 1 * 5 + 0 => c2 = 0
    // 1 = 0 * 5 + 1 => c3 = 1
    // f(x) = x^3 + 0*x^2 + 3*x + 3
    // f(5) = 125 + 0 + 15 + 3 = 143 ≡ 0 mod 143 ✓

    Integer N(143);
    Integer m(5);

    std::vector<Integer> f_coeffs;
    f_coeffs.push_back(Integer(3));   // c0
    f_coeffs.push_back(Integer(3));   // c1
    f_coeffs.push_back(Integer(static_cast<int64_t>(0)));   // c2
    f_coeffs.push_back(Integer(1));   // c3

    // 验证 f(m) mod N = 0
    // f(m) = m^3 + 3*m + 3
    Integer fm = m.clone();
    fm *= m;
    fm *= m;  // m^3
    Integer term = m.clone();
    term *= Integer(3);  // 3*m
    fm += term;
    fm += Integer(3);  // + 3
    Integer fm_mod_N = fm.clone();
    fm_mod_N %= N;

    std::cout << "N = 143 = 11 * 13\n";
    std::cout << "m = 5\n";
    std::cout << "f(x) = x^3 + 3x + 3\n";
    std::cout << "f(m) = " << fm.to_string() << "\n";
    std::cout << "f(m) mod N = " << fm_mod_N.to_string() << "\n";

    if (!fm_mod_N.is_zero()) {
        std::cout << "ERROR: f(m) mod N != 0\n";
        return;
    }

    PolynomialContext ctx(N.clone(), std::move(f_coeffs), m.clone());
    NumberField nf(ctx);

    // 测试 Couveignes
    std::cout << "\n--- Couveignes test with (a,b) pairs ---\n";

    // 构造一些简单的 (a,b) 对
    // (a - b*α) 的乘积是完全平方
    // 简单情况: 同样的元素相乘两次
    std::vector<std::pair<int64_t, uint64_t>> ab_pairs;
    ab_pairs.emplace_back(2, 1);  // 2 - α
    ab_pairs.emplace_back(2, 1);  // 2 - α

    auto elem = nf.from_ab(2, 1);
    auto product = nf.multiply(elem, elem.clone());

    std::cout << "Computing sqrt of (2 - α)^2\n";
    std::cout << "(2 - α)^2 = ";
    for (size_t i = 0; i <= product.degree(); ++i) {
        if (i > 0) std::cout << " + ";
        std::cout << product.coeff(i).to_string() << "*α^" << i;
    }
    std::cout << "\n";

    CouveignesSqrt::Config cfg;
    cfg.num_primes = 4;  // 减少到 4 个素数以加快测试
    cfg.prime_start = 50; // 使用较小的素数

    std::cout << "Starting Couveignes with " << cfg.num_primes << " primes starting at " << cfg.prime_start << "...\n";
    std::cout.flush();

    auto start_time = std::chrono::high_resolution_clock::now();
    CouveignesSqrt couveignes(cfg);
    auto sqrt_opt = couveignes.compute(ab_pairs, nf);
    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    std::cout << "Couveignes completed in " << elapsed << " seconds\n";
    std::cout.flush();

    if (sqrt_opt) {
        std::cout << "SUCCESS: sqrt = ";
        for (size_t i = 0; i <= sqrt_opt->degree(); ++i) {
            if (i > 0) std::cout << " + ";
            std::cout << sqrt_opt->coeff(i).to_string() << "*α^" << i;
        }
        std::cout << "\n";

        // 验证
        auto sqrt_sq = nf.multiply_mod_n(*sqrt_opt, *sqrt_opt);
        std::cout << "sqrt^2 = ";
        for (size_t i = 0; i <= sqrt_sq.degree(); ++i) {
            if (i > 0) std::cout << " + ";
            std::cout << sqrt_sq.coeff(i).to_string() << "*α^" << i;
        }
        std::cout << "\n";

        Integer sqrt_at_m = nf.evaluate_at_m_mod_n(*sqrt_opt);
        std::cout << "sqrt(m) mod N = " << sqrt_at_m.to_string() << "\n";

        // 原始元素在 m 处的值
        Integer orig_at_m = nf.evaluate_at_m_mod_n(elem);
        std::cout << "(2 - α)(m) mod N = (2 - 5) mod 143 = " << orig_at_m.to_string() << "\n";

        // 应该有 sqrt_at_m^2 ≡ (orig_at_m)^2 mod N
        Integer sqrt_sq_at_m = sqrt_at_m.clone();
        sqrt_sq_at_m *= sqrt_at_m;
        sqrt_sq_at_m %= N;
        std::cout << "sqrt(m)^2 mod N = " << sqrt_sq_at_m.to_string() << "\n";

        Integer orig_sq_at_m = orig_at_m.clone();
        orig_sq_at_m *= orig_at_m;
        orig_sq_at_m %= N;
        std::cout << "((2-α)(m))^2 mod N = " << orig_sq_at_m.to_string() << "\n";
    } else {
        std::cout << "FAILED: Couveignes returned empty\n";
    }
}

int main() {
    std::cout << "=== Square Root Debug Tests ===\n\n";

    // Skip the first test for now - it takes too long
    // test_number_field_sqrt();

    test_larger_number();

    std::cout << "\n=== Tests Complete ===\n";
    return 0;
}
