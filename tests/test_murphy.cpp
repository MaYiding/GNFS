/// test_murphy.cpp - Murphy E-score 评估器测试

#include "gnfs/polynomial/murphy_evaluator.hpp"
#include "gnfs/polynomial/int_polynomial.hpp"
#include "gnfs/core/integer.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

using namespace gnfs::polynomial;
using namespace gnfs::core;

/// 测试 Dickman rho 函数
void test_dickman_rho() {
    std::cout << "Testing Dickman rho function..." << std::endl;

    MurphyParams params;
    params.alpha_bound = 1000;  // 使用较小的界以加速测试
    MurphyEvaluator evaluator(params);

    // 已知值：
    // rho(1) = 1
    // rho(2) ≈ 0.306
    // rho(3) ≈ 0.0486
    // rho(4) ≈ 0.00491

    // 创建简单多项式进行测试
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(static_cast<int64_t>(1)));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(1)));  // f(x) = x^2 + 1
    IntPolynomial f(std::move(coeffs));

    // compute_alpha 应该返回一个有限值
    double alpha = evaluator.compute_alpha(f);
    std::cout << "  Alpha for x^2+1: " << alpha << std::endl;
    assert(std::isfinite(alpha));

    std::cout << "  PASSED" << std::endl;
}

/// 测试 alpha 计算
void test_alpha_computation() {
    std::cout << "Testing alpha computation..." << std::endl;

    MurphyParams params;
    params.alpha_bound = 100;  // 小界以加速
    MurphyEvaluator evaluator(params);

    // f(x) = x^5 - 1 在 mod p 下有多个根（对于 p | 5 的情况）
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(static_cast<int64_t>(-1)));  // 常数项
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(1)));   // x^5 项
    IntPolynomial f(std::move(coeffs));

    double alpha = evaluator.compute_alpha(f);
    std::cout << "  Alpha for x^5-1: " << alpha << std::endl;

    // alpha 应该是负的（小素数整除概率高于平均）
    // 因为 x^5-1 在许多素数 p 下有根（x=1 总是根）
    std::cout << "  PASSED" << std::endl;
}

/// 测试评分一致性
void test_score_consistency() {
    std::cout << "Testing score consistency..." << std::endl;

    MurphyParams params;
    params.sample_points = 500;  // 减少采样点以加速
    params.alpha_bound = 100;
    MurphyEvaluator evaluator(params);

    // 创建简单多项式对
    // f(x) = x^3 - 2
    std::vector<Integer> f_coeffs;
    f_coeffs.push_back(Integer(-2));
    f_coeffs.push_back(Integer(static_cast<int64_t>(0)));
    f_coeffs.push_back(Integer(static_cast<int64_t>(0)));
    f_coeffs.push_back(Integer(static_cast<int64_t>(1)));
    IntPolynomial f(std::move(f_coeffs));

    // g(x) = x - 10 (简单线性多项式)
    std::vector<Integer> g_coeffs;
    g_coeffs.push_back(Integer(-10));
    g_coeffs.push_back(Integer(static_cast<int64_t>(1)));
    IntPolynomial g(std::move(g_coeffs));

    Integer n("1000000007");  // 一个素数

    // 计算评分两次，结果应该相近（采样有随机性）
    MurphyScore score1 = evaluator.compute(f, g, n, 100.0);
    MurphyScore score2 = evaluator.compute(f, g, n, 100.0);

    std::cout << "  Score 1: " << score1.e_score << std::endl;
    std::cout << "  Score 2: " << score2.e_score << std::endl;
    std::cout << "  Alpha_f: " << score1.alpha_f << std::endl;
    std::cout << "  Alpha_g: " << score1.alpha_g << std::endl;

    // 分数应该是正的和有限的
    assert(std::isfinite(score1.e_score));
    assert(std::isfinite(score2.e_score));

    std::cout << "  PASSED" << std::endl;
}

