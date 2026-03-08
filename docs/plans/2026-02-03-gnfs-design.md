# GNFS 工业级实现设计文档

**日期**: 2026-02-03
**版本**: 1.0
**状态**: 已确认，待实现

---

## 1. 项目概述

### 1.1 目标

实现一个工业级的广义数域筛法（General Number Field Sieve, GNFS），能够：

- 分解 **150位以上** 的大整数
- 真正可用于密码系统安全性测试
- 在 macOS 平台上运行，利用 Metal GPU 加速

### 1.2 技术选型

| 项目 | 选择 | 理由 |
|------|------|------|
| 语言 | C++20/23 | 现代模块化设计，concepts，coroutines |
| 大整数库 | GMP + NTL（辅助） | 业界标准，CADO-NFS/msieve 都使用 |
| GPU框架 | Metal | macOS 原生，Apple Silicon 最佳性能 |
| 并行 | 先单机多线程，后期扩展分布式 | 务实路线，快速验证 |

### 1.3 主要算法选择

| 阶段 | 算法 |
|------|------|
| 多项式选择 | Kleinjung 两阶段 + Murphy's E 评估 + GPU 加速搜索 |
| 筛法 | 格筛法 + Special-Q + 桶筛 + 2LP/3LP |
| 线性代数 | Block Lanczos（块大小 64） |
| 平方根 | Montgomery + Couveignes + CRT |

---

## 2. 整体架构

```
+------------------------------------------------------------------+
|                      GNFS Pipeline                                |
+------------------------------------------------------------------+
|  +---------------+    +---------------+    +---------------+      |
|  |  Polynomial   |--->|   Sieving     |--->|   Relation    |      |
|  |  Selection    |    |   (Lattice)   |    |  Processing   |      |
|  +---------------+    +---------------+    +---------------+      |
|         |                   |                   |                 |
|         v                   v                   v                 |
|  +---------------+    +---------------+    +---------------+      |
|  |    Square     |<---|    Linear     |<---|   Matrix      |      |
|  |     Root      |    |   Algebra     |    |   Builder     |      |
|  +---------------+    +---------------+    +---------------+      |
+------------------------------------------------------------------+
|                      Core Infrastructure                          |
|  +------------+  +------------+  +------------+  +------------+   |
|  |  GMP/NTL   |  |   Metal    |  |  Thread    |  |   I/O &    |   |
|  |  Wrapper   |  |   Compute  |  |   Pool     |  |  Checkpt   |   |
|  +------------+  +------------+  +------------+  +------------+   |
+------------------------------------------------------------------+
```

### 2.1 目录结构

```
gnfs/
├── core/           # 大整数封装、代数数论基础、核心类型
├── factor_base/    # 因子基生成与管理
├── polynomial/     # 多项式选择（Kleinjung、Base-m、Murphy's E）
├── sieve/          # 格筛法（Special-Q、桶筛、余因子分解）
├── relation/       # 关系收集、去重、过滤、循环查找
├── linalg/         # Block Lanczos、高斯消元
├── sqrt/           # Montgomery 平方根
├── metal/          # GPU 抽象层
├── util/           # 线程池、日志、检查点、配置
└── docs/           # 文档
```

### 2.2 模块依赖图

```
gnfs.core         --> (GMP/NTL)
gnfs.util         --> (标准库)
gnfs.metal        --> gnfs.core (独立，被其他模块可选依赖)
gnfs.factor_base  --> gnfs.core
gnfs.polynomial   --> gnfs.core, gnfs.factor_base
gnfs.sieve        --> gnfs.core, gnfs.factor_base, gnfs.polynomial
gnfs.relation     --> gnfs.core, gnfs.factor_base
gnfs.linalg       --> gnfs.core, gnfs.relation
gnfs.sqrt         --> gnfs.core, gnfs.polynomial, gnfs.linalg
```

---

## 3. 核心类型设计（core/）

### 3.1 Integer — 大整数封装

