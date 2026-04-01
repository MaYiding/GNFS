#pragma once

#include "sparse_matrix.hpp"
#include "schirokauer.hpp"
#include "../core/relation.hpp"
#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
#include "../factor_base/factor_base.hpp"
#include "../sqrt/class_group.hpp"
#include "../sqrt/modular_poly.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <memory>
#include <iostream>

namespace gnfs {
namespace linalg {

using core::Integer;
using core::Relation;
using core::PrimePower;
using core::PolynomialContext;
using factor_base::FactorBase;

/// 代数侧素理想键 (p, r)——区分同一素数上方的不同素理想
struct PrimeIdealKey {
    uint64_t p;  // 素数
    uint64_t r;  // 根 mod p

    bool operator==(const PrimeIdealKey& other) const noexcept {
        return p == other.p && r == other.r;
    }
};

/// PrimeIdealKey 的哈希函数
struct PrimeIdealKeyHash {
    size_t operator()(const PrimeIdealKey& k) const noexcept {
        size_t h = 14695981039346656037ULL;
        h ^= std::hash<uint64_t>{}(k.p);
        h *= 1099511628211ULL;
        h ^= std::hash<uint64_t>{}(k.r);
        h *= 1099511628211ULL;
        return h;
    }
};

/// 矩阵列映射
/// 管理素数到列索引的映射
struct ColumnMapping {
    size_t num_rational_fb = 0;        // 有理因子基列数
    size_t num_algebraic_fb = 0;       // 代数因子基列数
    size_t num_large_primes_rat = 0;   // 有理大素数列数
    size_t num_large_primes_alg = 0;   // 代数大素数列数
    size_t num_qc_columns = 0;         // 二次特征列数
    size_t num_class_group_columns = 0; // 类群特征列数
    size_t num_schirokauer_columns = 0; // Schirokauer map 列数
    size_t sign_column = 0;            // 符号列（如果有）
    bool has_sign_column = false;      // 是否有符号列

    // 有理大素数 -> 列索引（有理侧无根，按 p 即可）
    std::unordered_map<uint64_t, uint32_t> rat_lp_to_col;

    // 代数大素数 -> 列索引（按 (p, r) 键，区分不同素理想）
    std::unordered_map<PrimeIdealKey, uint32_t, PrimeIdealKeyHash> alg_lp_to_col;

    // 二次特征素数列表
    std::vector<uint32_t> qc_primes;

    // Schirokauer map 素数列表
    std::vector<uint32_t> schirokauer_primes;

    /// 总列数
    [[nodiscard]] size_t total_columns() const noexcept {
        return num_rational_fb + num_algebraic_fb +
               num_large_primes_rat + num_large_primes_alg +
               num_qc_columns + num_class_group_columns +
               num_schirokauer_columns +
               (has_sign_column ? 1 : 0);
    }

    /// 获取有理因子基列的起始索引
    [[nodiscard]] size_t rat_fb_start() const noexcept {
        return has_sign_column ? 1 : 0;
    }

    /// 获取代数因子基列的起始索引
    [[nodiscard]] size_t alg_fb_start() const noexcept {
        return rat_fb_start() + num_rational_fb;
    }

    /// 获取有理大素数列的起始索引
    [[nodiscard]] size_t rat_lp_start() const noexcept {
        return alg_fb_start() + num_algebraic_fb;
    }

    /// 获取代数大素数列的起始索引
    [[nodiscard]] size_t alg_lp_start() const noexcept {
        return rat_lp_start() + num_large_primes_rat;
    }

    /// 获取二次特征列的起始索引
    [[nodiscard]] size_t qc_start() const noexcept {
        return alg_lp_start() + num_large_primes_alg;
    }

    /// 获取类群特征列的起始索引
    [[nodiscard]] size_t class_group_start() const noexcept {
        return qc_start() + num_qc_columns;
    }

