/// test_factor_with_kleinjung.cpp - 使用 Kleinjung 多项式选择的 GNFS 分解测试
///
/// 演示完整的 GNFS 流程，比较 Base-m 和 Kleinjung 多项式选择的效果

#include <gnfs/polynomial/kleinjung_selector.hpp>
#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::sieve;
using namespace gnfs::cofactor;
using namespace gnfs::relation;
using namespace gnfs::linalg;
using namespace gnfs::sqrt;

// Helper wrapper for find_dependencies (BlockLanczos method)
inline std::vector<std::vector<bool>> find_dependencies(const SparseMatrix& mat, size_t max_deps) {
    BlockLanczos solver;
    return solver.find_dependencies(mat, max_deps);
}

// Helper to verify a dependency: sum of selected rows should XOR to zero
inline bool verify_dependency(const SparseMatrix& mat, const std::vector<bool>& dep) {
    if (dep.size() != mat.num_rows()) return false;

    // Count set bits in each column for selected rows
    std::vector<size_t> col_count(mat.num_cols(), 0);
    for (size_t row = 0; row < mat.num_rows(); ++row) {
        if (row < dep.size() && dep[row]) {
            for (uint32_t col : mat.row(row).indices()) {
                col_count[col]++;
            }
        }
    }

    // All columns should have even count (XOR to zero)
    for (size_t c = 0; c < col_count.size(); ++c) {
        if (col_count[c] % 2 != 0) return false;
    }
    return true;
}

// Convert std::vector<bool> to BitVector
inline BitVector to_bitvector(const std::vector<bool>& vec) {
    BitVector bv(vec.size());
    for (size_t i = 0; i < vec.size(); ++i) {
        if (vec[i]) bv.set(i);
    }
    return bv;
}

// 计时器
class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}
    double elapsed_ms() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }
    double elapsed_sec() const { return elapsed_ms() / 1000.0; }
    void reset() { start_ = std::chrono::high_resolution_clock::now(); }
private:
    std::chrono::high_resolution_clock::time_point start_;
};

// 打印分隔线
void print_section(const char* title) {
    std::cout << "\n========================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "========================================\n";
    std::cout.flush();
}

// 打印多项式
void print_poly(const IntPolynomial& f, const char* name) {
    std::cout << name << "(x) = ";
    bool first = true;
    for (int i = static_cast<int>(f.degree()); i >= 0; --i) {
        if (f[i].is_zero() && i > 0) continue;
        if (!first) {
            std::cout << (f[i].is_negative() ? " - " : " + ");
        } else if (f[i].is_negative()) {
            std::cout << "-";
        }
        first = false;
        Integer abs_c = f[i].clone();
        abs_c.abs();
        if (i == 0 || abs_c.to_string() != "1") {
            std::cout << abs_c.to_string();
            if (i > 0) std::cout << "*";
        }
        if (i > 0) std::cout << "x" << (i > 1 ? "^" + std::to_string(i) : "");
    }
    std::cout << "\n";
}

// 分解结果
struct GNFSFactorResult {
    Integer n;
    Integer p, q;
    bool success = false;
    size_t relations = 0;
    double time_sec = 0.0;
    std::string poly_method;
    double murphy_log_e = 0.0;
};