```cpp
class Integer {
    mpz_t value_;
public:
    // 构造/移动（Move-only，避免不必要拷贝）
    Integer() noexcept;
    explicit Integer(int64_t v) noexcept;
    explicit Integer(std::string_view s, int base = 10);
    Integer(Integer&&) noexcept;
    Integer& operator=(Integer&&) noexcept;
    ~Integer();

    // 快速路径
    bool fits_int64() const noexcept;
    int64_t to_int64() const;
    bool fits_uint64() const noexcept;
    uint64_t to_uint64() const;

    // 位操作（Block Lanczos 热点）
    size_t bit_length() const noexcept;
    bool test_bit(size_t i) const noexcept;
    void set_bit(size_t i) noexcept;
    void clear_bit(size_t i) noexcept;

    // 复合运算避免临时对象
    static void addmul(Integer& r, const Integer& a, const Integer& b);
    static void submul(Integer& r, const Integer& a, const Integer& b);

    // 底层访问
    mpz_srcptr get() const noexcept;
    mpz_ptr get() noexcept;
};
```

### 3.2 ABPair — 关系的原子单位

```cpp
struct ABPair {
    int64_t  a;
    uint64_t b;  // b > 0 always

    constexpr bool operator==(const ABPair&) const = default;
    constexpr auto operator<=>(const ABPair&) const = default;
};

struct ABPairHash {
    size_t operator()(const ABPair& ab) const noexcept {
        return std::hash<int64_t>{}(ab.a) ^
               (std::hash<uint64_t>{}(ab.b) << 1);
    }
};
```

### 3.3 PrimePower — 因子分解基本单位

```cpp
struct PrimePower {
    uint32_t p;   // 素数
    uint32_t r;   // 代数侧的根 mod p（有理侧可忽略）
    uint8_t  e;   // 指数
};
```

### 3.4 Relation — 筛法输出

```cpp
struct Relation {
    ABPair ab;
    SmallVector<uint32_t, 8> rat_factors;   // 有理因子基索引
    SmallVector<uint32_t, 8> alg_factors;   // 代数因子基索引
    SmallVector<PrimePower, 4> large_primes_rat;
    SmallVector<PrimePower, 4> large_primes_alg;

    void serialize(std::ostream& os) const;
    static Relation deserialize(std::istream& is);
};
```

### 3.5 PolynomialContext — 多项式上下文

```cpp
class PolynomialContext {
    Integer n_;                     // 待分解的数
    std::vector<Integer> f_coeffs_; // f(x) 系数
    Integer m_;                     // f(m) = 0 (mod n)
    uint32_t degree_;
    double skewness_;
public:
    uint32_t degree() const noexcept;
    const Integer& n() const noexcept;
    const Integer& m() const noexcept;
    double skewness() const noexcept;
};
```

### 3.6 AlgebraicInteger — 筛法阶段紧凑表示

```cpp
class AlgebraicInteger {
    static constexpr size_t MAX_DEGREE = 8;

    std::array<int64_t, MAX_DEGREE> coeffs_{};
    const PolynomialContext* ctx_;
public:
    explicit AlgebraicInteger(const PolynomialContext* ctx) noexcept;

    // 范数计算（筛法主要用途）
    Integer norm() const;
};
```

---

## 4. 因子基模块（factor_base/）

### 4.1 数据结构

```cpp
struct RationalPrime {
    uint32_t p;        // 素数
    uint32_t log_p;    // floor(log2(p) * scale)，筛法用定点数
};

struct AlgebraicPrime {
    uint32_t p;
    uint32_t r;        // f(r) = 0 (mod p)，UINT32_MAX 表示 projective root
    uint32_t log_p;
    uint8_t  degree;
};

struct FactorBaseParams {
    uint32_t rational_bound;
    uint32_t algebraic_bound;
    uint32_t large_prime_bound;
    uint8_t  log_scale = 10;
};
```

### 4.2 FactorBase 类

```cpp
class FactorBase {
    std::vector<RationalPrime>   rational_;
    std::vector<AlgebraicPrime>  algebraic_;
    FactorBaseParams params_;

    // 快速查找表
    std::unordered_map<uint64_t, uint32_t> alg_index_;  // key = (p << 32) | r

public:
    explicit FactorBase(const PolynomialContext& ctx, const FactorBaseParams& params);

    std::span<const RationalPrime> rational() const noexcept;
    std::span<const AlgebraicPrime> algebraic() const noexcept;
    std::optional<uint32_t> find_rational(uint32_t p) const;
    std::optional<uint32_t> find_algebraic(uint32_t p, uint32_t r) const;
    std::optional<uint32_t> find_special_q(uint32_t q, uint32_t r) const;

    size_t rational_count() const noexcept;
    size_t algebraic_count() const noexcept;

    void save(std::ostream& os) const;
    static FactorBase load(std::istream& is, const PolynomialContext& ctx);
};
```