    /// 获取 Schirokauer map 列的起始索引
    [[nodiscard]] size_t schirokauer_start() const noexcept {
        return class_group_start() + num_class_group_columns;
    }
};

/// 矩阵构建结果
struct MatrixBuildResult {
    SparseMatrix matrix;         // 构建的矩阵
    ColumnMapping mapping;       // 列映射信息
    std::vector<size_t> row_to_relation;  // 行索引 -> 原始关系索引的映射
};

/// 矩阵构建配置
struct MatrixBuilderConfig {
    bool include_sign_column = true;       // 是否包含符号列
    bool include_qc_columns = true;        // 是否包含二次特征列
    bool include_class_group = true;       // 是否包含类群特征列
    bool include_schirokauer = true;       // 是否包含 Schirokauer map 列
    size_t num_qc_primes = 10;             // 二次特征素数数量
    uint32_t qc_prime_start = 1000;        // 二次特征素数搜索起点
    std::vector<uint32_t> schirokauer_primes = {2};  // GF(2) 矩阵只能用 ℓ=2
    bool verbose = false;                   // 详细输出
};

/// MatrixBuilder - 从关系构建指数矩阵
class MatrixBuilder {
public:
    using Config = MatrixBuilderConfig;

    explicit MatrixBuilder(const Config& config = Config{})
        : config_(config) {}

    /// 从关系和因子基构建矩阵
    /// @param relations 关系列表
    /// @param fb 因子基
    /// @return 构建结果
    [[nodiscard]] MatrixBuildResult build(
            const std::vector<Relation>& relations,
            const FactorBase& fb) const {

        MatrixBuildResult result;

        // 第一步：收集所有大素数
        auto lp_info = collect_large_primes(relations);

        // 第二步：设置列映射
        setup_column_mapping(result.mapping, fb, lp_info);

        // 第三步：构建矩阵
        result.matrix = SparseMatrix(relations.size(), result.mapping.total_columns());
        result.row_to_relation.resize(relations.size());

        for (size_t i = 0; i < relations.size(); ++i) {
            build_row(result.matrix.row(i), relations[i], fb, result.mapping);
            result.row_to_relation[i] = i;
        }

        return result;
    }

