#include "gnfs/core/integer.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <gmp.h>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace gnfs::core {

namespace {

void set_mpz_from_uint64(mpz_t dest, uint64_t value) {
    mpz_import(dest, 1, 1, sizeof(value), 0, 0, &value);
}

void set_mpz_from_int64(mpz_t dest, int64_t value) {
    if (value >= 0) {
        set_mpz_from_uint64(dest, static_cast<uint64_t>(value));
        return;
    }
    uint64_t magnitude = static_cast<uint64_t>(-(value + 1)) + 1ULL;
    set_mpz_from_uint64(dest, magnitude);
    mpz_neg(dest, dest);
}

uint64_t abs_mpz_to_uint64(const mpz_t value) {
    uint64_t out = 0;
    size_t count = 0;
    mpz_export(&out, &count, 1, sizeof(out), 0, 0, value);
    return out;
}

// ── BACKLOG P3 DEBT: GMP OOM 默认 abort() — 改用抛 std::bad_alloc ──
//
// GMP 文档警告"自定义分配器不应抛异常 — GMP 内部非异常安全"。但默认
// abort() 在大 N 下完全无法恢复,catch (std::bad_alloc) 至少允许:
//   1. caller 优雅降级 (放弃当前 SQ, 切下个素数,等)
//   2. RAII 析构链触发 (释放其他资源)
// 风险: 若异常穿过 GMP 内部 mpz 函数调用,GMP state 可能不一致。
// 但 Integer 类已用 mpz_class 等价的 RAII 包装 (mpz_clear in ~Integer),
// 内部 GMP-only 调用栈较短,实践中较安全。
//
// 启用此自定义分配器: 默认开启 (GNFS_GMP_NO_THROW_OOM 定义则禁用)。
#ifndef GNFS_GMP_NO_THROW_OOM

void* gmp_alloc(size_t n) {
    void* p = std::malloc(n);
    if (!p)
        throw std::bad_alloc{};
    return p;
}

void* gmp_realloc(void* old_ptr, size_t /*old_size*/, size_t new_size) {
    void* p = std::realloc(old_ptr, new_size);
    if (!p)
        throw std::bad_alloc{};
    return p;
}

void gmp_free(void* p, size_t /*size*/) {
    std::free(p);
}

struct GMPMemorySetup {
    GMPMemorySetup() {
        // mp_set_memory_functions 在 GMP 全局生效 — 必须在任何 mpz 操作前调用。
        // 由于此处为静态初始化,GMP-using 代码可能在 main 前已经构造若干
        // Integer 实例,这些实例使用 GMP 默认 malloc。setup 后才会切到我们的
        // 抛异常 allocator。这是可以接受的: 进程启动早期内存压力低,
        // 大量 mpz 分配发生在主算法运行时(setup 之后)。
        mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
    }
};
static GMPMemorySetup gmp_memory_setup;

#endif // !GNFS_GMP_NO_THROW_OOM

} // anonymous namespace

// ============================================================
// Construction & Destruction
// ============================================================

Integer::Integer() {
    mpz_init(value_);
}

Integer::Integer(int value) {
    mpz_init_set_si(value_, value);
}

Integer::Integer(int64_t value) {
    mpz_init(value_);
    set_mpz_from_int64(value_, value);
}

Integer::Integer(unsigned int value) {
    mpz_init_set_ui(value_, value);
}

Integer::Integer(uint64_t value) {
    mpz_init(value_);
    set_mpz_from_uint64(value_, value);
}

Integer::Integer(const char* str, int base) {
    if (mpz_init_set_str(value_, str, base) != 0) {
        mpz_clear(value_);
        throw std::invalid_argument("Invalid string for Integer construction");
    }
}

Integer::Integer(const std::string& str, int base) : Integer(str.c_str(), base) {}

Integer::Integer(const Integer& other) {
    mpz_init_set(value_, other.value_);
}

Integer::Integer(Integer&& other) noexcept {
    // Move by swapping internals
    value_[0] = other.value_[0];
    mpz_init(other.value_);
}

Integer::~Integer() {
    mpz_clear(value_);
}

// ============================================================
// Assignment
// ============================================================

Integer& Integer::operator=(const Integer& other) {
    if (this != &other) {
        mpz_set(value_, other.value_);
    }
    return *this;
}

Integer& Integer::operator=(Integer&& other) noexcept {
    if (this != &other) {
        mpz_swap(value_, other.value_);
    }
    return *this;
}

Integer& Integer::operator=(int64_t value) {
    set_mpz_from_int64(value_, value);
    return *this;
}

Integer& Integer::operator=(uint64_t value) {
    set_mpz_from_uint64(value_, value);
    return *this;
}

// ============================================================
// Cloning
// ============================================================

Integer Integer::clone() const {
    return Integer(*this);
}

// ============================================================
// Conversion
// ============================================================

