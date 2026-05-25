/// test_kleinjung_large.cpp - Kleinjung 多项式选择器大数测试

#include "gnfs/polynomial/kleinjung_selector.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/polynomial/murphy_evaluator.hpp"
#include "gnfs/core/integer.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>

using namespace gnfs::polynomial;
using namespace gnfs::core;

/// 打印多项式
void print_polynomial(const IntPolynomial& f, const char* name) {
    std::cout << "  " << name << "(x) = ";
    bool first = true;
    for (int i = static_cast<int>(f.degree()); i >= 0; --i) {
        const size_t idx = static_cast<size_t>(i);
        if (f[idx].is_zero() && i > 0) continue;

        if (!first) {
            if (f[idx].is_negative()) {
                std::cout << " - ";
            } else {
                std::cout << " + ";
            }
        } else if (f[idx].is_negative()) {
            std::cout << "-";
        }
        first = false;

        Integer abs_c = f[idx].clone();
        abs_c.abs();

        if (i == 0) {
            std::cout << abs_c.to_string();
        } else if (abs_c.to_string() == "1") {
            if (i == 1) {
                std::cout << "x";
            } else {
                std::cout << "x^" << i;
            }
        } else {
            if (i == 1) {
                std::cout << abs_c.to_string() << "*x";
            } else {
                std::cout << abs_c.to_string() << "*x^" << i;
            }
        }
    }
    std::cout << std::endl;
}

/// 验证多项式
bool verify_polynomial(const IntPolynomial& f, const Integer& m, const Integer& n) {
    Integer fm = f.evaluate(m);
    Integer remainder;
    Integer quotient;
    Integer::divmod(quotient, remainder, fm, n);
    return remainder.is_zero();
}