// 使用给定的多项式上下文进行 GNFS 分解
GNFSFactorResult factor_with_context(
        const Integer& n,
        const PolynomialContext& ctx,
        const std::string& method_name,
        double murphy_log_e,
        bool verbose = true) {

    GNFSFactorResult result;
    result.n = n.clone();
    result.poly_method = method_name;
    result.murphy_log_e = murphy_log_e;

    Timer total_timer;

    // ============ 因子基构建 ============
    if (verbose) std::cout << "\n[Factor Base Construction]\n";

    size_t bits = n.bit_length();
    uint32_t fb_bound = 5000;
    if (bits > 50) fb_bound = 10000;
    if (bits > 70) fb_bound = 30000;

    uint32_t sq_max = fb_bound * 3;

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = fb_bound;
    fb_opts.algebraic_bound = fb_bound;
    fb_opts.special_q_bound = sq_max;
    fb_opts.parallel = true;

    // Use static build method
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    if (verbose) {
        std::cout << "  Rational primes: " << fb.rational_count() << "\n";
        std::cout << "  Algebraic primes: " << fb.algebraic_count() << "\n";
        std::cout.flush();
    }

    // ============ 筛选 ============
    if (verbose) std::cout << "\n[Sieving]\n";

    SieveParams sieve_params;
    sieve_params.rational_threshold = 50;
    sieve_params.algebraic_threshold = 50;

    SieveRegion sieve_region;
    sieve_region.i_min = -3000;
    sieve_region.i_max = 2999;
    sieve_region.j_min = 1;
    sieve_region.j_max = 800;

    CofactorizerConfig cofac_config;
    cofac_config.large_prime_bound = fb.params().large_prime_bound;
    cofac_config.allow_1lp = true;
    cofac_config.allow_2lp = true;

    Cofactorizer cofactorizer(ctx, fb, cofac_config);

    SpecialQRange sq_range;
    sq_range.min_q = fb_bound + 1;
    sq_range.max_q = sq_max;

    SpecialQGenerator sq_gen(fb, sq_range);

    CollectorConfig coll_config;
    coll_config.check_duplicates = true;
    RelationCollector collector(coll_config);

    size_t target_relations = fb.rational_count() + fb.sieve_algebraic_count() + 100;
    // LP-aware: need more raw relations for singleton filtering
    if (cofac_config.large_prime_bound > fb_bound) {
        double lp_primes = (static_cast<double>(cofac_config.large_prime_bound) - fb_bound) /
                           std::log(static_cast<double>(cofac_config.large_prime_bound));
        target_relations = std::max(target_relations, static_cast<size_t>(lp_primes * 3));
    }

    LatticeSieve sieve(ctx, fb, sieve_params);
    sieve.set_region(sieve_region);

    size_t sq_count = 0;
    size_t max_sq = 3000;

    while (sq_gen.has_next() && collector.size() < target_relations && sq_count < max_sq) {
        auto sq = sq_gen.next();
        if (!sq) break;

        auto sieve_result = sieve.sieve_special_q(*sq);

        for (const auto& cand : sieve_result.candidates) {
            auto rel_opt = cofactorizer.verify(cand);
            if (rel_opt) {
                collector.add(std::move(*rel_opt));
            }
        }

        ++sq_count;

        if (verbose && sq_count % 200 == 0) {
            std::cout << "  SQ #" << sq_count << ": " << collector.size() << " relations\n";
            std::cout.flush();
        }
    }

    if (verbose) {
        std::cout << "  Total relations: " << collector.size() << "\n";
    }

    result.relations = collector.size();

    if (collector.size() < 10) {
        if (verbose) std::cout << "  Not enough relations!\n";
        result.time_sec = total_timer.elapsed_sec();
        return result;
    }

    // ============ 过滤 ============
    auto relations = collector.get_relations();

    FilterConfig filter_config;
    filter_config.remove_singletons = true;
    filter_config.max_passes = 10;

    RelationFilter filter(filter_config);
    relations = filter.filter(std::move(relations));

    if (verbose) {
        std::cout << "  After filtering: " << relations.size() << " relations\n";
    }

    if (relations.size() < 5) {
        result.time_sec = total_timer.elapsed_sec();
        return result;
    }

    // ============ 线性代数 ============
    if (verbose) std::cout << "\n[Linear Algebra]\n";

    MatrixBuilderConfig mb_config;
    mb_config.include_sign_column = true;
    mb_config.include_qc_columns = true;
    mb_config.include_class_group = true;
    mb_config.include_schirokauer = true;
    mb_config.num_qc_primes = 64;
    mb_config.verbose = false;

    MatrixBuilder mb(mb_config);
    auto build_result = mb.build_with_qc(relations, fb, ctx);

    auto matrix_stats = compute_matrix_stats(build_result.matrix);

    if (verbose) {
        std::cout << "  Matrix: " << matrix_stats.num_rows << " x " << matrix_stats.num_cols << "\n";
        std::cout << "  Excess: " << matrix_stats.excess << "\n";
    }

    if (!matrix_stats.has_excess()) {
        result.time_sec = total_timer.elapsed_sec();
        return result;
    }

    auto dependencies = find_dependencies(build_result.matrix, 50);

    if (verbose) {
        std::cout << "  Dependencies: " << dependencies.size() << "\n";
    }

    if (dependencies.empty()) {
        result.time_sec = total_timer.elapsed_sec();
        return result;
    }

    // ============ 平方根和因子提取 ============
    if (verbose) std::cout << "\n[Factor Extraction]\n";

    for (size_t dep_idx = 0; dep_idx < dependencies.size(); ++dep_idx) {
        const auto& dep = dependencies[dep_idx];

        if (!verify_dependency(build_result.matrix, dep)) continue;

        auto rat_result = compute_rational_sqrt(to_bitvector(dep), relations, fb, n, ctx.m());
        if (!rat_result.success) continue;

        auto alg_result = compute_algebraic_sqrt(to_bitvector(dep), relations, ctx);
        if (!alg_result.success) {
            alg_result.value = Integer(1);
        }

        // 尝试 Y 和 -Y
        Integer alg_neg = n.clone();
        alg_neg -= alg_result.value;

        auto factors = extract_factors(rat_result.value, alg_result.value, n);

        auto is_nontrivial = [&n](const Integer& f) {
            if (f.fits_uint64() && f.to_uint64() == 1) return false;
            return f.compare(n) != 0;
        };

        Integer f1, f2;
        bool found = false;

        if (is_nontrivial(factors.factor1)) {
            f1 = factors.factor1.clone();
            f2 = n.clone();
            f2 /= f1;
            found = true;
        } else if (is_nontrivial(factors.factor2)) {
            f1 = factors.factor2.clone();
            f2 = n.clone();
            f2 /= f1;
            found = true;
        }

        if (!found) {
            auto factors_neg = extract_factors(rat_result.value, alg_neg, n);
            if (is_nontrivial(factors_neg.factor1)) {
                f1 = factors_neg.factor1.clone();
                f2 = n.clone();
                f2 /= f1;
                found = true;
            } else if (is_nontrivial(factors_neg.factor2)) {
                f1 = factors_neg.factor2.clone();
                f2 = n.clone();
                f2 /= f1;
                found = true;
            }
        }

        if (found) {
            Integer check = f1.clone();
            check *= f2;
            if (check.compare(n) == 0 && is_nontrivial(f1) && is_nontrivial(f2)) {
                result.p = std::move(f1);
                result.q = std::move(f2);
                result.success = true;
                break;
            }
        }
    }

    result.time_sec = total_timer.elapsed_sec();
    return result;
}

