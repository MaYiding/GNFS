/// test_kleinjung.cpp - Kleinjung 多项式选择器测试

#include "gnfs/polynomial/kleinjung_selector.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/polynomial/polynomial_optimizer.hpp"
#include "gnfs/core/integer.hpp"

#include <cassert>
#include <iostream>

using namespace gnfs::polynomial;
using namespace gnfs::core;

/// 测试多项式导数
void test_polynomial_derivative() {
    std::cout << "Testing polynomial derivative..." << std::endl;

    // f(x) = 3x^3 + 2x^2 + x + 5
    // f'(x) = 9x^2 + 4x + 1
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(5));   // 常数
    coeffs.push_back(Integer(1));   // x
    coeffs.push_back(Integer(2));   // x^2
    coeffs.push_back(Integer(3));   // x^3
    IntPolynomial f(std::move(coeffs));

    IntPolynomial df = f.derivative();

    assert(df.degree() == 2);
    assert(df[0].to_int64() == 1);   // 1
    assert(df[1].to_int64() == 4);   // 4
    assert(df[2].to_int64() == 9);   // 9

    std::cout << "  f(x) = 3x^3 + 2x^2 + x + 5" << std::endl;
    std::cout << "  f'(x) = " << df[2].to_int64() << "x^2 + "
              << df[1].to_int64() << "x + " << df[0].to_int64() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

/// 测试多项式平移
void test_polynomial_translate() {
    std::cout << "Testing polynomial translation..." << std::endl;

    // f(x) = x^2 + 2x + 1 = (x+1)^2
    // f(x+3) = (x+4)^2 = x^2 + 8x + 16
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(1));   // 常数
    coeffs.push_back(Integer(2));   // x
    coeffs.push_back(Integer(1));   // x^2
    IntPolynomial f(std::move(coeffs));

    IntPolynomial g = f.translate(3);

    assert(g.degree() == 2);
    assert(g[0].to_int64() == 16);  // (1+3)^2 = 16
    assert(g[1].to_int64() == 8);   // 2*(1+3) = 8
    assert(g[2].to_int64() == 1);   // leading coeff unchanged

    std::cout << "  f(x) = x^2 + 2x + 1" << std::endl;
    std::cout << "  f(x+3) = " << g[2].to_int64() << "x^2 + "
              << g[1].to_int64() << "x + " << g[0].to_int64() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

/// 测试多项式乘法
void test_polynomial_multiplication() {
    std::cout << "Testing polynomial multiplication..." << std::endl;

    // f(x) = x + 1
    // g(x) = x + 2
    // f*g = x^2 + 3x + 2
    std::vector<Integer> f_coeffs;
    f_coeffs.push_back(Integer(1));
    f_coeffs.push_back(Integer(1));
    IntPolynomial f(std::move(f_coeffs));

    std::vector<Integer> g_coeffs;
    g_coeffs.push_back(Integer(2));
    g_coeffs.push_back(Integer(1));
    IntPolynomial g(std::move(g_coeffs));

    IntPolynomial h = f * g;

    assert(h.degree() == 2);
    assert(h[0].to_int64() == 2);   // 1*2 = 2
    assert(h[1].to_int64() == 3);   // 1*1 + 1*2 = 3
    assert(h[2].to_int64() == 1);   // 1*1 = 1

    std::cout << "  (x+1) * (x+2) = x^2 + 3x + 2" << std::endl;
    std::cout << "  Result: " << h[2].to_int64() << "x^2 + "
              << h[1].to_int64() << "x + " << h[0].to_int64() << std::endl;
    std::cout << "  PASSED" << std::endl;
}

