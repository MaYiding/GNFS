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
    static Integer zero;
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
    if (coeffs_.empty()) return Integer(0);
    
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
        coeffs_.push_back(Integer(0));
    }
}

} // namespace gnfs::core
