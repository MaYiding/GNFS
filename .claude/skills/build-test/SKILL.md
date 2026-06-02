---
name: build-test
description: 构建 GNFS 项目并运行测试。可选参数：test name 或 "all"
---

# GNFS Build & Test

快速构建并测试 GNFS 项目。

## Workflow

1. **编译项目**:
```bash
cmake --build build -j$(sysctl -n hw.ncpu) 2>&1
```

2. **运行测试**:
   - 如果用户指定了测试名称，只运行该测试
   - 如果参数是 "all" 或无参数，运行全部测试
   - **E2E 测试始终最后运行**（最重要的验证）

```bash
# 全部测试
ctest --test-dir build --output-on-failure

# 单个测试
./build/test_gnfs_e2e
```

3. **结果报告**: 汇总通过/失败的测试，标注失败原因

## Test Names
- `test_small_vector`, `test_integer`, `test_thread_pool`
- `test_factor_base`, `test_special_q`
- `test_lattice_sieve`, `test_relation_collector`, `test_sieve_basic`
- `test_cofactor`, `test_linalg`, `test_sqrt`, `test_sqrt_debug`
- `test_murphy`, `test_kleinjung`, `test_kleinjung_large`
- `test_factor_with_kleinjung`
- `test_gnfs_e2e` ← **最重要**

## 编译失败处理
如果编译失败：
1. 检查错误消息中的文件和行号
2. 读取相关代码
3. 修复编译错误
4. 重新编译
