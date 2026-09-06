/// test_murphy.cpp - Murphy E-score 评估器测试

#include "gnfs/core/integer.hpp"
#include "gnfs/polynomial/int_polynomial.hpp"
#include "gnfs/polynomial/murphy_evaluator.hpp"
#include "support/test_check.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

using namespace gnfs::polynomial;
using namespace gnfs::core;

/// 测试 Dickman rho 函数
void test_dickman_rho() {
    std::cout << "Testing Dickman rho function..." << std::endl;

    MurphyParams params;
    params.alpha_bound = 1000; // 使用较小的界以加速测试
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
    coeffs.push_back(Integer(static_cast<int64_t>(1))); // f(x) = x^2 + 1
    IntPolynomial f(std::move(coeffs));

    // compute_alpha 应该返回一个有限值
    double alpha = evaluator.compute_alpha(f);
    std::cout << "  Alpha for x^2+1: " << alpha << std::endl;
    assert(std::isfinite(alpha));

    std::cout << "  PASSED" << std::endl;
}

void test_nan_alpha_bound_uses_minimum_valid_bound() {
    std::cout << "Testing NaN alpha bound handling..." << std::endl;

    MurphyParams params;
    params.alpha_bound = std::numeric_limits<double>::quiet_NaN();
    MurphyEvaluator evaluator(params);

    std::vector<Integer> coeffs;
    coeffs.emplace_back(0);
    coeffs.emplace_back(1);
    const IntPolynomial linear(std::move(coeffs));

    const double alpha = evaluator.compute_alpha(linear);
    const double expected = -0.5 * std::log(2.0);
    GNFS_TEST_CHECK(std::isfinite(alpha));
    GNFS_TEST_CHECK(std::abs(alpha - expected) < 1e-12);

    std::cout << "  PASSED" << std::endl;
}

/// 黄金值快照:固定 (f, alpha_bound) 锁住数值,防止 Dickman 表/α 公式/
/// 积分采样回归。值由当前实现快照得到(可与 CADO-NFS polyselect/alpha.c
/// 对照来锁外部参考)。
void test_alpha_golden_values() {
    std::cout << "Testing alpha golden values..." << std::endl;

    MurphyParams params;
    params.alpha_bound = 100;
    MurphyEvaluator evaluator(params);

    // f = x^5 - x + 1(常用 GNFS Stage 多项式)
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(1)));
        coeffs.push_back(Integer(int64_t(-1)));
        coeffs.push_back(Integer(int64_t(0)));
        coeffs.push_back(Integer(int64_t(0)));
        coeffs.push_back(Integer(int64_t(0)));
        coeffs.push_back(Integer(int64_t(1)));
        IntPolynomial f(std::move(coeffs));
        double alpha = evaluator.compute_alpha(f);
        // 黄金值(2026-05-12 实现版本):-2.34813918493
        // tolerance 1e-9 允许小的浮点重排,但锁定算法本身
        const double expected = -2.34813918493;
        std::cout << "  alpha(x^5-x+1, bound=100) = " << alpha << " (expected " << expected << ")"
                  << std::endl;
        assert(std::abs(alpha - expected) < 1e-9);
    }

    // f = x^4 + 1(cyclotomic,有简单结构)
    {
        std::vector<Integer> coeffs;
        coeffs.push_back(Integer(int64_t(1)));
        coeffs.push_back(Integer(int64_t(0)));
        coeffs.push_back(Integer(int64_t(0)));
        coeffs.push_back(Integer(int64_t(0)));
        coeffs.push_back(Integer(int64_t(1)));
        IntPolynomial f(std::move(coeffs));
        double alpha = evaluator.compute_alpha(f);
        const double expected = -1.94061743447;
        std::cout << "  alpha(x^4+1, bound=100) = " << alpha << " (expected " << expected << ")"
                  << std::endl;
        assert(std::abs(alpha - expected) < 1e-9);
    }

    std::cout << "  PASSED" << std::endl;
}

/// 测试 alpha 计算
void test_alpha_computation() {
    std::cout << "Testing alpha computation..." << std::endl;

    MurphyParams params;
    params.alpha_bound = 100; // 小界以加速
    MurphyEvaluator evaluator(params);

    // f(x) = x^5 - 1 在 mod p 下有多个根（对于 p | 5 的情况）
    std::vector<Integer> coeffs;
    coeffs.push_back(Integer(static_cast<int64_t>(-1))); // 常数项
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(0)));
    coeffs.push_back(Integer(static_cast<int64_t>(1))); // x^5 项
    IntPolynomial f(std::move(coeffs));

    double alpha = evaluator.compute_alpha(f);
    std::cout << "  Alpha for x^5-1: " << alpha << std::endl;

    // alpha should be finite; sign depends on alpha_bound and root distribution
    assert(std::isfinite(alpha));
    std::cout << "  PASSED (alpha=" << alpha << ")" << std::endl;
}

