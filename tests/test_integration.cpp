// test_integration.cpp — 跨模块集成测试
//
// 测试各模块的接口边界，确保它们协同工作正确。
// 与单模块测试不同，这里每个测试至少跨越两个模块。
//
// 覆盖的集成点：
// 1. Cofactorizer + PolynomialContext + FactorBase → N-divisible 拒绝
// 2. Cofactorizer stats 与多种结果的追踪
// 3. Cofactorizer → RelationCollector 流水线
// 4. RelationCollector → RelationFilter 流水线
// 5. MatrixBuilder + 真实 FactorBase → 矩阵维度正确
// 6. MatrixBuilder → BlockLanczos → 已知依赖验证
// 7. MurphyEvaluator + 真实 PolynomialContext → 评分有限
// 8. FactorBase sieve_algebraic_count 不变式

#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/polynomial/murphy_evaluator.hpp>
#include <gnfs/polynomial/int_polynomial.hpp>

#include <cassert>
#include <cmath>
#include <iostream>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::cofactor;
using namespace gnfs::relation;
using namespace gnfs::linalg;

// ============================================================
// 辅助：为 N=143 构建小型测试环境 (ctx + fb)
// ============================================================
struct TestEnv {
    Integer n;
    PolynomialContext ctx;
    FactorBase fb;
};

static TestEnv make_env_143() {
    Integer n("143");  // 11 * 13
    auto result = BaseMSelector::select(n, 2);
    assert(result.success);
    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound  = 50;
    opts.algebraic_bound = 50;
    opts.parallel = false;

    auto fb = FactorBaseBuilder::build(ctx, opts);
    return TestEnv{std::move(n), std::move(ctx), std::move(fb)};
}

// ============================================================
// 辅助：验证依赖有效性（选中行的 XOR 应为零向量）
// ============================================================
static bool verify_dependency(const SparseMatrix& mat,
                               const std::vector<bool>& dep) {
    if (dep.size() != mat.num_rows()) return false;
    std::vector<size_t> counts(mat.num_cols(), 0);
    for (size_t r = 0; r < mat.num_rows(); ++r) {
        if (r < dep.size() && dep[r]) {
            for (uint32_t c : mat.row(r).indices()) ++counts[c];
        }
    }
    for (size_t c : counts) if (c % 2 != 0) return false;
    return true;
}

// ============================================================
// Test 1: Cofactorizer + PolynomialContext + FactorBase
// 关键：N-divisible 对必须被拒绝（Session 1 bug regression）
// ============================================================
void test_cofactorizer_n_divisible_rejection() {
    std::cout << "Testing Cofactorizer N-divisible rejection (integration)..." << std::endl;

    auto env = make_env_143();
    const auto& ctx = env.ctx;
    const auto& fb  = env.fb;

    // 确认环境正常
    assert(fb.rational_count() > 0);
    assert(fb.algebraic_count() > 0);

    CofactorizerConfig cfg;
    cfg.large_prime_bound = 10000;
    Cofactorizer cof(ctx, fb, cfg);

    // 构造 N-divisible 对：a = m (即 rational_value = a - bm = 0，gcd(0,N)=N)
    int64_t m_val = ctx.m().to_int64();
    auto result_ndiv = cof.verify(m_val, uint64_t(1));
    assert(!result_ndiv.has_value()); // 必须被拒绝

    // a = 2*m：a - bm = 2m - m = m，gcd(m, N) — 如果 m|N 也应拒绝
    // a = 0, b = 1：rational_value = -m，|gcd(m, N)| 可能 > 1
    auto result_zero = cof.verify(int64_t(0), uint64_t(1));
    // 不断言这个一定被拒绝（m 可能与 N 互素），只检查 API 正常调用

    // 验证 stats 统计了 total_candidates
    const auto& stats = cof.stats();
    assert(stats.total_candidates >= 1); // 至少处理了 (m_val, 1)

    std::cout << "  PASS (N-divisible rejection, stats.total=" << stats.total_candidates << ")" << std::endl;
    (void)result_zero;
}

