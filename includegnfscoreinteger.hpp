#pragma once

#include <gmp.h>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace gnfs::core {

/// Integer class wrapping GMP's mpz_t for arbitrary-precision arithmetic
class Integer {
public:
    // Construction & Destruction
    Integer();
    explicit Integer(int64_t value);
    Integer(const char* str, int base = 10);
    Integer(const std::string& str, int base = 10);
    Integer(const Integer& other);
    Integer(Integer&& other) noexcept;
    ~Integer();

    // Assignment
    Integer& operator=(const Integer& other);
    Integer& operator=(Integer&& other) noexcept;
    Integer& operator=(int64_t value);

    // Cloning
    Integer clone() const;

    // Conversion
    int64_t to_int64() const;
    std::string to_string(int base = 10) const;
    size_t bit_length() const;

    // Arithmetic operations
    Integer& operator+=(const Integer& other);
    Integer& operator-=(const Integer& other);
    Integer& operator*=(const Integer& other);
    Integer& operator/=(const Integer& other);
    Integer& operator%=(const Integer& other);

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

private:
    mpz_t value_;
};

// Non-member functions
Integer gcd(const Integer& a, const Integer& b);
Integer lcm(const Integer& a, const Integer& b);
Integer powmod(const Integer& base, const Integer& exp, const Integer& mod);
Integer sqrt(const Integer& n);

// Stream output
std::ostream& operator<<(std::ostream& os, const Integer& n);

} // namespace gnfs::core