int64_t Integer::to_int64() const {
    if (!fits_int64()) {
        throw std::overflow_error("Integer does not fit in int64_t");
    }
    if (mpz_sgn(value_) >= 0) {
        return static_cast<int64_t>(abs_mpz_to_uint64(value_));
    }
    uint64_t magnitude = abs_mpz_to_uint64(value_);
    constexpr uint64_t int64_min_abs = uint64_t{1} << 63;
    if (magnitude == int64_min_abs) {
        return std::numeric_limits<int64_t>::min();
    }
    return -static_cast<int64_t>(magnitude);
}

std::string Integer::to_string(int base) const {
    char* str = mpz_get_str(nullptr, base, value_);
    std::string result(str);
    free(str);
    return result;
}

size_t Integer::bit_length() const {
    // GMP reports one digit for zero in every base. A bit length is the
    // number of significant bits, so zero has no set bits and must report 0.
    if (mpz_sgn(value_) == 0) {
        return 0;
    }
    return mpz_sizeinbase(value_, 2);
}

// ============================================================
// Arithmetic operations
// ============================================================

Integer& Integer::operator+=(const Integer& other) {
    mpz_add(value_, value_, other.value_);
    return *this;
}

Integer& Integer::operator-=(const Integer& other) {
    mpz_sub(value_, value_, other.value_);
    return *this;
}

Integer& Integer::operator*=(const Integer& other) {
    mpz_mul(value_, value_, other.value_);
    return *this;
}

Integer& Integer::operator/=(const Integer& other) {
    if (mpz_sgn(other.value_) == 0) {
        throw std::domain_error("Integer division by zero");
    }
    mpz_tdiv_q(value_, value_, other.value_);
    return *this;
}

Integer& Integer::operator%=(const Integer& other) {
    if (mpz_sgn(other.value_) == 0) {
        throw std::domain_error("Integer modulo by zero");
    }
    mpz_tdiv_r(value_, value_, other.value_);
    return *this;
}

Integer Integer::operator+(const Integer& other) const {
    Integer result;
    mpz_add(result.value_, value_, other.value_);
    return result;
}

Integer Integer::operator-(const Integer& other) const {
    Integer result;
    mpz_sub(result.value_, value_, other.value_);
    return result;
}

Integer Integer::operator*(const Integer& other) const {
    Integer result;
    mpz_mul(result.value_, value_, other.value_);
    return result;
}

Integer Integer::operator/(const Integer& other) const {
    if (mpz_sgn(other.value_) == 0) {
        throw std::domain_error("Integer division by zero");
    }
    Integer result;
    mpz_tdiv_q(result.value_, value_, other.value_);
    return result;
}

Integer Integer::operator%(const Integer& other) const {
    if (mpz_sgn(other.value_) == 0) {
        throw std::domain_error("Integer modulo by zero");
    }
    Integer result;
    mpz_tdiv_r(result.value_, value_, other.value_);
    return result;
}

Integer Integer::operator-() const {
    Integer result;
    mpz_neg(result.value_, value_);
    return result;
}

// ============================================================
// Comparison
// ============================================================

bool Integer::operator==(const Integer& other) const {
    return mpz_cmp(value_, other.value_) == 0;
}

bool Integer::operator==(int64_t rhs) const {
    Integer rhs_int(rhs);
    return mpz_cmp(value_, rhs_int.value_) == 0;
}

bool Integer::operator!=(const Integer& other) const {
    return mpz_cmp(value_, other.value_) != 0;
}

bool Integer::operator<(const Integer& other) const {
    return mpz_cmp(value_, other.value_) < 0;
}

bool Integer::operator>(const Integer& other) const {
    return mpz_cmp(value_, other.value_) > 0;
}

bool Integer::operator<=(const Integer& other) const {
    return mpz_cmp(value_, other.value_) <= 0;
}

bool Integer::operator>=(const Integer& other) const {
    return mpz_cmp(value_, other.value_) >= 0;
}

int Integer::compare(const Integer& other) const {
    return mpz_cmp(value_, other.value_);
}

// ============================================================
// Bit operations
// ============================================================

bool Integer::test_bit(size_t index) const {
    return mpz_tstbit(value_, index) != 0;
}

void Integer::set_bit(size_t index) {
    mpz_setbit(value_, index);
}

void Integer::clear_bit(size_t index) {
    mpz_clrbit(value_, index);
}

// ============================================================
// Status queries
// ============================================================

bool Integer::is_zero() const {
    return mpz_sgn(value_) == 0;
}

bool Integer::is_one() const {
    return mpz_cmp_ui(value_, 1) == 0;
}

bool Integer::is_negative() const {
    return mpz_sgn(value_) < 0;
}

bool Integer::is_positive() const {
    return mpz_sgn(value_) > 0;
}

// ============================================================
// Modular arithmetic
// ============================================================

int Integer::is_probable_prime(int reps) const {
    return mpz_probab_prime_p(value_, reps);
}

// ============================================================
// Absolute value
// ============================================================

