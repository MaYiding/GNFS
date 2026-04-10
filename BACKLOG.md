# BACKLOG — 待办备忘录

> 只记录**未完成**的问题。已完成和误报条目见 `RESOLVED.md`。
> 从文件开头往下读即为优先级：P1 > P1-OPT > P2 > P3 > TEST。

| 级别 | 条数 | 涵盖 |
|------|------|------|
| **P1** | 0 | (已全部修复 — Session 61) |
| **P1-OPT** | 0 | (已清空) |
| **P2** | 1 | class group ×1 |
| **P3** | 24 | 远期架构 ×7, style ×5, dead code ×4, quality ×5, risk ×1, LP merge ×1 |
| **TEST** | 0 | (已全部修复 — Session 64) |

---

## P1 — 高优先级（影响正确性、可靠性）

> **全部已修复** — Session 61, 2026-03-14。详见 RESOLVED.md。

---

## P2 — 中优先级

> **Session 62 修复 25 条, Session 63 修复 7 条, Session 64 修复 9 条。详见 RESOLVED.md。**

### 基础设施

#### [BUG] Class Group Characters 实现仅支持 Cubic Fields
- **发现日期**: 2026-03-13
- **文件**: `sqrt/class_group.hpp`
- **描述**: `class_group.hpp` 的 Minkowski bound、signature、character computation 均假定 degree=3。对 degree≥4 产生错误列值，导致所有 BL 依赖在 sqrt 阶段失败。当前已全局禁用 (`include_class_group=false`)，QC+Schirokauer 足够替代。如需恢复 class group 功能需全面重写。
- **建议**: 若要支持，需正确处理任意 degree 的签名 (r1,r2)、SNF、character computation。参考 PARI/GP 或 SageMath 的实现。

---

## P3 — 低优先级

### 远期架构

#### [FEAT] CSR 矩阵格式（100K+ 行）
- **文件**: `linalg/sparse_matrix.hpp:179-315`
- **描述**: 当前 `vector<SparseRow>` 每行独立 heap 分配。改 CSR（连续 `col_indices` + `row_offsets`）利于 prefetch 和 SIMD

#### [FEAT] Block Wiedemann（130+ 位）
- **描述**: 矩阵 >5M 时 BL 顺序迭代成瓶颈，BW 的 SpMV 可分布式并行

#### [FEAT] 行筛实现（小 N 更高效）
- **描述**: 所有 N 都走格筛。<50 十进制位行筛更简单高效，非紧急

#### [FEAT] Out-of-core Relations
- **文件**: `relation/collector.hpp`
- **描述**: 50+ 十进制位需 10-100M 关系（数 GB），当前全在内存

#### [FEAT] Block Lanczos Out-of-core 矩阵
- **描述**: 矩阵必须完全在 RAM 中

#### [FEAT] ThreadPool Work-Stealing
- **描述**: 筛选 special-Q 开销不均匀，work-stealing 可提升负载均衡

#### [OPT] Bucket Sieve for B > 500K (80+ digit 必需)
- **发现日期**: 2026-03-12
- **描述**: 80-digit (264 bit, degree 5) 实测 15 rels/s，需 11 天完成筛法。主因：lattice sieve O(sieve_entries × sieve_area) 对 149K 条目 × 33M 位置太慢。
  CADO-NFS 用 bucket sieve：按 log_p 桶分配、顺序写入，O(hits) 而非 O(entries×area)。
  还需 Kleinjung poly selection（降低有效范数 10-100×）。
- **建议**: 实现 2 级 bucket sieve (small primes 直接筛, large primes 用 bucket)。参考 CADO-NFS `las/` 或 msieve `sieve/`。

### 代码质量

#### [DEBT] 零大小筛区对极端 skewness
- **发现日期**: 2026-03-14
- **文件**: `sieve/lattice_basis.hpp:196-214`
- **描述**: 极端 skewness 下 `i_half` 可能舍入为 0，导致 `i_width() = 0`，筛法静默产生零候选。
- **建议**: 添加后置条件检查并回退到最小区域。

#### [DEBT] Hensel verbose 路径 f_lead 不可逆时 assert 崩溃
- **发现日期**: 2026-03-14
- **文件**: `sqrt/hensel_sqrt.hpp:979-980`
- **描述**: verbose 模式下 `poly_mul_mod(S, S, f_int, d, modulus)` 重新计算 `f_lead_inv`，若 `f[d]` 对 lifted modulus 不可逆则 assert 崩溃。仅影响 verbose 输出，不影响正确性。
- **建议**: 将 lift 循环中已计算的 `fli` 提升到函数作用域，或用 try/catch 包裹。

#### [DEBT] ECM stage2_naive batch 边界因子遗漏
- **发现日期**: 2026-03-14
- **文件**: `cofactor/ecm.hpp:594-619`
- **描述**: `g == n` fallback 后 `batch_primes.clear()` 清除了包含当前素数的列表。若因子恰好在批次边界素数处，retry 循环可能漏检。
- **建议**: fallback 后确保当前素数也被单独检查。

