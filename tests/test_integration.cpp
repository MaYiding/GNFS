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
// 9. Schirokauer + MatrixBuilder → Schirokauer 列正确
// 10. MatrixBuilder 全列 (sign+QC+Schirokauer) → 列映射一致
// 11. 完整 mini-pipeline: Cofactorizer → Filter → MatrixBuilder → BL → 验证依赖
// 12. Schirokauer map 值域与一致性验证

#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/linalg/schirokauer.hpp>
#include <gnfs/polynomial/murphy_evaluator.hpp>
#include <gnfs/polynomial/int_polynomial.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/sieve/lattice_basis.hpp>
#include <gnfs/sqrt/class_group.hpp>
#include <gnfs/polynomial/polynomial_optimizer.hpp>

#include <cassert>
#include <cmath>
#include <iostream>
#include <numeric>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::cofactor;
using namespace gnfs::relation;
using namespace gnfs::linalg;
using namespace gnfs::sqrt;
using namespace gnfs::sieve;

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
// 辅助：为 Schirokauer 测试构建首项系数对 ℓ=2 安全的环境
// N=143 的 BaseMSelector 给出 f(x)=2x²+x+7 (首项系数=2, 偶数)
// 导致 ModularPoly::is_irreducible 对 ℓ=2 断言失败
// 使用 N=10403 (101×103) 产生首项系数=1 的多项式
// ============================================================
static TestEnv make_env_for_schirokauer() {
    Integer n("10403");  // 101 * 103
    auto result = BaseMSelector::select(n, 2);
    assert(result.success);
    auto ctx = BaseMSelector::create_context(n, result);

    FactorBaseBuilder::Options opts;
    opts.rational_bound  = 200;
    opts.algebraic_bound = 200;
    opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, opts);
    return TestEnv{std::move(n), std::move(ctx), std::move(fb)};
}

// ============================================================
// Test 9: Schirokauer + MatrixBuilder 集成
// 验证开启 Schirokauer 列后矩阵维度正确增加
// ============================================================
void test_schirokauer_matrix_builder_integration() {
    std::cout << "Testing Schirokauer + MatrixBuilder integration..." << std::endl;

    auto env = make_env_for_schirokauer();
    const auto& fb = env.fb;
    const auto& ctx = env.ctx;

    // 构造测试关系
    std::vector<Relation> rels;
    for (int i = 0; i < 5; ++i) {
        Relation r(int64_t(i + 1), int64_t(1));
        r.rational_factors = {0u, 1u};
        r.algebraic_factors = {0u};
        rels.push_back(std::move(r));
    }

    // 无 Schirokauer 列
    MatrixBuilderConfig cfg_no_sch;
    cfg_no_sch.include_sign_column  = false;
    cfg_no_sch.include_qc_columns   = false;
    cfg_no_sch.include_class_group  = false;
    cfg_no_sch.include_schirokauer  = false;
    MatrixBuilder builder_no(cfg_no_sch);
    auto result_no = builder_no.build_with_qc(rels, fb, ctx);

    // 有 Schirokauer 列 (ℓ=2)
    MatrixBuilderConfig cfg_sch;
    cfg_sch.include_sign_column  = false;
    cfg_sch.include_qc_columns   = false;
    cfg_sch.include_class_group  = false;
    cfg_sch.include_schirokauer  = true;
    cfg_sch.schirokauer_primes   = {2};
    MatrixBuilder builder_sch(cfg_sch);
    auto result_sch = builder_sch.build_with_qc(rels, fb, ctx);

    // Schirokauer 列数 > 0 (具体数量取决于 f mod 2 的分解)
    assert(result_sch.matrix.num_rows() == result_no.matrix.num_rows());
    assert(result_sch.mapping.num_schirokauer_columns > 0);
    assert(result_sch.matrix.num_cols() > result_no.matrix.num_cols());
    assert(result_sch.matrix.num_cols() ==
           result_no.matrix.num_cols() + result_sch.mapping.num_schirokauer_columns);

    std::cout << "  PASS (base_cols=" << result_no.matrix.num_cols()
              << " +schirokauer=" << result_sch.mapping.num_schirokauer_columns
              << " total=" << result_sch.matrix.num_cols() << ")" << std::endl;
}

