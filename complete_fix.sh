#!/bin/bash
# complete_fix.sh - 完整自动修复所有编译错误

echo "=========================================="
echo "GNFS 完整修复脚本"
echo "=========================================="
echo ""

set -e  # 遇到错误就停止

# 步骤 1: 备份关键文件
echo "步骤 1: 备份关键文件..."
cp include/gnfs/core/integer.hpp include/gnfs/core/integer.hpp.backup
cp src/core/integer.cpp src/core/integer.cpp.backup
echo "✓ 备份完成"
echo ""

# 步骤 2: 替换 Integer 头文件
echo "步骤 2: 更新 Integer 头文件..."
cp integer_extended.hpp include/gnfs/core/integer.hpp
echo "✓ Integer.hpp 已更新"
echo ""

# 步骤 3: 在 Integer.cpp 末尾添加新方法
echo "步骤 3: 添加新方法到 Integer.cpp..."

# 在 namespace 结束前插入新方法
awk '
/^} \/\/ namespace gnfs::core/ {
    # 插入新方法
    print ""
    print "uint64_t Integer::to_uint64() const {"
    print "    if (!mpz_fits_ulong_p(value_)) {"
    print "        throw std::overflow_error(\"Integer does not fit in uint64_t\");"
    print "    }"
    print "    return mpz_get_ui(value_);"
    print "}"
    print ""
    print "double Integer::to_double() const {"
    print "    return mpz_get_d(value_);"
    print "}"
    print ""
    print "size_t Integer::num_digits(int base) const {"
    print "    return mpz_sizeinbase(value_, base);"
    print "}"
    print ""
    print "bool Integer::fits_uint64() const {"
    print "    return mpz_fits_ulong_p(value_) != 0;"
    print "}"
    print ""
    print "bool Integer::fits_int64() const {"
    print "    return mpz_fits_slong_p(value_) != 0;"
    print "}"
    print ""
    print "bool Integer::is_odd() const {"
    print "    return mpz_odd_p(value_) != 0;"
    print "}"
    print ""
    print "bool Integer::is_even() const {"
    print "    return mpz_even_p(value_) != 0;"
    print "}"
    print ""
    print "Integer& Integer::operator*=(int64_t value) {"
    print "    mpz_mul_si(value_, value_, value);"
    print "    return *this;"
    print "}"
    print ""
    print "Integer& Integer::operator+=(int64_t value) {"
    print "    if (value >= 0) {"
    print "        mpz_add_ui(value_, value_, static_cast<unsigned long>(value));"
    print "    } else {"
    print "        mpz_sub_ui(value_, value_, static_cast<unsigned long>(-value));"
    print "    }"
    print "    return *this;"
    print "}"
    print ""
    print "Integer& Integer::operator-=(int64_t value) {"
    print "    if (value >= 0) {"
    print "        mpz_sub_ui(value_, value_, static_cast<unsigned long>(value));"
    print "    } else {"
    print "        mpz_add_ui(value_, value_, static_cast<unsigned long>(-value));"
    print "    }"
    print "    return *this;"
    print "}"
    print ""
    print "Integer& Integer::operator/=(int64_t value) {"
    print "    if (value > 0) {"
    print "        mpz_tdiv_q_ui(value_, value_, static_cast<unsigned long>(value));"
    print "    } else if (value < 0) {"
    print "        mpz_tdiv_q_ui(value_, value_, static_cast<unsigned long>(-value));"
    print "        mpz_neg(value_, value_);"
    print "    } else {"
    print "        throw std::domain_error(\"Division by zero\");"
    print "    }"
    print "    return *this;"
    print "}"
    print ""
    print "Integer& Integer::operator%=(int64_t value) {"
    print "    unsigned long abs_val = (value >= 0) ? value : -value;"
    print "    mpz_tdiv_r_ui(value_, value_, abs_val);"
    print "    return *this;"
    print "}"
    print ""
}
{ print }
' src/core/integer.cpp > /tmp/integer.cpp.new

mv /tmp/integer.cpp.new src/core/integer.cpp
echo "✓ Integer.cpp 已更新"
echo ""

# 步骤 4: 修复所有 Integer(0) 类型歧义
echo "步骤 4: 修复类型歧义..."

find include -name "*.hpp" -type f -exec sed -i.bak \
    -e 's/Integer(0)/Integer(static_cast<int64_t>(0))/g' \
    -e 's/Integer(1)/Integer(static_cast<int64_t>(1))/g' \
    {} \;

echo "✓ 类型歧义已修复"
echo ""

# 步骤 5: 修复字段名
echo "步骤 5: 修复 Relation 字段名..."

find include -name "*.hpp" -type f -exec sed -i.bak2 \
    -e 's/\.ab\.a/.a/g' \
    -e 's/\.ab\.b/.b/g' \
    -e 's/\.rat_factors/.rational_factors/g' \
    -e 's/\.alg_factors/.algebraic_factors/g' \
    -e 's/\.large_primes_rat/.rational_large_prime/g' \
    -e 's/\.large_primes_alg/.algebraic_large_prime/g' \
    {} \;

echo "✓ 字段名已修复"
echo ""

# 清理备份文件
find include -name "*.bak" -delete
find include -name "*.bak2" -delete

echo "=========================================="
echo "修复完成！"
echo "=========================================="
echo ""
echo "现在重新编译："
echo "  cd build"
echo "  make clean"
echo "  make -j12"
