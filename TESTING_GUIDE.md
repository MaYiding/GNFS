# GNFS 测试运行指南

## 准备工作

### 1. 编译项目

```bash
cd /path/to/GNFS
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### 2. 验证编译成功

```bash
# 检查生成的测试可执行文件
ls -lh test_*

# 应该看到：
# test_integer
# test_small_vector
# test_thread_pool
# test_factor_base
# test_lattice_sieve
# ... 等等
```

## 测试执行计划

### 第一批：基础测试（应该 100% 通过）

这些测试不依赖复杂的算法，应该直接通过。

#### test_integer
```bash
./test_integer

# 预期输出：
# Testing construction...
#   Construction: PASS
# Testing arithmetic...
#   Arithmetic: PASS
# ...
# All tests passed!
```

**如果失败**:
- 检查 GMP 是否正确安装
- 检查链接是否正确
- 查看具体错误信息

#### test_small_vector
```bash
./test_small_vector

# 预期输出：
# Testing SmallVector...
# All tests passed!
```

**如果失败**:
- 可能是模板实例化问题
- 检查 C++20 支持

#### test_thread_pool
```bash
./test_thread_pool

# 预期输出：
# Testing ThreadPool...
# All tests passed!
```

**如果失败**:
- 检查线程库链接
- 检查 std::thread 支持

#### test_relation_collector
```bash
./test_relation_collector

# 预期输出：
# Testing RelationCollector...
# All tests passed!
```

### 第二批：API 测试（应该 95% 通过）

这些测试验证 API 是否正确匹配。

#### test_factor_base
```bash
./test_factor_base

# 预期输出：
# Testing prime sieve...
#   Prime sieve: PASS
# Testing algebraic roots...
#   Algebraic roots: PASS (XX primes)
# Testing index lookup...
#   Index lookup: PASS
# ...
```

**可能的问题**:
1. `BaseMSelector::select()` 返回类型不匹配
   - 检查返回 `PolynomialSelectionResult`
   
2. `FactorBaseBuilder::build()` API 不匹配
   - 检查 Options 结构体
   
3. `evaluate_mod()` 方法找不到
   - 检查 PolynomialContext 定义

**调试技巧**:
```bash
# 使用 gdb 调试
gdb ./test_factor_base
(gdb) run
# 如果崩溃，查看 backtrace
(gdb) bt
```

### 第三批：算法测试（预期 60-80% 通过）

#### test_lattice_sieve
```bash
./test_lattice_sieve

# 预期：可能找到一些关系，但不一定很多
```

**评估标准**:
- ✅ 不崩溃
- ✅ 能找到至少几个关系
- ⚠️ 关系数量可能不足（算法简化版）

**如果完全没有关系**:
- 检查筛选阈值
- 检查因子基是否正确构造
- 增加筛选区域大小

#### test_linalg
```bash
./test_linalg

# 预期：能找到一些依赖向量
```

**评估标准**:
- ✅ 矩阵构造成功
- ✅ 找到至少一个依赖向量
- ✅ 依赖向量满足 Ax = 0

**调试**:
```cpp
// 如果找不到依赖，尝试：
// 1. 打印矩阵维度
std::cout << "Matrix: " << matrix.rows << "x" << matrix.cols << std::endl;

// 2. 检查矩阵不是零矩阵
for (size_t i = 0; i < matrix.rows; ++i) {
    std::cout << "Row " << i << ": " << matrix.data[i].size() << " entries" << std::endl;
}
```

#### test_sqrt
```bash
./test_sqrt

# 预期：能计算平方根（结果可能不完全正确）
```

**评估标准**:
- ✅ 不崩溃
- ✅ 返回一个整数
- ⚠️ 结果可能不是正确的因子（算法简化）

#### test_cofactor
```bash
./test_cofactor

# 预期：试除法工作，但大数可能失败
```

**评估标准**:
- ✅ 小素数因子能找到
- ⚠️ 大余因子处理不完整

### 第四批：集成测试（可能失败）

#### test_gnfs_e2e
```bash
./test_gnfs_e2e

# 预期：可能在某个阶段失败
```

**典型失败点**:
1. **筛法阶段**: 找不到足够的关系
   - 增加筛选区域
   - 降低阈值
   
2. **线性代数阶段**: 找不到依赖
   - 需要更多关系
   - 矩阵可能太稀疏
   
3. **平方根阶段**: 计算错误
   - 代数侧实现简化
   - 可能需要更复杂的算法

**逐步调试**:
```bash
# 添加详细输出
export GNFS_VERBOSE=1
./test_gnfs_e2e

# 或者修改测试代码添加打印
```

## 测试分析工具

### 运行所有测试并记录结果

```bash
# 运行所有测试并保存输出
ctest --verbose > test_results.txt 2>&1

