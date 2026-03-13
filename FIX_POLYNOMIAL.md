# 修复 polynomial.cpp 的歧义错误

## 问题
编译器在 `Integer(0)` 时不知道选择哪个构造函数：
- `Integer(int64_t value)` 
- `Integer(const char* str, int base = 10)`

## 解决方案

在 `src/core/polynomial.cpp` 中做以下修改：

### 修改 1: 第 35 行
**原代码:**
```cpp
if (coeffs_.empty()) return Integer(0);
```

**改为:**
```cpp
if (coeffs_.empty()) return Integer(static_cast<int64_t>(0));
```

### 修改 2: 第 55 行
**原代码:**
```cpp
coeffs_.push_back(Integer(0));
```

**改为:**
```cpp
coeffs_.push_back(Integer(static_cast<int64_t>(0)));
```

## 完整修复后的代码

```cpp
#include "gnfs/core/polynomial.hpp"

namespace gnfs::core {

IntPolynomial::IntPolynomial(size_t degree) : coeffs_(degree + 1) {}

IntPolynomial::IntPolynomial(std::vector<Integer> coeffs) : coeffs_(std::move(coeffs)) {
    normalize();
}

size_t IntPolynomial::degree() const {
    if (coeffs_.empty()) return 0;
    for (size_t i = coeffs_.size(); i > 0; --i) {
        if (!coeffs_[i - 1].is_zero()) {
            return i - 1;
        }
    }
    return 0;
}

const Integer& IntPolynomial::operator[](size_t i) const {
    static Integer zero(static_cast<int64_t>(0));
    if (i >= coeffs_.size()) return zero;
    return coeffs_[i];
}

Integer& IntPolynomial::operator[](size_t i) {
    if (i >= coeffs_.size()) {
        coeffs_.resize(i + 1);
    }
    return coeffs_[i];
}

Integer IntPolynomial::evaluate(const Integer& x) const {
    if (coeffs_.empty()) return Integer(static_cast<int64_t>(0));  // 修复行
    
    // Horner's method
    Integer result = coeffs_.back().clone();
    for (size_t i = coeffs_.size() - 1; i > 0; --i) {
        result *= x;
        result += coeffs_[i - 1];
    }
    return result;
}

void IntPolynomial::resize(size_t new_degree) {
    coeffs_.resize(new_degree + 1);
}

void IntPolynomial::normalize() {
    while (!coeffs_.empty() && coeffs_.back().is_zero()) {
        coeffs_.pop_back();
    }
    if (coeffs_.empty()) {
        coeffs_.push_back(Integer(static_cast<int64_t>(0)));  // 修复行
    }
}

} // namespace gnfs::core
```

## 快速修复命令

使用 sed 命令快速修复：

```bash
# 备份原文件
cp src/core/polynomial.cpp src/core/polynomial.cpp.bak

# 修复第一处
sed -i.tmp 's/return Integer(0);/return Integer(static_cast<int64_t>(0));/g' src/core/polynomial.cpp

# 修复第二处
sed -i.tmp 's/Integer(0)/Integer(static_cast<int64_t>(0))/g' src/core/polynomial.cpp

# 删除临时文件
rm src/core/polynomial.cpp.tmp

# 重新编译
bash fix_gmp_and_compile.sh
```

或者手动编辑 `src/core/polynomial.cpp` 文件，替换上述两行。