    /// 从关系、因子基和多项式上下文构建矩阵（包含二次特征、类群特征和Schirokauer maps）
    /// @param relations 关系列表
    /// @param fb 因子基
    /// @param ctx 多项式上下文（用于计算代数范数）
    /// @return 构建结果
    [[nodiscard]] MatrixBuildResult build_with_qc(
            const std::vector<Relation>& relations,
            const FactorBase& fb,
            const PolynomialContext& ctx) const {

        MatrixBuildResult result;

        // 第一步：收集所有大素数
        auto lp_info = collect_large_primes(relations);

        // 第二步：选择二次特征素数
        // If Schirokauer will be unavailable (f reducible mod 2), use extra QC primes
        uint32_t effective_qc_count = config_.num_qc_primes;
        bool can_use_schirokauer = config_.include_schirokauer &&
                                   ctx.degree() <= FastPoly::MAX_DEGREE;
        if (can_use_schirokauer) {
            // Full irreducibility check mod 2 (not just "no roots")
            uint32_t d_check = ctx.degree();
            std::vector<uint64_t> f_mod2(d_check + 1);
            for (uint32_t i = 0; i <= d_check; ++i) {
                Integer c = ctx.coeff(i).clone();
                c %= Integer(uint64_t(2));
                if (c.is_negative()) c += Integer(uint64_t(2));
                f_mod2[i] = c.to_uint64();
            }
            bool f_irred_mod2 = sqrt::ModularPoly::is_irreducible(f_mod2, 2);
            if (!f_irred_mod2) {
                // Compensate: more QC primes to replace missing Schirokauer
                effective_qc_count = std::max(effective_qc_count, static_cast<uint32_t>(config_.num_qc_primes + d_check * 8));
            }
        } else if (config_.include_schirokauer) {
            // Degree exceeds FastPoly::MAX_DEGREE — cannot use Schirokauer maps
            // Compensate with extra QC primes
            effective_qc_count = std::max(effective_qc_count,
                static_cast<uint32_t>(config_.num_qc_primes + ctx.degree() * 8));
        }
        std::vector<uint32_t> qc_primes;
        if (config_.include_qc_columns) {
            qc_primes = select_qc_primes(ctx, effective_qc_count);
        }

        // 第三步：计算类群（如果启用）
        std::unique_ptr<sqrt::ClassGroup> class_group;
        if (config_.include_class_group) {
            sqrt::ClassGroupConfig cg_config;
            cg_config.verbose = config_.verbose;
            class_group = std::make_unique<sqrt::ClassGroup>(ctx, cg_config);

            if (config_.verbose) {
                std::cerr << "[ClassGroup] Discriminant: " << class_group->discriminant().to_string() << "\n"
                          << "[ClassGroup] Minkowski bound: " << class_group->minkowski_bound() << "\n"
                          << "[ClassGroup] Class number estimate: " << class_group->class_number() << "\n"
                          << "[ClassGroup] Generators: " << class_group->num_generators() << "\n";
            }
        }

        // 第四步：创建 Schirokauer map 计算器（如果启用且 degree <= MAX_DEGREE）
        // AUTO-SELECT: find primes where f is irreducible (inert)
        std::unique_ptr<SchirokaurMap> schirokauer;
        if (can_use_schirokauer) {
            std::vector<uint32_t> sm_primes;
            if (!config_.schirokauer_primes.empty()) {
                // Validate user-specified primes: only keep inert ones
                uint32_t d = ctx.degree();
                for (uint32_t ell : config_.schirokauer_primes) {
                    // Full Rabin irreducibility test (not just "no roots")
                    std::vector<uint64_t> f_mod_ell(d + 1);
                    for (uint32_t i = 0; i <= d; ++i) {
                        Integer c = ctx.coeff(i).clone();
                        c %= Integer(static_cast<uint64_t>(ell));
                        if (c.is_negative()) c += Integer(static_cast<uint64_t>(ell));
                        f_mod_ell[i] = c.to_uint64();
                    }
                    if (sqrt::ModularPoly::is_irreducible(f_mod_ell, ell)) {
                        sm_primes.push_back(ell);
                    }
                }
            }
            // Always use ℓ=2 for GF(2) matrix Schirokauer.
            // If f is reducible mod 2, the split Schirokauer map handles it.
            if (sm_primes.empty()) {
                sm_primes.push_back(2);
            }
            SchirokaurConfig sm_config;
            sm_config.primes = sm_primes;
            sm_config.verbose = config_.verbose;
            schirokauer = std::make_unique<SchirokaurMap>(ctx, sm_config);

            if (config_.verbose) {
                std::cerr << "[Schirokauer] Primes: ";
                for (auto p : sm_primes) std::cerr << p << " ";
                std::cerr << "\n[Schirokauer] Columns per prime: " << ctx.degree() << "\n"
                          << "[Schirokauer] Total columns: " << schirokauer->num_columns() << "\n";
            }
        }

        // 第五步：设置列映射（包含类群特征和 Schirokauer）
        setup_column_mapping_with_qc(result.mapping, fb, lp_info, qc_primes);

        // 添加类群特征列
        if (class_group) {
            result.mapping.num_class_group_columns = class_group->num_generators();
        }

        // 添加 Schirokauer map 列
        if (schirokauer) {
            result.mapping.num_schirokauer_columns = schirokauer->num_columns();
            result.mapping.schirokauer_primes = schirokauer->primes();  // 存储实际使用的素数，非 config 原始值
        }

        // 第六步：构建矩阵
        if (config_.verbose) {
            std::cerr << "[Matrix] Starting matrix build: " << relations.size()
                      << " x " << result.mapping.total_columns() << "\n";
        }
        result.matrix = SparseMatrix(relations.size(), result.mapping.total_columns());
        result.row_to_relation.resize(relations.size());

        for (size_t i = 0; i < relations.size(); ++i) {
            if (config_.verbose && i % 1000 == 0) {
                std::cerr << "[Matrix] Building row " << i << "/" << relations.size() << "\n";
            }
            build_row_with_qc(result.matrix.row(i), relations[i], fb, ctx, result.mapping);

            // ClassGroup characters: χ is a homomorphism, so for merged relation
            // χ(∏ x_i) = Σ χ(x_i) mod 2  →  XOR individual character bits
            if (class_group && class_group->num_generators() > 0) {
                const auto& rel = relations[i];
                auto cg_chars = class_group->compute_character(rel.a, rel.b);

                for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                    auto extra_chars = class_group->compute_character(ea, static_cast<uint64_t>(eb));
                    for (size_t j = 0; j < cg_chars.size(); ++j) {
                        cg_chars[j] = cg_chars[j] ^ extra_chars[j];
                    }
                }

                for (size_t j = 0; j < cg_chars.size(); ++j) {
                    if (cg_chars[j]) {
                        result.matrix.row(i).set(
                            static_cast<uint32_t>(result.mapping.class_group_start() + j));
                    }
                }
            }

            // Schirokauer maps: λ is a homomorphism, so for merged relation
            // λ(∏ x_i) = Σ λ(x_i) mod ℓ.  Sum all pairs, then take mod 2 for GF(2).
            if (schirokauer) {
                const auto& rel = relations[i];
                auto sm_values = schirokauer->compute_flat(rel.a, rel.b);

                for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                    auto extra_sm = schirokauer->compute_flat(ea, static_cast<uint64_t>(eb));
                    for (size_t j = 0; j < sm_values.size(); ++j) {
                        sm_values[j] += extra_sm[j];
                    }
                }

                size_t sm_start = result.mapping.schirokauer_start();
                for (size_t j = 0; j < sm_values.size(); ++j) {
                    if (sm_values[j] % 2 == 1) {
                        result.matrix.row(i).set(static_cast<uint32_t>(sm_start + j));
                    }
                }
            }