# 查看通过/失败统计
grep -E "(PASS|FAIL)" test_results.txt | sort | uniq -c
```

### 内存检查（如果有 valgrind）

```bash
# 检查内存泄漏
valgrind --leak-check=full ./test_integer

# 检查其他测试
for test in test_*; do
    echo "Checking $test..."
    valgrind --leak-check=summary ./$test 2>&1 | grep "definitely lost"
done
```

### 性能分析

```bash
# 使用 time 测试
time ./test_lattice_sieve
time ./test_linalg

# 使用 perf (Linux)
perf stat ./test_gnfs_e2e
```

## 常见问题和解决方案

### 问题 1: Segmentation Fault

```bash
# 使用 gdb
gdb ./test_name
(gdb) run
# 崩溃后
(gdb) bt        # 查看调用栈
(gdb) info locals  # 查看局部变量
```

**常见原因**:
- 数组越界
- 空指针访问
- 栈溢出（递归太深）

### 问题 2: 测试挂起

```bash
# 使用 timeout
timeout 30s ./test_name

# 如果超时，可能是死锁或无限循环
```

**常见原因**:
- 线程死锁
- 无限循环
- 等待资源

### 问题 3: 数值结果不正确

**调试技巧**:
1. 添加断言验证中间结果
2. 打印关键变量
3. 使用小数据测试
4. 与已知正确实现对比

### 问题 4: 性能问题

```bash
# 编译优化版本
cmake .. -DCMAKE_BUILD_TYPE=Release

# 检查是否使用 -O3
grep "O3" CMakeCache.txt
```

## 测试报告模板

创建测试报告以跟踪进度：

```markdown
# GNFS 测试报告 - YYYY-MM-DD

## 系统环境
- OS: 
- Compiler: 
- GMP Version: 
- CMake Version: 

## 测试结果

### 基础测试
- [ ] test_integer: PASS/FAIL
- [ ] test_small_vector: PASS/FAIL
- [ ] test_thread_pool: PASS/FAIL
- [ ] test_relation_collector: PASS/FAIL

### API 测试
- [ ] test_factor_base: PASS/FAIL
  - Issues: 
  
### 算法测试
- [ ] test_lattice_sieve: PASS/FAIL
  - Relations found: 
- [ ] test_linalg: PASS/FAIL
  - Dependencies found: 
- [ ] test_sqrt: PASS/FAIL
- [ ] test_cofactor: PASS/FAIL

### 集成测试
- [ ] test_gnfs_e2e: PASS/FAIL
  - Failed at: 
  
## 统计
- Total tests: 
- Passed: 
- Failed: 
- Pass rate: 

## 主要问题
1. 
2. 
3. 

## 下一步行动
1. 
2. 
3. 
```

## 持续集成建议

### 自动化测试脚本

```bash
#!/bin/bash
# run_tests.sh

set -e

echo "=== GNFS Test Suite ==="
echo "Date: $(date)"
echo ""

# 编译
echo "Building..."
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# 运行测试
echo ""
echo "Running tests..."

TOTAL=0
PASSED=0

for test in test_*; do
    if [ -x "$test" ]; then
        TOTAL=$((TOTAL + 1))
        echo -n "Running $test... "
        
        if ./$test > /dev/null 2>&1; then
            echo "PASS"
            PASSED=$((PASSED + 1))
        else
            echo "FAIL"
        fi
    fi
done

echo ""
echo "=== Summary ==="
echo "Total: $TOTAL"
echo "Passed: $PASSED"
echo "Failed: $((TOTAL - PASSED))"
echo "Pass rate: $(echo "scale=2; $PASSED * 100 / $TOTAL" | bc)%"
```

使用:
```bash
chmod +x run_tests.sh
./run_tests.sh
```

## 成功标准

### 最小可行产品（MVP）
- ✅ 所有基础测试通过
- ✅ test_factor_base 通过
- ✅ test_lattice_sieve 找到至少 10 个关系
- ✅ test_linalg 找到至少 1 个依赖
- ⚠️ test_gnfs_e2e 可以运行（即使不成功）

### 完全功能
- ✅ 所有测试通过
- ✅ 能分解至少一个 40-50 位的合数
- ✅ 没有内存泄漏
- ✅ 性能合理

## 获取帮助

如果遇到问题：
1. 查看 `BUILD.md` 编译问题
2. 查看 `DEBUGGING_SUMMARY.md` 已知问题
3. 查看 `PROGRESS_UPDATE.md` 实现细节
4. 提交 Issue 附上：
   - 完整错误信息
   - 系统环境
   - 测试输出
   - GDB backtrace（如果崩溃）

---

祝测试顺利！🧪
