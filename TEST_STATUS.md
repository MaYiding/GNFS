# GNFS 测试状态报告

## 测试概览

| 测试名称 | 状态 | 说明 |
|---------|------|------|
| test_small_vector | ✅ 应该通过 | 容器测试，不依赖外部库 |
| test_integer | ✅ 应该通过 | Integer 类测试，依赖 GMP |
| test_thread_pool | ✅ 应该通过 | 线程池测试 |
| test_factor_base | ⚠️ API 不匹配 | 需要修改 FactorBase API |
| test_special_q | ⚠️ API 不匹配 | 需要修改 SpecialQGenerator API |
| test_lattice_sieve | ⚠️ 实现不完整 | LatticeSieve 是占位符 |
| test_relation_collector | ✅ 应该通过 | 基本功能已实现 |
| test_sieve_basic | ⚠️ 依赖未完成模块 | 需要完整的筛法实现 |
| test_cofactor | ⚠️ 可能部分通过 | 基本功能已实现 |
| test_linalg | ⚠️ 实现不完整 | BlockLanczos 是占位符 |
| test_sqrt | ⚠️ 实现不完整 | 平方根计算简化版 |
| test_gnfs_e2e | ❌ 依赖所有模块 | 需要所有模块完整实现 |
| test_murphy | ⚠️ 简化实现 | MurphyEvaluator 简化版 |
| test_kleinjung | ⚠️ 简化实现 | Kleinjung 算法简化版 |
| test_kleinjung_large | ⚠️ 简化实现 | 同上 |
| test_factor_with_kleinjung | ❌ 依赖所有模块 | 端到端测试 |
| test_sqrt_debug | ⚠️ 实现不完整 | 平方根调试测试 |

## 详细测试分析

### ✅ 应该通过的测试

#### 1. test_small_vector
**文件**: `tests/test_small_vector.cpp`  
**依赖**: 仅头文件 `gnfs/util/small_vector.hpp`  
**状态**: ✅ 完全实现  
**预期结果**: PASS

**功能**:
- 构造和析构
- push_back / pop_back
- 容量管理
- 迭代器
- 移动语义

**可能的问题**: 无

---

#### 2. test_integer
**文件**: `tests/test_integer.cpp`  
**依赖**: `gnfs/core/integer.hpp` + GMP  
**状态**: ✅ 完全实现  
**预期结果**: PASS（如果 GMP 正确安装）

**功能**:
- 构造（从 int, string）
- 算术运算（+, -, *, /, %）
- 比较运算
- 位操作
- GCD, powmod
- 素性测试

**可能的问题**:
- GMP 未安装或路径不正确
- 链接错误

---

#### 3. test_thread_pool
**文件**: `tests/test_thread_pool.cpp`  
**依赖**: `gnfs/util/thread_pool.hpp`  
**状态**: ✅ 完全实现  
**预期结果**: PASS

**功能**:
- 任务提交
- 并行执行
- Future 获取结果
- 等待完成

**可能的问题**:
- 线程库链接问题

---

#### 4. test_relation_collector
**文件**: `tests/test_relation_collector.cpp`  
**依赖**: `gnfs/relation/collector.hpp`  
**状态**: ✅ 基本实现完整  
**预期结果**: 应该 PASS

**功能**:
- 添加关系
- 线程安全
- 获取关系列表
- 大小统计

**可能的问题**:
- 如果测试依赖特定的 API 可能需要调整

---

### ⚠️ API 不匹配需要修改

#### 5. test_factor_base
**文件**: `tests/test_factor_base.cpp`  
**当前状态**: ⚠️ API 不匹配  
**预期结果**: 编译错误或运行失败

**测试期望的 API**:
```cpp
// 期望的接口
auto result = BaseMSelector::select(n, degree);  // 返回结果对象
auto ctx = BaseMSelector::create_context(n, result);

struct FactorBaseBuilder::Options {
    uint32_t rational_bound;
    uint32_t algebraic_bound;
    bool parallel;
};

auto fb = FactorBaseBuilder::build(ctx, opts);

// 期望的 FactorBase 方法
fb.rational_count();
fb.algebraic_count();
fb.rational();  // 返回 span 或 vector
fb.algebraic();
fb.find_rational(p);  // 返回 optional<size_t>

// 期望的 PolynomialContext 方法
ctx.evaluate_mod(r, p);  // f(r) mod p
```

**当前实现的 API**:
```cpp
// 当前接口
PolynomialContext select_base_m_polynomial(const Integer& n, uint32_t degree);

FactorBaseBuilder builder(ctx);
FactorBase build(uint32_t rational_bound, uint32_t algebraic_bound);

fb.rational_size();
fb.algebraic_size();
```

**需要的修改**:
1. 修改 `BaseMSelector` 返回类型
2. 添加 `Options` 结构体
3. 添加 `rational()`, `algebraic()` 方法
4. 添加 `find_rational()` 查找方法
5. 在 `PolynomialContext` 添加 `evaluate_mod()` 方法

---

#### 6. test_special_q
**文件**: `tests/test_special_q.cpp`  
**当前状态**: ⚠️ API 可能不匹配  
**预期结果**: 可能编译错误

**需要检查**: SpecialQGenerator 的接口是否匹配测试期望

---

### ⚠️ 实现不完整但可以编译

#### 7. test_lattice_sieve
**文件**: `tests/test_lattice_sieve.cpp`  
**当前状态**: ⚠️ 实现是占位符  
**预期结果**: 编译通过，但测试失败（返回空结果）