            result.row_to_relation[i] = i;
        }

        return result;
    }

    /// 验证矩阵（检查每行是否正确表示关系的指数向量）
    [[nodiscard]] static bool verify_matrix(
            const SparseMatrix& matrix,
            const std::vector<Relation>& relations,
            const FactorBase& fb,
            const ColumnMapping& mapping) {

        if (matrix.num_rows() != relations.size()) {
            return false;
        }

        // 验证每一行
        for (size_t i = 0; i < relations.size(); ++i) {
            // 重新构建行并比较
            SparseRow expected;
            MatrixBuilder builder;
            builder.build_row(expected, relations[i], fb, mapping);

            const auto& actual = matrix.row(i);
            if (expected.indices() != actual.indices()) {
                return false;
            }
        }

        return true;
    }

private:
    Config config_;

    /// 大素数收集结果
    struct LargePrimeInfo {
        std::unordered_set<uint64_t> rat_primes;  // 有理侧大素数集合
        std::unordered_set<PrimeIdealKey, PrimeIdealKeyHash> alg_primes;  // 代数侧素理想 (p,r) 集合
    };

    /// 收集所有大素数（仅包含有效贡献的 LP）
    /// 合并关系中，共享 LP 的指数为偶数（在 GF(2) 矩阵中贡献为 0），
    /// 不应为其创建列。只收集在至少一个关系中有奇数指数的 LP。
    [[nodiscard]] LargePrimeInfo collect_large_primes(
            const std::vector<Relation>& relations) const {

        LargePrimeInfo info;

        for (const auto& rel : relations) {
            // 有理侧：按素数累计指数，只收集奇数指数的
            std::unordered_map<uint64_t, uint8_t> rat_exp;
            for (const auto& lp : rel.rational_large_prime) {
                rat_exp[lp.p] += lp.e;
            }
            for (const auto& [p, exp] : rat_exp) {
                if (exp % 2 == 1) {
                    info.rat_primes.insert(p);
                }
            }

            // 代数侧：按 (p,r) 素理想累计指数，只收集奇数指数的
            std::unordered_map<PrimeIdealKey, uint8_t, PrimeIdealKeyHash> alg_exp;
            for (const auto& lp : rel.algebraic_large_prime) {
                alg_exp[{lp.p, lp.r}] += lp.e;
            }
            for (const auto& [key, exp] : alg_exp) {
                if (exp % 2 == 1) {
                    info.alg_primes.insert(key);
                }
            }
        }

        return info;
    }