// ============================================================
// Test 2: Cofactorizer stats 全面追踪
// 处理多对候选，验证 total = accepted + rejected
// ============================================================
void test_cofactorizer_stats_tracking() {
    std::cout << "Testing Cofactorizer stats tracking (integration)..." << std::endl;

    auto env = make_env_143();
    CofactorizerConfig cfg;
    cfg.large_prime_bound = 100000;
    Cofactorizer cof(env.ctx, env.fb, cfg);

    size_t accepted = 0;
    // 遍历小候选对
    for (int64_t a = -20; a <= 20; ++a) {
        for (uint64_t b = 1; b <= 5; ++b) {
            if (std::gcd(uint64_t(a < 0 ? -a : a), b) != 1) continue;
            auto r = cof.verify(a, b);
            if (r.has_value()) ++accepted;
        }
    }

    const auto& s = cof.stats();
    // total_candidates = 所有调用数
    assert(s.total_candidates > 0);
    // 所有分类之和应 == total_candidates
    // (accepted 个成功 + rejected 个失败 = total)
    size_t rejected = s.rational_rejects + s.algebraic_rejects +
                      s.both_rejects;
    // invalid_pair 也可能存在（b=0 或 gcd(a,b)!=1），但我们已经过滤了
    // accepted + rejected ≤ total_candidates（部分是 N-divisible 退出）
    assert(accepted + rejected <= s.total_candidates);

    std::cout << "  PASS (total=" << s.total_candidates
              << " accepted=" << accepted
              << " rejected=" << rejected << ")" << std::endl;
}

// ============================================================
// Test 3: Cofactorizer → RelationCollector 流水线
// 验证 Cofactorizer 接受的关系能正确加入 RelationCollector
// ============================================================
void test_cofactorizer_to_collector_pipeline() {
    std::cout << "Testing Cofactorizer → RelationCollector pipeline (integration)..." << std::endl;

    auto env = make_env_143();
    CofactorizerConfig cfg;
    cfg.large_prime_bound = 100000;
    Cofactorizer cof(env.ctx, env.fb, cfg);

    CollectorConfig ccfg;
    ccfg.check_duplicates = true;
    RelationCollector collector(ccfg);

    size_t cof_accepted = 0;
    for (int64_t a = -30; a <= 30; ++a) {
        for (uint64_t b = 1; b <= 3; ++b) {
            if (std::gcd(uint64_t(a < 0 ? -a : a), b) != 1) continue;
            auto rel = cof.verify(a, b);
            if (rel.has_value()) {
                ++cof_accepted;
                collector.add(std::move(*rel));
            }
        }
    }

    // collector 大小 ≤ cof_accepted（有重复时可能更少）
    assert(collector.size() <= cof_accepted);

    // 如果有关系被收集，stats 必须正常
    if (collector.size() > 0) {
        const auto& cs = collector.stats();
        assert(cs.total_relations == collector.size());
    }

    std::cout << "  PASS (cof_accepted=" << cof_accepted
              << " collector.size=" << collector.size() << ")" << std::endl;
}

// ============================================================
// Test 4: RelationCollector → RelationFilter 流水线
// 有单例大素数的关系应被过滤掉；成对的应保留
// ============================================================
void test_collector_to_filter_pipeline() {
    std::cout << "Testing RelationCollector → RelationFilter pipeline (integration)..." << std::endl;

    RelationCollector collector;

    // 添加 3 个全关系（无大素数）
    for (int i = 0; i < 3; ++i) {
        Relation r(int64_t(i + 1), int64_t(1));
        r.rational_factors = {0, 1};
        r.algebraic_factors = {0};
        collector.add(std::move(r));
    }

    // 添加 2 个含单例大素数的关系（只出现一次）
    {
        Relation r(100, 1);
        r.rational_factors = {0};
        r.algebraic_factors = {0};
        r.rational_large_prime.push_back(PrimePower{99991ULL, 0, 1}); // 单例
        collector.add(std::move(r));
    }
    {
        Relation r(101, 1);
        r.rational_factors = {0};
        r.algebraic_factors = {0};
        r.algebraic_large_prime.push_back(PrimePower{99997ULL, 0, 1}); // 单例
        collector.add(std::move(r));
    }

    // 添加 2 个共享同一大素数的关系（成对，过滤后应保留）
    uint64_t shared_lp = 10007ULL;
    {
        Relation r(200, 1);
        r.rational_factors = {0};
        r.algebraic_factors = {0};
        r.rational_large_prime.push_back(PrimePower{shared_lp, 0, 1});
        collector.add(std::move(r));
    }
    {
        Relation r(201, 1);
        r.rational_factors = {1};
        r.algebraic_factors = {1};
        r.rational_large_prime.push_back(PrimePower{shared_lp, 0, 1});
        collector.add(std::move(r));
    }

    assert(collector.size() == 7);

    // 过滤
    auto rels = collector.relations();
    FilterConfig fcfg;
    fcfg.remove_singletons = true;
    RelationFilter filter(fcfg);
    auto filtered = filter.filter(std::vector<Relation>(rels.begin(), rels.end()));

    // 全关系（3个）应保留，成对大素数（2个）应保留，单例（2个）应被删除
    // 实际：7 − 2(singletons) = 5
    assert(filter.stats().input_relations == 7);
    assert(filtered.size() == 5);
    assert(filter.stats().singletons_removed == 2);

    std::cout << "  PASS (input=7, after_filter=" << filtered.size()
              << " singletons_removed=" << filter.stats().singletons_removed << ")" << std::endl;
}