void Integer::abs() {
    mpz_abs(value_, value_);
}

void Integer::negate() {
    mpz_neg(value_, value_);
}

// ============================================================
// Static operations
// ============================================================

void Integer::add(Integer& result, const Integer& a, const Integer& b) {
    mpz_add(result.value_, a.value_, b.value_);
}

void Integer::sub(Integer& result, const Integer& a, const Integer& b) {
    mpz_sub(result.value_, a.value_, b.value_);
}

void Integer::mul(Integer& result, const Integer& a, const Integer& b) {
    mpz_mul(result.value_, a.value_, b.value_);
}

void Integer::div(Integer& result, const Integer& a, const Integer& b) {
    if (mpz_sgn(b.value_) == 0) {
        throw std::domain_error("Integer division by zero");
    }
    mpz_tdiv_q(result.value_, a.value_, b.value_);
}

void Integer::mod(Integer& result, const Integer& a, const Integer& b) {
    if (mpz_sgn(b.value_) == 0) {
        throw std::domain_error("Integer modulo by zero");
    }
    mpz_tdiv_r(result.value_, a.value_, b.value_);
}

void Integer::divmod(Integer& quot, Integer& rem, const Integer& a, const Integer& b) {
    if (mpz_sgn(b.value_) == 0) {
        throw std::domain_error("Integer division by zero");
    }
    mpz_tdiv_qr(quot.value_, rem.value_, a.value_, b.value_);
}

// ============================================================
// Non-member functions
// ============================================================

Integer gcd(const Integer& a, const Integer& b) {
    Integer result;
    mpz_gcd(result.get_mpz(), a.get_mpz(), b.get_mpz());
    return result;
}

Integer lcm(const Integer& a, const Integer& b) {
    Integer result;
    mpz_lcm(result.get_mpz(), a.get_mpz(), b.get_mpz());
    return result;
}

Integer powmod(const Integer& base, const Integer& exp, const Integer& mod) {
    Integer result;
    mpz_powm(result.get_mpz(), base.get_mpz(), exp.get_mpz(), mod.get_mpz());
    return result;
}

Integer pow(const Integer& base, uint32_t exp) {
    Integer result;
    mpz_pow_ui(result.get_mpz(), base.get_mpz(), exp);
    return result;
}

Integer sqrt(const Integer& n) {
    Integer result;
    mpz_sqrt(result.get_mpz(), n.get_mpz());
    return result;
}

std::ostream& operator<<(std::ostream& os, const Integer& n) {
    return os << n.to_string();
}

uint64_t Integer::to_uint64() const {
    if (!fits_uint64()) {
        throw std::overflow_error("Integer does not fit in uint64_t");
    }
    return abs_mpz_to_uint64(value_);
}

double Integer::to_double() const {
    return mpz_get_d(value_);
}

size_t Integer::num_digits(int base) const {
    return mpz_sizeinbase(value_, base);
}

bool Integer::fits_uint64() const {
    if (mpz_sgn(value_) < 0)
        return false;
    mpz_t max;
    mpz_init(max);
    set_mpz_from_uint64(max, std::numeric_limits<uint64_t>::max());
    const bool fits = mpz_cmp(value_, max) <= 0;
    mpz_clear(max);
    return fits;
}

bool Integer::fits_int64() const {
    mpz_t min_value;
    mpz_t max_value;
    mpz_init(min_value);
    mpz_init(max_value);
    set_mpz_from_int64(min_value, std::numeric_limits<int64_t>::min());
    set_mpz_from_int64(max_value, std::numeric_limits<int64_t>::max());
    const bool fits = mpz_cmp(value_, min_value) >= 0 && mpz_cmp(value_, max_value) <= 0;
    mpz_clear(min_value);
    mpz_clear(max_value);
    return fits;
}

bool Integer::is_odd() const {
    return mpz_odd_p(value_) != 0;
}

bool Integer::is_even() const {
    return mpz_even_p(value_) != 0;
}

Integer& Integer::operator*=(int64_t value) {
    Integer rhs(value);
    mpz_mul(value_, value_, rhs.value_);
    return *this;
}

Integer& Integer::operator+=(int64_t value) {
    Integer rhs(value);
    mpz_add(value_, value_, rhs.value_);
    return *this;
}

Integer& Integer::operator-=(int64_t value) {
    Integer rhs(value);
    mpz_sub(value_, value_, rhs.value_);
    return *this;
}

Integer& Integer::operator/=(int64_t value) {
    if (value == 0) {
        throw std::domain_error("Division by zero");
    }
    Integer rhs(value);
    mpz_tdiv_q(value_, value_, rhs.value_);
    return *this;
}

Integer& Integer::operator%=(int64_t value) {
    if (value == 0) {
        throw std::domain_error("Integer modulo by zero");
    }
    Integer rhs(value);
    mpz_tdiv_r(value_, value_, rhs.value_);
    return *this;
}

} // namespace gnfs::core