### 4.3 FactorBaseBuilder

```cpp
class FactorBaseBuilder {
public:
    static std::vector<AlgebraicPrime> build_algebraic(
        const PolynomialContext& ctx,
        uint32_t bound,
        uint8_t log_scale);

    static std::vector<AlgebraicPrime> build_algebraic_parallel(
        const PolynomialContext& ctx,
        uint32_t bound,
        uint8_t log_scale,
        uint32_t num_threads);
};
```

---

## 5. 多项式选择模块（polynomial/）

### 5.1 参数

```cpp
struct PolySelectParams {
    uint32_t degree = 5;
    double   skewness_min = 1e4;
    double   skewness_max = 1e7;

    // Kleinjung Stage 1
    uint64_t leading_coeff_bound;
    uint32_t num_leading_coeffs;

    // Stage 2
    uint32_t root_opt_iterations = 256;
    double   root_opt_precision = 1e-6;

    // Murphy's E
    double   alpha_bound = 1e7;
    uint32_t murphy_sample_points = 2000;

    uint32_t num_threads = 0;
    bool     use_gpu = true;
};
```

### 5.2 IntPolynomial

```cpp
class IntPolynomial {
    std::vector<Integer> coeffs_;
public:
    uint32_t degree() const noexcept;
    const Integer& operator[](size_t i) const;
    Integer value_at(const Integer& x) const;
    double value_at_double(double x) const;

    // 小 p：暴力枚举；大 p：Cantor-Zassenhaus
    std::vector<uint32_t> roots_mod_p(uint32_t p) const;

    Integer discriminant() const;
    static Integer resultant(const IntPolynomial& f, const IntPolynomial& g);
};
```

### 5.3 Murphy's E 评估

```cpp
class MurphyEvaluator {
public:
    struct Score {
        double e_score;
        double alpha_f;
        double alpha_g;
        double size_score;
        double root_score;
    };

    Score compute(
        const IntPolynomial& f,
        const IntPolynomial& g,
        const Integer& n,
        double skewness) const;

    double compute_alpha(const IntPolynomial& f, double bound) const;
};
```

### 5.4 Kleinjung 选择器

```cpp
class KleinjungSelector {
public:
    struct Result {
        IntPolynomial f;
        IntPolynomial g;
        Integer m;
        double skewness;
        MurphyEvaluator::Score score;
    };

    explicit KleinjungSelector(const Integer& n, const PolySelectParams& params);

    std::vector<Result> stage1_search(
        std::function<void(const ProgressInfo&)> progress_cb = nullptr);

    Result stage2_optimize(const Result& candidate);
    Result select_best();
};

struct ProgressInfo {
    size_t current;
    size_t total;
    double elapsed_seconds;
    double best_score_so_far;
};
```

### 5.5 Base-m Fallback

```cpp
class BaseMSelector {
public:
    static KleinjungSelector::Result select(
        const Integer& n,
        uint32_t degree);
};
```

### 5.6 GPU 搜索

```cpp
class MetalPolySearch {
public:
    struct GpuHit {
        uint32_t ad_index;
        uint64_t m_hint_low64;
    };

    std::vector<GpuHit> search_batch(
        const Integer& n,
        uint32_t degree,
        std::span<const int64_t> ad_candidates,
        double skewness_hint);

    static bool is_available() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

### 5.7 格式导入导出

```cpp
class PolynomialIO {
public:
    static KleinjungSelector::Result load_cado_poly(std::istream& is);
    static void save_cado_poly(std::ostream& os, const KleinjungSelector::Result& poly);
    static KleinjungSelector::Result load_msieve_fb(std::istream& is);
    static void save_msieve_fb(std::ostream& os, const KleinjungSelector::Result& poly);
};
```

---

## 6. 筛法模块（sieve/）

### 6.1 参数

```cpp
struct SieveParams {
    int64_t  a_min, a_max;
    uint64_t b_max;

    uint32_t special_q_min;
    uint32_t special_q_max;
    bool     special_q_on_alg_side = true;

    uint8_t  sieve_threshold_rat;
    uint8_t  sieve_threshold_alg;

    uint8_t  large_prime_bits = 28;
    uint8_t  num_large_primes = 2;  // 2LP 为主，3LP 可选