// 测试分解
void test_factorization(const char* name, const Integer& n, uint32_t degree) {
    print_section(name);

    std::cout << "N = " << n.to_string() << "\n";
    std::cout << "Digits: " << n.num_digits() << ", Bits: " << n.bit_length() << "\n";
    std::cout << "Polynomial degree: " << degree << "\n";

    // ============ Base-m 多项式选择 ============
    std::cout << "\n--- Method 1: Base-m ---\n";

    Timer basem_timer;
    auto basem_result = BaseMSelector::select(n, degree);

    if (!basem_result.success) {
        std::cout << "Base-m selection failed!\n";
        return;
    }

    auto basem_ctx = BaseMSelector::create_context(n, basem_result);

    // 计算 Murphy E-score
    MurphyParams murphy_params;
    murphy_params.sample_points = 500;
    murphy_params.alpha_bound = 500;
    MurphyEvaluator evaluator(murphy_params);

    std::vector<Integer> g_coeffs;
    Integer neg_m = basem_result.m.clone();
    neg_m.negate();
    g_coeffs.push_back(std::move(neg_m));
    g_coeffs.push_back(Integer(1));
    IntPolynomial g_basem(std::move(g_coeffs));

    auto basem_score = evaluator.compute(basem_result.f, g_basem, n, 1.0);

    std::cout << "  ";
    print_poly(basem_result.f, "f");
    std::cout << "  m = " << basem_result.m.to_string() << "\n";
    std::cout << "  Skewness: " << 1.0 << "\n";
    std::cout << "  Murphy log(E): " << basem_score.log_e_score << "\n";
    std::cout << "  Selection time: " << basem_timer.elapsed_ms() << " ms\n";
    std::cout.flush();

    // ============ Kleinjung 多项式选择 ============
    std::cout << "\n--- Method 2: Kleinjung ---\n";
    std::cout.flush();

    KleinjungParams kparams;
    kparams.degree = degree;
    kparams.leading_coeff_bound = 3000;
    kparams.num_candidates = 300;
    kparams.search_radius = 150;
    kparams.parallel = true;
    kparams.murphy_params.sample_points = 300;
    kparams.murphy_params.alpha_bound = 300;

    KleinjungSelector selector(kparams);

    selector.set_progress_callback([](size_t cur, size_t tot, double score, const char* stage) {
        if (cur % 100 == 0 || cur == tot) {
            std::cout << "  " << stage << ": " << cur << "/" << tot
                      << " log(E)=" << std::fixed << std::setprecision(2) << score << "\n";
        }
    });

    Timer kleinjung_timer;
    auto kleinjung_result = selector.select(n);

    if (!kleinjung_result.success) {
        std::cout << "Kleinjung selection failed!\n";
        std::cout << "Using Base-m polynomial for factorization...\n";

        auto factor_result = factor_with_context(n, basem_ctx, "Base-m", basem_score.log_e_score);

        std::cout << "\n=== RESULT (Base-m only) ===\n";
        if (factor_result.success) {
            std::cout << "SUCCESS!\n";
            std::cout << "N = " << factor_result.n.to_string() << "\n";
            std::cout << "p = " << factor_result.p.to_string() << "\n";
            std::cout << "q = " << factor_result.q.to_string() << "\n";

            // 验证
            Integer check = factor_result.p.clone();
            check *= factor_result.q;
            std::cout << "Verified: " << (check.compare(n) == 0 ? "YES" : "NO") << "\n";
        } else {
            std::cout << "Factorization failed.\n";
        }
        return;
    }

    auto kleinjung_ctx = create_context_from_kleinjung(n, kleinjung_result);

    std::cout << "  ";
    print_poly(kleinjung_result.f, "f");
    std::cout << "  m = " << kleinjung_result.m.to_string() << "\n";
    std::cout << "  Skewness: " << kleinjung_result.skewness << "\n";
    std::cout << "  Murphy log(E): " << kleinjung_result.score.log_e_score << "\n";
    std::cout << "  Alpha_f: " << kleinjung_result.score.alpha_f << "\n";
    std::cout << "  Selection time: " << kleinjung_timer.elapsed_ms() << " ms\n";

    // ============ 比较 ============
    std::cout << "\n--- Comparison ---\n";
    std::cout << "  Base-m log(E):    " << std::fixed << std::setprecision(2)
              << basem_score.log_e_score << "\n";
    std::cout << "  Kleinjung log(E): " << std::fixed << std::setprecision(2)
              << kleinjung_result.score.log_e_score << "\n";
    std::cout << "  Improvement: " << std::fixed << std::setprecision(2)
              << (kleinjung_result.score.log_e_score - basem_score.log_e_score) << "\n";

    // ============ 使用更好的多项式进行分解 ============
    bool use_kleinjung = kleinjung_result.score.log_e_score > basem_score.log_e_score;

    std::cout << "\n--- Factorization using "
              << (use_kleinjung ? "Kleinjung" : "Base-m") << " polynomial ---\n";

    GNFSFactorResult factor_result;
    if (use_kleinjung) {
        factor_result = factor_with_context(n, kleinjung_ctx, "Kleinjung",
                                            kleinjung_result.score.log_e_score);
    } else {
        factor_result = factor_with_context(n, basem_ctx, "Base-m",
                                            basem_score.log_e_score);
    }

    // ============ 结果 ============
    std::cout << "\n=== FINAL RESULT ===\n";
    std::cout << "Polynomial method: " << factor_result.poly_method << "\n";
    std::cout << "Murphy log(E): " << factor_result.murphy_log_e << "\n";
    std::cout << "Relations collected: " << factor_result.relations << "\n";
    std::cout << "Total time: " << std::fixed << std::setprecision(2)
              << factor_result.time_sec << " seconds\n";

    if (factor_result.success) {
        std::cout << "\n*** FACTORIZATION SUCCESSFUL! ***\n";
        std::cout << "N = " << factor_result.n.to_string() << "\n";
        std::cout << "p = " << factor_result.p.to_string() << "\n";
        std::cout << "q = " << factor_result.q.to_string() << "\n";

        // 验证
        Integer check = factor_result.p.clone();
        check *= factor_result.q;
        std::cout << "p * q = " << check.to_string() << "\n";
        std::cout << "Verified: " << (check.compare(n) == 0 ? "YES" : "NO") << "\n";

        // 检查素性
        int p_prime = factor_result.p.is_probable_prime(25);
        int q_prime = factor_result.q.is_probable_prime(25);
        std::cout << "p is prime: " << (p_prime > 0 ? "YES" : "NO") << "\n";
        std::cout << "q is prime: " << (q_prime > 0 ? "YES" : "NO") << "\n";
    } else {
        std::cout << "\nFactorization did not succeed.\n";
        std::cout << "(May need more sieving or different parameters)\n";
    }
}

int main() {
    std::cout << "================================================================\n";
    std::cout << "    GNFS Factorization with Kleinjung Polynomial Selection\n";
    std::cout << "================================================================\n";

    // 测试 1: 小半素数 (~27 bits)
    {
        Integer p("10007");
        Integer q("10009");
        Integer n = p * q;  // 100160063
        test_factorization("Test 1: 27-bit semiprime (10007 * 10009)", n, 3);
    }

    // 测试 2: 中等半素数 (~40 bits)
    {
        Integer p("1000003");
        Integer q("1000033");
        Integer n = p * q;  // 1000036000099
        test_factorization("Test 2: 40-bit semiprime (1000003 * 1000033)", n, 3);
    }

    // 测试 3: 较大半素数 (~50 bits)
    {
        Integer p("10000019");
        Integer q("10000079");
        Integer n = p * q;  // 100000980001501
        test_factorization("Test 3: 50-bit semiprime (10000019 * 10000079)", n, 4);
    }

    std::cout << "\n================================================================\n";
    std::cout << "                    Tests Complete\n";
    std::cout << "================================================================\n";

    return 0;
}
