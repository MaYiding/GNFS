#!/bin/bash
# extend_integer_class.sh - 扩展 Integer 类添加所有缺失的方法

echo "=========================================="
echo "扩展 Integer 类"
echo "=========================================="
echo ""

# 备份原文件
cp include/gnfs/core/integer.hpp include/gnfs/core/integer.hpp.before_extend
cp src/core/integer.cpp src/core/integer.cpp.before_extend

echo "步骤 1: 添加方法声明到 integer.hpp..."

# 在 integer.hpp 的 Conversion 部分添加新方法
cat > /tmp/integer_additions.hpp << 'EOF'
    // Conversion
    int64_t to_int64() const;
    uint64_t to_uint64() const;
    double to_double() const;
    std::string to_string(int base = 10) const;
    size_t bit_length() const;
    size_t num_digits(int base = 10) const;
    
    // Fit checks
    bool fits_uint64() const;
    bool fits_int64() const;
EOF

# 在 Status queries 部分添加
cat > /tmp/integer_status_additions.hpp << 'EOF'
    // Status queries
    bool is_zero() const;
    bool is_one() const;
    bool is_negative() const;
    bool is_positive() const;
    bool is_odd() const;
    bool is_even() const;
EOF

# 在 Assignment 部分添加更多运算符
cat > /tmp/integer_operators.hpp << 'EOF'
    // Assignment with primitives
    Integer& operator=(const Integer& other);
    Integer& operator=(Integer&& other) noexcept;
    Integer& operator=(int64_t value);
    
    // Arithmetic with primitives
    Integer& operator*=(int64_t value);
    Integer& operator+=(int64_t value);
    Integer& operator-=(int64_t value);
    Integer& operator/=(int64_t value);
    Integer& operator%=(int64_t value);
EOF

# 添加 get() 方法用于访问底层 mpz_t
cat > /tmp/integer_get.hpp << 'EOF'
    // Access to underlying GMP type (non-const for modification)
    mpz_t& get_mpz() { return value_; }
    const mpz_t& get_mpz() const { return value_; }
    mpz_t* get() { return &value_; }
    const mpz_t* get() const { return &value_; }
EOF

echo "步骤 2: 添加实现到 integer.cpp..."

# 创建新的实现
cat > /tmp/integer_new_methods.cpp << 'EOF'

uint64_t Integer::to_uint64() const {
    if (!mpz_fits_ulong_p(value_)) {
        throw std::overflow_error("Integer does not fit in uint64_t");
    }
    return mpz_get_ui(value_);
}

double Integer::to_double() const {
    return mpz_get_d(value_);
}

size_t Integer::num_digits(int base) const {
    return mpz_sizeinbase(value_, base);
}

bool Integer::fits_uint64() const {
    return mpz_fits_ulong_p(value_) != 0;
}

bool Integer::fits_int64() const {
    return mpz_fits_slong_p(value_) != 0;
}

bool Integer::is_odd() const {
    return mpz_odd_p(value_) != 0;
}

bool Integer::is_even() const {
    return mpz_even_p(value_) != 0;
}

Integer& Integer::operator*=(int64_t value) {
    mpz_mul_si(value_, value_, value);
    return *this;
}

Integer& Integer::operator+=(int64_t value) {
    if (value >= 0) {
        mpz_add_ui(value_, value_, value);
    } else {
        mpz_sub_ui(value_, value_, -value);
    }
    return *this;
}

Integer& Integer::operator-=(int64_t value) {
    if (value >= 0) {
        mpz_sub_ui(value_, value_, value);
    } else {
        mpz_add_ui(value_, value_, -value);
    }
    return *this;
}

Integer& Integer::operator/=(int64_t value) {
    mpz_tdiv_q_ui(value_, value_, std::abs(value));
    if (value < 0) {
        mpz_neg(value_, value_);
    }
    return *this;
}

Integer& Integer::operator%=(int64_t value) {
    mpz_tdiv_r_ui(value_, value_, std::abs(value));
    return *this;
}
EOF

echo "✓ 准备完成"
echo ""
echo "现在手动应用这些更改..."
echo "或运行完整的修复脚本"