    uint32_t bucket_region_bits = 16;

    uint32_t num_threads = 0;
    bool     use_gpu = true;

    std::string relation_file;
    size_t   checkpoint_interval = 100000;
};
```

### 6.2 Special-Q

```cpp
struct SpecialQ {
    uint32_t q;
    uint32_t r;
    int64_t  u0, v0;  // 格基向量
    int64_t  u1, v1;

    ABPair to_ab(int64_t i, int64_t j) const noexcept;
};

class SpecialQGenerator {
public:
    SpecialQGenerator(const FactorBase& fb, uint32_t q_min, uint32_t q_max, bool on_alg_side);

    static SpecialQ compute_lattice(uint32_t q, uint32_t r);
    std::optional<SpecialQ> next();
    void seek(uint32_t q, uint32_t r);

private:
    static void reduce_basis(int64_t& u0, int64_t& v0, int64_t& u1, int64_t& v1);
};
```

### 6.3 桶筛

```cpp
class BucketSieve {
public:
    struct BucketEntry {
        uint32_t offset;
        uint32_t fb_index;
        uint8_t  log_p;
    };

    struct Bucket {
        SmallVector<BucketEntry, 16> entries;
    };

    explicit BucketSieve(size_t region_size, size_t num_buckets);

    void fill_buckets(const FactorBase& fb, const SpecialQ& sq, int64_t i_min, int64_t i_max);
    void apply_bucket(size_t bucket_idx, std::span<uint8_t> sieve_array);
    void clear() noexcept;
};
```

### 6.4 格筛法主类

```cpp
class LatticeSieve {
public:
    struct SieveResult {
        SpecialQ sq;
        std::vector<ABPair> survivors;
        size_t relations_found;
        double elapsed_seconds;
    };

    LatticeSieve(const PolynomialContext& ctx, const FactorBase& fb, const SieveParams& params);

    SieveResult sieve_special_q(const SpecialQ& sq);

    void run(RelationCollector& collector,
             std::function<void(const SieveProgress&)> progress_cb = nullptr);

    void resume_from_checkpoint(const std::string& checkpoint_file);

    size_t compute_sieve_region_size(const SpecialQ& sq) const;

private:
    struct ThreadLocalData {
        std::vector<uint8_t> sieve_rat_;
        std::vector<uint8_t> sieve_alg_;
        std::vector<ABPair> survivors_;
    };
};

struct SieveProgress {
    uint32_t current_q;
    uint32_t total_q_range;
    size_t relations_found;
    size_t relations_needed;
    double elapsed_seconds;
    double relations_per_second;
    double eta_seconds;
};
```

### 6.5 余因子分解

```cpp
class Cofactorizer {
public:
    explicit Cofactorizer(const FactorBase& fb, uint8_t num_large_primes);

    struct CofactorResult {
        bool success;
        std::vector<PrimePower> large_primes_rat;
        std::vector<PrimePower> large_primes_alg;
    };

    CofactorResult factorize(
        const ABPair& ab,
        const Integer& cofactor_rat,
        const Integer& cofactor_alg);

private:
    // 分解策略链
    bool try_trial_division(const Integer& n, std::vector<PrimePower>& result);
    bool try_squfof(const Integer& n, std::vector<PrimePower>& result);
    bool try_ecm(const Integer& n, std::vector<PrimePower>& result);
};
```

### 6.6 GPU 筛法

```cpp
class MetalSieve {
public:
    void small_prime_sieve_gpu(
        std::span<uint8_t> sieve_array,
        const SpecialQ& sq,
        std::span<const RationalPrime> small_primes,
        int64_t i_min, int64_t i_max);

    std::vector<uint32_t> threshold_scan(
        std::span<const uint8_t> sieve_rat,
        std::span<const uint8_t> sieve_alg,
        uint8_t threshold_rat,
        uint8_t threshold_alg);

    static bool is_available() noexcept;
};
```

---

## 7. 关系处理模块（relation/）

### 7.1 关系收集器

```cpp
class RelationCollector {
public:
    struct Stats {
        size_t total;
        size_t full;
        size_t partial_1lp;
        size_t partial_2lp;
        size_t partial_3lp;
        size_t duplicates;
    };

    explicit RelationCollector(const std::string& output_file, size_t flush_interval = 10000);