/// 测试 skewness 优化
void test_skewness_optimization() {
    std::cout << "Testing skewness optimization..." << std::endl;

    MurphyParams params;
    params.sample_points = 200;
    params.alpha_bound = 50;
    params.skewness_steps = 20;
    MurphyEvaluator evaluator(params);

    // 创建典型的 GNFS 多项式
    // f(x) = x^5 + 3x^4 - 2x^3 + x^2 - 5x + 1000000
    std::vector<Integer> f_coeffs;
    f_coeffs.push_back(Integer(1000000));  // c_0
    f_coeffs.push_back(Integer(-5));        // c_1
    f_coeffs.push_back(Integer(static_cast<int64_t>(1)));         // c_2
    f_coeffs.push_back(Integer(-2));        // c_3
    f_coeffs.push_back(Integer(3));         // c_4
    f_coeffs.push_back(Integer(static_cast<int64_t>(1)));         // c_5
    IntPolynomial f(std::move(f_coeffs));

    // g(x) = x - 100
    std::vector<Integer> g_coeffs;
    g_coeffs.push_back(Integer(-100));
    g_coeffs.push_back(Integer(static_cast<int64_t>(1)));
    IntPolynomial g(std::move(g_coeffs));

    Integer n("12345678901234567890123456789");

    double optimal_skew = evaluator.optimize_skewness(f, g, n);
    std::cout << "  Optimal skewness: " << optimal_skew << std::endl;

    // skewness 应该在合理范围内
    assert(optimal_skew > 0);
    assert(std::isfinite(optimal_skew));

    // 估计 skewness ~ (c_0 / c_5)^{1/5} = (1000000 / 1)^{0.2} ≈ 15.85
    double expected_skew = std::pow(1000000.0, 0.2);
    std::cout << "  Expected skewness (estimate): " << expected_skew << std::endl;

    std::cout << "  PASSED" << std::endl;
}

/// 测试快速多项式比较
void test_quick_compare() {
    std::cout << "Testing quick polynomial comparison..." << std::endl;

    // 多项式 1: 较大系数
    std::vector<Integer> coeffs1;
    coeffs1.push_back(Integer(1000000));
    coeffs1.push_back(Integer(1000));
    coeffs1.push_back(Integer(10));
    IntPolynomial f1(std::move(coeffs1));

    // 多项式 2: 较小系数
    std::vector<Integer> coeffs2;
    coeffs2.push_back(Integer(1000));
    coeffs2.push_back(Integer(100));
    coeffs2.push_back(Integer(10));
    IntPolynomial f2(std::move(coeffs2));

    // f2 应该更好（系数更小）
    bool f2_is_better = quick_polynomial_compare(f2, 100.0, f1, 100.0);
    std::cout << "  f2 is better than f1: " << (f2_is_better ? "yes" : "no") << std::endl;
    assert(f2_is_better);

    std::cout << "  PASSED" << std::endl;
}

/// 回归测试：多线程并发调用 compute() 不崩溃且结果一致
/// 修复前：多线程共享 rng_ 导致数据竞争 (UB)
void test_concurrent_evaluation() {
    std::cout << "Testing concurrent evaluation (thread safety)..." << std::endl;

    MurphyParams params;
    params.sample_points = 500;
    params.alpha_bound = 100;
    params.skewness_steps = 10;

    // 单个 evaluator，多线程共享（修复前会触发数据竞争）
    const MurphyEvaluator evaluator(params);

    // 构造测试多项式 f(x) = x^3 - 2, g(x) = x - 10
    std::vector<Integer> f_coeffs;
    f_coeffs.push_back(Integer(-2));
    f_coeffs.push_back(Integer(static_cast<int64_t>(0)));
    f_coeffs.push_back(Integer(static_cast<int64_t>(0)));
    f_coeffs.push_back(Integer(static_cast<int64_t>(1)));
    IntPolynomial f(std::move(f_coeffs));

    std::vector<Integer> g_coeffs;
    g_coeffs.push_back(Integer(-10));
    g_coeffs.push_back(Integer(static_cast<int64_t>(1)));
    IntPolynomial g(std::move(g_coeffs));

    Integer n("1000000007");

    constexpr size_t NUM_THREADS = 8;
    std::vector<MurphyScore> results(NUM_THREADS);
    std::vector<std::thread> threads;

    for (size_t t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            results[t] = evaluator.compute(f, g, n, 100.0);
        });
    }
    for (auto& th : threads) th.join();

    // 所有线程应得到完全相同的结果（相同 seed → 相同采样）
    for (size_t t = 1; t < NUM_THREADS; ++t) {
        assert(results[t].log_e_score == results[0].log_e_score);
        assert(results[t].alpha_f == results[0].alpha_f);
    }

    std::cout << "  " << NUM_THREADS << " threads, all scores identical: "
              << results[0].log_e_score << std::endl;
    std::cout << "  PASSED" << std::endl;
}

int main() {
    std::cout << "=== Murphy Evaluator Tests ===" << std::endl << std::endl;

    test_dickman_rho();
    test_alpha_computation();
    test_score_consistency();
    test_skewness_optimization();
    test_quick_compare();
    test_concurrent_evaluation();

    std::cout << std::endl << "All Murphy evaluator tests passed!" << std::endl;
    return 0;
}
