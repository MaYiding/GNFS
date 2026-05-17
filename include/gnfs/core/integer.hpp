#pragma once

#include <gmp.h>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <type_traits>

namespace gnfs::core {

/// Integer class wrapping GMP's mpz_t for arbitrary-precision arithmetic
class Integer {
public:
    // Construction & Destruction
    Integer();
    explicit Integer(int value);
    explicit Integer(int64_t value);
    explicit Integer(unsigned int value);
    explicit Integer(uint64_t value);
    // On platforms where uint64_t/int64_t are `unsigned long`/`long`
    // (Linux LP64), `unsigned long long`/`long long` are distinct
    // 64-bit integer types and constructor overload resolution
    // becomes ambiguous between Integer(int), Integer(int64_t),
    // Integer(uint64_t) etc. Add dedicated overloads there; on
    // platforms where they alias (macOS), the requires-clauses
    // disable them to avoid redefinition.
    template <typename T>
        requires (std::is_same_v<T, unsigned long long>
                  && !std::is_same_v<unsigned long long, uint64_t>)
    explicit Integer(T value) : Integer(static_cast<uint64_t>(value)) {}
    template <typename T>
        requires (std::is_same_v<T, long long>
                  && !std::is_same_v<long long, int64_t>)
    explicit Integer(T value) : Integer(static_cast<int64_t>(value)) {}
    Integer(const char* str, int base = 10);
    Integer(const std::string& str, int base = 10);
    Integer(const Integer& other);
    Integer(Integer&& other) noexcept;
    ~Integer();

    // Assignment
    Integer& operator=(const Integer& other);
    Integer& operator=(Integer&& other) noexcept;
    Integer& operator=(int64_t value);
    Integer& operator=(uint64_t value);

    // Cloning
    Integer clone() const;

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

    // Arithmetic operations with Integer
    Integer& operator+=(const Integer& other);
    Integer& operator-=(const Integer& other);
    Integer& operator*=(const Integer& other);
    Integer& operator/=(const Integer& other);
    Integer& operator%=(const Integer& other);

    // Arithmetic operations with primitives
    Integer& operator*=(int64_t value);
    Integer& operator+=(int64_t value);
    Integer& operator-=(int64_t value);
    Integer& operator/=(int64_t value);
    Integer& operator%=(int64_t value);

    Integer operator+(const Integer& other) const;
    Integer operator-(const Integer& other) const;
    Integer operator*(const Integer& other) const;
    Integer operator/(const Integer& other) const;
    Integer operator%(const Integer& other) const;

    Integer operator-() const;  // unary minus

    // Comparison
    bool operator==(const Integer& other) const;
    bool operator!=(const Integer& other) const;
    bool operator<(const Integer& other) const;
    bool operator>(const Integer& other) const;
    bool operator<=(const Integer& other) const;
    bool operator>=(const Integer& other) const;

    // 与 int64_t/uint64_t 直接比较,避免临时 Integer 分配 (走 mpz_cmp_si/ui)
    bool operator==(int64_t rhs) const;
    bool operator!=(int64_t rhs) const { return !(*this == rhs); }

    int compare(const Integer& other) const;

    // Bit operations
    bool test_bit(size_t index) const;
    void set_bit(size_t index);
    void clear_bit(size_t index);

    // Status queries
    bool is_zero() const;
    bool is_one() const;
    bool is_negative() const;
    bool is_positive() const;
    bool is_odd() const;
    bool is_even() const;

    // Modular arithmetic
    int is_probable_prime(int reps = 25) const;

    // Absolute value
    void abs();
    void negate();

    // Static operations
    static void add(Integer& result, const Integer& a, const Integer& b);
    static void sub(Integer& result, const Integer& a, const Integer& b);
    static void mul(Integer& result, const Integer& a, const Integer& b);
    static void div(Integer& result, const Integer& a, const Integer& b);
    static void mod(Integer& result, const Integer& a, const Integer& b);
    static void divmod(Integer& quot, Integer& rem, const Integer& a, const Integer& b);

    // Access to underlying GMP type
    mpz_t& get_mpz() { return value_; }
    const mpz_t& get_mpz() const { return value_; }
    mpz_t* get() { return &value_; }
    const mpz_t* get() const { return &value_; }

private:
    mpz_t value_;
};

// Non-member functions
Integer gcd(const Integer& a, const Integer& b);
Integer lcm(const Integer& a, const Integer& b);
Integer powmod(const Integer& base, const Integer& exp, const Integer& mod);
Integer pow(const Integer& base, uint32_t exp);
Integer sqrt(const Integer& n);

/// 计算 a 模 m 的逆元 (a^{-1} mod m)
/// 如果逆元不存在返回 0
inline Integer mod_inverse(const Integer& a, const Integer& m) {
    Integer result;
    int ok = mpz_invert(result.get_mpz(), a.get_mpz(), m.get_mpz());
    if (!ok) return Integer{};
    return result;
}

// Stream output
std::ostream& operator<<(std::ostream& os, const Integer& n);

} // namespace gnfs::core