    void add(Relation&& rel);
    void add_batch(std::vector<Relation>&& rels);

    Stats stats() const noexcept;

    void flush();
    void save_checkpoint(const std::string& path);
    void load_checkpoint(const std::string& path);
};
```

### 7.2 去重

```cpp
class RelationDeduplicator {
public:
    static void deduplicate(
        const std::string& input_file,
        const std::string& output_file,
        size_t memory_limit_mb = 4096);

    static void merge_sorted(
        std::span<const std::string> input_files,
        const std::string& output_file);
};
```

### 7.3 过滤

```cpp
struct FilterParams {
    uint32_t target_density = 20;
    uint32_t min_weight = 17;
    uint32_t singleton_passes = 10;
    uint32_t clique_removal_passes = 5;
    bool     verbose = true;
};

class RelationFilter {
public:
    struct FilterStats {
        size_t input_relations;
        size_t output_relations;
        size_t singletons_removed;
        size_t cliques_removed;
        size_t excess;
    };

    FilterStats filter(
        const std::string& input_file,
        const std::string& output_file,
        const FactorBase& fb);

    FilterStats filter_streaming(
        const std::string& input_file,
        const std::string& output_file,
        const FactorBase& fb,
        size_t memory_limit_mb);
};
```

### 7.4 循环查找

```cpp
class CycleFinder {
public:
    struct CycleStats {
        size_t full_relations;
        size_t partial_relations;
        size_t cycles_found;
        size_t merged_relations;
    };

    CycleStats find_cycles(
        const std::string& relation_file,
        const std::string& output_file);

private:
    struct LargePrimeGraph {
        std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint64_t>>> adj;

        void add_relation_2lp(uint32_t rel_idx, uint64_t p1, uint64_t p2);
        void add_relation_3lp(uint32_t rel_idx, uint64_t p1, uint64_t p2, uint64_t p3);
    };
};
```

### 7.5 矩阵构建

```cpp
class MatrixBuilder {
public:
    struct SparseMatrix {
        size_t nrows;
        size_t ncols;
        std::vector<uint32_t> row_ptr;
        std::vector<uint32_t> col_idx;

        size_t nnz() const;
        double density() const;
    };

    struct ColumnAssignment {
        uint32_t sign_col;
        std::vector<uint32_t> qchar_cols;
        uint32_t rational_start;
        uint32_t algebraic_start;
        uint32_t large_prime_start;
        uint32_t total_cols;
    };

    SparseMatrix build(
        const std::string& filtered_relation_file,
        const FactorBase& fb);

    static void save(const SparseMatrix& mat, const std::string& path);
    static SparseMatrix load(const std::string& path);
};
```

### 7.6 关系 I/O

```cpp
class RelationIO {
public:
    struct Header {
        uint64_t n_digits;
        uint32_t rational_bound;
        uint32_t algebraic_bound;
        size_t   relation_count;
        std::string poly_hash;
        bool compressed = false;  // 预留 zstd 压缩
    };

    class Writer { /* ... */ };
    class Reader { /* 支持迭代器 */ };
};
```

---

## 8. 线性代数模块（linalg/）

### 8.1 参数

```cpp
struct LanczosParams {
    uint32_t block_size = 64;
    uint32_t max_iterations = 0;  // 0 = auto
    uint32_t checkpoint_interval = 100;

    uint32_t min_solutions = 64;
    uint32_t target_solutions = 96;

    uint32_t num_threads = 0;
    bool     use_gpu = true;
    bool     verify_result = true;
    uint32_t verbose_level = 1;
};
```

### 8.2 稀疏矩阵

```cpp
class SparseMatrixGF2 {
public:
    explicit SparseMatrixGF2(size_t nrows, size_t ncols);

    size_t nrows() const noexcept;
    size_t ncols() const noexcept;
    size_t nnz() const noexcept;

    void multiply(std::span<const uint64_t> x, std::span<uint64_t> y) const;
    void multiply_transpose(std::span<const uint64_t> x, std::span<uint64_t> y) const;
    void multiply_ata(std::span<const uint64_t> x, std::span<uint64_t> y) const;

    struct LayoutOptimization {
        bool reorder_rows = true;   // Cuthill-McKee
        bool reorder_cols = true;
        uint32_t block_rows = 256;
    };
    void optimize_layout(const LayoutOptimization& opt);