#### [DEBT] BL gauss_bytes size_t 溢出对超大矩阵
- **发现日期**: 2026-03-14
- **文件**: `src/linalg/block_lanczos.cpp:556`
- **描述**: `m * ((m + n + 63) / 64) * sizeof(uint64_t)` 对 m=n≈4.6M 溢出 `size_t`。当前 GNFS 矩阵不到这个规模，但无保护。
- **建议**: 中间计算用 `uint64_t` 显式类型转换。

### 风格 & 一致性

#### [DEBT] 命名空间风格不一致
- **发现日期**: 2026-03-14
- **文件**: 多个（polynomial_context.hpp, int_polynomial.hpp, gauss.hpp, factor_base.hpp 等）
- **描述**: 部分文件用 `namespace gnfs { namespace core {` (C++98)，部分用 `namespace gnfs::core {` (C++17)。项目标准为 C++20。
- **建议**: 统一为 C++17 嵌套命名空间语法。

#### [DEBT] relation.hpp 包含完整 `<iostream>` 而非 `<iosfwd>`
- **发现日期**: 2026-03-14
- **文件**: `core/relation.hpp:8`
- **描述**: `<iostream>` 在广泛包含的头文件中引入不必要的编译开销。`serialize`/`deserialize` 只需 `std::ostream&` 和 `std::istream&`。
- **建议**: 替换为 `#include <iosfwd>` 或将方法移至 `.cpp`。

#### [DEBT] params.hpp 魔法数 0.30103
- **发现日期**: 2026-03-14
- **文件**: `core/params.hpp:68`
- **描述**: `n_bits * 0.30103` 是 `log10(2)` 的近似值，应使用命名常量。
- **建议**: `constexpr double LOG10_2 = 0.30103;` 或用 `std::log10(2.0)`。

#### [DEBT] SparseRow::xor_with 使用 const_cast 而非 mutable
- **发现日期**: 2026-03-14
- **文件**: `linalg/sparse_matrix.hpp:100`
- **描述**: 对 `const SparseRow& other` 使用 `const_cast` 调用 `ensure_sorted()`。应将 `sorted_` 标记为 `mutable`，`ensure_sorted()` 改为 `const`。
- **建议**: `mutable bool sorted_;` + `void ensure_sorted() const`。

### 潜在风险（需进一步调查）

#### [RISK] Block Lanczos 三步递推与 Montgomery 1995 不一致
- **发现日期**: 2026-03-14
- **文件**: `src/linalg/block_lanczos.cpp:479-499`
- **描述**: 代码使用包含 `V_pprev`（前两步向量）的三步递推，而 Montgomery 1995 论文只使用两步递推（V_cur 和 V_prev）。额外的 `F_cur = V_pprev^T · B · V_cur` 项和 `D_pprev * F_cur` 应用没有已知数学基础。然而所有测试（L1-L5, 25-digit, stress）均通过，可能是有效的变体或冗余项。
- **建议**: 对比 Montgomery 1995 §3 公式逐项核实。若额外项冗余（恒等于零），则为死代码可移除。若实际影响结果，需确认数学正当性。

#### [DEBT] LP merge 顺序因 unordered_map 不确定
- **发现日期**: 2026-03-14
- **文件**: `relation/filter.hpp:344-359`
- **描述**: Phase 1 和 Phase 2 遍历 `unordered_map`，迭代顺序依赖实现和 ASLR。相同输入不同运行产生不同 LP 配对，导致合并后关系数不确定，影响可重复性。
- **建议**: 排序 key 后处理，或使用 `std::map`。

### Dead Code

#### [DEBT] params.hpp print_summary() 全空 no-op
- **发现日期**: 2026-03-14
- **文件**: `core/params.hpp:348-356`
- **描述**: 定义了 `print_size` lambda 后立即 `(void)print_size`，函数体无任何输出。
- **建议**: 实现功能或删除。

#### [DEBT] BL header 串行 SpMV 函数未使用
- **发现日期**: 2026-03-14
- **文件**: `linalg/block_lanczos.hpp:185-205`
- **描述**: `spmv_forward` 和 `spmv_transpose` 串行版本从未被调用，`.cpp` 只使用并行版本。
- **建议**: 删除。

#### [DEBT] factor_base compute_log_prime 非精确版未使用
- **发现日期**: 2026-03-14
- **文件**: `factor_base/factor_base.hpp:189-195`
- **描述**: 基于 `__builtin_clz` 的粗略版本从未被调用，所有实际代码使用 `compute_log_prime_precise`。
- **建议**: 删除或标记 `[[deprecated]]`。

#### [DEBT] PolynomialOptimizer golden_section_skewness 未使用
- **发现日期**: 2026-03-14
- **文件**: `polynomial/polynomial_optimizer.hpp:243`
- **描述**: 黄金分割搜索法定义但未被调用（Murphy 用网格搜索）。
- **建议**: 删除。

---

## FEAT — 80/100-digit Scalability
- **描述**: 已用 CADO-NFS 校准参数 (C80: B=1M/2M, C100: B=8M/16M)。矩阵大小合理但筛法太慢。需 bucket sieve + Kleinjung 才能在合理时间完成