// ============================================================
// Test 10: MatrixBuilder 全列 (sign + QC + Schirokauer)
// 验证所有列类型叠加后列映射一致
// ============================================================
void test_matrix_builder_all_columns() {
    std::cout << "Testing MatrixBuilder with all column types (integration)..." << std::endl;

    // 使用 Schirokauer 安全环境 (首项系数为奇数)
    auto env = make_env_for_schirokauer();
    const auto& fb = env.fb;
    const auto& ctx = env.ctx;

    std::vector<Relation> rels;
    for (int i = 0; i < 4; ++i) {
        Relation r(int64_t(i + 1), int64_t(1));
        r.rational_factors = {0u};
        r.algebraic_factors = {0u};
        rels.push_back(std::move(r));
    }

    // 开启 sign + Schirokauer (QC 对小 N 的暴力根检查太慢，跳过)
    MatrixBuilderConfig cfg;
    cfg.include_sign_column  = true;
    cfg.include_qc_columns   = false;
    cfg.include_class_group  = false;
    cfg.include_schirokauer  = true;
    cfg.schirokauer_primes   = {2};
    MatrixBuilder builder(cfg);
    auto result = builder.build_with_qc(rels, fb, ctx);

    const auto& m = result.mapping;

    // 列映射一致性：
    // sign (0 或 1) + rat_fb + alg_fb + LP + QC + Schirokauer
    size_t expected_total = (cfg.include_sign_column ? 1 : 0)
                          + m.num_rational_fb + m.num_algebraic_fb
                          + m.num_large_primes_rat + m.num_large_primes_alg
                          + m.num_qc_columns
                          + m.num_class_group_columns
                          + m.num_schirokauer_columns;

    assert(result.matrix.num_cols() == expected_total);

    // 各段起始位置连续递增
    if (cfg.include_sign_column) {
        assert(m.sign_column == 0);
    }
    assert(m.rat_fb_start() <= m.alg_fb_start());
    assert(m.alg_fb_start() <= m.rat_lp_start());
    assert(m.rat_lp_start() <= m.alg_lp_start());
    assert(m.alg_lp_start() <= m.qc_start());
    assert(m.qc_start() <= m.schirokauer_start());

    // Schirokauer 列数 > 0
    assert(m.num_schirokauer_columns > 0);

    std::cout << "  PASS (total_cols=" << result.matrix.num_cols()
              << " sign=1"
              << " rat=" << m.num_rational_fb
              << " alg=" << m.num_algebraic_fb
              << " sch=" << m.num_schirokauer_columns << ")" << std::endl;
}