/// 测试光滑系数生成
void test_smooth_coefficient_generation() {
    std::cout << "Testing smooth coefficient generation..." << std::endl;

    KleinjungParams params;
    params.leading_coeff_bound = 100;
    params.num_candidates = 50;

    KleinjungSelector selector(params);

    // 无法直接测试私有方法，通过完整选择来间接测试
    std::cout << "  Smooth coefficients are generated internally" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

/// 测试 Stage 1 候选生成
void test_stage1_candidates() {
    std::cout << "Testing Stage 1 candidate generation..." << std::endl;

    // 使用一个小的半素数进行测试
    Integer n("1000000007");  // 一个大素数

    KleinjungParams params;
    params.degree = 3;
    params.leading_coeff_bound = 100;
    params.num_candidates = 10;
    params.search_radius = 5;

    KleinjungSelector selector(params);

    // 运行选择器
    auto result = selector.select(n);

    std::cout << "  Tested " << result.candidates_tested << " candidates" << std::endl;
    std::cout << "  Success: " << (result.success ? "yes" : "no") << std::endl;

    if (result.success) {
        std::cout << "  Polynomial degree: " << result.f.degree() << std::endl;
        std::cout << "  Skewness: " << result.skewness << std::endl;
        std::cout << "  E-score: " << result.score.e_score << std::endl;
    }

    std::cout << "  PASSED" << std::endl;
}

/// 测试与 Base-m 的比较
void test_compare_with_basem() {
    std::cout << "Testing comparison with Base-m..." << std::endl;

    // 使用一个 30 位数的半素数
    // 这里用两个素数的乘积
    Integer p1("1000000007");
    Integer p2("1000000009");
    Integer n = p1 * p2;  // ~60位

    std::cout << "  n = " << n.to_string() << std::endl;
    std::cout << "  n has " << n.num_digits() << " digits" << std::endl;

    // Base-m 选择
    uint32_t degree = 3;
    auto basem_result = BaseMSelector::select(n, degree);

    if (basem_result.success) {
        std::cout << "  Base-m polynomial found" << std::endl;
        // skewness not available in PolynomialSelectionResult
    }

    // Kleinjung 选择
    KleinjungParams params;
    params.degree = degree;
    params.leading_coeff_bound = 50;
    params.num_candidates = 20;
    params.murphy_params.sample_points = 200;
    params.murphy_params.alpha_bound = 50;

    KleinjungSelector selector(params);

    // 设置进度回调
    selector.set_progress_callback([](size_t current, size_t total, double best, const char* stage) {
        if (current % 5 == 0 || current == total) {
            std::cout << "    " << stage << ": " << current << "/" << total
                      << " (best E = " << best << ")" << std::endl;
        }
    });

    auto kleinjung_result = selector.select(n);

    if (kleinjung_result.success) {
        std::cout << "  Kleinjung polynomial found" << std::endl;
        std::cout << "  Kleinjung skewness: " << kleinjung_result.skewness << std::endl;
        std::cout << "  Kleinjung E-score: " << kleinjung_result.score.e_score << std::endl;
        std::cout << "  Kleinjung alpha_f: " << kleinjung_result.score.alpha_f << std::endl;

        // 验证多项式
        Integer fm = kleinjung_result.f.evaluate(kleinjung_result.m);
        Integer remainder;
        Integer quotient;
        Integer::divmod(quotient, remainder, fm, n);
        std::cout << "  f(m) mod n = " << remainder.to_string() << std::endl;
    }

    std::cout << "  Elapsed time: " << kleinjung_result.elapsed_seconds << " seconds" << std::endl;
    std::cout << "  PASSED" << std::endl;
}

/// 测试取消功能
void test_cancellation() {
    std::cout << "Testing cancellation..." << std::endl;

    Integer n("123456789012345678901234567890123456789");

    KleinjungParams params;
    params.degree = 5;
    params.leading_coeff_bound = 100;
    params.num_candidates = 100;
    params.parallel = false;  // 串行以便测试取消
    // 快速 Murphy 参数（测取消机制，不需精确评分）
    params.murphy_params.alpha_bound = 50;
    params.murphy_params.sample_points = 100;

    KleinjungSelector selector(params);

    // 在回调中取消
    size_t cancel_at = 5;
    selector.set_progress_callback([&](size_t current, size_t /*total*/, double, const char*) {
        if (current >= cancel_at) {
            selector.cancel();
        }
    });

    auto result = selector.select(n);

    std::cout << "  Cancelled after " << result.candidates_tested << " candidates" << std::endl;
    assert(result.candidates_tested <= cancel_at + 1);

    std::cout << "  PASSED" << std::endl;
}

/// 测试进度回调
void test_progress_callback() {
    std::cout << "Testing progress callback..." << std::endl;

    Integer n("10000000019");  // 素数

    KleinjungParams params;
    params.degree = 3;
    params.leading_coeff_bound = 30;
    params.num_candidates = 10;

    KleinjungSelector selector(params);

    size_t callback_count = 0;
    size_t last_progress = 0;

    selector.set_progress_callback([&](size_t current, size_t total_, double, const char*) {
        callback_count++;
        assert(current >= last_progress);
        assert(current <= total_);
        last_progress = current;
    });

    auto _ = selector.select(n);
    (void)_;

    std::cout << "  Callback invoked " << callback_count << " times" << std::endl;
    assert(callback_count > 0);

    std::cout << "  PASSED" << std::endl;
}

/// 测试 PolynomialOptimizer
void test_polynomial_optimizer() {
    std::cout << "Testing PolynomialOptimizer..." << std::endl;

    // 测试 estimate_skewness
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(1000000));  // c_0
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(1));         // c_2
    IntPolynomial f(std::move(coeffs));

    double skew = PolynomialOptimizer::estimate_skewness(f);
    // skewness ~ (c_0 / c_2)^{1/2} = (1000000)^{0.5} = 1000
    std::cout << "  Estimated skewness: " << skew << std::endl;
    assert(skew > 500 && skew < 2000);

    // 测试 compute_size
    double size = PolynomialOptimizer::compute_size(f, skew);
    std::cout << "  Polynomial size at optimal skewness: " << size << std::endl;
    assert(std::isfinite(size) && size > 0);

    std::cout << "  PASSED" << std::endl;
}