**问题**:
- `LatticeSieve::sieve()` 当前只返回空向量
- 需要实现实际的格筛法算法

**需要实现**:
1. 格基构造
2. 筛选区域扫描
3. 光滑数对识别
4. 因子分解

---

#### 8. test_cofactor
**文件**: `tests/test_cofactor.cpp`  
**当前状态**: ⚠️ 简化实现  
**预期结果**: 部分测试可能通过

**已实现**:
- 试除法
- 小素数因子提取
- 简单的大素数检测

**未实现**:
- ECM 算法
- SIQS 余因子分解
- 高级优化

---

#### 9. test_linalg
**文件**: `tests/test_linalg.cpp`  
**当前状态**: ⚠️ BlockLanczos 是占位符  
**预期结果**: 编译通过，测试失败

**问题**:
- `BlockLanczos::find_dependencies()` 返回空依赖

**需要实现**:
1. Block Lanczos 迭代
2. GF(2) 矩阵乘法
3. 零空间向量查找
4. 收敛判断

---

#### 10. test_sqrt
**文件**: `tests/test_sqrt.cpp`  
**当前状态**: ⚠️ 简化实现  
**预期结果**: 可能失败（计算不正确）

**问题**:
- `RationalSqrt` 简化实现
- `AlgebraicSqrt` 不在代数数域中计算

**需要实现**:
1. 正确的有理侧乘积和平方根
2. 代数数域中的运算
3. 模 n 的平方根计算

---

### ❌ 依赖未完成模块的测试

#### 11. test_sieve_basic
**文件**: `tests/test_sieve_basic.cpp`  
**当前状态**: ❌ 依赖未完成的筛法  
**预期结果**: 测试失败

**依赖**:
- 完整的 LatticeSieve 实现
- Cofactorizer 工作正常
- RelationCollector 工作正常

---

#### 12. test_gnfs_e2e
**文件**: `tests/test_gnfs_e2e.cpp`  
**当前状态**: ❌ 端到端测试，依赖所有模块  
**预期结果**: 失败

**依赖模块**:
1. ✅ Polynomial selection
2. ✅ Factor base construction
3. ❌ Sieving (不完整)
4. ⚠️ Cofactorization (简化)
5. ✅ Relation collection
6. ✅ Relation filtering
7. ✅ Matrix building
8. ❌ Linear algebra (不完整)
9. ❌ Square root (不完整)
10. Factor extraction

**需要**: 完成所有上述模块

---

#### 13-16. Kleinjung 相关测试
**文件**: 
- `test_murphy.cpp`
- `test_kleinjung.cpp`
- `test_kleinjung_large.cpp`
- `test_factor_with_kleinjung.cpp`

**当前状态**: ⚠️ 简化实现  
**预期结果**: 
- Murphy 测试可能通过（简化版）
- Kleinjung 测试会运行但选择的多项式不是最优的
- 完整分解测试会失败（依赖其他未完成模块）

---

## 优先级修复顺序

### 第一步：修复 API（让测试能编译）

1. **test_factor_base**: 修改 FactorBase API
   - 添加 `Options` 结构
   - 添加 `rational()`, `algebraic()` 方法
   - 添加 `find_rational()` 方法
   - 添加 `evaluate_mod()` 到 PolynomialContext

2. **test_special_q**: 检查并修复 SpecialQGenerator API

### 第二步：实现核心算法（让测试通过）

3. **test_lattice_sieve**: 实现格筛法
4. **test_linalg**: 实现 Block Lanczos
5. **test_sqrt**: 实现正确的平方根计算
6. **test_cofactor**: 优化余因子分解

### 第三步：端到端测试

7. **test_sieve_basic**: 集成测试
8. **test_gnfs_e2e**: 完整流程测试

## 运行测试的建议顺序

```bash
# 第一批：应该通过的基础测试
./test_integer
./test_small_vector
./test_thread_pool
./test_relation_collector

# 第二批：API 需要修复的测试（修复后运行）
./test_factor_base
./test_special_q

# 第三批：实现不完整的测试（实现后运行）
./test_lattice_sieve
./test_cofactor
./test_linalg
./test_sqrt

# 第四批：集成测试（所有模块完成后运行）
./test_sieve_basic
./test_gnfs_e2e
./test_factor_with_kleinjung
```

## 编译诊断

如果编译失败，按以下顺序检查：

1. **GMP 相关错误**:
   ```
   fatal error: gmp.h: No such file or directory
   ```
   → 安装 GMP: `brew install gmp` 或 `apt install libgmp-dev`

2. **C++20 相关错误**:
   ```
   error: 'std::invoke_result_t' has not been declared
   ```
   → 更新编译器或添加 `-std=c++20`

3. **链接错误**:
   ```
   undefined reference to `mpz_init'
   ```
   → 检查 CMakeLists.txt 是否正确链接 GMP

4. **头文件找不到**:
   ```
   fatal error: 'gnfs/core/integer.hpp' file not found
   ```
   → 检查文件是否存在于 `include/gnfs/core/integer.hpp`

## 总结

- ✅ **4 个测试应该直接通过**: integer, small_vector, thread_pool, relation_collector
- ⚠️ **2 个测试需要 API 修复**: factor_base, special_q
- ⚠️ **6 个测试需要完善实现**: lattice_sieve, cofactor, linalg, sqrt, murphy, kleinjung
- ❌ **3 个端到端测试**: 需要所有模块完成

**当前可编译程度**: ~85%  
**当前可运行测试**: ~25%  
**完全实现程度**: ~40%

---

**建议**: 先确保基础测试通过，再逐步修复 API 和实现核心算法。