/// 测试指定大小的数
void test_with_number(const char* name, const Integer& n, uint32_t degree) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Testing: " << name << std::endl;
    std::cout << "n = " << n.to_string() << std::endl;
    std::cout << "Digits: " << n.num_digits() << std::endl;
    std::cout << "Bits: " << n.bit_length() << std::endl;
    std::cout << "Degree: " << degree << std::endl;
    std::cout << "========================================" << std::endl;

    // ==================== Base-m 选择 ====================
    std::cout << "\n--- Base-m Selection ---" << std::endl;
    auto basem_start = std::chrono::high_resolution_clock::now();
    auto basem_result = BaseMSelector::select(n, degree);
    auto basem_end = std::chrono::high_resolution_clock::now();
    double basem_time = std::chrono::duration<double>(basem_end - basem_start).count();

    if (basem_result.success) {
        print_polynomial(basem_result.f, "f");
        std::cout << "  m = " << basem_result.m.to_string() << std::endl;
        std::cout << "  Skewness: " << 1.0 << std::endl;  // Base-m uses skewness = 1.0
        std::cout << "  Time: " << basem_time << " seconds" << std::endl;

        // 验证
        bool valid = verify_polynomial(basem_result.f, basem_result.m, n);
        std::cout << "  Valid: " << (valid ? "YES" : "NO") << std::endl;

        // 计算 Murphy E-score
        MurphyParams murphy_params;
        murphy_params.sample_points = 1000;
        murphy_params.alpha_bound = 1000;
        MurphyEvaluator evaluator(murphy_params);

        // 构造 g(x) = x - m
        std::vector<Integer> g_coeffs;
        Integer neg_m = basem_result.m.clone();
        neg_m.negate();
        g_coeffs.push_back(std::move(neg_m));
        g_coeffs.push_back(Integer(1));
        IntPolynomial g(std::move(g_coeffs));

        MurphyScore basem_score = evaluator.compute(basem_result.f, g, n, 1.0);  // Base-m uses skewness = 1.0
        std::cout << "  Murphy E-score: " << basem_score.e_score << std::endl;
        std::cout << "  Murphy log(E): " << basem_score.log_e_score << std::endl;
        std::cout << "  Alpha_f: " << basem_score.alpha_f << std::endl;
    } else {
        std::cout << "  Base-m selection FAILED" << std::endl;
    }

    // ==================== Kleinjung 选择 ====================
    std::cout << "\n--- Kleinjung Selection ---" << std::endl;

    KleinjungParams params;
    params.degree = degree;
    params.leading_coeff_bound = 5000;      // 领导系数上界
    params.num_candidates = 500;            // 候选数量
    params.search_radius = 200;             // 搜索半径
    params.root_opt_iterations = 100;       // 牛顿法迭代
    params.parallel = true;
    params.murphy_params.sample_points = 500;
    params.murphy_params.alpha_bound = 500;

    KleinjungSelector selector(params);

    // 设置进度回调
    size_t last_reported = 0;
    selector.set_progress_callback([&](size_t current, size_t total, double best_score, const char* stage) {
        // 每 10% 或完成时报告
        if (total > 0) {
            size_t percent = (current * 100) / total;
            if (percent >= last_reported + 10 || current == total) {
                std::cout << "  " << stage << ": " << current << "/" << total
                          << " (" << percent << "%) best E = " << std::fixed << std::setprecision(6)
                          << best_score << std::endl;
                last_reported = percent;
            }
        }
    });

    auto kleinjung_start = std::chrono::high_resolution_clock::now();
    auto kleinjung_result = selector.select(n);
    auto kleinjung_end = std::chrono::high_resolution_clock::now();
    double kleinjung_time = std::chrono::duration<double>(kleinjung_end - kleinjung_start).count();

    if (kleinjung_result.success) {
        print_polynomial(kleinjung_result.f, "f");
        std::cout << "  m = " << kleinjung_result.m.to_string() << std::endl;
        std::cout << "  Skewness: " << kleinjung_result.skewness << std::endl;
        std::cout << "  Murphy E-score: " << kleinjung_result.score.e_score << std::endl;
        std::cout << "  Murphy log(E): " << kleinjung_result.score.log_e_score << std::endl;
        std::cout << "  Alpha_f: " << kleinjung_result.score.alpha_f << std::endl;
        std::cout << "  Alpha_g: " << kleinjung_result.score.alpha_g << std::endl;
        std::cout << "  Candidates tested: " << kleinjung_result.candidates_tested << std::endl;
        std::cout << "  Time: " << kleinjung_time << " seconds" << std::endl;

        // 验证
        bool valid = verify_polynomial(kleinjung_result.f, kleinjung_result.m, n);
        std::cout << "  Valid: " << (valid ? "YES" : "NO") << std::endl;
    } else {
        std::cout << "  Kleinjung selection FAILED" << std::endl;
        std::cout << "  Candidates tested: " << kleinjung_result.candidates_tested << std::endl;
        std::cout << "  Time: " << kleinjung_time << " seconds" << std::endl;
    }

    std::cout << std::endl;
}

int main() {
    std::cout << "=== Kleinjung Large Number Tests ===" << std::endl;

    // 测试 1: 40 位数 (两个 20 位素数的乘积)
    {
        Integer p1("12345678901234567891");   // 20 位素数
        Integer p2("98765432109876543219");   // 20 位素数
        Integer n = p1 * p2;
        test_with_number("40-digit semiprime", n, 4);
    }

    // 测试 2: 50 位数
    {
        Integer p1("123456789012345678901234567891");  // ~30 位
        Integer p2("987654321098765432101");           // ~21 位
        Integer n = p1 * p2;
        test_with_number("50-digit semiprime", n, 4);
    }

    // 测试 3: 60 位数
    {
        Integer p1("1234567890123456789012345678901234567891");  // ~40 位
        Integer p2("98765432109876543219");                      // ~20 位
        Integer n = p1 * p2;
        test_with_number("60-digit semiprime", n, 5);
    }

    // 测试 4: RSA-59 类似大小 (59 位)
    {
        // RSA-59 = 71641520761751435455133616475667090434063332228247871795429
        Integer n("71641520761751435455133616475667090434063332228247871795429");
        test_with_number("RSA-59 size number", n, 5);
    }

    // 测试 5: 80 位数
    {
        Integer p1("12345678901234567890123456789012345678901234567891");  // ~50 位
        Integer p2("9876543210987654321098765432101");                     // ~31 位
        Integer n = p1 * p2;
        test_with_number("80-digit semiprime", n, 5);
    }

    std::cout << "\n=== All Large Number Tests Completed ===" << std::endl;
    return 0;
}
