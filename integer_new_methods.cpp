// 添加到 src/core/integer.cpp 的新方法实现

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
        mpz_add_ui(value_, value_, static_cast<unsigned long>(value));
    } else {
        mpz_sub_ui(value_, value_, static_cast<unsigned long>(-value));
    }
    return *this;
}

Integer& Integer::operator-=(int64_t value) {
    if (value >= 0) {
        mpz_sub_ui(value_, value_, static_cast<unsigned long>(value));
    } else {
        mpz_add_ui(value_, value_, static_cast<unsigned long>(-value));
    }
    return *this;
}

Integer& Integer::operator/=(int64_t value) {
    if (value > 0) {
        mpz_tdiv_q_ui(value_, value_, static_cast<unsigned long>(value));
    } else if (value < 0) {
        mpz_tdiv_q_ui(value_, value_, static_cast<unsigned long>(-value));
        mpz_neg(value_, value_);
    } else {
        throw std::domain_error("Division by zero");
    }
    return *this;
}

Integer& Integer::operator%=(int64_t value) {
    unsigned long abs_val = (value >= 0) ? value : -value;
    mpz_tdiv_r_ui(value_, value_, abs_val);
    return *this;
}