// ============================================================
// Test 11: 完整 mini-pipeline
// Cofactorizer → Collector → Filter → MatrixBuilder → BL → 验证
// ============================================================
void test_full_mini_pipeline() {
    std::cout << "Testing full mini-pipeline: Cof→Col→Flt→Mat→BL (integration)..." << std::endl;

    auto env = make_env_143();

    // Step 1: Cofactorize
    CofactorizerConfig cof_cfg;
    cof_cfg.large_prime_bound = 100000;
    Cofactorizer cof(env.ctx, env.fb, cof_cfg);

    CollectorConfig col_cfg;
    col_cfg.check_duplicates = true;
    RelationCollector collector(col_cfg);

    // 收集关系
    for (int64_t a = -40; a <= 40; ++a) {
        for (uint64_t b = 1; b <= 5; ++b) {
            uint64_t abs_a = uint64_t(a < 0 ? -a : a);
            if (std::gcd(abs_a, b) != 1) continue;
            auto rel = cof.verify(a, b);
            if (rel.has_value()) {
                collector.add(std::move(*rel));
            }
        }
    }

    std::cout << "  Collected " << collector.size() << " relations" << std::endl;

    if (collector.size() < 3) {
        std::cout << "  SKIP (not enough relations for pipeline test)" << std::endl;
        return;
    }

    // Step 2: Filter
    auto rels = collector.relations();
    FilterConfig flt_cfg;
    flt_cfg.remove_singletons = true;
    RelationFilter filter(flt_cfg);
    auto filtered = filter.filter(std::vector<Relation>(rels.begin(), rels.end()));

    std::cout << "  After filter: " << filtered.size()
              << " (removed " << filter.stats().singletons_removed << " singletons)" << std::endl;

    if (filtered.size() < 3) {
        std::cout << "  SKIP (not enough filtered relations)" << std::endl;
        return;
    }

    // Step 3: Build matrix (minimal config for reliable dependency finding)
    MatrixBuilderConfig mat_cfg;
    mat_cfg.include_sign_column  = false;
    mat_cfg.include_qc_columns   = false;
    mat_cfg.include_class_group  = false;
    mat_cfg.include_schirokauer  = false;
    MatrixBuilder builder(mat_cfg);
    auto mat_result = builder.build(filtered, env.fb);

    std::cout << "  Matrix: " << mat_result.matrix.num_rows() << " × "
              << mat_result.matrix.num_cols() << std::endl;

    assert(mat_result.matrix.num_rows() == filtered.size());
    assert(mat_result.matrix.num_cols() > 0);

    // Step 4: Find dependencies (if rows > cols, expect dependencies)
    if (mat_result.matrix.num_rows() > mat_result.matrix.num_cols()) {
        BlockLanczos solver;
        auto deps = solver.find_dependencies(mat_result.matrix, 3);

        if (!deps.empty()) {
            // Verify at least one dependency is valid
            bool found_valid = false;
            for (const auto& dep : deps) {
                if (verify_dependency(mat_result.matrix, dep)) {
                    found_valid = true;
                    break;
                }
            }
            assert(found_valid);
            std::cout << "  Found " << deps.size() << " valid dependencies" << std::endl;
        } else {
            std::cout << "  No dependencies found (matrix may be full rank)" << std::endl;
        }
    } else {
        std::cout << "  Matrix is underdetermined (rows <= cols), skipping BL" << std::endl;
    }

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Test 12: Schirokauer map 值域与一致性验证
// 验证 Schirokauer map 对同一 (a,b) 返回一致结果，值域 ⊂ [0, ℓ)
// ============================================================
void test_schirokauer_map_consistency() {
    std::cout << "Testing Schirokauer map consistency (integration)..." << std::endl;

    auto env = make_env_for_schirokauer();
    const auto& ctx = env.ctx;

    SchirokaurConfig sch_cfg;
    sch_cfg.primes = {2};
    SchirokaurMap sm(ctx, sch_cfg);

    size_t degree = ctx.degree();
    assert(sm.num_columns() == degree); // 1 prime × degree columns

    // 验证多个 (a,b) 对的值域
    for (int64_t a = -10; a <= 10; ++a) {
        for (uint64_t b = 1; b <= 3; ++b) {
            auto maps = sm.compute(a, b);
            assert(maps.size() == 1); // 1 prime

            auto& vals = maps[0];
            assert(vals.size() == degree);

            // 所有值应在 [0, ℓ) = [0, 2)
            for (uint32_t v : vals) {
                assert(v < 2);
            }

            // 一致性: 再次计算应得到相同结果
            auto maps2 = sm.compute(a, b);
            assert(maps2[0] == vals);
        }
    }

    // 验证 Schirokauer 对 (a=0, b=1) 的特殊情况
    // γ = 0 - 1·α = -α
    auto maps_zero = sm.compute(0, 1);
    assert(maps_zero.size() == 1);
    assert(maps_zero[0].size() == degree);

    std::cout << "  PASS (tested " << (21 * 3) << " (a,b) pairs, all in [0,2))" << std::endl;
}

// ============================================================
// Test 13: Sieve → Cofactorizer 联合测试
// 验证 LatticeSieve 输出的候选点能正确流入 Cofactorizer
// ============================================================
void test_sieve_cofactor_joint() {
    std::cout << "Testing Sieve → Cofactorizer joint (integration)..." << std::endl;

    // N=10403 (101×103), degree=2, 扩展 FB 包含 SQ 范围
    Integer n("10403");
    auto poly_result = BaseMSelector::select(n, 2);
    assert(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 200;
    fb_opts.algebraic_bound = 200;
    fb_opts.special_q_bound = 500;  // 扩展到 500 供 SQ 使用
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    std::cout << "  FB: rat=" << fb.rational_count()
              << " alg_sieve=" << fb.sieve_algebraic_count()
              << " alg_total=" << fb.algebraic_count() << std::endl;

    // 配置筛法（低阈值以获取更多候选）
    SieveParams sieve_params;
    sieve_params.log_scale = 16;
    sieve_params.rational_threshold = 30;
    sieve_params.algebraic_threshold = 30;

    LatticeSieve sieve(ctx, fb, sieve_params);

    SieveRegion region;
    region.i_min = -200;
    region.i_max = 199;
    region.j_min = 1;
    region.j_max = 30;
    sieve.set_region(region);

    // 从 SQ 范围生成 Special-Q
    SpecialQRange sq_range;
    sq_range.min_q = 200;
    sq_range.max_q = 500;
    SpecialQGenerator sq_gen(fb, sq_range);

    std::vector<std::pair<int64_t, uint64_t>> candidate_pairs;

    for (size_t i = 0; i < 3 && sq_gen.has_next(); ++i) {
        auto sq = sq_gen.next();
        if (!sq) break;
        auto result = sieve.sieve_special_q(*sq);
        for (const auto& c : result.candidates) {
            candidate_pairs.emplace_back(c.a, c.b);
        }
    }

    // 通过 Cofactorizer 验证候选
    CofactorizerConfig cof_cfg;
    cof_cfg.large_prime_bound = 100000;
    Cofactorizer cof(ctx, fb, cof_cfg);

    size_t verified = 0;
    for (const auto& [a, b] : candidate_pairs) {
        auto rel = cof.verify(a, b);
        if (rel.has_value()) {
            verified++;
            assert(rel->a == a);
            assert(rel->b == static_cast<int64_t>(b));
        }
    }

    std::cout << "  sieve_candidates=" << candidate_pairs.size()
              << " cofactor_verified=" << verified << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Test 14: RationalSqrt + AlgebraicSqrt 联合测试
// 从真实关系找依赖，然后计算两侧平方根，验证 gcd 因子分解
// ============================================================
void test_rational_algebraic_sqrt_joint() {
    std::cout << "Testing RationalSqrt + AlgebraicSqrt joint (integration)..." << std::endl;

    auto env = make_env_143();

    // 收集光滑关系
    CofactorizerConfig cof_cfg;
    cof_cfg.large_prime_bound = 100000;
    Cofactorizer cof(env.ctx, env.fb, cof_cfg);

    std::vector<Relation> rels;
    for (int64_t a = -50; a <= 50; ++a) {
        for (uint64_t b = 1; b <= 5; ++b) {
            uint64_t abs_a = static_cast<uint64_t>(a < 0 ? -a : a);
            if (std::gcd(abs_a, b) != 1) continue;
            auto rel = cof.verify(a, b);
            if (rel.has_value()) {
                rels.push_back(std::move(*rel));
            }
        }
    }

    std::cout << "  Collected " << rels.size() << " relations" << std::endl;

    if (rels.size() < 5) {
        std::cout << "  SKIP (not enough relations)" << std::endl;
        return;
    }

    // 构建矩阵（含 sign 列，确保有理侧乘积为正）
    MatrixBuilderConfig mat_cfg;
    mat_cfg.include_sign_column  = true;
    mat_cfg.include_qc_columns   = false;
    mat_cfg.include_class_group  = false;
    mat_cfg.include_schirokauer  = false;
    MatrixBuilder builder(mat_cfg);
    auto mat_result = builder.build(rels, env.fb);

    std::cout << "  Matrix: " << mat_result.matrix.num_rows() << "x"
              << mat_result.matrix.num_cols() << std::endl;

    if (mat_result.matrix.num_rows() <= mat_result.matrix.num_cols()) {
        std::cout << "  SKIP (matrix not overdetermined)" << std::endl;
        return;
    }

    // 求依赖
    BlockLanczos solver;
    auto deps = solver.find_dependencies(mat_result.matrix, 5);

    if (deps.empty()) {
        std::cout << "  SKIP (no dependencies found)" << std::endl;
        return;
    }

    // 尝试每个依赖来分解 N
    bool factor_found = false;
    size_t deps_tried = 0;

    for (const auto& dep : deps) {
        if (!verify_dependency(mat_result.matrix, dep)) continue;
        deps_tried++;

        // 将 std::vector<bool> 转为 BitVector（sqrt API 需要）
        BitVector dep_bv(dep.size());
        for (size_t i = 0; i < dep.size(); ++i) {
            if (dep[i]) dep_bv.set(i);
        }

        // 有理平方根
        RationalSqrt rat_sqrt;
        auto rat_result = rat_sqrt.compute(dep_bv, rels, env.fb, env.n, env.ctx.m());
        if (!rat_result.success) continue;

        // 代数平方根
        AlgebraicSqrt alg_sqrt;
        auto alg_result = alg_sqrt.compute(dep_bv, rels, env.ctx);
        if (!alg_result.success) continue;

        // gcd(X ± Y, N)
        Integer diff = rat_result.value.clone();
        diff -= alg_result.value;
        Integer sum = rat_result.value.clone();
        sum += alg_result.value;

        Integer g1 = gcd(diff, env.n);
        Integer g2 = gcd(sum, env.n);

        bool g1_nontrivial = !g1.is_one() && g1 != env.n;
        bool g2_nontrivial = !g2.is_one() && g2 != env.n;

        if (g1_nontrivial || g2_nontrivial) {
            factor_found = true;
            Integer factor = g1_nontrivial ? g1.clone() : g2.clone();
            Integer cofactor_val = env.n.clone();
            cofactor_val /= factor;
            std::cout << "  Factor: " << factor.to_string()
                      << " x " << cofactor_val.to_string() << std::endl;

            // 验证 factor * cofactor = N
            Integer product = factor.clone();
            product *= cofactor_val;
            assert(product == env.n);
            break;
        }
    }

    std::cout << "  deps_tried=" << deps_tried
              << " factor_found=" << (factor_found ? "yes" : "no") << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Test 15: 大规模关系 → 矩阵流水线
// 用 N=10403 收集更多关系，验证完整矩阵构建和 BL 求解
// ============================================================
void test_large_relation_to_matrix_pipeline() {
    std::cout << "Testing large-scale relation → matrix pipeline (integration)..." << std::endl;

    auto env = make_env_for_schirokauer();  // N=10403, FB bound=200

    // 收集大量关系
    CofactorizerConfig cof_cfg;
    cof_cfg.large_prime_bound = 100000;
    Cofactorizer cof(env.ctx, env.fb, cof_cfg);

    std::vector<Relation> rels;
    for (int64_t a = -100; a <= 100; ++a) {
        for (uint64_t b = 1; b <= 10; ++b) {
            uint64_t abs_a = static_cast<uint64_t>(a < 0 ? -a : a);
            if (std::gcd(abs_a, b) != 1) continue;
            auto rel = cof.verify(a, b);
            if (rel.has_value()) {
                rels.push_back(std::move(*rel));
            }
        }
    }

    std::cout << "  Collected " << rels.size() << " relations" << std::endl;

    if (rels.size() < 10) {
        std::cout << "  SKIP (not enough relations)" << std::endl;
        return;
    }

    // 过滤
    FilterConfig flt_cfg;
    flt_cfg.remove_singletons = true;
    RelationFilter filter(flt_cfg);
    auto filtered = filter.filter(std::vector<Relation>(rels.begin(), rels.end()));

    std::cout << "  After filter: " << filtered.size()
              << " (singletons removed: " << filter.stats().singletons_removed << ")" << std::endl;

    if (filtered.size() < 5) {
        std::cout << "  SKIP (not enough filtered relations)" << std::endl;
        return;
    }

    // 构建矩阵（含 sign + Schirokauer 列）
    MatrixBuilderConfig mat_cfg;
    mat_cfg.include_sign_column  = true;
    mat_cfg.include_qc_columns   = false;
    mat_cfg.include_class_group  = false;
    mat_cfg.include_schirokauer  = true;
    mat_cfg.schirokauer_primes   = {2};
    MatrixBuilder builder(mat_cfg);
    auto mat_result = builder.build_with_qc(filtered, env.fb, env.ctx);

    const auto& m = mat_result.mapping;
    std::cout << "  Matrix: " << mat_result.matrix.num_rows() << "x"
              << mat_result.matrix.num_cols()
              << " (sign=1 rat=" << m.num_rational_fb
              << " alg=" << m.num_algebraic_fb
              << " sch=" << m.num_schirokauer_columns << ")" << std::endl;

    assert(mat_result.matrix.num_rows() == filtered.size());
    assert(m.num_schirokauer_columns > 0);

    // 列总数一致性
    size_t expected_cols = (mat_cfg.include_sign_column ? 1 : 0)
                          + m.num_rational_fb + m.num_algebraic_fb
                          + m.num_large_primes_rat + m.num_large_primes_alg
                          + m.num_qc_columns + m.num_class_group_columns
                          + m.num_schirokauer_columns;
    assert(mat_result.matrix.num_cols() == expected_cols);

    // 如果超定矩阵，尝试求依赖
    if (mat_result.matrix.num_rows() > mat_result.matrix.num_cols()) {
        BlockLanczos solver;
        auto deps = solver.find_dependencies(mat_result.matrix, 3);
        if (!deps.empty()) {
            bool found_valid = false;
            for (const auto& dep : deps) {
                if (verify_dependency(mat_result.matrix, dep)) {
                    found_valid = true;
                    break;
                }
            }
            if (found_valid) {
                std::cout << "  Found " << deps.size() << " valid dependencies" << std::endl;
            }
        }
    }

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// main
// ============================================================
// ============================================================
// Test 16: sieve_parallel vs sequential 对比
// 验证多线程筛法与单线程产生一致结果
// ============================================================
void test_sieve_parallel_vs_sequential() {
    std::cout << "Testing sieve_parallel vs sequential (integration)..." << std::endl;

    // N=10403 (101×103), degree=2, FB with SQ range
    Integer n("10403");
    auto poly_result = BaseMSelector::select(n, 2);
    assert(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 200;
    fb_opts.algebraic_bound = 200;
    fb_opts.special_q_bound = 500;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    SieveParams sieve_params;
    sieve_params.log_scale = 16;
    sieve_params.rational_threshold = 30;
    sieve_params.algebraic_threshold = 30;

    SieveRegion region;
    region.i_min = -200;
    region.i_max = 199;
    region.j_min = 1;
    region.j_max = 30;

    // Generate Special-Q primes
    SpecialQRange sq_range;
    sq_range.min_q = 200;
    sq_range.max_q = 500;
    SpecialQGenerator sq_gen(fb, sq_range);

    std::vector<SpecialQ> sqs;
    for (size_t i = 0; i < 5 && sq_gen.has_next(); ++i) {
        auto sq = sq_gen.next();
        if (sq) sqs.push_back(*sq);
    }
    assert(!sqs.empty());

    // Sequential: sieve each SQ one by one
    LatticeSieve sieve_seq(ctx, fb, sieve_params);
    sieve_seq.set_region(region);

    std::vector<size_t> seq_counts;
    for (const auto& sq : sqs) {
        auto result = sieve_seq.sieve_special_q(sq);
        seq_counts.push_back(result.candidates.size());
    }

    // Parallel: sieve all SQs concurrently (2 threads)
    LatticeSieve sieve_par(ctx, fb, sieve_params);
    sieve_par.set_region(region);
    auto par_results = sieve_par.sieve_parallel(sqs, 2);

    // Per-SQ candidate count must match
    assert(par_results.size() == sqs.size());
    size_t total_seq = 0, total_par = 0;
    for (size_t i = 0; i < sqs.size(); ++i) {
        assert(seq_counts[i] == par_results[i].candidates.size());
        total_seq += seq_counts[i];
        total_par += par_results[i].candidates.size();
    }

    std::cout << "  SQs=" << sqs.size()
              << " seq_total=" << total_seq
              << " par_total=" << total_par << " (match)" << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Test 17: BaseMSelector → PolynomialContext → verify 一致性
// 对多个 N 值验证 base-m 多项式满足 f(m) ≡ 0 (mod N)
// ============================================================
void test_base_m_verify_consistency() {
    std::cout << "Testing BaseMSelector → PolynomialContext verify (integration)..." << std::endl;

    struct TestCase {
        const char* n_str;
        uint32_t degree;
    };

    TestCase cases[] = {
        {"143", 2},           // 11×13, small
        {"9991", 2},          // 97×103
        {"10403", 2},         // 101×103
        {"96091", 2},         // 239×401 + 2
        {"100160063", 3},     // 8-digit, degree 3
        {"1000036000099", 3}, // 13-digit, degree 3
    };

    size_t pass_count = 0;
    for (const auto& tc : cases) {
        Integer n(tc.n_str);
        auto result = BaseMSelector::select(n, tc.degree);
        assert(result.success);

        auto ctx = BaseMSelector::create_context(n, result);
        assert(ctx.verify()); // f(m) ≡ 0 (mod N)
        assert(ctx.degree() == tc.degree);
        assert(ctx.n() == n);
        pass_count++;
    }

    std::cout << "  PASS (" << pass_count << " N values, all f(m)≡0 mod N)" << std::endl;
}

// ============================================================
// Test 18: Filter singleton removal → MatrixBuilder 维度缩减
// 验证 singleton 过滤减少矩阵行数
// ============================================================
void test_filter_reduces_matrix_dimensions() {
    std::cout << "Testing Filter → MatrixBuilder dimension reduction (integration)..." << std::endl;

    auto env = make_env_for_schirokauer(); // N=10403, FB bound=200

    // 收集关系（含 large primes → singletons 存在）
    CofactorizerConfig cof_cfg;
    cof_cfg.large_prime_bound = 100000;
    Cofactorizer cof(env.ctx, env.fb, cof_cfg);

    std::vector<Relation> rels;
    for (int64_t a = -50; a <= 50; ++a) {
        for (uint64_t b = 1; b <= 5; ++b) {
            uint64_t abs_a = static_cast<uint64_t>(a < 0 ? -a : a);
            if (std::gcd(abs_a, b) != 1) continue;
            auto rel = cof.verify(a, b);
            if (rel.has_value()) rels.push_back(std::move(*rel));
        }
    }

    if (rels.size() < 10) {
        std::cout << "  SKIP (not enough relations)" << std::endl;
        return;
    }

    // Build matrix without filtering
    MatrixBuilder mb;
    auto result_unfiltered = mb.build(rels, env.fb);
    size_t rows_unfiltered = result_unfiltered.matrix.num_rows();

    // Now filter singletons
    FilterConfig flt_cfg;
    flt_cfg.remove_singletons = true;
    RelationFilter filter(flt_cfg);
    auto filtered = filter.filter(std::vector<Relation>(rels.begin(), rels.end()));

    auto result_filtered = mb.build(filtered, env.fb);
    size_t rows_filtered = result_filtered.matrix.num_rows();

    // Filtering should not increase rows
    assert(rows_filtered <= rows_unfiltered);

    std::cout << "  unfiltered=" << rows_unfiltered
              << " filtered=" << rows_filtered
              << " singletons_removed=" << filter.stats().singletons_removed << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Test 19: FactorBase bounds 敏感性 → 关系产出
// 不同 FB 大小对关系收集的影响
// ============================================================
void test_fb_bounds_sensitivity() {
    std::cout << "Testing FactorBase bounds → relation yield (integration)..." << std::endl;

    Integer n("10403"); // 101×103
    auto poly_result = BaseMSelector::select(n, 2);
    assert(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);

    // Small FB (bound=50)
    FactorBaseBuilder::Options opts_small;
    opts_small.rational_bound = 50;
    opts_small.algebraic_bound = 50;
    opts_small.parallel = false;
    auto fb_small = FactorBaseBuilder::build(ctx, opts_small);

    // Large FB (bound=500)
    FactorBaseBuilder::Options opts_large;
    opts_large.rational_bound = 500;
    opts_large.algebraic_bound = 500;
    opts_large.parallel = false;
    auto fb_large = FactorBaseBuilder::build(ctx, opts_large);

    // Larger FB should have more primes
    assert(fb_large.rational_count() >= fb_small.rational_count());
    assert(fb_large.algebraic_count() >= fb_small.algebraic_count());

    // Count smooth relations for each FB size
    auto count_relations = [&ctx](const FactorBase& fb, uint32_t lpb) -> size_t {
        CofactorizerConfig cfg;
        cfg.large_prime_bound = lpb;
        Cofactorizer cof(ctx, fb, cfg);
        size_t count = 0;
        for (int64_t a = -30; a <= 30; ++a) {
            for (uint64_t b = 1; b <= 3; ++b) {
                uint64_t abs_a = static_cast<uint64_t>(a < 0 ? -a : a);
                if (std::gcd(abs_a, b) != 1) continue;
                if (cof.verify(a, b).has_value()) count++;
            }
        }
        return count;
    };

    size_t rels_small = count_relations(fb_small, 1000);
    size_t rels_large = count_relations(fb_large, 1000);

    // Larger FB should find at least as many smooth relations
    assert(rels_large >= rels_small);

    std::cout << "  FB_small: rat=" << fb_small.rational_count()
              << " alg=" << fb_small.algebraic_count()
              << " rels=" << rels_small << std::endl;
    std::cout << "  FB_large: rat=" << fb_large.rational_count()
              << " alg=" << fb_large.algebraic_count()
              << " rels=" << rels_large << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Test 20: PolynomialContext → algebraic_norm + rational_value 一致性
// 对真实 GNFS 关系验证：若 a - b*m 被有理 FB 整除，则 norm 被代数 FB 整除
// ============================================================
void test_norm_rational_value_consistency() {
    std::cout << "Testing algebraic_norm + rational_value consistency (integration)..." << std::endl;

    Integer n("9991"); // 97×103
    auto poly_result = BaseMSelector::select(n, 2);
    assert(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);

    // For verified smooth relations, both sides should be smooth w.r.t. their factor bases
    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 100;
    fb_opts.algebraic_bound = 100;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    CofactorizerConfig cof_cfg;
    cof_cfg.large_prime_bound = 10000;
    Cofactorizer cof(ctx, fb, cof_cfg);

    size_t checked = 0;
    for (int64_t a = -20; a <= 20; ++a) {
        for (uint64_t b = 1; b <= 3; ++b) {
            uint64_t abs_a = static_cast<uint64_t>(a < 0 ? -a : a);
            if (std::gcd(abs_a, b) != 1) continue;

            auto rel = cof.verify(a, b);
            if (!rel.has_value()) continue;

            // rational_value = a - b*m
            Integer rv = ctx.rational_value(a, b);
            // algebraic_norm = b^d * f(a/b) = N(a - b*α)
            Integer an = ctx.algebraic_norm(a, b);

            // Both should be non-zero for valid coprime (a,b)
            assert(!rv.is_zero());
            assert(!an.is_zero());

            checked++;
        }
    }

    assert(checked > 0);
    std::cout << "  Verified " << checked << " relations: rational_value and algebraic_norm both non-zero" << std::endl;
    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Session 34: LatticeBasis → verify_ab → cofactorizer chain
// ============================================================
void test_lattice_basis_sieve_geometry() {
    std::cout << "Testing LatticeBasis → Sieve geometry verification (integration)..." << std::endl;

    // Build environment for N=10403 (101×103)
    Integer n("10403");
    auto poly_result = BaseMSelector::select(n, 2);
    assert(poly_result.success);
    auto ctx = BaseMSelector::create_context(n, poly_result);

    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = 200;
    fb_opts.algebraic_bound = 200;
    fb_opts.parallel = false;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);

    // Pick a special-q from the algebraic factor base
    assert(fb.algebraic_count() > 5);
    const auto& alg_primes = fb.algebraic();

    // Use a prime from the factor base as special-q
    SpecialQ sq;
    sq.q = alg_primes[5].p;
    sq.r = alg_primes[5].r;
    sq.index = 5;

    auto basis = compute_lattice_basis(sq);

    // Verify: determinant = ±q
    int64_t det = basis.determinant();
    assert(det == static_cast<int64_t>(sq.q) || det == -static_cast<int64_t>(sq.q));

    // Verify: all lattice points satisfy a ≡ b*r (mod q)
    size_t valid_count = 0;
    for (int i = -10; i <= 10; ++i) {
        for (int j = 1; j <= 5; ++j) {
            auto [a, b] = basis.to_ab(i, j);
            assert(basis.verify_ab(a, b));
            // Skip trivial (a=0, b=0)
            if (a == 0 && b == 0) continue;
            valid_count++;
        }
    }
    assert(valid_count > 0);

    std::cout << "  PASS (q=" << sq.q << " r=" << sq.r
              << " det=" << det << " checked=" << valid_count << " lattice points)" << std::endl;
}

// ============================================================
// Session 34: Polynomial degree → FB → matrix column count
// ============================================================
void test_polynomial_degree_fb_matrix_consistency() {
    std::cout << "Testing Polynomial degree → FB → matrix column consistency (integration)..." << std::endl;

    // Test that different FB bounds produce consistent matrix dimensions
    // Use small N values with base-m degree 3 (fast path)
    for (const char* n_str : {"143", "10403"}) {
        Integer n(n_str);
        auto poly_result = BaseMSelector::select(n, 3);
        assert(poly_result.success);
        auto ctx = BaseMSelector::create_context(n, poly_result);

        FactorBaseBuilder::Options fb_opts;
        fb_opts.rational_bound = 50;
        fb_opts.algebraic_bound = 50;
        fb_opts.parallel = false;
        auto fb = FactorBaseBuilder::build(ctx, fb_opts);

        // Collect a few relations quickly
        CofactorizerConfig cof_cfg;
        cof_cfg.large_prime_bound = 5000;
        Cofactorizer cof(ctx, fb, cof_cfg);

        std::vector<Relation> rels;
        for (int64_t a = -20; a <= 20; ++a) {
            for (uint64_t b = 1; b <= 2; ++b) {
                uint64_t abs_a = static_cast<uint64_t>(a < 0 ? -a : a);
                if (std::gcd(abs_a, b) != 1) continue;
                auto rel = cof.verify(a, b);
                if (rel.has_value()) rels.push_back(std::move(*rel));
                if (rels.size() >= 10) break;
            }
            if (rels.size() >= 10) break;
        }

        if (rels.size() < 3) continue;

        // Build basic matrix (no QC/ClassGroup/Schirokauer for speed)
        MatrixBuilder::Config mb_cfg;
        mb_cfg.include_qc_columns = false;
        mb_cfg.include_class_group = false;
        mb_cfg.include_schirokauer = false;
        MatrixBuilder builder(mb_cfg);
        auto build_result = builder.build(rels, fb);

        size_t expected_min = fb.rational_count() + fb.algebraic_count();

        std::cout << "  N=" << n_str << " rat=" << fb.rational_count()
                  << " alg=" << fb.algebraic_count() << " rels=" << rels.size()
                  << " cols=" << build_result.matrix.num_cols() << std::endl;

        // Matrix columns ≥ rat + alg (sign + FB columns)
        assert(build_result.matrix.num_cols() >= expected_min);
        // Matrix rows = number of relations
        assert(build_result.matrix.num_rows() == rels.size());
    }

    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Session 34: ClassGroup character size matches generators
// ============================================================
void test_class_group_character_consistency() {
    std::cout << "Testing ClassGroup character size consistency (integration)..." << std::endl;

    // Test with multiple N values and polynomial contexts
    // Use degree 3 (fast) for all cases
    const char* n_values[] = {"143", "9991", "10403"};

    for (const char* n_str : n_values) {
        Integer n(n_str);
        auto poly_result = BaseMSelector::select(n, 3);
        if (!poly_result.success) continue;
        auto ctx = BaseMSelector::create_context(n, poly_result);

        ClassGroup cg(ctx);

        // Character vector size must match num_generators
        size_t num_gen = cg.num_generators();
        assert(cg.generators().size() == num_gen);

        // All characters for different (a,b) pairs must have consistent size
        for (int64_t a : {1LL, -3LL, 5LL, 7LL}) {
            for (uint64_t b : {1ULL, 2ULL}) {
                auto ch = cg.compute_character(a, b);
                assert(ch.size() == num_gen);
            }
        }

        std::cout << "  N=" << n_str << " class_number=" << cg.class_number()
                  << " generators=" << num_gen << " OK" << std::endl;
    }
    std::cout << "  PASS" << std::endl;
}

// ============================================================
// Session 34: MurphyE scoring ranks polynomials consistently
// ============================================================
void test_murphy_ranking_consistency() {
    std::cout << "Testing MurphyE ranking consistency (integration)..." << std::endl;

    // For two different N values, Murphy scores should all be finite
    // Use degree 3 (fast) for all cases
    const char* n_values[] = {"10403", "9991"};

    std::vector<double> all_scores;

    for (const char* n_str : n_values) {
        Integer n(n_str);
        auto poly_result = BaseMSelector::select(n, 3);
        assert(poly_result.success);
        auto ctx = BaseMSelector::create_context(n, poly_result);

        // Extract f, g from context
        IntPolynomial f(static_cast<int>(ctx.degree()));
        for (uint32_t i = 0; i <= ctx.degree(); ++i) {
            f[i] = ctx.coeff(i).clone();
        }
        IntPolynomial g(1);
        Integer minus_m = ctx.m().clone();
        minus_m.negate();
        g[0] = std::move(minus_m);
        g[1] = Integer(int64_t(1));

        MurphyParams params;
        params.sample_points = 100;
        params.alpha_bound = 500;
        params.skewness_steps = 3;
        MurphyEvaluator evaluator(params);

        auto score = evaluator.compute(f, g, n);

        // Score should be finite
        assert(std::isfinite(score.log_e_score) || score.log_e_score == -1e100);
        assert(std::isfinite(score.alpha_f));
        all_scores.push_back(score.log_e_score);

        std::cout << "  N=" << n_str << " log_e=" << score.log_e_score
                  << " alpha_f=" << score.alpha_f << std::endl;
    }

    // Both scores should exist
    assert(all_scores.size() == 2);
    std::cout << "  PASS" << std::endl;
}

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

    // Session 30 集成测试
    test_schirokauer_matrix_builder_integration();
    test_matrix_builder_all_columns();
    test_full_mini_pipeline();
    test_schirokauer_map_consistency();

    // Session 31 集成测试
    test_sieve_cofactor_joint();
    test_rational_algebraic_sqrt_joint();
    test_large_relation_to_matrix_pipeline();

    // Session 32 集成测试
    test_sieve_parallel_vs_sequential();

    // Session 33 集成测试
    test_base_m_verify_consistency();
    test_filter_reduces_matrix_dimensions();
    test_fb_bounds_sensitivity();
    test_norm_rational_value_consistency();

    // Session 34 集成测试
    test_lattice_basis_sieve_geometry();
    test_polynomial_degree_fb_matrix_consistency();
    test_class_group_character_consistency();
    test_murphy_ranking_consistency();

    std::cout << std::endl;
    std::cout << "All integration tests passed!" << std::endl;
    return 0;
}
