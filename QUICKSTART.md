# GNFS 快速入门指南

## 5 分钟快速启动

### 1. 安装依赖 (1 分钟)

**macOS:**
```bash
brew install cmake gmp
```

**Ubuntu/Debian:**
```bash
sudo apt update && sudo apt install -y cmake libgmp-dev build-essential
```

**Fedora/RHEL:**
```bash
sudo dnf install -y cmake gmp-devel gcc-c++
```

### 2. 构建项目 (2 分钟)

```bash
# 克隆或进入项目目录
cd GNFS

# 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j8
```

### 3. 运行测试 (2 分钟)

```bash
# 运行基础测试
./test_integer
./test_small_vector
./test_thread_pool

# 运行所有测试
ctest
```

## 基本使用示例

### 示例 1: 使用 Integer 类

```cpp
#include "gnfs/core/integer.hpp"
#include <iostream>

using gnfs::core::Integer;

int main() {
    // 创建大整数
    Integer a("123456789012345678901234567890");
    Integer b("987654321098765432109876543210");
    
    // 算术运算
    Integer sum = a + b;
    Integer product = a * b;
    
    // GCD
    Integer g = gcd(a, b);
    
    std::cout << "GCD: " << g << std::endl;
    
    return 0;
}
```

### 示例 2: 多项式选择

```cpp
#include "gnfs/polynomial/base_m.hpp"
#include <iostream>

using gnfs::core::Integer;
using gnfs::polynomial::select_base_m_polynomial;

int main() {
    // 要分解的数
    Integer n("1000036000099");  // 1000003 * 1000033
    
    // 选择 3 次多项式
    auto ctx = select_base_m_polynomial(n, 3);
    
    // 输出多项式
    std::cout << "f(x) = ";
    for (int i = ctx.f.degree(); i >= 0; --i) {
        std::cout << ctx.f[i];
        if (i > 0) std::cout << "*x^" << i << " + ";
    }
    std::cout << std::endl;
    
    return 0;
}
```

### 示例 3: 构造因子基

```cpp
#include "gnfs/polynomial/base_m.hpp"
#include "gnfs/factor_base/builder.hpp"
#include <iostream>

using gnfs::core::Integer;
using gnfs::polynomial::select_base_m_polynomial;
using gnfs::factor_base::FactorBaseBuilder;

int main() {
    Integer n("1000036000099");
    auto ctx = select_base_m_polynomial(n, 3);
    
    // 构造因子基
    FactorBaseBuilder builder(ctx);
    auto fb = builder.build(1000, 1000);  // 界限: 1000
    
    std::cout << "Rational primes: " << fb.rational_size() << std::endl;
    std::cout << "Algebraic primes: " << fb.algebraic_size() << std::endl;
    
    return 0;
}
```

## 常见问题快速解决

### Q: 编译错误 "找不到 gmp.h"

```bash
# 安装 GMP
brew install gmp              # macOS
sudo apt install libgmp-dev   # Ubuntu
```

### Q: 编译错误 "C++20 不支持"

```bash
# 更新编译器
# Ubuntu
sudo apt install gcc-11 g++-11
export CC=gcc-11
export CXX=g++-11

# 然后重新配置
cd build
rm -rf *
cmake ..
```

### Q: 链接错误 "undefined reference to mpz_*"

```bash
# 手动指定 GMP 路径
cmake .. -DGMP_LIBRARY=/usr/local/lib/libgmp.so \
         -DGMP_INCLUDE_DIR=/usr/local/include
```

### Q: 测试失败

```bash
# 运行详细测试看具体错误
ctest --verbose

# 或单独运行失败的测试
./test_name --verbose
```

## 项目结构速览

```
GNFS/
├── include/gnfs/          # 公共头文件
│   ├── core/              # 核心类型 (Integer, Polynomial)
│   ├── polynomial/        # 多项式选择
│   ├── factor_base/       # 因子基构造
│   ├── sieve/             # 筛法
│   ├── cofactor/          # 余因子分解
│   ├── relation/          # 关系处理
│   ├── linalg/            # 线性代数
│   ├── sqrt/              # 平方根计算
│   └── util/              # 工具类
├── src/                   # 实现文件
├── tests/                 # 测试文件
├── CMakeLists.txt         # 构建配置
├── README.md              # 项目说明
├── BUILD.md               # 详细构建指南
└── DEBUGGING_SUMMARY.md   # 调试总结
```

## 核心概念

### GNFS 算法流程

```
1. 多项式选择    → select_base_m_polynomial() 或 select_kleinjung_polynomial()
2. 因子基构造    → FactorBaseBuilder::build()
3. 筛法          → LatticeSieve::sieve()
4. 余因子分解    → Cofactorizer::try_factor()
5. 关系收集      → RelationCollector::add()
6. 关系过滤      → RelationFilter::filter()
7. 矩阵构造      → MatrixBuilder::build()
8. 线性代数      → BlockLanczos::find_dependencies()
9. 平方根计算    → RationalSqrt::compute() + AlgebraicSqrt::compute()
10. 因子提取     → gcd(rational_sqrt - algebraic_sqrt, n)
```

## 性能提示

1. **Release 构建**: 始终使用 Release 模式进行性能测试
   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Release
   ```

2. **并行编译**: 使用多核心加速编译
   ```bash
   cmake --build . -j$(nproc)
   ```

3. **线程数**: 设置合适的线程数（测试中会用到）
   ```bash
   export OMP_NUM_THREADS=16
   ```

## 进阶主题

详细信息请参考：
- **BUILD.md** - 完整构建说明和故障排除
- **README.md** - 项目功能和限制
- **DEBUGGING_SUMMARY.md** - 当前状态和待办事项

## 贡献

欢迎贡献！当前最需要帮助的领域：
1. 实现完整的格筛法 (`src/sieve/lattice_sieve.cpp`)
2. 实现 Block Lanczos 算法 (`src/linalg/block_lanczos.cpp`)
3. 实现代数侧平方根计算 (`src/sqrt/algebraic_sqrt.cpp`)
4. 添加更多测试用例
5. 性能优化

## 获取帮助

- 查看文档: `README.md`, `BUILD.md`, `DEBUGGING_SUMMARY.md`
- 提交 Issue: 包含操作系统、编译器版本、完整错误信息
- 查看测试代码: `tests/` 目录中有使用示例

---

**祝你使用愉快！** 🚀