    /// 选择二次特征素数
    /// Select primes where f is IRREDUCIBLE mod p, so that F_p[x]/f(x) is a field.
    /// This allows us to compute proper quadratic characters in the field extension.
    [[nodiscard]] std::vector<uint32_t> select_qc_primes(
            const PolynomialContext& ctx,
            size_t num_primes) const {

        std::vector<uint32_t> qc_primes;
        qc_primes.reserve(num_primes);

        const Integer& n = ctx.n();
        uint32_t d = ctx.degree();

        uint32_t p = config_.qc_prime_start;

        while (qc_primes.size() < num_primes) {
            p = next_prime(p);

            // Skip primes that divide N (use Integer arithmetic — safe for N > 2^64)
            {
                Integer n_mod = n.clone();
                n_mod %= Integer(static_cast<uint64_t>(p));
                if (n_mod.is_zero()) continue;
            }

            // Compute f coefficients mod p using Integer arithmetic (safe for large coefficients)
            std::vector<uint64_t> f_mod(d + 1);
            for (uint32_t i = 0; i <= d; ++i) {
                Integer c = ctx.coeff(i).clone();
                c %= Integer(static_cast<uint64_t>(p));
                if (c.is_negative()) c += Integer(static_cast<uint64_t>(p));
                f_mod[i] = c.to_uint64();
            }

            // Skip if leading coefficient vanishes mod p (degree drops)
            if (f_mod[d] == 0) continue;

            // Use Rabin irreducibility test — correct for all degrees.
            // "No roots" is only equivalent for degree ≤ 3; for degree ≥ 4,
            // a product of irreducible quadratics has no roots but is reducible.
            if (sqrt::ModularPoly::is_irreducible(f_mod, p)) {
                qc_primes.push_back(p);
            }
        }

        return qc_primes;
    }

    /// 检查多项式是否在 F_p 上有重根
    [[nodiscard]] bool has_multiple_root(const PolynomialContext& ctx, uint32_t p) const {
        // 简化检查：计算 f(x) 和 f'(x) 的 gcd
        // 如果 gcd 非平凡，则有重根

        uint32_t d = ctx.degree();
        if (d == 0) return false;

        // 计算 f mod p
        std::vector<uint64_t> f(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            Integer c = ctx.coeff(i).clone();
            c %= Integer(static_cast<uint64_t>(p));
            if (c.is_negative()) c += Integer(static_cast<uint64_t>(p));
            f[i] = c.to_uint64();
        }

        // 计算 f' mod p
        std::vector<uint64_t> fp(d);
        for (uint32_t i = 1; i <= d; ++i) {
            fp[i - 1] = (f[i] * i) % p;
        }

        // 简化：如果 f' 全为零，有重根
        bool all_zero = true;
        for (uint64_t c : fp) {
            if (c != 0) {
                all_zero = false;
                break;
            }
        }

        return all_zero;
    }

    /// 设置列映射
    void setup_column_mapping(ColumnMapping& mapping,
                              const FactorBase& fb,
                              const LargePrimeInfo& lp_info) const {

        // 符号列
        mapping.has_sign_column = config_.include_sign_column;
        mapping.sign_column = 0;

        // 因子基列
        mapping.num_rational_fb = fb.rational_count();
        mapping.num_algebraic_fb = fb.sieve_algebraic_count();

        // 大素数列
        mapping.num_large_primes_rat = lp_info.rat_primes.size();
        mapping.num_large_primes_alg = lp_info.alg_primes.size();

        // 无二次特征列
        mapping.num_qc_columns = 0;

        // 为有理大素数分配列索引
        uint32_t col = static_cast<uint32_t>(mapping.rat_lp_start());
        for (uint64_t p : lp_info.rat_primes) {
            mapping.rat_lp_to_col[p] = col++;
        }

        // 为代数大素数（素理想）分配列索引——按 (p, r) 键
        col = static_cast<uint32_t>(mapping.alg_lp_start());
        for (const auto& key : lp_info.alg_primes) {
            mapping.alg_lp_to_col[key] = col++;
        }
    }

    /// 设置带二次特征的列映射
    void setup_column_mapping_with_qc(ColumnMapping& mapping,
                                      const FactorBase& fb,
                                      const LargePrimeInfo& lp_info,
                                      const std::vector<uint32_t>& qc_primes) const {

        setup_column_mapping(mapping, fb, lp_info);

        // 添加二次特征列
        mapping.num_qc_columns = qc_primes.size();
        mapping.qc_primes = qc_primes;
    }

