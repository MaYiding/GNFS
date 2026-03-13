#!/bin/bash
# apply_polynomial_fix.sh - 自动修复 polynomial.cpp

echo "=========================================="
echo "修复 polynomial.cpp 中的类型歧义"
echo "=========================================="
echo ""

# 检查文件是否存在
if [ ! -f "src/core/polynomial.cpp" ]; then
    echo "错误: src/core/polynomial.cpp 不存在"
    echo "请先运行: bash organize_files.sh"
    exit 1
fi

# 备份原文件
echo "备份原文件..."
cp src/core/polynomial.cpp src/core/polynomial.cpp.backup
echo "✓ 备份完成: src/core/polynomial.cpp.backup"
echo ""

# 创建修复后的文件
echo "应用修复..."
cat > src/core/polynomial.cpp << 'EOF'
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
    if (coeffs_.empty()) return Integer(static_cast<int64_t>(0));
    
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
        coeffs_.push_back(Integer(static_cast<int64_t>(0)));
    }
}

} // namespace gnfs::core
EOF

echo "✓ 修复已应用"
echo ""

# 显示差异
echo "修改内容:"
echo "  - Integer(0) → Integer(static_cast<int64_t>(0))"
echo ""

echo "=========================================="
echo "修复完成！"
echo "=========================================="
echo ""
echo "现在运行: bash fix_gmp_and_compile.sh"