    void save(const std::string& path) const;
    static SparseMatrixGF2 load(const std::string& path);

private:
    std::vector<uint32_t> row_ptr_;
    std::vector<uint32_t> col_idx_;
    std::vector<uint32_t> col_ptr_;  // 转置索引
    std::vector<uint32_t> row_idx_;
};
```

### 8.3 块向量

```cpp
class BlockVectorGF2 {
public:
    explicit BlockVectorGF2(size_t length, uint32_t block_size = 64);

    size_t length() const noexcept;
    uint64_t* data() noexcept;

    bool get(size_t row, uint32_t block_col) const noexcept;
    void set(size_t row, uint32_t block_col, bool value) noexcept;

    void xor_with(const BlockVectorGF2& other);
    void randomize(std::mt19937_64& rng);
    void clear() noexcept;

    DenseMatrixGF2_64 inner_product(const BlockVectorGF2& other) const;
};
```

### 8.4 64x64 密集小矩阵

```cpp
class DenseMatrixGF2_64 {
public:
    std::array<uint64_t, 64> rows;

    std::optional<DenseMatrixGF2_64> inverse() const;
    uint32_t rank() const;
    DenseMatrixGF2_64 multiply(const DenseMatrixGF2_64& other) const;
};
```

### 8.5 Block Lanczos

```cpp
class BlockLanczos {
public:
    struct Result {
        std::vector<BlockVectorGF2> solutions;
        size_t iterations;
        double elapsed_seconds;
        bool verified;
    };

    struct Progress {
        size_t current_iteration;
        size_t estimated_total;
        double elapsed_seconds;
        double eta_seconds;
    };

    explicit BlockLanczos(const LanczosParams& params);

    Result solve(
        const SparseMatrixGF2& matrix,
        std::function<void(const Progress&)> progress_cb = nullptr);

    Result resume(
        const SparseMatrixGF2& matrix,
        const std::string& checkpoint_file,
        std::function<void(const Progress&)> progress_cb = nullptr);

    void save_checkpoint(const std::string& path) const;

private:
    bool should_terminate() const;  // 最大迭代 / V=0 / 足够解
};
```

### 8.6 GPU 加速

```cpp
class MetalLinAlg {
public:
    void multiply_ata_gpu(
        const SparseMatrixGF2& matrix,
        std::span<const uint64_t> x,
        std::span<uint64_t> y);

    void batch_xor_gpu(std::span<uint64_t> dest, std::span<const uint64_t> src);

    void upload_matrix(const SparseMatrixGF2& matrix);
    void release_matrix();

    static bool is_available() noexcept;
};
```

### 8.7 高斯消元（调试用）

```cpp
class GaussianElimination {
public:
    static std::vector<std::vector<bool>> null_space(
        const SparseMatrixGF2& matrix);

    static std::vector<std::vector<bool>> structured_gauss(
        const SparseMatrixGF2& matrix,
        size_t max_dense_rows = 1000);
};
```

---

## 9. 平方根模块（sqrt/）

### 9.1 参数

```cpp
struct SqrtParams {
    uint32_t num_threads = 0;
    uint32_t max_attempts = 100;
    bool     verify_factors = true;
    uint32_t verbose_level = 1;
};
```

### 9.2 代数数（完整算术）

```cpp
class AlgebraicNumber {
public:
    explicit AlgebraicNumber(const PolynomialContext& ctx);
    AlgebraicNumber(const PolynomialContext& ctx, std::vector<Integer> coeffs);

    AlgebraicNumber operator+(const AlgebraicNumber& other) const;
    AlgebraicNumber operator-(const AlgebraicNumber& other) const;
    AlgebraicNumber operator*(const AlgebraicNumber& other) const;

    static AlgebraicNumber product(std::span<const AlgebraicNumber> factors);
    AlgebraicNumber square() const;
    AlgebraicNumber pow(uint64_t exp) const;

    static AlgebraicNumber from_ab(const PolynomialContext& ctx, int64_t a, uint64_t b);

    Integer norm() const;
    std::vector<uint64_t> reduce_mod_p(uint64_t p) const;
    const std::vector<Integer>& coefficients() const noexcept;

private:
    void reduce();  // 约简到 f(alpha) = 0
};
```

### 9.3 Montgomery 平方根

```cpp
class MontgomerySqrt {
public:
    explicit MontgomerySqrt(const PolynomialContext& ctx);