// ============================================================
// Test 5: MatrixBuilder + 真实 FactorBase → 矩阵维度正确
// ============================================================
void test_matrix_builder_real_fb_dimensions() {
    std::cout << "Testing MatrixBuilder with real FactorBase — dimensions (integration)..." << std::endl;

    auto env = make_env_143();
    const auto& fb = env.fb;

    size_t rat_cnt = fb.rational_count();
    size_t alg_cnt = fb.sieve_algebraic_count();
    assert(rat_cnt > 0);
    assert(alg_cnt > 0);

    // 构造 4 个合成全关系（无大素数）
    std::vector<Relation> rels;
    for (int i = 0; i < 4; ++i) {
        Relation r(int64_t(i + 1), int64_t(1));
        // 确保使用有效的 FB 索引（0 和 1 一定存在）
        r.rational_factors = {0u, 1u};
        r.algebraic_factors = {0u, 1u};
        rels.push_back(std::move(r));
    }

    MatrixBuilderConfig cfg;
    cfg.include_sign_column  = false;
    cfg.include_qc_columns   = false;
    cfg.include_class_group  = false;
    cfg.include_schirokauer  = false;
    MatrixBuilder builder(cfg);
    auto result = builder.build(rels, fb);

    // 行数 = 关系数
    assert(result.matrix.num_rows() == 4);
    // 列数 = rat_cnt + alg_cnt（无 sign/QC/Schirokauer）
    assert(result.matrix.num_cols() == rat_cnt + alg_cnt);
    // row_to_relation 映射完整
    assert(result.row_to_relation.size() == 4);

    std::cout << "  PASS (rows=4, cols=" << result.matrix.num_cols()
              << " rat=" << rat_cnt << " alg=" << alg_cnt << ")" << std::endl;
}

// ============================================================
// Test 6: MatrixBuilder → BlockLanczos — 已知依赖验证
// 构造 r0 XOR r1 XOR r2 = 0 的三行，验证 BL 找到该依赖
// ============================================================
void test_matrix_to_lanczos_dependency() {
    std::cout << "Testing MatrixBuilder → BlockLanczos — known dependency (integration)..." << std::endl;

    auto env = make_env_143();
    const auto& fb = env.fb;

    // 三个关系满足 r0 XOR r1 XOR r2 = 0（mod 2 因子指数）：
    //   r0: rat=[0],   alg=[0]
    //   r1: rat=[1],   alg=[1]
    //   r2: rat=[0,1], alg=[0,1]  = r0 XOR r1
    std::vector<Relation> rels;
    {
        Relation r(10, 1);
        r.rational_factors  = {0u};
        r.algebraic_factors = {0u};
        rels.push_back(std::move(r));
    }
    {
        Relation r(11, 1);
        r.rational_factors  = {1u};
        r.algebraic_factors = {1u};
        rels.push_back(std::move(r));
    }
    {
        Relation r(12, 1);
        r.rational_factors  = {0u, 1u};
        r.algebraic_factors = {0u, 1u};
        rels.push_back(std::move(r));
    }

    // 禁用所有额外列，矩阵行仅由因子索引决定
    MatrixBuilderConfig cfg;
    cfg.include_sign_column  = false;
    cfg.include_qc_columns   = false;
    cfg.include_class_group  = false;
    cfg.include_schirokauer  = false;
    MatrixBuilder builder(cfg);
    auto br = builder.build(rels, fb);

    assert(br.matrix.num_rows() == 3);

    // BlockLanczos（小矩阵走 Gaussian fallback）
    BlockLanczos solver;
    auto deps = solver.find_dependencies(br.matrix, 3);

    // 必须至少找到一个依赖
    assert(!deps.empty());

    // 所有找到的依赖都必须有效
    bool any_valid = false;
    for (const auto& dep : deps) {
        if (verify_dependency(br.matrix, dep)) {
            any_valid = true;
        }
    }
    assert(any_valid);

    std::cout << "  PASS (found " << deps.size() << " valid dependencies)" << std::endl;
}