    /// 构建单行
    void build_row(SparseRow& row,
                   const Relation& rel,
                   const FactorBase& fb,
                   const ColumnMapping& mapping) const {

        (void)fb;  // 未使用，但保持接口一致

        // 清空行
        row.clear_all();

        // 符号列：此处不设置——符号应基于 (a - b*m) 的正负而非 a 的正负。
        // build_with_qc() 会用 PolynomialContext 正确计算并设置符号列。

        // 有理因子基：计算每个素数的指数模 2
        {
            std::unordered_map<uint32_t, uint8_t> exponents;
            for (size_t j = 0; j < rel.rational_factors.size(); ++j) {
                exponents[rel.rational_factors[j]]++;
            }

            for (const auto& [idx, exp] : exponents) {
                if (exp % 2 == 1 && idx < mapping.num_rational_fb) {
                    row.set(static_cast<uint32_t>(mapping.rat_fb_start() + idx));
                }
            }
        }

        // 代数因子基：计算每个素理想的指数模 2
        {
            std::unordered_map<uint32_t, uint8_t> exponents;
            for (size_t j = 0; j < rel.algebraic_factors.size(); ++j) {
                exponents[rel.algebraic_factors[j]]++;
            }

            for (const auto& [idx, exp] : exponents) {
                if (exp % 2 == 1 && idx < mapping.num_algebraic_fb) {
                    row.set(static_cast<uint32_t>(mapping.alg_fb_start() + idx));
                }
            }
        }

        // 有理大素数
        {
            std::unordered_map<uint64_t, uint8_t> exponents;
            for (size_t j = 0; j < rel.rational_large_prime.size(); ++j) {
                exponents[rel.rational_large_prime[j].p] += rel.rational_large_prime[j].e;
            }

            for (const auto& [p, exp] : exponents) {
                if (exp % 2 == 1) {
                    auto it = mapping.rat_lp_to_col.find(p);
                    if (it != mapping.rat_lp_to_col.end()) {
                        row.set(it->second);
                    }
                }
            }
        }

        // 代数大素数——按 (p, r) 素理想键累积指数
        {
            std::unordered_map<PrimeIdealKey, uint8_t, PrimeIdealKeyHash> exponents;
            for (size_t j = 0; j < rel.algebraic_large_prime.size(); ++j) {
                PrimeIdealKey key{rel.algebraic_large_prime[j].p,
                                  rel.algebraic_large_prime[j].r};
                exponents[key] += rel.algebraic_large_prime[j].e;
            }

            for (const auto& [key, exp] : exponents) {
                if (exp % 2 == 1) {
                    auto it = mapping.alg_lp_to_col.find(key);
                    if (it != mapping.alg_lp_to_col.end()) {
                        row.set(it->second);
                    }
                }
            }
        }
    }

    /// 构建带二次特征的单行
    void build_row_with_qc(SparseRow& row,
                           const Relation& rel,
                           const FactorBase& fb,
                           const PolynomialContext& ctx,
                           const ColumnMapping& mapping) const {

        // 首先构建基础行
        build_row(row, rel, fb, mapping);

        // Sign column: product (a_0 - b_0*m)·...·(a_k - b_k*m) is negative
        // iff an odd number of factors are negative → XOR of individual sign bits
        if (mapping.has_sign_column) {
            auto is_neg = [&](int64_t ai, int64_t bi) {
                Integer v = Integer(ai);
                Integer bm = ctx.m().clone();
                bm *= Integer(static_cast<int64_t>(bi));
                v -= bm;
                return v.is_negative();
            };

            bool sign_bit = is_neg(rel.a, rel.b);
            for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                sign_bit ^= is_neg(ea, eb);
            }

            bool currently_set = row.test(static_cast<uint32_t>(mapping.sign_column));
            if (sign_bit != currently_set) {
                if (sign_bit) {
                    row.set(static_cast<uint32_t>(mapping.sign_column));
                } else {
                    row.clear(static_cast<uint32_t>(mapping.sign_column));
                }
            }
        }