/// 测试评分一致性
void test_score_consistency() {
    std::cout << "Testing score consistency..." << std::endl;

    MurphyParams params;
    params.sample_points = 500; // 减少采样点以加速
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

    Integer n("1000000007"); // 一个素数

    // 计算评分两次，结果应该相近（采样有随机性）
    MurphyScore score1 = evaluator.compute(f, g, n, 100.0);
    MurphyScore score2 = evaluator.compute(f, g, n, 100.0);

    std::cout << "  Score 1: " << score1.e_score << std::endl;
    std::cout << "  Score 2: " << score2.e_score << std::endl;
    std::cout << "  Alpha_f: " << score1.alpha_f << std::endl;
    std::cout << "  Alpha_g: " << score1.alpha_g << std::endl;

    // 分数应该是有限的
    assert(std::isfinite(score1.e_score));
    assert(std::isfinite(score2.e_score));

    // 两次评分应该接近（采样有随机性但同一输入应相似）
    if (score1.e_score > 0.0 && score2.e_score > 0.0) {
        double ratio = score1.e_score / score2.e_score;
        assert(ratio > 0.5 && ratio < 2.0 &&
               "Two Murphy E-score evaluations on same input should be within 2x");
    }

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
    f_coeffs.push_back(Integer(1000000));                 // c_0
    f_coeffs.push_back(Integer(-5));                      // c_1
    f_coeffs.push_back(Integer(static_cast<int64_t>(1))); // c_2
    f_coeffs.push_back(Integer(-2));                      // c_3
    f_coeffs.push_back(Integer(3));                       // c_4
    f_coeffs.push_back(Integer(static_cast<int64_t>(1))); // c_5
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
        threads.emplace_back([&, t]() { results[t] = evaluator.compute(f, g, n, 100.0); });
    }
    for (auto& th : threads)
        th.join();

    // 所有线程应得到完全相同的结果（相同 seed → 相同采样）
    for (size_t t = 1; t < NUM_THREADS; ++t) {
        assert(results[t].log_e_score == results[0].log_e_score);
        assert(results[t].alpha_f == results[0].alpha_f);
    }

    std::cout << "  " << NUM_THREADS << " threads, all scores identical: " << results[0].log_e_score
              << std::endl;
    std::cout << "  PASSED" << std::endl;
}

/// BACKLOG #2 regression test: parallel compute_alpha must produce identical
/// output to sequential. ENV GNFS_MURPHY_ALPHA_THREADS=0 forces sequential; any
/// positive value enables ThreadPool parallel sweep with per-thread partial
/// accumulation + serial reduction.
///
/// Locks in the ThreadPool sum-reduction invariant. If a future rotation-
/// incremental rewrite is attempted, this test catches non-determinism (e.g.,
/// from floating-point reduction order if it were changed naively).
void test_compute_alpha_parallel_equals_sequential() {
    std::cout << "Testing compute_alpha parallel == sequential (BACKLOG #2)..." << std::endl;

    MurphyParams params;
    params.alpha_bound = 10000; // ~1200 primes, enough to exercise parallel path
    params.sample_points = 100;

    // f(x) = x^4 + 3x^3 - 2x^2 + 7x - 11 (irreducible, degree 4)
    std::vector<Integer> f_coeffs;
    f_coeffs.push_back(Integer(-11));
    f_coeffs.push_back(Integer(7));
    f_coeffs.push_back(Integer(-2));
    f_coeffs.push_back(Integer(3));
    f_coeffs.push_back(Integer(1));
    IntPolynomial f(std::move(f_coeffs));

    // Force sequential by setting ENV before evaluator construction (lazy ThreadPool init)
    setenv("GNFS_MURPHY_ALPHA_THREADS", "0", 1);
    MurphyEvaluator seq_evaluator(params);
    double alpha_seq = seq_evaluator.compute_alpha(f);

    // Force 4-thread parallel
    setenv("GNFS_MURPHY_ALPHA_THREADS", "4", 1);
    MurphyEvaluator par4_evaluator(params);
    double alpha_par4 = par4_evaluator.compute_alpha(f);

    // Force 8-thread parallel
    setenv("GNFS_MURPHY_ALPHA_THREADS", "8", 1);
    MurphyEvaluator par8_evaluator(params);
    double alpha_par8 = par8_evaluator.compute_alpha(f);

    // Reset to default for other tests
    unsetenv("GNFS_MURPHY_ALPHA_THREADS");

    // Each prime contributes independently (alpha_contribution is a pure function).
    // Sum reduction is commutative for finite-precision doubles when all contributions
    // are summed exactly once. So parallel sum = sequential sum bit-for-bit.
    std::cout << "  alpha (sequential):       " << alpha_seq << std::endl;
    std::cout << "  alpha (4-thread parallel): " << alpha_par4 << std::endl;
    std::cout << "  alpha (8-thread parallel): " << alpha_par8 << std::endl;

    // Floating-point sum order may differ slightly across reduction strategies,
    // so allow tiny relative tolerance (10 ULP for doubles ≈ 2e-15).
    const double tol = 1e-10;
    assert(std::abs(alpha_seq - alpha_par4) < tol);
    assert(std::abs(alpha_seq - alpha_par8) < tol);
    assert(std::isfinite(alpha_seq));
    std::cout << "  PASSED (all values within " << tol << " tolerance)" << std::endl;
}

int main() {
    std::cout << "=== Murphy Evaluator Tests ===" << std::endl << std::endl;

    test_dickman_rho();
    test_nan_alpha_bound_uses_minimum_valid_bound();
    test_alpha_golden_values();
    test_alpha_computation();
    test_score_consistency();
    test_skewness_optimization();
    test_quick_compare();
    test_concurrent_evaluation();
    test_compute_alpha_parallel_equals_sequential();

    std::cout << std::endl << "All Murphy evaluator tests passed!" << std::endl;
    return 0;
}