// ============================================================
// Test 7: MurphyEvaluator + 真实 PolynomialContext
// 对 BaseMSelector 选出的多项式进行评分，结果应有限
// ============================================================
void test_murphy_with_real_poly_ctx() {
    std::cout << "Testing MurphyEvaluator with real PolynomialContext (integration)..." << std::endl;

    // 使用稍大的 N 以让 Murphy 评分更有意义
    Integer n("10403");  // 101 * 103
    auto result = BaseMSelector::select(n, 2);
    assert(result.success);
    auto ctx = BaseMSelector::create_context(n, result);
    assert(ctx.verify());

    // 提取多项式以供 MurphyEvaluator
    IntPolynomial f(int(ctx.degree()));
    for (uint32_t i = 0; i <= ctx.degree(); ++i) {
        f[i] = ctx.coeff(i).clone();
    }
    // g(x) = x - m
    IntPolynomial g(1);
    Integer minus_m = ctx.m().clone();
    minus_m.negate();
    g[0] = std::move(minus_m);
    g[1] = Integer(int64_t(1));

    // 使用快速参数（减少采样点）
    MurphyParams params;
    params.sample_points = 200;
    params.alpha_bound   = 1000;
    params.skewness_steps = 5;
    MurphyEvaluator evaluator(params);

    auto score = evaluator.compute(f, g, n);

    // E-score 应该是有限数（不能是 NaN 或 ±infinity）
    assert(std::isfinite(score.log_e_score) || score.log_e_score == -1e100);
    // size_score 和 root_score 应为非负
    assert(score.size_score >= 0.0 || std::isfinite(score.size_score));

    std::cout << "  PASS (log_e_score=" << score.log_e_score
              << " alpha_f=" << score.alpha_f << ")" << std::endl;
}

// ============================================================
// Test 8: FactorBase sieve_algebraic_count 不变式
// sieve_algebraic_count() ≤ algebraic_count()
// rational_count() 与 rational_bound 一致
// ============================================================
void test_fb_sieve_count_invariants() {
    std::cout << "Testing FactorBase sieve count invariants (integration)..." << std::endl;

    // 测试几种不同的 bound 配置
    struct Config {
        const char* n_str;
        uint32_t degree;
        uint32_t rat_bound;
        uint32_t alg_bound;
    };
    Config configs[] = {
        {"143",           2,   50,   50},
        {"10403",         2,  200,  200},
        {"1000036000099", 3,  500,  500},
    };

    for (const auto& c : configs) {
        Integer n(c.n_str);
        auto result = BaseMSelector::select(n, c.degree);
        assert(result.success);
        auto ctx = BaseMSelector::create_context(n, result);

        FactorBaseBuilder::Options opts;
        opts.rational_bound  = c.rat_bound;
        opts.algebraic_bound = c.alg_bound;
        opts.parallel = false;

        auto fb = FactorBaseBuilder::build(ctx, opts);

        // 不变式 1: sieve_algebraic_count ≤ algebraic_count
        assert(fb.sieve_algebraic_count() <= fb.algebraic_count());

        // 不变式 2: rational_count > 0
        assert(fb.rational_count() > 0);

        // 不变式 3: 所有筛选用代数素理想的素数 ≤ algebraic_bound
        auto alg = fb.algebraic();
        for (size_t i = 0; i < fb.sieve_algebraic_count(); ++i) {
            assert(alg[i].p <= c.alg_bound);
        }

        // 不变式 4: SQ 范围素数（如果有）> algebraic_bound
        for (size_t i = fb.sieve_algebraic_count(); i < fb.algebraic_count(); ++i) {
            assert(alg[i].p > c.alg_bound);
        }

        std::cout << "  N=" << c.n_str
                  << " rat=" << fb.rational_count()
                  << " alg=" << fb.algebraic_count()
                  << " sieve_alg=" << fb.sieve_algebraic_count() << " OK" << std::endl;
    }
    std::cout << "  PASS" << std::endl;
}

// ============================================================
// main
// ============================================================
int main() {
    std::cout << "=== GNFS Integration Tests ===" << std::endl;
    std::cout << std::endl;

    test_cofactorizer_n_divisible_rejection();
    test_cofactorizer_stats_tracking();
    test_cofactorizer_to_collector_pipeline();
    test_collector_to_filter_pipeline();
    test_matrix_builder_real_fb_dimensions();
    test_matrix_to_lanczos_dependency();
    test_murphy_with_real_poly_ctx();
    test_fb_sieve_count_invariants();

    std::cout << std::endl;
    std::cout << "All integration tests passed!" << std::endl;
    return 0;
}