    std::optional<AlgebraicNumber> compute(const AlgebraicNumber& gamma);

private:
    std::vector<uint64_t> select_primes(const AlgebraicNumber& gamma);

    Integer estimate_coefficient_bound(
        const std::vector<Relation>& selected_relations);
};
```

### 9.4 有理平方根

```cpp
class RationalSqrt {
public:
    static Integer compute(
        std::span<const ABPair> selected_pairs,
        const Integer& m,
        const Integer& n);
};
```

### 9.5 平方根组合器

```cpp
class SqrtCombiner {
public:
    struct Result {
        Integer factor1;
        Integer factor2;
        size_t attempts;
        double elapsed_seconds;
        bool success;

        enum class Status {
            Success,
            NeedMoreRelations,
            NeedMoreSolutions,
            AlgebraicSqrtFailed,
            UnknownError
        } status;

        std::string error_message;
    };

    struct Progress {
        size_t current_attempt;
        size_t max_attempts;
        std::string status;  // "computing_algebraic", "computing_rational", "verifying"
    };

    SqrtCombiner(const PolynomialContext& ctx, const SqrtParams& params);

    Result extract_factors(
        const std::vector<Relation>& relations,
        const std::vector<BlockVectorGF2>& solutions,
        std::function<void(const Progress&)> progress_cb = nullptr);

private:
    // 两个符号都尝试
    std::optional<std::pair<Integer, Integer>> try_solution(
        const std::vector<Relation>& relations,
        const std::vector<size_t>& selected_indices);
};
```

### 9.6 顶层协调器

```cpp
struct FactorizationResult {
    Integer n;
    std::vector<Integer> factors;
    std::vector<uint32_t> exponents;
    double total_time_seconds;

    struct Timing {
        double poly_select;
        double sieving;
        double filtering;
        double linear_algebra;
        double sqrt;
    } timing;

    bool verify() const;
    std::string to_string() const;
};

class GNFSFactorizer {
public:
    struct Params {
        PolySelectParams poly;
        SieveParams sieve;
        FilterParams filter;
        LanczosParams linalg;
        SqrtParams sqrt;

        std::string work_dir = "./gnfs_work";
        bool keep_intermediates = false;
    };

    explicit GNFSFactorizer(const Params& params);

    FactorizationResult factorize(
        const Integer& n,
        std::function<void(const std::string&)> status_cb = nullptr);

    FactorizationResult resume(
        const std::string& checkpoint_dir,
        std::function<void(const std::string&)> status_cb = nullptr);
};
```

---

## 10. 工具模块（util/）

### 10.1 线程池

```cpp
class ThreadPool {
public:
    explicit ThreadPool(uint32_t num_threads = 0);

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    template<typename Iter, typename Func>
    void parallel_for(Iter begin, Iter end, Func&& func);

    void wait_all();
    uint32_t num_threads() const noexcept;
};
```

### 10.2 日志

```cpp
enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error, Fatal };

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level) noexcept;
    void set_file(const std::string& path);

    template<typename... Args>
    void log_fmt(LogLevel level, std::string_view module,
                 std::format_string<Args...> fmt, Args&&... args);
};

#define GNFS_INFO(module, ...) /* ... */
#define GNFS_DEBUG(module, ...) /* ... */
#define GNFS_ERROR(module, ...) /* ... */
```

### 10.3 检查点管理

```cpp
class CheckpointManager {
public:
    explicit CheckpointManager(const std::string& work_dir);

    struct PolyCheckpoint { uint64_t ad_searched; double best_score; std::string best_poly_file; };
    struct SieveCheckpoint { uint32_t last_q; uint32_t last_r; size_t relations_found; };
    struct LinalgCheckpoint { size_t iteration; std::string state_file; };

    void save_poly(const PolyCheckpoint& cp);
    void save_sieve(const SieveCheckpoint& cp);
    void save_linalg(const LinalgCheckpoint& cp);

    std::optional<PolyCheckpoint> load_poly();
    std::optional<SieveCheckpoint> load_sieve();
    std::optional<LinalgCheckpoint> load_linalg();
};
```

### 10.4 配置

```cpp
class Config {
public:
    static Config load(const std::string& path);  // TOML 格式

    template<typename T>
    T get(std::string_view key) const;

