#pragma once

#include "sparse_matrix.hpp"
#include "schirokauer.hpp"
#include "relation_source.hpp"
#include "../core/relation.hpp"
#include "../core/integer.hpp"
#include "../core/polynomial_context.hpp"
#include "../factor_base/factor_base.hpp"
#include "../relation/large_prime_key.hpp"
#include "../sqrt/class_group.hpp"
#include "../sqrt/modular_poly.hpp"
#include "../polynomial/int_polynomial.hpp"
#include "../util/primes.hpp"
#include "../util/thread_pool.hpp"

#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <memory>
#include <iostream>
#include <stdexcept>

namespace gnfs::linalg {

using core::Integer;
using core::Relation;
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

    // 二次特征 per-root (prime, root) 对列表——每对对应一个 QC 列
    std::vector<std::pair<uint32_t, uint32_t>> qc_prime_roots;

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
    bool include_class_group = false;      // 类群特征列（默认禁用：大多数 N class number=1，无需额外列）
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
        size_t effective_qc_count = config_.num_qc_primes;
        bool can_use_schirokauer = config_.include_schirokauer &&
                                   ctx.degree() <= FastPoly::MAX_DEGREE;
        if (can_use_schirokauer) {
            // Full irreducibility check mod 2 (not just "no roots")
            uint32_t d_check = ctx.degree();
            std::vector<uint64_t> f_mod2(d_check + 1);
            // v22: c 复用 + Integer(2) 提到 loop 外
            Integer c;
            const Integer two(uint64_t(2));
            for (uint32_t i = 0; i <= d_check; ++i) {
                c = ctx.coeff(i);
                c %= two;
                if (c.is_negative()) c += two;
                f_mod2[i] = c.to_uint64();
            }
            bool f_irred_mod2 = sqrt::ModularPoly::is_irreducible(f_mod2, 2);
            if (!f_irred_mod2) {
                // Compensate: more QC primes to replace missing Schirokauer
                effective_qc_count = std::max(
                    effective_qc_count,
                    config_.num_qc_primes + static_cast<size_t>(d_check) * 8);
            }
        } else if (config_.include_schirokauer) {
            // Degree exceeds FastPoly::MAX_DEGREE — cannot use Schirokauer maps
            // Compensate with extra QC primes
            effective_qc_count = std::max(
                effective_qc_count,
                config_.num_qc_primes + static_cast<size_t>(ctx.degree()) * 8);
        }
        std::vector<std::pair<uint32_t, uint32_t>> qc_prime_roots;
        if (config_.include_qc_columns) {
            // QC primes must be above algebraic FB bound to avoid Legendre=0 corruption
            uint32_t alg_bound = fb.params().algebraic_bound;
            qc_prime_roots = select_qc_prime_roots(ctx, effective_qc_count, alg_bound + 1);
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
                // v22: c 复用 across primes × coeffs
                Integer c;
                for (uint32_t ell : config_.schirokauer_primes) {
                    // Full Rabin irreducibility test (not just "no roots")
                    std::vector<uint64_t> f_mod_ell(d + 1);
                    const Integer ell_int(static_cast<uint64_t>(ell));
                    for (uint32_t i = 0; i <= d; ++i) {
                        c = ctx.coeff(i);
                        c %= ell_int;
                        if (c.is_negative()) c += ell_int;
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
        setup_column_mapping_with_qc(result.mapping, fb, lp_info, qc_prime_roots);

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

        // 主循环并行化:每行 i 独立写入 SparseMatrix::row(i) — vector<SparseRow>
        // 元素互不冲突;build_row_with_qc / class_group->compute_character /
        // schirokauer->compute_flat 全部 const,无内部状态修改。
        // SchirokauerMap::compute_flat 1M+ relations 是热点。
        gnfs::util::ThreadPool pool(0);
        pool.parallel_for_index(0, relations.size(), [&](size_t i) {
            build_row_with_qc(result.matrix.row(i), relations[i], fb, ctx, result.mapping);

            // ClassGroup characters: χ is a homomorphism, so for merged relation
            // χ(∏ x_i) = Σ χ(x_i) mod 2  →  XOR individual character bits
            if (class_group && class_group->num_generators() > 0) {
                const auto& rel = relations[i];
                auto cg_chars = class_group->compute_character(rel.a, rel.b);

                for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                    auto extra_chars = class_group->compute_character(ea, eb);
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
                    auto extra_sm = schirokauer->compute_flat(ea, eb);
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
        });

        return result;
    }

    /// Streaming variant of build_with_qc: consumes any RelationSource
    /// (vector adapter or OOC mmap reader) one relation at a time. Avoids
    /// materializing the full vector<Relation> in RAM.
    ///
    /// Functionally identical to build_with_qc(vector, fb, ctx) when fed via
    /// VectorRelationSource(vec): produces bit-for-bit identical
    /// MatrixBuildResult. Verified by tests/test_sge_streaming.cpp.
    ///
    /// Memory cost: O(unique LPs + matrix nnz). The vector<Relation> never
    /// exists. Each parallel thread fetches its relation on demand and lets
    /// it drop after row build — RAM stays flat over the build.
    ///
    /// I/O cost: ~2 full scans of the source.
    ///   Pass 1: collect_large_primes_streaming
    ///   Pass 2: build_row_with_qc (parallel)
    /// For OOCRelationSource these are mmap reads — page cache handles repeat
    /// access. For VectorRelationSource the cost is negligible (vector copy).
    template <RelationSource Source>
    [[nodiscard]] MatrixBuildResult build_with_qc_streaming(
            const Source& source,
            const FactorBase& fb,
            const PolynomialContext& ctx) const {

        MatrixBuildResult result;

        // 第一步：流式收集大素数
        auto lp_info = collect_large_primes_streaming(source);

        // 第二步：选择二次特征素数 (与 vector 版完全相同)
        size_t effective_qc_count = config_.num_qc_primes;
        bool can_use_schirokauer = config_.include_schirokauer &&
                                   ctx.degree() <= FastPoly::MAX_DEGREE;
        if (can_use_schirokauer) {
            uint32_t d_check = ctx.degree();
            std::vector<uint64_t> f_mod2(d_check + 1);
            Integer c;
            const Integer two(uint64_t(2));
            for (uint32_t i = 0; i <= d_check; ++i) {
                c = ctx.coeff(i);
                c %= two;
                if (c.is_negative()) c += two;
                f_mod2[i] = c.to_uint64();
            }
            bool f_irred_mod2 = sqrt::ModularPoly::is_irreducible(f_mod2, 2);
            if (!f_irred_mod2) {
                effective_qc_count = std::max(
                    effective_qc_count,
                    config_.num_qc_primes + static_cast<size_t>(d_check) * 8);
            }
        } else if (config_.include_schirokauer) {
            effective_qc_count = std::max(
                effective_qc_count,
                config_.num_qc_primes + static_cast<size_t>(ctx.degree()) * 8);
        }
        std::vector<std::pair<uint32_t, uint32_t>> qc_prime_roots;
        if (config_.include_qc_columns) {
            uint32_t alg_bound = fb.params().algebraic_bound;
            qc_prime_roots = select_qc_prime_roots(ctx, effective_qc_count, alg_bound + 1);
        }

        // 第三步：类群
        std::unique_ptr<sqrt::ClassGroup> class_group;
        if (config_.include_class_group) {
            sqrt::ClassGroupConfig cg_config;
            cg_config.verbose = config_.verbose;
            class_group = std::make_unique<sqrt::ClassGroup>(ctx, cg_config);
            if (config_.verbose) {
                std::cerr << "[ClassGroup] Discriminant: "
                          << class_group->discriminant().to_string() << "\n"
                          << "[ClassGroup] Generators: "
                          << class_group->num_generators() << "\n";
            }
        }

        // 第四步：Schirokauer
        std::unique_ptr<SchirokaurMap> schirokauer;
        if (can_use_schirokauer) {
            std::vector<uint32_t> sm_primes;
            if (!config_.schirokauer_primes.empty()) {
                uint32_t d = ctx.degree();
                Integer c;
                for (uint32_t ell : config_.schirokauer_primes) {
                    std::vector<uint64_t> f_mod_ell(d + 1);
                    const Integer ell_int(static_cast<uint64_t>(ell));
                    for (uint32_t i = 0; i <= d; ++i) {
                        c = ctx.coeff(i);
                        c %= ell_int;
                        if (c.is_negative()) c += ell_int;
                        f_mod_ell[i] = c.to_uint64();
                    }
                    if (sqrt::ModularPoly::is_irreducible(f_mod_ell, ell)) {
                        sm_primes.push_back(ell);
                    }
                }
            }
            if (sm_primes.empty()) {
                sm_primes.push_back(2);
            }
            SchirokaurConfig sm_config;
            sm_config.primes = sm_primes;
            sm_config.verbose = config_.verbose;
            schirokauer = std::make_unique<SchirokaurMap>(ctx, sm_config);
            if (config_.verbose) {
                std::cerr << "[Schirokauer] Total columns: "
                          << schirokauer->num_columns() << "\n";
            }
        }

        // 第五步：列映射
        setup_column_mapping_with_qc(result.mapping, fb, lp_info, qc_prime_roots);

        if (class_group) {
            result.mapping.num_class_group_columns = class_group->num_generators();
        }
        if (schirokauer) {
            result.mapping.num_schirokauer_columns = schirokauer->num_columns();
            result.mapping.schirokauer_primes = schirokauer->primes();
        }

        // 第六步：流式矩阵构建
        const std::size_t n = source.count();
        if (config_.verbose) {
            std::cerr << "[Matrix-streaming] Starting matrix build: " << n
                      << " x " << result.mapping.total_columns() << "\n";
        }
        result.matrix = SparseMatrix(n, result.mapping.total_columns());
        result.row_to_relation.resize(n);

        // Parallel row build: each thread reads its own relation from the
        // source via source.read(i). RelationSource concept requires read(i)
        // to be thread-safe for distinct i — both OOCRelationReader (mmap +
        // local deserialize) and VectorRelationSource (const access) satisfy
        // this. The relation goes out of scope at the end of each lambda
        // invocation — no accumulation in any shared structure.
        gnfs::util::ThreadPool pool(0);
        pool.parallel_for_index(0, n, [&](std::size_t i) {
            Relation rel = source.read(i);
            build_row_with_qc(result.matrix.row(i), rel, fb, ctx, result.mapping);

            if (class_group && class_group->num_generators() > 0) {
                auto cg_chars = class_group->compute_character(rel.a, rel.b);
                for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                    auto extra_chars = class_group->compute_character(ea, eb);
                    for (std::size_t j = 0; j < cg_chars.size(); ++j) {
                        cg_chars[j] = cg_chars[j] ^ extra_chars[j];
                    }
                }
                for (std::size_t j = 0; j < cg_chars.size(); ++j) {
                    if (cg_chars[j]) {
                        result.matrix.row(i).set(
                            static_cast<uint32_t>(result.mapping.class_group_start() + j));
                    }
                }
            }

            if (schirokauer) {
                auto sm_values = schirokauer->compute_flat(rel.a, rel.b);
                for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                    auto extra_sm = schirokauer->compute_flat(ea, eb);
                    for (std::size_t j = 0; j < sm_values.size(); ++j) {
                        sm_values[j] += extra_sm[j];
                    }
                }
                std::size_t sm_start = result.mapping.schirokauer_start();
                for (std::size_t j = 0; j < sm_values.size(); ++j) {
                    if (sm_values[j] % 2 == 1) {
                        result.matrix.row(i).set(static_cast<uint32_t>(sm_start + j));
                    }
                }
            }

            result.row_to_relation[i] = i;
            // rel destroyed here — RAM not accumulating.
        });

        return result;
    }

private:
    Config config_;

    /// 大素数收集结果
    struct LargePrimeInfo {
        std::unordered_set<uint64_t> rat_primes;  // 有理侧大素数集合
        std::unordered_set<PrimeIdealKey, PrimeIdealKeyHash> alg_primes;  // 代数侧素理想 (p,r) 集合
    };

    /// Accumulate a single relation's LP contributions into LargePrimeInfo.
    /// Extracted from collect_large_primes so both vector and streaming paths
    /// share identical parity semantics. Column assignment sorts the collected
    /// keys later, so hash iteration order cannot affect the layout.
    static void accumulate_lp_one(LargePrimeInfo& info, const Relation& rel) {
        gnfs::relation::for_each_odd_large_prime_key(
            rel, [&](const gnfs::relation::LargePrimeKey& key) {
                if (key.is_algebraic) {
                    info.alg_primes.insert(PrimeIdealKey{key.prime, key.root});
                } else {
                    info.rat_primes.insert(key.prime);
                }
            });
    }

    /// 收集所有大素数（仅包含有效贡献的 LP）
    /// 合并关系中，共享 LP 的指数为偶数（在 GF(2) 矩阵中贡献为 0），
    /// 不应为其创建列。只收集在至少一个关系中有奇数指数的 LP。
    [[nodiscard]] LargePrimeInfo collect_large_primes(
            const std::vector<Relation>& relations) const {

        LargePrimeInfo info;
        // Reserve for typical LP density (60d-scale: ~M cols expected for M rels).
        info.rat_primes.reserve(relations.size());
        info.alg_primes.reserve(relations.size());

        for (const auto& rel : relations) {
            accumulate_lp_one(info, rel);
        }

        return info;
    }

    /// Streaming variant: collect LP info from a RelationSource.
    /// SGE-OOC: mirrors collect_large_primes(vector) byte-for-byte but reads
    /// from the source one relation at a time, so RAM usage stays O(unique LPs)
    /// instead of O(relations) needed by the vector path.
    template <RelationSource Source>
    [[nodiscard]] LargePrimeInfo collect_large_primes_streaming(
            const Source& source) const {

        const std::size_t n = source.count();
        LargePrimeInfo info;
        info.rat_primes.reserve(n);
        info.alg_primes.reserve(n);

        for (std::size_t i = 0; i < n; ++i) {
            Relation rel = source.read(i);
            accumulate_lp_one(info, rel);
            // rel goes out of scope here — no accumulation of relations in RAM.
        }

        return info;
    }

    /// 选择二次特征素数——Per-root QC (Briggs 2004)
    /// Select SPLIT primes where f(x) has roots mod p, returning (prime, root) pairs.
    /// Each (p, r) pair becomes one QC column: Legendre symbol ((a - b*r) / p).
    /// This gives d independent bits per fully-split prime (vs 1 bit for norm-based QC).
    [[nodiscard]] std::vector<std::pair<uint32_t, uint32_t>> select_qc_prime_roots(
            const PolynomialContext& ctx,
            size_t num_columns,
            uint32_t min_prime = 0) const {

        std::vector<std::pair<uint32_t, uint32_t>> qc_pairs;
        qc_pairs.reserve(num_columns);

        const Integer& n = ctx.n();
        uint32_t d = ctx.degree();

        // QC primes MUST be above the factor base algebraic bound.
        // If primes are inside FB, many relations have (a-b*r) ≡ 0 (mod q),
        // giving Legendre symbol = 0 (undefined), corrupting the GF(2) constraints.
        uint32_t p = std::max(config_.qc_prime_start, min_prime);

        // Build IntPolynomial from ctx for efficient Cantor-Zassenhaus root finding
        std::vector<Integer> f_coeffs;
        f_coeffs.reserve(d + 1);
        for (uint32_t i = 0; i <= d; ++i) {
            f_coeffs.emplace_back(ctx.coeff(i));  // Integer copy ctor
        }
        polynomial::IntPolynomial f_poly(std::move(f_coeffs));

        while (qc_pairs.size() < num_columns) {
            p = next_prime(p);

            // Skip primes that divide N or leading coeff — direct GMP (zero alloc)
            if (mpz_divisible_ui_p(n.get_mpz(), p)) continue;
            if (mpz_divisible_ui_p(ctx.coeff(d).get_mpz(), p)) continue;

            // Find roots using Cantor-Zassenhaus (O(d²·log p), fast for large p)
            auto roots = f_poly.roots_mod_p(p);

            // Skip inert primes (no roots) — useless for per-root QC
            if (roots.empty()) continue;

            // Skip primes with repeated roots (gcd(f, f') non-trivial mod p)
            if (roots.size() < d && has_multiple_root(ctx, p)) continue;

            // Add each root as a separate QC column
            for (uint32_t r : roots) {
                qc_pairs.emplace_back(p, r);
                if (qc_pairs.size() >= num_columns) break;
            }
        }

        return qc_pairs;
    }

    /// 检查多项式是否在 F_p 上有重根
    /// Uses gcd(f, f') — has repeated root iff deg(gcd) >= 1
    [[nodiscard]] bool has_multiple_root(const PolynomialContext& ctx, uint32_t p) const {
        uint32_t d = ctx.degree();
        if (d == 0) return false;

        uint64_t p64 = p;

        // 计算 f mod p
        // v22: c 复用 + Integer(p) extract
        std::vector<uint64_t> f(d + 1);
        Integer c;
        const Integer p_int(p64);
        for (uint32_t i = 0; i <= d; ++i) {
            c = ctx.coeff(i);
            c %= p_int;
            if (c.is_negative()) c += p_int;
            f[i] = c.to_uint64();
        }

        // 计算 f' mod p
        std::vector<uint64_t> fp(d);
        for (uint32_t i = 1; i <= d; ++i) {
            fp[i - 1] = gnfs::util::mul_mod_u64(f[i], i, p64);
        }

        // gcd(f, f') via Euclidean algorithm over F_p[x]
        // Copy f and fp into working buffers
        auto a = f;    // degree d
        auto b = fp;   // degree d-1

        // Find actual degrees
        auto deg = [](const std::vector<uint64_t>& poly) -> int {
            for (int i = static_cast<int>(poly.size()) - 1; i >= 0; --i) {
                if (poly[static_cast<size_t>(i)] != 0) return i;
            }
            return -1;  // zero polynomial
        };

        // Modular inverse via extended Euclidean
        auto mod_inv = [p64](uint64_t x) -> uint64_t {
            if (x == 0) return 0;
            int64_t a0 = static_cast<int64_t>(p64), a1 = static_cast<int64_t>(x);
            int64_t s0 = 0, s1 = 1;
            while (a1 != 0) {
                int64_t q = a0 / a1;
                int64_t tmp = a0 - q * a1; a0 = a1; a1 = tmp;
                tmp = s0 - q * s1; s0 = s1; s1 = tmp;
            }
            return static_cast<uint64_t>((s0 % static_cast<int64_t>(p64) + static_cast<int64_t>(p64)) % static_cast<int64_t>(p64));
        };

        while (true) {
            int db = deg(b);
            if (db < 0) break;  // b is zero → gcd = a
            int da = deg(a);
            if (da < db) { std::swap(a, b); continue; }

            // a = a - (lead_a / lead_b) * x^(da-db) * b
            uint64_t inv_lb = mod_inv(b[static_cast<size_t>(db)]);
            uint64_t scale = gnfs::util::mul_mod_u64(a[static_cast<size_t>(da)], inv_lb, p64);
            int shift = da - db;
            for (int i = 0; i <= db; ++i) {
                const size_t b_idx = static_cast<size_t>(i);
                const size_t a_idx = static_cast<size_t>(i + shift);
                uint64_t sub = gnfs::util::mul_mod_u64(scale, b[b_idx], p64);
                a[a_idx] = (a[a_idx] + p64 - sub) % p64;
            }
            // Trim leading zeros
            while (a.size() > 1 && a.back() == 0) a.pop_back();
            std::swap(a, b);
        }

        // gcd = a; has repeated root iff deg(gcd) >= 1
        return deg(a) >= 1;
    }

    /// Assign deterministic LP columns after factor-base/sign offsets are set.
    /// unordered_set iteration order is not portable across standard-library
    /// implementations, so both vector and streaming builds sort the structural
    /// keys before assigning column numbers.
    static void setup_large_prime_mapping(
            ColumnMapping& mapping,
            const LargePrimeInfo& lp_info) {
        mapping.num_large_primes_rat = lp_info.rat_primes.size();
        mapping.num_large_primes_alg = lp_info.alg_primes.size();

        std::vector<uint64_t> rational_primes(
            lp_info.rat_primes.begin(), lp_info.rat_primes.end());
        std::sort(rational_primes.begin(), rational_primes.end());

        std::vector<PrimeIdealKey> algebraic_primes(
            lp_info.alg_primes.begin(), lp_info.alg_primes.end());
        std::sort(algebraic_primes.begin(), algebraic_primes.end(),
                  [](const PrimeIdealKey& lhs, const PrimeIdealKey& rhs) {
                      if (lhs.p != rhs.p) return lhs.p < rhs.p;
                      return lhs.r < rhs.r;
                  });

        mapping.rat_lp_to_col.clear();
        mapping.rat_lp_to_col.reserve(rational_primes.size());
        uint32_t col = static_cast<uint32_t>(mapping.rat_lp_start());
        for (uint64_t p : rational_primes) {
            mapping.rat_lp_to_col.emplace(p, col++);
        }

        mapping.alg_lp_to_col.clear();
        mapping.alg_lp_to_col.reserve(algebraic_primes.size());
        col = static_cast<uint32_t>(mapping.alg_lp_start());
        for (const auto& key : algebraic_primes) {
            mapping.alg_lp_to_col.emplace(key, col++);
        }
    }

    /// 设置列映射（无 QC 版本 — build_row 不设 sign 列，强制禁用）
    void setup_column_mapping(ColumnMapping& mapping,
                              const FactorBase& fb,
                              const LargePrimeInfo& lp_info) const {

        // build_row() cannot compute sign (needs PolynomialContext),
        // so force sign column off to avoid all-zero sign column
        mapping.has_sign_column = false;
        mapping.sign_column = 0;

        // 因子基列
        mapping.num_rational_fb = fb.rational_count();
        mapping.num_algebraic_fb = fb.sieve_algebraic_count();

        // 无二次特征列
        mapping.num_qc_columns = 0;

        setup_large_prime_mapping(mapping, lp_info);
    }

    /// 设置带二次特征的列映射（per-root QC）
    /// 此版本可选启用符号列（build_with_qc 有 PolynomialContext，可以正确计算符号）
    void setup_column_mapping_with_qc(ColumnMapping& mapping,
                                      const FactorBase& fb,
                                      const LargePrimeInfo& lp_info,
                                      const std::vector<std::pair<uint32_t, uint32_t>>& qc_prime_roots) const {

        // Enable sign column if requested — build_with_qc can compute it
        if (config_.include_sign_column) {
            mapping.has_sign_column = true;
            mapping.sign_column = 0;  // column 0 = sign
        } else {
            mapping.has_sign_column = false;
            mapping.sign_column = 0;
        }

        // 因子基列
        mapping.num_rational_fb = fb.rational_count();
        mapping.num_algebraic_fb = fb.sieve_algebraic_count();

        // 二次特征列
        mapping.num_qc_columns = qc_prime_roots.size();
        mapping.qc_prime_roots = qc_prime_roots;

        setup_large_prime_mapping(mapping, lp_info);
    }

    /// 构建单行
    void build_row(SparseRow& row,
                   const Relation& rel,
                   const FactorBase& fb,
                   const ColumnMapping& mapping) const {

        (void)fb;  // 未使用，但保持接口一致

        // 清空行
        row.clear_all();
        // Reserve upper bound: rat_fb + alg_fb + LP + QC + SM + sign columns
        // typical 50d row weight = 30-50 nonzero. Use 64 generic upper bound.
        row.reserve(64);

        // 符号列：此处不设置——符号应基于 (a - b*m) 的正负而非 a 的正负。
        // build_with_qc() 会用 PolynomialContext 正确计算并设置符号列。

        // 有理因子基：计算每个素数的指数模 2
        // FB factor counts: 50d ~10-15 per rel, 60d ~10-30. Use stack array
        // for size<=32 (typical), fallback to unordered_map for larger.
        {
            const auto& rfacs = rel.rational_factors;
            if (rfacs.size() <= 32) {
                uint32_t rkeys[32]; uint8_t rexps[32]; size_t ru = 0;
                for (uint32_t f : rfacs) {
                    size_t j = 0;
                    for (; j < ru; ++j) if (rkeys[j] == f) break;
                    if (j == ru) { rkeys[ru] = f; rexps[ru] = 1; ++ru; }
                    else ++rexps[j];
                }
                for (size_t i = 0; i < ru; ++i) {
                    if ((rexps[i] & 1u) && rkeys[i] < mapping.num_rational_fb) {
                        row.append_unchecked(static_cast<uint32_t>(mapping.rat_fb_start() + rkeys[i]));
                    }
                }
            } else {
                std::unordered_map<uint32_t, uint8_t> exponents;
                exponents.reserve(rfacs.size());
                for (uint32_t f : rfacs) exponents[f]++;
                for (const auto& [idx, exp] : exponents) {
                    if ((exp & 1u) && idx < mapping.num_rational_fb) {
                        row.append_unchecked(static_cast<uint32_t>(mapping.rat_fb_start() + idx));
                    }
                }
            }
        }

        // 代数因子基：计算每个素理想的指数模 2
        {
            const auto& afacs = rel.algebraic_factors;
            if (afacs.size() <= 32) {
                uint32_t akeys[32]; uint8_t aexps[32]; size_t au = 0;
                for (uint32_t f : afacs) {
                    size_t j = 0;
                    for (; j < au; ++j) if (akeys[j] == f) break;
                    if (j == au) { akeys[au] = f; aexps[au] = 1; ++au; }
                    else ++aexps[j];
                }
                for (size_t i = 0; i < au; ++i) {
                    if ((aexps[i] & 1u) && akeys[i] < mapping.num_algebraic_fb) {
                        row.append_unchecked(static_cast<uint32_t>(mapping.alg_fb_start() + akeys[i]));
                    }
                }
            } else {
                std::unordered_map<uint32_t, uint8_t> exponents;
                exponents.reserve(afacs.size());
                for (uint32_t f : afacs) exponents[f]++;
                for (const auto& [idx, exp] : exponents) {
                    if ((exp & 1u) && idx < mapping.num_algebraic_fb) {
                        row.append_unchecked(static_cast<uint32_t>(mapping.alg_fb_start() + idx));
                    }
                }
            }
        }

        // Large-prime columns use the same canonical per-relation parity view
        // as filtering and adaptive relation metrics.
        gnfs::relation::for_each_odd_large_prime_key(
            rel, [&](const gnfs::relation::LargePrimeKey& key) {
                if (key.is_algebraic) {
                    auto it = mapping.alg_lp_to_col.find(
                        PrimeIdealKey{key.prime, key.root});
                    if (it != mapping.alg_lp_to_col.end()) {
                        row.append_unchecked(it->second);
                    }
                } else {
                    auto it = mapping.rat_lp_to_col.find(key.prime);
                    if (it != mapping.rat_lp_to_col.end()) {
                        row.append_unchecked(it->second);
                    }
                }
            });

        // build_with_qc 后续会 test() sign 列;ensure_sorted 让 test 走 O(log n) 二分。
        row.ensure_sorted();
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
            // v = ai - m*bi via mpz_submul_ui (fused FMS, drops bm temp)
            Integer v;
            auto is_neg = [&](int64_t ai, uint64_t bi) {
                v = ai;  // mpz_set_si direct
                mpz_submul_ui(v.get_mpz(), ctx.m().get_mpz(),
                              static_cast<unsigned long>(bi));
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

        // Per-root QC columns (Briggs 2004): for each (q, r) pair where r is a root
        // of f(x) mod q, compute Legendre symbol ((a - b*r) / q).
        // For merged relations, XOR individual bits (character is multiplicative).
        for (size_t i = 0; i < mapping.qc_prime_roots.size(); ++i) {
            auto [q, r] = mapping.qc_prime_roots[i];
            int64_t q_s = static_cast<int64_t>(q);

            // Compute (a - b*r) mod q for primary pair
            auto compute_legendre_bit = [&](int64_t a, uint64_t b) -> bool {
                int64_t a_mod = ((a % q_s) + q_s) % q_s;
                uint64_t b_mod = b % q;
                uint64_t br = (b_mod * r) % q;
                int64_t val = (a_mod - static_cast<int64_t>(br) + q_s) % q_s;
                if (val == 0) return false;  // (0/q) = 0, contributes 0 to GF(2)
                uint64_t leg = powmod_u64(static_cast<uint64_t>(val), (q - 1) / 2, q);
                return (leg == q - 1);  // -1 → bit=1 (non-residue)
            };

            bool qc_bit = compute_legendre_bit(rel.a, rel.b);
            for (const auto& [ea, eb] : rel.extra_ab_pairs) {
                qc_bit ^= compute_legendre_bit(ea, eb);
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
                result = gnfs::util::mul_mod_u64(result, base, mod);
            }
            base = gnfs::util::mul_mod_u64(base, base, mod);
            exp >>= 1;
        }
        return result;
    }

    /// 计算 Legendre 符号 (a / p) - optimized for small primes
    [[nodiscard]] static int legendre_symbol(const Integer& a, uint32_t p) {
        // mpz_fdiv_ui returns floor-div remainder ∈ [0, p-1] regardless of sign (zero alloc)
        uint64_t a_val = static_cast<uint64_t>(mpz_fdiv_ui(a.get_mpz(), p));
        if (a_val == 0) {
            return 0;
        }

        // (a / p) = a^((p-1)/2) mod p using fast native arithmetic
        uint64_t r = powmod_u64(a_val, (p - 1) / 2, p);

        if (r == 0) return 0;
        if (r == 1) return 1;
        if (r == p - 1) return -1;

        // a^((p-1)/2) mod p ∈ {0, 1, p-1} for prime p (Euler's criterion).
        // 走到这里说明 p 不是素数或 powmod 有 bug — Release 下不能静默返回 0
        // (会让 QR/NQR 判定错误，下游矩阵列污染)。
        assert(false && "legendre_symbol: unexpected residue");
        throw std::logic_error("legendre_symbol: a^((p-1)/2) ∉ {0, 1, p-1}; p is not prime or powmod is broken");
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

    /// 简单素性测试。整数 sqrt 避免 std::sqrt(double) 在接近 2^32 边界的精度问题。
    [[nodiscard]] static bool is_prime(uint32_t n) {
        if (n < 2) return false;
        if (n == 2) return true;
        if (n % 2 == 0) return false;

        // 整数 sqrt:用 floor(sqrt(double)) + 修正,防止 n 接近 UINT32_MAX 时
        // double 浮点丢精度导致 sqrt_n 少 1 漏检。
        uint32_t sqrt_n = static_cast<uint32_t>(std::sqrt(static_cast<double>(n)));
        while (static_cast<uint64_t>(sqrt_n + 1) * (sqrt_n + 1) <= n) ++sqrt_n;
        while (static_cast<uint64_t>(sqrt_n) * sqrt_n > n) --sqrt_n;
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
                        (static_cast<double>(stats.num_rows) *
                         static_cast<double>(stats.num_cols));
    }

    if (stats.num_rows > stats.num_cols) {
        stats.excess = stats.num_rows - stats.num_cols;
    }

    return stats;
}

/// BACKLOG #1 diagnostic (F.1): extended matrix shape stats for Phase 5.
/// Captures row/col weight distribution that MatrixStats hides — empty/singleton
/// counts reveal sieve gap (empty cols) or SGE-eliminable garbage (singleton
/// cols/rows). Walked once per Phase 5 matrix build (~10M nnz scan on 50d).
struct MatrixDiagnostics {
    // Row distribution
    size_t empty_rows = 0;        // weight=0 (degenerate, would be dropped by SGE)
    size_t singleton_rows = 0;    // weight=1 (forces a column to 0, often-useless)
    size_t min_row_weight = 0;
    size_t max_row_weight = 0;

    // Col distribution
    size_t empty_cols = 0;        // weight=0 (column never appears; sieve gap signal)
    size_t singleton_cols = 0;    // weight=1 (SGE eliminable, free Gauss pivot)
    size_t low_weight_cols = 0;   // weight ∈ [2, 4] (eligible for SGE w1/w2)
    size_t max_col_weight = 0;
    double avg_col_weight = 0.0;  // total_weight / num_cols (includes empty cols)
};

[[nodiscard]] inline MatrixDiagnostics compute_matrix_diagnostics(const SparseMatrix& matrix) {
    MatrixDiagnostics d;
    const size_t num_rows = matrix.num_rows();
    const size_t num_cols = matrix.num_cols();

    if (num_rows == 0 || num_cols == 0) {
        d.empty_cols = num_cols;
        return d;
    }

    // Pass 1: row stats + accumulate per-column tally
    std::vector<size_t> col_weight(num_cols, 0);
    bool first_row = true;
    for (const auto& row : matrix.rows()) {
        const auto& idx = row.indices();  // sorted + dedup'd
        const size_t w = idx.size();
        if (w == 0) ++d.empty_rows;
        else if (w == 1) ++d.singleton_rows;
        if (first_row) {
            d.min_row_weight = w;
            d.max_row_weight = w;
            first_row = false;
        } else {
            if (w < d.min_row_weight) d.min_row_weight = w;
            if (w > d.max_row_weight) d.max_row_weight = w;
        }
        for (uint32_t c : idx) {
            if (c < num_cols) ++col_weight[c];
        }
    }

    // Pass 2: col-weight bucketing
    size_t col_total = 0;
    for (size_t c = 0; c < num_cols; ++c) {
        const size_t w = col_weight[c];
        col_total += w;
        if (w == 0) ++d.empty_cols;
        else if (w == 1) ++d.singleton_cols;
        else if (w <= 4) ++d.low_weight_cols;
        if (w > d.max_col_weight) d.max_col_weight = w;
    }
    d.avg_col_weight = static_cast<double>(col_total) / static_cast<double>(num_cols);

    return d;
}

} // namespace gnfs::linalg