        // QC columns: Legendre symbol is multiplicative, so for merged relation
        // (∏ norm_i / q) = ∏ (norm_i / q).  In GF(2): XOR individual Legendre bits.
        Integer alg_norm = ctx.algebraic_norm(rel.a, rel.b);

        // Collect all norms for merged relations
        std::vector<Integer> extra_norms;
        if (rel.is_merged()) {
            extra_norms.reserve(rel.extra_ab_pairs.size());
            for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                extra_norms.push_back(ctx.algebraic_norm(ea, static_cast<uint64_t>(eb)));
            }
        }

        for (size_t i = 0; i < mapping.qc_primes.size(); ++i) {
            uint32_t q = mapping.qc_primes[i];

            bool qc_bit = (legendre_symbol(alg_norm, q) == -1);
            for (const auto& norm : extra_norms) {
                qc_bit ^= (legendre_symbol(norm, q) == -1);
            }

            if (qc_bit) {
                row.set(static_cast<uint32_t>(mapping.qc_start() + i));
            }
        }
    }

    /// Fast modular exponentiation using native uint64_t
    [[nodiscard]] static uint64_t powmod_u64(uint64_t base, uint64_t exp, uint64_t mod) {
        uint64_t result = 1;
        base %= mod;
        while (exp > 0) {
            if (exp & 1) {
                result = static_cast<uint64_t>(
                    (static_cast<__uint128_t>(result) * base) % mod);
            }
            base = static_cast<uint64_t>(
                (static_cast<__uint128_t>(base) * base) % mod);
            exp >>= 1;
        }
        return result;
    }

    /// 计算 Legendre 符号 (a / p) - optimized for small primes
    [[nodiscard]] static int legendre_symbol(const Integer& a, uint32_t p) {
        // Get a mod p as uint64_t
        Integer a_mod = a.clone();
        a_mod %= Integer(static_cast<uint64_t>(p));

        if (a_mod.is_zero()) {
            return 0;
        }

        if (a_mod.is_negative()) {
            a_mod += Integer(static_cast<uint64_t>(p));
        }

        uint64_t a_val = a_mod.to_uint64();

        // (a / p) = a^((p-1)/2) mod p using fast native arithmetic
        uint64_t r = powmod_u64(a_val, (p - 1) / 2, p);

        if (r == 0) return 0;
        if (r == 1) return 1;
        if (r == p - 1) return -1;

        return 0;  // Should not reach
    }

    /// 找下一个素数
    [[nodiscard]] static uint32_t next_prime(uint32_t n) {
        n++;
        if (n <= 2) return 2;
        if (n % 2 == 0) n++;

        while (!is_prime(n)) {
            n += 2;
        }
        return n;
    }

    /// 简单素性测试
    [[nodiscard]] static bool is_prime(uint32_t n) {
        if (n < 2) return false;
        if (n == 2) return true;
        if (n % 2 == 0) return false;

        uint32_t sqrt_n = static_cast<uint32_t>(std::sqrt(n));
        for (uint32_t i = 3; i <= sqrt_n; i += 2) {
            if (n % i == 0) return false;
        }
        return true;
    }
};

/// 计算矩阵统计信息
struct MatrixStats {
    size_t num_rows = 0;
    size_t num_cols = 0;
    size_t total_weight = 0;
    double avg_row_weight = 0.0;
    double density = 0.0;  // 非零比例
    size_t excess = 0;     // 行数 - 列数 (期望 > 0)

    [[nodiscard]] bool has_excess() const noexcept {
        return num_rows > num_cols;
    }
};

[[nodiscard]] inline MatrixStats compute_matrix_stats(const SparseMatrix& matrix) {
    MatrixStats stats;
    stats.num_rows = matrix.num_rows();
    stats.num_cols = matrix.num_cols();
    stats.total_weight = matrix.total_weight();
    stats.avg_row_weight = matrix.average_row_weight();

    if (stats.num_rows > 0 && stats.num_cols > 0) {
        stats.density = static_cast<double>(stats.total_weight) /
                        (stats.num_rows * stats.num_cols);
    }

    if (stats.num_rows > stats.num_cols) {
        stats.excess = stats.num_rows - stats.num_cols;
    }

    return stats;
}

} // namespace linalg
} // namespace gnfs