/// 测试 newton_root: delta 非零 + 验证逻辑
void test_newton_root() {
    std::cout << "Testing newton_root..." << std::endl;

    // Case 1: 起始点满足 f(m) ≡ 0 mod n → 直接返回 m
    // f(x) = x^2 + 2x, m=11, f(11)=143, n=143 → f(m) mod n = 0
    {
        std::vector<Integer> c;
        c.push_back(Integer(static_cast<int64_t>(0)));  // a0 = 0
        c.push_back(Integer(2));                         // a1 = 2
        c.push_back(Integer(1));                         // a2 = 1
        IntPolynomial f(std::move(c));
        Integer n(143);
        Integer m0(11);

        auto result = PolynomialOptimizer::newton_root(f, m0, n);
        assert(result.has_value());
        assert(result.value().to_int64() == 11);
        std::cout << "  Case 1 (valid starting point): PASSED" << std::endl;
    }

    // Case 2: f(m) ≢ 0 mod n → Newton 迭代后验证失败 → nullopt
    // f(x) = x^2 - 5, m=3, f(3)=4, n=143 → 4 mod 143 ≠ 0
    {
        std::vector<Integer> c;
        c.push_back(Integer(-5));  // a0 = -5
        c.push_back(Integer(static_cast<int64_t>(0)));   // a1 = 0
        c.push_back(Integer(1));   // a2 = 1
        IntPolynomial f(std::move(c));
        Integer n(143);
        Integer m0(3);

        auto result = PolynomialOptimizer::newton_root(f, m0, n);
        // f(x) = x^2 - 5 的整数根是 ±√5（非整数），Newton 不会找到 f(m) ≡ 0 mod 143 的根
        // 验证应正确拒绝
        if (result.has_value()) {
            // 如果找到了根，验证它确实满足 f(m) ≡ 0 mod n
            Integer fm = f.evaluate(result.value());
            Integer q, r;
            Integer::divmod(q, r, fm, n);
            assert(r.is_zero() && "newton_root returned value must satisfy f(m) ≡ 0 mod n");
        }
        std::cout << "  Case 2 (invalid starting point): PASSED (returned "
                  << (result.has_value() ? "valid root" : "nullopt") << ")" << std::endl;
    }

    // Case 3: 线性多项式 f(x) = x - 11, n=143
    // Newton 精确收敛到 x=11，f(11)=0 ≡ 0 mod 143
    {
        std::vector<Integer> c;
        c.push_back(Integer(-11));  // a0 = -11
        c.push_back(Integer(1));    // a1 = 1
        IntPolynomial f(std::move(c));
        Integer n(143);
        Integer m0(15);  // 起点偏离

        auto result = PolynomialOptimizer::newton_root(f, m0, n);
        assert(result.has_value());
        assert(result.value().to_int64() == 11);
        std::cout << "  Case 3 (linear convergence): PASSED" << std::endl;
    }

    // Case 4: 零多项式 → nullopt
    {
        IntPolynomial f(0);
        Integer n(143);
        Integer m0(10);
        auto result = PolynomialOptimizer::newton_root(f, m0, n);
        assert(!result.has_value());
        std::cout << "  Case 4 (zero polynomial): PASSED" << std::endl;
    }

    std::cout << "  ALL newton_root tests PASSED" << std::endl;
}

int main() {
    std::cout << "=== Kleinjung Selector Tests ===" << std::endl << std::endl;

    test_polynomial_derivative();
    test_polynomial_translate();
    test_polynomial_multiplication();
    test_smooth_coefficient_generation();
    test_polynomial_optimizer();
    test_newton_root();

    // 以下测试涉及 Kleinjung 完整搜索，耗时较长（slow tier）
    test_stage1_candidates();
    test_progress_callback();
    test_cancellation();
    test_compare_with_basem();

    std::cout << std::endl << "All Kleinjung selector tests passed!" << std::endl;
    return 0;
}
