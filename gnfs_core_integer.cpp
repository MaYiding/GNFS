#include "gnfs/core/integer.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace gnfs::core {

// ============================================================
// Construction & Destruction
// ============================================================

Integer::Integer() {
    mpz_init(value_);
}

Integer::Integer(int64_t value) {
    mpz_init_set_si(value_, value);
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
    mpz_set_si(value_, value);
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
    if (!mpz_fits_slong_p(value_)) {
        throw std::overflow_error("Integer does not fit in int64_t");
    }
    return mpz_get_si(value_);
}

std::string Integer::to_string(int base) const {
    char* str = mpz_get_str(nullptr, base, value_);
    std::string result(str);
    free(str);
    return result;
}

size_t Integer::bit_length() const {
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
    mpz_tdiv_q(value_, value_, other.value_);
    return *this;
}

Integer& Integer::operator%=(const Integer& other) {
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
    Integer result;
    mpz_tdiv_q(result.value_, value_, other.value_);
    return result;
}

Integer Integer::operator%(const Integer& other) const {
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
    mpz_tdiv_q(result.value_, a.value_, b.value_);
}

void Integer::mod(Integer& result, const Integer& a, const Integer& b) {
    mpz_tdiv_r(result.value_, a.value_, b.value_);
}

void Integer::divmod(Integer& quot, Integer& rem, const Integer& a, const Integer& b) {
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

Integer sqrt(const Integer& n) {
    Integer result;
    mpz_sqrt(result.get_mpz(), n.get_mpz());
    return result;
}

std::ostream& operator<<(std::ostream& os, const Integer& n) {
    return os << n.to_string();
}

} // namespace gnfs::core