    PolySelectParams to_poly_params() const;
    SieveParams to_sieve_params() const;
    // ... 其他模块参数转换
};
```

### 10.5 SmallVector

```cpp
template<typename T, size_t InlineCapacity>
class SmallVector {
public:
    // Move-only，禁止拷贝
    SmallVector() noexcept = default;
    SmallVector(SmallVector&& other) noexcept;

    size_t size() const noexcept;
    bool is_inline() const noexcept;

    void push_back(T&& value);
    template<typename... Args>
    T& emplace_back(Args&&... args);

private:
    alignas(T) std::byte inline_storage_[sizeof(T) * InlineCapacity];
    T* heap_data_ = nullptr;
};
```

---

## 11. Metal GPU 模块（metal/）

### 11.1 设备管理

```cpp
class MetalDevice {
public:
    static MetalDevice& instance();

    bool is_available() const noexcept;
    std::string device_name() const;
    size_t max_buffer_size() const;

    void* mtl_device() const noexcept;
    void* command_queue() const noexcept;
};
```

### 11.2 缓冲区

```cpp
class MetalBuffer {
public:
    enum class StorageMode { Shared, Private, Managed };

    MetalBuffer(size_t size, StorageMode mode = StorageMode::Shared);

    void upload(const void* data, size_t size, size_t offset = 0);
    void download(void* data, size_t size, size_t offset = 0) const;

    void* map();
    void unmap();
};
```

### 11.3 计算内核

```cpp
class MetalKernel {
public:
    static MetalKernel compile(std::string_view source, std::string_view function_name);
    static MetalKernel load(const std::string& library_path, std::string_view function_name);

    void set_buffer(uint32_t index, const MetalBuffer& buffer);

    template<typename T>
    void set_value(uint32_t index, const T& value);

    void dispatch_1d(uint32_t total_threads, uint32_t threads_per_group = 256);
    void wait();
};
```

### 11.4 Metal Shading Language 内核

主要内核文件：
- `metal/kernels/poly_search.metal` — 多项式搜索初筛
- `metal/kernels/sieve.metal` — 小素数筛法
- `metal/kernels/linalg.metal` — 稀疏矩阵乘法

---

## 12. 实现计划

### 12.1 代码规模估算

| 模块 | 文件数 | 代码行数 |
|------|--------|----------|
| core/ | 5 | 800 |
| factor_base/ | 3 | 600 |
| polynomial/ | 7 | 2000 |
| sieve/ | 8 | 3500 |
| relation/ | 6 | 2000 |
| linalg/ | 6 | 2500 |
| sqrt/ | 5 | 1500 |
| util/ | 6 | 1000 |
| metal/ | 4 + kernels | 800 |
| **总计** | **约50** | **约15000** |

### 12.2 实现顺序

**Phase 1: 基础设施**
1. util/ — 线程池、日志、SmallVector
2. core/ — Integer、ABPair、Relation 等基础类型
3. metal/ — GPU 抽象层（可并行开发）

**Phase 2: 核心算法（无 GPU）**
4. factor_base/ — 因子基
5. polynomial/ — Base-m 选择器（Kleinjung 后续）
6. sieve/ — 格筛法（CPU 版本）
7. relation/ — 关系收集、去重、过滤

**Phase 3: 线性代数与平方根**
8. linalg/ — Block Lanczos
9. sqrt/ — Montgomery 平方根
10. 集成测试：完整流程跑通

**Phase 4: 优化**
11. polynomial/ — Kleinjung 完整实现
12. Metal GPU 加速各模块
13. 性能调优、大规模测试

### 12.3 里程碑

| 里程碑 | 目标 |
|--------|------|
| M1 | 能分解 60 位数（Base-m + 简单筛法） |
| M2 | 能分解 100 位数（完整流程，无 GPU） |
| M3 | 能分解 120 位数（Kleinjung + GPU 加速） |
| M4 | 能分解 150+ 位数（完整优化） |

---

## 13. 参考资料

1. **CADO-NFS** — https://cado-nfs.gitlabpages.inria.fr/
2. **msieve** — https://github.com/radii/msieve
3. **A Tale of Two Sieves** — Carl Pomerance
4. **The Development of the Number Field Sieve** — Lenstra & Lenstra
5. **Montgomery's Square Root Algorithm** — Peter Montgomery
6. **Block Lanczos Algorithm** — Montgomery, 1995

---

*文档结束*
